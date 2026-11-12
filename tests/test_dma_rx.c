#include "dp104_dma_rx.h"
#include "dp104_wire.h"

#include <stdio.h>
#include <string.h>

#define CAPTURE_CAPACITY 512u

typedef struct capture {
    uint8_t bytes[CAPTURE_CAPACITY];
    size_t length;
    size_t calls;
    int overflow;
} capture;

static void capture_emit(void *context, const uint8_t *data, size_t length)
{
    capture *out = (capture *)context;
    if(length > CAPTURE_CAPACITY - out->length)
    {
        out->overflow = 1;
        return;
    }
    memcpy(out->bytes + out->length, data, length);
    out->length += length;
    ++out->calls;
}

#define CHECK(condition) do { \
    if(!(condition)) { \
        fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition); \
        return 0; \
    } \
} while(0)

static int test_half_idle_and_wrap(void)
{
    uint8_t buffer[64];
    uint8_t expected[73];
    capture out = {{0}, 0, 0, 0};
    dp104_dma_rx_cursor cursor;
    size_t i;

    for(i = 0; i < sizeof(buffer); ++i) buffer[i] = (uint8_t)i;
    for(i = 0; i < sizeof(expected); ++i) expected[i] = (uint8_t)i;
    dp104_dma_rx_cursor_init(&cursor);

    CHECK(dp104_dma_rx_consume(&cursor, buffer, sizeof(buffer), 56u, 0,
          capture_emit, &out) == DP104_DMA_RX_OK);
    CHECK(cursor.consumed == 8u && out.length == 8u);
    CHECK(dp104_dma_rx_consume(&cursor, buffer, sizeof(buffer), 49u, 0,
          capture_emit, &out) == DP104_DMA_RX_OK);
    CHECK(cursor.consumed == 15u && out.length == 15u);
    CHECK(dp104_dma_rx_consume(&cursor, buffer, sizeof(buffer), 32u,
          DP104_DMA_RX_EVENT_HALF, capture_emit, &out) == DP104_DMA_RX_OK);
    CHECK(cursor.consumed == 32u && out.length == 32u);

    /* A repeated half-transfer and IDLE at the same position add no bytes. */
    CHECK(dp104_dma_rx_consume(&cursor, buffer, sizeof(buffer), 32u,
          DP104_DMA_RX_EVENT_HALF, capture_emit, &out) == DP104_DMA_RX_OK);
    CHECK(dp104_dma_rx_consume(&cursor, buffer, sizeof(buffer), 32u, 0,
          capture_emit, &out) == DP104_DMA_RX_OK);
    CHECK(cursor.consumed == 32u && out.length == 32u);

    CHECK(dp104_dma_rx_consume(&cursor, buffer, sizeof(buffer), 17u, 0,
          capture_emit, &out) == DP104_DMA_RX_OK);
    CHECK(cursor.consumed == 47u && out.length == 47u);

    buffer[0] = 64u;
    buffer[1] = 65u;
    buffer[2] = 66u;
    CHECK(dp104_dma_rx_consume(&cursor, buffer, sizeof(buffer), 61u,
          DP104_DMA_RX_EVENT_COMPLETE, capture_emit, &out) == DP104_DMA_RX_OK);
    CHECK(cursor.consumed == 3u && out.length == 67u);

    for(i = 3u; i < 9u; ++i) buffer[i] = (uint8_t)(64u + i);
    CHECK(dp104_dma_rx_consume(&cursor, buffer, sizeof(buffer), 55u, 0,
          capture_emit, &out) == DP104_DMA_RX_OK);
    CHECK(cursor.consumed == 9u && out.length == sizeof(expected));
    CHECK(out.calls == 7u && !out.overflow);
    CHECK(memcmp(out.bytes, expected, sizeof(expected)) == 0);
    return 1;
}

static int test_remaining_zero_boundary(void)
{
    uint8_t buffer[64];
    capture out = {{0}, 0, 0, 0};
    dp104_dma_rx_cursor cursor;
    size_t i;

    for(i = 0; i < sizeof(buffer); ++i) buffer[i] = (uint8_t)(0x80u + i);
    dp104_dma_rx_cursor_init(&cursor);
    CHECK(dp104_dma_rx_consume(&cursor, buffer, sizeof(buffer), 32u,
          DP104_DMA_RX_EVENT_HALF, capture_emit, &out) == DP104_DMA_RX_OK);
    CHECK(dp104_dma_rx_consume(&cursor, buffer, sizeof(buffer), 0u, 0,
          capture_emit, &out) == DP104_DMA_RX_OK);
    CHECK(cursor.consumed == 0u && cursor.pending_complete == 1u);
    CHECK(out.length == sizeof(buffer));

    /* Repeated IDLE at the consumed endpoint is harmless. */
    CHECK(dp104_dma_rx_consume(&cursor, buffer, sizeof(buffer), 0u, 0,
          capture_emit, &out) == DP104_DMA_RX_OK);
    CHECK(out.length == sizeof(buffer));

    /* The following TC at remaining==capacity acknowledges that same end. */
    CHECK(dp104_dma_rx_consume(&cursor, buffer, sizeof(buffer), sizeof(buffer),
          DP104_DMA_RX_EVENT_COMPLETE, capture_emit, &out) == DP104_DMA_RX_OK);
    CHECK(cursor.consumed == 0u && cursor.pending_complete == 2u);
    CHECK(dp104_dma_rx_consume(&cursor, buffer, sizeof(buffer), sizeof(buffer),
          0, capture_emit, &out) == DP104_DMA_RX_OK);
    CHECK(out.length == sizeof(buffer) && !out.overflow);
    return 1;
}

static int test_delayed_complete_and_repeated_idle(void)
{
    uint8_t buffer[64];
    uint8_t expected[71];
    capture out = {{0}, 0, 0, 0};
    dp104_dma_rx_cursor cursor;
    size_t i;

    for(i = 0; i < sizeof(buffer); ++i) buffer[i] = (uint8_t)i;
    for(i = 0; i < sizeof(expected); ++i) expected[i] = (uint8_t)i;
    dp104_dma_rx_cursor_init(&cursor);
    CHECK(dp104_dma_rx_consume(&cursor, buffer, sizeof(buffer), 32u,
          DP104_DMA_RX_EVENT_HALF, capture_emit, &out) == DP104_DMA_RX_OK);
    CHECK(dp104_dma_rx_consume(&cursor, buffer, sizeof(buffer), 0u, 0,
          capture_emit, &out) == DP104_DMA_RX_OK);
    CHECK(cursor.consumed == 0u && cursor.pending_complete == 1u);

    for(i = 0; i < 4u; ++i) buffer[i] = (uint8_t)(64u + i);
    CHECK(dp104_dma_rx_consume(&cursor, buffer, sizeof(buffer), 60u, 0,
          capture_emit, &out) == DP104_DMA_RX_OK);
    CHECK(cursor.consumed == 4u && cursor.pending_complete == 1u);
    for(i = 4u; i < 7u; ++i) buffer[i] = (uint8_t)(64u + i);
    CHECK(dp104_dma_rx_consume(&cursor, buffer, sizeof(buffer), 57u,
          DP104_DMA_RX_EVENT_COMPLETE, capture_emit, &out) == DP104_DMA_RX_OK);
    CHECK(cursor.consumed == 7u && cursor.pending_complete == 0u);
    CHECK(out.length == sizeof(expected) && !out.overflow);
    CHECK(memcmp(out.bytes, expected, sizeof(expected)) == 0);

    /* A TC-consumed exact endpoint tolerates repeated IDLE at remaining==0. */
    dp104_dma_rx_cursor_init(&cursor);
    memset(&out, 0, sizeof(out));
    CHECK(dp104_dma_rx_consume(&cursor, buffer, sizeof(buffer), 32u,
          DP104_DMA_RX_EVENT_HALF, capture_emit, &out) == DP104_DMA_RX_OK);
    CHECK(dp104_dma_rx_consume(&cursor, buffer, sizeof(buffer), 0u,
          DP104_DMA_RX_EVENT_COMPLETE, capture_emit, &out) == DP104_DMA_RX_OK);
    CHECK(cursor.consumed == 0u && cursor.pending_complete == 2u);
    CHECK(dp104_dma_rx_consume(&cursor, buffer, sizeof(buffer), 0u, 0,
          capture_emit, &out) == DP104_DMA_RX_OK);
    CHECK(dp104_dma_rx_consume(&cursor, buffer, sizeof(buffer), sizeof(buffer),
          0, capture_emit, &out) == DP104_DMA_RX_OK);
    CHECK(out.length == sizeof(buffer));

    /* A fresh TC at that unchanged position is ambiguous, but emits nothing. */
    CHECK(dp104_dma_rx_consume(&cursor, buffer, sizeof(buffer), sizeof(buffer),
          DP104_DMA_RX_EVENT_COMPLETE, capture_emit, &out) == DP104_DMA_RX_OVERRUN);
    CHECK(cursor.consumed == 0u && cursor.pending_complete == 0u);
    CHECK(out.length == sizeof(buffer) && !out.overflow);
    return 1;
}

static int test_overrun_and_invalid_inputs(void)
{
    uint8_t buffer[64] = {0};
    capture out = {{0}, 0, 0, 0};
    dp104_dma_rx_cursor cursor;
    dp104_dma_rx_result result;

    dp104_dma_rx_cursor_init(&cursor);
    result = dp104_dma_rx_consume(&cursor, buffer, sizeof(buffer),
        sizeof(buffer), DP104_DMA_RX_EVENT_COMPLETE, capture_emit, &out);
    CHECK(result == DP104_DMA_RX_OVERRUN);
    CHECK(cursor.consumed == 0u && out.length == 0u);

    dp104_dma_rx_cursor_init(&cursor);
    CHECK(dp104_dma_rx_consume(&cursor, buffer, sizeof(buffer), 32u,
          DP104_DMA_RX_EVENT_HALF, capture_emit, &out) == DP104_DMA_RX_OK);
    CHECK(out.length == 32u);
    result = dp104_dma_rx_consume(&cursor, buffer, sizeof(buffer), 44u,
        DP104_DMA_RX_EVENT_HALF | DP104_DMA_RX_EVENT_COMPLETE,
        capture_emit, &out);
    CHECK(result == DP104_DMA_RX_OVERRUN);
    CHECK(cursor.consumed == 20u && out.length == 32u);

    result = dp104_dma_rx_consume(&cursor, buffer, sizeof(buffer), 44u,
        DP104_DMA_RX_EVENT_COMPLETE, capture_emit, &out);
    CHECK(result == DP104_DMA_RX_OVERRUN);
    CHECK(cursor.consumed == 20u && out.length == 32u);

    /* Invalid capacity/counter/event arguments leave state untouched. */
    CHECK(dp104_dma_rx_consume(&cursor, buffer, 0u, 0u, 0,
          capture_emit, &out) == DP104_DMA_RX_ERROR);
    CHECK(cursor.consumed == 20u);
    CHECK(dp104_dma_rx_consume(&cursor, buffer, (size_t)UINT16_MAX + 1u,
          0u, 0, capture_emit, &out) == DP104_DMA_RX_ERROR);
    CHECK(cursor.consumed == 20u);
    CHECK(dp104_dma_rx_consume(&cursor, buffer, sizeof(buffer), 65u, 0,
          capture_emit, &out) == DP104_DMA_RX_ERROR);
    CHECK(cursor.consumed == 20u);
    CHECK(dp104_dma_rx_consume(&cursor, buffer, sizeof(buffer), 20u, 0x80u,
          capture_emit, &out) == DP104_DMA_RX_ERROR);
    CHECK(cursor.consumed == 20u);
    CHECK(dp104_dma_rx_consume(&cursor, NULL, sizeof(buffer), 20u, 0,
          capture_emit, &out) == DP104_DMA_RX_ERROR);
    CHECK(cursor.consumed == 20u);
    CHECK(dp104_dma_rx_consume(&cursor, buffer, sizeof(buffer), 20u, 0,
          NULL, &out) == DP104_DMA_RX_ERROR);
    CHECK(cursor.consumed == 20u);

    cursor.consumed = (uint16_t)sizeof(buffer);
    CHECK(dp104_dma_rx_consume(&cursor, buffer, sizeof(buffer), 20u, 0,
          capture_emit, &out) == DP104_DMA_RX_ERROR);
    CHECK(cursor.consumed == sizeof(buffer));
    cursor.consumed = 0;
    cursor.pending_complete = 3u;
    CHECK(dp104_dma_rx_consume(&cursor, buffer, sizeof(buffer), 64u, 0,
          capture_emit, &out) == DP104_DMA_RX_ERROR);
    CHECK(cursor.consumed == 0u && cursor.pending_complete == 3u);
    CHECK(!out.overflow);
    return 1;
}

static int append_bytes(uint8_t *out, size_t capacity, size_t *used,
                        const uint8_t *data, size_t length)
{
    if(!out || !used || !data || length > capacity - *used) return 0;
    memcpy(out + *used, data, length);
    *used += length;
    return 1;
}

static int append_legacy_record(uint8_t *out, size_t capacity, size_t *used,
                                uint8_t seed)
{
    uint8_t record[17];
    unsigned i;
    record[0] = DP104_WIRE_REQUEST;
    record[1] = 0x31u;
    record[2] = 14u;
    for(i = 3u; i < sizeof(record); ++i)
        record[i] = (uint8_t)(seed + i);
    return append_bytes(out, capacity, used, record, sizeof(record));
}

static int append_rgb_record(uint8_t *out, size_t capacity, size_t *used,
                             uint8_t transaction)
{
    uint8_t payload[DP104_REPORT_BYTES] = {0};
    uint8_t frame[DP104_WIRE_BYTES];
    payload[0] = DP104_COMMAND;
    payload[1] = DP104_OP_DATA;
    payload[2] = transaction;
    dp104_write16(payload + 4, (uint16_t)(transaction * 8u));
    payload[6] = DP104_CHUNK_LEDS;
    if(!dp104_wire_encode(DP104_WIRE_REQUEST, payload, sizeof(payload),
                          frame, sizeof(frame)))
        return 0;
    return append_bytes(out, capacity, used, frame, sizeof(frame));
}

static int test_continuous_legacy_rgb_stream(void)
{
    uint8_t stream[256];
    uint8_t dma[128];
    capture out = {{0}, 0, 0, 0};
    dp104_dma_rx_cursor cursor;
    size_t used = 0;
    unsigned i;

    for(i = 0; i < 4u; ++i)
        CHECK(append_legacy_record(stream, sizeof(stream), &used,
                                  (uint8_t)(i * 17u)));
    CHECK(append_rgb_record(stream, sizeof(stream), &used, 17u));
    for(i = 0; i < 3u; ++i)
        CHECK(append_legacy_record(stream, sizeof(stream), &used,
                                  (uint8_t)(0x40u + i * 17u)));
    CHECK(append_rgb_record(stream, sizeof(stream), &used, 18u));
    CHECK(used == 211u && used > sizeof(dma));

    dp104_dma_rx_cursor_init(&cursor);
    memcpy(dma, stream, 64u);
    CHECK(dp104_dma_rx_consume(&cursor, dma, sizeof(dma), 64u,
          DP104_DMA_RX_EVENT_HALF, capture_emit, &out) == DP104_DMA_RX_OK);
    CHECK(cursor.consumed == 64u && out.length == 64u);

    memcpy(dma + 64u, stream + 64u, 64u);
    CHECK(dp104_dma_rx_consume(&cursor, dma, sizeof(dma), sizeof(dma),
          DP104_DMA_RX_EVENT_COMPLETE, capture_emit, &out) == DP104_DMA_RX_OK);
    CHECK(cursor.consumed == 0u && out.length == 128u);

    memcpy(dma, stream + 128u, 64u);
    CHECK(dp104_dma_rx_consume(&cursor, dma, sizeof(dma), 64u,
          DP104_DMA_RX_EVENT_HALF, capture_emit, &out) == DP104_DMA_RX_OK);
    CHECK(cursor.consumed == 64u && out.length == 192u);

    memcpy(dma + 64u, stream + 192u, used - 192u);
    CHECK(dp104_dma_rx_consume(&cursor, dma, sizeof(dma),
          sizeof(dma) - (used - 128u), 0, capture_emit, &out) ==
          DP104_DMA_RX_OK);
    CHECK(cursor.consumed == used - 128u && out.length == used);
    CHECK(out.calls == 4u && !out.overflow);
    CHECK(memcmp(out.bytes, stream, used) == 0);
    return 1;
}

int main(void)
{
    if(!test_half_idle_and_wrap() ||
       !test_remaining_zero_boundary() ||
       !test_delayed_complete_and_repeated_idle() ||
       !test_overrun_and_invalid_inputs() ||
       !test_continuous_legacy_rgb_stream())
        return 1;
    puts("DMA RX cursor tests passed");
    return 0;
}
