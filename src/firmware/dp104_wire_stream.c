#include "dp104_wire_stream.h"
#include "dp104_wire.h"

#include <string.h>

static void consume_prefix(dp104_wire_stream *stream, size_t count)
{
    if(count >= stream->used)
    {
        stream->used = 0;
        return;
    }
    memmove(stream->data, stream->data + count, stream->used - count);
    stream->used = (uint16_t)(stream->used - count);
}

void dp104_wire_stream_reset(dp104_wire_stream *stream)
{
    if(!stream) return;
    stream->used = 0;
    stream->dropping = 0;
}

void dp104_wire_stream_init(dp104_wire_stream *stream)
{
    dp104_wire_stream_reset(stream);
}

int dp104_wire_stream_push(dp104_wire_stream *stream,
                           const uint8_t *bytes, size_t length)
{
    if(!stream || (length && !bytes)) return 0;
    if(stream->used > DP104_WIRE_STREAM_CAPACITY)
    {
        stream->used = 0;
        stream->dropping = 1;
        return 0;
    }
    if(length > DP104_WIRE_STREAM_CAPACITY - stream->used)
    {
        /* Do not keep an unparseable tail or copy past the FIFO. */
        stream->used = 0;
        stream->dropping = 1;
        return 0;
    }
    if(length)
    {
        memmove(stream->data + stream->used, bytes, length);
        stream->used = (uint16_t)(stream->used + length);
    }
    return 1;
}

/* A valid six-bit payload never emits 0, 1, or 2, so these bytes begin a new
 * record even when an extension frame was truncated or corrupted. */
static int find_extension_resync(const dp104_wire_stream *stream, size_t record_size,
                                 size_t *prefix)
{
    const size_t end = stream->used < record_size ? stream->used : record_size;
    size_t i = DP104_WIRE_HEADER_BYTES;
    while(i < end)
    {
        if(stream->data[i] <= 2u)
        {
            *prefix = i;
            return 1;
        }
        ++i;
    }
    return 0;
}

int dp104_wire_stream_next(dp104_wire_stream *stream,
                           uint8_t *out, size_t capacity,
                           size_t *out_length, uint8_t *is_extension)
{
    if(!stream || !out || !out_length || !is_extension) return -1;
    *out_length = 0;
    *is_extension = 0;
    if(stream->used > DP104_WIRE_STREAM_CAPACITY)
    {
        stream->used = 0;
        stream->dropping = 1;
        return 0;
    }

    for(;;)
    {
        uint8_t direction, type, payload_length;
        size_t record_size;
        if(!stream->used) return 0;

        if(stream->dropping)
        {
            size_t candidate = 0;
            while(candidate < stream->used && stream->data[candidate] > 2u)
                ++candidate;
            if(candidate == stream->used)
            {
                stream->used = 0;
                return 0;
            }
            consume_prefix(stream, candidate);
            stream->dropping = 0;
            continue;
        }

        if(stream->used < DP104_WIRE_HEADER_BYTES) return 0;
        direction = stream->data[0];
        type = stream->data[1];
        payload_length = stream->data[2];
        if(direction > 2u)
        {
            consume_prefix(stream, 1u);
            stream->dropping = 1;
            continue;
        }

        if(type == DP104_WIRE_TYPE && direction <= 1u)
        {
            size_t interrupt = 0;
            int scan;
            uint8_t decoded[DP104_REPORT_BYTES];

            if(payload_length != DP104_WIRE_ENCODED_BYTES)
            {
                /* Old escaped extension frames have at most 64 payload
                 * bytes. Consume a complete reserved envelope as a unit so
                 * no legacy bytes from an incompatible frame can leak. */
                if(payload_length <= 64u)
                {
                    record_size = DP104_WIRE_HEADER_BYTES + payload_length;
                    if(stream->used < record_size) return 0;
                    consume_prefix(stream, record_size);
                    continue;
                }
                consume_prefix(stream, DP104_WIRE_HEADER_BYTES);
                stream->dropping = 1;
                continue;
            }
            record_size = DP104_WIRE_HEADER_BYTES + (size_t)payload_length;
            scan = find_extension_resync(stream, record_size, &interrupt);
            if(scan > 0)
            {
                consume_prefix(stream, interrupt);
                stream->dropping = 0;
                continue;
            }
            if(stream->used < record_size) return 0;
            if(!dp104_wire_decode(direction, stream->data, record_size,
                                  decoded, sizeof(decoded)))
            {
                consume_prefix(stream, record_size);
                stream->dropping = 1;
                continue;
            }
            if(capacity < record_size) return -1;
            memcpy(out, stream->data, record_size);
            *out_length = record_size;
            *is_extension = 1;
            consume_prefix(stream, record_size);
            stream->dropping = 0;
            return 1;
        }

        /* Legacy framing is opaque: wait for its declared whole record and do
         * not scan inside its payload, even if it contains header-like bytes. */
        if(payload_length > 64u)
        {
            consume_prefix(stream, 1u);
            stream->dropping = 1;
            continue;
        }
        record_size = DP104_WIRE_HEADER_BYTES + (size_t)payload_length;
        if(stream->used < record_size) return 0;
        if(capacity < record_size) return -1;
        memcpy(out, stream->data, record_size);
        *out_length = record_size;
        *is_extension = 0;
        consume_prefix(stream, record_size);
        return 1;
    }
}
