#!/usr/bin/env python3
"""Cross-compile the portable DP104 receiver core as freestanding objects.

This script only compiles and inspects relocatable ELF objects. It does not
link a firmware image, assign target addresses, create UF2 files, or access a
device.
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
from pathlib import Path
from typing import Any


ROOT = Path(__file__).resolve().parents[1]
DEFAULT_ZIG = ROOT / ".local" / "toolchain" / "zig-x86_64-windows-0.15.2" / "zig.exe"
OUTPUT_ROOT = ROOT / "build" / "firmware"
SOURCES = (
    Path("src/firmware/dp104_receiver.c"),
    Path("src/firmware/dp104_master_bridge.c"),
    Path("src/firmware/dp104_wire.c"),
    Path("src/firmware/dp104_wire_stream.c"),
    Path("src/firmware/dp104_usb_stage.c"),
    Path("src/firmware/dp104_initdata.c"),
    Path("src/firmware/dp104_twiddle.c"),
    Path("src/firmware/dp104_lz.c"),
    Path("src/firmware/freestanding_memory.c"),
)

ELFCLASS32 = 1
ELFDATA2LSB = 1
EV_CURRENT = 1
ET_REL = 1
EM_ARM = 40
EM_RISCV = 243
SHT_SYMTAB = 2
SHT_RELA = 4
SHT_REL = 9
STB_GLOBAL = 1
STB_WEAK = 2
SHN_UNDEF = 0
EF_RISCV_RVC = 0x1
EF_RISCV_FLOAT_ABI = 0x6
EF_ARM_ABI_FLOAT_SOFT = 0x200
EF_ARM_ABI_FLOAT_HARD = 0x400
EF_ARM_EABIMASK = 0xFF000000

RISCV_RELOCATIONS = {
    47: "R_RISCV_GPREL_I",
    48: "R_RISCV_GPREL_S",
    51: "R_RISCV_RELAX",
}


class BuildError(RuntimeError):
    pass


def c_string(table: bytes, offset: int) -> str:
    if offset < 0 or offset >= len(table):
        return ""
    end = table.find(b"\0", offset)
    if end < 0:
        end = len(table)
    return table[offset:end].decode("utf-8", errors="replace")


def read_slice(data: bytes, offset: int, size: int, label: str) -> bytes:
    if offset < 0 or size < 0 or offset + size > len(data):
        raise BuildError(f"ELF has an out-of-range {label}.")
    return data[offset:offset + size]


def inspect_elf(path: Path) -> dict[str, Any]:
    data = path.read_bytes()
    if len(data) < 52 or data[:4] != b"\x7fELF":
        raise BuildError(f"{path} is not an ELF file.")
    if data[4] != ELFCLASS32:
        raise BuildError(f"{path} is not ELF32 (class={data[4]}).")
    if data[5] != ELFDATA2LSB:
        raise BuildError(f"{path} is not little-endian ELF.")
    if data[6] != EV_CURRENT:
        raise BuildError(f"{path} has an unsupported ELF ident version.")

    fields = struct.unpack_from("<HHIIIIIHHHHHH", data, 16)
    (elf_type, machine, version, entry, phoff, shoff, flags, ehsize,
     phentsize, phnum, shentsize, shnum, shstrndx) = fields
    if version != EV_CURRENT or ehsize != 52:
        raise BuildError(f"{path} has an invalid ELF32 header.")
    if elf_type != ET_REL:
        raise BuildError(f"{path} is not relocatable ELF (e_type={elf_type}).")
    if entry != 0:
        raise BuildError(f"{path} unexpectedly has an entry address 0x{entry:x}.")
    if phnum != 0 or phoff != 0:
        raise BuildError(f"{path} unexpectedly contains program headers.")
    if shentsize != 40 or shnum == 0 or shstrndx >= shnum:
        raise BuildError(f"{path} has an unsupported section table.")

    section_bytes = read_slice(data, shoff, shentsize * shnum, "section table")
    raw_sections = [
        struct.unpack_from("<IIIIIIIIII", section_bytes, index * shentsize)
        for index in range(shnum)
    ]
    shstr = raw_sections[shstrndx]
    shstr_data = read_slice(data, shstr[4], shstr[5], "section-name table")

    sections: list[dict[str, Any]] = []
    for raw in raw_sections:
        name_offset, section_type, section_flags, address, offset, size, link, info, align, entsize = raw
        name = c_string(shstr_data, name_offset)
        if address != 0:
            raise BuildError(f"{path} section {name!r} has an assigned address 0x{address:x}.")
        section_data = b"" if section_type == 8 else read_slice(data, offset, size, f"section {name!r}")
        sections.append({
            "name": name,
            "type": section_type,
            "flags": section_flags,
            "address": address,
            "offset": offset,
            "size": size,
            "link": link,
            "info": info,
            "alignment": align,
            "entry_size": entsize,
            "data": section_data,
        })

    defined: set[str] = set()
    undefined: set[str] = set()
    relocations: list[dict[str, Any]] = []
    for section_index, section in enumerate(sections):
        if section["type"] == SHT_SYMTAB:
            if section["entry_size"] != 16 or section["link"] >= len(sections):
                raise BuildError(f"{path} has an unsupported symbol table.")
            strings = sections[section["link"]]["data"]
            symbols_data = section["data"]
            for offset in range(0, len(symbols_data), section["entry_size"]):
                name_offset, _value, _size, info, _other, shndx = struct.unpack_from(
                    "<IIIBBH", symbols_data, offset
                )
                name = c_string(strings, name_offset)
                binding = info >> 4
                if not name or binding not in (STB_GLOBAL, STB_WEAK):
                    continue
                if shndx == SHN_UNDEF:
                    undefined.add(name)
                else:
                    defined.add(name)
        elif section["type"] in (SHT_REL, SHT_RELA):
            entry_size = section["entry_size"]
            expected_size = 8 if section["type"] == SHT_REL else 12
            if entry_size != expected_size or entry_size == 0:
                raise BuildError(f"{path} has an unsupported relocation table.")
            relocation_data = section["data"]
            for offset in range(0, len(relocation_data), entry_size):
                if section["type"] == SHT_REL:
                    relocation_offset, info = struct.unpack_from("<II", relocation_data, offset)
                else:
                    relocation_offset, info, _addend = struct.unpack_from("<IIi", relocation_data, offset)
                relocation_type = info & 0xFF
                if machine == EM_RISCV:
                    relocation_name = RISCV_RELOCATIONS.get(
                        relocation_type, f"R_RISCV_RELOC_{relocation_type}"
                    )
                elif machine == EM_ARM:
                    relocation_name = f"R_ARM_RELOC_{relocation_type}"
                else:
                    relocation_name = f"type_{relocation_type}"
                relocations.append({
                    "section": section["name"],
                    "offset": relocation_offset,
                    "type": relocation_type,
                    "name": relocation_name,
                })

    visible_sections = [
        {key: value for key, value in section.items() if key != "data"}
        for section in sections
    ]
    section_sizes = {
        section["name"]: section["size"]
        for section in sections
        if section["name"]
    }
    attribute_strings = {}
    for section_name in (".riscv.attributes", ".ARM.attributes"):
        for section in sections:
            if section["name"] == section_name:
                attribute_strings[section_name] = section["data"].decode(
                    "ascii", errors="replace"
                )

    return {
        "path": str(path.relative_to(ROOT)),
        "file_size": len(data),
        "elf_class": 32,
        "endianness": "little",
        "type": elf_type,
        "machine": machine,
        "version": version,
        "entry": entry,
        "program_header_count": phnum,
        "flags": flags,
        "sections": visible_sections,
        "section_sizes": section_sizes,
        "attributes": attribute_strings,
        "defined_global_symbols": sorted(defined),
        "undefined_global_symbols": sorted(undefined),
        "relocations": relocations,
    }


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def compile_object(zig: Path, flags: list[str], source: Path, output: Path,
                   env: dict[str, str]) -> None:
    command = [str(zig), "cc", *flags, "-c", str(source), "-o", str(output)]
    result = subprocess.run(
        command,
        cwd=ROOT,
        env=env,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=False,
    )
    if result.returncode != 0:
        detail = (result.stdout + result.stderr).strip()
        raise BuildError(
            f"compile failed for {source.relative_to(ROOT)} ({result.returncode}):\n{detail}"
        )


def validate_profile(profile: dict[str, Any], objects: list[dict[str, Any]]) -> list[str]:
    problems: list[str] = []
    definitions = {
        symbol
        for item in objects
        for symbol in item["defined_global_symbols"]
    }
    undefined = {
        symbol
        for item in objects
        for symbol in item["undefined_global_symbols"]
    }
    unresolved = sorted(undefined - definitions)

    for item in objects:
        if item["machine"] != profile["machine"]:
            problems.append(
                f"{item['path']}: e_machine={item['machine']}, expected {profile['machine']}"
            )
        if item["type"] != ET_REL:
            problems.append(f"{item['path']}: output is not relocatable ELF")
        forbidden = {".interp", ".dynamic", ".dynsym"}
        present = sorted(
            name
            for name in item["section_sizes"]
            if name in forbidden or name.startswith((".sdata", ".sbss", ".srodata"))
        )
        if present:
            problems.append(f"{item['path']}: forbidden sections present: {present}")

    if profile["machine"] == EM_RISCV:
        for item in objects:
            if not item["flags"] & EF_RISCV_RVC:
                problems.append(f"{item['path']}: RISC-V compressed-extension flag is absent")
            if item["flags"] & EF_RISCV_FLOAT_ABI:
                problems.append(f"{item['path']}: RISC-V float ABI flags are set")
            attributes = item["attributes"].get(".riscv.attributes", "")
            if not re.search(r"rv32i\d+p\d+_m\d+p\d+_c\d+p\d+", attributes):
                problems.append(f"{item['path']}: RV32IMC architecture attribute is absent")
            if re.search(r"_(?:f|d|q)\d+p\d+", attributes):
                problems.append(f"{item['path']}: floating-point ISA extension is present")
            bad_relocations = [
                reloc["name"] for reloc in item["relocations"]
                if reloc["name"] in ("R_RISCV_RELAX", "R_RISCV_GPREL_I", "R_RISCV_GPREL_S")
            ]
            if bad_relocations:
                problems.append(
                    f"{item['path']}: GP-relative/relax relocations present: {bad_relocations}"
                )
    else:
        for item in objects:
            flags = item["flags"]
            if (flags & EF_ARM_EABIMASK) != 0x05000000:
                problems.append(f"{item['path']}: ARM EABI5 flag is absent (0x{flags:08x})")
            if not flags & EF_ARM_ABI_FLOAT_SOFT:
                problems.append(f"{item['path']}: ARM soft-float ABI flag is absent")
            if flags & EF_ARM_ABI_FLOAT_HARD:
                problems.append(f"{item['path']}: ARM hard-float ABI flag is set")
            if "cortex-m3" not in item["attributes"].get(".ARM.attributes", ""):
                problems.append(f"{item['path']}: Cortex-M3 ARM attribute is absent")

    if unresolved:
        problems.append(f"unresolved external symbols after memory support: {unresolved}")
    return problems


def build_profile(zig: Path, version: str, profile: dict[str, Any], env: dict[str, str]) -> dict[str, Any]:
    output_dir = OUTPUT_ROOT / profile["name"]
    output_dir.mkdir(parents=True, exist_ok=True)
    common_flags = [
        "-target", profile["triple"],
        "-std=c99",
        "-Os",
        "-ffreestanding",
        "-fno-builtin",
        "-fno-stack-protector",
        "-fno-common",
        "-fno-pic",
        "-fno-pie",
        "-fno-unwind-tables",
        "-fno-asynchronous-unwind-tables",
        "-nostdlib",
        "-Wall", "-Wextra", "-Werror",
        "-Isrc/firmware/freestanding",
        "-Iinclude",
        *profile["architecture_flags"],
    ]

    object_metadata = []
    for source in SOURCES:
        output = output_dir / (source.stem + ".o")
        compile_object(zig, common_flags, ROOT / source, output, env)
        object_metadata.append(inspect_elf(output))

    problems = validate_profile(profile, object_metadata)
    metadata = {
        "profile": profile["name"],
        "target_triple": profile["triple"],
        "compiler": "Zig " + version,
        "compile_flags": common_flags,
        "inputs": {
            str(source): sha256(ROOT / source)
            for source in (
                *SOURCES,
                Path("src/firmware/freestanding/string.h"),
                Path("src/firmware/dp104_receiver.h"),
                Path("src/firmware/dp104_master_bridge.h"),
                Path("src/firmware/dp104_wire.h"),
                Path("src/firmware/dp104_wire_stream.h"),
                Path("src/firmware/dp104_usb_stage.h"),
                Path("src/firmware/dp104_initdata.h"),
                Path("src/firmware/dp104_twiddle.h"),
                Path("src/firmware/dp104_lz.h"),
                Path("include/dp104_protocol.h"),
                Path(__file__).resolve().relative_to(ROOT),
            )
        },
        "objects": object_metadata,
        "combined_undefined_symbols": sorted({
            symbol
            for item in object_metadata
            for symbol in item["undefined_global_symbols"]
        } - {
            symbol
            for item in object_metadata
            for symbol in item["defined_global_symbols"]
        }),
        "validation_problems": problems,
        "build_only": True,
        "linked": False,
        "addresses_assigned": False,
        "uf2_emitted": False,
    }
    (output_dir / "metadata.json").write_text(
        json.dumps(metadata, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )
    return metadata


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--zig",
        type=Path,
        default=DEFAULT_ZIG,
        help=f"Zig executable (default: {DEFAULT_ZIG})",
    )
    args = parser.parse_args()
    zig = args.zig.resolve()
    if not zig.is_file():
        print(f"Zig executable not found: {zig}", file=sys.stderr)
        return 2

    OUTPUT_ROOT.mkdir(parents=True, exist_ok=True)
    cache_root = OUTPUT_ROOT / "zig-cache"
    global_cache = cache_root / "global"
    local_cache = cache_root / "local"
    global_cache.mkdir(parents=True, exist_ok=True)
    local_cache.mkdir(parents=True, exist_ok=True)
    env = os.environ.copy()
    env["ZIG_GLOBAL_CACHE_DIR"] = str(global_cache)
    env["ZIG_LOCAL_CACHE_DIR"] = str(local_cache)

    version_result = subprocess.run(
        [str(zig), "version"],
        cwd=ROOT,
        env=env,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=False,
    )
    if version_result.returncode != 0:
        print(version_result.stderr.strip() or "Unable to query Zig version.", file=sys.stderr)
        return version_result.returncode or 2
    version = version_result.stdout.strip()
    if version != "0.15.2":
        print(f"Expected Zig 0.15.2, found {version!r}.", file=sys.stderr)
        return 2

    profiles = [
        {
            "name": "rv32imc-ilp32",
            "triple": "riscv32-freestanding-none",
            "machine": EM_RISCV,
            "architecture_flags": [
                "-mcpu=generic_rv32+m+c",
                "-mabi=ilp32",
                "-msmall-data-limit=0",
                "-mno-relax",
            ],
        },
        {
            "name": "thumb-cortex-m3-softfloat",
            "triple": "thumb-freestanding-eabi",
            "machine": EM_ARM,
            "architecture_flags": [
                "-mcpu=cortex_m3",
                "-mthumb",
                "-mfloat-abi=soft",
            ],
        },
    ]

    failures = []
    for profile in profiles:
        try:
            metadata = build_profile(zig, version, profile, env)
        except Exception as error:  # Keep the other architecture attempt independent.
            failures.append(f"{profile['name']}: {error}")
            continue

        print(f"{profile['name']}: e_machine={metadata['objects'][0]['machine']} ", end="")
        print("sizes=" + ", ".join(
            f"{Path(item['path']).name}:{item['file_size']}B"
            for item in metadata["objects"]
        ))
        for item in metadata["objects"]:
            print(f"  {item['path']}: {item['section_sizes']}")
            print(f"  undefined: {item['undefined_global_symbols']}")
        if metadata["validation_problems"]:
            failures.extend(f"{profile['name']}: {problem}" for problem in metadata["validation_problems"])
        else:
            print("  ELF metadata and combined symbol validation: PASS")

    if failures:
        for failure in failures:
            print(f"ERROR: {failure}", file=sys.stderr)
        print(f"Metadata and objects are under {OUTPUT_ROOT}.", file=sys.stderr)
        return 1

    print(f"Relocatable objects and metadata: {OUTPUT_ROOT}")
    print("No link addresses, firmware image, UF2, or device operations were performed.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
