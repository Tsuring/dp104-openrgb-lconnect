#ifndef DP104_WIRE_H
#define DP104_WIRE_H

#include <stddef.h>
#include <stdint.h>
#include "dp104_protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Proposed extension. Stock DP104 1.2.0 does not implement these requests. */
#define DP104_WIRE_TYPE 0x7du
#define DP104_WIRE_HEADER_BYTES 3u
#define DP104_WIRE_ENCODED_BYTES 43u
#define DP104_WIRE_MAX_ENCODED_BYTES DP104_WIRE_ENCODED_BYTES
#define DP104_WIRE_SYMBOL_MIN 0x21u
#define DP104_WIRE_SYMBOL_MAX 0x60u
#define DP104_WIRE_BYTES (DP104_WIRE_HEADER_BYTES + DP104_WIRE_MAX_ENCODED_BYTES)
#define DP104_WIRE_REQUEST 1u
#define DP104_WIRE_RESPONSE 0u

/* Internal frame: direction, 0x7d, fixed length 43, then 43 six-bit symbols.
 * Each symbol is value+0x21 (range 0x21..0x60), so no direction byte or
 * escape marker can occur in the payload. The final symbol's low 2 bits are 0. */
int dp104_wire_encode(uint8_t direction, const uint8_t *payload, size_t payload_size,
                      uint8_t *frame, size_t capacity);
size_t dp104_wire_frame_size(const uint8_t *frame, size_t available);
int dp104_wire_decode(uint8_t direction, const uint8_t *frame, size_t frame_size,
                      uint8_t *payload, size_t capacity);

typedef void (*dp104_wire_sink)(void *context, const uint8_t *data, size_t length);
/* Route a complete batch. Legacy record bytes/order are preserved. Reserved
 * 7D records in directions 0/1 are consumed even when malformed; they never
 * fall through to the stock dispatcher. The extension sink receives the full
 * encoded envelope, not a decoded payload. */
unsigned dp104_wire_route_batch(uint8_t direction, const uint8_t *batch, size_t length,
                                dp104_wire_sink extension, dp104_wire_sink legacy,
                                void *context);

#ifdef __cplusplus
}
#endif
#endif
