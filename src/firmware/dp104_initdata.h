#ifndef DP104_INITDATA_H
#define DP104_INITDATA_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Decode the bounded initdata packet stream into exactly dst_len bytes.
 *
 * Tokens with bit 7 set emit (token & 0x7f) + 1 zero bytes. Other tokens copy
 * that many following literal bytes. Returns 1 only when all input is consumed
 * and exactly dst_len bytes are produced; malformed, truncated, or oversized
 * streams return 0.
 *
 * A pointer may be NULL only when its corresponding length is zero. Buffers
 * must not overlap. This routine does not allocate memory or call libc.
 */
int dp104_initdata_decode(const uint8_t *src, size_t src_len,
                          uint8_t *dst, size_t dst_len);

#ifdef __cplusplus
}
#endif

#endif
