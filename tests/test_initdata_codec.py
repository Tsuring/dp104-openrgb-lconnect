import itertools
import random
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from initdata_codec import pack_initdata, unpack_initdata


def optimal_packet_size(data):
    """Small independent cost oracle for the packetization rules."""
    best = [0] * (len(data) + 1)
    for offset in range(len(data) - 1, -1, -1):
        choices = [1 + count + best[offset + count]
                   for count in range(1, min(128, len(data) - offset) + 1)]
        for count in range(1, min(128, len(data) - offset) + 1):
            if all(value == 0 for value in data[offset:offset + count]):
                choices.append(1 + best[offset + count])
        best[offset] = min(choices)
    return best[0]


class InitdataCodecTests(unittest.TestCase):
    def test_empty_and_boundary_packet_lengths(self):
        self.assertEqual(pack_initdata(b""), b"")
        self.assertEqual(unpack_initdata(b"", 0), b"")

        for length in (1, 127, 128, 129):
            with self.subTest(kind="zero", length=length):
                source = bytes(length)
                if length <= 128:
                    expected = bytes((0x80 | (length - 1),))
                else:
                    expected = b"\xff\x80"
                packed = pack_initdata(source)
                self.assertEqual(packed, expected)
                self.assertEqual(unpack_initdata(packed, length), source)

            with self.subTest(kind="literal", length=length):
                source = b"\x01" * length
                if length <= 128:
                    expected = bytes((length - 1,)) + source
                else:
                    expected = b"\x7f" + source[:128] + b"\x00\x01"
                packed = pack_initdata(source)
                self.assertEqual(packed, expected)
                self.assertEqual(unpack_initdata(packed, length), source)

    def test_mixed_packet_vector_and_determinism(self):
        source = b"ABC\0\0\0\0XY"
        expected = b"\x02ABC\x83\x01XY"
        self.assertEqual(pack_initdata(source), expected)
        self.assertEqual(pack_initdata(source), pack_initdata(source))
        self.assertEqual(unpack_initdata(expected, len(source)), source)

    def test_rejects_truncation_overflow_trailing_input_and_short_output(self):
        malformed = (
            (b"\x02A", 3),       # literal payload is short
            (b"\x80", 0),        # run exceeds empty destination
            (b"\x01AB", 1),      # literal exceeds destination
            (b"\x00A\x80", 1),  # packet trails exact output
            (b"\x00A", 2),       # stream ends before expected output
            (b"\x00A\x00", 1),  # trailing literal token is malformed
        )
        for encoded, expected_length in malformed:
            with self.subTest(encoded=encoded, expected_length=expected_length):
                with self.assertRaises(ValueError):
                    unpack_initdata(encoded, expected_length)

        for invalid_length in (-1, 1.5, True):
            with self.subTest(expected_length=invalid_length):
                with self.assertRaises(ValueError):
                    unpack_initdata(b"", invalid_length)

    def test_deterministic_randomized_roundtrips(self):
        rng = random.Random(0x104)
        samples = [b"", bytes(range(1, 256)), bytes(257)]
        for _ in range(160):
            length = rng.randrange(0, 700)
            samples.append(bytes(rng.randrange(8) for _ in range(length)))

        for source in samples:
            with self.subTest(length=len(source), prefix=source[:8]):
                packed = pack_initdata(source)
                self.assertEqual(packed, pack_initdata(source))
                self.assertEqual(unpack_initdata(packed, len(source)), source)

    def test_packetization_is_optimal_for_small_inputs(self):
        for length in range(7):
            for values in itertools.product((0, 1, 2), repeat=length):
                source = bytes(values)
                with self.subTest(source=source):
                    packed = pack_initdata(source)
                    self.assertEqual(len(packed), optimal_packet_size(source))
                    self.assertEqual(unpack_initdata(packed, length), source)


if __name__ == "__main__":
    unittest.main()
