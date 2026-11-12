"""Audit pinned 8K hook windows and propose explicit BSS reservations.

Reads original UF2 inputs and cross-compiles a storage-layout descriptor. Emits
only a review plan and compiler objects under build/firmware/hook-plan. It does
not modify firmware, emit UF2, choose extension code space, or access hardware.
Reservation recipes are not proof of sufficient stack/heap headroom.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import struct
import subprocess
import zipfile
from pathlib import Path

from build_firmware_objects import inspect_elf
from verify_target import TARGET, parse_uf2, require, validate_archive

ROOT = Path(__file__).resolve().parents[1]
OUTPUT = ROOT / "build/firmware/hook-plan"
DEFAULT_ZIG = ROOT / ".local/toolchain/zig-x86_64-windows-0.15.2/zig.exe"


def align_up(value, alignment):
    require(alignment > 0 and not alignment & (alignment - 1), "Bad alignment")
    return (value + alignment - 1) & -alignment


def rv_pair_target(pc, pair):
    require(len(pair) == 8, "Expected two RV32 instructions")
    upper, lower = struct.unpack("<II", pair)
    require(upper & 0x7f == 0x17, "Expected AUIPC")
    rd = (upper >> 7) & 31
    require(lower & 0x707f == 0x13 and (lower >> 7) & 31 == rd and
            (lower >> 15) & 31 == rd, "Expected matching ADDI")
    immediate = lower >> 20
    if immediate & 0x800:
        immediate -= 0x1000
    return (pc + (upper & 0xfffff000) + immediate) & 0xffffffff


def rv_address_pair(pc, target, register):
    require(0 <= pc <= 0xffffffff and 0 <= target <= 0xffffffff,
            "RV32 address out of range")
    require(1 <= register < 32, "Invalid destination register")
    delta = (target - pc) & 0xffffffff
    if delta & 0x80000000:
        delta -= 0x100000000
    high = (delta + 0x800) >> 12
    low = delta - (high << 12)
    require(-2048 <= low <= 2047, "Bad ADDI displacement")
    return struct.pack("<II", ((high & 0xfffff) << 12) | (register << 7) | 0x17,
                       ((low & 0xfff) << 20) | (register << 15) |
                       (register << 7) | 0x13)


def rv_call_target(pc, pair):
    require(len(pair) == 8, "Expected an RV32 AUIPC/JALR pair")
    upper, lower = struct.unpack("<II", pair)
    require(upper & 0x7f == 0x17 and lower & 0x707f == 0x67 and
            (upper >> 7) & 31 == (lower >> 15) & 31,
            "Expected matching AUIPC/JALR")
    low = lower >> 20
    if low & 0x800:
        low -= 0x1000
    return ((pc + (upper & 0xfffff000) + low) & 0xffffffff) & ~1


def image_offset(role, runtime_address, length, raw):
    require(role in ("master", "plc"), "Unknown image role")
    if role == "master" and 0x20000000 <= runtime_address < 0x2000ce00:
        offset = runtime_address - 0x20000000 + 4
        require(runtime_address + length <= 0x2000ce00,
                "Window crosses master RAM-copy boundary")
    else:
        offset = runtime_address - (0x8000 if role == "master" else 0x08008000)
    require(offset >= 0 and length > 0 and offset + length <= len(raw),
            "Hook window outside pinned image")
    return offset


def window(role, address, length, raw, purpose):
    offset = image_offset(role, address, length, raw)
    return {"role": role, "runtimeAddress": hex(address), "imageOffset": offset,
            "storageAddress": hex((0x8000 if role == "master" else 0x08008000) + offset),
            "originalHex": raw[offset:offset + length].hex(), "purpose": purpose}


def replacement(role, address, before, after, raw, purpose):
    require(len(before) == len(after), "Reservation recipe changes image length")
    item = window(role, address, len(before), raw, purpose)
    require(bytes.fromhex(item["originalHex"]) == before,
            "Unexpected reservation initializer")
    item["replacementHex"] = after.hex()
    return item


def run(command, env):
    result = subprocess.run(command, cwd=ROOT, env=env, capture_output=True,
                            text=True, timeout=60, check=False)
    require(result.returncode == 0, result.stderr.strip() or result.stdout.strip() or
            f"Compiler failed: {result.returncode}")
    return result.stdout.strip()


def measure_layout(zig, env):
    source = OUTPUT / "storage-layout.c"
    source.write_text(
        '#include <stddef.h>\n#include "dp104_patch_state.h"\n'
        '__attribute__((used, section(".dp104.layout")))\n'
        'const unsigned dp104_layout[] = {sizeof(void *),\n'
        'sizeof(dp104_master_patch_state), __alignof__(dp104_master_patch_state),\n'
        'sizeof(dp104_plc_patch_state), __alignof__(dp104_plc_patch_state),\n'
        'offsetof(dp104_plc_patch_state, twiddle)};\n',
        encoding="ascii")
    profiles = {
        "master": ["-target", "riscv32-freestanding-none", "-mcpu=generic_rv32+m+c",
                   "-mabi=ilp32", "-msmall-data-limit=0", "-mno-relax"],
        "plc": ["-target", "thumb-freestanding-eabi", "-mcpu=cortex_m3", "-mthumb",
                "-mfloat-abi=soft"],
    }
    results = {}
    for role, flags in profiles.items():
        output = OUTPUT / f"{role}-layout.o"
        command = [str(zig), "cc", *flags, "-std=c99", "-Os", "-g0", "-ffreestanding",
                   "-fno-common", "-fno-pic", "-fno-pie", "-fno-stack-protector",
                   "-Iinclude", "-Isrc/firmware", "-c", str(source), "-o", str(output)]
        run(command, env)
        elf = inspect_elf(output)
        require(elf["machine"] == (243 if role == "master" else 40), "Wrong ELF machine")
        sections = [s for s in elf["sections"] if s["name"] == ".dp104.layout"]
        require(len(sections) == 1 and sections[0]["size"] == 24, "Missing layout descriptor")
        section = sections[0]
        values = struct.unpack_from("<6I", output.read_bytes(), section["offset"])
        require(values[0] == 4, "Expected 32-bit target pointers")
        index = 1 if role == "master" else 3
        require(values[index] > 0 and values[index + 1] in (1, 2, 4, 8, 16),
                "State exceeds the planner's supported alignment contract")
        results[role] = {"bytes": values[index], "alignment": values[index + 1],
                         "objectSha256": hashlib.sha256(output.read_bytes()).hexdigest(),
                         "command": command}
        if role == "plc":
            require(values[5] % 4 == 0 and values[5] + 8192 <= values[index],
                    "Coefficient table is not contained in PLC state")
            results[role]["twiddleOffset"] = values[5]
    return results


def build_trampolines(zig, env, images):
    """Compile displaced entry semantics, without assigning injection addresses."""
    master = images["master"]
    start = image_offset("master", 0x200046ee, 8, master)
    displaced = master[start:start + 8]
    require(hashlib.sha256(displaced).hexdigest() ==
            "83a3d70da866cacfb017c16f03bb5b64141e81df90d42c2b9858ebc1869034d3",
            "Master entry is not the reviewed stack/save-only window")
    plc = images["plc"]
    start = image_offset("plc", 0x0800dd30, 4, plc)
    require(hashlib.sha256(plc[start:start + 4]).hexdigest() ==
            "b12bb5dccb67c881a1c943b9f336f864a795a0d4becff34ca4cd0af4d4a47ab2",
            "PLC setter entry changed")
    literal = image_offset("plc", 0x0800dd64, 4, plc)
    require(struct.unpack_from("<I", plc, literal)[0] == 0x20002348,
            "PLC displaced LDR literal changed")
    sources = {
        "master": '.option norelax\n.option nopic\n.text\n.balign 2\n'
                  '.globl dp104_master_host_legacy\n.type dp104_master_host_legacy,@function\n'
                  'dp104_master_host_legacy:\n.byte ' + ','.join(str(b) for b in displaced) +
                  '\nli t0, 0x200046f6\njr t0\n'
                  '.size dp104_master_host_legacy,.-dp104_master_host_legacy\n',
        "plc": '.syntax unified\n.thumb\n.text\n.balign 2\n'
               '.globl dp104_plc_setter_legacy\n.type dp104_plc_setter_legacy,%function\n'
               '.thumb_func\ndp104_plc_setter_legacy:\n'
               'push {r4,r5,r6,r7,lr}\nldr r5,.Lbuffer\nldr.w pc,.Lresume\n'
               '.balign 4\n.Lbuffer: .word 0x20002348\n.Lresume: .word 0x0800dd35\n'
               '.size dp104_plc_setter_legacy,.-dp104_plc_setter_legacy\n',
    }
    flags = {
        "master": ["-target", "riscv32-freestanding-none", "-mcpu=generic_rv32+m+c",
                   "-mabi=ilp32", "-msmall-data-limit=0", "-mno-relax"],
        "plc": ["-target", "thumb-freestanding-eabi", "-mcpu=cortex_m3", "-mthumb",
                "-mfloat-abi=soft"],
    }
    results = {}
    for role, source in sources.items():
        assembly = OUTPUT / f"{role}-legacy.S"
        assembly.write_text(source, encoding="ascii")
        output = OUTPUT / f"{role}-legacy.o"
        command = [str(zig), "cc", *flags[role], "-g0", "-ffreestanding", "-fno-pic",
                   "-fno-pie", "-c", str(assembly), "-o", str(output)]
        run(command, env)
        elf = inspect_elf(output)
        require(not elf["undefined_global_symbols"] and not elf["relocations"],
                "Unexpected unresolved trampoline symbol or relocation")
        sections = [s for s in elf["sections"] if s["name"] == ".text"]
        require(len(sections) == 1, "Missing trampoline text")
        text = output.read_bytes()[sections[0]["offset"]:][:sections[0]["size"]]
        if role == "master":
            require(text[:8] == displaced, "Master displaced instructions not preserved")
            # Check LUI/ADDI materialization of the absolute resume address.
            require(len(text) == 18, "Unexpected master trampoline encoding")
            upper, lower = struct.unpack_from("<II", text, 8)
            require(upper & 0xfff == 0x2b7, "Expected LUI t0")
            fake_auipc = (upper & ~0x7f) | 0x17
            require(rv_pair_target(0, struct.pack("<II", fake_auipc, lower)) == 0x200046f6,
                    "Wrong master trampoline continuation")
            require(text[-2:] == bytes.fromhex("8282"), "Expected JR t0")
        else:
            require(text[:2] == plc[start:start + 2], "PLC push not preserved")
            require(sections[0]["alignment"] >= 4, "PLC literal pool needs four-byte alignment")
            require(len(text) == 16, "Unexpected PLC trampoline encoding")
            ldr = struct.unpack_from("<H", text, 2)[0]
            require(ldr & 0xff00 == 0x4d00 and 4 + (ldr & 0xff) * 4 == 8,
                    "Wrong relocated LDR r5 target")
            first, second = struct.unpack_from("<HH", text, 4)
            require(first == 0xf8df and second >> 12 == 15 and
                    8 + (second & 0xfff) == 12, "Wrong relocated LDR pc target")
            require(text[-8:] == struct.pack("<II", 0x20002348, 0x0800dd35),
                    "Wrong relocated PLC literals")
        results[role] = {"object": str(output.relative_to(ROOT)),
                         "objectSha256": hashlib.sha256(output.read_bytes()).hexdigest(),
                         "textBytes": len(text), "textAlignment": sections[0]["alignment"],
                         "command": command,
                         "resumeAddress": "0x200046f6" if role == "master" else "0x0800dd35",
                         "codeAddressAssigned": False, "runtimeVerified": False}
    return results


def make_plan(raw_images, layout, master_code_bytes=0):
    master, plc = raw_images["master"], raw_images["plc"]
    require(type(master_code_bytes) is int and master_code_bytes >= 0,
            "Master code reservation must be a nonnegative byte count")
    master_start = align_up(0x20012a04, 16)
    master_state_end = align_up(master_start + layout["master"]["bytes"], 16)
    master_end = align_up(master_state_end + master_code_bytes, 16)
    plc_start = align_up(0x2000cba8, 16)
    plc_end = align_up(plc_start + layout["plc"]["bytes"], 16)
    require(master_end < 0x20020000 - 0x40 and plc_end < 0x20038000,
            "Reservation reaches initial stack top")
    master_pc = 0x200001ba
    original = master[image_offset("master", master_pc, 8, master):][:8]
    require(rv_pair_target(master_pc, original) == 0x20012a04,
            "Master BSS endpoint changed")
    recipes = [replacement("master", master_pc, original,
                           rv_address_pair(master_pc, master_end, 11), master,
                           "Extend existing BSS zero endpoint; no code placement assigned")]
    heap_floor_pc = 0x21a4c
    heap_floor_offset = image_offset("master", heap_floor_pc, 8, master)
    floor_pair = master[heap_floor_offset:heap_floor_offset + 8]
    require(rv_pair_target(heap_floor_pc, floor_pair) == 0x20012a04,
            "Master heap floor changed")
    recipes.append(replacement("master", heap_floor_pc, floor_pair,
                               rv_address_pair(heap_floor_pc, master_end, 14), master,
                               "Move allocator floor together with BSS endpoint"))
    heap_initializer = replacement("master", 0x464b8, struct.pack("<I", 0x20012a04),
                                   struct.pack("<I", master_end), master,
                                   "Move initialized heap-break word (RAM 0x2000da38)")
    heap_initializer["applyBeforeInitdataPacking"] = True
    recipes.append(heap_initializer)
    for address, purpose in ((0x0801a678, "Extend PLC startup BSS zero endpoint"),
                             (0x0800ce84, "Move initial PLC heap above reserved state")):
        recipes.append(replacement("plc", address, struct.pack("<I", 0x2000cba8),
                                   struct.pack("<I", plc_end), plc, purpose))
    twiddle_ram = None
    if "twiddleOffset" in layout["plc"]:
        twiddle_ram = plc_start + layout["plc"]["twiddleOffset"]
        require(twiddle_ram % 4 == 0 and twiddle_ram + 8192 <= plc_end,
                "Reconstructed table exceeds explicit PLC reservation")
        recipes.append(replacement("plc", 0x08018334, struct.pack("<I", 0x0801cc5c),
                                   struct.pack("<I", twiddle_ram), plc,
                                   "Retarget coefficient-table descriptor after pre-application reconstruction"))
    hooks = [window("master", 0x200046ee, 8, master, "RawHID entry; continuation 0x200046f6"),
             window("master", 0x0001be00, 8, master, "Existing task-side worker call; ABI pending"),
             window("master", 0x0001d722, 12, master, "UART ISR callback; relocation pending"),
             window("plc", 0x0800dd30, 4, plc, "Key setter entry; displaced PC-relative LDR needs relocation"),
             window("plc", 0x08013a5e, 4, plc, "Existing receive-queue call; ABI pending"),
             window("plc", 0x08017564, 4, plc, "USART ISR entry redirected to circular RX handler"),
              window("plc", 0x08008080, 4, plc, "Default DMA1 channel6 vector; Thumb handler only"),
              window("plc", 0x080188f6, 4, plc, "RX enable ORR: circular mode plus half/full interrupts"),
              window("plc", 0x08017576, 4, plc, "Adjacent USART SR/DR loads; diagnostic replay must preserve registers and flags"),
             window("plc", 0x0801889a, 4, plc, "RX DMA packed initialization count MOV.W"),
             window("plc", 0x0801757c, 2, plc, "RX DMA empty-count CMP immediate"),
             window("plc", 0x0801758e, 4, plc, "RX DMA callback-length RSB.W immediate"),
             window("plc", 0x08017598, 2, plc, "RX DMA reload-count MOVS immediate"),
             window("plc", 0x080175b0, 4, plc, "RX ISR callback-buffer pointer literal"),
             window("plc", 0x08018930, 4, plc, "RX DMA CMAR buffer pointer literal")]
    call_offset = image_offset("master", 0x1be00, 8, master)
    worker_target = rv_call_target(0x1be00, master[call_offset:call_offset + 8])
    require(worker_target == 0x20001a56, "Existing task worker call changed")
    return {"schema": "dp104-hook-review-v1", "target": "DP104 1.2.0 8K + PLC 1.2.0",
            "patchImplemented": False, "firmwareModified": False, "uf2Emitted": False,
            "hardwareActions": False, "storageLayout": layout,
            "reservations": {
                "master": {"previousBssEnd": hex(0x20012a04), "stateStart": hex(master_start),
                           "stateEnd": hex(master_state_end),
                           "reservedCodeStart": hex(master_state_end) if master_code_bytes else None,
                           "reservedCodeBytes": master_code_bytes,
                           "newBssEnd": hex(master_end),
                           "heapStrategy": "Move BSS end, initialized heap break and allocator floor together; preserve existing sp-0x40 guard",
                           "initialStackTopUnchanged": hex(0x20020000)},
                "plc": {"previousBssEndAndHeapStart": hex(0x2000cba8), "stateStart": hex(plc_start),
                        "newBssEndAndHeapStart": hex(plc_end),
                        "reconstructedTwiddleRam": hex(twiddle_ram) if twiddle_ram is not None else None,
                        "coefficientRebuildBeforeApplicationRequired": twiddle_ram is not None,
                        "initialStackTopUnchanged": hex(0x20038000)}},
            "reservationRecipesNotApplied": recipes, "originalHookWindows": hooks,
            "recipeInput": "Original unpacked raw image only; never flat-patch these offsets after initdata packing",
            "checkedWorkerCall": {"pc": "0x1be00", "target": hex(worker_target),
                                  "arithmetic": "(pc + AUIPC immediate + signed JALR immediate) modulo 2^32, bit 0 cleared"},
            "unresolved": ["Extension code placement and relocated trampolines",
                           "Indirect allocator/pool aliases and runtime stack/heap headroom",
                           "Exact hook calling context, serialization, and target bindings",
                           "PLC modified-image acceptance and independent recovery",
                           "Physical LED mapping and RGB channel semantics"],
            "interpretation": "Explicit reservation proposal, not proven safe placement or flashable firmware"}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("archive", type=Path)
    parser.add_argument("--zig", type=Path, default=DEFAULT_ZIG)
    parser.add_argument("--master-code-bytes", type=lambda value: int(value, 0), default=0,
                        help="Review-only extra RAM code reservation after master state")
    args = parser.parse_args()
    validate_archive(args.archive)
    spec = json.loads(TARGET.read_text(encoding="utf-8"))
    with zipfile.ZipFile(args.archive) as archive:
        images = {role: parse_uf2(archive.read(spec[role]["archiveEntry"]), spec[role])
                  for role in ("master", "plc")}
    OUTPUT.mkdir(parents=True, exist_ok=True)
    env = os.environ.copy()
    for kind in ("GLOBAL", "LOCAL"):
        cache = ROOT / "build/firmware/zig-cache" / kind.lower()
        cache.mkdir(parents=True, exist_ok=True)
        env[f"ZIG_{kind}_CACHE_DIR"] = str(cache)
    zig = args.zig.resolve()
    require(run([str(zig), "version"], env) == "0.15.2", "Expected Zig 0.15.2")
    plan = make_plan(images, measure_layout(zig, env), args.master_code_bytes)
    plan["compiledLegacyTrampolines"] = build_trampolines(zig, env, images)
    plan["inputHashes"] = {role: hashlib.sha256(raw).hexdigest() for role, raw in images.items()}
    plan["sourceHashes"] = {str(p.relative_to(ROOT)): hashlib.sha256(p.read_bytes()).hexdigest()
                           for p in (Path(__file__), ROOT / "src/firmware/dp104_patch_state.h",
                                     ROOT / "src/firmware/dp104_dma_rx.h",
                                     ROOT / "src/firmware/dp104_plc_diagnostic.h",
                                     ROOT / "src/firmware/dp104_master_bridge.h",
                                     ROOT / "src/firmware/dp104_receiver.h",
                                     ROOT / "src/firmware/dp104_usb_stage.h",
                                     ROOT / "src/firmware/dp104_twiddle.h",
                                     ROOT / "include/dp104_protocol.h", TARGET)}
    destination = OUTPUT / "plan.json"
    destination.write_text(json.dumps(plan, indent=2) + "\n", encoding="utf-8")
    for role, region in plan["reservations"].items():
        print(f"{role}: state={layout_size(plan, role)} bytes; {region}")
    print(f"Review plan: {destination}")
    print("No firmware modified; code placement, memory headroom and hook ABI remain unverified.")


def layout_size(plan, role):
    return plan["storageLayout"][role]["bytes"]


if __name__ == "__main__":
    try:
        main()
    except (ValueError, OSError, KeyError, zipfile.BadZipFile, subprocess.TimeoutExpired) as error:
        raise SystemExit(str(error))
