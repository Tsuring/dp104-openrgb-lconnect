#ifndef DP104_TWIDDLE_H
#define DP104_TWIDDLE_H
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DP104_TWIDDLE_WORDS 2048u
/* Reconstruct the pinned PLC's 1024-point complex coefficient table from its
 * existing 2048-point real-FFT table. Inputs/outputs are IEEE-754 bit patterns;
 * no floating-point calculation is performed. This relationship is specific
 * to the byte-verified target tables, not arbitrary coefficient libraries.
 * Both buffers must have 2048 words, be aligned and nonoverlapping. */
int dp104_twiddle_rebuild(uint32_t *output, size_t output_words,
                          const uint32_t *basis, size_t basis_words);

#ifdef __cplusplus
}
#endif
#endif
