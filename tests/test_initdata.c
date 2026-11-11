#include "dp104_initdata.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(x) do { if(!(x)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); exit(1); } } while(0)

static void test_literal_length(size_t length)
{
    uint8_t encoded[131];
    uint8_t decoded[129];
    size_t input_length;
    size_t i;

    if (length <= 128)
    {
        encoded[0] = (uint8_t)(length - 1u);
        memset(encoded + 1, 0x55, length);
        input_length = length + 1u;
    }
    else
    {
        encoded[0] = 0x7f;
        memset(encoded + 1, 0x55, 128);
        encoded[129] = 0;
        encoded[130] = 0x56;
        input_length = 131;
    }

    CHECK(dp104_initdata_decode(encoded, input_length, decoded, length));
    for (i = 0; i < length; ++i)
        CHECK(decoded[i] == (i == 128 ? 0x56 : 0x55));
}

static void test_zero_length(size_t length)
{
    uint8_t encoded[2];
    uint8_t decoded[129];
    size_t input_length;
    size_t i;

    if (length <= 128)
    {
        encoded[0] = (uint8_t)(0x80u + length - 1u);
        input_length = 1;
    }
    else
    {
        encoded[0] = 0xff;
        encoded[1] = 0x80;
        input_length = 2;
    }

    memset(decoded, 0xa5, sizeof(decoded));
    CHECK(dp104_initdata_decode(encoded, input_length, decoded, length));
    for (i = 0; i < length; ++i)
        CHECK(decoded[i] == 0);
}

static void test_mixed_and_canaries(void)
{
    static const uint8_t encoded[] = {
        0x02, 'A', 'B', 'C', 0x83, 0x01, 'X', 'Y'
    };
    static const uint8_t expected[] = {
        'A', 'B', 'C', 0, 0, 0, 0, 'X', 'Y'
    };
    uint8_t guarded[11];

    memset(guarded, 0xa5, sizeof(guarded));
    CHECK(dp104_initdata_decode(encoded, sizeof(encoded), guarded + 1,
                                sizeof(expected)));
    CHECK(guarded[0] == 0xa5 && guarded[10] == 0xa5);
    CHECK(memcmp(guarded + 1, expected, sizeof(expected)) == 0);
}

static void test_rejections_and_bounds(void)
{
    static const uint8_t short_literal[] = { 0x02, 'A' };
    static const uint8_t short_output[] = { 0x00, 'A' };
    static const uint8_t oversized_zero[] = { 0x81 };
    static const uint8_t valid_then_trailing[] = { 0x00, 'A', 0x80 };
    uint8_t guarded[4] = { 0xa5, 0xa5, 0xa5, 0xa5 };

    CHECK(dp104_initdata_decode(NULL, 0, NULL, 0));
    CHECK(!dp104_initdata_decode(NULL, 1, NULL, 0));
    CHECK(!dp104_initdata_decode(short_output, sizeof(short_output), NULL, 1));
    CHECK(!dp104_initdata_decode(short_literal, sizeof(short_literal), guarded + 1, 3));
    CHECK(guarded[0] == 0xa5 && guarded[1] == 0xa5 &&
          guarded[2] == 0xa5 && guarded[3] == 0xa5);
    CHECK(!dp104_initdata_decode(short_output, sizeof(short_output), guarded + 1, 2));
    CHECK(guarded[0] == 0xa5 && guarded[3] == 0xa5);

    memset(guarded, 0xa5, sizeof(guarded));
    CHECK(!dp104_initdata_decode(oversized_zero, sizeof(oversized_zero), guarded + 1, 1));
    CHECK(guarded[0] == 0xa5 && guarded[1] == 0xa5 &&
          guarded[2] == 0xa5 && guarded[3] == 0xa5);
    CHECK(!dp104_initdata_decode(valid_then_trailing, sizeof(valid_then_trailing), guarded + 1, 1));
    CHECK(guarded[0] == 0xa5 && guarded[2] == 0xa5 && guarded[3] == 0xa5);
}

int main(void)
{
    test_zero_length(1);
    test_zero_length(127);
    test_zero_length(128);
    test_zero_length(129);
    test_literal_length(1);
    test_literal_length(127);
    test_literal_length(128);
    test_literal_length(129);
    test_mixed_and_canaries();
    test_rejections_and_bounds();
    puts("initdata: bounded decode, boundaries, malformed streams and canaries PASS");
    return 0;
}
