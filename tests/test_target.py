import hashlib
import importlib.util
import struct
import unittest
from pathlib import Path

spec = importlib.util.spec_from_file_location(
    "verify_target", Path(__file__).resolve().parents[1] / "tools/verify_target.py")
verifier = importlib.util.module_from_spec(spec)
spec.loader.exec_module(verifier)


class FirmwareInputGuardTests(unittest.TestCase):
    def setUp(self):
        self.raw = bytes(range(256)) * 2
        self.target = {"family": "0xe560e104", "base": "0x8000", "size": 512,
                       "rawSha256": hashlib.sha256(self.raw).hexdigest()}
        self.blocks = []
        for i in range(2):
            block = bytearray(512)
            struct.pack_into("<8I", block, 0, 0x0A324655, 0x9E5D5157, 0x2000,
                             0x8000 + i * 256, 256, i, 2, 0xE560E104)
            block[32:288] = self.raw[i * 256:(i + 1) * 256]
            struct.pack_into("<I", block, 508, 0x0AB16F30)
            self.blocks.append(block)

    def parse(self, blocks=None):
        return verifier.parse_uf2(b"".join(self.blocks if blocks is None else blocks), self.target)

    def test_complete_blocks_can_arrive_in_either_order(self):
        self.assertEqual(self.parse(), self.raw)
        self.assertEqual(self.parse(list(reversed(self.blocks))), self.raw)

    def test_wrong_image_fingerprint_is_rejected(self):
        self.blocks[1][100] ^= 1
        with self.assertRaisesRegex(ValueError, "hash"):
            self.parse()

    def test_duplicate_block_rejected(self):
        with self.assertRaisesRegex(ValueError, "duplicate"):
            self.parse([self.blocks[0], self.blocks[0]])

    def test_bad_magic_flags_family_address_size_and_count_rejected(self):
        for offset, value in [(0, 0), (8, 0), (12, 0x9000), (16, 512),
                              (24, 3), (28, 0xE560E105), (508, 0)]:
            with self.subTest(offset=offset):
                block = bytearray(self.blocks[0])
                struct.pack_into("<I", block, offset, value)
                with self.assertRaises(ValueError):
                    self.parse([block, self.blocks[1]])

    def test_partial_block_rejected(self):
        with self.assertRaisesRegex(ValueError, "length"):
            verifier.parse_uf2(bytes(self.blocks[0][:-1]), self.target)


if __name__ == "__main__":
    unittest.main()
