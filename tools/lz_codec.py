"""Deterministic packer and strict decoder for the DP104 LZ token format."""

from __future__ import annotations

from bisect import bisect_left

_MAX_DISTANCE = 4096
_MAX_MATCH = 18
_FLAG_BITS = 8


def _longest_matches(data: bytes) -> tuple[list[int], list[int]]:
    """Return the longest match length and nearest matching distance per byte."""
    length = len(data)
    max_lengths = [0] * length
    distances = [0] * length
    positions_by_key: dict[bytes, list[int]] = {}

    for offset in range(length):
        if length - offset >= 3:
            key = data[offset:offset + 3]
            candidates = positions_by_key.get(key)
            if candidates:
                first = bisect_left(candidates, offset - _MAX_DISTANCE)
                max_count = min(_MAX_MATCH, length - offset)

                for candidate_index in range(len(candidates) - 1, first - 1, -1):
                    candidate = candidates[candidate_index]
                    distance = offset - candidate
                    count = 3
                    while (count < max_count and
                           data[candidate + count] == data[offset + count]):
                        count += 1

                    if count > max_lengths[offset]:
                        max_lengths[offset] = count
                        distances[offset] = distance
                        if count == max_count:
                            break

            positions_by_key.setdefault(key, []).append(offset)

    return max_lengths, distances


def pack_lz(data: bytes | bytearray | memoryview) -> bytes:
    """Return a minimum-size deterministic encoding of *data*.

    Tokens are grouped in sets of up to eight after a flags byte. A clear flag
    bit is followed by one literal byte. A set bit is followed by a little-
    endian word whose high 12 bits encode distance-1 and low 4 bits encode
    length-3. The DP accounts for a flags byte whenever a token group starts.
    Ties prefer the longest match, then a literal; equal-length matches use the
    nearest distance.
    """
    if not isinstance(data, (bytes, bytearray, memoryview)):
        raise TypeError("data must be bytes-like")

    raw = bytes(data)
    length = len(raw)
    if length == 0:
        return b""

    max_matches, match_distances = _longest_matches(raw)
    costs = [[0] * _FLAG_BITS for _ in range(length + 1)]
    choices: list[list[tuple[int, int] | None]] = [
        [None] * _FLAG_BITS for _ in range(length)
    ]

    for offset in range(length - 1, -1, -1):
        for phase in range(_FLAG_BITS):
            next_phase = (phase + 1) % _FLAG_BITS
            group_cost = 1 if phase == 0 else 0
            best_cost: int | None = None
            best_choice: tuple[int, int] | None = None

            max_match = max_matches[offset]
            for count in range(max_match, 2, -1):
                candidate_cost = group_cost + 2 + costs[offset + count][next_phase]
                if best_cost is None or candidate_cost < best_cost:
                    best_cost = candidate_cost
                    best_choice = (count, match_distances[offset])

            literal_cost = group_cost + 1 + costs[offset + 1][next_phase]
            if best_cost is None or literal_cost < best_cost:
                best_cost = literal_cost
                best_choice = (1, 0)  # length, distance; zero distance means literal

            costs[offset][phase] = best_cost
            choices[offset][phase] = best_choice

    encoded = bytearray()
    offset = 0
    phase = 0
    flag_offset = 0

    while offset < length:
        if phase == 0:
            flag_offset = len(encoded)
            encoded.append(0)

        choice = choices[offset][phase]
        assert choice is not None
        count, distance = choice

        if distance == 0:
            encoded.append(raw[offset])
        else:
            encoded[flag_offset] |= 1 << phase
            match = ((distance - 1) << 4) | (count - 3)
            encoded.append(match & 0xff)
            encoded.append((match >> 8) & 0xff)

        offset += count
        phase = (phase + 1) % _FLAG_BITS

    return bytes(encoded)


def unpack_lz(encoded: bytes | bytearray | memoryview,
              expected_length: int) -> bytes:
    """Decode *encoded*, requiring exact input consumption and output length.

    Raise ValueError for malformed, truncated, overlong, or trailing input.
    """
    if not isinstance(encoded, (bytes, bytearray, memoryview)):
        raise TypeError("encoded must be bytes-like")
    if (not isinstance(expected_length, int) or isinstance(expected_length, bool)
            or expected_length < 0):
        raise ValueError("expected_length must be a nonnegative integer")

    source = bytes(encoded)
    output = bytearray()
    input_offset = 0

    while len(output) < expected_length:
        if input_offset >= len(source):
            raise ValueError("missing flags byte")

        flags = source[input_offset]
        input_offset += 1
        token_index = 0

        while token_index < _FLAG_BITS and len(output) < expected_length:
            if flags & (1 << token_index):
                if len(source) - input_offset < 2:
                    raise ValueError("truncated match token")

                match = source[input_offset] | (source[input_offset + 1] << 8)
                input_offset += 2
                distance = (match >> 4) + 1
                count = (match & 0x0f) + 3

                if distance > len(output):
                    raise ValueError("match distance exceeds produced output")
                if count > expected_length - len(output):
                    raise ValueError("match exceeds expected output length")

                for _ in range(count):
                    output.append(output[-distance])
            else:
                if input_offset >= len(source):
                    raise ValueError("truncated literal token")
                output.append(source[input_offset])
                input_offset += 1

            token_index += 1

        if len(output) == expected_length and token_index < _FLAG_BITS:
            if flags >> token_index:
                raise ValueError("unused flag bits must be zero")

    if input_offset != len(source):
        raise ValueError("trailing encoded bytes")

    return bytes(output)
