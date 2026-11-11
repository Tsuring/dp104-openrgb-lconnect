"""Address arithmetic checks for the read-only firmware review planner."""
import struct
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from plan_firmware_hooks import align_up, image_offset, rv_address_pair, rv_pair_target, rv_call_target, make_plan


class HookPlanTests(unittest.TestCase):
    def test_rv32_pair_roundtrip_across_signed_low_carry_and_address_wrap(self):
        for pc in (0x200001ba, 0x14e04, 0xfffffff0, 0):
            for delta in (-0x200001ba, -0x801, -0x800, -1, 0, 0x7ff, 0x800,
                          0x801, 0x20000000, 0x7fffffff, 0x80000000):
                target = (pc + delta) & 0xffffffff
                with self.subTest(pc=hex(pc), target=hex(target)):
                    pair = rv_address_pair(pc, target, 11)
                    self.assertEqual(rv_pair_target(pc, pair), target)
                    first, second = struct.unpack("<II", pair)
                    self.assertEqual((first >> 7) & 31, 11)
                    self.assertEqual((second >> 7) & 31, 11)
                    self.assertEqual((second >> 15) & 31, 11)

    def test_ram_runtime_window_is_not_flat_flash_offset(self):
        raw = bytes(256000)
        self.assertEqual(image_offset("master", 0x200046ee, 8, raw), 0x46f2)
        self.assertEqual(image_offset("master", 0x1be00, 8, raw), 0x13e00)
        self.assertEqual(image_offset("plc", 0x0800dd30, 4, bytes(103936)), 0x5d30)
        for address, length in ((0x2000cdfe, 4), (0x2000ce00, 1), (0x7fff, 4)):
            with self.subTest(address=hex(address)), self.assertRaises(ValueError):
                image_offset("master", address, length, raw)

    def test_cross_segment_auipc_jalr_resolves_with_32bit_arithmetic(self):
        pair = struct.pack("<II", (0x1ffe6 << 12) | (1 << 7) | 0x17,
                           (0xc56 << 20) | (1 << 15) | (1 << 7) | 0x67)
        self.assertEqual(rv_call_target(0x1be00, pair), 0x20001a56)
        with self.assertRaises(ValueError):
            rv_call_target(0x1be00, rv_address_pair(0x1be00, 0x20001a56, 1))

    def test_invalid_or_mismatched_instruction_pair_rejected(self):
        good = rv_address_pair(0x200001ba, 0x20012a04, 11)
        bad_register = bytearray(good)
        second = struct.unpack_from("<I", bad_register, 4)[0] ^ (1 << 15)
        struct.pack_into("<I", bad_register, 4, second)
        for pair in (bytes(8), good[:-1], bytes(bad_register)):
            with self.assertRaises(ValueError):
                rv_pair_target(0x200001ba, pair)
        for pc, target, register in ((-1, 0, 11), (0, 0x100000000, 11), (0, 0, 0)):
            with self.assertRaises(ValueError):
                rv_address_pair(pc, target, register)

    def test_state_alignment(self):
        self.assertEqual(align_up(0x2000cba8, 16), 0x2000cbb0)
        self.assertEqual(align_up(0x20012a04, 16), 0x20012a10)
        with self.assertRaises(ValueError):
            align_up(0x2000cba8, 3)

    def test_master_reservation_moves_all_three_heap_and_bss_initializers(self):
        master, plc = bytearray(256000), bytearray(103936)
        def put(role, address, value):
            image = master if role == "master" else plc
            offset = image_offset(role, address, len(value), image)
            image[offset:offset + len(value)] = value
        put("master", 0x200001ba, rv_address_pair(0x200001ba, 0x20012a04, 11))
        put("master", 0x21a4c, rv_address_pair(0x21a4c, 0x20012a04, 14))
        put("master", 0x464b8, struct.pack("<I", 0x20012a04))
        call = bytearray(rv_address_pair(0x1be00, 0x20001a56, 1))
        word = struct.unpack_from("<I", call, 4)[0]
        struct.pack_into("<I", call, 4, (word & ~0x7f) | 0x67)
        put("master", 0x1be00, call)
        for address in (0x0801a678, 0x0800ce84):
            put("plc", address, struct.pack("<I", 0x2000cba8))
        put("plc", 0x08018334, struct.pack("<I", 0x0801cc5c))
        layout = {"master": {"bytes": 408}, "plc": {"bytes": 8896, "twiddleOffset": 364}}
        plan = make_plan({"master": master, "plc": plc}, layout)
        code_plan = make_plan({"master": master, "plc": plc}, layout, master_code_bytes=0x900)
        self.assertEqual(code_plan["reservations"]["master"]["stateEnd"], "0x20012bb0")
        self.assertEqual(code_plan["reservations"]["master"]["reservedCodeStart"], "0x20012bb0")
        self.assertEqual(code_plan["reservations"]["master"]["newBssEnd"], "0x200134b0")
        with self.assertRaises(ValueError):
            make_plan({"master": master, "plc": plc}, layout, master_code_bytes=-1)
        recipes = [item for item in plan["reservationRecipesNotApplied"] if item["role"] == "master"]
        self.assertEqual(len(recipes), 3)
        for item in recipes:
            offset = item["imageOffset"]
            before, after = bytes.fromhex(item["originalHex"]), bytes.fromhex(item["replacementHex"])
            self.assertEqual(master[offset:offset + len(before)], before)
            master[offset:offset + len(after)] = after
        for address in (0x200001ba, 0x21a4c):
            offset = image_offset("master", address, 8, master)
            self.assertEqual(rv_pair_target(address, master[offset:offset + 8]), 0x20012bb0)
        offset = image_offset("master", 0x464b8, 4, master)
        self.assertEqual(struct.unpack_from("<I", master, offset)[0], 0x20012bb0)
        self.assertTrue(recipes[-1]["applyBeforeInitdataPacking"])
        self.assertEqual(plan["reservations"]["plc"]["newBssEndAndHeapStart"], "0x2000ee70")
        self.assertEqual(plan["reservations"]["plc"]["reconstructedTwiddleRam"], "0x2000cd1c")


if __name__ == "__main__":
    unittest.main()
