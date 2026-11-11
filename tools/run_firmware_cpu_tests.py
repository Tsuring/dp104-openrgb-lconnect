#!/usr/bin/env python3
"""Build and run pure DP104 core tests under QEMU Linux-user emulation.

Only the new portable receiver, master bridge, wire codec, and memory support
are executed. The Linux executables use a synthetic TEST VMA and raw exit
syscalls; no stock firmware, MMIO, target addresses, or devices are involved.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import struct
import subprocess
import sys
from pathlib import Path
from typing import Any

from wsl_paths import to_wsl_path


ROOT = Path(__file__).resolve().parents[1]
DEFAULT_ZIG = ROOT / ".local" / "toolchain" / "zig-x86_64-windows-0.15.2" / "zig.exe"
OUTPUT_ROOT = ROOT / "build" / "firmware" / "cpu-tests"
ZIG_CACHE_ROOT = ROOT / "build" / "firmware" / "zig-cache"
TEST_VMA = 0x100000
TEST_VMA_LIMIT = TEST_VMA + 0x100000
ET_EXEC = 2
EM_ARM = 40
EM_RISCV = 243
PT_LOAD = 1
PT_INTERP = 3
EF_RISCV_RVC = 0x1
EF_RISCV_FLOAT_ABI = 0x6
EF_ARM_ABI_FLOAT_SOFT = 0x200
EF_ARM_ABI_FLOAT_HARD = 0x400
EF_ARM_EABIMASK = 0xFF000000

CORE_SOURCES = (
    "src/firmware/dp104_receiver.c",
    "src/firmware/dp104_master_bridge.c",
    "src/firmware/dp104_wire.c",
    "src/firmware/dp104_wire_stream.c",
    "src/firmware/dp104_lz.c",
    "src/firmware/freestanding_memory.c",
    "tests/firmware_cpu.c",
)


class CpuTestError(RuntimeError):
    pass


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def run(command: list[str], cwd: Path, env: dict[str, str], timeout: int = 60) -> subprocess.CompletedProcess[str]:
    try:
        result = subprocess.run(
            command,
            cwd=cwd,
            env=env,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            timeout=timeout,
            check=False,
        )
    except subprocess.TimeoutExpired as error:
        raise CpuTestError(f"command timed out after {timeout}s: {command}") from error
    if result.returncode != 0:
        detail = (result.stdout + result.stderr).strip()
        raise CpuTestError(
            f"command exited {result.returncode}: {command}\n{detail}"
        )
    return result


def inspect_executable(path: Path, profile: dict[str, Any]) -> dict[str, Any]:
    data = path.read_bytes()
    if len(data) < 52 or data[:4] != b"\x7fELF":
        raise CpuTestError(f"{path} is not an ELF executable.")
    if data[4] != 1 or data[5] != 1 or data[6] != 1:
        raise CpuTestError(f"{path} is not current ELF32 little-endian.")

    header = struct.unpack_from("<HHIIIIIHHHHHH", data, 16)
    (elf_type, machine, version, entry, phoff, shoff, flags, ehsize,
     phentsize, phnum, shentsize, shnum, shstrndx) = header
    if elf_type != ET_EXEC or version != 1 or ehsize != 52:
        raise CpuTestError(f"{path} is not a valid ET_EXEC binary.")
    if machine != profile["machine"]:
        raise CpuTestError(f"{path} e_machine={machine}, expected {profile['machine']}.")
    if phentsize != 32 or phnum == 0:
        raise CpuTestError(f"{path} has no supported program-header table.")
    if entry & ~1 < TEST_VMA or entry & ~1 >= TEST_VMA_LIMIT:
        raise CpuTestError(f"{path} entry 0x{entry:x} is outside synthetic TEST VMA.")

    program_table_end = phoff + phentsize * phnum
    if phoff < 52 or program_table_end > len(data):
        raise CpuTestError(f"{path} has an out-of-range program-header table.")
    load_segments = []
    has_interpreter = False
    for index in range(phnum):
        fields = struct.unpack_from("<IIIIIIII", data, phoff + index * phentsize)
        p_type, _offset, vaddr, paddr, filesz, memsz, segment_flags, alignment = fields
        if p_type == PT_INTERP:
            has_interpreter = True
        elif p_type == PT_LOAD:
            load_segments.append({
                "virtual_address": vaddr,
                "physical_address": paddr,
                "file_size": filesz,
                "memory_size": memsz,
                "flags": segment_flags,
                "alignment": alignment,
            })
    if has_interpreter:
        raise CpuTestError(f"{path} unexpectedly requires a dynamic interpreter.")
    if not load_segments:
        raise CpuTestError(f"{path} has no PT_LOAD segments.")
    if any(segment["virtual_address"] < TEST_VMA or
           segment["virtual_address"] >= TEST_VMA_LIMIT
           for segment in load_segments):
        raise CpuTestError(f"{path} has a load segment outside synthetic TEST VMA.")

    if machine == EM_RISCV:
        if not flags & EF_RISCV_RVC or flags & EF_RISCV_FLOAT_ABI:
            raise CpuTestError(f"{path} has unexpected RISC-V flags 0x{flags:08x}.")
    else:
        if (flags & EF_ARM_EABIMASK) != 0x05000000 or \
           not flags & EF_ARM_ABI_FLOAT_SOFT or flags & EF_ARM_ABI_FLOAT_HARD:
            raise CpuTestError(f"{path} has unexpected ARM ABI flags 0x{flags:08x}.")

    if shentsize != 40 or shnum == 0 or shstrndx >= shnum:
        raise CpuTestError(f"{path} has an unsupported section table.")
    if shoff + shentsize * shnum > len(data):
        raise CpuTestError(f"{path} section table lies outside the file.")
    sections = [
        struct.unpack_from("<IIIIIIIIII", data, shoff + index * shentsize)
        for index in range(shnum)
    ]
    strings_header = sections[shstrndx]
    strings = data[strings_header[4]:strings_header[4] + strings_header[5]]
    section_names = []
    for section in sections:
        offset = section[0]
        end = strings.find(b"\0", offset) if offset < len(strings) else -1
        if end < 0:
            section_names.append("")
        else:
            section_names.append(strings[offset:end].decode("ascii", errors="replace"))
    if any(name in (".interp", ".dynamic", ".dynsym") for name in section_names):
        raise CpuTestError(f"{path} contains dynamic-link sections.")

    return {
        "path": str(path.relative_to(ROOT)),
        "file_size": len(data),
        "elf_class": 32,
        "endianness": "little",
        "type": elf_type,
        "machine": machine,
        "flags": flags,
        "entry": entry,
        "program_headers": phnum,
        "load_segments": load_segments,
        "section_names": section_names,
    }


def compile_profile(zig: Path, env: dict[str, str], profile: dict[str, Any]) -> dict[str, Any]:
    output_dir = OUTPUT_ROOT / profile["name"]
    output_dir.mkdir(parents=True, exist_ok=True)
    executable = output_dir / "dp104_cpu_test"
    linker_script = output_dir / "test_layout.ld"
    linker_script.write_text(
        "\n".join((
            "ENTRY(_start)",
            "PHDRS",
            "{",
            "  test_rx PT_LOAD FLAGS(5);",
            "  test_rw PT_LOAD FLAGS(6);",
            "}",
            "SECTIONS",
            "{",
            f"  . = 0x{TEST_VMA:x};",
            "  .text : { *(.text .text.*) } :test_rx",
            "  .rodata : { *(.rodata .rodata.*) } :test_rx",
            "  .ARM.extab : { *(.ARM.extab* .gnu.linkonce.armextab.*) } :test_rx",
            "  .ARM.exidx : { *(.ARM.exidx* .gnu.linkonce.armexidx.*) } :test_rx",
            "  . = ALIGN(0x1000);",
            "  .data : { *(.data .data.*) } :test_rw",
            "  .bss : { *(.bss .bss.*) *(COMMON) } :test_rw",
            "}",
            "",
        )),
        encoding="ascii",
    )
    common = [
        str(zig), "cc",
        "-target", profile["triple"],
        "-std=c99", "-Os",
        "-ffreestanding", "-fno-builtin", "-fno-stack-protector",
        "-fno-common", "-fno-pic", "-fno-pie",
        "-fno-unwind-tables", "-fno-asynchronous-unwind-tables",
        "-nostdlib", "-static", "-no-pie",
        "-Wall", "-Wextra", "-Werror",
        "-Isrc/firmware/freestanding", "-Isrc/firmware", "-Iinclude",
        *profile["architecture_flags"],
        *CORE_SOURCES,
        "-Wl,-e,_start",
        f"-Wl,-T,{linker_script}",
        "-Wl,--build-id=none",
        "-Wl,--no-undefined",
    ]
    command = common + ["-o", str(executable)]
    compile_result = run(command, ROOT, env)
    elf = inspect_executable(executable, profile)

    qemu_command = ["wsl.exe", "-d", "Ubuntu", "--", profile["qemu"],
                    to_wsl_path(executable)]
    qemu_result = run(qemu_command, ROOT, env, timeout=30)

    return {
        "profile": profile["name"],
        "target_triple": profile["triple"],
        "cpu_profile": profile["cpu"],
        "compile_command": command,
        "compile_stdout": compile_result.stdout.strip(),
        "linker_script": str(linker_script.relative_to(ROOT)),
        "linker_script_sha256": sha256(linker_script),
        "elf": elf,
        "elf_sha256": sha256(executable),
        "qemu_command": qemu_command,
        "qemu_exit_code": qemu_result.returncode,
        "qemu_stdout": qemu_result.stdout.strip(),
        "qemu_stderr": qemu_result.stderr.strip(),
        "test_contract": "new portable core only; no stock firmware, MMIO, or peripheral execution",
        "test_vma": TEST_VMA,
        "linked_with_host_libc": False,
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--zig", type=Path, default=DEFAULT_ZIG,
                        help=f"Zig executable (default: {DEFAULT_ZIG})")
    args = parser.parse_args()
    zig = args.zig.resolve()
    if not zig.is_file():
        print(f"Zig executable not found: {zig}", file=sys.stderr)
        return 2

    OUTPUT_ROOT.mkdir(parents=True, exist_ok=True)
    global_cache = ZIG_CACHE_ROOT / "global"
    local_cache = ZIG_CACHE_ROOT / "local"
    global_cache.mkdir(parents=True, exist_ok=True)
    local_cache.mkdir(parents=True, exist_ok=True)
    env = os.environ.copy()
    env["ZIG_GLOBAL_CACHE_DIR"] = str(global_cache)
    env["ZIG_LOCAL_CACHE_DIR"] = str(local_cache)

    version = run([str(zig), "version"], ROOT, env).stdout.strip()
    if version != "0.15.2":
        print(f"Expected Zig 0.15.2, found {version!r}.", file=sys.stderr)
        return 2

    profiles = [
        {
            "name": "rv32imc-ilp32",
            "triple": "riscv32-linux-musl",
            "machine": EM_RISCV,
            "cpu": "generic_rv32+m+c / ilp32",
            "qemu": "/usr/bin/qemu-riscv32",
            "architecture_flags": [
                "-mcpu=generic_rv32+m+c", "-mabi=ilp32",
                "-msmall-data-limit=0", "-mno-relax",
            ],
        },
        {
            "name": "thumb-cortex-m3-softfloat",
            "triple": "thumb-linux-musleabi",
            "machine": EM_ARM,
            "cpu": "cortex_m3 / Thumb / soft-float",
            "qemu": "/usr/bin/qemu-arm",
            "architecture_flags": [
                "-mcpu=cortex_m3", "-mthumb", "-mfloat-abi=soft",
            ],
        },
    ]

    results = []
    for profile in profiles:
        results.append(compile_profile(zig, env, profile))
        result = results[-1]
        print(
            f"{profile['name']}: ELF32 machine={result['elf']['machine']} "
            f"flags=0x{result['elf']['flags']:08x} entry=0x{result['elf']['entry']:x}; "
            f"QEMU exit={result['qemu_exit_code']} PASS"
        )

    metadata = {
        "purpose": "bounded CPU execution of new portable DP104 firmware core",
        "test_vma": TEST_VMA,
        "not_firmware_placement": True,
        "stock_firmware_executed": False,
        "mmio_or_peripheral_access": False,
        "linked_with_host_libc": False,
        "zig_version": version,
        "source_hashes": {
            str(source): sha256(ROOT / source)
            for source in (*CORE_SOURCES, Path(__file__).resolve().relative_to(ROOT))
        },
        "results": results,
    }
    metadata_path = OUTPUT_ROOT / "results.json"
    metadata_path.write_text(json.dumps(metadata, indent=2, sort_keys=True) + "\n",
                             encoding="utf-8")
    print(f"CPU test metadata: {metadata_path}")
    print("Executed only the new portable core under Linux-user emulation; no hardware or firmware image.")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except CpuTestError as error:
        print(f"ERROR: {error}", file=sys.stderr)
        raise SystemExit(1)
