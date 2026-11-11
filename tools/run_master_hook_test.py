#!/usr/bin/env python3
"""Build and run the uninstalled RV32 RawHID hook ABI test under QEMU.

The ELF contains only the new veneer and synthetic test stubs. Its linker
script and load segments are confined to TEST VMA 0x100000..0x200000; it does
not contain or execute stock firmware, target addresses, MMIO, or peripherals.
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
OUTPUT_ROOT = ROOT / "build" / "firmware" / "master-hook-test"
ZIG_CACHE_ROOT = ROOT / "build" / "firmware" / "zig-cache"
TEST_VMA_START = 0x100000
TEST_VMA_END = 0x200000
ET_EXEC = 2
ET_REL = 1
EM_RISCV = 243
PT_LOAD = 1
PT_INTERP = 3
EF_RISCV_RVC = 0x1
EF_RISCV_FLOAT_ABI = 0x6
SHT_SYMTAB = 2
SHT_REL = 9
SHT_RELA = 4
STB_GLOBAL = 1
STB_WEAK = 2
SHN_UNDEF = 0
RISCV_FORBIDDEN_RELOCATIONS = {
    47: "R_RISCV_GPREL_I",
    48: "R_RISCV_GPREL_S",
    51: "R_RISCV_RELAX",
}

SOURCE_FILES = (
    Path("src/firmware/rv32_host_hook.S"),
    Path("src/firmware/rv32_task_hook.S"),
    Path("tests/master_hook_cpu.c"),
    Path("tests/master_hook_cpu_helpers.S"),
    Path("tools/run_master_hook_test.py"),
)


class HookTestError(RuntimeError):
    pass


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def run(command: list[str], cwd: Path, env: dict[str, str], timeout: int = 60) -> subprocess.CompletedProcess[str]:
    try:
        return subprocess.run(
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
        raise HookTestError(f"command timed out after {timeout}s: {command}") from error


def run_checked(command: list[str], cwd: Path, env: dict[str, str]) -> subprocess.CompletedProcess[str]:
    result = run(command, cwd, env)
    if result.returncode != 0:
        detail = (result.stdout + result.stderr).strip()
        raise HookTestError(f"command exited {result.returncode}: {command}\n{detail}")
    return result


def c_string(table: bytes, offset: int) -> str:
    if offset < 0 or offset >= len(table):
        return ""
    end = table.find(b"\0", offset)
    if end < 0:
        return ""
    return table[offset:end].decode("ascii", errors="replace")


def inspect_executable(path: Path) -> dict[str, Any]:
    data = path.read_bytes()
    if len(data) < 52 or data[:4] != b"\x7fELF":
        raise HookTestError(f"{path} is not an ELF executable.")
    if data[4:7] != b"\x01\x01\x01":
        raise HookTestError(f"{path} is not current ELF32 little-endian.")

    header = struct.unpack_from("<HHIIIIIHHHHHH", data, 16)
    (elf_type, machine, version, entry, phoff, shoff, flags, ehsize,
     phentsize, phnum, shentsize, shnum, shstrndx) = header
    if elf_type != ET_EXEC or machine != EM_RISCV or version != 1 or ehsize != 52:
        raise HookTestError(f"{path} has an unexpected ELF type, machine, or version.")
    if phentsize != 32 or phnum == 0 or phoff < 52:
        raise HookTestError(f"{path} has no supported program-header table.")
    if entry < TEST_VMA_START or entry >= TEST_VMA_END:
        raise HookTestError(f"entry 0x{entry:x} lies outside synthetic TEST VMA.")
    if not flags & EF_RISCV_RVC or flags & EF_RISCV_FLOAT_ABI:
        raise HookTestError(f"{path} has unexpected RISC-V ABI flags 0x{flags:08x}.")
    if phoff + phentsize * phnum > len(data):
        raise HookTestError(f"{path} program-header table lies outside the file.")

    load_segments = []
    has_interpreter = False
    for index in range(phnum):
        fields = struct.unpack_from("<IIIIIIII", data, phoff + index * phentsize)
        p_type, offset, vaddr, paddr, filesz, memsz, segment_flags, alignment = fields
        if p_type == PT_INTERP:
            has_interpreter = True
        elif p_type == PT_LOAD:
            if vaddr < TEST_VMA_START or vaddr + memsz > TEST_VMA_END:
                raise HookTestError(
                    f"PT_LOAD {index} range 0x{vaddr:x}..0x{vaddr + memsz:x} "
                    "lies outside synthetic TEST VMA."
                )
            if filesz > memsz or offset + filesz > len(data):
                raise HookTestError(f"PT_LOAD {index} has inconsistent file/memory bounds.")
            load_segments.append({
                "virtual_address": vaddr,
                "physical_address": paddr,
                "file_offset": offset,
                "file_size": filesz,
                "memory_size": memsz,
                "flags": segment_flags,
                "alignment": alignment,
            })
    if has_interpreter or not load_segments:
        raise HookTestError(f"{path} has an interpreter or no PT_LOAD segments.")

    if shentsize != 40 or shnum == 0 or shstrndx >= shnum:
        raise HookTestError(f"{path} has an unsupported section table.")
    if shoff + shentsize * shnum > len(data):
        raise HookTestError(f"{path} section table lies outside the file.")
    sections = [
        struct.unpack_from("<IIIIIIIIII", data, shoff + index * shentsize)
        for index in range(shnum)
    ]
    strings_header = sections[shstrndx]
    strings_start = strings_header[4]
    strings_end = strings_start + strings_header[5]
    if strings_start > len(data) or strings_end > len(data):
        raise HookTestError(f"{path} has an out-of-range section-name table.")
    strings = data[strings_start:strings_end]
    section_names = [c_string(strings, section[0]) for section in sections]
    if any(name in (".interp", ".dynamic", ".dynsym") for name in section_names):
        raise HookTestError(f"{path} unexpectedly contains dynamic-link sections.")

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


def inspect_veneer_object(path: Path, task: bool = False) -> dict[str, Any]:
    data = path.read_bytes()
    if len(data) < 52 or data[:7] != b"\x7fELF\x01\x01\x01":
        raise HookTestError(f"{path} is not ELF32 little-endian.")
    header = struct.unpack_from("<HHIIIIIHHHHHH", data, 16)
    (elf_type, machine, version, entry, phoff, shoff, flags, ehsize,
     phentsize, phnum, shentsize, shnum, shstrndx) = header
    if (elf_type != ET_REL or machine != EM_RISCV or version != 1 or
            ehsize != 52 or entry != 0 or phoff != 0 or phnum != 0):
        raise HookTestError(f"{path} is not an unplaced RV32 relocatable object.")
    if not flags & EF_RISCV_RVC or flags & EF_RISCV_FLOAT_ABI:
        raise HookTestError(f"{path} has unexpected RISC-V ABI flags 0x{flags:08x}.")
    if shentsize != 40 or shnum == 0 or shstrndx >= shnum:
        raise HookTestError(f"{path} has an unsupported section table.")
    if shoff + shentsize * shnum > len(data):
        raise HookTestError(f"{path} section table lies outside the file.")

    sections = [
        struct.unpack_from("<IIIIIIIIII", data, shoff + index * shentsize)
        for index in range(shnum)
    ]
    names_header = sections[shstrndx]
    names_start = names_header[4]
    names_end = names_start + names_header[5]
    if names_start > len(data) or names_end > len(data):
        raise HookTestError(f"{path} has an out-of-range section-name table.")
    names = data[names_start:names_end]
    section_names = [c_string(names, section[0]) for section in sections]

    undefined: set[str] = set()
    defined: set[str] = set()
    forbidden_relocations: set[str] = set()
    for section_index, section in enumerate(sections):
        section_type = section[1]
        section_offset = section[4]
        section_size = section[5]
        section_link = section[6]
        entry_size = section[9]
        if section_type in (SHT_SYMTAB, SHT_REL, SHT_RELA):
            if section_offset + section_size > len(data):
                raise HookTestError(
                    f"{path} section {section_names[section_index]!r} is out of range."
                )
        if section_type == SHT_SYMTAB:
            if entry_size != 16 or section_link >= len(sections):
                raise HookTestError(f"{path} has an unsupported symbol table.")
            strings_section = sections[section_link]
            strings_start = strings_section[4]
            strings_end = strings_start + strings_section[5]
            if strings_start > len(data) or strings_end > len(data):
                raise HookTestError(f"{path} has an out-of-range symbol-name table.")
            strings = data[strings_start:strings_end]
            for offset in range(section_offset, section_offset + section_size, entry_size):
                name_offset, _value, _size, info, _other, symbol_section = struct.unpack_from(
                    "<IIIBBH", data, offset
                )
                binding = info >> 4
                if binding not in (STB_GLOBAL, STB_WEAK):
                    continue
                name = c_string(strings, name_offset)
                if not name:
                    continue
                (undefined if symbol_section == SHN_UNDEF else defined).add(name)
        elif section_type in (SHT_REL, SHT_RELA):
            expected_size = 8 if section_type == SHT_REL else 12
            if entry_size != expected_size or entry_size == 0:
                raise HookTestError(f"{path} has an unsupported relocation table.")
            for offset in range(section_offset, section_offset + section_size, entry_size):
                if section_type == SHT_REL:
                    _reloc_offset, info = struct.unpack_from("<II", data, offset)
                else:
                    _reloc_offset, info, _addend = struct.unpack_from("<IIi", data, offset)
                relocation_type = info & 0xFF
                if relocation_type in RISCV_FORBIDDEN_RELOCATIONS:
                    forbidden_relocations.add(RISCV_FORBIDDEN_RELOCATIONS[relocation_type])

    hook = "dp104_master_task_hook" if task else "dp104_master_host_hook"
    expected_undefined = ({"dp104_master_task_pump", "dp104_master_task_legacy"} if task else
                          {"dp104_master_host_intercept", "dp104_master_host_legacy"})
    if hook not in defined or undefined != expected_undefined:
        raise HookTestError(
            f"{path} symbols differ from the veneer contract: defined hook="
            f"{'dp104_master_host_hook' in defined}, undefined={sorted(undefined)}."
        )
    if forbidden_relocations:
        raise HookTestError(
            f"{path} has forbidden GP/relax relocations: {sorted(forbidden_relocations)}."
        )

    return {
        "path": str(path.relative_to(ROOT)),
        "file_size": len(data),
        "elf_class": 32,
        "endianness": "little",
        "type": elf_type,
        "machine": machine,
        "flags": flags,
        "defined_global_symbols": sorted(defined),
        "undefined_global_symbols": sorted(undefined),
        "forbidden_gp_or_relax_relocations": sorted(forbidden_relocations),
        "section_names": section_names,
    }


def linker_script_text() -> str:
    return "\n".join((
        "ENTRY(_start)",
        "PHDRS",
        "{",
        "  test_rx PT_LOAD FLAGS(5);",
        "  test_rw PT_LOAD FLAGS(6);",
        "}",
        "SECTIONS",
        "{",
        f"  . = 0x{TEST_VMA_START:x};",
        "  .text : { *(.text .text.*) *(.rodata .rodata.*) } :test_rx",
        "  . = ALIGN(0x1000);",
        "  .data : { *(.data .data.*) } :test_rw",
        "  .bss : { *(.bss .bss.*) *(COMMON) } :test_rw",
        "  /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) }",
        "}",
        "",
    ))


def main() -> int:
    global OUTPUT_ROOT
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--zig", type=Path, default=DEFAULT_ZIG,
                        help=f"Zig executable (default: {DEFAULT_ZIG})")
    parser.add_argument("--task", action="store_true",
                        help="Test task-pump veneer preservation with the same clobbering stubs")
    args = parser.parse_args()
    veneer_source = "src/firmware/rv32_task_hook.S" if args.task else "src/firmware/rv32_host_hook.S"
    if args.task:
        OUTPUT_ROOT = ROOT / "build" / "firmware" / "master-task-hook-test"
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

    version_command = [str(zig), "version"]
    version = run_checked(version_command, ROOT, env).stdout.strip()
    if version != "0.15.2":
        print(f"Expected Zig 0.15.2, found {version!r}.", file=sys.stderr)
        return 2

    linker_script = OUTPUT_ROOT / "test_layout.ld"
    linker_script.write_text(linker_script_text(), encoding="ascii")
    veneer_object = OUTPUT_ROOT / "rv32_host_hook.o"
    object_compile_command = [
        str(zig), "cc",
        "-target", "riscv32-freestanding-none",
        "-mcpu=generic_rv32+m+c", "-mabi=ilp32",
        "-msmall-data-limit=0", "-mno-relax",
        "-fno-pic", "-fno-pie", "-Wall", "-Wextra", "-Werror",
        "-c", veneer_source,
        "-o", str(veneer_object),
    ]
    object_compile_result = run(object_compile_command, ROOT, env)
    if object_compile_result.returncode != 0:
        detail = (object_compile_result.stdout + object_compile_result.stderr).strip()
        raise HookTestError(f"veneer object compile failed:\n{detail}")
    veneer_object_info = inspect_veneer_object(veneer_object, args.task)

    executable = OUTPUT_ROOT / "master_hook_cpu"
    compile_command = [
        str(zig), "cc",
        "-target", "riscv32-linux-musl",
        "-std=c11", "-Os",
        "-ffreestanding", "-fno-builtin", "-fno-stack-protector",
        "-fno-common", "-fno-pic", "-fno-pie",
        "-fno-unwind-tables", "-fno-asynchronous-unwind-tables",
        "-nostdlib", "-static", "-no-pie",
        "-Wall", "-Wextra", "-Werror",
        "-mcpu=generic_rv32+m+c", "-mabi=ilp32",
        "-msmall-data-limit=0", "-mno-relax",
        veneer_source,
        "tests/master_hook_cpu.c",
        "tests/master_hook_cpu_helpers.S",
        "-Wl,-e,_start",
        f"-Wl,-T,{linker_script}",
        "-Wl,--build-id=none",
        "-Wl,--no-undefined",
        "-Wl,-z,max-page-size=0x1000",
        "-o", str(executable),
    ]
    if args.task:
        compile_command[2:2] = [
            "-DDP104_TASK_HOOK_TEST=1",
            "-Ddp104_master_host_hook=dp104_master_task_hook",
            "-Ddp104_master_host_intercept=dp104_master_task_pump",
            "-Ddp104_master_host_legacy=dp104_master_task_legacy",
        ]
    compile_result = run(compile_command, ROOT, env)
    if compile_result.returncode != 0:
        detail = (compile_result.stdout + compile_result.stderr).strip()
        raise HookTestError(f"compile failed:\n{detail}")

    elf = inspect_executable(executable)
    wsl_executable = to_wsl_path(executable)
    qemu_command = ["wsl.exe", "-d", "Ubuntu", "--", "/usr/bin/qemu-riscv32", wsl_executable]
    qemu_result = run(qemu_command, ROOT, env, timeout=30)

    metadata = {
        "purpose": "bounded CPU ABI test for a new, uninstalled RV32 RawHID hook veneer",
        "status": "passed" if qemu_result.returncode == 0 else "failed",
        "test_contract": [
            "false path restores entry caller-saved registers before tail-calling a test legacy stub",
            "handled path restores caller-saved registers and returns a0=0 without calling legacy",
            "callback receives a0-a2, runs with 16-byte stack alignment, and deliberately clobbers caller-saved registers",
            "both paths preserve callee-saved registers and balance the stack",
        ],
        "synthetic_bounds": {
            "vma_start_inclusive": TEST_VMA_START,
            "vma_end_exclusive": TEST_VMA_END,
            "not_firmware_placement": True,
        },
        "stock_firmware_executed": False,
        "target_addresses_used": False,
        "mmio_or_peripheral_access": False,
        "device_access": False,
        "uf2_created": False,
        "linked_with_host_libc": False,
        "zig_version": version,
        "zig_version_command": version_command,
        "compile_command": compile_command,
        "compile_stdout": compile_result.stdout.strip(),
        "compile_stderr": compile_result.stderr.strip(),
        "veneer_object_compile_command": object_compile_command,
        "veneer_object_compile_stdout": object_compile_result.stdout.strip(),
        "veneer_object_compile_stderr": object_compile_result.stderr.strip(),
        "veneer_object": veneer_object_info,
        "veneer_object_sha256": sha256(veneer_object),
        "linker_script": str(linker_script.relative_to(ROOT)),
        "linker_script_sha256": sha256(linker_script),
        "elf": elf,
        "elf_sha256": sha256(executable),
        "qemu_command": qemu_command,
        "qemu_exit_code": qemu_result.returncode,
        "qemu_stdout": qemu_result.stdout.strip(),
        "qemu_stderr": qemu_result.stderr.strip(),
        "source_hashes": {
            source.as_posix(): sha256(ROOT / source)
            for source in SOURCE_FILES
        },
    }
    metadata_path = OUTPUT_ROOT / "results.json"
    if args.task:
        metadata["purpose"] = "bounded CPU ABI test for a new uninstalled RV32 task veneer"
        metadata["test_contract"] = [
            "task pump deliberately clobbers caller-saved registers and runs with 16-byte stack alignment",
            "veneer restores entry registers and stack before tail-calling original-worker test stub",
            "legacy return reaches original caller; callee-saved registers remain intact",
        ]
    metadata_path.write_text(json.dumps(metadata, indent=2, sort_keys=True) + "\n",
                             encoding="utf-8")
    print(
        f"RV32 hook CPU ABI test: VMA 0x{TEST_VMA_START:x}..0x{TEST_VMA_END:x}; "
        f"QEMU exit={qemu_result.returncode}; results={metadata_path}"
    )
    print("This validates only the new veneer and synthetic stubs; no target firmware or device ran.")
    return 0 if qemu_result.returncode == 0 else 1


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except HookTestError as error:
        print(f"ERROR: {error}", file=sys.stderr)
        raise SystemExit(1)
