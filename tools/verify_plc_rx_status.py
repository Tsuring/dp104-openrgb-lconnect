#!/usr/bin/env python3
"""Run an isolated QEMU ABI check for the PLC UART status veneer.

The linked fixture executes the checked-in Thumb veneer against synthetic RAM.
It does not map stock MMIO, firmware, a board, or an installed runtime.
"""

from __future__ import annotations

import hashlib
import struct
import subprocess
from pathlib import Path
from typing import Any

from build_data_restore import (
    PROFILES,
    ROOT,
    RestoreBuildError,
    check_machine_abi,
    inspect_elf,
    require,
)
from wsl_paths import WslPathError, to_wsl_path


RX_VMA = 0x00100000
RW_VMA = 0x00200000
SYNTHETIC_LIMIT = 0x00300000
SYNTHETIC_STATE = 0x00200090
SYNTHETIC_STACK_TOP = 0x00202568
EXPECTED_TARGET_STATE = 0x2000CBB0
STATUS_OFFSET = 8896
CNDTR_OFFSET = 8900
COUNT_OFFSET = 8904
STATE_BYTES = COUNT_OFFSET + 8
HOOK_SIZE = 28
HOOK_PREFIX = bytes.fromhex(
    "13b51a685b6804480260216f4160816801f10101816013bd"
)
PT_DYNAMIC = 2
PT_INTERP = 3
PF_R = 4
PF_X = 1
PF_W = 2


class RxStatusCpuError(RestoreBuildError):
    """Raised when the isolated hook ABI check cannot be completed."""


def _sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def _relative(path: Path) -> str:
    try:
        return path.resolve().relative_to(ROOT).as_posix()
    except ValueError as error:
        raise RxStatusCpuError(f"output path is outside the repository: {path}") from error


def _linker_script(state_bytes: int) -> str:
    return f"""ENTRY(_start)
PHDRS
{{
  test_rx PT_LOAD FLAGS(5);
  test_rw PT_LOAD FLAGS(6);
}}
SECTIONS
{{
  . = 0x{RX_VMA:x};
  .text.startup : {{ KEEP(*(.text.startup)) }} :test_rx
  . = ALIGN(4);
  .rx_status_hook : {{ KEEP(*(.text.dp104_plc_rx_status_hook)) }} :test_rx
  .text : {{ *(.text .text.*) }} :test_rx
  .rodata : {{ *(.rodata .rodata.*) }} :test_rx
  .ARM.extab : {{ *(.ARM.extab* .gnu.linkonce.armextab.*) }} :test_rx
  .ARM.exidx : {{ *(.ARM.exidx* .gnu.linkonce.armexidx.*) }} :test_rx
  . = 0x{RW_VMA:x};
  .test_ram (NOLOAD) :
  {{
    . = ALIGN(8);
    __rx_test_usart_guard_before = .;
    . += 4;
    __rx_test_usart = .;
    . += 8;
    __rx_test_usart_guard_after = .;
    . += 4;

    __rx_test_dma_guard_before = .;
    . += 4;
    __rx_test_dma = .;
    . += 0x6c;
    __rx_test_dma_guard_cndtr = .;
    . += 4;
    __rx_test_dma_after_cndtr = .;
    . += 4;
    __rx_test_dma_end = .;

    . = ALIGN(16);
    __dp104_plc_state = .;
    . += {state_bytes};
    __rx_test_state_after = .;
    . += 4;

    . = ALIGN(8);
    __rx_test_stack_bottom = .;
    . += 512;
    __rx_test_stack_top = .;
  }} :test_rw
  /DISCARD/ : {{ *(.comment) *(.note .note.*) *(.eh_frame .eh_frame.*) }}
}}
ASSERT(ADDR(.text.startup) == 0x{RX_VMA:x}, "fixture entry left synthetic RX range")
ASSERT(SIZEOF(.rx_status_hook) == {HOOK_SIZE}, "status hook size changed")
ASSERT(__dp104_plc_state + {state_bytes} <= 0x{SYNTHETIC_LIMIT:x},
       "synthetic PLC state escaped writable range")
ASSERT(__dp104_plc_state == 0x{SYNTHETIC_STATE:x},
       "synthetic PLC state address changed")
ASSERT(__rx_test_dma_end - __rx_test_dma >= 0x74,
       "synthetic DMA array is too small")
ASSERT(__rx_test_stack_top == 0x{SYNTHETIC_STACK_TOP:x},
       "synthetic stack address changed")
ASSERT(__rx_test_stack_top % 8 == 0, "synthetic stack is not 8-byte aligned")
"""


def _inspect_fixture(path: Path, elf: dict[str, Any]) -> dict[str, Any]:
    profile = PROFILES["plc"]
    check_machine_abi(elf, profile, "PLC RX status CPU fixture")
    require(not elf["undefined_symbols"],
            f"fixture ELF has unresolved symbols: {elf['undefined_symbols']}")
    section_names = {section["name"] for section in elf["sections"]}
    require(not section_names.intersection({".interp", ".dynamic", ".dynsym", ".dynstr"}),
            "fixture ELF contains dynamic-link sections")

    data = path.read_bytes()
    fields = struct.unpack_from("<HHIIIIIHHHHHH", data, 16)
    phoff, phentsize, phnum = fields[4], fields[8], fields[9]
    require(phentsize == 32 and phoff + phentsize * phnum <= len(data),
            "fixture ELF has an invalid program-header table")
    program_types = []
    for index in range(phnum):
        p_type = struct.unpack_from(
            "<I", data, phoff + index * phentsize
        )[0]
        program_types.append(p_type)
    require(PT_INTERP not in program_types and PT_DYNAMIC not in program_types,
            "fixture ELF has an interpreter or dynamic segment")

    loads = elf["program_headers"]
    require(len(loads) == 2, f"expected RX/RW synthetic segments, found {len(loads)}")
    for segment in loads:
        start = segment["virtual_address"]
        end = start + segment["memory_size"]
        require(RX_VMA <= start < end <= SYNTHETIC_LIMIT,
                f"PT_LOAD escaped synthetic memory: 0x{start:x}..0x{end:x}")
        require(segment["file_size"] <= segment["memory_size"],
                "PT_LOAD file size exceeds memory size")
    rx = next((segment for segment in loads
               if segment["flags"] == (PF_R | PF_X)), None)
    rw = next((segment for segment in loads
               if segment["flags"] == (PF_R | PF_W)), None)
    require(rx is not None and rw is not None,
            "fixture must have separate RX and RW PT_LOAD segments")
    require(rx["virtual_address"] == RX_VMA and rw["virtual_address"] == RW_VMA,
            "fixture PT_LOAD addresses differ from synthetic map")
    require(rx["virtual_address"] + rx["memory_size"] <= rw["virtual_address"],
            "synthetic RX and RW PT_LOAD segments overlap")

    sections = [section for section in elf["sections"]
                if section["name"] == ".rx_status_hook"]
    require(len(sections) == 1 and sections[0]["size"] == HOOK_SIZE,
            "linked hook section is missing or has unexpected size")
    hook = sections[0]
    require(hook["flags"] & 0x4 and hook["address"] % 4 == 0,
            "linked hook section is not aligned executable code")
    hook_bytes = data[hook["offset"]:hook["offset"] + hook["size"]]
    require(len(hook_bytes) == HOOK_SIZE, "cannot read complete hook section")

    synthetic_state_end = SYNTHETIC_STATE + STATE_BYTES
    require(RW_VMA <= SYNTHETIC_STATE and
            synthetic_state_end <= rw["virtual_address"] + rw["memory_size"],
            "synthetic PLC state is outside writable PT_LOAD")
    if elf["symbol_table_present"]:
        state_symbol = elf["symbols"].get("__dp104_plc_state")
        hook_symbol = elf["symbols"].get("dp104_plc_rx_status_hook")
        require(state_symbol is None or
                (state_symbol["value"] & ~1) == SYNTHETIC_STATE,
                "synthetic state symbol differs from linker assertion")
        require(hook_symbol is None or
                (hook_symbol["value"] & ~1) == hook["address"],
                "hook symbol differs from linked hook section")
    require(elf["entry"] & ~1 == RX_VMA,
            "fixture entry is outside synthetic RX image")

    return {
        "machine": elf["machine"],
        "elf_type": elf["elf_type"],
        "has_interpreter": elf["has_interpreter"],
        "undefined_symbols": elf["undefined_symbols"],
        "load_segments": loads,
        "hook_address": hook["address"],
        "hook_size": hook["size"],
        "hook_bytes": hook_bytes,
        "synthetic_state": SYNTHETIC_STATE,
        "entry": elf["entry"],
    }


def build_rx_status_cpu_test(zig: Path, env: dict[str, str], output_dir: Path,
                             expected_code: bytes) -> dict[str, Any]:
    """Build/run the actual PLC hook in synthetic memory and verify its ABI.

    ``expected_code`` is the linked target hook body (28 bytes). The fixture
    must match its first 24 instruction bytes exactly; only the final literal
    is relocated to the synthetic PLC state address.
    """
    output_dir = output_dir.resolve()
    _relative(output_dir)
    zig = zig.resolve()
    require(zig.is_file(), f"Zig compiler not found: {zig}")
    expected_code = bytes(expected_code)
    require(len(expected_code) == HOOK_SIZE,
            f"target hook must be exactly {HOOK_SIZE} bytes")
    require(expected_code[:len(HOOK_PREFIX)] == HOOK_PREFIX,
            "target hook instruction bytes differ from the expected ABI sequence")
    target_literal = int.from_bytes(expected_code[24:28], "little")
    require(target_literal == EXPECTED_TARGET_STATE + STATUS_OFFSET,
            "target hook literal is not PLC state + 8896")

    hook_source = ROOT / "src" / "firmware" / "thumb_rx_status_hook.S"
    fixture_source = ROOT / "tests" / "plc_rx_status_cpu.S"
    require(hook_source.is_file() and fixture_source.is_file(),
            "PLC RX status hook or synthetic CPU fixture is missing")
    output_dir.mkdir(parents=True, exist_ok=True)
    executable = output_dir / "plc_rx_status_cpu_test"
    linker_script = output_dir / "plc_rx_status_cpu.ld"
    linker_script.write_text(_linker_script(STATE_BYTES), encoding="ascii")

    profile = PROFILES["plc"]
    build_env = dict(env)
    global_cache = output_dir / "zig-global"
    local_cache = output_dir / "zig-local"
    global_cache.mkdir(parents=True, exist_ok=True)
    local_cache.mkdir(parents=True, exist_ok=True)
    build_env["ZIG_GLOBAL_CACHE_DIR"] = str(global_cache)
    build_env["ZIG_LOCAL_CACHE_DIR"] = str(local_cache)
    command = [
        str(zig), "cc",
        "-target", profile["linux_triple"],
        "-std=c99", "-Os", "-g0",
        "-ffreestanding", "-fno-builtin", "-fno-stack-protector",
        "-fno-common", "-fno-pic", "-fno-pie",
        "-fno-unwind-tables", "-fno-asynchronous-unwind-tables",
        "-nostdlib", "-static", "-no-pie",
        "-Wall", "-Wextra", "-Werror",
        *profile["architecture_flags"],
        str(hook_source), str(fixture_source),
        "-Wl,-e,_start",
        "-Wl,-T," + str(linker_script),
        "-Wl,--build-id=none",
        "-Wl,--no-undefined",
        "-o", str(executable),
    ]
    try:
        compiled = subprocess.run(
            command, cwd=ROOT, env=build_env, text=True,
            stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            timeout=90, check=False,
        )
    except (OSError, WslPathError, subprocess.TimeoutExpired) as error:
        raise RxStatusCpuError(f"could not compile synthetic fixture: {error}") from error
    require(compiled.returncode == 0,
            "synthetic fixture compilation failed:\n" +
            (compiled.stdout + compiled.stderr).strip())

    elf = inspect_elf(executable)
    profile_check = _inspect_fixture(executable, elf)
    fixture_code = profile_check.pop("hook_bytes")
    require(fixture_code[:24] == expected_code[:24],
            "synthetic hook instruction prefix differs from target hook")
    synthetic_literal = int.from_bytes(fixture_code[24:28], "little")
    require(synthetic_literal == profile_check["synthetic_state"] + STATUS_OFFSET,
            "fixture hook literal does not target synthetic PLC state + 8896")

    qemu_command = [
        "wsl.exe", "-d", "Ubuntu", "--exec", profile["qemu"],
        to_wsl_path(executable),
    ]
    try:
        qemu = subprocess.run(
            qemu_command, cwd=ROOT, env=build_env, text=True,
            stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            timeout=30, check=False,
        )
    except (OSError, subprocess.TimeoutExpired) as error:
        raise RxStatusCpuError(f"could not run synthetic QEMU fixture: {error}") from error
    require(qemu.returncode == 0,
            f"synthetic ABI cases failed with exit {qemu.returncode}:\n" +
            (qemu.stdout + qemu.stderr).strip())

    return {
        "schema": "dp104-plc-rx-status-cpu-v1",
        "target_triple": profile["linux_triple"],
        "cpu_profile": profile["cpu"],
        "compile_command": command,
        "compile_stdout": compiled.stdout.strip(),
        "qemu_command": qemu_command,
        "qemu_exit_code": qemu.returncode,
        "qemu_stdout": qemu.stdout.strip(),
        "qemu_stderr": qemu.stderr.strip(),
        "executable": _relative(executable),
        "executable_sha256": _sha256(executable.read_bytes()),
        "linker_script": _relative(linker_script),
        "linker_script_sha256": _sha256(linker_script.read_bytes()),
        "hook_source_sha256": _sha256(hook_source.read_bytes()),
        "fixture_source_sha256": _sha256(fixture_source.read_bytes()),
        "target_hook_sha256": _sha256(expected_code),
        "target_hook_bytes": len(expected_code),
        "target_literal": hex(target_literal),
        "synthetic_literal": hex(synthetic_literal),
        "hook_instruction_prefix_exact": True,
        "synthetic_abi_cases_passed": 1,
        "synthetic_case": {
            "sr": "0xa5a5f00d",
            "dr": "0xd00dfeed",
            "cndtr": 72,
            "count_before": "0xffffffff",
            "count_after": 0,
            "registers_preserved": ["r0", "r1", "r4-r12", "sp"],
            "r2_is_preserved_sr": True,
            "r3_is_read_dr": True,
            "apsr_nzcvq_preserved": True,
            "adjacent_canaries_preserved": True,
        },
        "synthetic_state_bytes": STATE_BYTES,
        "synthetic_state_offsets": {
            "raw_sr": STATUS_OFFSET,
            "cndtr": CNDTR_OFFSET,
            "count": COUNT_OFFSET,
        },
        "boundary": {
            "all_pt_loads_inside": f"0x{RX_VMA:x}..0x{SYNTHETIC_LIMIT:x}",
            "stock_firmware_executed": False,
            "hardware_or_mmio_accessed": False,
            "libc_linked": False,
        },
        "elf": {key: value for key, value in profile_check.items()
                if key != "entry"},
    }
