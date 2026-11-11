#include "dp104_twiddle.h"

int dp104_twiddle_rebuild(uint32_t *output, size_t output_words,
                          const uint32_t *basis, size_t basis_words)
{
    unsigned k;
    uintptr_t out, in;
    if(!output || !basis || output_words != DP104_TWIDDLE_WORDS ||
       basis_words != DP104_TWIDDLE_WORDS) return 0;
    out = (uintptr_t)output;
    in = (uintptr_t)basis;
    if((out | in) & 3u) return 0;
    if((out >= in ? out - in : in - out) < DP104_TWIDDLE_WORDS * sizeof(uint32_t))
        return 0;
    for(k = 0; k < 1024u; ++k)
    {
        const unsigned j = (k & 511u) * 4u;
        const uint32_t sign = k >= 512u ? UINT32_C(0x80000000) : 0;
        output[k * 2u] = basis[j + 1u] ^ sign;
        output[k * 2u + 1u] = basis[j] ^ sign;
    }
    /* The original table uses positive zero at this exact half-turn entry. */
    output[1025] = 0;
    return 1;
}
