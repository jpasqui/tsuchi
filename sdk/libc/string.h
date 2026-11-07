/* The functions of <string.h> a compiled mod has: the ones the SDK's support code defines. */
#ifndef TSUCHI_SDK_STRING_H
#define TSUCHI_SDK_STRING_H
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif

void *memcpy(void *to, const void *from, size_t count);
void *memmove(void *to, const void *from, size_t count);
void *memset(void *to, int value, size_t count);
int memcmp(const void *one, const void *other, size_t count);
size_t strlen(const char *text);
int strcmp(const char *one, const char *other);

#ifdef __cplusplus
}
#endif
#endif
