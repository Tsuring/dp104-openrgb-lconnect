#include "dp104_lz.h"

int dp104_lz_decode(const uint8_t *src, size_t src_len,
                    uint8_t *dst, size_t dst_len)
{
    size_t input = 0;
    size_t output = 0;

    if ((src_len != 0 && src == NULL) || (dst_len != 0 && dst == NULL))
        return 0;

    while (output < dst_len)
    {
        uint8_t flags;
        size_t token_index;

        if (input >= src_len)
            return 0;

        flags = src[input++];
        for (token_index = 0; token_index < 8u && output < dst_len;
             ++token_index)
        {
            if ((flags & (uint8_t)(1u << token_index)) == 0)
            {
                if (input >= src_len)
                    return 0;
                dst[output++] = src[input++];
            }
            else
            {
                uint16_t match;
                size_t distance;
                size_t count;
                size_t copied;

                if (src_len - input < 2u)
                    return 0;

                match = (uint16_t)src[input] |
                        (uint16_t)((uint16_t)src[input + 1u] << 8);
                input += 2u;

                distance = (size_t)(match >> 4) + 1u;
                count = (size_t)(match & 0x0fu) + 3u;
                if (distance > output || count > dst_len - output)
                    return 0;

                for (copied = 0; copied < count; ++copied)
                {
                    dst[output] = dst[output - distance];
                    ++output;
                }
            }
        }

        if (output == dst_len && token_index < 8u)
        {
            uint8_t unused_flags = (uint8_t)(flags >> token_index);
            if (unused_flags != 0)
                return 0;
        }
    }

    return input == src_len;
}
