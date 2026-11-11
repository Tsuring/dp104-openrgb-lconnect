#ifndef DP104_PROTOCOL_H
#define DP104_PROTOCOL_H

#include <stdint.h>

/* Proposed extension. Stock DP104 1.2.0 does not implement these requests. */
#define DP104_REPORT_BYTES 32u
#define DP104_COMMAND 0xd2u
#define DP104_LED_COUNT 104u
#define DP104_FRAME_BYTES (DP104_LED_COUNT * 3u)
#define DP104_CHUNK_LEDS 8u
#define DP104_PROTOCOL_MAJOR 1u
#define DP104_LEASE_MS 1000u
#define DP104_CAPS_MAGIC "DP104RGB"

enum dp104_op {
    DP104_OP_CAPS = 0,
    DP104_OP_BEGIN = 1,
    DP104_OP_DATA = 2,
    DP104_OP_COMMIT = 3,
    DP104_OP_RELEASE = 4,
    DP104_OP_STATUS = 5
};

enum dp104_status {
    DP104_OK = 0,
    DP104_BAD_LENGTH = 1,
    DP104_BAD_STATE = 2,
    DP104_BAD_INDEX = 3,
    DP104_BAD_CRC = 4,
    DP104_BUSY = 5,
    DP104_UNSUPPORTED = 6,
    DP104_BAD_REQUEST = 7,
    DP104_INCOMPLETE = 8
};

/* All multibyte fields are little endian; colors on the wire are R,G,B.
 * Common header: command, operation, transaction, request0/response status.
 * CAPS response: [4..11] magic, [12] major, [13] minor, [14..15] LEDs,
 * [16] chunk LEDs, [17] flags(bit0 = end-to-end receiver ready),
 * [18..19] lease milliseconds. Reserved bytes must be zero.
 * BEGIN: [4..5] LED count, [6..7] zero, [8..11] whole-frame CRC32.
 * DATA: [4..5] first LED, [6] count(1..8), [7..] RGB triples.
 * COMMIT/RELEASE/STATUS: no payload. BEGIN/DATA/COMMIT share transaction.
 * STATUS response: [4] direct active, [5] staging active, [6] active txn.
 */

static inline uint16_t dp104_read16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static inline uint32_t dp104_read32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static inline void dp104_write16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static inline void dp104_write32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static inline uint32_t dp104_crc32(const uint8_t *data, unsigned length)
{
    uint32_t crc = 0xffffffffu;
    unsigned i, bit;
    for(i = 0; i < length; ++i)
    {
        crc ^= data[i];
        for(bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

#endif
