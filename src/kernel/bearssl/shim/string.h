#ifndef AETHER_BEARSSL_STRING_H
#define AETHER_BEARSSL_STRING_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

void* memcpy(void* dest, const void* src, size_t len);
void* memmove(void* dest, const void* src, size_t len);
void* memset(void* dest, int val, size_t len);
int memcmp(const void* s1, const void* s2, size_t len);
size_t strlen(const char* str);

#ifdef __cplusplus
}
#endif

#endif
