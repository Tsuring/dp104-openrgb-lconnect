#!/usr/bin/env python3
"""Build a review-only PLC twiddle payload and synthetic CPU proof."""

from __future__ import annotations

import argparse
import json
import os
import struct
import subprocess
import sys
import zipfile
from pathlib import Path
from typing import Any

from build_data_restore import (
    CPU_VMA, CPU_VMA_END, DEFAULT_ARCHIVE, DEFAULT_ZIG, PF_R, PF_W, PF_X,
    PROFILES, ROOT, RestoreBuildError, ZIG_CACHE_ROOT, check_machine_abi,
    inspect_elf, require, run, sha256, sha256_file, wsl_path,
)
from verify_target import TARGET, parse_uf2, validate_archive


OUTPUT_ROOT = ROOT / "build" / "firmware" / "twiddle-restore"
TWIDDLE_MANIFEST = ROOT / "firmware" / "targets" / "dp104-plc-twiddle.json"
CMSIS_MATCH = ROOT / ".local" / "cmsis4" / "table-match.json"
CMSIS_PROVENANCE = ROOT / ".local" / "cmsis4" / "provenance.json"
CMSIS_SOURCE = ROOT / ".local" / "cmsis4" / "arm_common_tables.c"
PLAN_PATH = ROOT / "build" / "firmware" / "hook-plan" / "plan.json"

TABLE_BYTES = 8192
TABLE_WORDS = 2048
TABLE_A = 0x0801CC5C
TABLE_B = 0x0801EC5C
TABLE_END = 0x0801EC5C
MAIN_ENTRY = 0x0801263C
MAIN_ENTRY_THUMB = MAIN_ENTRY | 1
APPCALL_SITE = 0x0801A662
TABLE_POINTER = 0x08018334
TWIDDLE_RAM = 0x2000CD1C
STATE_START = 0x2000CBB0
STATE_BYTES = 8636
TWIDDLE_OFFSET = 364
NEW_BSS_HEAP_END = 0x2000ED70
CMSIS_COMMIT = "f2cad4345783c948ed4a7f5cdb02cdc0856366f1"

PROVENANCE_PATHS = (
    "tools/build_twiddle_restore.py",
    "tools/build_data_restore.py",
    "tools/verify_target.py",
    "tools/plan_firmware_hooks.py",
    "src/firmware/dp104_twiddle.c",
    "src/firmware/dp104_twiddle.h",
    "src/firmware/dp104_patch_state.h",
    "tests/data_restore_cpu.c",
    "tests/test_twiddle.c",
    "firmware/targets/dp104-v1.2.0-8k.json",
    "firmware/targets/dp104-plc-twiddle.json",
    "build/firmware/hook-plan/plan.json",
    ".local/cmsis4/table-match.json",
    ".local/cmsis4/provenance.json",
    ".local/cmsis4/arm_common_tables.c",
)


def load_json(path: Path) -> dict[str, Any]:
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise RestoreBuildError(f"cannot read {path}: {error}") from error


def snapshot_sources() -> dict[str, str]:
    result = {}
    for relative in PROVENANCE_PATHS:
        path = ROOT / relative
        require(path.is_file(), f"provenance source is missing: {relative}")
        result[relative] = sha256_file(path)
    return result


def contract_checks(plan: dict[str, Any], manifest: dict[str, Any], raw: bytes,
                    raw_sha: str, raw_base: int, cmsis_match: dict[str, Any],
                    cmsis_provenance: dict[str, Any]) -> dict[str, Any]:
    require(plan.get("patchImplemented") is False and
            plan.get("firmwareModified") is False and
            plan.get("uf2Emitted") is False and
            plan.get("hardwareActions") is False,
            "planner is no longer an unapplied review plan")
    state = plan["storageLayout"]["plc"]
    reservation = plan["reservations"]["plc"]
    require(state["bytes"] == STATE_BYTES and state["twiddleOffset"] == TWIDDLE_OFFSET,
            "planner PLC state size or twiddle offset changed")
    require(int(reservation["stateStart"], 0) == STATE_START and
            int(reservation["reconstructedTwiddleRam"], 0) == TWIDDLE_RAM and
            int(reservation["newBssEndAndHeapStart"], 0) == NEW_BSS_HEAP_END,
            "planner PLC output/BSS/heap addresses changed")
    require(TWIDDLE_RAM == STATE_START + TWIDDLE_OFFSET and
            TWIDDLE_RAM + TABLE_BYTES <= NEW_BSS_HEAP_END,
            "planned twiddle output lies outside the reserved PLC state")
    require(plan["inputHashes"]["plc"] == raw_sha,
            "planner input hash differs from the pinned PLC image")
    plan_sources = {key.replace("\\", "/"): value
                    for key, value in plan.get("sourceHashes", {}).items()}
    for path in ("tools/plan_firmware_hooks.py", "src/firmware/dp104_patch_state.h",
                 "src/firmware/dp104_twiddle.h"):
        require(plan_sources.get(path) == sha256_file(ROOT / path),
                f"planner source hash is stale for {path}")

    recipes = [item for item in plan["reservationRecipesNotApplied"]
               if item.get("role") == "plc" and
               int(item.get("runtimeAddress", "0"), 0) == TABLE_POINTER]
    require(len(recipes) == 1, "planner table-pointer recipe missing or duplicated")
    pointer = recipes[0]
    old_pointer = struct.pack("<I", TABLE_A)
    new_pointer = struct.pack("<I", TWIDDLE_RAM)
    pointer_offset = TABLE_POINTER - raw_base
    require(pointer["imageOffset"] == pointer_offset and
            pointer["originalHex"] == old_pointer.hex() and
            pointer["replacementHex"] == new_pointer.hex() and
            raw[pointer_offset:pointer_offset + 4] == old_pointer,
            "table-pointer recipe does not match pinned raw bytes")

    require(cmsis_provenance.get("repo") == "ARM-software/CMSIS_4" and
            cmsis_provenance.get("commit") == CMSIS_COMMIT,
            "CMSIS 4 provenance is not pinned to the reviewed official revision")
    cmsis_source = next((item for item in cmsis_provenance["sources"]
                         if item.get("path", "").replace("\\", "/").endswith(
                             ".local/cmsis4/arm_common_tables.c")), None)
    require(cmsis_source is not None and
            cmsis_source["sha256"] == sha256_file(CMSIS_SOURCE),
            "local CMSIS 4 source differs from provenance hash")
    arrays = {item["name"]: item for item in cmsis_match.get("arrayMatches", [])}
    for entry in (manifest["original"], manifest["basis"]):
        match = arrays.get(entry["cmsisName"])
        require(match is not None and match.get("byteExactMatch") is True and
                match.get("bytes") == TABLE_BYTES and match.get("sha256") == entry["sha256"] and
                int(match["address"], 0) == int(entry["address"], 0),
                f"CMSIS byte-match evidence differs for {entry['cmsisName']}")

    return {
        "plan_path": str(PLAN_PATH.relative_to(ROOT)),
        "plan_sha256": sha256_file(PLAN_PATH),
        "state_start": STATE_START,
        "state_bytes": state["bytes"],
        "twiddle_offset": state["twiddleOffset"],
        "twiddle_ram": TWIDDLE_RAM,
        "new_bss_heap_end": NEW_BSS_HEAP_END,
        "table_pointer_recipe": {
            "address": TABLE_POINTER,
            "raw_file_offset": pointer_offset,
            "old_value": TABLE_A,
            "old_bytes_hex": old_pointer.hex(),
            "new_value": TWIDDLE_RAM,
            "new_bytes_hex": new_pointer.hex(),
            "applied": False,
        },
        "heap_transform_applied": False,
        "bss_heap_recipes_applied": False,
        "cmsis4": {
            "repo": cmsis_provenance["repo"],
            "commit": cmsis_provenance["commit"],
            "table_match_sha256": sha256_file(CMSIS_MATCH),
            "provenance_sha256": sha256_file(CMSIS_PROVENANCE),
            "official_source_sha256": cmsis_source["sha256"],
            "table_match_byte_exact": True,
        },
    }


def thumb_bl_target(data: bytes, address: int) -> int:
    require(len(data) >= 4, "truncated Thumb BL")
    first, second = struct.unpack_from("<HH", data)
    require(first & 0xF800 == 0xF000 and second & 0xD000 == 0xD000,
            f"0x{address:x} is not a Thumb BL")
    s, j1, j2 = (first >> 10) & 1, (second >> 13) & 1, (second >> 11) & 1
    i1, i2 = (~(j1 ^ s)) & 1, (~(j2 ^ s)) & 1
    immediate = (s << 24) | (i1 << 23) | (i2 << 22) | \
                ((first & 0x3FF) << 12) | ((second & 0x7FF) << 1)
    displacement = (immediate ^ (1 << 24)) - (1 << 24)
    return (address + 4 + displacement) & 0xFFFFFFFE


def verify_appentry_hook(code: bytes, address: int, wrapper_address: int) -> dict[str, int]:
    require(len(code) >= 28, "linked appentry hook is truncated")
    require(code[:4] == bytes.fromhex("2de90f50"),
            "appentry hook does not save r0-r3, r12 and lr in 24 bytes")
    restore_target = thumb_bl_target(code[4:8], address + 4)
    require(restore_target == wrapper_address,
            f"appentry BL target 0x{restore_target:x} differs from wrapper 0x{wrapper_address:x}")
    require(struct.unpack_from("<H", code, 8)[0] == 0x2800,
            "appentry return check is not CMP r0,#0")
    branch = struct.unpack_from("<H", code, 10)[0]
    require(branch & 0xFF00 == 0xD000, "appentry failure check is not BEQ")
    failure_target = address + 10 + 4 + (((branch & 0xFF) ^ 0x80) - 0x80) * 2
    require(failure_target == address + 20, "appentry failure branch misses halt loop")
    require(code[12:16] == bytes.fromhex("bde80f50"),
            "appentry hook does not restore r0-r3, r12 and lr")
    ldr1, ldr2 = struct.unpack_from("<HH", code, 16)
    require(ldr1 == 0xF8DF and ldr2 & 0xF000 == 0xF000,
            "appentry hook does not use LDR.W PC literal")
    literal_address = ((address + 20) & ~3) + (ldr2 & 0xFFF)
    require(literal_address == address + 24 and
            struct.unpack_from("<I", code, 24)[0] == MAIN_ENTRY_THUMB,
            "appentry hook does not resume the original Thumb main entry")
    require(struct.unpack_from("<H", code, 20)[0] == 0xE7FE,
            "appentry failure path is not a self-loop")
    return {
        "address": address,
        "bytes": 28,
        "restore_call_target": restore_target,
        "failure_branch_target": failure_target,
        "resume_main_literal": MAIN_ENTRY_THUMB,
        "stack_save_bytes": 24,
    }


def payload_linker_script(payload_start: int, payload_end: int,
                          basis_address: int) -> str:
    return f"""ENTRY(dp104_appcall_shim)
__dp104_packed_source = 0x{basis_address:x};
__dp104_initdata_ram = 0x{TWIDDLE_RAM:x};
PHDRS
{{
  appcall PT_LOAD FLAGS(5);
  payload PT_LOAD FLAGS(5);
}}
SECTIONS
{{
  .appcall 0x{APPCALL_SITE:x} : {{ KEEP(*(.appcall)) }} :appcall
  .payload 0x{payload_start:x} :
  {{
    KEEP(*(.apphook))
    . = ALIGN(4);
    KEEP(*(.text.dp104_restore_data))
    *(.text .text.*)
    *(.rodata .rodata.*)
    *(.srodata .srodata.*)
    *(.ARM.extab .ARM.extab.*)
  }} :payload
  .ARM.exidx : {{ *(.ARM.exidx .ARM.exidx.*) }} :payload
  .data : {{ *(.data .data.* .sdata .sdata.*) }} :payload
  .bss (NOLOAD) : {{ *(.bss .bss.* .sbss .sbss.*) *(COMMON) }} :payload
  .got : {{ *(.got .got.*) }} :payload
  /DISCARD/ : {{ *(.comment) *(.note .note.*) *(.eh_frame .eh_frame.*) }}
}}
ASSERT(SIZEOF(.appcall) == 4, \"appcall shim must be one Thumb BL\")
ASSERT(ADDR(.payload) == 0x{payload_start:x}, \"payload is outside the original A table\")
ASSERT(ADDR(.payload) + SIZEOF(.payload) <= 0x{payload_end:x}, \"payload exceeds A table\")
ASSERT(ADDR(.ARM.exidx) + SIZEOF(.ARM.exidx) <= 0x{payload_end:x}, \"ARM exidx exceeds A table\")
ASSERT(SIZEOF(.data) == 0, \"twiddle restore has initialized data\")
ASSERT(SIZEOF(.bss) == 0, \"twiddle restore has BSS\")
ASSERT(SIZEOF(.got) == 0, \"twiddle restore has GOT state\")
ASSERT(__dp104_packed_source == 0x{basis_address:x}, \"basis source address changed\")
ASSERT(__dp104_initdata_ram == 0x{TWIDDLE_RAM:x}, \"twiddle RAM address changed\")
ASSERT((dp104_appentry_hook & 0xfffffffe) == 0x{payload_start:x}, \"appentry hook is misplaced\")
ASSERT((dp104_appentry_hook_end & 0xfffffffe) == 0x{payload_start + 28:x}, \"appentry hook size changed\")
ASSERT((dp104_restore_data & 0xfffffffe) == 0x{payload_start + 28:x}, \"wrapper is not after appentry hook\")
ASSERT((dp104_twiddle_rebuild & 0xfffffffe) >= 0x{payload_start:x} &&
       (dp104_twiddle_rebuild & 0xfffffffe) < 0x{payload_end:x}, \"rebuild function is outside A table\")
"""


def cpu_linker_script(basis_bytes: int, original_bytes: int) -> str:
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
  .restore_data ALIGN(4) :
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
    . += 4;
    __dp104_initdata_ram = .;
    . += {original_bytes};
    __dp104_dst_guard_after = .;
    . += 4;
  }} :test_rw
  .data : {{ *(.data .data.* .sdata .sdata.*) }} :test_rw
  .bss (NOLOAD) : {{ *(.bss .bss.* .sbss .sbss.*) *(COMMON) }} :test_rw
}}
ASSERT(__dp104_packed_source >= 0x{CPU_VMA:x} &&
       __dp104_packed_source + {basis_bytes} <= 0x{CPU_VMA_END:x}, \"basis outside synthetic VMA\")
ASSERT(__dp104_original_reference >= 0x{CPU_VMA:x} &&
       __dp104_original_reference + {original_bytes} <= 0x{CPU_VMA_END:x}, \"reference outside synthetic VMA\")
ASSERT(__dp104_initdata_ram % 4 == 0 &&
       __dp104_initdata_ram + {original_bytes} <= 0x{CPU_VMA_END:x}, \"output outside aligned synthetic RAM\")
ASSERT(__dp104_dst_guard_before >= 0x{CPU_VMA:x} &&
       __dp104_dst_guard_after < 0x{CPU_VMA_END:x}, \"canaries outside synthetic VMA\")
"""


def wrapper_source() -> str:
    return """#include <stdint.h>
#include \"dp104_twiddle.h\"

extern uint32_t __dp104_initdata_ram[];
extern const uint32_t __dp104_packed_source[];

int dp104_restore_data(void)
{
    return dp104_twiddle_rebuild(__dp104_initdata_ram, 2048u,
                                 __dp104_packed_source, 2048u);
}
"""


def appentry_assembly() -> str:
    return """.syntax unified
.cpu cortex-m3
.thumb
.section .apphook,\"ax\",%progbits
.balign 2
.globl dp104_appentry_hook
.type dp104_appentry_hook,%function
.thumb_func
dp104_appentry_hook:
    push.w {r0-r3, r12, lr}
    bl dp104_restore_data
    cmp r0, #0
    beq .Lrestore_halt
    pop.w {r0-r3, r12, lr}
    ldr.w pc, .Lresume_main
.Lrestore_halt:
    b .Lrestore_halt
    .balign 4
.Lresume_main:
    .word 0x0801263d
.globl dp104_appentry_hook_end
dp104_appentry_hook_end:
.size dp104_appentry_hook, .-dp104_appentry_hook
"""


def appcall_assembly() -> str:
    return """.syntax unified
.cpu cortex-m3
.thumb
.section .appcall,\"ax\",%progbits
.balign 2
.globl dp104_appcall_shim
.type dp104_appcall_shim,%function
.thumb_func
dp104_appcall_shim:
    bl dp104_appentry_hook
.size dp104_appcall_shim, .-dp104_appcall_shim
"""


def build_payload(zig: Path, env: dict[str, str], raw: bytes,
                  raw_base: int, manifest: dict[str, Any], output_dir: Path) -> dict[str, Any]:
    profile = PROFILES["plc"]
    a_start = int(manifest["original"]["address"], 0)
    b_start = int(manifest["basis"]["address"], 0)
    a_size, b_size = int(manifest["original"]["bytes"]), int(manifest["basis"]["bytes"])
    a_end = a_start + a_size
    require(a_start == TABLE_A and b_start == TABLE_B and a_end == TABLE_END and
            b_start == a_end and a_size == TABLE_BYTES and b_size == TABLE_BYTES,
            "twiddle manifest A/B ranges differ from pinned target layout")
    a_offset, b_offset = a_start - raw_base, b_start - raw_base
    require(0 <= a_offset < b_offset and b_offset + b_size <= len(raw),
            "A/B table range is outside the pinned PLC raw image")
    original = raw[a_offset:a_offset + a_size]
    basis = raw[b_offset:b_offset + b_size]
    require(sha256(original) == manifest["original"]["sha256"],
            "A table bytes differ from pinned SHA-256")
    require(sha256(basis) == manifest["basis"]["sha256"],
            "B table bytes differ from pinned SHA-256")

    call_offset = APPCALL_SITE - raw_base
    pointer_offset = TABLE_POINTER - raw_base
    original_call = raw[call_offset:call_offset + 4]
    require(thumb_bl_target(original_call, APPCALL_SITE) == MAIN_ENTRY,
            "original appcall slot is not BL to the pinned main entry")
    require(raw[pointer_offset:pointer_offset + 4] == struct.pack("<I", TABLE_A),
            "original table pointer literal does not point to A")

    output_dir.mkdir(parents=True, exist_ok=True)
    basis_path = output_dir / "basis.bin"
    original_path = output_dir / "original_data.bin"
    basis_path.write_bytes(basis)
    original_path.write_bytes(original)
    wrapper = output_dir / "restore_wrapper.c"
    apphook = output_dir / "appentry_hook.S"
    appcall = output_dir / "appcall_shim.S"
    linker = output_dir / "payload_layout.ld"
    wrapper.write_text(wrapper_source(), encoding="ascii")
    apphook.write_text(appentry_assembly(), encoding="ascii")
    appcall.write_text(appcall_assembly(), encoding="ascii")
    linker.write_text(payload_linker_script(a_start, a_end, b_start), encoding="ascii")

    payload_elf = output_dir / "twiddle_restore_payload.elf"
    module = ROOT / "src" / "firmware" / "dp104_twiddle.c"
    flags = [
        "-target", profile["freestanding_triple"], "-std=c99", "-Os", "-g0",
        "-ffreestanding", "-fno-builtin", "-fno-stack-protector", "-fno-common",
        "-fno-pic", "-fno-pie", "-fno-unwind-tables",
        "-fno-asynchronous-unwind-tables", "-ffunction-sections", "-nostdlib",
        "-static", "-no-pie", "-Wall", "-Wextra", "-Werror", "-Isrc/firmware",
        *profile["architecture_flags"], str(module), str(wrapper), str(apphook), str(appcall),
        "-Wl,-T," + str(linker), "-Wl,--build-id=none", "-Wl,--no-undefined",
        "-Wl,-z,max-page-size=0x4", "-o", str(payload_elf),
    ]
    compile_result = run([str(zig), "cc", *flags], env)
    elf = inspect_elf(payload_elf)
    check_machine_abi(elf, profile, "twiddle restore payload")
    require(not elf["has_interpreter"] and not elf["undefined_symbols"],
            "linked payload has an interpreter or unresolved symbols")
    sections = {section["name"]: section for section in elf["sections"]}
    for name in (".data", ".sdata", ".bss", ".sbss", ".got"):
        require(sections.get(name, {"size": 0})["size"] == 0,
                f"linked payload unexpectedly contains {name}")
    loads = elf["program_headers"]
    require(len(loads) == 2 and all(item["flags"] == (PF_R | PF_X) and
                                     not item["flags"] & PF_W for item in loads),
            "payload must have only two read-only executable PT_LOAD segments")
    call_load = [item for item in loads if item["virtual_address"] == APPCALL_SITE]
    data_load = [item for item in loads if item["virtual_address"] == a_start]
    require(len(call_load) == 1 and call_load[0]["file_size"] == 4 and
            call_load[0]["memory_size"] == 4,
            "appcall PT_LOAD is not exactly four bytes")
    require(len(data_load) == 1 and data_load[0]["file_size"] ==
            data_load[0]["memory_size"] and data_load[0]["file_size"] > 28,
            "A-span payload PT_LOAD is absent or not file-backed")
    data_end = a_start + data_load[0]["memory_size"]
    require(data_end <= a_end, "linked payload exceeds original A-table span")
    require(sections.get(".appcall", {}).get("address") == APPCALL_SITE and
            sections.get(".payload", {}).get("address") == a_start,
            "linked sections differ from planned target addresses")
    require(elf["file_size"] < 65536, "linked ELF contains unexpected address-gap padding")

    elf_bytes = payload_elf.read_bytes()
    appcall_bytes = elf_bytes[call_load[0]["offset"]:call_load[0]["offset"] + 4]
    payload_bytes = elf_bytes[data_load[0]["offset"]:
                               data_load[0]["offset"] + data_load[0]["file_size"]]
    new_appcall_target = thumb_bl_target(appcall_bytes, APPCALL_SITE)
    require(new_appcall_target == a_start,
            "new appcall does not target the A-span appentry hook")
    hook_check = verify_appentry_hook(payload_bytes, a_start, a_start + 28)

    appcall_bin, payload_bin = output_dir / "appcall.bin", output_dir / "payload.bin"
    appcall_bin.write_bytes(appcall_bytes)
    payload_bin.write_bytes(payload_bytes)
    return {
        "original_callsite": {
            "address": APPCALL_SITE, "raw_file_offset": call_offset,
            "original_bytes_hex": original_call.hex(), "original_target": MAIN_ENTRY,
        },
        "appcall_shim": {
            "address": APPCALL_SITE, "raw_file_offset": call_offset,
            "length": len(appcall_bytes), "target": new_appcall_target,
            "sha256": sha256(appcall_bytes), "artifact": str(appcall_bin.relative_to(ROOT)),
        },
        "payload": {
            "address": a_start, "end_exclusive": data_end,
            "capacity_end_exclusive": a_end, "bytes": len(payload_bytes),
            "remaining_a_span_bytes": a_end - data_end, "sha256": sha256(payload_bytes),
            "artifact": str(payload_bin.relative_to(ROOT)),
            "linked_elf": str(payload_elf.relative_to(ROOT)),
            "elf_sha256": sha256_file(payload_elf), "appentry_hook": hook_check,
            "loaded_segments": loads, "all_loaded_bytes_read_only": True,
            "data_bss_got_bytes": 0, "no_undefined_enforced_by_linker": True,
            "compile_command": [str(zig), "cc", *flags],
            "compile_stdout": compile_result.stdout.strip(),
        },
        "source_tables": {
            "original_a": {
                "address": a_start, "raw_file_offset": a_offset,
                "bytes": len(original), "sha256": sha256(original),
                "private_reference": str(original_path.relative_to(ROOT)),
            },
            "basis_b": {
                "address": b_start, "raw_file_offset": b_offset,
                "bytes": len(basis), "sha256": sha256(basis),
                "private_input": str(basis_path.relative_to(ROOT)),
                "source_bytes_unchanged": True,
            },
        },
    }


def build_cpu_test(zig: Path, env: dict[str, str], output_dir: Path,
                   basis_path: Path, original_path: Path) -> dict[str, Any]:
    profile = PROFILES["plc"]
    cpu_dir = output_dir / "cpu" / profile["name"]
    cpu_dir.mkdir(parents=True, exist_ok=True)
    linker = cpu_dir / "cpu_layout.ld"
    linker.write_text(cpu_linker_script(TABLE_BYTES, TABLE_BYTES), encoding="ascii")
    inputs = cpu_dir / "cpu_inputs.S"
    inputs.write_text(
        '.section .restore_packed,"a",%progbits\n'
        f'.incbin "{basis_path.relative_to(ROOT).as_posix()}"\n'
        '.section .restore_expected,"a",%progbits\n'
        f'.incbin "{original_path.relative_to(ROOT).as_posix()}"\n',
        encoding="ascii",
    )
    output = cpu_dir / "twiddle_rebuild_cpu_test"
    module = ROOT / "src" / "firmware" / "dp104_twiddle.c"
    wrapper = output_dir / "restore_wrapper.c"
    driver = ROOT / "tests" / "data_restore_cpu.c"
    flags = [
        "-target", profile["linux_triple"], "-std=c99", "-Os", "-g0",
        "-ffreestanding", "-fno-builtin", "-fno-stack-protector", "-fno-common",
        "-fno-pic", "-fno-pie", "-fno-unwind-tables",
        "-fno-asynchronous-unwind-tables", "-ffunction-sections", "-nostdlib",
        "-static", "-no-pie", "-Wall", "-Wextra", "-Werror",
        f"-DDP104_INITDATA_LENGTH={TABLE_BYTES}", "-Isrc/firmware",
        *profile["architecture_flags"], str(module), str(wrapper), str(driver), str(inputs),
        "-Wl,-e,_start", "-Wl,-T," + str(linker), "-Wl,--build-id=none",
        "-Wl,--no-undefined", "-o", str(output),
    ]
    compile_result = run([str(zig), "cc", *flags], env)
    elf = inspect_elf(output)
    check_machine_abi(elf, profile, "twiddle CPU test")
    require(elf["elf_type"] == 2 and not elf["has_interpreter"] and
            not elf["undefined_symbols"], "CPU test ELF is not fully linked/static")
    require(CPU_VMA <= elf["entry"] < CPU_VMA_END,
            "CPU test entry is outside the synthetic Linux VMA")
    require(elf["program_headers"], "CPU test ELF has no load segments")
    for segment in elf["program_headers"]:
        require(CPU_VMA <= segment["virtual_address"] < CPU_VMA_END and
                segment["virtual_address"] + segment["memory_size"] <= CPU_VMA_END,
                "CPU test PT_LOAD is outside the synthetic Linux VMA")

    command = ["wsl.exe", "-d", "Ubuntu", "--", profile["qemu"], wsl_path(output)]
    result = run(command, env, timeout=30)
    return {
        "profile": profile["name"],
        "target_triple": profile["linux_triple"],
        "machine": elf["machine"],
        "flags": elf["flags"],
        "synthetic_vma": {"start": CPU_VMA, "end_exclusive": CPU_VMA_END},
        "linked_with_host_libc": False,
        "compile_command": [str(zig), "cc", *flags],
        "compile_stdout": compile_result.stdout.strip(),
        "linker_script": str(linker.relative_to(ROOT)),
        "elf": str(output.relative_to(ROOT)),
        "elf_sha256": sha256_file(output),
        "load_segments": elf["program_headers"],
        "qemu_command": command,
        "qemu_exit_code": result.returncode,
        "qemu_stdout": result.stdout.strip(),
        "qemu_stderr": result.stderr.strip(),
        "assertions": "wrapper returned success; all 8192 bytes equal original A; canaries intact",
        "test_boundary": "new twiddle module/wrapper and private A/B arrays only; no appentry shim, stock firmware, MMIO or peripherals",
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--archive", type=Path, default=DEFAULT_ARCHIVE)
    parser.add_argument("--zig", type=Path, default=DEFAULT_ZIG)
    args = parser.parse_args()
    archive_path, zig = args.archive.resolve(), args.zig.resolve()
    report_path = OUTPUT_ROOT / "report.json"
    report: dict[str, Any] = {
        "schema_version": 1,
        "status": "in_progress",
        "purpose": "review-only PLC twiddle reconstruction and linked appentry payload",
        "boundary": {
            "full_raw_image_written": False,
            "uf2_written": False,
            "replacement_recipe_applied": False,
            "heap_or_bss_recipe_applied": False,
            "hardware_accessed": False,
        },
        "archive_path": str(archive_path),
    }
    hashes_before = None

    try:
        require(archive_path.is_file(), f"pinned original archive not found: {archive_path}")
        require(zig.is_file(), f"Zig compiler not found: {zig}")
        require((ZIG_CACHE_ROOT / "global").is_dir() and
                (ZIG_CACHE_ROOT / "local").is_dir(),
                "existing firmware Zig caches are required")

        target_manifest = load_json(TARGET)
        manifest = load_json(TWIDDLE_MANIFEST)
        plan = load_json(PLAN_PATH)
        table_match = load_json(CMSIS_MATCH)
        cmsis_provenance = load_json(CMSIS_PROVENANCE)
        archive_check = validate_archive(archive_path)
        try:
            with zipfile.ZipFile(archive_path) as archive:
                plc = target_manifest["plc"]
                member = plc["archiveEntry"]
                require(archive.namelist().count(member) == 1,
                        "pinned archive has a missing or duplicate PLC UF2")
                raw = parse_uf2(archive.read(member), plc)
        except (OSError, KeyError, zipfile.BadZipFile) as error:
            raise RestoreBuildError(f"cannot read pinned PLC UF2: {error}") from error

        raw_sha = sha256(raw)
        require(raw_sha == target_manifest["plc"]["rawSha256"] ==
                manifest["rawImageSha256"],
                "PLC raw image differs from the pinned target/twiddle hashes")
        raw_base = int(target_manifest["plc"]["base"], 0)
        contracts = contract_checks(plan, manifest, raw, raw_sha, raw_base,
                                    table_match, cmsis_provenance)
        version = run([str(zig), "version"], os.environ.copy()).stdout.strip()
        require(version == "0.15.2", f"expected Zig 0.15.2, found {version!r}")
        env = os.environ.copy()
        env["ZIG_GLOBAL_CACHE_DIR"] = str(ZIG_CACHE_ROOT / "global")
        env["ZIG_LOCAL_CACHE_DIR"] = str(ZIG_CACHE_ROOT / "local")

        OUTPUT_ROOT.mkdir(parents=True, exist_ok=True)
        role_dir = OUTPUT_ROOT / "plc"
        role_dir.mkdir(parents=True, exist_ok=True)
        hashes_before = snapshot_sources()
        report["source_hashes"] = {"before_build": hashes_before}
        report["pinned_inputs"] = {
            "archive_sha256": sha256_file(archive_path),
            "archive_validation": archive_check,
            "raw_image_sha256": raw_sha,
            "target_manifest": str(TARGET.relative_to(ROOT)),
            "target_manifest_sha256": sha256_file(TARGET),
            "twiddle_manifest": str(TWIDDLE_MANIFEST.relative_to(ROOT)),
            "twiddle_manifest_sha256": sha256_file(TWIDDLE_MANIFEST),
            "cmsis4": contracts["cmsis4"],
        }
        report["planner_contract"] = {key: value for key, value in contracts.items()
                                      if key != "cmsis4"}
        report["zig_version"] = version
        report["cache_paths"] = {
            "global": str((ZIG_CACHE_ROOT / "global").relative_to(ROOT)),
            "local": str((ZIG_CACHE_ROOT / "local").relative_to(ROOT)),
            "reused_existing": True,
        }

        payload = build_payload(zig, env, raw, raw_base, manifest, role_dir)
        cpu = build_cpu_test(zig, env, role_dir,
                             role_dir / "basis.bin", role_dir / "original_data.bin")
        hashes_after = snapshot_sources()
        report["source_hashes"].update({
            "after_build": hashes_after,
            "unchanged": hashes_before == hashes_after,
        })
        require(hashes_before == hashes_after,
                "source, manifest, plan or CMSIS provenance changed during the build")
        report["payload"] = payload
        report["cpu_test"] = cpu
        report["limits"] = [
            "QEMU executes only the new twiddle module/wrapper with private A/B table bytes.",
            "The linked appcall/appentry hook is statically checked but not executed.",
            "No stock firmware, full image, UF2, bootloader acceptance or hardware behavior is verified.",
            "The planner table-pointer and BSS/heap recipes remain unapplied.",
        ]
        report["status"] = "pass"
    except (RestoreBuildError, ValueError, OSError, KeyError, zipfile.BadZipFile,
            subprocess.SubprocessError) as error:
        report["status"] = "failed"
        report["failure"] = str(error)
        if hashes_before is not None:
            try:
                hashes_after = snapshot_sources()
                report.setdefault("source_hashes", {}).update({
                    "after_build": hashes_after,
                    "unchanged": hashes_before == hashes_after,
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
    print(f"PLC twiddle: payload={payload['payload']['bytes']} B; QEMU exit={cpu['qemu_exit_code']} PASS")
    print(f"Review-only report: {report_path}")
    print("No raw image, UF2, planner recipe or hardware state was changed.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
