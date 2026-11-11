#include "dp104_wire.h"

#include <string.h>

size_t dp104_wire_frame_size(const uint8_t *frame, size_t available)
{
    size_t encoded;
    size_t total;
    if(!frame || available < DP104_WIRE_HEADER_BYTES || frame[0] > 1u ||
       frame[1] != DP104_WIRE_TYPE)
        return 0;
    encoded = frame[2];
    if(encoded != DP104_WIRE_ENCODED_BYTES)
        return 0;
    total = DP104_WIRE_HEADER_BYTES + encoded;
    return available < total ? 0 : total;
}

int dp104_wire_encode(uint8_t direction, const uint8_t *payload, size_t payload_size,
                      uint8_t *frame, size_t capacity)
{
    uint8_t raw[DP104_REPORT_BYTES];
    size_t out = DP104_WIRE_HEADER_BYTES;
    uint32_t bits = 0;
    unsigned bit_count = 0;
    unsigned i;

    if(direction > 1u || !payload || !frame || payload_size != DP104_REPORT_BYTES ||
       payload[0] != DP104_COMMAND || capacity < DP104_WIRE_BYTES)
        return 0;

    /* Copy first so payload/frame may be the same or overlapping buffer. */
    memcpy(raw, payload, sizeof(raw));
    frame[0] = direction;
    frame[1] = DP104_WIRE_TYPE;
    frame[2] = DP104_WIRE_ENCODED_BYTES;
    for(i = 0; i < DP104_REPORT_BYTES; ++i)
    {
        bits = (bits << 8) | raw[i];
        bit_count += 8u;
        while(bit_count >= 6u)
        {
            uint8_t value;
            bit_count -= 6u;
            value = (uint8_t)((bits >> bit_count) & 0x3fu);
            frame[out++] = (uint8_t)(DP104_WIRE_SYMBOL_MIN + value);
            bits &= bit_count ? ((UINT32_C(1) << bit_count) - 1u) : 0u;
        }
    }
    if(bit_count != 0u)
    {
        const uint8_t value = (uint8_t)((bits << (6u - bit_count)) & 0x3fu);
        frame[out++] = (uint8_t)(DP104_WIRE_SYMBOL_MIN + value);
    }
    return out == DP104_WIRE_BYTES;
}

int dp104_wire_decode(uint8_t direction, const uint8_t *frame, size_t frame_size,
                      uint8_t *payload, size_t capacity)
{
    uint8_t decoded[DP104_REPORT_BYTES];
    uint32_t bits = 0;
    unsigned bit_count = 0;
    unsigned output = 0;
    unsigned i;

    if(direction > 1u || !frame || !payload || capacity < DP104_REPORT_BYTES ||
       frame_size != DP104_WIRE_BYTES || frame[0] != direction ||
       frame[1] != DP104_WIRE_TYPE || frame[2] != DP104_WIRE_ENCODED_BYTES)
        return 0;

    for(i = 0; i < DP104_WIRE_ENCODED_BYTES; ++i)
    {
        uint8_t symbol = frame[DP104_WIRE_HEADER_BYTES + i];
        uint8_t value;
        if(symbol < DP104_WIRE_SYMBOL_MIN || symbol > DP104_WIRE_SYMBOL_MAX)
            return 0;
        value = (uint8_t)(symbol - DP104_WIRE_SYMBOL_MIN);
        if(i + 1u == DP104_WIRE_ENCODED_BYTES && (value & 0x03u) != 0u)
            return 0; /* The last symbol contains 4 data bits and 2 zero pad bits. */
        bits = (bits << 6) | value;
        bit_count += 6u;
        while(bit_count >= 8u)
        {
            bit_count -= 8u;
            if(output >= DP104_REPORT_BYTES) return 0;
            decoded[output++] = (uint8_t)(bits >> bit_count);
            bits &= bit_count ? ((UINT32_C(1) << bit_count) - 1u) : 0u;
        }
    }
    if(output != DP104_REPORT_BYTES || bit_count != 2u || bits != 0u ||
       decoded[0] != DP104_COMMAND)
        return 0;
    memmove(payload, decoded, sizeof(decoded));
    return 1;
}

unsigned dp104_wire_route_batch(uint8_t direction, const uint8_t *batch, size_t length,
                                dp104_wire_sink extension, dp104_wire_sink legacy,
                                void *context)
{
    unsigned routed = 0;
    if(!batch || !length) return 0;
    while(length)
    {
        size_t record;
        const uint8_t record_direction = batch[0];
        const uint8_t type = length >= 2u ? batch[1] : 0xffu;
        const uint8_t payload_length = length >= 3u ? batch[2] : 0xffu;

        if(length < DP104_WIRE_HEADER_BYTES)
        {
            /* Even a truncated two-byte reserved prefix must not leak to the
             * stock dispatcher. There is no declared tail to consume. */
            if(length >= 2u && record_direction <= 1u &&
               type == DP104_WIRE_TYPE)
                break;
            if(legacy) legacy(context, batch, length);
            break;
        }
        if(record_direction > 2u)
        {
            if(legacy) legacy(context, batch, length);
            break;
        }

        /* Tag 7D is reserved for the extension only in directions 0/1. Never
         * pass such bytes to stock dispatch, even if their envelope is bad. */
        if(type == DP104_WIRE_TYPE && record_direction <= 1u)
        {
            if(payload_length != DP104_WIRE_ENCODED_BYTES)
            {
                record = DP104_WIRE_HEADER_BYTES + (size_t)payload_length;
                if(record > length) break;
                batch += record;
                length -= record;
                continue;
            }
            record = dp104_wire_frame_size(batch, length);
            if(!record) break; /* Truncated reserved record: consume remainder. */
            {
                uint8_t decoded[DP104_REPORT_BYTES];
                if(dp104_wire_decode(record_direction, batch, record,
                                     decoded, sizeof(decoded)))
                {
                    if(record_direction == direction && extension)
                    {
                        extension(context, batch, record);
                        ++routed;
                    }
                }
            }
            batch += record;
            length -= record;
            continue;
        }

        if(payload_length > 64u)
        {
            if(legacy) legacy(context, batch, length);
            break;
        }
        record = DP104_WIRE_HEADER_BYTES + (size_t)payload_length;
        if(record > length)
        {
            if(legacy) legacy(context, batch, length);
            break;
        }
        if(legacy) legacy(context, batch, record);
        batch += record;
        length -= record;
    }
    return routed;
}
