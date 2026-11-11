#ifndef DP104_FREESTANDING_STRING_H
#define DP104_FREESTANDING_STRING_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

void *memcpy(void *destination, const void *source, size_t count);
void *memmove(void *destination, const void *source, size_t count);
void *memset(void *destination, int value, size_t count);
int memcmp(const void *left, const void *right, size_t count);

#ifdef __cplusplus
}
#endif

#endif
