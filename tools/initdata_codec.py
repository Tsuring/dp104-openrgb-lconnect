"""Lossless codec for the bounded DP104 initdata packet format."""

from __future__ import annotations


def pack_initdata(data: bytes | bytearray | memoryview) -> bytes:
    """Return a minimum-size packetization of *data*.

    Literal and zero-run packets each cover 1..128 input bytes. Ties prefer a
    zero-run packet, then the longer packet, making output deterministic.
    """
    raw = bytes(data)
    length = len(raw)
    costs = [0] * (length + 1)
    choices = [0] * length
    zero_choices = [False] * length
    zero_run = [0] * (length + 1)

    for offset in range(length - 1, -1, -1):
        if raw[offset] == 0:
            zero_run[offset] = min(128, zero_run[offset + 1] + 1)

    for offset in range(length - 1, -1, -1):
        max_count = min(128, length - offset)
        best_key = None
        best_count = 0
        best_zero = False

        for count in range(1, max_count + 1):
            key = (1 + count + costs[offset + count], 1, -count)
            if best_key is None or key < best_key:
                best_key = key
                best_count = count
                best_zero = False

        for count in range(1, zero_run[offset] + 1):
            key = (1 + costs[offset + count], 0, -count)
            if key < best_key:
                best_key = key
                best_count = count
                best_zero = True

        costs[offset] = best_key[0]
        choices[offset] = best_count
        zero_choices[offset] = best_zero

    packed = bytearray()
    offset = 0
    while offset < length:
        count = choices[offset]
        if zero_choices[offset]:
            packed.append(0x80 | (count - 1))
        else:
            packed.append(count - 1)
            packed.extend(raw[offset:offset + count])
        offset += count

    return bytes(packed)


def unpack_initdata(encoded: bytes | bytearray | memoryview,
                    expected_length: int) -> bytes:
    """Decode *encoded*, requiring exactly *expected_length* output bytes.

    Raise ValueError for an invalid length, truncated literal, output overflow,
    or a stream that ends before the requested output length.
    """
    if (not isinstance(expected_length, int) or isinstance(expected_length, bool)
            or expected_length < 0):
        raise ValueError("expected_length must be a nonnegative integer")

    source = bytes(encoded)
    output = bytearray()
    offset = 0

    while offset < len(source):
        token = source[offset]
        offset += 1
        count = (token & 0x7f) + 1

        if count > expected_length - len(output):
            raise ValueError("decoded output exceeds expected_length")

        if token & 0x80:
            output.extend(bytes(count))
        else:
            if count > len(source) - offset:
                raise ValueError("truncated literal packet")
            output.extend(source[offset:offset + count])
            offset += count

    if len(output) != expected_length:
        raise ValueError("decoded output is shorter than expected_length")

    return bytes(output)
