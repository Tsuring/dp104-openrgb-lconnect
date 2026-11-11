#!/usr/bin/env python3
"""Link and review a local PLC payload without installing or flashing it."""

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
    DEFAULT_ARCHIVE, DEFAULT_ZIG, PF_R, PF_W, PF_X, PROFILES, ROOT,
    RestoreBuildError, ZIG_CACHE_ROOT, check_machine_abi, inspect_elf,
    require, run, sha256, sha256_file,
)
from build_twiddle_restore import appentry_assembly, thumb_bl_target, verify_appentry_hook
from verify_target import TARGET, parse_uf2, validate_archive
from release_version import load_release


OUTPUT_ROOT = ROOT / "build" / "firmware" / "plc-payload"
TWIDDLE_MANIFEST = ROOT / "firmware" / "targets" / "dp104-plc-twiddle.json"
CMSIS_MATCH = ROOT / ".local" / "cmsis4" / "table-match.json"
CMSIS_PROVENANCE = ROOT / ".local" / "cmsis4" / "provenance.json"
CMSIS_SOURCE = ROOT / ".local" / "cmsis4" / "arm_common_tables.c"
PLAN_PATH = ROOT / "build" / "firmware" / "hook-plan" / "plan.json"

PLC_BASE = 0x08008000
APP_ENTRY = 0x0801263C
APPCALL_SITE = 0x0801A662
QUEUECALL_SITE = 0x08013A5E
SETTER_SITE = 0x0800DD30
UART_RX_CALLBACK_WORD = 0x08012BA8
UART_RX_CALLBACK_OLD = 0x08015BC5
QUEUE_POP = 0x08018F24
TABLE_A = 0x0801CC5C
TABLE_B = 0x0801EC5C
TABLE_BYTES = 8192
TABLE_POINTER = 0x08018334
PLC_STATE_START = 0x2000CBB0
PLC_TWIDDLE_OFFSET = 364
PLC_TWIDDLE_RAM = 0x2000CD1C
BSS_END_WORD = 0x0801A678
HEAP_BASE_WORD = 0x0800CE84
KEY_OUTPUT = 0x20002348
SETTER_RESUME_THUMB = 0x0800DD35
MAIN_ENTRY_THUMB = APP_ENTRY | 1
CMSIS_COMMIT = "f2cad4345783c948ed4a7f5cdb02cdc0856366f1"

PROVENANCE_PATHS = (
    "tools/build_plc_payload.py",
    "tools/release_version.py",
    "CMakeLists.txt",
    "tools/build_twiddle_restore.py",
    "tools/build_data_restore.py",
    "tools/verify_target.py",
    "tools/plan_firmware_hooks.py",
    "src/firmware/dp104_plc_target.c",
    "src/firmware/dp104_plc_target.h",
    "src/firmware/dp104_patch_state.h",
    "src/firmware/dp104_receiver.c",
    "src/firmware/dp104_receiver.h",
    "src/firmware/dp104_wire.c",
    "src/firmware/dp104_wire.h",
    "src/firmware/dp104_wire_stream.c",
    "src/firmware/dp104_wire_stream.h",
    "src/firmware/dp104_twiddle.c",
    "src/firmware/dp104_twiddle.h",
    "src/firmware/freestanding_memory.c",
    "src/firmware/freestanding/string.h",
    "src/firmware/thumb_setter_hook.S",
    "src/firmware/dp104_master_bridge.h",
    "src/firmware/dp104_usb_stage.h",
    "include/dp104_protocol.h",
    "include/dp104_version.h",
    "firmware/compatibility.json",
    "firmware/targets/dp104-v1.2.0-8k.json",
    "firmware/targets/dp104-plc-twiddle.json",
    "build/firmware/hook-plan/plan.json",
    ".local/plc-data-pack-feasibility.json",
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
        require(path.is_file(), f"provenance input is missing: {relative}")
        result[relative] = sha256_file(path)
    return result


def thumb_wide_target(data: bytes, address: int, link: bool) -> int:
    require(len(data) >= 4, "truncated Thumb wide branch")
    first, second = struct.unpack_from("<HH", data)
    expected = 0xD000 if link else 0x9000
    require(first & 0xF800 == 0xF000 and second & 0xD000 == expected,
            f"0x{address:x} has the wrong Thumb wide-branch opcode")
    s, j1, j2 = (first >> 10) & 1, (second >> 13) & 1, (second >> 11) & 1
    i1, i2 = (~(j1 ^ s)) & 1, (~(j2 ^ s)) & 1
    immediate = (s << 24) | (i1 << 23) | (i2 << 22) | \
                ((first & 0x3FF) << 12) | ((second & 0x7FF) << 1)
    displacement = (immediate ^ (1 << 24)) - (1 << 24)
    return (address + 4 + displacement) & 0xFFFFFFFE


def appentry_source() -> str:
    return (appentry_assembly()
            .replace("dp104_appentry_hook", "dp104_plc_appentry_hook")
            .replace("dp104_restore_data", "dp104_plc_target_init"))


def appcall_sources() -> tuple[str, str, str]:
    prefix = ".syntax unified\n.cpu cortex-m3\n.thumb\n"
    appcall = prefix + """.section .appcall,"ax",%progbits
.balign 2
.globl dp104_plc_appcall_shim
.type dp104_plc_appcall_shim,%function
.thumb_func
dp104_plc_appcall_shim:
    bl dp104_plc_appentry_hook
.size dp104_plc_appcall_shim, .-dp104_plc_appcall_shim
"""
    queuecall = prefix + """.section .queuecall,"ax",%progbits
.balign 2
.globl dp104_plc_queuecall_shim
.type dp104_plc_queuecall_shim,%function
.thumb_func
dp104_plc_queuecall_shim:
    bl dp104_plc_queue_hook
.size dp104_plc_queuecall_shim, .-dp104_plc_queuecall_shim
"""
    setterjump = prefix + """.section .setterjump,"ax",%progbits
.balign 2
.globl dp104_plc_setterjump_shim
.type dp104_plc_setterjump_shim,%function
.thumb_func
dp104_plc_setterjump_shim:
    b.w dp104_plc_setter_hook
.size dp104_plc_setterjump_shim, .-dp104_plc_setterjump_shim
"""
    return appcall, queuecall, setterjump


def setter_legacy_source() -> str:
    return """.syntax unified
.cpu cortex-m3
.thumb
.section .setter_legacy,"ax",%progbits
.balign 4
.globl dp104_plc_setter_legacy
.type dp104_plc_setter_legacy,%function
.thumb_func
dp104_plc_setter_legacy:
    push {r4,r5,r6,r7,lr}
    ldr r5, .Lkey_output
    ldr.w pc, .Lsetter_resume
    .balign 4
.Lkey_output:
    .word 0x20002348
.Lsetter_resume:
    .word 0x0800dd35
.size dp104_plc_setter_legacy, .-dp104_plc_setter_legacy
"""


def payload_linker_script(plan: dict[str, Any]) -> str:
    reservation = plan["reservations"]["plc"]
    state_bytes = int(plan["storageLayout"]["plc"]["bytes"])
    state_start = int(reservation["stateStart"], 0)
    new_end = int(reservation["newBssEndAndHeapStart"], 0)
    twiddle_ram = int(reservation["reconstructedTwiddleRam"], 0)
    return f"""ENTRY(dp104_plc_appentry_hook)
__dp104_plc_state = 0x{state_start:x};
__dp104_twiddle_basis = 0x{TABLE_B:x};
__dp104_plc_twiddle_ram = 0x{twiddle_ram:x};
PHDRS
{{
  appcall PT_LOAD FLAGS(5);
  queuecall PT_LOAD FLAGS(5);
  setterjump PT_LOAD FLAGS(5);
  payload PT_LOAD FLAGS(5);
}}
SECTIONS
{{
  .appcall 0x{APPCALL_SITE:x} : {{ KEEP(*(.appcall)) }} :appcall
  .queuecall 0x{QUEUECALL_SITE:x} : {{ KEEP(*(.queuecall)) }} :queuecall
  .setterjump 0x{SETTER_SITE:x} : {{ KEEP(*(.setterjump)) }} :setterjump
  .apphook 0x{TABLE_A:x} : {{ KEEP(*(.apphook)) }} :payload
  .target_init : {{ KEEP(*(.text.dp104_plc_target_init)) }} :payload
  .queue_hook : {{ KEEP(*(.text.dp104_plc_queue_hook)) }} :payload
  .uart_rx_hook : {{ KEEP(*(.text.dp104_plc_uart_rx)) }} :payload
  .setter_hook : {{ KEEP(*(.text.dp104_plc_setter_hook)) }} :payload
  .setter_predicate : {{ KEEP(*(.text.dp104_plc_should_skip_setter)) }} :payload
  .setter_legacy ALIGN(4) : {{ KEEP(*(.setter_legacy)) }} :payload
  .core :
  {{
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
ASSERT(SIZEOF(.appcall) == 4, \"appcall must be one BL\")
ASSERT(SIZEOF(.queuecall) == 4, \"queuecall must be one BL\")
ASSERT(SIZEOF(.setterjump) == 4, \"setter site must be one B.W\")
ASSERT(ADDR(.apphook) == 0x{TABLE_A:x}, \"appentry hook is not at A start\")
ASSERT(ADDR(.apphook) + SIZEOF(.apphook) <= 0x{TABLE_B:x}, \"appentry hook exceeds A table\")
ASSERT(ADDR(.ARM.exidx) + SIZEOF(.ARM.exidx) <= 0x{TABLE_B:x}, \"ARM exidx exceeds A table\")
ASSERT(SIZEOF(.data) == 0, \"PLC payload has initialized data\")
ASSERT(SIZEOF(.bss) == 0, \"PLC payload has BSS\")
ASSERT(SIZEOF(.got) == 0, \"PLC payload has GOT state\")
ASSERT(__dp104_plc_state == 0x{state_start:x}, \"PLC state address mismatch\")
ASSERT(__dp104_twiddle_basis == 0x{TABLE_B:x}, \"B table address mismatch\")
ASSERT(__dp104_plc_twiddle_ram == 0x{twiddle_ram:x}, \"twiddle RAM address mismatch\")
ASSERT((dp104_plc_appentry_hook & 0xfffffffe) == ADDR(.apphook), \"appentry symbol mismatch\")
ASSERT((dp104_plc_appentry_hook_end & 0xfffffffe) == ADDR(.apphook) + 28, \"appentry hook size changed\")
ASSERT((dp104_plc_target_init & 0xfffffffe) == ADDR(.target_init), \"target init section mismatch\")
ASSERT((dp104_plc_queue_hook & 0xfffffffe) == ADDR(.queue_hook), \"queue hook section mismatch\")
ASSERT((dp104_plc_uart_rx & 0xfffffffe) == ADDR(.uart_rx_hook), \"UART RX hook section mismatch\")
ASSERT((dp104_plc_setter_hook & 0xfffffffe) == ADDR(.setter_hook), \"setter hook section mismatch\")
ASSERT((dp104_plc_should_skip_setter & 0xfffffffe) == ADDR(.setter_predicate), \"setter predicate section mismatch\")
ASSERT((dp104_plc_setter_legacy & 0xfffffffe) == ADDR(.setter_legacy), \"legacy setter section mismatch\")
ASSERT(ADDR(.setter_legacy) % 4 == 0 && SIZEOF(.setter_legacy) == 16, \"legacy setter alignment/size mismatch\")
ASSERT(ADDR(.setter_legacy) + SIZEOF(.setter_legacy) <= 0x{TABLE_B:x}, \"legacy setter exceeds A table\")
ASSERT(__dp104_plc_state + {state_bytes} <= 0x{new_end:x}, \"full PLC state exceeds reservation\")
ASSERT(0x{twiddle_ram:x} + 8192 <= 0x{new_end:x}, \"twiddle RAM exceeds PLC reservation\")
"""


def validate_plan(plan: dict[str, Any], raw: bytes, raw_sha: str,
                  raw_base: int) -> dict[str, Any]:
    require(plan.get("patchImplemented") is False and
            plan.get("firmwareModified") is False and
            plan.get("uf2Emitted") is False and
            plan.get("hardwareActions") is False,
            "planner is not an unapplied review plan")
    layout = plan["storageLayout"]["plc"]
    reservation = plan["reservations"]["plc"]
    state_start = int(reservation["stateStart"], 0)
    new_end = int(reservation["newBssEndAndHeapStart"], 0)
    twiddle_ram = int(reservation["reconstructedTwiddleRam"], 0)
    require(layout["twiddleOffset"] == 364 and
            twiddle_ram == state_start + layout["twiddleOffset"] and
            twiddle_ram + TABLE_BYTES <= new_end,
            "planner twiddle RAM offset or reservation range is inconsistent")
    require(int(reservation["previousBssEndAndHeapStart"], 0) == 0x2000CBA8,
            "planner previous PLC BSS/heap boundary changed")
    require(plan["inputHashes"]["plc"] == raw_sha,
            "planner input hash differs from pinned PLC image")
    plan_hashes = {key.replace("\\", "/"): value
                   for key, value in plan.get("sourceHashes", {}).items()}
    for path in ("tools/plan_firmware_hooks.py", "src/firmware/dp104_patch_state.h",
                 "src/firmware/dp104_twiddle.h"):
        require(plan_hashes.get(path) == sha256_file(ROOT / path),
                f"planner source hash is stale for {path}")

    recipes = {}
    for item in plan["reservationRecipesNotApplied"]:
        if item.get("role") == "plc":
            recipes[int(item["runtimeAddress"], 0)] = item
    desired = {
        BSS_END_WORD: new_end,
        HEAP_BASE_WORD: new_end,
        TABLE_POINTER: twiddle_ram,
    }
    checked = {}
    for address, new_value in desired.items():
        item = recipes.get(address)
        require(item is not None, f"planner recipe missing for 0x{address:x}")
        offset = address - raw_base
        old_bytes = bytes.fromhex(item["originalHex"])
        new_bytes = bytes.fromhex(item["replacementHex"])
        require(item["imageOffset"] == offset and len(old_bytes) == 4 and
                len(new_bytes) == 4 and new_bytes == struct.pack("<I", new_value) and
                raw[offset:offset + 4] == old_bytes,
                f"planner replacement recipe differs from raw bytes at 0x{address:x}")
        checked[address] = {
            "offset": offset,
            "old": old_bytes,
            "new": new_bytes,
            "purpose": item["purpose"],
        }
    return {
        "plan_sha256": sha256_file(PLAN_PATH),
        "state_start": state_start,
        "state_bytes": layout["bytes"],
        "twiddle_offset": layout["twiddleOffset"],
        "twiddle_ram": twiddle_ram,
        "new_bss_heap_end": new_end,
        "recipes": checked,
        "applied": False,
    }


def validate_cmsis(manifest: dict[str, Any], table_match: dict[str, Any],
                   provenance: dict[str, Any]) -> dict[str, Any]:
    require(provenance.get("repo") == "ARM-software/CMSIS_4" and
            provenance.get("commit") == "f2cad4345783c948ed4a7f5cdb02cdc0856366f1",
            "CMSIS 4 provenance differs from the pinned official revision")
    source = next((item for item in provenance["sources"]
                   if item.get("path", "").replace("\\", "/").endswith(
                       ".local/cmsis4/arm_common_tables.c")), None)
    require(source is not None and source["sha256"] == sha256_file(CMSIS_SOURCE),
            "CMSIS table source differs from provenance hash")
    matches = {item["name"]: item for item in table_match["arrayMatches"]}
    for entry in (manifest["original"], manifest["basis"]):
        match = matches.get(entry["cmsisName"])
        require(match is not None and match.get("byteExactMatch") is True and
                match.get("bytes") == TABLE_BYTES and match.get("sha256") == entry["sha256"] and
                int(match["address"], 0) == int(entry["address"], 0),
                f"CMSIS table match does not confirm {entry['cmsisName']}")
    return {
        "repo": provenance["repo"],
        "commit": provenance["commit"],
        "source_sha256": source["sha256"],
        "table_match_sha256": sha256_file(CMSIS_MATCH),
        "provenance_sha256": sha256_file(CMSIS_PROVENANCE),
        "byte_exact_full_tables": True,
    }


def get_raw_inputs(archive_path: Path) -> tuple[dict[str, Any], bytes, bytes, dict[str, Any]]:
    target_manifest = load_json(TARGET)
    table_manifest = load_json(TWIDDLE_MANIFEST)
    archive_validation = validate_archive(archive_path)
    try:
        with zipfile.ZipFile(archive_path) as archive:
            plc = target_manifest["plc"]
            member = plc["archiveEntry"]
            require(archive.namelist().count(member) == 1,
                    "pinned archive has a missing or duplicate PLC UF2")
            original_uf2 = archive.read(member)
            raw = parse_uf2(original_uf2, plc)
    except (OSError, KeyError, zipfile.BadZipFile) as error:
        raise RestoreBuildError(f"cannot read pinned PLC UF2: {error}") from error
    raw_sha = sha256(raw)
    require(raw_sha == target_manifest["plc"]["rawSha256"] ==
            table_manifest["rawImageSha256"],
            "PLC image hash differs from pinned manifests")
    return target_manifest, raw, original_uf2, {
        "sha256": raw_sha,
        "archive_sha256": sha256_file(archive_path),
        "archive_validation": archive_validation,
        "target_manifest_sha256": sha256_file(TARGET),
        "twiddle_manifest_sha256": sha256_file(TWIDDLE_MANIFEST),
        "raw_base": int(target_manifest["plc"]["base"], 0),
    }


def validate_tables(table_manifest: dict[str, Any], table_match: dict[str, Any],
                    raw: bytes, raw_base: int) -> dict[str, Any]:
    original = table_manifest["original"]
    basis = table_manifest["basis"]
    require(int(original["address"], 0) == TABLE_A and
            int(basis["address"], 0) == TABLE_B and
            int(original["bytes"]) == TABLE_BYTES and
            int(basis["bytes"]) == TABLE_BYTES and
            TABLE_A + TABLE_BYTES == TABLE_B,
            "twiddle manifest A/B address or length contract changed")
    a_offset, b_offset = TABLE_A - raw_base, TABLE_B - raw_base
    require(0 <= a_offset < b_offset and b_offset + TABLE_BYTES <= len(raw),
            "A/B tables are outside the pinned PLC raw image")
    a_data = raw[a_offset:a_offset + TABLE_BYTES]
    b_data = raw[b_offset:b_offset + TABLE_BYTES]
    require(sha256(a_data) == original["sha256"] and
            sha256(b_data) == basis["sha256"],
            "original A/B table bytes differ from manifest hashes")
    matches = {item["name"]: item for item in table_match["arrayMatches"]}
    for entry, address in ((original, TABLE_A), (basis, TABLE_B)):
        match = matches.get(entry["cmsisName"])
        require(match is not None and match.get("byteExactMatch") is True and
                match.get("bytes") == TABLE_BYTES and
                match.get("sha256") == entry["sha256"] and
                int(match["address"], 0) == address,
                f"CMSIS table-match report differs for {entry['cmsisName']}")
    return {
        "original_a": {"address": TABLE_A, "raw_file_offset": a_offset,
                       "bytes": len(a_data), "sha256": sha256(a_data)},
        "basis_b": {"address": TABLE_B, "raw_file_offset": b_offset,
                    "bytes": len(b_data), "sha256": sha256(b_data)},
    }


def planner_contract(plan: dict[str, Any], raw: bytes, raw_sha: str,
                     raw_base: int) -> dict[str, Any]:
    require(plan.get("patchImplemented") is False and
            plan.get("firmwareModified") is False and
            plan.get("uf2Emitted") is False and
            plan.get("hardwareActions") is False,
            "current planner output is not an unapplied review plan")
    layout = plan["storageLayout"]["plc"]
    reservation = plan["reservations"]["plc"]
    state_start = int(reservation["stateStart"], 0)
    twiddle_offset = int(layout["twiddleOffset"])
    twiddle_ram = int(reservation["reconstructedTwiddleRam"], 0)
    bss_heap_end = int(reservation["newBssEndAndHeapStart"], 0)
    require(state_start == PLC_STATE_START and twiddle_offset == PLC_TWIDDLE_OFFSET and
            twiddle_ram == PLC_TWIDDLE_RAM and
            twiddle_ram == state_start + twiddle_offset and
            twiddle_ram + TABLE_BYTES <= bss_heap_end and
            state_start + int(layout["bytes"]) <= bss_heap_end,
            "planner twiddle destination is not contained in reserved PLC state")
    require(int(reservation["previousBssEndAndHeapStart"], 0) == 0x2000CBA8 and
            plan["inputHashes"]["plc"] == raw_sha,
            "planner previous boundary or PLC image fingerprint changed")
    plan_hashes = {key.replace("\\", "/"): value
                   for key, value in plan.get("sourceHashes", {}).items()}
    for path in ("tools/plan_firmware_hooks.py", "src/firmware/dp104_patch_state.h",
                 "src/firmware/dp104_twiddle.h"):
        require(plan_hashes.get(path) == sha256_file(ROOT / path),
                f"planner source hash is stale for {path}")

    recipes = {}
    for item in plan["reservationRecipesNotApplied"]:
        if item.get("role") == "plc":
            recipes[int(item["runtimeAddress"], 0)] = item
    desired = {BSS_END_WORD: bss_heap_end, HEAP_BASE_WORD: bss_heap_end,
               TABLE_POINTER: twiddle_ram}
    changes = {}
    for address, new_value in desired.items():
        item = recipes.get(address)
        require(item is not None, f"planner recipe missing at 0x{address:x}")
        offset = address - raw_base
        old, new = bytes.fromhex(item["originalHex"]), bytes.fromhex(item["replacementHex"])
        require(item["imageOffset"] == offset and len(old) == 4 and len(new) == 4 and
                new == struct.pack("<I", new_value) and raw[offset:offset + 4] == old,
                f"planner recipe differs from pinned raw bytes at 0x{address:x}")
        changes[address] = {"offset": offset, "old": old, "new": new,
                            "purpose": item["purpose"]}

    hooks = {int(item["runtimeAddress"], 0): item
             for item in plan["originalHookWindows"] if item.get("role") == "plc"}
    setter_window = hooks.get(SETTER_SITE)
    queue_window = hooks.get(QUEUECALL_SITE)
    require(setter_window is not None and setter_window["originalHex"] == "f0b50c4d" and
            raw[setter_window["imageOffset"]:setter_window["imageOffset"] + 4] ==
            bytes.fromhex(setter_window["originalHex"]),
            "planner setter window differs from the original push/LDR sequence")
    require(queue_window is not None and
            raw[queue_window["imageOffset"]:queue_window["imageOffset"] + 4] ==
            bytes.fromhex(queue_window["originalHex"]),
            "planner queue-call window differs from original raw bytes")
    return {
        "plan_sha256": sha256_file(PLAN_PATH),
        "state_start": state_start,
        "state_bytes": int(layout["bytes"]),
        "twiddle_offset": twiddle_offset,
        "twiddle_ram": twiddle_ram,
        "new_bss_heap_end": bss_heap_end,
        "recipes": changes,
        "setter_original_hex": setter_window["originalHex"],
        "queue_original_hex": queue_window["originalHex"],
        "recipes_applied": False,
    }


def generate_private_sources(output_dir: Path) -> dict[str, Path]:
    appcall, queuecall, setterjump = appcall_sources()
    paths = {
        "appentry": output_dir / "appentry_hook.S",
        "appcall": output_dir / "appcall_shim.S",
        "queuecall": output_dir / "queuecall_shim.S",
        "setterjump": output_dir / "setterjump_shim.S",
        "setter_legacy": output_dir / "setter_legacy.S",
        "linker": output_dir / "payload_layout.ld",
    }
    paths["appentry"].write_text(appentry_source(), encoding="ascii")
    paths["appcall"].write_text(appcall, encoding="ascii")
    paths["queuecall"].write_text(queuecall, encoding="ascii")
    paths["setterjump"].write_text(setterjump, encoding="ascii")
    paths["setter_legacy"].write_text(setter_legacy_source(), encoding="ascii")
    return paths


def validate_cmsis(provenance: dict[str, Any]) -> dict[str, Any]:
    require(provenance.get("repo") == "ARM-software/CMSIS_4" and
            provenance.get("commit") == CMSIS_COMMIT,
            "CMSIS 4 provenance is not the pinned official revision")
    source = next((item for item in provenance["sources"]
                   if item.get("path", "").replace("\\", "/").endswith(
                       ".local/cmsis4/arm_common_tables.c")), None)
    require(source is not None and source["sha256"] == sha256_file(CMSIS_SOURCE),
            "CMSIS source hash differs from its provenance record")
    return {
        "repo": provenance["repo"],
        "commit": provenance["commit"],
        "source_sha256": source["sha256"],
        "table_match_sha256": sha256_file(CMSIS_MATCH),
        "provenance_sha256": sha256_file(CMSIS_PROVENANCE),
    }


def section(elf: dict[str, Any], name: str) -> dict[str, Any]:
    matches = [item for item in elf["sections"] if item["name"] == name]
    require(len(matches) == 1, f"linked ELF lacks unique {name} section")
    return matches[0]


def verify_legacy_setter(code: bytes, address: int, original_prologue: bytes) -> dict[str, int]:
    require(len(code) == 16 and address % 4 == 0,
            "legacy setter trampoline must be 16 bytes and 4-byte aligned")
    require(code[:2] == original_prologue[:2] == bytes.fromhex("f0b5"),
            "legacy setter does not replay the original PUSH r4-r7,lr")
    ldr_r5 = struct.unpack_from("<H", code, 2)[0]
    require(ldr_r5 & 0xF800 == 0x4800 and (ldr_r5 >> 8) & 7 == 5,
            "legacy setter does not load r5 from a PC literal")
    buffer_literal = (((address + 2 + 4) & ~3) + ((ldr_r5 & 0xFF) << 2))
    require(buffer_literal == address + 8 and
            struct.unpack_from("<I", code, 8)[0] == KEY_OUTPUT,
            "legacy setter does not restore r5=0x20002348")
    ldr1, ldr2 = struct.unpack_from("<HH", code, 4)
    require(ldr1 == 0xF8DF and ldr2 & 0xF000 == 0xF000,
            "legacy setter does not tail-jump through an LDR.W PC literal")
    resume_literal = ((address + 8) & ~3) + (ldr2 & 0xFFF)
    require(resume_literal == address + 12 and
            struct.unpack_from("<I", code, 12)[0] == SETTER_RESUME_THUMB,
            "legacy setter resume literal differs from original continuation")
    return {"address": address, "bytes": len(code), "resume": SETTER_RESUME_THUMB,
            "key_output": KEY_OUTPUT}


def verify_setter_hook(code: bytes, address: int, predicate: int,
                       legacy: int) -> dict[str, int]:
    require(len(code) == 26, "setter hook instruction size changed")
    require(code[:4] == bytes.fromhex("2de90f50"),
            "setter hook does not preserve r0-r3,r12,lr")
    predicate_target = thumb_bl_target(code[4:8], address + 4)
    require(predicate_target == predicate,
            "setter hook BL does not target the ownership predicate")
    require(struct.unpack_from("<H", code, 8)[0] == 0x2800,
            "setter hook does not compare ownership result")
    conditional = struct.unpack_from("<H", code, 10)[0]
    require(conditional & 0xFF00 == 0xD000,
            "setter fallback branch is not BEQ")
    fallback = address + 10 + 4 + (((conditional & 0xFF) ^ 0x80) - 0x80) * 2
    require(fallback == address + 18,
            "setter fallback branch does not reach legacy restore path")
    require(code[12:16] == bytes.fromhex("bde80f50") and
            struct.unpack_from("<H", code, 16)[0] == 0x4770,
            "owned setter path does not restore registers and return")
    require(code[18:22] == bytes.fromhex("bde80f50"),
            "legacy setter path does not restore registers")
    legacy_target = thumb_wide_target(code[22:26], address + 22, link=False)
    require(legacy_target == legacy,
            "setter fallback B.W does not target the legacy trampoline")
    return {"address": address, "bytes": len(code), "predicate": predicate_target,
            "legacy_target": legacy_target}


def segment_bytes(elf_path: Path, segment: dict[str, Any]) -> bytes:
    content = elf_path.read_bytes()
    start = segment["offset"]
    end = start + segment["file_size"]
    require(0 <= start <= end <= len(content), "PT_LOAD slice exceeds ELF file")
    return content[start:end]


def output_section_bytes(elf: dict[str, Any], elf_path: Path,
                         payload_address: int, name: str) -> tuple[dict[str, Any], bytes]:
    item = section(elf, name)
    payload_segment = next(segment for segment in elf["program_headers"]
                           if segment["virtual_address"] == payload_address)
    require(payload_address <= item["address"] and
            item["address"] + item["size"] <=
            payload_address + payload_segment["memory_size"],
            f"{name} section is outside the payload PT_LOAD")
    data = elf_path.read_bytes()[item["offset"]:item["offset"] + item["size"]]
    require(len(data) == item["size"], f"cannot extract linked {name} bytes")
    return item, data


def build_linked_payload(zig: Path, env: dict[str, str], raw: bytes,
                         target: dict[str, Any], table_manifest: dict[str, Any],
                         plan: dict[str, Any], planner: dict[str, Any],
                         output_dir: Path) -> dict[str, Any]:
    raw_base = int(target["base"], 0)
    table_info = validate_tables(
        table_manifest, load_json(CMSIS_MATCH), raw, raw_base
    )
    appcall_offset = APPCALL_SITE - raw_base
    queuecall_offset = QUEUECALL_SITE - raw_base
    setter_offset = SETTER_SITE - raw_base
    uart_rx_offset = UART_RX_CALLBACK_WORD - raw_base
    original_appcall = raw[appcall_offset:appcall_offset + 4]
    original_queuecall = raw[queuecall_offset:queuecall_offset + 4]
    require(thumb_bl_target(original_appcall, APPCALL_SITE) == APP_ENTRY,
            "original appcall is not BL to the pinned main entry")
    require(thumb_bl_target(original_queuecall, QUEUECALL_SITE) == QUEUE_POP,
            "original queue call is not BL to the pinned queue-pop function")
    original_uart_rx_pointer = raw[uart_rx_offset:uart_rx_offset + 4]
    require(original_uart_rx_pointer == struct.pack("<I", UART_RX_CALLBACK_OLD),
            "original UART receive callback literal differs from the pinned vendor target")
    original_setter = raw[setter_offset:setter_offset + 4]
    require(original_setter == bytes.fromhex(planner["setter_original_hex"]),
            "original setter prologue differs from planner evidence")

    output_dir.mkdir(parents=True, exist_ok=True)
    private = generate_private_sources(output_dir)
    private["linker"].write_text(payload_linker_script(plan), encoding="ascii")
    payload_elf = output_dir / "plc_payload.elf"
    profile = PROFILES["plc"]
    sources = [
        ROOT / "src/firmware/dp104_receiver.c",
        ROOT / "src/firmware/dp104_wire.c",
        ROOT / "src/firmware/dp104_wire_stream.c",
        ROOT / "src/firmware/dp104_twiddle.c",
        ROOT / "src/firmware/dp104_plc_target.c",
        ROOT / "src/firmware/freestanding_memory.c",
        ROOT / "src/firmware/thumb_setter_hook.S",
        private["appentry"], private["appcall"], private["queuecall"],
        private["setterjump"], private["setter_legacy"],
    ]
    flags = [
        "-target", profile["freestanding_triple"], "-std=c99", "-Os", "-g0",
        "-ffreestanding", "-fno-builtin", "-fno-stack-protector", "-fno-common",
        "-fno-pic", "-fno-pie", "-fno-unwind-tables",
        "-fno-asynchronous-unwind-tables", "-ffunction-sections", "-nostdlib",
        "-static", "-no-pie", "-Wall", "-Wextra", "-Werror",
        "-Isrc/firmware/freestanding", "-Isrc/firmware", "-Iinclude",
        *profile["architecture_flags"], *[str(path) for path in sources],
        "-Wl,-T," + str(private["linker"]), "-Wl,--build-id=none",
        "-Wl,--no-undefined", "-Wl,-z,max-page-size=0x4", "-o", str(payload_elf),
    ]
    compile_result = run([str(zig), "cc", *flags], env)
    elf = inspect_elf(payload_elf)
    check_machine_abi(elf, profile, "full PLC payload")
    require(not elf["has_interpreter"] and not elf["undefined_symbols"],
            "PLC payload has an interpreter or unresolved symbols")
    sections = {item["name"]: item for item in elf["sections"]}
    for name in (".data", ".sdata", ".bss", ".sbss", ".got"):
        require(sections.get(name, {"size": 0})["size"] == 0,
                f"PLC payload unexpectedly contains {name}")

    loads = elf["program_headers"]
    require(len(loads) == 4 and all(segment["flags"] == (PF_R | PF_X) and
                                     not segment["flags"] & PF_W for segment in loads),
            "PLC payload must have four read-only executable PT_LOAD segments")
    segments = {segment["virtual_address"]: segment for segment in loads}
    expected_sites = (APPCALL_SITE, QUEUECALL_SITE, SETTER_SITE, TABLE_A)
    require(set(segments) == set(expected_sites),
            "PLC payload PT_LOAD addresses differ from the approved code windows")
    for address in (APPCALL_SITE, QUEUECALL_SITE, SETTER_SITE):
        require(segments[address]["file_size"] == 4 and
                segments[address]["memory_size"] == 4,
                f"branch shim at 0x{address:x} is not exactly four bytes")
    payload_load = segments[TABLE_A]
    payload_end = TABLE_A + payload_load["memory_size"]
    require(payload_load["file_size"] == payload_load["memory_size"] and
            payload_load["file_size"] > 28 and payload_end <= TABLE_B,
            "linked PLC payload does not fit fully inside the original A-table span")
    require(elf["file_size"] < 65536,
            "linked PLC payload ELF contains unexpected address-gap padding")

    apphook_section, apphook = output_section_bytes(elf, payload_elf, TABLE_A, ".apphook")
    target_init, _target_init_bytes = output_section_bytes(
        elf, payload_elf, TABLE_A, ".target_init")
    queue_hook, _queue_hook_bytes = output_section_bytes(
        elf, payload_elf, TABLE_A, ".queue_hook")
    uart_rx_hook, _uart_rx_bytes = output_section_bytes(
        elf, payload_elf, TABLE_A, ".uart_rx_hook")
    setter_hook, setter_hook_bytes = output_section_bytes(
        elf, payload_elf, TABLE_A, ".setter_hook")
    predicate, _predicate_bytes = output_section_bytes(
        elf, payload_elf, TABLE_A, ".setter_predicate")
    legacy_section, legacy_bytes = output_section_bytes(
        elf, payload_elf, TABLE_A, ".setter_legacy")
    require(apphook_section["address"] == TABLE_A and apphook_section["size"] == 28 and
            target_init["size"] > 0 and queue_hook["size"] > 0 and
            uart_rx_hook["size"] > 0 and setter_hook["size"] > 0 and
            predicate["size"] > 0,
            "one or more required PLC entry functions are absent")
    apphook_check = verify_appentry_hook(apphook, TABLE_A, target_init["address"])
    setter_check = verify_setter_hook(
        setter_hook_bytes, setter_hook["address"], predicate["address"],
        legacy_section["address"],
    )
    legacy_check = verify_legacy_setter(legacy_bytes, legacy_section["address"],
                                        original_setter)
    uart_rx_pointer = uart_rx_hook["address"] | 1
    require(TABLE_A <= (uart_rx_pointer & ~1) < payload_end,
            "UART receive callback function lies outside A-span payload")

    elf_bytes = payload_elf.read_bytes()
    appcall_bytes = segment_bytes(payload_elf, segments[APPCALL_SITE])
    queuecall_bytes = segment_bytes(payload_elf, segments[QUEUECALL_SITE])
    setterjump_bytes = segment_bytes(payload_elf, segments[SETTER_SITE])
    payload_bytes = segment_bytes(payload_elf, payload_load)
    require(thumb_bl_target(appcall_bytes, APPCALL_SITE) == TABLE_A,
            "appcall BL does not target the new appentry hook")
    require(thumb_bl_target(queuecall_bytes, QUEUECALL_SITE) == queue_hook["address"],
            "queuecall BL does not target dp104_plc_queue_hook")
    require(thumb_wide_target(setterjump_bytes, SETTER_SITE, link=False) ==
            setter_hook["address"],
            "setter-site B.W does not target dp104_plc_setter_hook")

    appcall_bin = output_dir / "appcall.bin"
    queuecall_bin = output_dir / "queuecall.bin"
    setterjump_bin = output_dir / "setterjump.bin"
    payload_bin = output_dir / "payload.bin"
    for path, content in ((appcall_bin, appcall_bytes), (queuecall_bin, queuecall_bytes),
                          (setterjump_bin, setterjump_bytes), (payload_bin, payload_bytes)):
        path.write_bytes(content)

    return {
        "original_calls": {
            "appcall_target": APP_ENTRY,
            "queuecall_target": QUEUE_POP,
            "setter_prologue_hex": original_setter.hex(),
            "uart_rx_callback_value": UART_RX_CALLBACK_OLD,
        },
        "linked_addresses": {
            "appcall": APPCALL_SITE,
            "queuecall": QUEUECALL_SITE,
            "setterjump": SETTER_SITE,
            "payload_start": TABLE_A,
            "payload_end_exclusive": payload_end,
            "a_table_end_exclusive": TABLE_B,
            "payload_bytes": payload_load["memory_size"],
            "remaining_a_bytes": TABLE_B - payload_end,
            "target_init": target_init["address"],
            "queue_hook": queue_hook["address"],
            "uart_rx_hook": uart_rx_hook["address"],
            "uart_rx_callback_pointer": uart_rx_pointer,
            "setter_hook": setter_hook["address"],
            "setter_predicate": predicate["address"],
            "setter_legacy": legacy_section["address"],
        },
        "static_hook_checks": {
            "appentry": apphook_check,
            "setter_hook": setter_check,
            "setter_legacy": legacy_check,
            "original_uart_rx_callback": UART_RX_CALLBACK_OLD,
            "uart_rx_callback_pointer": uart_rx_pointer,
            "appcall_target": thumb_bl_target(appcall_bytes, APPCALL_SITE),
            "queuecall_target": thumb_bl_target(queuecall_bytes, QUEUECALL_SITE),
            "setterjump_target": thumb_wide_target(setterjump_bytes, SETTER_SITE, False),
            "uart_rx_callback_target": uart_rx_pointer & 0xFFFFFFFE,
            "uart_rx_callback_thumb_bit_set": bool(uart_rx_pointer & 1),
        },
        "payload_elf": {
            "path": str(payload_elf.relative_to(ROOT)),
            "sha256": sha256_file(payload_elf),
            "bytes": elf["file_size"],
            "machine": elf["machine"],
            "flags": elf["flags"],
            "load_segments": loads,
            "all_loaded_bytes_read_only": True,
            "data_bss_got_bytes": 0,
            "no_undefined_enforced_by_linker": True,
            "compile_command": [str(zig), "cc", *flags],
            "compile_stdout": compile_result.stdout.strip(),
        },
        "patch_blobs": {
            "appcall": {"bytes": appcall_bytes, "path": appcall_bin},
            "queuecall": {"bytes": queuecall_bytes, "path": queuecall_bin},
            "setterjump": {"bytes": setterjump_bytes, "path": setterjump_bin},
            "payload": {"bytes": payload_bytes, "path": payload_bin},
            "uart_rx_callback": {
                "address": UART_RX_CALLBACK_WORD,
                "bytes": struct.pack("<I", uart_rx_pointer),
                "old": original_uart_rx_pointer,
            },
        },
        "legacy_trampoline_source": str(private["setter_legacy"].relative_to(ROOT)),
        "generated_sources": {key: str(path.relative_to(ROOT))
                               for key, path in private.items()},
    }


def build_raw_replacements(raw: bytes, raw_base: int,
                          planner: dict[str, Any],
                          linked: dict[str, Any]) -> tuple[bytes, list[dict[str, Any]]]:
    blobs = linked["patch_blobs"]
    replacements = []
    for address, item in planner["recipes"].items():
        replacements.append({
            "address": address,
            "offset": item["offset"],
            "old": item["old"],
            "new": item["new"],
            "artifact": "planner recipe (review only)",
            "purpose": item["purpose"],
        })
    for address, name, purpose in (
        (APPCALL_SITE, "appcall", "Redirect original startup BL through target initialization"),
        (QUEUECALL_SITE, "queuecall", "Route the task queue pop through the PLC receiver"),
        (SETTER_SITE, "setterjump", "Branch to the guarded setter without changing LR"),
        (TABLE_A, "payload", "Replace the original A table prefix with linked read-only code"),
    ):
        new = blobs[name]["bytes"]
        offset = address - raw_base
        old = raw[offset:offset + len(new)]
        require(len(old) == len(new), f"replacement span outside raw image at 0x{address:x}")
        replacements.append({
            "address": address,
            "offset": offset,
            "old": old,
            "new": new,
            "artifact": str(blobs[name]["path"].relative_to(ROOT)),
            "purpose": purpose,
        })
    uart_rx = blobs["uart_rx_callback"]
    callback_offset = UART_RX_CALLBACK_WORD - raw_base
    require(raw[callback_offset:callback_offset + 4] == uart_rx["old"] and
            uart_rx["old"] == struct.pack("<I", UART_RX_CALLBACK_OLD) and
            len(uart_rx["bytes"]) == 4 and uart_rx["bytes"][0] & 1 == 1,
            "UART callback pointer recipe differs from original or is not Thumb")
    replacements.append({
        "address": UART_RX_CALLBACK_WORD,
        "offset": callback_offset,
        "old": uart_rx["old"],
        "new": uart_rx["bytes"],
        "artifact": "linker-resolved dp104_plc_uart_rx Thumb pointer",
        "purpose": "Register stream-framing UART receive callback",
    })

    release = load_release()
    version = int(release["packedFirmwareVersion"], 0)
    legacy = release["plcVersion"]["legacyReply"]
    address = int(legacy["runtimeAddress"], 0)
    offset = address - raw_base
    window_offset = int(legacy["windowStart"], 0) - raw_base
    window = raw[window_offset:window_offset + legacy["windowLength"]]
    require(address == 0x08014436 and legacy["length"] == 2 and
            sha256(window) == legacy["windowSha256"] and
            raw[offset:offset + 2] == bytes.fromhex(legacy["originalHex"]) == b"\x20\x22",
            "legacy PLC version reply differs from the pinned MOVS r2 window")
    require(version >> 8 == 1,
            "legacy PLC version recipe only supports the preserved high byte 0x01")
    replacements.append({
        "address": address,
        "offset": offset,
        "old": raw[offset:offset + 2],
        "new": struct.pack("<H", 0x2200 | (version & 0xff)),
        "artifact": "firmware/compatibility.json legacyReply",
        "purpose": "Report the real compatibility version through the original PLC D1 reply",
    })

    replacements.sort(key=lambda item: item["offset"])
    covered: set[int] = set()
    patched = bytearray(raw)
    for item in replacements:
        offset = item["offset"]
        old, new = item["old"], item["new"]
        require(offset >= 0 and offset + len(old) <= len(raw) and len(old) == len(new),
                f"invalid replacement range at 0x{item['address']:x}")
        require(raw[offset:offset + len(old)] == old,
                f"source bytes changed at 0x{item['address']:x}")
        positions = set(range(offset, offset + len(new)))
        require(not covered.intersection(positions), "replacement ranges overlap")
        covered.update(positions)
        patched[offset:offset + len(new)] = new

    patched_raw = bytes(patched)
    changed = {offset for offset, (before, after) in
               enumerate(zip(raw, patched_raw)) if before != after}
    require(changed <= covered, "raw image differs outside approved replacement spans")
    basis_offset = TABLE_B - raw_base
    require(patched_raw[basis_offset:basis_offset + TABLE_BYTES] ==
            raw[basis_offset:basis_offset + TABLE_BYTES],
            "original B basis table was modified")

    data_report_path = ROOT / ".local" / "plc-data-pack-feasibility.json"
    data_report = load_json(data_report_path)
    lma_range = data_report["InitializedData"]["flashLmaRange"]
    left, right = lma_range.strip("[]()").split(",")
    data_start, data_end = int(left, 16) - raw_base, int(right, 16) - raw_base
    require(data_start >= 0 and data_end <= len(raw) and
            patched_raw[data_start:data_end] == raw[data_start:data_end],
            "original initialized-data image bytes were modified")

    summaries = [{
        "address": item["address"],
        "raw_file_offset": item["offset"],
        "length": len(item["new"]),
        "original_sha256": sha256(item["old"]),
        "replacement_sha256": sha256(item["new"]),
        "artifact": item["artifact"],
        "purpose": item["purpose"],
    } for item in replacements]
    return patched_raw, summaries


def rebuild_uf2(original_uf2: bytes, raw: bytes, patched_raw: bytes,
                target: dict[str, Any]) -> tuple[bytes, dict[str, Any]]:
    require(len(original_uf2) % 512 == 0 and len(raw) == len(patched_raw),
            "source UF2/raw image geometry changed")
    block_files: dict[int, int] = {}
    for file_offset in range(0, len(original_uf2), 512):
        number = struct.unpack_from("<I", original_uf2, file_offset + 20)[0]
        require(number not in block_files, "duplicate UF2 block in original archive")
        block_files[number] = file_offset
    require(len(block_files) == len(raw) // 256,
            "UF2 block count differs from raw image size")

    result = bytearray(original_uf2)
    allowed_file_offsets: set[int] = set()
    for raw_offset, (before, after) in enumerate(zip(raw, patched_raw)):
        if before == after:
            continue
        block_number, payload_offset = divmod(raw_offset, 256)
        require(block_number in block_files, "changed raw byte has no original UF2 block")
        file_offset = block_files[block_number] + 32 + payload_offset
        result[file_offset] = after
        allowed_file_offsets.add(file_offset)

    patched_uf2 = bytes(result)
    changed_uf2 = {offset for offset, (before, after) in
                   enumerate(zip(original_uf2, patched_uf2)) if before != after}
    require(changed_uf2 <= allowed_file_offsets,
            "UF2 headers, addresses, padding or end markers changed")
    patched_spec = dict(target)
    patched_spec["rawSha256"] = sha256(patched_raw)
    parsed = parse_uf2(patched_uf2, patched_spec)
    require(parsed == patched_raw, "rebuilt UF2 did not parse to the patched raw image")
    return patched_uf2, {
        "bytes": len(patched_uf2),
        "sha256": sha256(patched_uf2),
        "structurally_valid": True,
        "changed_uf2_payload_bytes": len(changed_uf2),
        "header_padding_magic_preserved": True,
        "block_count": len(block_files),
    }


def planner_report(plan: dict[str, Any], contract: dict[str, Any]) -> dict[str, Any]:
    return {
        "sha256": contract["plan_sha256"],
        "state_start": contract["state_start"],
        "state_bytes_from_fresh_plan": contract["state_bytes"],
        "twiddle_offset": contract["twiddle_offset"],
        "twiddle_ram": contract["twiddle_ram"],
        "new_bss_heap_end": contract["new_bss_heap_end"],
        "recipes_applied_only_to_local_review_image": True,
        "recipes": {
            f"0x{address:08x}": {
                "raw_file_offset": item["offset"],
                "original_hex": item["old"].hex(),
                "replacement_hex": item["new"].hex(),
                "purpose": item["purpose"],
            }
            for address, item in contract["recipes"].items()
        },
        "reservation_recipes_applied_to_source_or_hardware": False,
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
        "purpose": "review-only linked full PLC payload within original A-table storage",
        "boundary": {
            "full_image_artifacts_local_only": True,
            "source_firmware_written": False,
            "hardware_accessed": False,
            "flash_or_bootloader_commands": False,
            "native_rgb_or_runtime_acceptance_verified": False,
            "calibration_ready_default": False,
            "automatic_color_order_or_mapping_enabled": False,
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
        table_manifest = load_json(TWIDDLE_MANIFEST)
        plan = load_json(PLAN_PATH)
        cmsis = validate_cmsis(load_json(CMSIS_PROVENANCE))
        target_manifest, raw, original_uf2, input_info = get_raw_inputs(archive_path)
        raw_base = input_info["raw_base"]
        planner = planner_contract(plan, raw, input_info["sha256"], raw_base)
        table_info = validate_tables(table_manifest, load_json(CMSIS_MATCH), raw, raw_base)

        env = os.environ.copy()
        env["ZIG_GLOBAL_CACHE_DIR"] = str(ZIG_CACHE_ROOT / "global")
        env["ZIG_LOCAL_CACHE_DIR"] = str(ZIG_CACHE_ROOT / "local")
        version = run([str(zig), "version"], env).stdout.strip()
        require(version == "0.15.2", f"expected Zig 0.15.2, found {version!r}")

        OUTPUT_ROOT.mkdir(parents=True, exist_ok=True)
        role_dir = OUTPUT_ROOT / "plc"
        role_dir.mkdir(parents=True, exist_ok=True)
        hashes_before = snapshot_sources()
        report["source_hashes"] = {"before_build": hashes_before}
        report["pinned_inputs"] = {
            **input_info,
            "archive_validation": input_info["archive_validation"],
            "target_manifest": str(TARGET.relative_to(ROOT)),
            "target_manifest_sha256": input_info["target_manifest_sha256"],
            "twiddle_manifest": str(TWIDDLE_MANIFEST.relative_to(ROOT)),
            "twiddle_manifest_sha256": input_info["twiddle_manifest_sha256"],
            "cmsis4": cmsis,
            "table_hashes": table_info,
        }
        report["planner_contract"] = planner_report(plan, planner)
        report["zig_version"] = version
        report["compatibility_version"] = load_release()["compatibilityVersion"]
        report["cache_paths"] = {
            "global": str((ZIG_CACHE_ROOT / "global").relative_to(ROOT)),
            "local": str((ZIG_CACHE_ROOT / "local").relative_to(ROOT)),
            "reused_existing": True,
        }

        linked = build_linked_payload(zig, env, raw, target_manifest["plc"],
                                      table_manifest, plan, planner, role_dir)
        patched_raw, replacements = build_raw_replacements(raw, raw_base, planner, linked)
        patched_uf2, uf2_info = rebuild_uf2(original_uf2, raw, patched_raw,
                                            target_manifest["plc"])
        hashes_after = snapshot_sources()
        report["source_hashes"].update({
            "after_build": hashes_after,
            "unchanged": hashes_before == hashes_after,
        })
        require(hashes_before == hashes_after,
                "source/header/manifest/planner fingerprints changed during build")

        raw_path = OUTPUT_ROOT / "patched-plc.bin"
        uf2_path = OUTPUT_ROOT / "patched-plc.uf2"
        raw_path.write_bytes(patched_raw)
        uf2_path.write_bytes(patched_uf2)
        require(sha256_file(raw_path) == sha256(patched_raw) and
                sha256_file(uf2_path) == uf2_info["sha256"],
                "local review image artifacts changed while being written")
        patched_manifest = dict(target_manifest["plc"])
        patched_manifest["rawSha256"] = sha256(patched_raw)
        require(parse_uf2(uf2_path.read_bytes(), patched_manifest) == patched_raw,
                "written patched UF2 failed structural/raw roundtrip validation")

        report["linked_payload"] = {
            key: value for key, value in linked.items() if key != "patch_blobs"
        }
        report["replacement_recipe"] = replacements
        report["local_review_images"] = {
            "raw_image": {
                "path": str(raw_path.relative_to(ROOT)),
                "bytes": len(patched_raw),
                "sha256": sha256(patched_raw),
                "not_installed": True,
            },
            "uf2": {
                "path": str(uf2_path.relative_to(ROOT)),
                **uf2_info,
                "not_installed": True,
            },
            "structural_parse_roundtrip": True,
            "changed_bytes_only_in_approved_windows": True,
            "original_basis_B_unchanged": True,
            "original_initialized_data_unchanged": True,
        }
        report["limits"] = [
            "No target hardware, stock firmware, or MMIO was executed.",
            "UF2 structure and byte whitelist are verified; bootloader acceptance and recovery are not.",
            "Host tests are reported separately and do not prove target runtime behavior.",
            "Indirect aliases, target task timing, physical LED mapping, and calibration remain unverified.",
            "Calibration/color order remains unverified and disabled by default.",
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
    print(f"PLC payload: {linked['linked_addresses']['payload_bytes']} B in A span; "
          f"patched raw/UF2 {len(patched_raw)}/{len(patched_uf2)} B PASS")
    print(f"Review-only PLC report: {report_path}")
    print("No image was installed and no hardware or bootloader was touched.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
