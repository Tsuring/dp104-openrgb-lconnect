#include "dp104_lz.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
        exit(1); \
    } \
} while (0)

static void test_overlap_backcopy_and_canaries(void)
{
    static const uint8_t encoded[] = { 0x02, 'A', 0x0f, 0x00 };
    uint8_t guarded[21];
    size_t i;

    memset(guarded, 0xa5, sizeof(guarded));
    CHECK(dp104_lz_decode(encoded, sizeof(encoded), guarded + 1, 19) == 1);
    CHECK(guarded[0] == 0xa5 && guarded[20] == 0xa5);
    for (i = 1; i < 20; ++i)
        CHECK(guarded[i] == 'A');
}

static void test_literal_group_boundaries(void)
{
    static const uint8_t eight[] = {
        0x00, 'a', 'b', 'c', 'd', 'e', 'f', 'g', 'h'
    };
    static const uint8_t nine[] = {
        0x00, 'a', 'b', 'c', 'd', 'e', 'f', 'g', 'h', 0x00, 'i'
    };
    uint8_t output[9];

    CHECK(dp104_lz_decode(eight, sizeof(eight), output, 8) == 1);
    CHECK(memcmp(output, "abcdefgh", 8) == 0);
    CHECK(dp104_lz_decode(nine, sizeof(nine), output, 9) == 1);
    CHECK(memcmp(output, "abcdefghi", 9) == 0);
}

static void test_empty_and_null_contract(void)
{
    static uint8_t byte = 0;

    CHECK(dp104_lz_decode(NULL, 0, NULL, 0) == 1);
    CHECK(dp104_lz_decode(&byte, 0, NULL, 0) == 1);
    CHECK(dp104_lz_decode(NULL, 0, &byte, 0) == 1);
    CHECK(dp104_lz_decode(NULL, 1, &byte, 0) == 0);
    CHECK(dp104_lz_decode(&byte, 0, NULL, 1) == 0);
    CHECK(dp104_lz_decode(NULL, 0, &byte, 1) == 0);
}

static void test_malformed_streams_and_bounds(void)
{
    static const uint8_t invalid_distance[] = { 0x01, 0x00, 0x00 };
    static const uint8_t truncated_flags[] = { 0x00 };
    static const uint8_t truncated_match[] = { 0x01, 0x00 };
    static const uint8_t unused_flags[] = { 0x80, 'x' };
    static const uint8_t trailing[] = { 0x00, 'x', 0x00 };
    static const uint8_t short_output[] = { 0x00, 'x' };
    static const uint8_t overflow_match[] = { 0x02, 'A', 0x0f, 0x00 };
    uint8_t guarded[20];

    memset(guarded, 0xa5, sizeof(guarded));
    CHECK(dp104_lz_decode(invalid_distance, sizeof(invalid_distance),
                          guarded + 1, 3) == 0);
    CHECK(guarded[0] == 0xa5 && guarded[4] == 0xa5);
    CHECK(dp104_lz_decode(truncated_flags, 0, guarded + 1, 1) == 0);
    CHECK(dp104_lz_decode(truncated_flags, sizeof(truncated_flags),
                          guarded + 1, 1) == 0);
    CHECK(dp104_lz_decode(truncated_match, sizeof(truncated_match),
                          guarded + 1, 3) == 0);

    memset(guarded, 0xa5, sizeof(guarded));
    CHECK(dp104_lz_decode(unused_flags, sizeof(unused_flags),
                          guarded + 1, 1) == 0);

    CHECK(guarded[0] == 0xa5 && guarded[2] == 0xa5);

    memset(guarded, 0xa5, sizeof(guarded));
    CHECK(dp104_lz_decode(trailing, sizeof(trailing), guarded + 1, 1) == 0);

    CHECK(guarded[0] == 0xa5 && guarded[2] == 0xa5);

    memset(guarded, 0xa5, sizeof(guarded));
    CHECK(dp104_lz_decode(short_output, sizeof(short_output),
                          guarded + 1, 2) == 0);

    CHECK(guarded[0] == 0xa5 && guarded[3] == 0xa5);

    memset(guarded, 0xa5, sizeof(guarded));
    CHECK(dp104_lz_decode(overflow_match, sizeof(overflow_match),
                          guarded + 1, 18) == 0);
    CHECK(guarded[0] == 0xa5 && guarded[19] == 0xa5);
}

int main(void)
{
    test_overlap_backcopy_and_canaries();
    test_literal_group_boundaries();
    test_empty_and_null_contract();
    test_malformed_streams_and_bounds();
    puts("lz: overlap, flag groups, malformed streams, nulls and canaries PASS");
    return 0;
}
