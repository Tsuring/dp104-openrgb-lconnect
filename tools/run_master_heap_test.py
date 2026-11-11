#!/usr/bin/env python3
"""Run one patched 42-byte master heap helper under bounded RV32 Linux-user QEMU.

Only the selected function fragment is taken from the pinned 8K master. The
ELF maps that fragment, a tiny authored harness, one synthetic break word, and
one synthetic stack. It does not execute the full firmware or access hardware.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import struct
import subprocess
import sys
import zipfile
from pathlib import Path
from typing import Any

from wsl_paths import to_wsl_path

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_ARCHIVE = ROOT / ".local" / "firmware" / "DP104_v1.2.0.zip"
DEFAULT_ZIG = ROOT / ".local" / "toolchain" / "zig-x86_64-windows-0.15.2" / "zig.exe"
OUTPUT_DIR = ROOT / "build" / "firmware" / "master-heap-test"
ZIG_CACHE_ROOT = ROOT / "build" / "firmware" / "zig-cache"

FRAGMENT_START = 0x21A46
FLOOR_SITE = 0x21A4C
FRAGMENT_END = 0x21A70
FRAGMENT_SIZE = FRAGMENT_END - FRAGMENT_START
FLOOR_PAIR_OFFSET = FLOOR_SITE - FRAGMENT_START
EXPECTED_OLD_FLOOR = 0x20012A04
BREAK_WORD = 0x2000DA38
GP_VALUE = 0x2000E1B0
INITIAL_SP = 0x20020000
STACK_START = 0x2001F000
STACK_SIZE = INITIAL_SP - STACK_START
TEST_START = 0x00100000
TEST_END = 0x00101000
PT_LOAD = 1
PT_DYNAMIC = 2
PT_INTERP = 3
ET_EXEC = 2
EM_RISCV = 243
EF_RISCV_RVC = 0x1
EF_RISCV_FLOAT_ABI = 0x6
SHT_SYMTAB = 2
SHT_RELA = 4
SHT_DYNAMIC = 6
SHT_REL = 9
SHT_DYNSYM = 11

sys.path.insert(0, str(ROOT / "tools"))
from plan_firmware_hooks import image_offset, rv_address_pair, rv_pair_target  # noqa: E402
from verify_target import TARGET, parse_uf2, validate_archive  # noqa: E402


class HeapTestError(RuntimeError):
    pass


def require(condition: bool, message: str) -> None:
    if not condition:
        raise HeapTestError(message)


def sha256_bytes(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def run(command: list[str], cwd: Path, env: dict[str, str], timeout: int = 90) -> subprocess.CompletedProcess[str]:
    try:
        return subprocess.run(command, cwd=cwd, env=env, text=True,
                              stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                              timeout=timeout, check=False)
    except subprocess.TimeoutExpired as error:
        raise HeapTestError(f"command timed out after {timeout}s: {command}") from error


def checked(command: list[str], cwd: Path, env: dict[str, str], timeout: int = 90) -> subprocess.CompletedProcess[str]:
    result = run(command, cwd, env, timeout)
    if result.returncode != 0:
        detail = (result.stdout + result.stderr).strip()
        raise HeapTestError(f"command exited {result.returncode}: {command}\n{detail}")
    return result


def extract_and_patch(archive_path: Path, plan_path: Path) -> dict[str, Any]:
    validation = validate_archive(archive_path)
    spec = json.loads(TARGET.read_text(encoding="utf-8"))
    master_target = spec["master"]
    with zipfile.ZipFile(archive_path) as archive:
        master_uf2 = archive.read(master_target["archiveEntry"])
    require(sha256_bytes(master_uf2) == master_target["uf2Sha256"],
            "Pinned master UF2 hash mismatch")
    master_raw = parse_uf2(master_uf2, master_target)
    require(sha256_bytes(master_raw) == "dea1d6b9f1b8dbfa323fb9a74ae3cee93cf0c3ae82b35435bdacfa4748a1ca4d",
            "Pinned 8K master raw SHA256 mismatch")

    plan = json.loads(plan_path.read_text(encoding="utf-8"))
    require(plan.get("inputHashes", {}).get("master") == sha256_bytes(master_raw),
            "Latest hook plan does not match the pinned master")
    new_floor = int(plan["reservations"]["master"]["newBssEnd"], 0)
    floor_recipes = [item for item in plan.get("reservationRecipesNotApplied", [])
                     if item.get("role") == "master" and
                     int(item.get("runtimeAddress", "0"), 0) == FLOOR_SITE]
    require(len(floor_recipes) == 1, "Expected exactly one current master heap-floor recipe")
    recipe = floor_recipes[0]

    fragment_offset = image_offset("master", FRAGMENT_START, FRAGMENT_SIZE, master_raw)
    original = master_raw[fragment_offset:fragment_offset + FRAGMENT_SIZE]
    require(len(original) == 42, "Original heap helper is not exactly 42 bytes")
    original_pair = original[FLOOR_PAIR_OFFSET:FLOOR_PAIR_OFFSET + 8]
    require(original_pair.hex() == "1717ff1f130787fb",
            "Pinned heap-floor pair differs from reviewed bytes")
    require(rv_pair_target(FLOOR_SITE, original_pair) == EXPECTED_OLD_FLOOR,
            "Original floor pair does not resolve to the expected BSS/heap boundary")

    patched_pair = rv_address_pair(FLOOR_SITE, new_floor, 14)
    require(rv_pair_target(FLOOR_SITE, patched_pair) == new_floor,
            "Python decoder did not resolve patched AUIPC+ADDI to new floor")
    require(recipe.get("replacementHex") == patched_pair.hex(),
            "Latest hook-plan replacement differs from rv_address_pair output")
    require(recipe.get("originalHex") == original_pair.hex(),
            "Latest hook-plan original pair differs from pinned master")

    patched = bytearray(original)
    patched[FLOOR_PAIR_OFFSET:FLOOR_PAIR_OFFSET + 8] = patched_pair
    require(bytes(patched[:FLOOR_PAIR_OFFSET]) == original[:FLOOR_PAIR_OFFSET] and
            bytes(patched[FLOOR_PAIR_OFFSET + 8:]) == original[FLOOR_PAIR_OFFSET + 8:],
            "Patch changed bytes outside the single 8-byte floor pair")
    return {
        "archiveValidation": validation,
        "archiveSha256": sha256_file(archive_path),
        "masterUf2Member": master_target["archiveEntry"],
        "masterUf2Sha256": sha256_bytes(master_uf2),
        "masterRawSha256": sha256_bytes(master_raw),
        "targetSpecSha256": sha256_file(TARGET),
        "planSha256": sha256_file(plan_path),
        "fragmentStart": hex(FRAGMENT_START),
        "fragmentEndExclusive": hex(FRAGMENT_END),
        "fragmentBytes": len(original),
        "originalFragmentSha256": sha256_bytes(original),
        "patchedFragmentSha256": sha256_bytes(bytes(patched)),
        "floorPairOffsetInFragment": FLOOR_PAIR_OFFSET,
        "originalFloorPair": original_pair.hex(),
        "patchedFloorPair": patched_pair.hex(),
        "oldFloor": hex(EXPECTED_OLD_FLOOR),
        "newFloorFromLatestPlan": hex(new_floor),
        "decodedPatchedTarget": hex(rv_pair_target(FLOOR_SITE, patched_pair)),
        "changedByteRange": [hex(FLOOR_PAIR_OFFSET), hex(FLOOR_PAIR_OFFSET + 8)],
        "patchedHelper": bytes(patched),
    }


def harness_source(new_floor: int) -> str:
    ceiling = INITIAL_SP - 0x40
    return f""".option push
.option norelax
.option nopic
.section .text.start,\"ax\",@progbits
.balign 4
.globl _start
.type _start,@function
_start:
    li gp, 0x{GP_VALUE:08x}
    li sp, 0x{INITIAL_SP:08x}

    # The RW PT_LOAD initializer must seed the stock global break at newfloor.
    li t0, 0x{BREAK_WORD:08x}
    lw t1, 0(t0)
    li t2, 0x{new_floor:08x}
    bne t1, t2, .Lfail_10

    # Allocate 16: return old floor and advance global break by 16.
    li a0, 16
    jal ra, heap_helper
    li t0, 0x{new_floor:08x}
    bne a0, t0, .Lfail_11
    li t0, 0x{BREAK_WORD:08x}
    lw t1, 0(t0)
    li t2, 0x{new_floor + 16:08x}
    bne t1, t2, .Lfail_12
    li t0, 0x{GP_VALUE:08x}
    bne gp, t0, .Lfail_13
    li t0, 0x{INITIAL_SP:08x}
    bne sp, t0, .Lfail_13

    # Shrink by 32 from floor+16: underflow below floor must fail unchanged.
    li a0, -32
    jal ra, heap_helper
    li t0, -1
    bne a0, t0, .Lfail_21
    li t0, 0x{BREAK_WORD:08x}
    lw t1, 0(t0)
    li t2, 0x{new_floor + 16:08x}
    bne t1, t2, .Lfail_22
    li t0, 0x{GP_VALUE:08x}
    bne gp, t0, .Lfail_23
    li t0, 0x{INITIAL_SP:08x}
    bne sp, t0, .Lfail_23

    # Grow exactly to SP-0x40: the inclusive ceiling is accepted.
    li t0, 0x{BREAK_WORD:08x}
    li t1, 0x{ceiling - 32:08x}
    sw t1, 0(t0)
    li a0, 32
    jal ra, heap_helper
    li t0, 0x{ceiling - 32:08x}
    bne a0, t0, .Lfail_31
    li t0, 0x{BREAK_WORD:08x}
    lw t1, 0(t0)
    li t2, 0x{ceiling:08x}
    bne t1, t2, .Lfail_32
    li t0, 0x{GP_VALUE:08x}
    bne gp, t0, .Lfail_33
    li t0, 0x{INITIAL_SP:08x}
    bne sp, t0, .Lfail_33

    # One byte beyond SP-0x40 must fail without changing the break.
    li a0, 1
    jal ra, heap_helper
    li t0, -1
    bne a0, t0, .Lfail_41
    li t0, 0x{BREAK_WORD:08x}
    lw t1, 0(t0)
    li t2, 0x{ceiling:08x}
    bne t1, t2, .Lfail_42
    li t0, 0x{GP_VALUE:08x}
    bne gp, t0, .Lfail_43
    li t0, 0x{INITIAL_SP:08x}
    bne sp, t0, .Lfail_43

    # Shrink from floor+16 back to floor: equality with the lower bound passes.
    li t0, 0x{BREAK_WORD:08x}
    li t1, 0x{new_floor + 16:08x}
    sw t1, 0(t0)
    li a0, -16
    jal ra, heap_helper
    li t0, 0x{new_floor + 16:08x}
    bne a0, t0, .Lfail_51
    li t0, 0x{BREAK_WORD:08x}
    lw t1, 0(t0)
    li t2, 0x{new_floor:08x}
    bne t1, t2, .Lfail_52
    li t0, 0x{GP_VALUE:08x}
    bne gp, t0, .Lfail_53
    li t0, 0x{INITIAL_SP:08x}
    bne sp, t0, .Lfail_53

    li a0, 0
    j .Lexit
.Lfail_10: li a0, 10; j .Lexit
.Lfail_11: li a0, 11; j .Lexit
.Lfail_12: li a0, 12; j .Lexit
.Lfail_13: li a0, 13; j .Lexit
.Lfail_21: li a0, 21; j .Lexit
.Lfail_22: li a0, 22; j .Lexit
.Lfail_23: li a0, 23; j .Lexit
.Lfail_31: li a0, 31; j .Lexit
.Lfail_32: li a0, 32; j .Lexit
.Lfail_33: li a0, 33; j .Lexit
.Lfail_41: li a0, 41; j .Lexit
.Lfail_42: li a0, 42; j .Lexit
.Lfail_43: li a0, 43; j .Lexit
.Lfail_51: li a0, 51; j .Lexit
.Lfail_52: li a0, 52; j .Lexit
.Lfail_53: li a0, 53; j .Lexit
.Lexit:
    li a7, 93
    ecall
    j .Lexit
.size _start, .-_start

.section .vendor_helper,\"ax\",@progbits
.balign 2
.globl heap_helper
.type heap_helper,@function
heap_helper:
    .incbin \"heap_helper.incbin\"
.size heap_helper, .-heap_helper

.section .heap_break,\"aw\",@progbits
.balign 4
.globl heap_break_word
heap_break_word:
    .word 0x{new_floor:08x}

.section .note.GNU-stack,\"\",@progbits
.option pop
"""


def linker_script_text() -> str:
    return """OUTPUT_ARCH(riscv)
ENTRY(_start)
PHDRS
{
  vendor_rx PT_LOAD FLAGS(5);
  harness_rx PT_LOAD FLAGS(5);
  heap_rw PT_LOAD FLAGS(6);
  test_stack PT_LOAD FLAGS(6);
}
SECTIONS
{
  .vendor_helper 0x00021a46 : { KEEP(*(.vendor_helper)) } :vendor_rx
  ASSERT(ADDR(.vendor_helper) == 0x00021a46, "wrong helper execution VMA")
  ASSERT(SIZEOF(.vendor_helper) == 42, "helper fragment size changed")

  .test 0x00100000 : { KEEP(*(.text.start)) } :harness_rx
  ASSERT(ADDR(.test) == 0x00100000, "wrong harness VMA")
  ASSERT(SIZEOF(.test) <= 0x1000, "harness exceeds allowed range")

  .heap_break 0x2000da38 : { KEEP(*(.heap_break)) } :heap_rw
  ASSERT(ADDR(.heap_break) == 0x2000da38, "wrong break-word VMA")
  ASSERT(SIZEOF(.heap_break) == 4, "break word size changed")

  .test_stack 0x2001f000 (NOLOAD) : { . += 0x1000; } :test_stack
  ASSERT(ADDR(.test_stack) == 0x2001f000, "wrong stack VMA")
  ASSERT(SIZEOF(.test_stack) == 0x1000, "wrong stack size")

  /DISCARD/ : { *(.comment) *(.riscv.attributes) *(.eh_frame*) *(.note*) }
}
"""


def c_string(table: bytes, offset: int) -> str:
    if offset < 0 or offset >= len(table):
        return ""
    end = table.find(b"\0", offset)
    return "" if end < 0 else table[offset:end].decode("ascii", errors="replace")


def inspect_elf(path: Path) -> dict[str, Any]:
    data = path.read_bytes()
    require(len(data) >= 52 and data[:7] == b"\x7fELF\x01\x01\x01",
            "ELF is not 32-bit little-endian")
    header = struct.unpack_from("<HHIIIIIHHHHHH", data, 16)
    (elf_type, machine, version, entry, phoff, shoff, flags, ehsize,
     phentsize, phnum, shentsize, shnum, shstrndx) = header
    require(elf_type == ET_EXEC and machine == EM_RISCV and version == 1 and ehsize == 52,
            "ELF type, architecture, or header version is wrong")
    require(entry == TEST_START, "ELF entry is not the authored Linux _start")
    require(flags & EF_RISCV_RVC and not flags & EF_RISCV_FLOAT_ABI,
            f"unexpected RISC-V ABI flags 0x{flags:08x}")
    require(phentsize == 32 and phnum > 0 and phoff >= 52 and
            phoff + phentsize * phnum <= len(data), "invalid program-header table")

    segments = []
    forbidden_types = []
    allowed_seen: set[str] = set()
    for index in range(phnum):
        p_type, p_offset, p_vaddr, p_paddr, p_filesz, p_memsz, p_flags, p_align = \
            struct.unpack_from("<IIIIIIII", data, phoff + index * phentsize)
        if p_type in (PT_INTERP, PT_DYNAMIC):
            forbidden_types.append(p_type)
        if p_type != PT_LOAD:
            continue
        require(p_filesz <= p_memsz and p_offset + p_filesz <= len(data),
                f"PT_LOAD {index} has invalid file/memory bounds")
        require(p_flags in (5, 6), f"PT_LOAD {index} has unexpected flags {p_flags}")
        require(p_align in (1, 2, 4, 8, 0x1000) and
                p_vaddr % p_align == p_offset % p_align,
                f"PT_LOAD {index} has invalid alignment")
        end = p_vaddr + p_memsz
        if p_vaddr == TEST_START:
            name = "authored_harness"
            require(TEST_START < end <= TEST_END and p_filesz == p_memsz and p_flags == 5,
                    "harness PT_LOAD escaped its RX whitelist")
        elif p_vaddr == FRAGMENT_START:
            name = "patched_vendor_fragment"
            require(p_memsz == FRAGMENT_SIZE and p_filesz == FRAGMENT_SIZE and p_flags == 5,
                    "vendor-fragment PT_LOAD is not exactly the 42-byte RX window")
        elif p_vaddr == BREAK_WORD:
            name = "synthetic_break_word"
            require(p_memsz == 4 and p_filesz == 4 and p_flags == 6,
                    "break-word PT_LOAD is not exactly one RW word")
        elif p_vaddr == STACK_START:
            name = "synthetic_noload_stack"
            require(p_memsz == STACK_SIZE and p_filesz == 0 and p_flags == 6,
                    "stack PT_LOAD is not the exact RW NOLOAD range")
        else:
            raise HeapTestError(
                f"unapproved PT_LOAD {index}: 0x{p_vaddr:08x}..0x{end:08x}"
            )
        require(name not in allowed_seen, f"duplicate PT_LOAD for {name}")
        allowed_seen.add(name)
        segments.append({"name": name, "type": p_type, "offset": p_offset,
                         "vaddr": p_vaddr, "paddr": p_paddr,
                         "filesz": p_filesz, "memsz": p_memsz,
                         "flags": p_flags, "alignment": p_align})
    require(not forbidden_types, f"forbidden dynamic/interpreter program headers: {forbidden_types}")
    required = {"authored_harness", "patched_vendor_fragment", "synthetic_break_word",
                "synthetic_noload_stack"}
    require(allowed_seen == required and len(segments) == 4,
            f"PT_LOAD whitelist mismatch: {sorted(allowed_seen)}")
    vaddrs = [s["vaddr"] for s in segments]
    require(vaddrs == sorted(vaddrs), "PT_LOAD entries are not sorted by VMA")
    ordered = sorted((s["vaddr"], s["vaddr"] + s["memsz"]) for s in segments)
    require(all(ordered[i][1] <= ordered[i + 1][0] for i in range(len(ordered) - 1)),
            "PT_LOAD virtual ranges overlap")

    require(shentsize == 40 and shnum > 0 and shstrndx < shnum and
            shoff + shentsize * shnum <= len(data), "invalid section-header table")
    sections = [struct.unpack_from("<IIIIIIIIII", data, shoff + i * shentsize)
                for i in range(shnum)]
    names_header = sections[shstrndx]
    names = data[names_header[4]:names_header[4] + names_header[5]]
    section_names = [c_string(names, section[0]) for section in sections]
    forbidden_sections = {".interp", ".dynamic", ".dynsym", ".plt", ".got.plt"}
    require(not (forbidden_sections & set(section_names)),
            "ELF contains dynamic/interpreter/linkage sections")
    require(not any(section[1] in (SHT_RELA, SHT_DYNAMIC, SHT_REL, SHT_DYNSYM)
                    for section in sections), "ELF contains dynamic or relocation sections")

    undefined = []
    for section in sections:
        if section[1] != SHT_SYMTAB:
            continue
        _name, _kind, _flags, _addr, symoff, symsize, link, _info, _align, entsize = section
        require(entsize == 16 and link < len(sections), "bad ELF symbol table")
        strings_header = sections[link]
        strings = data[strings_header[4]:strings_header[4] + strings_header[5]]
        for offset in range(symoff, symoff + symsize, entsize):
            name_offset, _value, _size, _info, _other, shndx = struct.unpack_from("<IIIBBH", data, offset)
            name = c_string(strings, name_offset)
            if shndx == 0 and name:
                undefined.append(name)
    require(not undefined, f"ELF has undefined symbols: {undefined}")
    return {"path": str(path.relative_to(ROOT)), "sha256": sha256_file(path),
            "elfClass": 32, "endianness": "little", "type": elf_type,
            "machine": machine, "flags": flags, "entry": hex(entry),
            "programHeaderCount": phnum, "loadSegments": segments,
            "sectionNames": section_names, "undefinedSymbols": undefined,
            "dynamicSections": [], "interpreter": False,
            "linkedWithHostLibc": False}


def write_results(result: dict[str, Any]) -> None:
    OUTPUT_DIR.mkdir(parents=True, exist_ok=True)
    (OUTPUT_DIR / "results.json").write_text(
        json.dumps(result, indent=2, sort_keys=True) + "\n", encoding="utf-8")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--archive", type=Path, default=DEFAULT_ARCHIVE)
    parser.add_argument("--zig", type=Path, default=DEFAULT_ZIG)
    args = parser.parse_args()
    result: dict[str, Any] = {
        "purpose": "isolated CPU proof of the stock 42-byte master heap-break helper with only its 8-byte floor pair changed",
        "status": "starting",
        "isolatedVendorFragmentExecuted": False,
        "fullMasterFirmwareExecuted": False,
        "hardwareAccessed": False,
        "mmioMapped": False,
        "physicalHeapHeadroomProven": False,
        "archivePath": str(args.archive),
        "outputDirectory": str(OUTPUT_DIR.relative_to(ROOT)),
        "testExitCodes": {
            "0": "all checks passed",
            "10": "break word did not start at the new floor",
            "11": "increment did not return the old floor",
            "12": "increment did not store floor+16",
            "13": "gp or sp changed after increment",
            "21": "shrink across floor did not return -1",
            "22": "failed shrink changed the break word",
            "23": "gp or sp changed after failed shrink",
            "31": "exact SP-0x40 growth did not return the old break",
            "32": "exact SP-0x40 growth did not store the ceiling",
            "33": "gp or sp changed after exact-ceiling growth",
            "41": "growth one byte beyond SP-0x40 did not return -1",
            "42": "failed over-ceiling growth changed the break word",
            "43": "gp or sp changed after failed over-ceiling growth",
            "51": "shrink back to floor did not return the old break",
            "52": "shrink back to floor did not store the floor",
            "53": "gp or sp changed after shrink back to floor"
        }
    }
    OUTPUT_DIR.mkdir(parents=True, exist_ok=True)
    try:
        archive_validation = extract_and_patch(args.archive.resolve(),
                                               ROOT / "build/firmware/hook-plan/plan.json")
        new_floor = int(archive_validation["newFloorFromLatestPlan"], 0)
        helper = archive_validation.pop("patchedHelper")
        incbin = OUTPUT_DIR / "heap_helper.incbin"
        incbin.write_bytes(helper)
        require(incbin.stat().st_size == FRAGMENT_SIZE,
                "Private .incbin must contain only the patched helper fragment")

        harness = OUTPUT_DIR / "heap_test.S"
        harness.write_text(harness_source(new_floor), encoding="ascii")
        linker_script = OUTPUT_DIR / "heap_test.ld"
        linker_script.write_text(linker_script_text(), encoding="ascii")

        zig = args.zig.resolve()
        require(zig.is_file(), f"Zig executable not found: {zig}")
        cache_global = ZIG_CACHE_ROOT / "global"
        cache_local = ZIG_CACHE_ROOT / "local"
        cache_global.mkdir(parents=True, exist_ok=True)
        cache_local.mkdir(parents=True, exist_ok=True)
        env = os.environ.copy()
        env["ZIG_GLOBAL_CACHE_DIR"] = str(cache_global)
        env["ZIG_LOCAL_CACHE_DIR"] = str(cache_local)
        version_command = [str(zig), "version"]
        version_result = checked(version_command, ROOT, env)
        zig_version = version_result.stdout.strip()
        require(zig_version == "0.15.2", f"Expected Zig 0.15.2, found {zig_version!r}")

        executable = OUTPUT_DIR / "master_heap_test"
        compile_command = [
            str(zig), "cc", "-target", "riscv32-linux-musl",
            "-mcpu=generic_rv32+m+c", "-mabi=ilp32", "-msmall-data-limit=0",
            "-mno-relax", "-fno-pic", "-fno-pie", "-ffreestanding",
            "-fno-builtin", "-fno-stack-protector", "-fno-unwind-tables",
            "-fno-asynchronous-unwind-tables", "-nostdlib", "-static", "-no-pie",
            "-Wl,--no-undefined", "-Wl,--build-id=none",
            "-Wl,-z,max-page-size=0x1000", f"-Wl,-T,{linker_script}",
            "-o", str(executable), "heap_test.S"
        ]
        compile_result = checked(compile_command, OUTPUT_DIR, env)
        elf = inspect_elf(executable)

        qemu_version_command = ["wsl.exe", "-d", "Ubuntu", "--",
                                "/usr/bin/qemu-riscv32", "--version"]
        qemu_version_result = checked(qemu_version_command, ROOT, env)
        wsl_executable = to_wsl_path(OUTPUT_DIR / "master_heap_test")
        qemu_command = ["wsl.exe", "-d", "Ubuntu", "--",
                        "/usr/bin/qemu-riscv32", wsl_executable]
        qemu_result = run(qemu_command, ROOT, env, timeout=30)
        result.update({
            "status": "passed" if qemu_result.returncode == 0 else "failed",
            "isolatedVendorFragmentExecuted": qemu_result.returncode in
                {int(code) for code in result["testExitCodes"]},
            "archiveAndFragment": archive_validation,
            "helperIncbin": {
                "path": str(incbin.relative_to(ROOT)),
                "bytes": incbin.stat().st_size,
                "sha256": sha256_file(incbin),
                "containsOnlySelectedFunction": True
            },
            "harness": {"path": str(harness.relative_to(ROOT)),
                        "sha256": sha256_file(harness),
                        "entry": hex(TEST_START),
                        "gp": hex(GP_VALUE), "sp": hex(INITIAL_SP)},
            "linkerScript": {"path": str(linker_script.relative_to(ROOT)),
                             "sha256": sha256_file(linker_script)},
            "toolchain": {"zig": str(zig), "version": zig_version,
                          "versionCommand": version_command,
                          "compileCommand": compile_command,
                          "compileStdout": compile_result.stdout.strip(),
                          "compileStderr": compile_result.stderr.strip()},
            "elf": elf,
            "qemu": {"versionCommand": qemu_version_command,
                     "versionStdout": qemu_version_result.stdout.strip(),
                     "command": qemu_command,
                     "exitCode": qemu_result.returncode,
                     "stdout": qemu_result.stdout.strip(),
                     "stderr": qemu_result.stderr.strip()},
            "testContract": [
                "initial break word equals the latest plan floor",
                "increment 16 returns the old floor and stores floor+16",
                "shrink by 32 across floor returns -1 and leaves break unchanged",
                "growth exactly to current sp-0x40 succeeds and stores the ceiling",
                "growth one byte beyond current sp-0x40 returns -1 unchanged",
                "shrink back to floor succeeds",
                "gp and sp remain unchanged after every helper call"
            ],
            "limitations": [
                "Only the 42-byte original helper fragment was executed; only its original 8-byte floor pair was replaced.",
                "MCU-like addresses are synthetic QEMU Linux-user virtual mappings, not physical MCU memory.",
                "No full stock firmware, MMIO, peripherals, device, or hardware was executed or accessed.",
                "This does not prove physical heap activity, allocator callers, maximum stack demand, or real-device heap headroom."
            ],
            "scriptSha256": sha256_file(Path(__file__))
        })
        write_results(result)
        print(json.dumps({"status": result["status"],
                          "qemuExitCode": qemu_result.returncode,
                          "elf": str(executable.relative_to(ROOT)),
                          "results": str((OUTPUT_DIR / "results.json").relative_to(ROOT))}, indent=2))
        return 0 if qemu_result.returncode == 0 else 1
    except Exception as error:
        result["status"] = "failed"
        result["error"] = f"{type(error).__name__}: {error}"
        write_results(result)
        print(result["error"], file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
