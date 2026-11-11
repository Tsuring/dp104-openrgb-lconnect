#!/usr/bin/env python3
"""Build review-only DP104 startup data-restore payloads and CPU checks.

The output contains replacement blobs and linked ELF evidence only. It never
rewrites a raw firmware image or UF2 file and never accesses a device.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import struct
import subprocess
import sys
import zipfile
from pathlib import Path
from typing import Any

from initdata_codec import pack_initdata, unpack_initdata
from verify_target import TARGET, parse_uf2, validate_archive
from wsl_paths import to_wsl_path


ROOT = Path(__file__).resolve().parents[1]
OUTPUT_ROOT = ROOT / "build" / "firmware" / "data-restore"
ZIG_CACHE_ROOT = ROOT / "build" / "firmware" / "zig-cache"
DEFAULT_ZIG = ROOT / ".local" / "toolchain" / "zig-x86_64-windows-0.15.2" / "zig.exe"
DEFAULT_ARCHIVE = ROOT / ".local" / "firmware" / "DP104_v1.2.0.zip"
CPU_VMA = 0x100000
CPU_VMA_END = 0x200000
PROVENANCE_SOURCES = (
    Path(__file__).resolve(),
    ROOT / "src" / "firmware" / "dp104_initdata.c",
    ROOT / "src" / "firmware" / "dp104_initdata.h",
    ROOT / "tools" / "initdata_codec.py",
    ROOT / "tests" / "data_restore_cpu.c",
    ROOT / "tools" / "verify_target.py",
)

ET_EXEC = 2
EM_ARM = 40
EM_RISCV = 243
PT_LOAD = 1
PT_INTERP = 3
SHT_SYMTAB = 2
SHT_STRTAB = 3
SHT_NOBITS = 8
PF_W = 2
PF_X = 1
PF_R = 4
EF_RISCV_RVC = 0x1
EF_RISCV_FLOAT_ABI = 0x6
EF_ARM_ABI_FLOAT_SOFT = 0x200
EF_ARM_ABI_FLOAT_HARD = 0x400
EF_ARM_EABIMASK = 0xFF000000


class RestoreBuildError(RuntimeError):
    """Raised when a review payload cannot be built or verified."""


PROFILES: dict[str, dict[str, Any]] = {
    "master": {
        "name": "rv32imc-ilp32",
        "arch": "riscv",
        "freestanding_triple": "riscv32-freestanding-none",
        "linux_triple": "riscv32-linux-musl",
        "machine": EM_RISCV,
        "cpu": "generic_rv32+m+c / ilp32",
        "qemu": "/usr/bin/qemu-riscv32",
        "architecture_flags": [
            "-mcpu=generic_rv32+m+c", "-mabi=ilp32",
            "-msmall-data-limit=0", "-mno-relax",
        ],
    },
    "plc": {
        "name": "thumb-cortex-m3-softfloat",
        "arch": "thumb",
        "freestanding_triple": "thumb-freestanding-eabi",
        "linux_triple": "thumb-linux-musleabi",
        "machine": EM_ARM,
        "cpu": "cortex_m3 / Thumb / soft-float",
        "qemu": "/usr/bin/qemu-arm",
        "architecture_flags": [
            "-mcpu=cortex_m3", "-mthumb", "-mfloat-abi=soft",
        ],
    },
}

ROLES: dict[str, dict[str, int | str]] = {
    "master": {
        "lma_start": 0x45880,
        "lma_end": 0x465D0,
        "ram_start": 0x2000CE00,
        "ram_end": 0x2000DB50,
        "startup_address": 0x2000018E,
        "startup_length": 40,
        "continuation": 0x200001B6,
        "startup_file_offset": 0x192,
    },
    "plc": {
        "lma_start": 0x08020DA8,
        "lma_end": 0x080211DC,
        "ram_start": 0x20000000,
        "ram_end": 0x20000434,
        "startup_address": 0x0801A634,
        "startup_length": 22,
        "continuation": 0x0801A64A,
        "startup_file_offset": 0x12634,
    },
}


def require(condition: bool, message: str) -> None:
    if not condition:
        raise RestoreBuildError(message)


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def sha256_file(path: Path) -> str:
    return sha256(path.read_bytes())


def source_hash_snapshot() -> dict[str, str]:
    hashes = {}
    for path in PROVENANCE_SOURCES:
        require(path.is_file(), f"provenance source is missing: {path}")
        relative = path.relative_to(ROOT).as_posix()
        hashes[relative] = sha256_file(path)
    return hashes


def run(command: list[str], env: dict[str, str], timeout: int = 90) -> subprocess.CompletedProcess[str]:
    try:
        result = subprocess.run(
            command,
            cwd=ROOT,
            env=env,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            timeout=timeout,
            check=False,
        )
    except subprocess.TimeoutExpired as error:
        raise RestoreBuildError(f"command timed out after {timeout}s: {command}") from error
    if result.returncode != 0:
        detail = (result.stdout + result.stderr).strip()
        raise RestoreBuildError(
            f"command exited {result.returncode}: {command}\n{detail}"
        )
    return result


def sign_extend(value: int, bits: int) -> int:
    sign = 1 << (bits - 1)
    return (value ^ sign) - sign


def align_up(value: int, alignment: int) -> int:
    require(alignment > 0 and alignment & (alignment - 1) == 0,
            "alignment must be a positive power of two")
    return (value + alignment - 1) & -alignment


def load_json(path: Path) -> dict[str, Any]:
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise RestoreBuildError(f"cannot read JSON evidence {path}: {error}") from error


def validate_feasibility_reports() -> dict[str, dict[str, Any]]:
    reports: dict[str, dict[str, Any]] = {}
    paths = {
        "master": ROOT / ".local" / "master-data-pack-feasibility.json",
        "plc": ROOT / ".local" / "plc-data-pack-feasibility.json",
    }
    for role, path in paths.items():
        if not path.is_file():
            reports[role] = {"present": False, "path": str(path.relative_to(ROOT))}
            continue

        report = load_json(path)
        expected = ROLES[role]
        if role == "master":
            span = report.get("InitializedDataSpan", {})
            lma = span.get("FlashLma", {})
            ram = lma.get("MappedRamVma", {})
            start = int(lma.get("Start", "-1"), 0)
            end = int(lma.get("EndExclusive", "-1"), 0)
            ram_start = int(ram.get("Start", "-1"), 0)
            ram_end = int(ram.get("EndExclusive", "-1"), 0)
            report_hash = report.get("RawImageSha256")
        else:
            span = report.get("InitializedData", {})
            lma_match = re.fullmatch(r"\[(0x[0-9a-fA-F]+),(0x[0-9a-fA-F]+)\)",
                                     span.get("flashLmaRange", ""))
            ram_match = re.fullmatch(r"\[(0x[0-9a-fA-F]+),(0x[0-9a-fA-F]+)\)",
                                     span.get("ramVmaRange", ""))
            require(lma_match is not None and ram_match is not None,
                    f"{path}: invalid initialized-data range")
            start, end = (int(value, 16) for value in lma_match.groups())
            ram_start, ram_end = (int(value, 16) for value in ram_match.groups())
            report_hash = report.get("Target", {}).get("sha256")

        require(start == int(expected["lma_start"]) and
                end == int(expected["lma_end"]) and
                ram_start == int(expected["ram_start"]) and
                ram_end == int(expected["ram_end"]),
                f"{path}: initialized-data ranges differ from the requested target")
        require(end - start == ram_end - ram_start,
                f"{path}: initialized-data source and destination lengths differ")

        reports[role] = {
            "present": True,
            "path": str(path.relative_to(ROOT)),
            "sha256": sha256_file(path),
            "raw_image_sha256": report_hash,
            "initialized_data_sha256": (
                span.get("Sha256") if role == "master" else None
            ),
        }
    return reports


def raw_image_for_role(archive_path: Path, manifest: dict[str, Any],
                       role: str) -> tuple[bytes, dict[str, Any]]:
    target = manifest[role]
    try:
        with zipfile.ZipFile(archive_path) as archive:
            member = target["archiveEntry"]
            require(archive.namelist().count(member) == 1,
                    f"archive has a missing or duplicate {role} UF2 member")
            raw = parse_uf2(archive.read(member), target)
    except (OSError, KeyError, zipfile.BadZipFile) as error:
        raise RestoreBuildError(f"cannot read pinned {role} UF2: {error}") from error
    require(sha256(raw) == target["rawSha256"],
            f"{role} raw image does not match target manifest")
    return raw, target


def master_startup_file_offset(target: dict[str, Any], role: dict[str, int | str]) -> int:
    source, _end = (int(value, 0) for value in target["codeCopySource"])
    dest_start, dest_end = (int(value, 0) for value in target["codeCopyDestination"])
    slot = int(role["startup_address"])
    require(dest_start <= slot and slot + int(role["startup_length"]) <= dest_end,
            "master startup slot is outside the copied RAM code window")
    raw_base = int(target["base"], 0)
    offset = source - raw_base + slot - dest_start
    require(offset == int(role["startup_file_offset"]),
            "master startup slot no longer maps to the pinned raw image offset")
    return offset


def wrapper_source(packed_length: int, original_length: int) -> str:
    return f"""#include <stddef.h>
#include <stdint.h>

#include \"dp104_initdata.h\"

#define DP104_PACKED_LENGTH {packed_length}u
#define DP104_ORIGINAL_LENGTH {original_length}u

extern const uint8_t __dp104_packed_source[];
extern uint8_t __dp104_initdata_ram[];

int dp104_restore_data(void)
{{
    return dp104_initdata_decode(__dp104_packed_source,
                                 (size_t)DP104_PACKED_LENGTH,
                                 __dp104_initdata_ram,
                                 (size_t)DP104_ORIGINAL_LENGTH);
}}
"""


def startup_assembly(arch: str) -> str:
    if arch == "riscv":
        return """.section .startup,\"ax\",@progbits
.balign 2
.option push
.option norvc
.option norelax
.globl dp104_startup_shim
.type dp104_startup_shim,@function
dp104_startup_shim:
    call dp104_restore_data
    bnez a0, .Lrestore_continue
.Lrestore_stop:
    j .Lrestore_stop
.Lrestore_continue:
    nop
    .rept 5
    nop
    .endr
.size dp104_startup_shim, .-dp104_startup_shim
.option pop
"""
    if arch == "thumb":
        return """.syntax unified
.cpu cortex-m3
.thumb
.section .startup,\"ax\",%progbits
.balign 2
.globl dp104_startup_shim
.type dp104_startup_shim,%function
.thumb_func
dp104_startup_shim:
    bl dp104_restore_data
    cmp r0, #0
    bne .Lrestore_continue
.Lrestore_stop:
    b .Lrestore_stop
.Lrestore_continue:
    .rept 6
    nop
    .endr
.size dp104_startup_shim, .-dp104_startup_shim
"""
    raise RestoreBuildError(f"unknown startup architecture {arch!r}")


def payload_linker_script(role: dict[str, int | str], helper_start: int) -> str:
    start = int(role["startup_address"])
    length = int(role["startup_length"])
    helper_end = int(role["lma_end"])
    return f"""ENTRY(dp104_startup_shim)
__dp104_packed_source = 0x{int(role['lma_start']):x};
__dp104_initdata_ram = 0x{int(role['ram_start']):x};
PHDRS
{{
  startup PT_LOAD FLAGS(5);
  helper PT_LOAD FLAGS(5);
}}
SECTIONS
{{
  .startup 0x{start:x} : {{ KEEP(*(.startup)) }} :startup
  .helper 0x{helper_start:x} :
  {{
    KEEP(*(.text.dp104_restore_data))
    *(.text .text.*)
    *(.rodata .rodata.*)
    *(.srodata .srodata.*)
    *(.ARM.extab .ARM.extab.*)
  }} :helper
  .ARM.exidx : {{ *(.ARM.exidx .ARM.exidx.*) }} :helper
  .data : {{ *(.data .data.* .sdata .sdata.*) }} :helper
  .bss (NOLOAD) : {{ *(.bss .bss.* .sbss .sbss.*) *(COMMON) }} :helper
  .got : {{ *(.got .got.*) }} :helper
  /DISCARD/ : {{ *(.comment) *(.note .note.*) *(.eh_frame .eh_frame.*) }}
}}
ASSERT(SIZEOF(.startup) == {length}, \"startup shim has wrong size\")
ASSERT(ADDR(.helper) == 0x{helper_start:x}, \"helper starts at wrong LMA\")
ASSERT(ADDR(.helper) + SIZEOF(.helper) <= 0x{helper_end:x}, \"helper exceeds owned LMA\")
ASSERT(SIZEOF(.data) == 0, \"restore code unexpectedly uses initialized data\")
ASSERT(SIZEOF(.bss) == 0, \"restore code unexpectedly uses BSS\")
ASSERT(SIZEOF(.got) == 0, \"restore code unexpectedly uses GOT state\")
ASSERT(__dp104_packed_source == 0x{int(role['lma_start']):x}, \"packed-source symbol has wrong address\")
ASSERT(__dp104_initdata_ram == 0x{int(role['ram_start']):x}, \"destination symbol has wrong address\")
ASSERT((dp104_restore_data & 0xfffffffe) == 0x{helper_start:x}, \"restore wrapper is not first in helper\")
ASSERT((dp104_startup_shim & 0xfffffffe) == 0x{start:x}, \"startup shim is not at the original slot\")
"""


def cpu_linker_script(packed_length: int, original_length: int) -> str:
    return f"""ENTRY(_start)
PHDRS
{{
  test_rx PT_LOAD FLAGS(5);
  test_rw PT_LOAD FLAGS(6);
}}
SECTIONS
{{
  . = 0x{CPU_VMA:x};
  .text : {{ *(.text .text.*) }} :test_rx
  .rodata : {{ *(.rodata .rodata.*) }} :test_rx
  .restore_data :
  {{
    __dp104_packed_source = .;
    KEEP(*(.restore_packed))
    . = ALIGN(4);
    __dp104_original_reference = .;
    KEEP(*(.restore_expected))
  }} :test_rx
  . = ALIGN(0x1000);
  .test_ram (NOLOAD) :
  {{
    __dp104_dst_guard_before = .;
    . += 1;
    __dp104_initdata_ram = .;
    . += {original_length};
    __dp104_dst_guard_after = .;
    . += 1;
  }} :test_rw
  .data : {{ *(.data .data.* .sdata .sdata.*) }} :test_rw
  .bss (NOLOAD) : {{ *(.bss .bss.* .sbss .sbss.*) *(COMMON) }} :test_rw
  /DISCARD/ : {{ *(.comment) *(.note .note.*) *(.eh_frame .eh_frame.*) }}
}}
ASSERT(ADDR(.text) >= 0x{CPU_VMA:x}, \"CPU test text below synthetic range\")
ASSERT(ADDR(.test_ram) + SIZEOF(.test_ram) <= 0x{CPU_VMA_END:x},
       \"CPU test data exceeds synthetic range\")
ASSERT(__dp104_packed_source >= ADDR(.restore_data) &&
       __dp104_packed_source + {packed_length} <= ADDR(.restore_data) + SIZEOF(.restore_data),
       \"packed CPU input is outside synthetic read-only data\")
ASSERT(__dp104_original_reference >= ADDR(.restore_data) &&
       __dp104_original_reference + {original_length} <= ADDR(.restore_data) + SIZEOF(.restore_data),
       \"original CPU reference is outside synthetic read-only data\")
ASSERT(__dp104_initdata_ram >= ADDR(.test_ram) &&
       __dp104_initdata_ram + {original_length} <= ADDR(.test_ram) + SIZEOF(.test_ram),
       \"restore destination is outside synthetic RAM\")
ASSERT(__dp104_dst_guard_before >= ADDR(.test_ram) &&
       __dp104_dst_guard_after < ADDR(.test_ram) + SIZEOF(.test_ram),
       \"destination canaries are outside synthetic RAM\")
"""


def c_string(data: bytes, offset: int) -> str:
    if offset < 0 or offset >= len(data):
        return ""
    end = data.find(b"\0", offset)
    if end < 0:
        return ""
    return data[offset:end].decode("ascii", errors="replace")


def inspect_elf(path: Path) -> dict[str, Any]:
    data = path.read_bytes()
    require(len(data) >= 52 and data[:4] == b"\x7fELF", f"{path} is not ELF")
    require(data[4] == 1 and data[5] == 1 and data[6] == 1,
            f"{path} is not current ELF32 little-endian")
    fields = struct.unpack_from("<HHIIIIIHHHHHH", data, 16)
    (elf_type, machine, version, entry, phoff, shoff, flags, ehsize,
     phentsize, phnum, shentsize, shnum, shstrndx) = fields
    require(version == 1 and ehsize == 52 and phentsize == 32,
            f"{path} has unsupported ELF headers")
    require(phoff + phentsize * phnum <= len(data),
            f"{path} program headers are out of range")
    require(shentsize == 40 and shnum > 0 and shstrndx < shnum and
            shoff + shentsize * shnum <= len(data),
            f"{path} section headers are unsupported or out of range")

    programs = []
    has_interp = False
    for index in range(phnum):
        p_type, p_offset, p_vaddr, p_paddr, p_filesz, p_memsz, p_flags, p_align = \
            struct.unpack_from("<IIIIIIII", data, phoff + index * phentsize)
        if p_type == PT_INTERP:
            has_interp = True
        if p_type == PT_LOAD:
            require(p_offset + p_filesz <= len(data),
                    f"{path} has a PT_LOAD outside the file")
            programs.append({
                "offset": p_offset,
                "virtual_address": p_vaddr,
                "physical_address": p_paddr,
                "file_size": p_filesz,
                "memory_size": p_memsz,
                "flags": p_flags,
                "alignment": p_align,
            })

    raw_sections = [
        struct.unpack_from("<IIIIIIIIII", data, shoff + index * shentsize)
        for index in range(shnum)
    ]
    name_header = raw_sections[shstrndx]
    require(name_header[4] + name_header[5] <= len(data),
            f"{path} section-name table is out of range")
    name_table = data[name_header[4]:name_header[4] + name_header[5]]
    sections = []
    symbols: dict[str, dict[str, int]] = {}
    undefined: set[str] = set()
    arm_attributes = ""
    symbol_table_present = False

    for raw in raw_sections:
        name_offset, section_type, section_flags, address, offset, size, link, info, align, entsize = raw
        name = c_string(name_table, name_offset)
        if section_type != SHT_NOBITS:
            require(offset + size <= len(data), f"{path} section {name!r} is out of range")
            section_data = data[offset:offset + size]
        else:
            section_data = b""
        sections.append({
            "name": name,
            "type": section_type,
            "flags": section_flags,
            "address": address,
            "offset": offset,
            "size": size,
            "link": link,
            "alignment": align,
            "entry_size": entsize,
        })
        if name == ".ARM.attributes":
            arm_attributes = section_data.decode("ascii", errors="replace")

    for index, raw in enumerate(raw_sections):
        section_type, offset, size, link, entsize = raw[1], raw[4], raw[5], raw[6], raw[9]
        if section_type != SHT_SYMTAB:
            continue
        symbol_table_present = True
        require(entsize == 16 and link < len(raw_sections),
                f"{path} has an unsupported symbol table")
        strings_header = raw_sections[link]
        require(strings_header[1] == SHT_STRTAB and
                strings_header[4] + strings_header[5] <= len(data),
                f"{path} symbol strings are out of range")
        strings = data[strings_header[4]:strings_header[4] + strings_header[5]]
        require(size % entsize == 0, f"{path} symbol table size is invalid")
        for sym_offset in range(offset, offset + size, entsize):
            name_offset, value, sym_size, info, _other, shndx = struct.unpack_from(
                "<IIIBBH", data, sym_offset
            )
            name = c_string(strings, name_offset)
            if not name:
                continue
            symbols[name] = {"value": value, "size": sym_size, "section": shndx,
                             "binding": info >> 4}
            if shndx == 0:
                undefined.add(name)

    return {
        "path": str(path.relative_to(ROOT)),
        "file_size": len(data),
        "elf_type": elf_type,
        "machine": machine,
        "flags": flags,
        "entry": entry,
        "program_headers": programs,
        "has_interpreter": has_interp,
        "sections": sections,
        "symbols": symbols,
        "symbol_table_present": symbol_table_present,
        "undefined_symbols": sorted(undefined),
        "arm_attributes": arm_attributes,
    }


def check_machine_abi(elf: dict[str, Any], profile: dict[str, Any], label: str) -> None:
    require(elf["elf_type"] == ET_EXEC, f"{label} is not a linked ET_EXEC")
    require(elf["machine"] == profile["machine"], f"{label} has the wrong ELF machine")
    require(not elf["has_interpreter"], f"{label} unexpectedly needs an interpreter")
    if profile["arch"] == "riscv":
        require(elf["flags"] & EF_RISCV_RVC and
                not elf["flags"] & EF_RISCV_FLOAT_ABI,
                f"{label} has unexpected RISC-V ABI flags 0x{elf['flags']:08x}")
    else:
        require((elf["flags"] & EF_ARM_EABIMASK) == 0x05000000 and
                elf["flags"] & EF_ARM_ABI_FLOAT_SOFT and
                not elf["flags"] & EF_ARM_ABI_FLOAT_HARD,
                f"{label} has unexpected ARM ABI flags 0x{elf['flags']:08x}")
        require("cortex-m3" in elf["arm_attributes"],
                f"{label} lacks a Cortex-M3 ARM attribute")


def validate_helper_elf(elf: dict[str, Any], profile: dict[str, Any],
                        role: dict[str, int | str], helper_start: int) -> tuple[bytes, bytes, dict[str, Any]]:
    check_machine_abi(elf, profile, "linked startup payload")
    require(not elf["undefined_symbols"],
            f"linked startup payload has unresolved symbols: {elf['undefined_symbols']}")
    sections = {section["name"]: section for section in elf["sections"]}
    require(sections.get(".startup", {}).get("address") ==
            int(role["startup_address"]),
            "linked startup output section is at the wrong address")
    require(sections.get(".helper", {}).get("address") == helper_start,
            "linked helper output section is at the wrong address")
    for name in (".data", ".sdata", ".bss", ".sbss", ".got"):
        require(sections.get(name, {"size": 0})["size"] == 0,
                f"linked startup payload unexpectedly contains {name}")
    require(not any(section["name"] in (".interp", ".dynamic", ".dynsym")
                    for section in elf["sections"]),
            "linked startup payload contains dynamic-link sections")

    load_segments = elf["program_headers"]
    require(len(load_segments) == 2,
            f"expected two PT_LOAD segments, found {len(load_segments)}")
    require(all(segment["flags"] == (PF_R | PF_X) for segment in load_segments),
            "startup/helper PT_LOAD segments must be read-only executable")
    startup_address = int(role["startup_address"])
    startup_length = int(role["startup_length"])
    startup = [segment for segment in load_segments
               if segment["virtual_address"] == startup_address]
    helper = [segment for segment in load_segments
              if segment["virtual_address"] == helper_start]
    require(len(startup) == 1 and startup[0]["file_size"] == startup_length and
            startup[0]["memory_size"] == startup_length,
            "linked startup segment does not exactly fill the original slot")
    require(len(helper) == 1 and helper[0]["file_size"] > 0 and
            helper[0]["file_size"] == helper[0]["memory_size"],
            "linked helper is absent or contains non-file-backed state")
    helper_segment = helper[0]
    helper_end = helper_start + helper_segment["memory_size"]
    require(helper_end <= int(role["lma_end"]),
            f"helper ends at 0x{helper_end:x}, beyond owned LMA end 0x{int(role['lma_end']):x}")
    require(not any(segment["flags"] & PF_W for segment in load_segments),
            "linked startup payload contains writable loaded bytes")

    restore_address = helper_start + (1 if profile["arch"] == "thumb" else 0)

    payload_file = Path(elf["path"])
    data = (ROOT / payload_file).read_bytes()
    startup_bytes = data[startup[0]["offset"]:
                         startup[0]["offset"] + startup[0]["file_size"]]
    helper_bytes = data[helper_segment["offset"]:
                        helper_segment["offset"] + helper_segment["file_size"]]
    return startup_bytes, helper_bytes, {
        "restore_function_address": restore_address,
        "helper_start": helper_start,
        "helper_end_exclusive": helper_end,
        "helper_bytes": len(helper_bytes),
        "startup_runtime_address": startup_address,
        "startup_bytes": len(startup_bytes),
    }


def verify_startup_bytes(arch: str, data: bytes, address: int,
                         restore_address: int, slot_length: int,
                         continuation: int) -> dict[str, int]:
    require(len(data) == slot_length, "startup shim byte length differs from slot")
    if arch == "riscv":
        require(len(data) == 40, "RISC-V startup shim must occupy 40 bytes")
        upper, lower = struct.unpack_from("<II", data, 0)
        require(upper & 0x7F == 0x17 and (upper >> 7) & 31 == 1,
                "RISC-V startup does not begin with AUIPC ra")
        require(lower & 0x707F == 0x67 and (lower >> 7) & 31 == 1 and
                (lower >> 15) & 31 == 1,
                "RISC-V startup does not use JALR ra,ra for the restore call")
        call_target = (address + sign_extend(upper & 0xFFFFF000, 32) +
                       sign_extend(lower >> 20, 12)) & 0xFFFFFFFF
        require(call_target == restore_address,
                f"RISC-V call targets 0x{call_target:x}, expected 0x{restore_address:x}")

        branch = struct.unpack_from("<I", data, 8)[0]
        require(branch & 0x7F == 0x63 and (branch >> 12) & 7 == 1 and
                (branch >> 15) & 31 == 10 and (branch >> 20) & 31 == 0,
                "RISC-V return check is not BNE a0,zero")
        branch_imm = (((branch >> 31) & 1) << 12) | (((branch >> 7) & 1) << 11) | \
                     (((branch >> 25) & 0x3F) << 5) | (((branch >> 8) & 0xF) << 1)
        branch_target = address + 8 + sign_extend(branch_imm, 13)
        require(branch_target == address + 16,
                "RISC-V success branch does not enter the padded fallthrough path")
        stop = struct.unpack_from("<I", data, 12)[0]
        require(stop == 0x0000006F, "RISC-V failure path is not a self-loop")
        require(all(struct.unpack_from("<I", data, offset)[0] == 0x00000013
                    for offset in range(16, 40, 4)),
                "RISC-V continuation padding is not NOP-filled")
        fallthrough = address + len(data)
    elif arch == "thumb":
        require(len(data) == 22, "Thumb startup shim must occupy 22 bytes")
        first, second = struct.unpack_from("<HH", data, 0)
        require(first & 0xF800 == 0xF000 and second & 0xD000 == 0xD000,
                "Thumb startup does not begin with BL")
        s = (first >> 10) & 1
        j1 = (second >> 13) & 1
        j2 = (second >> 11) & 1
        i1 = (~(j1 ^ s)) & 1
        i2 = (~(j2 ^ s)) & 1
        call_imm = (s << 24) | (i1 << 23) | (i2 << 22) | \
                   ((first & 0x3FF) << 12) | ((second & 0x7FF) << 1)
        call_target = (address + 4 + sign_extend(call_imm, 25)) & 0xFFFFFFFF
        require((call_target & ~1) == (restore_address & ~1),
                f"Thumb BL targets 0x{call_target:x}, expected 0x{restore_address:x}")
        require(struct.unpack_from("<H", data, 4)[0] == 0x2800,
                "Thumb startup return check is not CMP r0,#0")
        conditional = struct.unpack_from("<H", data, 6)[0]
        require(conditional & 0xFF00 == 0xD100,
                "Thumb success branch is not BNE")
        branch_target = address + 6 + 4 + sign_extend(conditional & 0xFF, 8) * 2
        require(branch_target == address + 10,
                "Thumb success branch does not enter the padded fallthrough path")
        stop = struct.unpack_from("<H", data, 8)[0]
        require(stop == 0xE7FE, "Thumb failure path is not a self-loop")
        require(all(struct.unpack_from("<H", data, offset)[0] == 0xBF00
                    for offset in range(10, 22, 2)),
                "Thumb continuation padding is not NOP-filled")
        fallthrough = address + len(data)
    else:
        raise RestoreBuildError(f"unknown shim architecture {arch!r}")

    require(fallthrough == continuation,
            f"startup falls through to 0x{fallthrough:x}, expected 0x{continuation:x}")
    return {
        "restore_call_target": restore_address,
        "success_branch_target": branch_target,
        "fallthrough_continuation": fallthrough,
    }


def compile_profile_sources(zig: Path, env: dict[str, str], role_name: str,
                            role: dict[str, int | str], profile: dict[str, Any],
                            role_dir: Path, cpu: bool = False) -> tuple[dict[str, Any], Path]:
    packed = (role_dir / "packed.bin").read_bytes()
    original = (role_dir / "original_data.bin").read_bytes()
    wrapper = role_dir / "restore_wrapper.c"
    wrapper.write_text(wrapper_source(len(packed), len(original)), encoding="ascii")
    code = ROOT / "src" / "firmware" / "dp104_initdata.c"
    if not cpu:
        helper_start = int(role["helper_start"])
        linker = role_dir / "payload_layout.ld"
        shim = role_dir / "startup.S"
        linker.write_text(payload_linker_script(role, helper_start), encoding="ascii")
        shim.write_text(startup_assembly(profile["arch"]), encoding="ascii")
        output = role_dir / "restore_payload.elf"
        command = [
            str(zig), "cc", "-target", profile["freestanding_triple"],
            "-std=c99", "-Os", "-g0", "-ffreestanding", "-fno-builtin",
            "-fno-stack-protector", "-fno-common", "-fno-pic", "-fno-pie",
            "-fno-unwind-tables", "-fno-asynchronous-unwind-tables",
            "-ffunction-sections", "-nostdlib", "-static", "-no-pie",
            "-Wall", "-Wextra", "-Werror",
            "-Isrc/firmware", *profile["architecture_flags"], str(code),
            str(wrapper), str(shim), "-Wl,-T," + str(linker),
            "-Wl,--build-id=none", "-Wl,--no-undefined",
            "-Wl,-z,max-page-size=0x4", "-o", str(output),
        ]
    else:
        cpu_dir = role_dir / "cpu" / profile["name"]
        cpu_dir.mkdir(parents=True, exist_ok=True)
        linker = cpu_dir / "cpu_layout.ld"
        linker.write_text(cpu_linker_script(len(packed), len(original)), encoding="ascii")
        inputs = cpu_dir / "cpu_inputs.S"
        inputs.write_text(
            '.section .restore_packed,"a",%progbits\n'
            f'.incbin "{(role_dir / "packed.bin").relative_to(ROOT).as_posix()}"\n'
            '.section .restore_expected,"a",%progbits\n'
            f'.incbin "{(role_dir / "original_data.bin").relative_to(ROOT).as_posix()}"\n',
            encoding="ascii",
        )
        output = cpu_dir / "data_restore_cpu_test"
        qemu_source = ROOT / "tests" / "data_restore_cpu.c"
        command = [
            str(zig), "cc", "-target", profile["linux_triple"],
            "-std=c99", "-Os", "-g0", "-ffreestanding", "-fno-builtin",
            "-fno-stack-protector", "-fno-common", "-fno-pic", "-fno-pie",
            "-fno-unwind-tables", "-fno-asynchronous-unwind-tables",
            "-ffunction-sections", "-nostdlib", "-static", "-no-pie",
            "-Wall", "-Wextra", "-Werror",
            f"-DDP104_INITDATA_LENGTH={len(original)}",
            "-Isrc/firmware", *profile["architecture_flags"], str(code),
            str(wrapper), str(qemu_source), str(inputs), "-Wl,-e,_start",
            "-Wl,-T," + str(linker), "-Wl,--build-id=none",
            "-Wl,--no-undefined", "-o", str(output),
        ]

    result = run(command, env)
    return {
        "compile_command": command,
        "compile_stdout": result.stdout.strip(),
        "linker_script": str(linker.relative_to(ROOT)),
        "linker_script_sha256": sha256_file(linker),
        "wrapper_source": str(wrapper.relative_to(ROOT)),
        "wrapper_sha256": sha256_file(wrapper),
    }, output


def wsl_path(path: Path) -> str:
    return to_wsl_path(path)


def test_cpu_wrapper(zig: Path, env: dict[str, str], role_name: str,
                     role: dict[str, int | str], profile: dict[str, Any],
                     role_dir: Path) -> dict[str, Any]:
    metadata, output = compile_profile_sources(
        zig, env, role_name, role, profile, role_dir, cpu=True
    )
    elf = inspect_elf(output)
    check_machine_abi(elf, profile, "CPU wrapper test ELF")
    require(elf["elf_type"] == ET_EXEC and not elf["undefined_symbols"],
            "CPU wrapper test ELF is not fully linked")
    require(not elf["has_interpreter"], "CPU wrapper test unexpectedly uses an interpreter")
    require(elf["entry"] >= CPU_VMA and elf["entry"] < CPU_VMA_END,
            "CPU wrapper test entry is outside synthetic VMA")
    require(elf["program_headers"], "CPU wrapper test ELF has no load segments")
    for segment in elf["program_headers"]:
        require(CPU_VMA <= segment["virtual_address"] < CPU_VMA_END and
                segment["virtual_address"] + segment["memory_size"] <= CPU_VMA_END,
                "CPU wrapper test PT_LOAD lies outside synthetic VMA")
    require(any(segment["flags"] & (PF_R | PF_X) == (PF_R | PF_X) and
                not segment["flags"] & PF_W for segment in elf["program_headers"]),
            "CPU wrapper test lacks a read-only executable segment")
    require(any(segment["flags"] & PF_W for segment in elf["program_headers"]),
            "CPU wrapper test lacks synthetic writable destination RAM")

    qemu_command = [
        "wsl.exe", "-d", "Ubuntu", "--", profile["qemu"], wsl_path(output)
    ]
    qemu_result = run(qemu_command, env, timeout=30)
    metadata.update({
        "profile": profile["name"],
        "target_triple": profile["linux_triple"],
        "cpu_profile": profile["cpu"],
        "elf": {
            "file_size": elf["file_size"],
            "machine": elf["machine"],
            "flags": elf["flags"],
            "entry": elf["entry"],
            "load_segments": elf["program_headers"],
            "undefined_symbols": elf["undefined_symbols"],
        },
        "elf_sha256": sha256_file(output),
        "qemu_command": qemu_command,
        "qemu_exit_code": qemu_result.returncode,
        "qemu_stdout": qemu_result.stdout.strip(),
        "qemu_stderr": qemu_result.stderr.strip(),
        "synthetic_vma": {"start": CPU_VMA, "end_exclusive": CPU_VMA_END},
        "linker_symbol_bounds_asserted": True,
        "linked_with_host_libc": False,
        "test_contract": (
            "new decoder and wrapper with private original/packed arrays only; "
            "exact output and destination canaries checked; no stock firmware, "
            "startup branch, MMIO, or peripheral execution"
        ),
    })
    return metadata


def build_role(zig: Path, env: dict[str, str], archive_path: Path,
               manifest: dict[str, Any], role_name: str,
               feasibility: dict[str, Any]) -> dict[str, Any]:
    role = dict(ROLES[role_name])
    profile = PROFILES[role_name]
    raw, target = raw_image_for_role(archive_path, manifest, role_name)
    lma_start = int(role["lma_start"])
    lma_end = int(role["lma_end"])
    raw_base = int(target["base"], 0)
    lma_offset = lma_start - raw_base
    lma_end_offset = lma_end - raw_base
    require(0 <= lma_offset < lma_end_offset <= len(raw),
            f"{role_name} initialized-data LMA does not map into its raw image")
    original = raw[lma_offset:lma_end_offset]
    expected_length = int(role["ram_end"]) - int(role["ram_start"])
    require(len(original) == expected_length,
            f"{role_name} LMA length differs from destination length")
    if feasibility[role_name].get("raw_image_sha256"):
        require(feasibility[role_name]["raw_image_sha256"] == sha256(raw),
                f"{role_name} raw image hash differs from static feasibility evidence")
    if feasibility[role_name].get("initialized_data_sha256"):
        require(feasibility[role_name]["initialized_data_sha256"] == sha256(original),
                f"{role_name} initialized-data bytes differ from feasibility evidence")

    packed = pack_initdata(original)
    require(unpack_initdata(packed, len(original)) == original,
            f"{role_name} optimal zero-RLE does not roundtrip original initialized data")
    helper_start = align_up(lma_start + len(packed), 4)
    helper_capacity = lma_end - helper_start
    require(helper_capacity > 0,
            f"{role_name} packed prefix ends at 0x{lma_start + len(packed):x}; "
            f"no aligned helper space remains before 0x{lma_end:x}")
    role["helper_start"] = helper_start

    role_dir = OUTPUT_ROOT / role_name
    role_dir.mkdir(parents=True, exist_ok=True)
    packed_path = role_dir / "packed.bin"
    original_path = role_dir / "original_data.bin"
    packed_path.write_bytes(packed)
    original_path.write_bytes(original)

    compile_metadata, payload_path = compile_profile_sources(
        zig, env, role_name, role, profile, role_dir, cpu=False
    )
    elf = inspect_elf(payload_path)
    startup, helper, placement = validate_helper_elf(elf, profile, role, helper_start)
    shim_check = verify_startup_bytes(
        profile["arch"], startup, int(role["startup_address"]),
        placement["restore_function_address"], int(role["startup_length"]),
        int(role["continuation"]),
    )
    startup_path = role_dir / "startup.bin"
    helper_path = role_dir / "helper.bin"
    startup_path.write_bytes(startup)
    helper_path.write_bytes(helper)

    startup_file_offset = (master_startup_file_offset(target, role)
                           if role_name == "master" else
                           int(role["startup_address"]) - raw_base)
    require(startup_file_offset == int(role["startup_file_offset"]),
            f"{role_name} startup slot maps to an unexpected raw file offset")

    cpu_results = [
        test_cpu_wrapper(zig, env, role_name, role, profile, role_dir)
    ]

    return {
        "target": {
            "archive_member": target["archiveEntry"],
            "uf2_sha256": target["uf2Sha256"],
            "raw_image_sha256": sha256(raw),
            "raw_image_bytes": len(raw),
            "raw_flash_base": raw_base,
        },
        "original_initialized_data": {
            "lma_start": lma_start,
            "lma_end_exclusive": lma_end,
            "raw_file_offset": lma_offset,
            "ram_start": int(role["ram_start"]),
            "ram_end_exclusive": int(role["ram_end"]),
            "length": len(original),
            "sha256": sha256(original),
            "private_copy": str(original_path.relative_to(ROOT)),
        },
        "compressed_prefix": {
            "format": "DP104 initdata zero-RLE",
            "lma_start": lma_start,
            "length": len(packed),
            "sha256": sha256(packed),
            "bytes_reclaimed_before_helper": len(original) - len(packed),
            "roundtrip_exact": True,
            "private_artifact": str(packed_path.relative_to(ROOT)),
        },
        "helper": {
            **placement,
            "aligned_after_packed_end": helper_start >= lma_start + len(packed),
            "alignment_padding": helper_start - (lma_start + len(packed)),
            "capacity_before_link": helper_capacity,
            "remaining_lma_bytes": lma_end - placement["helper_end_exclusive"],
            "sha256": sha256(helper),
            "linked_elf": str(payload_path.relative_to(ROOT)),
            "private_binary": str(helper_path.relative_to(ROOT)),
            "all_loaded_bytes_read_only": True,
            "fits_original_owned_lma": placement["helper_end_exclusive"] <= lma_end,
        },
        "startup_shim": {
            "runtime_address": int(role["startup_address"]),
            "raw_file_offset": startup_file_offset,
            "slot_length": int(role["startup_length"]),
            "continuation": int(role["continuation"]),
            "sha256": sha256(startup),
            "private_binary": str(startup_path.relative_to(ROOT)),
            **shim_check,
        },
        "compile": {
            **compile_metadata,
            "payload_elf_sha256": sha256_file(payload_path),
            "elf_machine": elf["machine"],
            "elf_flags": elf["flags"],
            "linked_undefined_symbols": elf["undefined_symbols"],
            "no_undefined_enforced_by_linker": True,
            "symbol_table_present": elf["symbol_table_present"],
            "loaded_segments": elf["program_headers"],
            "writable_loaded_bytes": False,
            "data_bss_got_bytes": 0,
        },
        "cpu_wrapper_tests": cpu_results,
        "replacement_recipe": [
            {
                "raw_image_file_offset": lma_offset,
                "flash_lma": lma_start,
                "length": len(packed),
                "artifact": str(packed_path.relative_to(ROOT)),
                "operation": "replace the original LMA prefix with the packed stream",
            },
            {
                "raw_image_file_offset": helper_start - raw_base,
                "flash_lma": helper_start,
                "length": len(helper),
                "artifact": str(helper_path.relative_to(ROOT)),
                "operation": "place linked read-only helper within the original data LMA",
            },
            {
                "raw_image_file_offset": startup_file_offset,
                "runtime_address": int(role["startup_address"]),
                "length": len(startup),
                "artifact": str(startup_path.relative_to(ROOT)),
                "operation": "replace only the reviewed startup copy-loop slot",
            },
        ],
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--archive", type=Path, default=DEFAULT_ARCHIVE,
                        help=f"pinned original firmware archive (default: {DEFAULT_ARCHIVE})")
    parser.add_argument("--zig", type=Path, default=DEFAULT_ZIG,
                        help=f"Zig 0.15.2 executable (default: {DEFAULT_ZIG})")
    args = parser.parse_args()
    archive_path = args.archive.resolve()
    zig = args.zig.resolve()
    report_path = OUTPUT_ROOT / "report.json"
    report: dict[str, Any] = {
        "schema_version": 1,
        "status": "in_progress",
        "purpose": "review-only linked startup restore payload feasibility",
        "artifact_boundary": (
            "replacement blobs and ELF payloads only; no modified raw image, "
            "UF2, hardware access, or firmware execution"
        ),
        "archive_path": str(archive_path),
        "generated_files": [],
    }
    source_hashes_before: dict[str, str] | None = None

    try:
        require(archive_path.is_file(), f"original archive not found: {archive_path}")
        require(zig.is_file(), f"Zig compiler not found: {zig}")
        require((ROOT / "src/firmware/dp104_initdata.c").is_file(),
                "bounded C initdata decoder is missing")
        require((ROOT / "tests/data_restore_cpu.c").is_file(),
                "CPU wrapper test driver is missing")
        require((ZIG_CACHE_ROOT / "global").is_dir() and
                (ZIG_CACHE_ROOT / "local").is_dir(),
                "existing build/firmware/zig-cache global/local caches are required")
        OUTPUT_ROOT.mkdir(parents=True, exist_ok=True)

        manifest = load_json(TARGET)
        archive_validation = validate_archive(archive_path)
        report["pinned_archive"] = {
            "sha256": sha256_file(archive_path),
            "target_manifest": str(TARGET.relative_to(ROOT)),
            "target_manifest_sha256": sha256_file(TARGET),
            "validation": archive_validation,
        }
        require(archive_validation.get("patchProduced") is False and
                archive_validation.get("bootloaderAcceptanceVerified") is False,
                "target archive validator returned an unexpected boundary")
        feasibility = validate_feasibility_reports()
        report["static_feasibility_reports"] = feasibility

        env = os.environ.copy()
        env["ZIG_GLOBAL_CACHE_DIR"] = str(ZIG_CACHE_ROOT / "global")
        env["ZIG_LOCAL_CACHE_DIR"] = str(ZIG_CACHE_ROOT / "local")
        version = run([str(zig), "version"], env).stdout.strip()
        require(version == "0.15.2", f"expected Zig 0.15.2, found {version!r}")
        report["zig_version"] = version
        report["cache_paths"] = {
            "global": str((ZIG_CACHE_ROOT / "global").relative_to(ROOT)),
            "local": str((ZIG_CACHE_ROOT / "local").relative_to(ROOT)),
            "reused_existing": True,
        }

        source_hashes_before = source_hash_snapshot()
        report["source_hashes"] = {"before_build": source_hashes_before}
        results = {}
        for role_name in ("master", "plc"):
            results[role_name] = build_role(
                zig, env, archive_path, manifest, role_name, feasibility
            )
            print(
                f"{role_name}: packed={results[role_name]['compressed_prefix']['length']} "
                f"helper={results[role_name]['helper']['helper_bytes']} "
                f"LMA end=0x{results[role_name]['helper']['helper_end_exclusive']:x} PASS"
            )

        source_hashes_after = source_hash_snapshot()
        source_hashes_unchanged = source_hashes_before == source_hashes_after
        report["source_hashes"].update({
            "after_build": source_hashes_after,
            "unchanged": source_hashes_unchanged,
        })
        require(source_hashes_unchanged,
                "provenance source hashes changed during the build")
        report["roles"] = results
        report["verification_limits"] = [
            "QEMU CPU tests execute only the new decoder/wrapper with private input arrays.",
            "The startup shim and helper placement are linked artifacts, not stock startup execution.",
            "Static reference scans do not exclude indirect or runtime-generated LMA references.",
            "No patched firmware image, UF2, bootloader acceptance, recovery path, or hardware behavior is established.",
        ]
        report["status"] = "pass"
    except (RestoreBuildError, ValueError, OSError, KeyError, zipfile.BadZipFile,
            subprocess.SubprocessError) as error:
        report["status"] = "failed"
        report["failure"] = str(error)
        if source_hashes_before is not None:
            try:
                source_hashes_after = source_hash_snapshot()
                report["source_hashes"].update({
                    "after_build": source_hashes_after,
                    "unchanged": source_hashes_before == source_hashes_after,
                })
            except RestoreBuildError as snapshot_error:
                report["source_hash_snapshot_error"] = str(snapshot_error)
        OUTPUT_ROOT.mkdir(parents=True, exist_ok=True)
        report_path.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n",
                               encoding="utf-8")
        print(f"ERROR: {error}", file=sys.stderr)
        print(f"Partial report: {report_path}", file=sys.stderr)
        return 1

    report_path.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n",
                           encoding="utf-8")
    print(f"Review-only data restore report: {report_path}")
    print("No raw image, UF2, device, or installed state was changed.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
