#include "dp104_twiddle.h"
#include <stdio.h>
#include <string.h>

#define CHECK(x) do { if(!(x)) { fprintf(stderr, "line %d: %s\n", __LINE__, #x); return 1; } } while(0)

int main(void)
{
    uint32_t basis[DP104_TWIDDLE_WORDS] = {0};
    uint32_t guarded[DP104_TWIDDLE_WORDS + 2u];
    uint32_t *output = guarded + 1;
    memset(guarded, 0xa5, sizeof(guarded));
    basis[1] = UINT32_C(0x3f800000);
    basis[4] = UINT32_C(0x11223344);
    basis[5] = UINT32_C(0x55667788);
    basis[2044] = UINT32_C(0x3bc90f88);
    basis[2045] = UINT32_C(0xbf7ffec4);
    CHECK(dp104_twiddle_rebuild(output, DP104_TWIDDLE_WORDS, basis, DP104_TWIDDLE_WORDS));
    CHECK(guarded[0] == UINT32_C(0xa5a5a5a5));
    CHECK(guarded[DP104_TWIDDLE_WORDS + 1u] == UINT32_C(0xa5a5a5a5));
    CHECK(output[0] == UINT32_C(0x3f800000) && output[1] == 0);
    CHECK(output[2] == UINT32_C(0x55667788) && output[3] == UINT32_C(0x11223344));
    CHECK(output[1024] == UINT32_C(0xbf800000) && output[1025] == 0);
    CHECK(output[2046] == UINT32_C(0x3f7ffec4) && output[2047] == UINT32_C(0xbbc90f88));
    CHECK(!dp104_twiddle_rebuild(basis, DP104_TWIDDLE_WORDS, basis, DP104_TWIDDLE_WORDS));
    CHECK(!dp104_twiddle_rebuild(basis + 1, DP104_TWIDDLE_WORDS, basis, DP104_TWIDDLE_WORDS));
    CHECK(!dp104_twiddle_rebuild(output, DP104_TWIDDLE_WORDS - 1u, basis, DP104_TWIDDLE_WORDS));
    CHECK(!dp104_twiddle_rebuild(0, DP104_TWIDDLE_WORDS, basis, DP104_TWIDDLE_WORDS));
    CHECK(basis[1] == UINT32_C(0x3f800000));
    return 0;
}
