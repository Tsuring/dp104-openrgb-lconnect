#!/usr/bin/env python3
"""Build and validate a review-only local payload for the pinned 8K master.

Only the verified second RAM-copy source span is repacked. The first boot copy
and initialized-data copy remain in place. Output candidates are written under
build/firmware/master-payload/ and are never sent to a device.
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
OUTPUT_ROOT = ROOT / "build" / "firmware" / "master-payload"
ZIG_CACHE_ROOT = ROOT / "build" / "firmware" / "zig-cache"
DEFAULT_ARCHIVE = ROOT / ".local" / "firmware" / "DP104_v1.2.0.zip"
DEFAULT_ZIG = ROOT / ".local" / "toolchain" / "zig-x86_64-windows-0.15.2" / "zig.exe"

EXPECTED_RAW_SHA256 = "dea1d6b9f1b8dbfa323fb9a74ae3cee93cf0c3ae82b35435bdacfa4748a1ca4d"
EXPECTED_SUFFIX_SHA256 = "082b8d9ce24c977c2df28e2270fb44db9d89de8d8313ddc8eb469a6dc8be7c7b"

# The first 528-byte reset copy is preserved. Only its later 44-byte copy
# loop is redirected to restore the following owned span.
FIRST_COPY_FLASH_START = 0x8004
FIRST_COPY_FLASH_END = 0x8214
FIRST_COPY_RAM_START = 0x20000000
FIRST_COPY_RAM_END = 0x20000210
FIRST_COPY_BOOT = 0x14E04
FIRST_COPY_JUMP_TARGET = 0x2000015E
COPY_SHIM_SITE = 0x20000162
COPY_SHIM_BYTES = 44
COPY_SHIM_CONTINUATION = 0x2000018E
SUFFIX_FLASH_START = 0x8214
SUFFIX_FLASH_END = 0x14E04
SUFFIX_RAM_START = 0x20000210
SUFFIX_RAM_END = 0x2000CE00
SUFFIX_BYTES = SUFFIX_FLASH_END - SUFFIX_FLASH_START

DATA_COPY_SITE = 0x2000018E
DATA_COPY_BYTES = 40
POST_BSS_SITE = 0x200001D0
POST_BSS_BYTES = 8
POST_BSS_CONTINUATION = 0x200001D8
RAW_HID_SITE = 0x200046EE
RAW_HID_BYTES = 8
RAW_HID_CONTINUATION = 0x200046F6
VERSION_SITE = 0x20005034
VERSION_BYTES = 12
VERSION_CONTINUATION = 0x20005040
sys.path.insert(0, str(ROOT / "tools"))
from release_version import load_release
COMPATIBILITY = load_release()
RELEASE_VERSION = COMPATIBILITY["compatibilityVersion"]
PACKED_VERSION = int(COMPATIBILITY["packedFirmwareVersion"], 0)
VERSION_HALFWORD = ((PACKED_VERSION & 255) << 8) | (PACKED_VERSION >> 8)
TASK_CALL_SITE = 0x0001BE00
TASK_CALL_BYTES = 8
TASK_LEGACY_ENTRY = 0x20001A56
UART_CALLBACK_PAIR = 0x0001B40E
UART_CALLBACK_BYTES = 8
UART_CALLBACK_OLD = 0x0001D722
UART_LINE_SITE = 0x20007166
UART_LINE_BYTES = 8
UART_LINE_ORIGINAL = bytes.fromhex("8347570073002030")
UART_RDA_SITE = 0x20007062
UART_RDA_BYTES = 8
UART_RDA_ORIGINAL = bytes.fromhex("83d6a1d783458700")
UART_FCR_SITE = 0x000237EE
UART_FCR_BYTES = 4
UART_FCR_ORIGINAL = bytes.fromhex("93e7070c")
UART_FCR_REPLACEMENT = bytes.fromhex("93e70704")  # ORI a5,a5,0x40: trigger2
UART_FCR_INIT_SITE = 0x000237CE
UART_FCR_INIT_BYTES = 4
UART_FCR_INIT_ORIGINAL = bytes.fromhex("930770f8")  # Original literal 0x87
UART_FCR_INIT_REPLACEMENT = bytes.fromhex("93077004")  # Literal 0x47: same low bits, trigger2

DATA_LMA_START = 0x45880
DATA_LMA_END = 0x465D0
INITIAL_HEAP_WORD = 0x000464B8
BSS_END_PAIR = 0x200001BA
HEAP_FLOOR_PAIR = 0x00021A4C
MEMCPY_FLASH = 0x00014E4A
MEMMOVE_FLASH = 0x00014E60
MEMSET_FLASH = 0x00014E86
STATE_START = 0x20012A10
INITIAL_SP = 0x20020000
SP_GUARD_BYTES = 0x40

CPU_VMA_START = 0x00100000
CPU_VMA_END = 0x00300000

FROZEN_INPUTS = {
    "src/firmware/dp104_master_target.c": "c974c16de18e6085f3d3a88818e0f199c6e00a39c2a4f3a30d5b9875ccb3fd0a",
    "src/firmware/dp104_master_target.h": "31caa0028b631362ffae036a37e6591fa32ea0a55f84c92073fe6e63e2317f4b",
    "src/firmware/rv32_task_hook.S": "beb2218bf842d1f7ff0da92f8c5662d37bde9e175e74539b591558694871759e",
    "src/firmware/dp104_patch_state.h": "3b9beff1924423fb76bddfd254372c7911b8fda24052f54b95f3f9c6a506337f",
}

SOURCE_PATHS = (
    "tools/build_master_payload.py",
    "tools/build_data_restore.py",
    "tools/build_plc_payload.py",
    "tools/plan_firmware_hooks.py",
    "tools/lz_codec.py",
    "tools/verify_target.py",
    "tools/release_version.py",
    "CMakeLists.txt",
    "src/firmware/dp104_master_target.c",
    "src/firmware/dp104_master_target.h",
    "src/firmware/dp104_master_bridge.c",
    "src/firmware/dp104_master_bridge.h",
    "src/firmware/dp104_patch_state.h",
    "src/firmware/dp104_plc_diagnostic.h",
    "src/firmware/dp104_dma_rx.h",
    "src/firmware/dp104_wire.c",
    "src/firmware/dp104_wire.h",
    "src/firmware/dp104_wire_stream.c",
    "src/firmware/dp104_wire_stream.h",
    "src/firmware/dp104_usb_stage.c",
    "src/firmware/dp104_usb_stage.h",
    "src/firmware/dp104_lz.c",
    "src/firmware/dp104_lz.h",
    "src/firmware/rv32_host_hook.S",
    "src/firmware/rv32_task_hook.S",
    "firmware/targets/dp104-v1.2.0-8k.json",
    "firmware/targets/dp104-master-ramcopy.json",
    "firmware/targets/dp104-master-uart.json",
    "firmware/compatibility.json",
    "include/dp104_protocol.h",
    "include/dp104_version.h",
    ".local/master-data-pack-feasibility.json",
    ".local/master-ram-pack-feasibility.json",
    ".local/master-relay-analysis.json",
    ".local/master-upper-ram-audit.json",
)


class MasterPayloadError(RuntimeError):
    """Raised when the bounded review payload cannot be verified."""


sys.path.insert(0, str(ROOT / "tools"))
from build_data_restore import (  # noqa: E402
    PROFILES,
    RestoreBuildError,
    check_machine_abi,
    inspect_elf,
    require,
    run,
    sha256,
    sha256_file,
)
from build_plc_payload import rebuild_uf2  # noqa: E402
from lz_codec import pack_lz, unpack_lz  # noqa: E402
from plan_firmware_hooks import (  # noqa: E402
    align_up,
    image_offset,
    make_plan,
    measure_layout,
    rv_address_pair,
    rv_call_target,
    rv_pair_target,
)
from verify_target import TARGET, parse_uf2, validate_archive  # noqa: E402


def load_json(path: Path) -> dict[str, Any]:
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise MasterPayloadError(f"cannot read {path}: {error}") from error


def source_snapshot() -> dict[str, str]:
    snapshot: dict[str, str] = {}
    for relative in SOURCE_PATHS:
        path = ROOT / relative
        require(path.is_file(), f"required payload input is missing: {relative}")
        snapshot[relative] = sha256_file(path)
    return snapshot


def verify_frozen_inputs(snapshot: dict[str, str]) -> None:
    for relative, expected in FROZEN_INPUTS.items():
        require(snapshot.get(relative) == expected,
                f"frozen master input changed: {relative}")


def prepare_env(zig: Path) -> tuple[Path, dict[str, str]]:
    zig = zig.resolve()
    require(zig.is_file(), f"Zig compiler not found: {zig}")
    require((ZIG_CACHE_ROOT / "global").is_dir() and
            (ZIG_CACHE_ROOT / "local").is_dir(),
            "the existing project Zig caches are required")
    env = os.environ.copy()
    env["ZIG_GLOBAL_CACHE_DIR"] = str(ZIG_CACHE_ROOT / "global")
    env["ZIG_LOCAL_CACHE_DIR"] = str(ZIG_CACHE_ROOT / "local")
    version = run([str(zig), "version"], env).stdout.strip()
    require(version == "0.15.2", f"expected Zig 0.15.2, found {version!r}")
    return zig, env


def load_original_inputs(archive_path: Path) -> tuple[dict[str, Any], bytes, bytes, bytes]:
    validate_archive(archive_path)
    target_manifest = load_json(TARGET)
    require(target_manifest.get("archiveSha256") == sha256_file(archive_path),
            "archive SHA differs from the pinned target manifest")
    master_target = target_manifest["master"]
    require(master_target.get("rawSha256") == EXPECTED_RAW_SHA256,
            "target manifest is not the pinned 8K master raw image")
    try:
        with zipfile.ZipFile(archive_path) as archive:
            member = master_target["archiveEntry"]
            require(archive.namelist().count(member) == 1,
                    "archive has a missing or duplicate master UF2")
            original_uf2 = archive.read(member)
            plc_member = target_manifest["plc"]["archiveEntry"]
            require(archive.namelist().count(plc_member) == 1,
                    "archive has a missing or duplicate PLC UF2 for layout planning")
            plc_raw = parse_uf2(archive.read(plc_member), target_manifest["plc"])
    except (OSError, KeyError, zipfile.BadZipFile) as error:
        raise MasterPayloadError(f"cannot read original firmware UF2: {error}") from error
    require(sha256(original_uf2) == master_target["uf2Sha256"],
            "original master UF2 SHA differs from the pinned target")
    raw = parse_uf2(original_uf2, master_target)
    require(sha256(raw) == EXPECTED_RAW_SHA256,
            "extracted master raw image SHA differs from the pinned target")
    return target_manifest, raw, original_uf2, plc_raw


def verify_ramcopy_evidence(raw: bytes, ramcopy: dict[str, Any],
                            pack_report: dict[str, Any], raw_base: int) -> dict[str, Any]:
    require(ramcopy.get("rawSha256") == EXPECTED_RAW_SHA256 and
            pack_report.get("targetRawSha256") == EXPECTED_RAW_SHA256,
            "RAM-copy evidence belongs to a different master image")
    first = ramcopy["firstBootCopy"]
    second = ramcopy["secondRamCopy"]
    require((first["flashStart"], first["flashEndExclusive"],
             first["ramStart"], first["ramEndExclusive"], first["bytes"]) ==
            (FIRST_COPY_FLASH_START, FIRST_COPY_FLASH_END,
             FIRST_COPY_RAM_START, FIRST_COPY_RAM_END, 528),
            "manifest first ROM copy range differs from the approved boot path")
    require((second["flashStart"], second["flashEndExclusive"],
             second["ramStart"], second["ramEndExclusive"], second["bytes"]) ==
            (SUFFIX_FLASH_START, SUFFIX_FLASH_END,
             SUFFIX_RAM_START, SUFFIX_RAM_END, SUFFIX_BYTES),
            "manifest second-copy span differs from the approved owned range")
    require(first["bootFunction"] == FIRST_COPY_BOOT and
            first["jumpTarget"] == FIRST_COPY_JUMP_TARGET,
            "first-copy boot transfer contract changed")
    require(pack_report["secondCopy"]["sha256"] == EXPECTED_SUFFIX_SHA256 and
            second["sha256"] == EXPECTED_SUFFIX_SHA256,
            "second-copy source SHA differs from frozen static evidence")

    raw_offset = SUFFIX_FLASH_START - raw_base
    end_offset = SUFFIX_FLASH_END - raw_base
    require(0 <= raw_offset < end_offset <= len(raw),
            "second-copy flash span is outside pinned raw image")
    suffix = raw[raw_offset:end_offset]
    require(len(suffix) == SUFFIX_BYTES and sha256(suffix) == EXPECTED_SUFFIX_SHA256,
            "pinned raw second-copy span hash differs from manifest")
    first_offset = FIRST_COPY_FLASH_START - raw_base
    first_end_offset = FIRST_COPY_FLASH_END - raw_base
    first_bytes = raw[first_offset:first_end_offset]
    require(len(first_bytes) == 528 and sha256(first_bytes) == first["sha256"],
            "pinned first boot-copy bytes differ from manifest")

    slot = second["copySlot"]
    require((slot["address"], slot["bytes"], slot["continuation"]) ==
            (COPY_SHIM_SITE, COPY_SHIM_BYTES, COPY_SHIM_CONTINUATION),
            "second-copy hook slot differs from manifest")
    slot_bytes = raw[image_offset("master", COPY_SHIM_SITE, COPY_SHIM_BYTES, raw):
                    image_offset("master", COPY_SHIM_SITE, COPY_SHIM_BYTES, raw) + COPY_SHIM_BYTES]
    require(sha256(slot_bytes) == slot["sha256"],
            "pinned second-copy instructions differ from manifest hash")
    private_report_slot = pack_report["secondCopy"]["copySlot"]
    require(bytes.fromhex(private_report_slot["originalHex"]) == slot_bytes,
            "private static report and pinned source copy-slot bytes disagree")
    require(len(slot_bytes) == COPY_SHIM_BYTES,
            "second-copy original slot byte count changed")
    require(rv_pair_target(COPY_SHIM_SITE, slot_bytes[:8]) == SUFFIX_FLASH_START,
            "original second-copy slot no longer sources the owned suffix")
    require(rv_pair_target(COPY_SHIM_SITE + 8, slot_bytes[8:16]) == SUFFIX_RAM_START,
            "original second-copy slot no longer targets the suffix RAM VMA")

    return {
        "first_copy": {"flash_start": first["flashStart"],
                       "flash_end_exclusive": first["flashEndExclusive"],
                       "ram_start": first["ramStart"],
                       "ram_end_exclusive": first["ramEndExclusive"],
                       "bytes": first["bytes"], "sha256": sha256(first_bytes),
                       "preserved": True},
        "second_copy": {"flash_start": SUFFIX_FLASH_START,
                        "flash_end_exclusive": SUFFIX_FLASH_END,
                        "ram_start": SUFFIX_RAM_START,
                        "ram_end_exclusive": SUFFIX_RAM_END,
                        "bytes": SUFFIX_BYTES, "sha256": sha256(suffix),
                        "copy_slot_address": COPY_SHIM_SITE,
                        "copy_slot_bytes": COPY_SHIM_BYTES,
                        "copy_continuation": COPY_SHIM_CONTINUATION},
        "private_slot_bytes_sha256": sha256(slot_bytes),
    }


def read_ram_window(raw: bytes, address: int, length: int) -> tuple[int, bytes]:
    offset = image_offset("master", address, length, raw)
    return offset, raw[offset:offset + length]


def decode_rvc_host_prologue(data: bytes) -> dict[str, Any]:
    require(len(data) == RAW_HID_BYTES, "RawHID displaced prologue length changed")
    first, save_s0, save_s1, save_ra = struct.unpack("<4H", data)
    require(first & 0xE003 == 0x6001 and ((first >> 7) & 31) == 2,
            "RawHID entry no longer begins with C.ADDI16SP sp")
    immediate = (((first >> 12) & 1) << 9) | (((first >> 3) & 3) << 7) | \
                (((first >> 5) & 1) << 6) | (((first >> 2) & 1) << 5) | \
                (((first >> 6) & 1) << 4)
    if immediate & 0x200:
        immediate -= 0x400
    saves = []
    for instruction, expected_register, expected_offset in (
        (save_s0, 8, 0x88), (save_s1, 9, 0x84), (save_ra, 1, 0x8C)
    ):
        require(instruction & 0xE003 == 0xC002,
                "RawHID displaced instruction is not C.SWSP")
        register = (instruction >> 2) & 31
        offset = (((instruction >> 9) & 0x0F) << 2) | \
                 (((instruction >> 7) & 0x03) << 6)
        require(register == expected_register and offset == expected_offset,
                "RawHID saved-register/stack-offset contract changed")
        saves.append({"register": register, "sp_offset": offset})
    require(immediate == -0x90, "RawHID stack allocation changed")
    return {"stack_allocation": immediate, "saved_registers": saves,
            "pc_relative_instructions": False, "bytes_hex": data.hex()}


def decode_post_bss_setup(data: bytes) -> dict[str, Any]:
    require(len(data) == POST_BSS_BYTES, "post-BSS displaced instruction span changed")
    first, second = struct.unpack("<II", data)
    require(first & 0x7F == 0x13 and ((first >> 7) & 31) == 5 and
            ((first >> 15) & 31) == 0 and ((first >> 12) & 7) == 0 and
            (first >> 20) == 0x25,
            "post-BSS first displaced instruction is not addi t0,zero,0x25")
    require(second & 0x7F == 0x73 and ((second >> 20) & 0xFFF) == 0xBC0 and
            ((second >> 15) & 31) == 5 and ((second >> 12) & 7) == 1 and
            ((second >> 7) & 31) == 0,
            "post-BSS second displaced instruction is not csrw 0xBC0,t0")
    return {"bytes_hex": data.hex(), "instruction_words": [first, second],
            "replay_after_stack_restore": True}


def verify_stock_memops(raw: bytes, raw_base: int) -> dict[str, Any]:
    rows = (("memcpy", MEMCPY_FLASH, "c.mv t1,a0"),
            ("memmove", MEMMOVE_FLASH, "c.beqz a2"),
            ("memset", MEMSET_FLASH, "c.mv t1,a0"))
    result = {}
    for name, address, expected in rows:
        offset = address - raw_base
        require(0 <= offset and offset + 2 <= len(raw),
                f"stock {name} entry is outside pinned flash image")
        first = struct.unpack_from("<H", raw, offset)[0]
        if expected == "c.mv t1,a0":
            require(first & 0xE003 == 0x8002 and ((first >> 12) & 1) == 0 and
                    ((first >> 7) & 31) == 6 and ((first >> 2) & 31) == 10,
                    f"stock {name} entry has changed")
        else:
            require(first & 0xE003 == 0xC001 and
                    8 + ((first >> 7) & 7) == 12,
                    "stock memmove entry has changed")
        result[name] = {"flash_address": address,
                        "first_instruction_hex": raw[offset:offset + 2].hex(),
                        "first_instruction": expected,
                        "abi": "a0/a1/a2 arguments; ra return"}
    return result


def riscv_jal_target(pc: int, instruction: int) -> int:
    require(instruction & 0x7F == 0x6F and ((instruction >> 7) & 31) == 0,
            "expected RV32 JAL x0")
    immediate = (((instruction >> 31) & 1) << 20) | \
                (((instruction >> 21) & 0x3FF) << 1) | \
                (((instruction >> 20) & 1) << 11) | \
                (((instruction >> 12) & 0xFF) << 12)
    if immediate & 0x100000:
        immediate -= 0x200000
    return (pc + immediate) & 0xFFFFFFFF


def riscv_branch_target(pc: int, instruction: int) -> int:
    require(instruction & 0x7F == 0x63, "expected RV32 branch")
    immediate = (((instruction >> 31) & 1) << 12) | \
                (((instruction >> 7) & 1) << 11) | \
                (((instruction >> 25) & 0x3F) << 5) | \
                (((instruction >> 8) & 0xF) << 1)
    if immediate & 0x1000:
        immediate -= 0x2000
    return (pc + immediate) & 0xFFFFFFFF


def entry_assembly() -> str:
    return f"""\
    .option push
    .option norvc
    .option norelax

    .section .rawhid_entry,"ax",@progbits
    .balign 2
    .globl dp104_master_rawhid_entry_patch
dp104_master_rawhid_entry_patch:
    jal zero, dp104_master_host_hook
    .option rvc
    c.nop
    c.nop
    .option norvc

    .section .version_entry,"ax",@progbits
    .balign 2
    .globl dp104_master_version_entry_patch
dp104_master_version_entry_patch:
    jal zero, dp104_master_version_hook
    .rept 4
    .hword 0x0001
    .endr

    .section .copy_shim,"ax",@progbits
    .balign 2
    .globl dp104_master_copy_shim
dp104_master_copy_shim:
    call dp104_restore_suffix
    beq a0, zero, .Ldp104_copy_failure
    jal zero, dp104_ram_copy_continue
.Ldp104_copy_failure:
    jal zero, .Ldp104_copy_failure
    .rept 6
    .word 0x00000013
    .endr

    .section .post_bss,"ax",@progbits
    .balign 2
    .globl dp104_master_post_bss_patch
dp104_master_post_bss_patch:
    call dp104_copy_code

    .section .taskcall,"ax",@progbits
    .balign 2
    .globl dp104_master_taskcall_patch
dp104_master_taskcall_patch:
    call dp104_master_task_hook

    .section .uart_callback,"ax",@progbits
    .balign 2
    .globl dp104_master_uart_callback_pair
dp104_master_uart_callback_pair:
    lla a0, dp104_master_uart_rx

    .section .uart_line_entry,"ax",@progbits
    .balign 2
    .globl dp104_master_uart_line_entry_patch
dp104_master_uart_line_entry_patch:
    jal zero, dp104_master_uart_line_hook
    .word 0x00000013

    .section .uart_rda_entry,"ax",@progbits
    .balign 2
    .globl dp104_master_uart_rda_entry_patch
dp104_master_uart_rda_entry_patch:
    jal zero, dp104_master_uart_rda_hook
    .word 0x00000013

    .section .uart_fcr,"ax",@progbits
    .balign 2
    ori a5, a5, 0x40

    .section .uart_fcr_init,"ax",@progbits
    .balign 2
    addi a5, zero, 0x47

    .option pop
"""


def host_legacy_assembly(original_prologue: bytes) -> str:
    require(len(original_prologue) == RAW_HID_BYTES,
            "host legacy trampoline requires the original eight-byte prologue")
    byte_lines = ", ".join(f"0x{value:02x}" for value in original_prologue)
    return f"""\
    .option push
    .option norelax
    .section .text.dp104_master_host_legacy,"ax",@progbits
    .balign 2
    .globl dp104_master_host_legacy
    .type dp104_master_host_legacy,@function
dp104_master_host_legacy:
    .byte {byte_lines}
    .option norvc
    jal zero, dp104_master_host_continue
    .size dp104_master_host_legacy, .-dp104_master_host_legacy
    .option pop

    .option push
    .option rvc
    .option norelax
    .section .text.dp104_master_version_hook,"ax",@progbits
    .balign 2
    .globl dp104_master_version_hook
    .type dp104_master_version_hook,@function
dp104_master_version_hook:
    li a5, 0x{VERSION_HALFWORD:x}
    sh zero, 14(sp)
    sh a5, 16(sp)
    jal zero, dp104_master_version_continue
    .size dp104_master_version_hook, .-dp104_master_version_hook
    .option pop

    .option push
    .option norvc
    .option norelax
    .section .text.dp104_master_uart_line_hook,"ax",@progbits
    .balign 2
    .globl dp104_master_uart_line_hook
    .type dp104_master_uart_line_hook,@function
dp104_master_uart_line_hook:
    lbu a0, 5(a4)
    call dp104_master_uart_line_status
    mret
    .size dp104_master_uart_line_hook, .-dp104_master_uart_line_hook

    .section .text.dp104_master_uart_rda_hook,"ax",@progbits
    .balign 2
    .globl dp104_master_uart_rda_hook
    .type dp104_master_uart_rda_hook,@function
dp104_master_uart_rda_hook:
    call dp104_master_uart_rda_drain
    mret
    .size dp104_master_uart_rda_hook, .-dp104_master_uart_rda_hook
    .option pop
"""


def helper_assembly(packed_length: int, code_lma: int, code_bytes: int,
                    code_start: int, csr_words: tuple[int, int]) -> str:
    return f"""\
    .option push
    .option norvc
    .option norelax

    .section .text.dp104_restore_suffix,"ax",@progbits
    .balign 2
    .globl dp104_restore_suffix
    .type dp104_restore_suffix,@function
dp104_restore_suffix:
    addi sp, sp, -16
    sw ra, 12(sp)
    li a0, 0x{SUFFIX_FLASH_START:x}
    li a1, {packed_length}
    li a2, 0x{SUFFIX_RAM_START:x}
    li a3, {SUFFIX_BYTES}
    call dp104_lz_decode
    lw ra, 12(sp)
    addi sp, sp, 16
    ret
    .size dp104_restore_suffix, .-dp104_restore_suffix

    .section .text.dp104_copy_code,"ax",@progbits
    .balign 2
    .globl dp104_copy_code
    .type dp104_copy_code,@function
dp104_copy_code:
    addi sp, sp, -16
    sw ra, 12(sp)
    li a0, 0x{code_start:x}
    li a1, 0x{code_lma:x}
    li a2, {code_bytes}
    call memcpy
    lw ra, 12(sp)
    addi sp, sp, 16
    .word 0x{csr_words[0]:08x}
    .word 0x{csr_words[1]:08x}
    ret
    .size dp104_copy_code, .-dp104_copy_code

    .option pop
"""


def compile_object(zig: Path, env: dict[str, str], source: Path,
                   output: Path, profile: dict[str, Any]) -> dict[str, Any]:
    output.parent.mkdir(parents=True, exist_ok=True)
    command = [
        str(zig), "cc", "-target", profile["freestanding_triple"],
        "-std=c99", "-Oz", "-g0", "-ffreestanding", "-fno-builtin",
        "-fno-stack-protector", "-fno-common", "-fno-pic", "-fno-pie",
        "-fno-unwind-tables", "-fno-asynchronous-unwind-tables",
        "-ffunction-sections", "-fdata-sections", "-nostdlib", "-Wall",
        "-Wextra", "-Werror", "-Isrc/firmware/freestanding",
        "-Isrc/firmware", "-Iinclude", *profile["architecture_flags"],
        "-c", str(source), "-o", str(output),
    ]
    result = run(command, env)
    return {"source": source.relative_to(ROOT).as_posix(),
            "object": output.relative_to(ROOT).as_posix(),
            "sha256": sha256_file(output), "command": command,
            "stdout": result.stdout.strip()}


def linker_script(code_start: int, state_start: int,
                  helper_start: int | None = None) -> str:
    helper_phdr = "  flash_helpers PT_LOAD FLAGS(5);\n" if helper_start is not None else ""
    helper_section = "" if helper_start is None else f"""
  .flash_helpers 0x{helper_start:x} :
  {{
    KEEP(*(.text.dp104_restore_suffix))
    KEEP(*(.text.dp104_copy_code))
    *dp104_lz.o(.text .text.* .rodata .rodata.* .srodata .srodata.*)
    *master_helpers.o(.text .text.* .rodata .rodata.* .srodata .srodata.*)
    . = ALIGN(2);
  }} :flash_helpers
"""
    helper_alias = helper_start if helper_start is not None else SUFFIX_FLASH_START
    return f"""ENTRY(dp104_master_host_hook)
PROVIDE(__dp104_master_state = 0x{state_start:x});
PROVIDE(__global_pointer$ = 0x2000e1b0);
PROVIDE(dp104_master_task_legacy = 0x{TASK_LEGACY_ENTRY:x});
PROVIDE(dp104_master_host_continue = 0x{RAW_HID_CONTINUATION:x});
PROVIDE(dp104_master_version_continue = 0x{VERSION_CONTINUATION:x});
PROVIDE(dp104_ram_copy_continue = 0x{COPY_SHIM_CONTINUATION:x});
memcpy = 0x{MEMCPY_FLASH:x};
memmove = 0x{MEMMOVE_FLASH:x};
memset = 0x{MEMSET_FLASH:x};
PROVIDE(dp104_restore_suffix = 0x{helper_alias:x});
PROVIDE(dp104_copy_code = 0x{helper_alias:x});
PHDRS
{{
  rawhid PT_LOAD FLAGS(5);
  version_entry PT_LOAD FLAGS(5);
  copy_shim PT_LOAD FLAGS(5);
  post_bss PT_LOAD FLAGS(5);
  taskcall PT_LOAD FLAGS(5);
  uart_callback PT_LOAD FLAGS(5);
  uart_line_entry PT_LOAD FLAGS(5);
  uart_rda_entry PT_LOAD FLAGS(5);
  uart_fcr PT_LOAD FLAGS(5);
  uart_fcr_init PT_LOAD FLAGS(5);
  ram_code PT_LOAD FLAGS(5);
{helper_phdr}}}
SECTIONS
{{
  .rawhid_entry 0x{RAW_HID_SITE:x} : {{ KEEP(*(.rawhid_entry)) }} :rawhid
  .version_entry 0x{VERSION_SITE:x} : {{ KEEP(*(.version_entry)) }} :version_entry
  .copy_shim 0x{COPY_SHIM_SITE:x} : {{ KEEP(*(.copy_shim)) }} :copy_shim
  .post_bss 0x{POST_BSS_SITE:x} : {{ KEEP(*(.post_bss)) }} :post_bss
  .taskcall 0x{TASK_CALL_SITE:x} : {{ KEEP(*(.taskcall)) }} :taskcall
  .uart_callback 0x{UART_CALLBACK_PAIR:x} : {{ KEEP(*(.uart_callback)) }} :uart_callback
  .uart_line_entry 0x{UART_LINE_SITE:x} : {{ KEEP(*(.uart_line_entry)) }} :uart_line_entry
  .uart_rda_entry 0x{UART_RDA_SITE:x} : {{ KEEP(*(.uart_rda_entry)) }} :uart_rda_entry
  .uart_fcr 0x{UART_FCR_SITE:x} : {{ KEEP(*(.uart_fcr)) }} :uart_fcr
  .uart_fcr_init 0x{UART_FCR_INIT_SITE:x} : {{ KEEP(*(.uart_fcr_init)) }} :uart_fcr_init
  .ram_code 0x{code_start:x} :
  {{
    KEEP(*(.text.dp104_master_host_hook))
    KEEP(*(.text.dp104_master_task_hook))
    KEEP(*(.text.dp104_master_host_legacy))
    KEEP(*(.text.dp104_master_version_hook))
    KEEP(*(.text.dp104_master_uart_line_hook))
    KEEP(*(.text.dp104_master_uart_rda_hook))
    *dp104_master_target.o(.text .text.* .rodata .rodata.* .srodata .srodata.*)
    *dp104_master_bridge.o(.text .text.* .rodata .rodata.* .srodata .srodata.*)
    *dp104_wire.o(.text .text.* .rodata .rodata.* .srodata .srodata.*)
    *dp104_wire_stream.o(.text .text.* .rodata .rodata.* .srodata .srodata.*)
    *dp104_usb_stage.o(.text .text.* .rodata .rodata.* .srodata .srodata.*)
    . = ALIGN(2);
  }} :ram_code
  .data : {{ *(.data .data.* .sdata .sdata.*) }} :ram_code
  .bss (NOLOAD) : {{ *(.bss .bss.* .sbss .sbss.*) *(COMMON) }} :ram_code
  .got : {{ *(.got .got.*) }} :ram_code
{helper_section}
  /DISCARD/ : {{ *(.comment) *(.note .note.*) *(.eh_frame .eh_frame.*) }}
}}
ASSERT(SIZEOF(.rawhid_entry) == {RAW_HID_BYTES}, "RawHID branch patch must be eight bytes")
ASSERT(SIZEOF(.version_entry) == {VERSION_BYTES}, "Version branch patch must be twelve bytes")
ASSERT(SIZEOF(.copy_shim) == {COPY_SHIM_BYTES}, "second-copy shim must be exactly 44 bytes")
ASSERT(SIZEOF(.post_bss) == {POST_BSS_BYTES}, "post-BSS patch must be exactly eight bytes")
ASSERT(SIZEOF(.taskcall) == {TASK_CALL_BYTES}, "task call patch must be one AUIPC/JALR pair")
ASSERT(SIZEOF(.uart_callback) == {UART_CALLBACK_BYTES}, "UART callback pair must be eight bytes")
ASSERT(SIZEOF(.uart_line_entry) == {UART_LINE_BYTES}, "UART status detour must be eight bytes")
ASSERT(SIZEOF(.uart_rda_entry) == {UART_RDA_BYTES}, "UART RDA detour must be eight bytes")
ASSERT(SIZEOF(.uart_fcr) == {UART_FCR_BYTES}, "UART trigger patch must be four bytes")
ASSERT(SIZEOF(.uart_fcr_init) == {UART_FCR_INIT_BYTES}, "UART initial trigger literal must be four bytes")
ASSERT(ADDR(.ram_code) == 0x{code_start:x}, "RAM code is not at measured state end")
ASSERT(SIZEOF(.ram_code) > 0, "linked RAM code is empty")
ASSERT(__dp104_master_state == 0x{state_start:x}, "master state address changed")
ASSERT(SIZEOF(.data) == 0, "master RAM payload unexpectedly has initialized data")
ASSERT(SIZEOF(.bss) == 0, "master RAM payload unexpectedly has BSS")
ASSERT(SIZEOF(.got) == 0, "master RAM payload unexpectedly has GOT state")
ASSERT((dp104_master_host_hook & 0xfffffffe) >= ADDR(.ram_code) &&
       (dp104_master_host_hook & 0xfffffffe) < ADDR(.ram_code) + SIZEOF(.ram_code),
       "host entry is outside RAM code")
ASSERT((dp104_master_task_hook & 0xfffffffe) >= ADDR(.ram_code) &&
       (dp104_master_task_hook & 0xfffffffe) < ADDR(.ram_code) + SIZEOF(.ram_code),
       "task entry is outside RAM code")
ASSERT((dp104_master_uart_rx & 0xfffffffe) >= ADDR(.ram_code) &&
       (dp104_master_uart_rx & 0xfffffffe) < ADDR(.ram_code) + SIZEOF(.ram_code),
       "UART entry is outside RAM code")
""" + ("ASSERT(ADDR(.flash_helpers) == 0x" + f"{helper_start:x}" +
        ", \"flash helpers are outside the owned second-copy span\")\n" if helper_start is not None else "")


def link_payload(zig: Path, env: dict[str, str], objects: list[Path],
                 script_path: Path, output: Path, profile: dict[str, Any]) -> dict[str, Any]:
    output.parent.mkdir(parents=True, exist_ok=True)
    command = [
        str(zig), "cc", "-target", profile["freestanding_triple"],
        "-Oz", "-g", "-nostdlib", "-static", "-no-pie", *profile["architecture_flags"],
        *[str(path) for path in objects], "-Wl,-T," + str(script_path),
        "-Wl,-e,dp104_master_host_hook", "-Wl,--build-id=none",
        "-Wl,--no-undefined", "-Wl,--gc-sections", "-Wl,-z,max-page-size=0x4",
        "-o", str(output),
    ]
    result = run(command, env)
    return {"path": output.relative_to(ROOT).as_posix(),
            "sha256": sha256_file(output), "command": command,
            "stdout": result.stdout.strip()}


def section_bytes(elf_path: Path, elf: dict[str, Any], name: str) -> tuple[dict[str, Any], bytes]:
    matches = [section for section in elf["sections"] if section["name"] == name]
    require(len(matches) == 1, f"ELF section {name} is missing or duplicated")
    section = matches[0]
    data = elf_path.read_bytes()
    return section, data[section["offset"]:section["offset"] + section["size"]]


def segment_bytes(elf_path: Path, segment: dict[str, Any]) -> bytes:
    data = elf_path.read_bytes()
    start = segment["offset"]
    end = start + segment["file_size"]
    require(end <= len(data), "ELF load segment exceeds its file")
    return data[start:end]


def compile_objects(zig: Path, env: dict[str, str], output_dir: Path,
                    raw: bytes, profile: dict[str, Any]) -> tuple[list[Path], list[dict[str, Any]]]:
    private = output_dir / "private"
    object_dir = output_dir / "objects"
    private.mkdir(parents=True, exist_ok=True)
    host_raw = read_ram_window(raw, RAW_HID_SITE, RAW_HID_BYTES)[1]
    decode_rvc_host_prologue(host_raw)
    legacy_source = private / "master_legacy.S"
    legacy_source.write_text(host_legacy_assembly(host_raw), encoding="ascii")
    entries_source = private / "master_entries.S"
    entries_source.write_text(entry_assembly(), encoding="ascii")
    sources = (
        ("dp104_master_target", ROOT / "src/firmware/dp104_master_target.c"),
        ("dp104_master_bridge", ROOT / "src/firmware/dp104_master_bridge.c"),
        ("dp104_wire", ROOT / "src/firmware/dp104_wire.c"),
        ("dp104_wire_stream", ROOT / "src/firmware/dp104_wire_stream.c"),
        ("dp104_usb_stage", ROOT / "src/firmware/dp104_usb_stage.c"),
        ("rv32_host_hook", ROOT / "src/firmware/rv32_host_hook.S"),
        ("rv32_task_hook", ROOT / "src/firmware/rv32_task_hook.S"),
        ("master_legacy", legacy_source),
        ("master_entries", entries_source),
    )
    objects: list[Path] = []
    results: list[dict[str, Any]] = []
    for name, source in sources:
        output = object_dir / f"{name}.o"
        results.append(compile_object(zig, env, source, output, profile))
        objects.append(output)
    return objects, results


def validate_payload_elf(elf_path: Path, code_start: int,
                         helper_start: int | None) -> tuple[dict[str, Any], bytes, dict[str, bytes]]:
    profile = PROFILES["master"]
    elf = inspect_elf(elf_path.resolve())
    check_machine_abi(elf, profile, "linked full master payload")
    require(not elf["has_interpreter"] and not elf["undefined_symbols"],
            f"master payload has dynamic or unresolved symbols: {elf['undefined_symbols']}")
    require(elf["entry"] != 0, "linked payload has an empty entry symbol")
    require(not any(item["name"] in (".interp", ".dynamic", ".dynsym")
                    for item in elf["sections"]),
            "master payload contains dynamic-link sections")
    sections = {item["name"]: item for item in elf["sections"]}
    for name in (".data", ".sdata", ".bss", ".sbss", ".got"):
        require(sections.get(name, {"size": 0})["size"] == 0,
                f"master payload unexpectedly contains {name}")
    expected_addresses = {RAW_HID_SITE, VERSION_SITE, COPY_SHIM_SITE, POST_BSS_SITE,
                          TASK_CALL_SITE, UART_CALLBACK_PAIR, UART_LINE_SITE, UART_RDA_SITE, UART_FCR_SITE, UART_FCR_INIT_SITE, code_start}
    if helper_start is not None:
        expected_addresses.add(helper_start)
    loads = elf["program_headers"]
    require(len(loads) == len(expected_addresses) and
            {item["virtual_address"] for item in loads} == expected_addresses,
            "master payload PT_LOAD addresses differ from approved windows")
    require(all(item["flags"] == 5 and
                item["file_size"] == item["memory_size"]
                for item in loads),
            "master payload contains writable or non-file-backed load segments")
    code_load = [item for item in loads if item["virtual_address"] == code_start]
    require(len(code_load) == 1 and code_load[0]["file_size"] > 0,
            "nonempty RAM code segment is missing")
    code = segment_bytes(elf_path.resolve(), code_load[0])
    ram_section, ram_section_bytes = section_bytes(elf_path.resolve(), elf, ".ram_code")
    require(ram_section["address"] == code_start and ram_section_bytes == code,
            "RAM load segment contains unaccounted orphan sections")
    for name, address in (("memcpy", MEMCPY_FLASH), ("memmove", MEMMOVE_FLASH),
                          ("memset", MEMSET_FLASH)):
        require(elf["symbols"].get(name, {}).get("value") == address,
                f"{name} did not bind to the verified stock flash helper")
    expected_sections = (
        (".rawhid_entry", RAW_HID_SITE, RAW_HID_BYTES),
        (".version_entry", VERSION_SITE, VERSION_BYTES),
        (".copy_shim", COPY_SHIM_SITE, COPY_SHIM_BYTES),
        (".post_bss", POST_BSS_SITE, POST_BSS_BYTES),
        (".taskcall", TASK_CALL_SITE, TASK_CALL_BYTES),
        (".uart_callback", UART_CALLBACK_PAIR, UART_CALLBACK_BYTES),
        (".uart_line_entry", UART_LINE_SITE, UART_LINE_BYTES),
        (".uart_rda_entry", UART_RDA_SITE, UART_RDA_BYTES),
        (".uart_fcr", UART_FCR_SITE, UART_FCR_BYTES),
        (".uart_fcr_init", UART_FCR_INIT_SITE, UART_FCR_INIT_BYTES),
    )
    windows: dict[str, bytes] = {}
    for name, address, length in expected_sections:
        section, data = section_bytes(elf_path.resolve(), elf, name)
        require(section["address"] == address and section["size"] == length,
                f"{name} does not fill its exact approved slot")
        windows[name] = data
    if helper_start is not None:
        helper_load = [item for item in loads if item["virtual_address"] == helper_start]
        require(len(helper_load) == 1 and helper_load[0]["file_size"] > 0,
                "flash restore-helper segment is missing or empty")
        windows[".flash_helpers"] = segment_bytes(elf_path.resolve(), helper_load[0])
    for name in ("dp104_master_host_hook", "dp104_master_task_hook",
                 "dp104_master_uart_rx", "dp104_master_uart_rda_drain"):
        symbol = elf["symbols"].get(name)
        require(symbol is not None and code_start <= symbol["value"] <
                code_start + len(code), f"required master entry {name} is absent")
    return elf, code, windows


def validate_entry_windows(elf: dict[str, Any], code_start: int,
                           code: bytes, windows: dict[str, bytes],
                           helper_start: int | None,
                           packed_suffix_bytes: int | None = None,
                           code_lma: int | None = None,
                           replay_words: tuple[int, int] | None = None) -> dict[str, Any]:
    symbols = elf["symbols"]
    host_hook = symbols["dp104_master_host_hook"]["value"]
    task_hook = symbols["dp104_master_task_hook"]["value"]
    uart_rx = symbols["dp104_master_uart_rx"]["value"]
    version_hook = symbols["dp104_master_version_hook"]
    version_patch = windows[".version_entry"]
    require(riscv_jal_target(VERSION_SITE, struct.unpack_from("<I", version_patch)[0]) ==
            version_hook["value"] and version_patch[4:] == b"\x01\x00" * 4,
            "firmware-version detour changes registers or has invalid padding")
    version_offset = version_hook["value"] - code_start
    leaf = code[version_offset:version_offset + version_hook["size"]]
    require(len(leaf) == 18, "firmware-version leaf has unexpected size")
    upper, lower = struct.unpack_from("<H", leaf)[0], struct.unpack_from("<I", leaf, 2)[0]
    immediate = ((upper >> 2) & 31) | (((upper >> 12) & 1) << 5)
    if immediate & 32:
        immediate -= 64
    addend = lower >> 20
    if addend & 0x800:
        addend -= 0x1000
    require(upper & 0xe003 == 0x6001 and (upper >> 7) & 31 == 15 and
            lower & 0x707f == 0x13 and (lower >> 7) & 31 == 15 and
            (lower >> 15) & 31 == 15 and
            ((immediate << 12) + addend) & 0xffffffff == VERSION_HALFWORD,
            "firmware-version leaf does not construct the requested packed version")
    require(struct.unpack_from("<II", leaf, 6) ==
            ((2 << 15) | (1 << 12) | (14 << 7) | 0x23,
             (15 << 20) | (2 << 15) | (1 << 12) | (16 << 7) | 0x23) and
            riscv_jal_target(version_hook["value"] + 14,
                             struct.unpack_from("<I", leaf, 14)[0]) == VERSION_CONTINUATION,
            "firmware-version leaf does not preserve original stores and continuation")

    rawhid = windows[".rawhid_entry"]
    require(riscv_jal_target(RAW_HID_SITE, struct.unpack_from("<I", rawhid)[0]) == host_hook and
            struct.unpack_from("<HH", rawhid, 4) == (0x0001, 0x0001),
            "compressed-suffix RawHID patch is not a register-preserving JAL/two-C.NOP")

    suffix_helper = symbols.get("dp104_restore_suffix", {}).get("value",
                                                                SUFFIX_FLASH_START)
    code_helper = symbols.get("dp104_copy_code", {}).get("value",
                                                          SUFFIX_FLASH_START)
    copy_shim = windows[".copy_shim"]
    require(rv_call_target(COPY_SHIM_SITE, copy_shim[:8]) == suffix_helper,
            "second-copy shim does not call the flash suffix restore helper")
    failure_branch = struct.unpack_from("<I", copy_shim, 8)[0]
    success_jump = struct.unpack_from("<I", copy_shim, 12)[0]
    failure_jump = struct.unpack_from("<I", copy_shim, 16)[0]
    require(((failure_branch >> 12) & 7) == 0 and
            ((failure_branch >> 15) & 31) == 10 and
            ((failure_branch >> 20) & 31) == 0 and
            riscv_branch_target(COPY_SHIM_SITE + 8, failure_branch) ==
            COPY_SHIM_SITE + 16,
            "second-copy shim does not halt when decompression returns zero")
    require(riscv_jal_target(COPY_SHIM_SITE + 12, success_jump) == COPY_SHIM_CONTINUATION and
            riscv_jal_target(COPY_SHIM_SITE + 16, failure_jump) == COPY_SHIM_SITE + 16 and
            all(struct.unpack_from("<I", copy_shim, offset)[0] == 0x00000013
                for offset in range(20, COPY_SHIM_BYTES, 4)),
            "second-copy shim continuation/failure/padding is invalid")

    post_bss = windows[".post_bss"]
    require(rv_call_target(POST_BSS_SITE, post_bss) == code_helper,
            "post-BSS shim does not call the flash code-copy helper")
    task = windows[".taskcall"]
    require(rv_call_target(TASK_CALL_SITE, task) == task_hook,
            "task AUIPC/JALR patch does not call the new task veneer")
    require(((struct.unpack_from("<I", task)[0] >> 7) & 31) == 1 and
            ((struct.unpack_from("<I", task, 4)[0] >> 7) & 31) == 1,
            "task call patch does not link through ra")
    line_hook = symbols["dp104_master_uart_line_hook"]
    line_status = symbols["dp104_master_uart_line_status"]["value"]
    line_entry = windows[".uart_line_entry"]
    require(riscv_jal_target(UART_LINE_SITE, struct.unpack_from("<I", line_entry)[0]) ==
            line_hook["value"] and struct.unpack_from("<I", line_entry, 4)[0] == 0x00000013,
            "UART line-status entry must be one JAL plus one NOP")
    line_offset = line_hook["value"] - code_start
    require(line_hook["size"] == 16 and 0 <= line_offset and
            line_offset + 16 <= len(code), "UART status wrapper is outside owned RAM code")
    line_body = code[line_offset:line_offset + 16]
    require(struct.unpack_from("<I", line_body)[0] == 0x00574503 and
            rv_call_target(line_hook["value"] + 4, line_body[4:12]) == line_status and
            struct.unpack_from("<I", line_body, 12)[0] == 0x30200073,
            "UART status wrapper must preserve one LBU at a4+5, call the recorder, then MRET")
    rda_hook = symbols["dp104_master_uart_rda_hook"]
    rda_drain = symbols["dp104_master_uart_rda_drain"]["value"]
    rda_entry = windows[".uart_rda_entry"]
    require(riscv_jal_target(UART_RDA_SITE, struct.unpack_from("<I", rda_entry)[0]) ==
            rda_hook["value"] and struct.unpack_from("<I", rda_entry, 4)[0] == 0x00000013,
            "UART RDA entry must be one JAL plus one NOP")
    rda_offset = rda_hook["value"] - code_start
    require(rda_hook["size"] == 12 and 0 <= rda_offset and rda_offset + 12 <= len(code),
            "UART RDA wrapper is outside owned RAM code")
    rda_body = code[rda_offset:rda_offset + 12]
    require(rv_call_target(rda_hook["value"], rda_body[:8]) == rda_drain and
            struct.unpack_from("<I", rda_body, 8)[0] == 0x30200073,
            "UART RDA wrapper must call bounded RFC drain then MRET")
    require(windows[".uart_fcr"] == UART_FCR_REPLACEMENT and
            struct.unpack("<I", windows[".uart_fcr"])[0] == 0x0407E793 and
            windows[".uart_fcr_init"] == UART_FCR_INIT_REPLACEMENT and
            struct.unpack("<I", windows[".uart_fcr_init"])[0] == 0x04700793,
            "UART FCR must initialize 0x47 and preserve trigger2 with ORI a5,a5,0x40")
    callback = windows[".uart_callback"]
    require(rv_pair_target(UART_CALLBACK_PAIR, callback) == uart_rx and
            ((struct.unpack_from("<I", callback)[0] >> 7) & 31) == 10,
            "UART constructor pair does not pass the new callback in a0")

    legacy = symbols.get("dp104_master_host_legacy")
    require(legacy is not None and legacy["size"] == RAW_HID_BYTES + 4,
            "host legacy trampoline is missing or not 12 bytes")
    legacy_offset = legacy["value"] - code_start
    require(0 <= legacy_offset and legacy_offset + legacy["size"] <= len(code),
            "host legacy trampoline lies outside RAM code")
    legacy_bytes = code[legacy_offset:legacy_offset + legacy["size"]]
    original_prologue = legacy_bytes[:RAW_HID_BYTES]
    decode_rvc_host_prologue(original_prologue)
    require(riscv_jal_target(legacy["value"] + RAW_HID_BYTES,
                             struct.unpack_from("<I", legacy_bytes, RAW_HID_BYTES)[0]) ==
            RAW_HID_CONTINUATION,
            "host legacy trampoline does not jump to the original continuation")
    require(symbols.get("dp104_master_task_legacy", {}).get("value") ==
            TASK_LEGACY_ENTRY,
            "task legacy symbol does not point to the pinned stock worker")

    helper_checks = None
    if helper_start is not None:
        require(packed_suffix_bytes is not None and code_lma is not None and
                replay_words is not None,
                "final helper validation lacks packed/code/replay operands")
        helper_checks = validate_helper_functions(
            elf, windows[".flash_helpers"], helper_start,
            packed_suffix_bytes, code_start, code_lma, len(code), replay_words)
    return {
        "symbols": {"host_hook": host_hook, "task_hook": task_hook,
                    "uart_rx": uart_rx, "host_legacy": legacy["value"],
                    "task_legacy": TASK_LEGACY_ENTRY},
        "rawhid_jump_target": host_hook,
        "compatibility_version": RELEASE_VERSION,
        "version_hook": {"address": version_hook["value"], "size": 18,
                         "packed_value": PACKED_VERSION, "register_clobbers": ["a5"],
                         "continuation": VERSION_CONTINUATION},
        "copy_shim": {"helper_target": suffix_helper,
                      "continuation": COPY_SHIM_CONTINUATION,
                      "failure_self_loop": True, "size": COPY_SHIM_BYTES},
        "post_bss_helper_target": code_helper,
        "task_call_target": task_hook,
        "uart_callback_target": uart_rx,
        "uart_line_status": {"site": UART_LINE_SITE, "hook": line_hook["value"],
                             "recorder": line_status, "single_original_lsr_read": True,
                             "wrapper_sha256": sha256(line_body)},
        "uart_receive_policy": {"rda_site": UART_RDA_SITE, "rda_hook": rda_hook["value"],
                                "drain": rda_drain, "max_fifo_reads_per_irq": 8,
                                "wrapper_sha256": sha256(rda_body),
                                "fcr_site": UART_FCR_SITE,
                                "fcr_replacement_hex": windows[".uart_fcr"].hex(),
                                "initial_fcr_site": UART_FCR_INIT_SITE,
                                "initial_fcr_literal_hex": windows[".uart_fcr_init"].hex(),
                                "trigger_reference_bytes": 2,
                                "rda_keeps_one_fifo_byte": True,
                                "legacy_timeout_callback_unchanged": True},
        "host_legacy_trampoline": {"address": legacy["value"],
                                   "length": legacy["size"],
                                   "bytes_sha256": sha256(legacy_bytes),
                                   "original_prologue_replayed": True,
                                   "continuation": RAW_HID_CONTINUATION},
        "restore_helper_checks": helper_checks,
    }


def validate_helper_functions(elf: dict[str, Any], helper: bytes,
                              helper_start: int, packed_suffix_bytes: int,
                              code_start: int, code_lma: int, code_bytes: int,
                              replay_words: tuple[int, int]) -> dict[str, Any]:
    symbols = elf["symbols"]
    results: dict[str, Any] = {}
    for name in ("dp104_restore_suffix", "dp104_copy_code"):
        symbol = symbols.get(name)
        require(symbol is not None and helper_start <= symbol["value"] <
                helper_start + len(helper) and symbol["size"] >= 24,
                f"flash helper symbol {name} is missing or too small")
        offset = symbol["value"] - helper_start
        body = helper[offset:offset + symbol["size"]]
        require(len(body) == symbol["size"] and len(body) % 4 == 0,
                f"flash helper {name} is not fixed-width RV32 instructions")
        require(struct.unpack_from("<I", body, 0)[0] == 0xFF010113 and
                struct.unpack_from("<I", body, 4)[0] == 0x00112623,
                f"flash helper {name} does not save ra in an aligned stack frame")
        require(struct.unpack_from("<I", body, len(body) - 4)[0] == 0x00008067,
                f"flash helper {name} does not return through restored ra")
        # The authored wrappers must leave gp untouched and balance their stack.
        require(not any((instruction & 0x7F) in (0x13, 0x33, 0x03, 0x17, 0x37,
                                                  0x67, 0x6F, 0x73) and
                        ((instruction >> 7) & 31) == 3
                        for instruction, in struct.iter_unpack("<I", body)),
                f"flash helper {name} writes gp")
        require(any(struct.unpack_from("<I", body, cursor)[0] == 0x00C12083
                    for cursor in range(0, len(body), 4)) and
                any(struct.unpack_from("<I", body, cursor)[0] == 0x01010113
                    for cursor in range(0, len(body), 4)),
                f"flash helper {name} does not restore ra and sp")
        expected_target = (symbols.get("dp104_lz_decode", {}).get("value")
                           if name == "dp104_restore_suffix" else MEMCPY_FLASH)
        require(expected_target is not None,
                "expected flash helper callee is missing")
        call_sites = []
        for cursor in range(8, len(body) - 4, 4):
            first = struct.unpack_from("<I", body, cursor)[0]
            second = struct.unpack_from("<I", body, cursor + 4)[0]
            if first & 0x7F == 0x17 and second & 0x707F == 0x67:
                call_sites.append((cursor, rv_call_target(
                    symbol["value"] + cursor, body[cursor:cursor + 8])))
        require(any(target == expected_target for _cursor, target in call_sites),
                f"flash helper {name} does not call expected target 0x{expected_target:x}")
        require(len(call_sites) == 1,
                f"flash helper {name} has an unexpected extra call")
        call_offset = next(cursor for cursor, target in call_sites
                           if target == expected_target)
        constants: dict[int, int] = {}
        for cursor in range(8, call_offset, 4):
            instruction = struct.unpack_from("<I", body, cursor)[0]
            opcode = instruction & 0x7F
            rd = (instruction >> 7) & 31
            if opcode == 0x37:
                constants[rd] = instruction & 0xFFFFF000
            elif opcode == 0x13 and ((instruction >> 12) & 7) == 0:
                rs1 = (instruction >> 15) & 31
                immediate = instruction >> 20
                if immediate & 0x800:
                    immediate -= 0x1000
                if rs1 == 0:
                    base = 0
                elif rs1 in constants:
                    base = constants[rs1]
                else:
                    continue
                constants[rd] = (base + immediate) & 0xFFFFFFFF
        actual_arguments = (constants.get(10), constants.get(11),
                            constants.get(12), constants.get(13))
        if name == "dp104_restore_suffix":
            expected_arguments = (SUFFIX_FLASH_START, packed_suffix_bytes,
                                  SUFFIX_RAM_START, SUFFIX_BYTES)
            require(actual_arguments == expected_arguments,
                    "suffix helper LZ source/destination/length arguments changed")
            require(body[-12:-4] == struct.pack("<II", 0x00C12083, 0x01010113),
                    "suffix restore helper does not restore ra and sp before returning")
        else:
            require(actual_arguments[:3] == (code_start, code_lma, code_bytes),
                    "code-copy helper memcpy source/destination/length arguments changed")
        if name == "dp104_copy_code":
            require(len(body) >= 24 and
                    body[-20:-12] == struct.pack("<II", 0x00C12083, 0x01010113) and
                    body[-12:-4] == struct.pack("<II", *replay_words),
                    "code-copy helper does not replay displaced CSR setup after restoring sp")
        results[name] = {"address": symbol["value"], "size": symbol["size"],
                         "saves_ra": True, "restores_ra_and_sp": True,
            "gp_writes": 0,
            "callee": expected_target,
            "arguments": actual_arguments,
                         "post_bss_replay_after_stack_restore": name == "dp104_copy_code"}
    return results


def compile_payload_objects(zig: Path, env: dict[str, str], output_dir: Path,
                            raw: bytes, profile: dict[str, Any]) -> tuple[list[Path], list[dict[str, Any]]]:
    private = output_dir / "private"
    private.mkdir(parents=True, exist_ok=True)
    host_raw = read_ram_window(raw, RAW_HID_SITE, RAW_HID_BYTES)[1]
    decode_rvc_host_prologue(host_raw)
    legacy = private / "master_legacy.S"
    legacy.write_text(host_legacy_assembly(host_raw), encoding="ascii")
    entries = private / "master_entries.S"
    entries.write_text(entry_assembly(), encoding="ascii")
    sources = (
        ("dp104_master_target", ROOT / "src/firmware/dp104_master_target.c"),
        ("dp104_master_bridge", ROOT / "src/firmware/dp104_master_bridge.c"),
        ("dp104_wire", ROOT / "src/firmware/dp104_wire.c"),
        ("dp104_wire_stream", ROOT / "src/firmware/dp104_wire_stream.c"),
        ("dp104_usb_stage", ROOT / "src/firmware/dp104_usb_stage.c"),
        ("rv32_host_hook", ROOT / "src/firmware/rv32_host_hook.S"),
        ("rv32_task_hook", ROOT / "src/firmware/rv32_task_hook.S"),
        ("master_legacy", legacy),
        ("master_entries", entries),
    )
    objects: list[Path] = []
    results: list[dict[str, Any]] = []
    for name, source in sources:
        output = output_dir / "objects" / f"{name}.o"
        results.append(compile_object(zig, env, source, output, profile))
        objects.append(output)
    return objects, results


def apply_raw_patch(raw: bytes, patched: bytearray, offset: int,
                    before: bytes, after: bytes, label: str,
                    ranges: list[dict[str, Any]]) -> dict[str, Any]:
    require(len(before) == len(after) and len(before) > 0,
            f"invalid patch size for {label}")
    require(0 <= offset and offset + len(before) <= len(raw),
            f"patch {label} is outside raw image")
    require(raw[offset:offset + len(before)] == before,
            f"source bytes differ at patch {label}")
    patched[offset:offset + len(after)] = after
    ranges.append({"start": offset, "end_exclusive": offset + len(after),
                   "label": label})
    return {"raw_file_offset": offset, "length": len(after),
            "old_sha256": sha256(before), "new_sha256": sha256(after),
            "label": label}


def apply_planner_recipes(raw: bytes, patched: bytearray, plan: dict[str, Any],
                          ranges: list[dict[str, Any]]) -> list[dict[str, Any]]:
    output = []
    master_recipes = [item for item in plan["reservationRecipesNotApplied"]
                      if item["role"] == "master"]
    require(len(master_recipes) == 3,
            "planner no longer emits exactly three coupled master reservation recipes")
    expected_addresses = {BSS_END_PAIR, HEAP_FLOOR_PAIR, INITIAL_HEAP_WORD}
    require({int(item["runtimeAddress"], 0) for item in master_recipes} ==
            expected_addresses,
            "planner master reservation recipe addresses changed")
    for recipe in master_recipes:
        address = int(recipe["runtimeAddress"], 0)
        offset = int(recipe["imageOffset"])
        before = bytes.fromhex(recipe["originalHex"])
        after = bytes.fromhex(recipe["replacementHex"])
        require(raw[offset:offset + len(before)] == before,
                f"planner original bytes differ at 0x{address:x}")
        label = f"reservation 0x{address:x}"
        output.append(apply_raw_patch(raw, patched, offset, before, after,
                                      label, ranges))
    return output


def pack_suffix(raw: bytes, raw_base: int, rawhid_patch: bytes,
                version_patch: bytes, uart_line_patch: bytes | None = None,
                uart_rda_patch: bytes | None = None) -> tuple[bytes, bytes, dict[str, Any]]:
    suffix_offset = SUFFIX_FLASH_START - raw_base
    suffix_end = SUFFIX_FLASH_END - raw_base
    suffix = bytearray(raw[suffix_offset:suffix_end])
    require(len(suffix) == SUFFIX_BYTES and sha256(suffix) == EXPECTED_SUFFIX_SHA256,
            "source suffix changed before compression")
    hid_offset = image_offset("master", RAW_HID_SITE, RAW_HID_BYTES, raw)
    hid_relative = hid_offset - suffix_offset
    require(0 <= hid_relative and hid_relative + RAW_HID_BYTES <= len(suffix),
            "RawHID entry is not inside the verified second-copy source span")
    original_hid = bytes(suffix[hid_relative:hid_relative + RAW_HID_BYTES])
    decode_rvc_host_prologue(original_hid)
    require(len(rawhid_patch) == RAW_HID_BYTES,
            "RawHID replacement has the wrong length")
    suffix[hid_relative:hid_relative + RAW_HID_BYTES] = rawhid_patch
    version_offset = image_offset("master", VERSION_SITE, VERSION_BYTES, raw)
    version_relative = version_offset - suffix_offset
    original_version = bytes(suffix[version_relative:version_relative + VERSION_BYTES])
    require(sha256(original_version) == COMPATIBILITY["masterVersion"]["originalSha256"] and
            len(version_patch) == VERSION_BYTES,
            "original firmware-version window or replacement size changed")
    suffix[version_relative:version_relative + VERSION_BYTES] = version_patch
    line_relative = None
    if uart_line_patch is not None:
        line_offset = image_offset("master", UART_LINE_SITE, UART_LINE_BYTES, raw)
        line_relative = line_offset - suffix_offset
        require(0 <= line_relative and line_relative + UART_LINE_BYTES <= len(suffix) and
                bytes(suffix[line_relative:line_relative + UART_LINE_BYTES]) == UART_LINE_ORIGINAL and
                len(uart_line_patch) == UART_LINE_BYTES,
                "UART status source window or replacement length changed")
        suffix[line_relative:line_relative + UART_LINE_BYTES] = uart_line_patch
    rda_relative = None
    if uart_rda_patch is not None:
        rda_offset = image_offset("master", UART_RDA_SITE, UART_RDA_BYTES, raw)
        rda_relative = rda_offset - suffix_offset
        require(0 <= rda_relative and rda_relative + UART_RDA_BYTES <= len(suffix) and
                bytes(suffix[rda_relative:rda_relative + UART_RDA_BYTES]) == UART_RDA_ORIGINAL and
                len(uart_rda_patch) == UART_RDA_BYTES,
                "UART RDA source window or replacement length changed")
        suffix[rda_relative:rda_relative + UART_RDA_BYTES] = uart_rda_patch
    patched_suffix = bytes(suffix)
    packed = pack_lz(patched_suffix)
    require(unpack_lz(packed, len(patched_suffix)) == patched_suffix,
            "patched second-copy suffix does not roundtrip through LZ")
    return packed, patched_suffix, {
        "source_flash_start": SUFFIX_FLASH_START,
        "source_flash_end_exclusive": SUFFIX_FLASH_END,
        "destination_ram_start": SUFFIX_RAM_START,
        "destination_ram_end_exclusive": SUFFIX_RAM_END,
        "original_bytes": SUFFIX_BYTES,
        "original_sha256": EXPECTED_SUFFIX_SHA256,
        "patched_bytes": len(patched_suffix),
        "patched_sha256": sha256(patched_suffix),
        "rawhid_source_offset": hid_relative,
        "rawhid_original_sha256": sha256(original_hid),
        "rawhid_replacement_sha256": sha256(rawhid_patch),
        "version_source_offset": version_relative,
        "version_replacement_sha256": sha256(version_patch),
        "uart_line_status_source_offset": line_relative,
        "uart_line_status_replacement_sha256": sha256(uart_line_patch) if uart_line_patch is not None else None,
        "uart_rda_source_offset": rda_relative,
        "uart_rda_replacement_sha256": sha256(uart_rda_patch) if uart_rda_patch is not None else None,
        "compatibility_version": RELEASE_VERSION,
        "packed_bytes": len(packed),
        "packed_sha256": sha256(packed),
        "roundtrip_exact": True,
    }


def pack_layout(packed_suffix: bytes, code: bytes) -> dict[str, Any]:
    code_lma = align_up(SUFFIX_FLASH_START + len(packed_suffix), 4)
    helper_start = align_up(code_lma + len(code), 4)
    require(helper_start < SUFFIX_FLASH_END,
            "compressed suffix plus linked RAM code leaves no room for restore helpers")
    return {"suffix_lma": SUFFIX_FLASH_START,
            "packed_suffix_bytes": len(packed_suffix),
            "packed_suffix_end": SUFFIX_FLASH_START + len(packed_suffix),
            "code_lma": code_lma,
            "code_bytes": len(code),
            "code_end": code_lma + len(code),
            "helper_lma": helper_start,
            "helper_capacity_before_link": SUFFIX_FLASH_END - helper_start,
            "owned_span_end": SUFFIX_FLASH_END}


def generate_helper_source(output_dir: Path, packed_length: int,
                           code_lma: int, code_length: int,
                           code_start: int, replay: tuple[int, int]) -> Path:
    path = output_dir / "private" / "master_helpers.S"
    path.write_text(helper_assembly(packed_length, code_lma, code_length,
                                    code_start, replay), encoding="ascii")
    return path


def copy_elf_inputs(objects: list[Path], helper_objects: list[Path]) -> list[Path]:
    return objects + helper_objects


def verify_helper_lma(elf: dict[str, Any], helper_start: int,
                      helper_length: int) -> None:
    require(helper_start >= SUFFIX_FLASH_START and
            helper_start + helper_length <= SUFFIX_FLASH_END,
            "linked restore helpers exceed the owned second-copy flash span")
    require(helper_length > 0,
            "linked restore-helper payload is empty")


def verify_data_lma_preserved(raw: bytes, patched: bytes, raw_base: int,
                              expected_heap: int) -> dict[str, Any]:
    start = DATA_LMA_START - raw_base
    end = DATA_LMA_END - raw_base
    heap = INITIAL_HEAP_WORD - raw_base
    require(0 <= start < end <= len(raw) and end - start == DATA_LMA_END - DATA_LMA_START,
            "initialized-data LMA does not map to its verified original raw span")
    original = raw[start:end]
    final = patched[start:end]
    expected = bytearray(original)
    heap_relative = INITIAL_HEAP_WORD - DATA_LMA_START
    plan_old = struct.unpack_from("<I", original, heap_relative)[0]
    new_heap = struct.unpack_from("<I", patched, heap)[0]
    struct.pack_into("<I", expected, heap_relative, new_heap)
    require(final == bytes(expected),
            "initialized-data LMA changed outside the flat heap-break word")
    require(plan_old == 0x20012A04,
            "initialized-data heap-break original value changed")
    require(new_heap == expected_heap,
            "preserved .data heap word does not equal the moved BSS/heap end")
    return {"lma_start": DATA_LMA_START, "lma_end_exclusive": DATA_LMA_END,
            "bytes": len(original), "original_sha256": sha256(original),
            "final_sha256": sha256(final), "heap_word_address": INITIAL_HEAP_WORD,
            "heap_word_old": plan_old, "heap_word_new": new_heap,
            "other_bytes_preserved": True}


def build_lma_suffix(raw: bytes, raw_base: int, packed_suffix: bytes,
                     code_lma: int, code: bytes,
                     helper_start: int, helper: bytes) -> tuple[bytes, list[dict[str, Any]]]:
    suffix_raw_offset = SUFFIX_FLASH_START - raw_base
    result = bytearray(raw)
    ranges: list[dict[str, Any]] = []
    packed_end = suffix_raw_offset + len(packed_suffix)
    code_offset = code_lma - raw_base
    helper_offset = helper_start - raw_base
    require(code_offset >= packed_end and helper_offset >= code_offset + len(code),
            "packed suffix, code and helper LMA spans overlap")
    require(helper_start + len(helper) <= SUFFIX_FLASH_END,
            "LZ/code/helper payload exceeds owned second-copy span")

    def write_span(offset: int, content: bytes, label: str) -> None:
        require(0 <= offset and offset + len(content) <= len(result),
                f"{label} leaves pinned raw image")
        result[offset:offset + len(content)] = content
        ranges.append({"start": offset, "end_exclusive": offset + len(content),
                       "label": label})

    write_span(suffix_raw_offset, packed_suffix, "packed patched RAM-copy suffix")
    if code_offset > packed_end:
        write_span(packed_end, b"\xff" * (code_offset - packed_end),
                   "alignment after packed suffix")
    write_span(code_offset, code, "uncompressed RAM code source")
    code_end = code_offset + len(code)
    if helper_offset > code_end:
        write_span(code_end, b"\xff" * (helper_offset - code_end),
                   "alignment before flash helpers")
    write_span(helper_offset, helper, "flash restore helpers and shared LZ decoder")
    return bytes(result), ranges


def verify_changed_bytes(original: bytes, patched: bytes,
                         ranges: list[dict[str, Any]]) -> dict[str, Any]:
    require(len(original) == len(patched), "raw image size changed")
    changed = {index for index, pair in enumerate(zip(original, patched))
               if pair[0] != pair[1]}
    covered = set()
    for item in ranges:
        start, end = item["start"], item["end_exclusive"]
        require(0 <= start <= end <= len(original),
                f"approved range {item['label']} is outside raw image")
        covered.update(range(start, end))
    require(changed <= covered,
            "raw image differs outside the explicit recipe/payload spans")
    return {"changed_raw_bytes": len(changed),
            "approved_range_count": len(ranges),
            "all_changes_covered": True,
            "unchanged_outside_ranges": True}


def build_cpu_test(zig: Path, env: dict[str, str], output_dir: Path,
                   packed_suffix: bytes, patched_suffix: bytes,
                   code: bytes, profile: dict[str, Any]) -> dict[str, Any]:
    cpu_dir = output_dir / "cpu"
    cpu_dir.mkdir(parents=True, exist_ok=True)
    payload_dir = cpu_dir / "inputs"
    payload_dir.mkdir(parents=True, exist_ok=True)
    inputs = {
        "packed_suffix.bin": packed_suffix,
        "expected_suffix.bin": patched_suffix,
        "code_source.bin": code,
        "expected_code.bin": code,
    }
    for name, content in inputs.items():
        (payload_dir / name).write_bytes(content)
    input_bundle = b"".join(inputs.values())
    input_bundle_sha256 = sha256(input_bundle)

    input_asm = cpu_dir / "cpu_inputs.S"
    # Zig's assembly cache does not track .incbin dependencies. Make their
    # contents part of the assembly source and verify the linked bytes below.
    input_asm.write_text(
        f".equ dp104_cpu_inputs_{input_bundle_sha256}, 0\n" + """\
    .section .rodata.dp104_cpu_inputs,"a",@progbits
    .balign 4
    .globl dp104_cpu_packed_suffix
dp104_cpu_packed_suffix:
    .incbin "build/firmware/master-payload/cpu/inputs/packed_suffix.bin"
    .globl dp104_cpu_expected_suffix
dp104_cpu_expected_suffix:
    .incbin "build/firmware/master-payload/cpu/inputs/expected_suffix.bin"
    .globl dp104_cpu_code_source
dp104_cpu_code_source:
    .incbin "build/firmware/master-payload/cpu/inputs/code_source.bin"
    .globl dp104_cpu_expected_code
dp104_cpu_expected_code:
    .incbin "build/firmware/master-payload/cpu/inputs/expected_code.bin"
""", encoding="ascii")

    test_c = cpu_dir / "master_restore_cpu.c"
    test_c.write_text(f"""#include <stddef.h>
#include <stdint.h>
#include \"dp104_lz.h\"

#define SUFFIX_BYTES {len(patched_suffix)}u
#define PACKED_SUFFIX_BYTES {len(packed_suffix)}u
#define CODE_BYTES {len(code)}u

extern const uint8_t dp104_cpu_packed_suffix[];
extern const uint8_t dp104_cpu_expected_suffix[];
extern const uint8_t dp104_cpu_code_source[];
extern const uint8_t dp104_cpu_expected_code[];
extern uint8_t dp104_cpu_suffix_destination[];
extern uint8_t dp104_cpu_code_destination[];
extern int dp104_test_restore_suffix(void);
extern void dp104_test_copy_code(void);

static uintptr_t read_sp(void)
{{
    uintptr_t value;
    __asm__ volatile (\"mv %0, sp\" : \"=r\"(value));
    return value;
}}

static uintptr_t read_gp(void)
{{
    uintptr_t value;
    __asm__ volatile (\"mv %0, gp\" : \"=r\"(value));
    return value;
}}

static int cpu_restore_and_check(void)
{{
    size_t i;
    uintptr_t gp_before, gp_after, sp_before, sp_after;
    if (sizeof(void *) != 4u) return 1;
    gp_before = read_gp();
    sp_before = read_sp();
    dp104_cpu_suffix_destination[0] = 0xa5;
    dp104_cpu_suffix_destination[SUFFIX_BYTES + 1u] = 0x5a;
    if (!dp104_test_restore_suffix()) return 2;
    gp_after = read_gp();
    sp_after = read_sp();
    if (gp_before != gp_after || sp_before != sp_after) return 3;
    if (dp104_cpu_suffix_destination[0] != 0xa5 ||
        dp104_cpu_suffix_destination[SUFFIX_BYTES + 1u] != 0x5a)
        return 4;
    for (i = 0; i < SUFFIX_BYTES; ++i)
        if (dp104_cpu_suffix_destination[i + 1u] != dp104_cpu_expected_suffix[i])
            return 5;

    dp104_cpu_code_destination[0] = 0x3c;
    dp104_cpu_code_destination[CODE_BYTES + 1u] = 0xc3;
    /* Synthetic wrapper uses a local bounded copier; privileged CSR replay is
     * verified statically from the target helper and is not executed here. */
    gp_before = read_gp();
    sp_before = read_sp();
    dp104_test_copy_code();
    if (read_gp() != gp_before || read_sp() != sp_before) return 6;
    if (dp104_cpu_code_destination[0] != 0x3c ||
        dp104_cpu_code_destination[CODE_BYTES + 1u] != 0xc3)
        return 7;
    for (i = 0; i < CODE_BYTES; ++i)
        if (dp104_cpu_code_destination[i + 1u] != dp104_cpu_expected_code[i])
            return 8;
    return 0;
}}

uint8_t dp104_cpu_suffix_destination[SUFFIX_BYTES + 2u];
uint8_t dp104_cpu_code_destination[CODE_BYTES + 2u];

__attribute__((noreturn, used)) void _start(void)
{{
    register long status __asm__(\"a0\") = cpu_restore_and_check();
    register long syscall_number __asm__(\"a7\") = 93;
    __asm__ volatile (\"ecall\" : : \"r\"(status), \"r\"(syscall_number) : \"memory\");
    __builtin_unreachable();
}}
""", encoding="ascii")

    cpu_helpers = cpu_dir / "cpu_helpers.S"
    cpu_helpers.write_text(f"""\
    .option push
    .option norvc
    .option norelax
    .section .text.dp104_test_restore_suffix,"ax",@progbits
    .globl dp104_test_restore_suffix
dp104_test_restore_suffix:
    addi sp, sp, -16
    sw ra, 12(sp)
    la a0, dp104_cpu_packed_suffix
    li a1, {len(packed_suffix)}
    la a2, dp104_cpu_suffix_destination
    addi a2, a2, 1
    li a3, {len(patched_suffix)}
    call dp104_lz_decode
    lw ra, 12(sp)
    addi sp, sp, 16
    ret

    .section .text.dp104_test_copy_code,"ax",@progbits
    .globl dp104_test_copy_code
dp104_test_copy_code:
    addi sp, sp, -16
    sw ra, 12(sp)
    la a0, dp104_cpu_code_destination
    addi a0, a0, 1
    la a1, dp104_cpu_code_source
    li a2, {len(code)}
    call dp104_test_copy_bytes
    lw ra, 12(sp)
    addi sp, sp, 16
    ret

    .section .text.dp104_test_copy_bytes,"ax",@progbits
    .globl dp104_test_copy_bytes
dp104_test_copy_bytes:
    beq a2, zero, .Ldp104_test_copy_done
.Ldp104_test_copy_loop:
    lbu t0, 0(a1)
    sb t0, 0(a0)
    addi a0, a0, 1
    addi a1, a1, 1
    addi a2, a2, -1
    bne a2, zero, .Ldp104_test_copy_loop
.Ldp104_test_copy_done:
    ret
    .option pop
""", encoding="ascii")

    start_asm = cpu_dir / "cpu_start.S"
    start_asm.write_text("""\
    .option push
    .option norvc
    .option norelax
    .section .text.start,"ax",@progbits
    .globl dp104_cpu_start
    .type dp104_cpu_start,@function
dp104_cpu_start:
    la sp, __dp104_cpu_stack_top
    li gp, 0x2000e1b0
    call _start
    .Lcpu_halt:
    jal zero, .Lcpu_halt
""", encoding="ascii")

    linker = cpu_dir / "cpu_layout.ld"
    linker.write_text(f"""ENTRY(dp104_cpu_start)
PHDRS
{{
  test_rx PT_LOAD FLAGS(5);
  test_rw PT_LOAD FLAGS(6);
}}
SECTIONS
{{
  . = 0x{CPU_VMA_START:x};
  .text : {{ *(.text.start) *(.text .text.*) }} :test_rx
  .rodata : {{ *(.rodata .rodata.*) *(.srodata .srodata.*) }} :test_rx
  . = ALIGN(0x1000);
  .data : {{ *(.data .data.* .sdata .sdata.*) }} :test_rw
  .bss (NOLOAD) : {{ *(.bss .bss.* .sbss .sbss.*) *(COMMON) }} :test_rw
  . = ALIGN(16);
  .test_stack (NOLOAD) :
  {{
    __dp104_cpu_stack_bottom = .;
    . += 0x4000;
    __dp104_cpu_stack_top = .;
  }} :test_rw
  /DISCARD/ : {{ *(.comment) *(.note .note.*) *(.eh_frame .eh_frame.*) }}
}}
ASSERT(ADDR(.text) >= 0x{CPU_VMA_START:x}, "CPU test text outside synthetic range")
ASSERT(ADDR(.test_stack) + SIZEOF(.test_stack) <= 0x{CPU_VMA_END:x}, "CPU test exceeds synthetic VMA")
ASSERT(SIZEOF(.data) == 0, "CPU restore test unexpectedly has initialized state")
""", encoding="ascii")

    output = cpu_dir / "master_restore_cpu.elf"
    command = [
        str(zig), "cc", "-target", profile["linux_triple"], "-std=c99", "-Oz",
        "-g0", "-ffreestanding", "-fno-builtin", "-fno-stack-protector",
        "-fno-common", "-fno-pic", "-fno-pie", "-fno-unwind-tables",
        "-fno-asynchronous-unwind-tables", "-ffunction-sections", "-fdata-sections",
        "-nostdlib", "-static", "-no-pie", "-Wall", "-Wextra", "-Werror",
        "-Isrc/firmware/freestanding", "-Isrc/firmware", "-Iinclude",
        *profile["architecture_flags"], str(ROOT / "src/firmware/dp104_lz.c"),
        str(test_c), str(input_asm), str(cpu_helpers), str(start_asm), "-Wl,-T," + str(linker),
        "-Wl,-e,dp104_cpu_start", "-Wl,--build-id=none", "-Wl,--no-undefined",
        "-o", str(output),
    ]
    build_result = run(command, env)
    elf = inspect_elf(output.resolve())
    _, linked_rodata = section_bytes(output.resolve(), elf, ".rodata")
    require(input_bundle in linked_rodata,
            "CPU test linked stale or altered .incbin input bytes")
    check_machine_abi(elf, profile, "synthetic master restore CPU test")
    require(elf["elf_type"] == 2 and not elf["undefined_symbols"] and
            not elf["has_interpreter"],
            "synthetic restore CPU test ELF is not fully static")
    require(elf["entry"] >= CPU_VMA_START and elf["entry"] < CPU_VMA_END,
            "synthetic restore CPU test entry is outside its VMA")
    require(all(CPU_VMA_START <= item["virtual_address"] and
                item["virtual_address"] + item["memory_size"] <= CPU_VMA_END
                for item in elf["program_headers"]),
            "synthetic CPU-test PT_LOAD escapes its bounded address map")
    require(any(item["flags"] == 5 for item in elf["program_headers"]) and
            any(item["flags"] == 6 for item in elf["program_headers"]),
            "CPU-test ELF lacks separated read-only code and writable memory")
    wsl_elf = to_wsl_path(output)
    qemu_command = ["wsl.exe", "-d", "Ubuntu", "--", profile["qemu"], wsl_elf]
    qemu_result = run(qemu_command, env, timeout=60)
    require(qemu_result.returncode == 0,
            f"synthetic restore CPU test failed with exit {qemu_result.returncode}: "
            f"{qemu_result.stdout} {qemu_result.stderr}")
    return {"status": "pass", "elf": output.relative_to(ROOT).as_posix(),
            "elf_sha256": sha256_file(output), "elf_machine": elf["machine"],
            "elf_flags": elf["flags"], "entry": elf["entry"],
            "load_segments": elf["program_headers"],
            "input_bundle_sha256": input_bundle_sha256,
            "input_bundle_bytes": len(input_bundle),
            "linked_input_bundle_matches": True,
            "qemu_command": qemu_command,
            "qemu_exit_code": qemu_result.returncode,
            "compile_stdout": build_result.stdout.strip(),
            "test_contract": (
                "Actual LZ decoder restores exact patched 52,208-byte suffix; a "
                "bounded synthetic copy restores exact linked RAM code. Both "
                "destinations have canaries; wrapper GP/SP are checked. Target "
                "privileged CSR replay is not executed and is verified statically."),
            "hardware_accessed": False}


def create_private_sources(output_dir: Path, raw: bytes,
                           code_start: int, state_start: int) -> tuple[Path, Path, Path]:
    private = output_dir / "private"
    private.mkdir(parents=True, exist_ok=True)
    prologue = read_ram_window(raw, RAW_HID_SITE, RAW_HID_BYTES)[1]
    entries = private / "master_entries.S"
    entries.write_text(entry_assembly(), encoding="ascii")
    legacy = private / "master_legacy.S"
    legacy.write_text(host_legacy_assembly(prologue), encoding="ascii")
    script = private / "master_payload.ld"
    script.write_text(linker_script(code_start, state_start), encoding="ascii")
    return entries, legacy, script


def verify_original_windows(raw: bytes, ramcopy: dict[str, Any],
                            pack_report: dict[str, Any]) -> dict[str, Any]:
    data_copy_offset, data_copy = read_ram_window(raw, DATA_COPY_SITE, DATA_COPY_BYTES)
    copy_slot_offset, copy_slot = read_ram_window(raw, COPY_SHIM_SITE, COPY_SHIM_BYTES)
    post_offset, post_bss = read_ram_window(raw, POST_BSS_SITE, POST_BSS_BYTES)
    hid_offset, hid = read_ram_window(raw, RAW_HID_SITE, RAW_HID_BYTES)
    require(copy_slot.hex() == pack_report["secondCopy"]["copySlot"]["originalHex"],
            "private RAM-copy report bytes differ from pinned source")
    require(sha256(copy_slot) == ramcopy["secondRamCopy"]["copySlot"]["sha256"],
            "public copy-slot SHA differs from pinned bytes")
    require(rv_pair_target(COPY_SHIM_SITE, copy_slot[:8]) == SUFFIX_FLASH_START and
            rv_pair_target(COPY_SHIM_SITE + 8, copy_slot[8:16]) == SUFFIX_RAM_START,
            "original second-copy AUIPC/ADDI source/destination changed")
    require(len(data_copy) == DATA_COPY_BYTES,
            "original initialized-data copy slot length changed")
    original_post = decode_post_bss_setup(post_bss)

    task_offset = image_offset("master", TASK_CALL_SITE, TASK_CALL_BYTES, raw)
    task = raw[task_offset:task_offset + TASK_CALL_BYTES]
    callback_offset = image_offset("master", UART_CALLBACK_PAIR,
                                   UART_CALLBACK_BYTES, raw)
    callback = raw[callback_offset:callback_offset + UART_CALLBACK_BYTES]
    require(rv_call_target(TASK_CALL_SITE, task) == TASK_LEGACY_ENTRY,
            "original task call does not target the reviewed worker")
    require(((struct.unpack_from("<I", task)[0] >> 7) & 31) == 1 and
            ((struct.unpack_from("<I", task, 4)[0] >> 7) & 31) == 1,
            "original task call no longer links through ra")
    require(rv_pair_target(UART_CALLBACK_PAIR, callback) == UART_CALLBACK_OLD and
            ((struct.unpack_from("<I", callback)[0] >> 7) & 31) == 10,
            "original UART constructor no longer builds a0=0x1d722")
    line_offset, line_original = read_ram_window(raw, UART_LINE_SITE, UART_LINE_BYTES)
    require(line_original == UART_LINE_ORIGINAL,
            "original UART line-status read/MRET window changed")
    uart_reference = load_json(ROOT / "firmware/targets/dp104-master-uart.json")
    require(uart_reference["rawSha256"] == EXPECTED_RAW_SHA256 and
            int(uart_reference["lineStatusEntry"], 0) == UART_LINE_SITE and
            uart_reference["lineStatusBytes"] == UART_LINE_ORIGINAL.hex(),
            "UART status register-map evidence belongs to another target")
    rda_offset, rda_original = read_ram_window(raw, UART_RDA_SITE, UART_RDA_BYTES)
    require(rda_original == UART_RDA_ORIGINAL, "original UART fixed-seven RDA entry changed")
    fcr_offset = image_offset("master", UART_FCR_SITE, UART_FCR_BYTES, raw)
    require(raw[fcr_offset:fcr_offset + UART_FCR_BYTES] == UART_FCR_ORIGINAL,
            "original UART FCR trigger ORI changed")
    fcr_init_offset = image_offset("master", UART_FCR_INIT_SITE, UART_FCR_INIT_BYTES, raw)
    require(raw[fcr_init_offset:fcr_init_offset + UART_FCR_INIT_BYTES] == UART_FCR_INIT_ORIGINAL,
            "original UART initial FCR literal changed")
    decode_rvc_host_prologue(hid)
    return {
        "second_copy_slot": {"raw_file_offset": copy_slot_offset,
                             "address": COPY_SHIM_SITE,
                             "length": COPY_SHIM_BYTES,
                             "sha256": sha256(copy_slot),
                             "source_target": SUFFIX_FLASH_START,
                             "ram_target": SUFFIX_RAM_START},
        "initialized_data_copy": {"raw_file_offset": data_copy_offset,
                                  "address": DATA_COPY_SITE,
                                  "length": DATA_COPY_BYTES,
                                  "sha256": sha256(data_copy),
                                  "preserved": True},
        "post_bss_csr_setup": {"raw_file_offset": post_offset,
                               "address": POST_BSS_SITE, **original_post},
        "rawhid_entry": {"raw_file_offset": hid_offset,
                         "address": RAW_HID_SITE,
                         **decode_rvc_host_prologue(hid)},
        "task_call": {"raw_file_offset": task_offset,
                      "original_target": TASK_LEGACY_ENTRY,
                      "bytes_sha256": sha256(task)},
        "uart_line_status": {"raw_file_offset": line_offset, "address": UART_LINE_SITE,
                             "original_hex": line_original.hex()},
        "uart_rda": {"raw_file_offset": rda_offset, "address": UART_RDA_SITE,
                     "original_hex": rda_original.hex()},
        "uart_fcr": {"raw_file_offset": fcr_offset, "address": UART_FCR_SITE,
                     "original_hex": UART_FCR_ORIGINAL.hex()},
        "uart_fcr_initial": {"raw_file_offset": fcr_init_offset, "address": UART_FCR_INIT_SITE,
                             "original_hex": UART_FCR_INIT_ORIGINAL.hex()},
        "uart_callback_constructor": {"raw_file_offset": callback_offset,
                                       "original_target": UART_CALLBACK_OLD,
                                       "bytes_sha256": sha256(callback)},
    }


def apply_recipe_to_plan(raw: bytes, plan: dict[str, Any],
                         heapword_expected: int) -> tuple[list[dict[str, Any]], int]:
    recipes = [item for item in plan["reservationRecipesNotApplied"]
               if item["role"] == "master"]
    require(len(recipes) == 3, "expected exactly three master heap/BSS recipes")
    expected = {BSS_END_PAIR, HEAP_FLOOR_PAIR, INITIAL_HEAP_WORD}
    require({int(item["runtimeAddress"], 0) for item in recipes} == expected,
            "master reservation recipe addresses changed")
    new_end = int(plan["reservations"]["master"]["newBssEnd"], 0)
    if new_end >= INITIAL_SP - SP_GUARD_BYTES:
        raise MasterPayloadError("master reservation reaches the stock SP-0x40 guard")
    for item in recipes:
        address = int(item["runtimeAddress"], 0)
        require(bytes.fromhex(item["originalHex"]) ==
                raw[int(item["imageOffset"]):int(item["imageOffset"]) +
                    len(bytes.fromhex(item["originalHex"]))],
                f"planner recipe source differs at 0x{address:x}")
        replacement = bytes.fromhex(item["replacementHex"])
        if address == INITIAL_HEAP_WORD:
            require(bytes.fromhex(item["originalHex"]) == struct.pack("<I", heapword_expected) and
                    replacement == struct.pack("<I", new_end),
                    "initialized heap-word recipe differs from measured new BSS end")
        else:
            require(rv_pair_target(address, replacement) == new_end,
                    f"planner AUIPC/ADDI replacement differs at 0x{address:x}")
        if address == INITIAL_HEAP_WORD:
            require(item.get("applyBeforeInitdataPacking") is True,
                    "heap-word recipe lost its original-data ordering contract")
    return recipes, new_end


def apply_payload_patches(raw: bytes, raw_base: int, plan: dict[str, Any],
                          windows: dict[str, bytes], packed_suffix: bytes,
                          code: bytes, code_lma: int, helper_start: int,
                          helper: bytes) -> tuple[bytes, list[dict[str, Any]], list[dict[str, Any]]]:
    patched = bytearray(raw)
    approved: list[dict[str, Any]] = []
    patches: list[dict[str, Any]] = []

    for recipe, new_end in ((item, int(plan["reservations"]["master"]["newBssEnd"], 0))
                            for item in plan["reservationRecipesNotApplied"]
                            if item["role"] == "master"):
        address = int(recipe["runtimeAddress"], 0)
        if address == INITIAL_HEAP_WORD:
            old = bytes.fromhex(recipe["originalHex"])
            new = struct.pack("<I", new_end)
            purpose = "move initialized heap break in preserved .data"
        else:
            old = bytes.fromhex(recipe["originalHex"])
            new = bytes.fromhex(recipe["replacementHex"])
            purpose = recipe["purpose"]
        offset = int(recipe["imageOffset"])
        patches.append(apply_raw_patch(raw, patched, offset, old, new,
                                       purpose, approved))

    for section_name, address, size, label in (
        (".copy_shim", COPY_SHIM_SITE, COPY_SHIM_BYTES, "second-copy startup shim"),
        (".post_bss", POST_BSS_SITE, POST_BSS_BYTES, "post-BSS code-copy shim"),
        (".taskcall", TASK_CALL_SITE, TASK_CALL_BYTES, "task-hook call pair"),
        (".uart_callback", UART_CALLBACK_PAIR, UART_CALLBACK_BYTES,
         "UART callback constructor pair"),
        (".uart_fcr", UART_FCR_SITE, UART_FCR_BYTES, "UART receive trigger bits"),
        (".uart_fcr_init", UART_FCR_INIT_SITE, UART_FCR_INIT_BYTES, "UART initial FIFO trigger literal"),
    ):
        offset = image_offset("master", address, size, raw)
        before = raw[offset:offset + size]
        after = windows[section_name]
        if section_name == ".uart_fcr":
            require(before == UART_FCR_ORIGINAL and after == UART_FCR_REPLACEMENT,
                    "UART receive trigger source or replacement changed before apply")
        if section_name == ".uart_fcr_init":
            require(before == UART_FCR_INIT_ORIGINAL and after == UART_FCR_INIT_REPLACEMENT,
                    "UART receive trigger initial literal changed before apply")
        patches.append(apply_raw_patch(raw, patched, offset, before, after,
                                       label, approved))

    lma_patched, payload_ranges = build_lma_suffix(
        bytes(patched), raw_base, packed_suffix, code_lma, code,
        helper_start, helper)
    patched = bytearray(lma_patched)
    approved.extend(payload_ranges)
    return bytes(patched), patches, approved


def build_cpu_inputs(output_dir: Path, packed_suffix: bytes,
                     patched_suffix: bytes, code: bytes) -> None:
    cpu_inputs = output_dir / "cpu" / "inputs"
    cpu_inputs.mkdir(parents=True, exist_ok=True)
    for name, data in (("packed_suffix.bin", packed_suffix),
                       ("expected_suffix.bin", patched_suffix),
                       ("code_source.bin", code),
                       ("expected_code.bin", code)):
        (cpu_inputs / name).write_bytes(data)


def run_build(archive_path: Path, zig_path: Path,
              measure_only: bool = False) -> dict[str, Any]:
    require(archive_path.is_file(), f"pinned archive not found: {archive_path}")
    start_sources = source_snapshot()
    verify_frozen_inputs(start_sources)
    target_manifest, raw, original_uf2, plc_raw = load_original_inputs(archive_path)
    raw_base = int(target_manifest["master"]["base"], 0)
    ramcopy_manifest = load_json(ROOT / "firmware/targets/dp104-master-ramcopy.json")
    ram_pack_report = load_json(ROOT / ".local/master-ram-pack-feasibility.json")
    ramcopy = verify_ramcopy_evidence(raw, ramcopy_manifest,
                                     ram_pack_report, raw_base)
    data_report = load_json(ROOT / ".local/master-data-pack-feasibility.json")
    require(data_report.get("RawImageSha256") == EXPECTED_RAW_SHA256,
            "master initialized-data evidence belongs to a different raw image")
    data_span = data_report["InitializedDataSpan"]["FlashLma"]
    require(int(data_span["Start"], 0) == DATA_LMA_START and
            int(data_span["EndExclusive"], 0) == DATA_LMA_END,
            "initialized-data span differs from preserved original span")
    original_windows = verify_original_windows(raw, ramcopy_manifest,
                                               ram_pack_report)
    memops = verify_stock_memops(raw, raw_base)
    replay_bytes = read_ram_window(raw, POST_BSS_SITE, POST_BSS_BYTES)[1]
    replay_words = struct.unpack("<II", replay_bytes)
    decode_post_bss_setup(replay_bytes)

    zig, env = prepare_env(zig_path)
    profile = PROFILES["master"]
    layout = measure_layout(zig, env)
    initial_plan = make_plan({"master": raw, "plc": plc_raw}, layout, 0)
    state_start = int(initial_plan["reservations"]["master"]["stateStart"], 0)
    code_start = int(initial_plan["reservations"]["master"]["stateEnd"], 0)
    require(state_start == STATE_START,
            f"measured master state starts at 0x{state_start:x}, expected {STATE_START:#x}")
    require(int(initial_plan["reservations"]["master"]["initialStackTopUnchanged"], 0) == INITIAL_SP,
            "planner changed the stock initial stack top")

    OUTPUT_ROOT.mkdir(parents=True, exist_ok=True)
    code_objects, code_compile = compile_payload_objects(
        zig, env, OUTPUT_ROOT, raw, profile)
    code_linker = OUTPUT_ROOT / "private" / "master_code_layout.ld"
    code_linker.write_text(linker_script(code_start, state_start), encoding="ascii")
    code_elf_path = OUTPUT_ROOT / "master_code_measure.elf"
    code_link = link_payload(zig, env, code_objects, code_linker,
                             code_elf_path, profile)
    code_elf, code, code_windows = validate_payload_elf(
        code_elf_path, code_start, None)
    code_entry_checks = validate_entry_windows(code_elf, code_start, code,
                                               code_windows, None)
    code_path = OUTPUT_ROOT / "master_code.bin"
    code_path.write_bytes(code)

    plan = make_plan({"master": raw, "plc": plc_raw}, layout, len(code))
    recipes, new_bss_end = apply_recipe_to_plan(raw, plan, 0x20012A04)
    reservation = plan["reservations"]["master"]
    require(int(reservation["reservedCodeStart"], 0) == code_start and
            int(reservation["reservedCodeBytes"]) == len(code) and
            code_start + len(code) <= new_bss_end,
            "planner reservation does not cover the linked code extent")

    rawhid_patch = code_windows[".rawhid_entry"]
    packed_suffix, patched_suffix, packed_report = pack_suffix(
        raw, raw_base, rawhid_patch, code_windows[".version_entry"], code_windows[".uart_line_entry"],
        code_windows[".uart_rda_entry"])
    layout_report = pack_layout(packed_suffix, code)
    code_lma = layout_report["code_lma"]
    helper_start = layout_report["helper_lma"]
    helper_source = generate_helper_source(OUTPUT_ROOT, len(packed_suffix),
                                           code_lma, len(code), code_start,
                                           replay_words)
    helper_object = OUTPUT_ROOT / "objects" / "master_helpers.o"
    helper_compile = compile_object(zig, env, helper_source,
                                    helper_object, profile)
    lz_object = OUTPUT_ROOT / "objects" / "dp104_lz.o"
    lz_compile = compile_object(zig, env, ROOT / "src/firmware/dp104_lz.c",
                                lz_object, profile)
    final_linker = OUTPUT_ROOT / "private" / "master_payload_layout.ld"
    final_linker.write_text(linker_script(code_start, state_start, helper_start),
                            encoding="ascii")
    final_elf_path = OUTPUT_ROOT / "master_payload.elf"
    final_objects = code_objects + [lz_object, helper_object]
    final_link = link_payload(zig, env, final_objects, final_linker,
                              final_elf_path, profile)
    final_elf, final_code, final_windows = validate_payload_elf(
        final_elf_path, code_start, helper_start)
    require(final_code == code,
            "adding flash wrappers changed the already-measured RAM code bytes")
    entry_checks = validate_entry_windows(final_elf, code_start, final_code,
                                          final_windows, helper_start,
                                          len(packed_suffix), code_lma,
                                          replay_words)
    helper = final_windows[".flash_helpers"]
    verify_helper_lma(final_elf, helper_start, len(helper))
    require(helper_start + len(helper) <= SUFFIX_FLASH_END,
            f"restore helper end {helper_start + len(helper):#x} exceeds owned span")
    remaining = SUFFIX_FLASH_END - (helper_start + len(helper))
    fit = {**layout_report, "helper_bytes": len(helper),
           "helper_end": helper_start + len(helper),
           "remaining_owned_span_bytes": remaining,
           "fits_owned_second_copy_span": remaining >= 0}
    require(fit["fits_owned_second_copy_span"],
            "packed suffix, uncompressed RAM code and restore helpers do not fit")

    if measure_only:
        end_sources = source_snapshot()
        require(start_sources == end_sources,
                "firmware/source/manifest inputs changed during fit measurement")
        return {
            "schema_version": 1,
            "status": "fit_measurement_pass",
            "purpose": "review-only master suffix/code/helper fit measurement",
            "boundary": {"hardware_accessed": False,
                         "flash_or_bootloader_commands": False,
                         "candidate_emitted": False},
            "source_hashes": {"before": start_sources, "after": end_sources,
                              "unchanged": True},
            "ramcopy_evidence": ramcopy,
            "original_windows": original_windows,
            "stock_memops": memops,
            "planner": {"state_start": state_start,
                        "state_bytes": layout["master"]["bytes"],
                        "state_end": code_start, "code_bytes": len(code),
                        "new_bss_and_heap_end": new_bss_end,
                        "stock_stack_top": INITIAL_SP},
            "lz": packed_report,
            "payload_layout": fit,
            "linked_payload": {"elf": final_link["path"],
                               "elf_sha256": final_link["sha256"],
                               "machine": final_elf["machine"],
                               "flags": final_elf["flags"],
                               "entry": final_elf["entry"],
                               "segments": final_elf["program_headers"],
                               "undefined_symbols": final_elf["undefined_symbols"],
                               "entry_checks": entry_checks},
            "candidate_emitted": False,
        }

    copy_slot_offset, _old_copy_slot = read_ram_window(raw, COPY_SHIM_SITE,
                                                        COPY_SHIM_BYTES)
    _post_offset, _old_post = read_ram_window(raw, POST_BSS_SITE, POST_BSS_BYTES)
    patched_raw, flat_patches, approved_ranges = apply_payload_patches(
        raw, raw_base, plan, final_windows, packed_suffix,
        final_code, code_lma, helper_start, helper)
    data_preservation = verify_data_lma_preserved(raw, patched_raw, raw_base,
                                                   new_bss_end)
    for recipe in recipes:
        offset = int(recipe["imageOffset"])
        replacement = bytes.fromhex(recipe["replacementHex"])
        require(patched_raw[offset:offset + len(replacement)] == replacement,
                f"final candidate is missing recipe at {recipe['runtimeAddress']}")
    changes = verify_changed_bytes(raw, patched_raw, approved_ranges)
    patched_uf2, uf2_info = rebuild_uf2(
        original_uf2, raw, patched_raw, target_manifest["master"])
    require(parse_uf2(patched_uf2, {
        **target_manifest["master"], "rawSha256": sha256(patched_raw)
    }) == patched_raw,
        "candidate UF2 does not roundtrip to exact patched raw bytes")

    cpu_test = build_cpu_test(zig, env, OUTPUT_ROOT, packed_suffix,
                              patched_suffix, final_code, profile)
    end_sources = source_snapshot()
    require(start_sources == end_sources,
            "firmware/source/manifest inputs changed during payload build")

    # Candidate files are written only after source, ELF, fit, patch-range,
    # preserved-data, UF2 roundtrip, and synthetic CPU checks all pass.
    candidate_raw_path = OUTPUT_ROOT / "patched-master.bin"
    candidate_uf2_path = OUTPUT_ROOT / "patched-master.uf2"
    candidate_raw_path.write_bytes(patched_raw)
    candidate_uf2_path.write_bytes(patched_uf2)
    return {
        "schema_version": 1,
        "status": "pass_review_only_candidate",
        "compatibility_version": RELEASE_VERSION,
        "purpose": "review-only local 8K master RAM-copy suffix payload",
        "boundary": {"hardware_accessed": False, "flash_or_bootloader_commands": False,
                     "installed_firmware_modified": False, "source_firmware_modified": False,
                     "bootloader_acceptance_verified": False,
                     "physical_runtime_or_led_behavior_verified": False,
                     "candidate_is_review_only": True},
        "archive": {"path": str(archive_path), "sha256": sha256_file(archive_path),
                    "member": target_manifest["master"]["archiveEntry"],
                    "original_uf2_sha256": sha256(original_uf2),
                    "original_raw_sha256": sha256(raw),
                    "candidate_raw_sha256": sha256(patched_raw),
                    "candidate_uf2_sha256": sha256(patched_uf2)},
        "source_hashes": {"before": start_sources, "after": end_sources,
                          "unchanged": True},
        "ramcopy_evidence": ramcopy,
        "original_windows": original_windows,
        "stock_memops": memops,
        "planner": {"state_start": state_start, "state_bytes": layout["master"]["bytes"],
                    "state_end": code_start, "code_bytes": len(code),
                    "new_bss_and_heap_end": new_bss_end,
                    "stock_stack_top": INITIAL_SP,
                    "all_three_master_recipes": [
                        {"address": int(item["runtimeAddress"], 0),
                         "raw_file_offset": int(item["imageOffset"]),
                         "original_hex": item["originalHex"],
                         "replacement_hex": item["replacementHex"],
                         "purpose": item["purpose"]}
                        for item in recipes]},
        "lz": packed_report,
        "payload_layout": fit,
        "linked_payload": {"elf": final_link["path"],
                           "elf_sha256": final_link["sha256"],
                           "machine": final_elf["machine"],
                           "flags": final_elf["flags"],
                           "entry": final_elf["entry"],
                           "segments": final_elf["program_headers"],
                           "undefined_symbols": final_elf["undefined_symbols"],
                           "data_bss_got_bytes": 0,
                           "compile_objects": code_compile + [lz_compile, helper_compile],
                           "link_command": final_link["command"],
                           "entry_checks": entry_checks},
        "preserved_initialized_data": data_preservation,
        "flat_recipe_patches": flat_patches,
        "changed_byte_whitelist": changes,
        "approved_raw_ranges": approved_ranges,
        "uf2_rebuild": uf2_info,
        "synthetic_cpu_test": cpu_test,
        "candidate_files": {"raw": str(candidate_raw_path.relative_to(ROOT)),
                            "uf2": str(candidate_uf2_path.relative_to(ROOT))},
        "evidence_limits": [
            "The CPU test runs the project LZ decoder and a synthetic bounded code copy only.",
            "The target CSR replay is statically checked and is not executed in Linux-user QEMU.",
            "Static RAM-copy reference scanning is bounded by the supplied private evidence and does not exclude indirect aliases.",
            "This does not verify bootloader acceptance, physical device behavior, or recovery after a write.",
        ],
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--archive", type=Path, default=DEFAULT_ARCHIVE)
    parser.add_argument("--zig", type=Path, default=DEFAULT_ZIG)
    parser.add_argument("--measure-only", action="store_true",
                        help="link and validate fit but do not write candidate BIN/UF2 files")
    args = parser.parse_args()
    report_path = OUTPUT_ROOT / ("fit-report.json" if args.measure_only else "report.json")
    report: dict[str, Any] = {"schema_version": 1, "status": "in_progress"}
    try:
        report = run_build(args.archive.resolve(), args.zig.resolve(),
                           measure_only=args.measure_only)
    except (MasterPayloadError, RestoreBuildError, OSError, ValueError,
            subprocess.SubprocessError) as error:
        report.update({"status": "failed", "error": str(error),
                       "candidate_emitted": False})
        OUTPUT_ROOT.mkdir(parents=True, exist_ok=True)
        report_path.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
        print(f"master payload build failed: {error}", file=sys.stderr)
        return 1
    report["candidate_emitted"] = not args.measure_only
    report_path.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({"status": report["status"],
                      "code_bytes": report["planner"]["code_bytes"],
                      "packed_suffix_bytes": report["lz"]["packed_bytes"],
                      "helper_bytes": report["payload_layout"]["helper_bytes"],
                      "remaining_owned_span_bytes": report["payload_layout"]["remaining_owned_span_bytes"],
                      "candidate_emitted": not args.measure_only,
                      "report": str(report_path.relative_to(ROOT))}, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
