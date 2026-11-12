#!/usr/bin/env python3
"""Synthetic CPU and static instruction checks for circular PLC RX."""

from __future__ import annotations

import struct
from pathlib import Path
from typing import Any

import capstone
from capstone import arm

from build_data_restore import (
    CPU_VMA,
    CPU_VMA_END,
    PF_R,
    PF_W,
    PF_X,
    PROFILES,
    ROOT,
    check_machine_abi,
    inspect_elf,
    require,
    run,
    sha256_file,
)
from wsl_paths import to_wsl_path


_USART_CR1 = 0x4000440C
_USART_SR = 0x40004400
_USART_DR = 0x40004404
_GPR_NAMES = {f"r{index}" for index in range(13)} | {"sp", "lr", "pc"}
_CALLER_SAVED = {"r0", "r1", "r2", "r3", "r12", "lr"}


def _cpu_linker_script() -> str:
    return f"""ENTRY(_start)
PHDRS
{{
  test_rx PT_LOAD FLAGS(5);
  test_rw PT_LOAD FLAGS(6);
}}
SECTIONS
{{
  . = 0x{CPU_VMA:x};
  .text : {{ KEEP(*(.text._start)) *(.text .text.*) }} :test_rx
  .rodata : {{ *(.rodata .rodata.*) }} :test_rx
  .ARM.extab : {{ *(.ARM.extab .ARM.extab.*) }} :test_rx
  .ARM.exidx : {{ *(.ARM.exidx .ARM.exidx.*) }} :test_rx
  . = ALIGN(0x1000);
  .data : {{ *(.data .data.* .sdata .sdata.*) }} :test_rw
  .bss (NOLOAD) : {{ *(.bss .bss.* .sbss .sbss.*) *(COMMON) }} :test_rw
  /DISCARD/ : {{ *(.comment) *(.note .note.*) *(.eh_frame .eh_frame.*) }}
}}
ASSERT(ADDR(.text) >= 0x{CPU_VMA:x}, "CPU text below synthetic range")
ASSERT(ADDR(.bss) + SIZEOF(.bss) <= 0x{CPU_VMA_END:x},
       "CPU writable memory exceeds synthetic range")
"""


def target_unity_source() -> str:
    """Compile the same cursor definition with its fixed-size target caller."""
    return ('#define DP104_PLC_UNITY_CURSOR 1\n' +
            '#include "' + (ROOT / 'src/firmware/dp104_dma_rx.c').as_posix() + '"\n' +
            '#include "' + (ROOT / 'src/firmware/dp104_plc_target.c').as_posix() + '"\n')


def _write_cpu_fixture(cpu_dir: Path) -> dict[str, Path]:
    include_dir = cpu_dir / "include"
    include_dir.mkdir(parents=True, exist_ok=True)
    stub_header = include_dir / "stdio.h"
    stub_header.write_text(
        "#ifndef DP104_CPU_STDIO_H\n"
        "#define DP104_CPU_STDIO_H\n"
        "typedef struct dp104_cpu_file FILE;\n"
        "extern FILE *stderr;\n"
        "int fprintf(FILE *stream, const char *format, ...);\n"
        "int puts(const char *text);\n"
        "#endif\n",
        encoding="ascii",
    )
    stdio_stub = cpu_dir / "stdio_stub.c"
    stdio_stub.write_text(
        '#include "stdio.h"\n'
        "FILE *stderr;\n"
        "int fprintf(FILE *stream, const char *format, ...)\n"
        "{\n"
        "    (void)stream;\n"
        "    (void)format;\n"
        "    return 0;\n"
        "}\n"
        "int puts(const char *text)\n"
        "{\n"
        "    (void)text;\n"
        "    return 0;\n"
        "}\n",
        encoding="ascii",
    )
    entry = cpu_dir / "entry.S"
    entry.write_text(
        ".syntax unified\n"
        ".thumb\n"
        '.section .text._start,"ax",%progbits\n'
        ".global _start\n"
        ".type _start,%function\n"
        ".thumb_func\n"
        "_start:\n"
        "    bl main\n"
        "    movs r7, #1\n"
        "    svc #0\n"
        "1:  b 1b\n"
        ".size _start, .-_start\n",
        encoding="ascii",
    )
    linker = cpu_dir / "cpu_layout.ld"
    linker.write_text(_cpu_linker_script(), encoding="ascii")
    return {
        "include_dir": include_dir,
        "stdio_header": stub_header,
        "stdio_stub": stdio_stub,
        "entry": entry,
        "linker": linker,
    }


def build_circular_rx_cpu_test(
    zig: Path, env: dict[str, str], output_dir: Path
) -> dict[str, Any]:
    """Build and run the real PLC target/receiver test in Linux-user QEMU."""
    profile = PROFILES["plc"]
    require(profile["arch"] == "thumb",
            "PLC CPU fixture profile is not ARM Thumb")
    cpu_dir = output_dir / "circular-cpu"
    cpu_dir.mkdir(parents=True, exist_ok=True)
    fixture = _write_cpu_fixture(cpu_dir)
    executable = cpu_dir / "plc_circular_rx_cpu_test"
    unity = cpu_dir / "plc_target_cursor_unity.c"
    unity.write_text(target_unity_source(), encoding="ascii")
    sources = [
        ROOT / "tests" / "test_plc_target.c",
        unity,
        ROOT / "src" / "firmware" / "dp104_plc_diagnostic.c",

        ROOT / "src" / "firmware" / "dp104_receiver.c",
        ROOT / "src" / "firmware" / "dp104_wire.c",
        ROOT / "src" / "firmware" / "dp104_wire_stream.c",
        ROOT / "src" / "firmware" / "dp104_twiddle.c",
        ROOT / "src" / "firmware" / "freestanding_memory.c",
        fixture["stdio_stub"],
        fixture["entry"],
    ]
    command = [
        str(zig), "cc", "-target", profile["linux_triple"],
        "-std=c99", "-Oz", "-g0", "-ffreestanding", "-fno-builtin",
        "-fno-stack-protector", "-fno-common", "-fno-pic", "-fno-pie",
        "-fno-unwind-tables", "-fno-asynchronous-unwind-tables",
        "-ffunction-sections", "-fdata-sections", "-nostdlib", "-static",
        "-no-pie", "-Wall", "-Wextra", "-Werror",
        "-DDP104_PLC_HOST_TEST=1",
        "-I" + str(fixture["include_dir"]),
        "-Isrc/firmware/freestanding", "-Iinclude", "-Isrc/firmware",
        *profile["architecture_flags"],
        *(str(source) for source in sources),
        "-Wl,-e,_start", "-Wl,-T," + str(fixture["linker"]),
        "-Wl,--build-id=none", "-Wl,--no-undefined",
        "-Wl,--gc-sections", "-Wl,-z,max-page-size=0x1000",
        "-o", str(executable),
    ]
    compile_result = run(command, env, timeout=120)
    elf = inspect_elf(executable)
    check_machine_abi(elf, profile, "circular RX CPU fixture")
    require(elf["elf_type"] == 2 and not elf["has_interpreter"] and
            not elf["undefined_symbols"],
            "circular RX CPU fixture is not a fully linked static ET_EXEC")
    require(CPU_VMA <= elf["entry"] < CPU_VMA_END,
            "circular RX CPU entry is outside the synthetic VMA")
    require(bool(elf["program_headers"]),
            "circular RX CPU fixture has no load segments")
    for segment in elf["program_headers"]:
        start = segment["virtual_address"]
        end = start + segment["memory_size"]
        require(CPU_VMA <= start < CPU_VMA_END and end <= CPU_VMA_END,
                "circular RX CPU PT_LOAD lies outside the synthetic VMA")
    require(any(segment["flags"] & (PF_R | PF_X) == (PF_R | PF_X) and
                not segment["flags"] & PF_W for segment in elf["program_headers"]),
            "circular RX CPU fixture lacks read-only executable memory")
    require(any(segment["flags"] & PF_W for segment in elf["program_headers"]),
            "circular RX CPU fixture lacks synthetic writable memory")

    qemu_command = [
        "wsl.exe", "-d", "Ubuntu", "--exec", profile["qemu"],
        to_wsl_path(executable),
    ]
    qemu_result = run(qemu_command, env, timeout=30)
    return {
        "compile_command": command,
        "compile_stdout": compile_result.stdout.strip(),
        "fixture": {
            "directory": str(cpu_dir.relative_to(ROOT)),
            "stdio_header": str(fixture["stdio_header"].relative_to(ROOT)),
            "stdio_header_sha256": sha256_file(fixture["stdio_header"]),
            "stdio_stub": str(fixture["stdio_stub"].relative_to(ROOT)),
            "stdio_stub_sha256": sha256_file(fixture["stdio_stub"]),
            "entry": str(fixture["entry"].relative_to(ROOT)),
            "entry_sha256": sha256_file(fixture["entry"]),
            "linker": str(fixture["linker"].relative_to(ROOT)),
            "linker_sha256": sha256_file(fixture["linker"]),
        },
        "sources": {
            source.relative_to(ROOT).as_posix(): sha256_file(source)
            for source in sources if source.is_file() and source.is_relative_to(ROOT)
        },
        "profile": profile["name"],
        "target_triple": profile["linux_triple"],
        "cpu_profile": profile["cpu"],
        "elf": {
            "path": str(executable.relative_to(ROOT)),
            "sha256": sha256_file(executable),
            "file_size": elf["file_size"],
            "machine": elf["machine"],
            "flags": elf["flags"],
            "entry": elf["entry"],
            "load_segments": elf["program_headers"],
            "undefined_symbols": elf["undefined_symbols"],
        },
        "qemu_command": qemu_command,
        "qemu_exit_code": qemu_result.returncode,
        "qemu_stdout": qemu_result.stdout.strip(),
        "qemu_stderr": qemu_result.stderr.strip(),
        "synthetic_vma": {"start": CPU_VMA, "end_exclusive": CPU_VMA_END},
        "limits": {
            "linked_with_host_libc": False,
            "stdio_output": "stubbed; test failure count and exit status remain active",
            "mmio_executed": False,
            "privileged_irq_instructions_executed": False,
            "stock_firmware_executed": False,
            "execution_scope": (
                "actual PLC C and test source compiled for Thumb Cortex-M3 soft-float; "
                "DP104_PLC_HOST_TEST routes peripheral accesses to synthetic callbacks"
            ),
        },
    }


def _gpr_name(instruction: Any, register: int) -> str | None:
    name = instruction.reg_name(register).lower()
    return name if name in _GPR_NAMES else None


def _written_registers(instruction: Any) -> set[str]:
    try:
        _read, written = instruction.regs_access()
    except capstone.CsError:
        written = []
    result = set()
    for register in written:
        name = instruction.reg_name(register).lower()
        if name in _GPR_NAMES:
            result.add(name)
    if not result and instruction.operands:
        first = instruction.operands[0]
        if first.type == arm.ARM_OP_REG:
            name = instruction.reg_name(first.reg).lower()
            if name in _GPR_NAMES:
                result.add(name)
    return result


def _literal_value(code: bytes, start: int, target: int) -> int | None:
    offset = target - start
    if offset < 0 or offset + 4 > len(code):
        return None
    return struct.unpack_from("<I", code, offset)[0]


def _memory_address(instruction: Any, registers: dict[str, int]) -> int | None:
    if len(instruction.operands) < 2:
        return None
    memory = instruction.operands[1]
    if memory.type != arm.ARM_OP_MEM or memory.mem.base == arm.ARM_REG_PC:
        return None
    base_name = instruction.reg_name(memory.mem.base).lower()
    if base_name not in registers:
        return None
    address = registers[base_name] + memory.mem.disp
    if memory.mem.index != 0:
        index_name = instruction.reg_name(memory.mem.index).lower()
        if index_name not in registers:
            return None
        index = registers[index_name]
        shift = getattr(memory.mem, "lshift", 0)
        scale = getattr(memory.mem, "scale", 1)
        address += (index << shift) * scale
    return address & 0xFFFFFFFF


def _is_load(instruction: Any) -> bool:
    mnemonic = instruction.mnemonic.lower()
    return mnemonic.startswith("ldr") or mnemonic.startswith("ldrex")


def _is_return(instruction: Any) -> bool:
    if instruction.group(capstone.CS_GRP_RET):
        return True
    mnemonic = instruction.mnemonic.lower()
    if mnemonic == "bx" and instruction.operands and \
       instruction.operands[0].type == arm.ARM_OP_REG:
        return instruction.reg_name(instruction.operands[0].reg).lower() == "lr"
    return mnemonic in ("pop", "pop.w") and any(
        operand.type == arm.ARM_OP_REG and
        instruction.reg_name(operand.reg).lower() == "pc"
        for operand in instruction.operands
    )


def _successors(instruction: Any, instruction_map: dict[int, Any], end: int) -> list[int]:
    address_after = instruction.address + instruction.size
    if _is_return(instruction):
        return []
    if instruction.group(capstone.CS_GRP_CALL):
        return [address_after] if address_after in instruction_map else []
    if instruction.group(capstone.CS_GRP_JUMP):
        targets = [operand.imm for operand in instruction.operands
                   if operand.type == arm.ARM_OP_IMM]
        target = targets[-1] if targets else None
        unconditional = instruction.mnemonic.lower() in ("b", "b.w") and \
            getattr(instruction, "cc", arm.ARM_CC_AL) == arm.ARM_CC_AL
        if unconditional:
            return [target] if target in instruction_map else []
        successors = []
        if target in instruction_map:
            successors.append(target)
        if address_after in instruction_map:
            successors.append(address_after)
        return successors
    return [address_after] if address_after < end and address_after in instruction_map else []


def _transfer_constants(instruction: Any, code: bytes, start: int,
                        registers: dict[str, int]) -> dict[str, int]:
    before = dict(registers)
    result = dict(registers)
    mnemonic = instruction.mnemonic.lower()
    written = _written_registers(instruction)

    for name in written:
        result.pop(name, None)

    if instruction.group(capstone.CS_GRP_CALL):
        for name in _CALLER_SAVED:
            result.pop(name, None)
        return result

    if not instruction.operands or instruction.operands[0].type != arm.ARM_OP_REG:
        return result
    destination = instruction.reg_name(instruction.operands[0].reg).lower()
    if destination not in _GPR_NAMES:
        return result

    if mnemonic == "movw" and len(instruction.operands) >= 2 and \
       instruction.operands[1].type == arm.ARM_OP_IMM:
        result[destination] = instruction.operands[1].imm & 0xFFFF
        return result
    if mnemonic == "movt" and len(instruction.operands) >= 2 and \
       instruction.operands[1].type == arm.ARM_OP_IMM:
        if destination in before:
            result[destination] = (before[destination] & 0xFFFF) | \
                                  ((instruction.operands[1].imm & 0xFFFF) << 16)
        return result
    if mnemonic in ("mov", "movs") and len(instruction.operands) >= 2:
        source = instruction.operands[1]
        if source.type == arm.ARM_OP_IMM:
            result[destination] = source.imm & 0xFFFFFFFF
        elif source.type == arm.ARM_OP_REG:
            source_name = instruction.reg_name(source.reg).lower()
            if source_name in before:
                result[destination] = before[source_name]
        return result
    if mnemonic.startswith("ldr") and len(instruction.operands) >= 2:
        memory = instruction.operands[1]
        if memory.type == arm.ARM_OP_MEM and memory.mem.base == arm.ARM_REG_PC:
            literal_address = ((instruction.address + 4) & ~3) + memory.mem.disp
            value = _literal_value(code, start, literal_address)
            if value is not None:
                result[destination] = value
        return result
    if mnemonic == "adr" and len(instruction.operands) >= 2 and \
       instruction.operands[1].type == arm.ARM_OP_IMM:
        result[destination] = instruction.operands[1].imm & 0xFFFFFFFF
        return result
    if mnemonic in ("add", "adds", "sub", "subs") and len(instruction.operands) >= 2:
        source_operand = instruction.operands[1]
        if source_operand.type == arm.ARM_OP_REG:
            source_name = instruction.reg_name(source_operand.reg).lower()
            source_value = before.get(source_name)
            immediate_index = 2
        else:
            source_value = before.get(destination)
            immediate_index = 1
        if immediate_index < len(instruction.operands) and \
           instruction.operands[immediate_index].type == arm.ARM_OP_IMM and \
           source_value is not None:
            immediate = instruction.operands[immediate_index].imm
            value = source_value - immediate if mnemonic.startswith("sub") else \
                    source_value + immediate
            result[destination] = value & 0xFFFFFFFF
        return result
    return result


def verify_usart_irq(code: bytes, address: int) -> dict[str, Any]:
    """Verify the emitted USART SR/DR reads with bounded Thumb dataflow."""
    require(bool(code), "USART IRQ section is empty")
    start = address & ~1
    decoder = capstone.Cs(capstone.CS_ARCH_ARM,
                          capstone.CS_MODE_THUMB | capstone.CS_MODE_MCLASS)
    decoder.detail = True
    instructions = list(decoder.disasm(code, start))
    require(bool(instructions), "USART IRQ section contains no Thumb instructions")
    instruction_map = {instruction.address: instruction for instruction in instructions}
    end = start + len(code)
    pending: list[tuple[int, dict[str, int]]] = [(start, {})]
    seen: set[tuple[int, tuple[tuple[str, int], ...]]] = set()
    reads: dict[int, dict[str, Any]] = {}
    visits = 0

    while pending:
        pc, registers = pending.pop()
        state_key = (pc, tuple(sorted(registers.items())))
        if state_key in seen:
            continue
        seen.add(state_key)
        visits += 1
        require(visits <= 512, "USART IRQ constant propagation exceeded its bound")
        instruction = instruction_map.get(pc)
        if instruction is None:
            continue

        if _is_load(instruction):
            effective = _memory_address(instruction, registers)
            if effective in (_USART_CR1, _USART_SR, _USART_DR):
                reads[instruction.address] = {
                    "address": f"0x{instruction.address:08x}",
                    "mnemonic": instruction.mnemonic,
                    "operands": instruction.op_str,
                    "effective_address": f"0x{effective:08x}",
                    "size": instruction.size,
                }

        updated = _transfer_constants(instruction, code, start, registers)
        for successor in _successors(instruction, instruction_map, end):
            if successor >= start and successor < end:
                pending.append((successor, updated))

    ordered_reads = [reads[key] for key in sorted(reads)]
    cr1_reads = [item for item in ordered_reads
                 if item["effective_address"] == f"0x{_USART_CR1:08x}"]
    sr_reads = [item for item in ordered_reads
                if item["effective_address"] == f"0x{_USART_SR:08x}"]
    dr_reads = [item for item in ordered_reads
                if item["effective_address"] == f"0x{_USART_DR:08x}"]
    sr_dr_pairs = []
    for sr in sr_reads:
        sr_address = int(sr["address"], 16)
        sr_instruction = instruction_map[sr_address]
        next_address = sr_address + sr_instruction.size
        following = reads.get(next_address)
        if following and following["effective_address"] == f"0x{_USART_DR:08x}":
            sr_dr_pairs.append({"sr": sr, "dr": following})

    require(len(cr1_reads) == 1,
            f"expected exactly one USART CR1 gate read at 0x{_USART_CR1:08x}, "
            f"found {len(cr1_reads)}")
    require(len(sr_reads) == 2,
            f"expected exactly two USART SR reads at 0x{_USART_SR:08x}, "
            f"found {len(sr_reads)}")
    require(len(dr_reads) == 1,
            f"expected exactly one USART DR read at 0x{_USART_DR:08x}, "
            f"found {len(dr_reads)}")
    require(len(sr_dr_pairs) == 1,
            "expected exactly one adjacent final USART SR/DR read pair")

    return {
        "section_address": f"0x{start:08x}",
        "section_bytes": len(code),
        "reachable_instruction_count": visits,
        "peripheral_reads": {
            "cr1_gate": cr1_reads,
            "sr": sr_reads,
            "dr": dr_reads,
        },
        "adjacent_final_sr_dr_pair": sr_dr_pairs[0],
        "constant_propagation": {
            "pc_relative_literals": True,
            "movw_movt_and_register_moves": True,
            "caller_saved_registers_killed_at_calls": sorted(_CALLER_SAVED),
            "load_destinations_clobber_prior_constants": True,
            "control_flow_bound": "reachable section instructions, max 512 states",
        },
        "limits": (
            "Static instruction/dataflow evidence only; it does not execute the "
            "USART peripheral or prove silicon-level SR/DR behavior."
        ),
    }
