import itertools
import random
import sys
import unittest
from functools import lru_cache
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from lz_codec import pack_lz, unpack_lz


def brute_force_minimum_size(data):
    """Independent small-input oracle over all legal token lengths/distances."""
    length = len(data)

    @lru_cache(maxsize=None)
    def cost(offset, phase):
        if offset == length:
            return 0

        group_cost = 1 if phase == 0 else 0
        next_phase = (phase + 1) % 8
        best = group_cost + 1 + cost(offset + 1, next_phase)
        maximum = min(18, length - offset)

        for distance in range(1, min(offset, 4096) + 1):
            match_length = 0
            while (match_length < maximum and
                   data[offset + match_length] ==
                   data[offset + match_length - distance]):
                match_length += 1
            for count in range(3, match_length + 1):
                best = min(best, group_cost + 2 + cost(offset + count, next_phase))

        return best

    return cost(0, 0)


class LzCodecTests(unittest.TestCase):
    def test_known_overlapping_backcopy_vector(self):
        encoded = bytes((0x02, ord("A"), 0x0f, 0x00))
        self.assertEqual(unpack_lz(encoded, 19), b"A" * 19)
        self.assertEqual(pack_lz(b"A" * 19), encoded)

    def test_literal_group_boundaries(self):
        eight = b"abcdefgh"
        nine = b"abcdefghi"
        self.assertEqual(pack_lz(eight), b"\x00" + eight)
        self.assertEqual(pack_lz(nine), b"\x00" + eight + b"\x00i")
        self.assertEqual(unpack_lz(pack_lz(eight), 8), eight)
        self.assertEqual(unpack_lz(pack_lz(nine), 9), nine)

    def test_empty_stream_and_argument_validation(self):
        self.assertEqual(pack_lz(b""), b"")
        self.assertEqual(unpack_lz(b"", 0), b"")
        for invalid in (-1, 1.5, True):
            with self.subTest(expected_length=invalid):
                with self.assertRaises(ValueError):
                    unpack_lz(b"", invalid)

    def test_rejects_invalid_distance_truncation_overflow_flags_and_trailing(self):
        malformed = (
            (b"\x01\x00\x00", 3),        # distance 1 before any output
            (b"", 1),                     # missing flags
            (b"\x00", 1),                # truncated literal
            (b"\x01\x00", 3),            # truncated match
            (b"\x80x", 1),               # nonzero unused final flag
            (b"\x00x\x00", 1),           # trailing token
            (b"\x00x", 2),               # short output
            (b"\x02A\x0f\x00", 18),      # match exceeds output capacity
            (b"\x00", 0),                # nonempty stream for empty output
        )
        for encoded, expected_length in malformed:
            with self.subTest(encoded=encoded, expected_length=expected_length):
                with self.assertRaises(ValueError):
                    unpack_lz(encoded, expected_length)

    def test_seeded_random_and_periodic_roundtrips(self):
        rng = random.Random(0xD2104)
        samples = [
            b"",
            bytes(range(256)),
            b"AB" * 300,
            b"A" * 513,
            bytes(range(64)) * 40,
        ]
        for index in range(120):
            length = rng.randrange(0, 513)
            if index % 3 == 0:
                source = bytes(rng.getrandbits(8) for _ in range(length))
            elif index % 3 == 1:
                source = bytes(rng.randrange(8) for _ in range(length))
            else:
                period = bytes(rng.getrandbits(8) for _ in range(rng.randrange(1, 20)))
                source = (period * ((length + len(period) - 1) // len(period)))[:length]
            samples.append(source)

        for source in samples:
            with self.subTest(length=len(source), prefix=source[:8]):
                packed = pack_lz(source)
                self.assertEqual(packed, pack_lz(source))
                self.assertEqual(unpack_lz(packed, len(source)), source)

    def test_dynamic_program_is_optimal_for_small_inputs(self):
        for length in range(6):
            for values in itertools.product((0, 1, 2), repeat=length):
                source = bytes(values)
                with self.subTest(source=source):
                    packed = pack_lz(source)
                    self.assertEqual(len(packed), brute_force_minimum_size(source))
                    self.assertEqual(unpack_lz(packed, length), source)


if __name__ == "__main__":
    unittest.main()
