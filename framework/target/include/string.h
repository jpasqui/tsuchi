/* The few C library routines the framework uses. The game build has no C
 * library, so target/libc.cpp provides them. */
#pragma once
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
void* memcpy(void* to, const void* from, size_t bytes);
void* memmove(void* to, const void* from, size_t bytes);
void* memset(void* to, int value, size_t bytes);
int memcmp(const void* a, const void* b, size_t bytes);
size_t strlen(const char* text);
int strcmp(const char* a, const char* b);
#ifdef __cplusplus
}
#endif
