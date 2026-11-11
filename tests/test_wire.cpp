#include "dp104_wire.h"
#include "dp104_wire_stream.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <vector>

#define CHECK(x) do { if(!(x)) { std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); std::exit(1); } } while(0)

struct Record { bool extension; std::vector<uint8_t> bytes; };
struct Sink { std::vector<Record> records; };
static void extension(void *p, const uint8_t *data, size_t length)
{ static_cast<Sink *>(p)->records.push_back({true, {data, data + length}}); }
static void legacy(void *p, const uint8_t *data, size_t length)
{ static_cast<Sink *>(p)->records.push_back({false, {data, data + length}}); }

static std::vector<uint8_t> encode(const std::array<uint8_t, DP104_REPORT_BYTES> &payload,
                                  uint8_t direction = DP104_WIRE_REQUEST)
{
    std::array<uint8_t, DP104_WIRE_BYTES> storage{};
    CHECK(dp104_wire_encode(direction, payload.data(), payload.size(),
                            storage.data(), storage.size()));
    const size_t size = dp104_wire_frame_size(storage.data(), storage.size());
    CHECK(size == DP104_WIRE_BYTES);
    return {storage.begin(), storage.begin() + size};
}

static std::vector<uint8_t> old_escape_frame(
    const std::array<uint8_t, DP104_REPORT_BYTES> &payload,
    uint8_t direction = DP104_WIRE_REQUEST)
{
    std::vector<uint8_t> frame = {direction, DP104_WIRE_TYPE, 0};
    for(uint8_t value : payload)
    {
        if(value <= 3u)
        {
            frame.push_back(3u);
            frame.push_back(static_cast<uint8_t>(value + 4u));
        }
        else frame.push_back(value);
    }
    frame[2] = static_cast<uint8_t>(frame.size() - DP104_WIRE_HEADER_BYTES);
    return frame;
}

static bool old_escape_decoder_accepts(const std::vector<uint8_t> &frame,
                                       uint8_t direction)
{
    if(frame.size() < DP104_WIRE_HEADER_BYTES || frame[0] != direction ||
       frame[1] != DP104_WIRE_TYPE || frame[2] < DP104_REPORT_BYTES ||
       frame[2] > 64u || frame.size() != DP104_WIRE_HEADER_BYTES + frame[2])
        return false;
    std::vector<uint8_t> decoded;
    for(size_t i = DP104_WIRE_HEADER_BYTES; i < frame.size(); ++i)
    {
        uint8_t value = frame[i];
        if(value == 3u)
        {
            if(++i >= frame.size() || frame[i] < 4u || frame[i] > 7u)
                return false;
            value = static_cast<uint8_t>(frame[i] - 4u);
        }
        decoded.push_back(value);
    }
    return decoded.size() == DP104_REPORT_BYTES && decoded[0] == DP104_COMMAND;
}

static std::array<uint8_t, DP104_REPORT_BYTES> sample_payload()
{
    std::array<uint8_t, DP104_REPORT_BYTES> payload{};
    payload[0] = DP104_COMMAND;
    for(size_t i = 1; i < payload.size(); ++i)
        payload[i] = static_cast<uint8_t>((i * 37u) & 0xffu);
    return payload;
}

static std::vector<uint8_t> next_record(dp104_wire_stream &stream, bool expected_extension)
{
    std::array<uint8_t, DP104_WIRE_STREAM_FRAME_MAX> out{};
    size_t out_size = 0;
    uint8_t is_extension = 0;
    CHECK(dp104_wire_stream_next(&stream, out.data(), out.size(),
                                 &out_size, &is_extension) == 1);
    CHECK((is_extension != 0) == expected_extension);
    return {out.begin(), out.begin() + out_size};
}

static void check_extension_round_trip(const std::vector<uint8_t> &frame,
                                       const std::array<uint8_t, DP104_REPORT_BYTES> &payload,
                                       uint8_t direction = DP104_WIRE_REQUEST)
{
    std::array<uint8_t, DP104_REPORT_BYTES> decoded{};
    CHECK(dp104_wire_frame_size(frame.data(), frame.size()) == frame.size());
    CHECK(dp104_wire_frame_size(frame.data(), frame.size() - 1u) == 0);
    CHECK(dp104_wire_decode(direction, frame.data(), frame.size(),
                            decoded.data(), decoded.size()));
    CHECK(decoded == payload);
}

int main()
{
    auto payload = sample_payload();
    const auto frame = encode(payload);
    check_extension_round_trip(frame, payload);
    CHECK(frame[0] == DP104_WIRE_REQUEST && frame[1] == DP104_WIRE_TYPE);
    CHECK(frame[2] == DP104_WIRE_ENCODED_BYTES);
    CHECK(frame.size() == DP104_WIRE_BYTES && frame.size() == 46u);
    for(size_t i = DP104_WIRE_HEADER_BYTES; i < frame.size(); ++i)
        CHECK(frame[i] >= DP104_WIRE_SYMBOL_MIN && frame[i] <= DP104_WIRE_SYMBOL_MAX);

    /* Every byte value round-trips through each report position. */
    for(unsigned value = 0; value <= 255u; ++value)
    {
        std::array<uint8_t, DP104_REPORT_BYTES> each_value{};
        each_value[0] = DP104_COMMAND;
        std::fill(each_value.begin() + 1, each_value.end(), static_cast<uint8_t>(value));
        const auto each_frame = encode(each_value);
        check_extension_round_trip(each_frame, each_value);
    }

    /* The fixed alphabet includes raw 0/1/2/3/255 without escapes. */
    payload[1] = 0;
    payload[2] = 1;
    payload[3] = 2;
    payload[4] = 3;
    payload[5] = 4;
    payload[6] = 7;
    payload[7] = 255;
    const auto boundaries = encode(payload);
    check_extension_round_trip(boundaries, payload);
    for(size_t i = DP104_WIRE_HEADER_BYTES; i < boundaries.size(); ++i)
        CHECK(boundaries[i] >= DP104_WIRE_SYMBOL_MIN && boundaries[i] <= DP104_WIRE_SYMBOL_MAX);
    CHECK(boundaries.size() == DP104_WIRE_BYTES);

    /* In-place operation is supported in both directions, including overlap. */
    std::array<uint8_t, DP104_WIRE_BYTES> in_place{};
    in_place[0] = DP104_COMMAND;
    in_place[1] = 0;
    in_place[2] = 1;
    in_place[3] = 2;
    in_place[4] = 3;
    for(size_t i = 5; i < DP104_REPORT_BYTES; ++i)
        in_place[i] = static_cast<uint8_t>(i + 8u);
    std::array<uint8_t, DP104_REPORT_BYTES> original{};
    std::copy_n(in_place.begin(), original.size(), original.begin());
    CHECK(dp104_wire_encode(DP104_WIRE_RESPONSE, in_place.data(), DP104_REPORT_BYTES,
                            in_place.data(), in_place.size()));
    const size_t in_place_size = dp104_wire_frame_size(in_place.data(), in_place.size());
    CHECK(in_place_size == DP104_WIRE_BYTES);
    CHECK(dp104_wire_decode(DP104_WIRE_RESPONSE, in_place.data(), in_place_size,
                            in_place.data(), DP104_REPORT_BYTES));
    CHECK(std::equal(original.begin(), original.end(), in_place.begin()));

    std::array<uint8_t, DP104_REPORT_BYTES> decoded{};
    auto malformed = boundaries;
    malformed[DP104_WIRE_HEADER_BYTES] = DP104_WIRE_SYMBOL_MIN - 1u;
    CHECK(!dp104_wire_decode(DP104_WIRE_REQUEST, malformed.data(), malformed.size(),
                             decoded.data(), decoded.size()));
    malformed = boundaries;
    malformed.back() ^= 1u; /* The final symbol has two required zero pad bits. */
    CHECK(!dp104_wire_decode(DP104_WIRE_REQUEST, malformed.data(), malformed.size(),
                             decoded.data(), decoded.size()));
    malformed = boundaries;
    malformed[2]--;
    CHECK(!dp104_wire_decode(DP104_WIRE_REQUEST, malformed.data(), malformed.size(),
                             decoded.data(), decoded.size()));
    CHECK(!dp104_wire_decode(DP104_WIRE_RESPONSE, boundaries.data(), boundaries.size(),
                             decoded.data(), decoded.size()));

    /* New and prior escaped envelopes are intentionally incompatible. */
    CHECK(!old_escape_decoder_accepts(frame, DP104_WIRE_REQUEST));
    const auto old_frame = old_escape_frame(payload);
    CHECK(old_escape_decoder_accepts(old_frame, DP104_WIRE_REQUEST));
    CHECK(!dp104_wire_decode(DP104_WIRE_REQUEST, old_frame.data(), old_frame.size(),
                             decoded.data(), decoded.size()));

    /* Every possible two-part split waits cleanly, then emits exactly one frame. */
    for(size_t split = 1; split < boundaries.size(); ++split)
    {
        dp104_wire_stream stream{};
        dp104_wire_stream_init(&stream);
        CHECK(dp104_wire_stream_push(&stream, boundaries.data(), split));
        std::array<uint8_t, DP104_WIRE_STREAM_FRAME_MAX> out{};
        size_t out_size = 123;
        uint8_t is_extension = 0;
        CHECK(dp104_wire_stream_next(&stream, out.data(), out.size(),
                                     &out_size, &is_extension) == 0);
        CHECK(out_size == 0 && is_extension == 0);
        CHECK(dp104_wire_stream_push(&stream, boundaries.data() + split,
                                     boundaries.size() - split));
        CHECK(next_record(stream, true) == boundaries);
        CHECK(dp104_wire_stream_next(&stream, out.data(), out.size(),
                                     &out_size, &is_extension) == 0);
    }

    /* Byte-at-a-time feeds and extension/legacy coalescing preserve records. */
    dp104_wire_stream stream{};
    dp104_wire_stream_init(&stream);
    std::vector<uint8_t> key = {0, 0x0b, 1, 0x52};
    std::vector<uint8_t> status = {2, 0x22, 4, 0x20, 1, 0, 0};
    std::vector<uint8_t> opaque = {1, 0x11, static_cast<uint8_t>(boundaries.size())};
    opaque.insert(opaque.end(), boundaries.begin(), boundaries.end());
    std::vector<uint8_t> coalesced = key;
    coalesced.insert(coalesced.end(), boundaries.begin(), boundaries.end());
    coalesced.insert(coalesced.end(), status.begin(), status.end());
    coalesced.insert(coalesced.end(), opaque.begin(), opaque.end());
    for(uint8_t byte : coalesced)
        CHECK(dp104_wire_stream_push(&stream, &byte, 1));
    CHECK(next_record(stream, false) == key);
    CHECK(next_record(stream, true) == boundaries);
    CHECK(next_record(stream, false) == status);
    CHECK(next_record(stream, false) == opaque);

    /* A partial extension is discarded at a new direction prefix; no body leaks. */
    dp104_wire_stream_init(&stream);
    std::vector<uint8_t> interrupted(boundaries.begin(), boundaries.begin() + 17);
    const std::vector<uint8_t> replacement = {1, 0x44, 1, 0xa5};
    interrupted.insert(interrupted.end(), replacement.begin(), replacement.end());
    CHECK(dp104_wire_stream_push(&stream, interrupted.data(), interrupted.size()));
    CHECK(next_record(stream, false) == replacement);

    /* Invalid symbol frames are dropped and the following legacy record survives. */
    dp104_wire_stream_init(&stream);
    malformed = boundaries;
    malformed[DP104_WIRE_HEADER_BYTES + 5u] = DP104_WIRE_SYMBOL_MAX + 1u;
    malformed.insert(malformed.end(), replacement.begin(), replacement.end());
    CHECK(dp104_wire_stream_push(&stream, malformed.data(), malformed.size()));
    CHECK(next_record(stream, false) == replacement);

    /* Invalid reserved envelopes are consumed as a whole and never leak. */
    dp104_wire_stream_init(&stream);
    std::vector<uint8_t> bad_header = {0, DP104_WIRE_TYPE, 31};
    bad_header.resize(DP104_WIRE_HEADER_BYTES + 31u, 0x55);
    bad_header.insert(bad_header.end(), replacement.begin(), replacement.end());
    CHECK(dp104_wire_stream_push(&stream, bad_header.data(), bad_header.size()));
    CHECK(next_record(stream, false) == replacement);
    dp104_wire_stream_init(&stream);
    auto old_record_then_legacy = old_frame;
    old_record_then_legacy.insert(old_record_then_legacy.end(), replacement.begin(), replacement.end());
    CHECK(dp104_wire_stream_push(&stream, old_record_then_legacy.data(),
                                 old_record_then_legacy.size()));
    CHECK(next_record(stream, false) == replacement);
    for(size_t split = 0; split < old_frame.size(); ++split)
    {
        dp104_wire_stream_init(&stream);
        CHECK(dp104_wire_stream_push(&stream, old_frame.data(), split));
        std::array<uint8_t, DP104_WIRE_STREAM_FRAME_MAX> out{};
        size_t out_size = 123;
        uint8_t is_extension = 0;
        CHECK(dp104_wire_stream_next(&stream, out.data(), out.size(),
                                     &out_size, &is_extension) == 0);
        CHECK(out_size == 0 && is_extension == 0);
        CHECK(dp104_wire_stream_push(&stream, old_record_then_legacy.data() + split,
                                     old_record_then_legacy.size() - split));
        CHECK(next_record(stream, false) == replacement);
        CHECK(dp104_wire_stream_next(&stream, out.data(), out.size(),
                                     &out_size, &is_extension) == 0);
    }
    dp104_wire_stream_init(&stream);
    std::vector<uint8_t> bad_direction = {9, 8, 7, 1, 0x44, 1, 0xa5};
    CHECK(dp104_wire_stream_push(&stream, bad_direction.data(), bad_direction.size()));
    CHECK(next_record(stream, false) == replacement);

    /* Capacity errors do not consume; overflow enters bounded drop/resync mode. */
    dp104_wire_stream_init(&stream);
    CHECK(dp104_wire_stream_push(&stream, boundaries.data(), boundaries.size()));
    std::array<uint8_t, 2> too_small{};
    size_t out_size = 0;
    uint8_t is_extension = 0;
    CHECK(dp104_wire_stream_next(&stream, too_small.data(), too_small.size(),
                                 &out_size, &is_extension) == -1);
    CHECK(stream.used == boundaries.size());
    CHECK(dp104_wire_stream_next(&stream, too_small.data(), too_small.size(),
                                 &out_size, &is_extension) == -1);

    dp104_wire_stream_init(&stream);
    std::array<uint8_t, DP104_WIRE_STREAM_CAPACITY + 1u> overflow{};
    CHECK(!dp104_wire_stream_push(&stream, overflow.data(), overflow.size()));
    CHECK(stream.used == 0 && stream.dropping == 1);
    CHECK(dp104_wire_stream_push(&stream, coalesced.data(), coalesced.size()));
    CHECK(next_record(stream, false) == key);
    CHECK(next_record(stream, true) == boundaries);

    /* The legacy batch helper preserves opaque records and never leaks reserved 7D. */
    Sink sink;
    CHECK(dp104_wire_route_batch(DP104_WIRE_REQUEST, coalesced.data(), coalesced.size(),
                                 extension, legacy, &sink) == 1);
    CHECK(sink.records.size() == 4);
    CHECK(!sink.records[0].extension && sink.records[0].bytes == key);
    CHECK(sink.records[1].extension && sink.records[1].bytes == boundaries);
    CHECK(!sink.records[2].extension && sink.records[2].bytes == status);
    CHECK(!sink.records[3].extension && sink.records[3].bytes == opaque);

    sink.records.clear();
    std::vector<uint8_t> old_batch = old_frame;
    old_batch.insert(old_batch.end(), key.begin(), key.end());
    CHECK(dp104_wire_route_batch(DP104_WIRE_REQUEST, old_batch.data(), old_batch.size(),
                                 extension, legacy, &sink) == 0);
    CHECK(sink.records.size() == 1 && !sink.records[0].extension &&
          sink.records[0].bytes == key);

    sink.records.clear();
    std::vector<uint8_t> bad_reserved = {0, DP104_WIRE_TYPE, 31};
    bad_reserved.resize(DP104_WIRE_HEADER_BYTES + 31u, 0x55);
    bad_reserved.insert(bad_reserved.end(), key.begin(), key.end());
    CHECK(dp104_wire_route_batch(DP104_WIRE_REQUEST, bad_reserved.data(), bad_reserved.size(),
                                 extension, legacy, &sink) == 0);
    CHECK(sink.records.size() == 1 && !sink.records[0].extension &&
          sink.records[0].bytes == key);
    sink.records.clear();
    const std::vector<uint8_t> truncated_reserved = {0, DP104_WIRE_TYPE};
    CHECK(dp104_wire_route_batch(DP104_WIRE_REQUEST, truncated_reserved.data(),
                                 truncated_reserved.size(), extension, legacy, &sink) == 0);
    CHECK(sink.records.empty());

    /* Encoding and decoding honor exact capacities and leave canaries intact. */
    std::array<uint8_t, DP104_WIRE_BYTES + 2u> guarded_frame{};
    std::array<uint8_t, DP104_REPORT_BYTES + 2u> guarded_payload{};
    guarded_frame.front() = guarded_frame.back() = 0xa5;
    guarded_payload.front() = guarded_payload.back() = 0x5a;
    CHECK(dp104_wire_encode(DP104_WIRE_REQUEST, payload.data(), payload.size(),
                            guarded_frame.data() + 1u, DP104_WIRE_BYTES));
    CHECK(guarded_frame.front() == 0xa5 && guarded_frame.back() == 0xa5);
    CHECK(dp104_wire_decode(DP104_WIRE_REQUEST, guarded_frame.data() + 1u,
                            DP104_WIRE_BYTES, guarded_payload.data() + 1u,
                            DP104_REPORT_BYTES));
    CHECK(guarded_payload.front() == 0x5a && guarded_payload.back() == 0x5a);
    CHECK(std::equal(payload.begin(), payload.end(), guarded_payload.begin() + 1));

    std::puts("wire: fixed 46-byte six-bit envelopes, all byte values, mismatch rejection, every split, coalescing, resync, overflow, in-place and legacy preservation PASS");
    return 0;
}
