#ifndef DP104_LZ_H
#define DP104_LZ_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Decode the bounded DP104 LZ token stream into exactly dst_len bytes.
 *
 * Each group begins with a flags byte consumed least-significant bit first.
 * A zero bit copies one literal byte; a one bit copies a two-byte little-
 * endian match. For a match, the high 12 bits encode distance - 1 and the
 * low 4 bits encode length - 3. Match copies may overlap. Unused high flag
 * bits in the final partial group must be zero.
 *
 * Returns 1 only when the stream is valid, fully consumed, and produces
 * exactly dst_len bytes; malformed input returns 0. A pointer may be NULL
 * only when its corresponding length is zero. Buffers must not overlap.
 * Partial writes inside dst are permitted on failure. This routine does not
 * allocate memory or call libc.
 */
int dp104_lz_decode(const uint8_t *src, size_t src_len,
                    uint8_t *dst, size_t dst_len);

#ifdef __cplusplus
}
#endif

#endif
