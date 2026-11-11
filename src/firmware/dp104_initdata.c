#include "dp104_initdata.h"

int dp104_initdata_decode(const uint8_t *src, size_t src_len,
                          uint8_t *dst, size_t dst_len)
{
    size_t input = 0;
    size_t output = 0;

    if ((src_len != 0 && src == NULL) || (dst_len != 0 && dst == NULL))
        return 0;

    while (input < src_len)
    {
        uint8_t token = src[input++];
        size_t count = (size_t)(token & 0x7fu) + 1u;
        size_t i;

        if (count > dst_len - output)
            return 0;

        if ((token & 0x80u) != 0)
        {
            for (i = 0; i < count; ++i)
                dst[output + i] = 0;
        }
        else
        {
            if (count > src_len - input)
                return 0;

            for (i = 0; i < count; ++i)
                dst[output + i] = src[input + i];
            input += count;
        }

        output += count;
    }

    return output == dst_len;
}
