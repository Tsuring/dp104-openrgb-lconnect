#ifndef DP104_WIRE_STREAM_H
#define DP104_WIRE_STREAM_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DP104_WIRE_STREAM_CAPACITY 256u
#define DP104_WIRE_STREAM_FRAME_MAX 67u

typedef struct dp104_wire_stream {
    uint8_t data[DP104_WIRE_STREAM_CAPACITY];
    uint16_t used;
    uint8_t dropping;
} dp104_wire_stream;

void dp104_wire_stream_init(dp104_wire_stream *stream);
void dp104_wire_stream_reset(dp104_wire_stream *stream);
/* Returns 1 when appended/no-op; 0 for invalid input or overflow. Overflow
 * clears buffered bytes and enters bounded resynchronization/drop mode. */
int dp104_wire_stream_push(dp104_wire_stream *stream,
                           const uint8_t *bytes, size_t length);
/* Returns 1 for a complete record, 0 when more bytes are needed, and -1 for
 * invalid arguments/too-small output capacity (without consuming a record).
 * Extension output is the complete encoded envelope, not decoded payload. */
int dp104_wire_stream_next(dp104_wire_stream *stream,
                           uint8_t *out, size_t capacity,
                           size_t *out_length, uint8_t *is_extension);

#ifdef __cplusplus
}
#endif
#endif
