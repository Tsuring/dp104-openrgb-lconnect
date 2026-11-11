#include <stddef.h>
#include <stdint.h>

void *memcpy(void *destination, const void *source, size_t count)
{
    volatile unsigned char *out = (volatile unsigned char *)destination;
    const volatile unsigned char *in = (const volatile unsigned char *)source;
    size_t index;
    for(index = 0; index < count; ++index)
    {
        out[index] = in[index];
    }
    return destination;
}

void *memmove(void *destination, const void *source, size_t count)
{
    volatile unsigned char *out = (volatile unsigned char *)destination;
    const volatile unsigned char *in = (const volatile unsigned char *)source;

    if((uintptr_t)out < (uintptr_t)in)
    {
        size_t index;
        for(index = 0; index < count; ++index)
        {
            out[index] = in[index];
        }
    }
    else if((uintptr_t)out > (uintptr_t)in)
    {
        while(count != 0)
        {
            --count;
            out[count] = in[count];
        }
    }
    return destination;
}

void *memset(void *destination, int value, size_t count)
{
    volatile unsigned char *out = (volatile unsigned char *)destination;
    size_t index;
    for(index = 0; index < count; ++index)
    {
        out[index] = (unsigned char)value;
    }
    return destination;
}

int memcmp(const void *left, const void *right, size_t count)
{
    const volatile unsigned char *a = (const volatile unsigned char *)left;
    const volatile unsigned char *b = (const volatile unsigned char *)right;
    size_t index;
    for(index = 0; index < count; ++index)
    {
        if(a[index] != b[index])
        {
            return (int)a[index] - (int)b[index];
        }
    }
    return 0;
}

/* Clang emits this ARM EABI helper for aligned zero-initialized local arrays. */
void __aeabi_memclr4(void *destination, size_t count)
{
    (void)memset(destination, 0, count);
}
