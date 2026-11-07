/* What the compiler expects to find in any program, for a compiled mod: a mod is linked
 * against nothing, so these are its own. Built with -fno-builtin, so that the loops here are
 * not turned back into calls to the functions they are. */
#include <stddef.h>

#include "tsuchi/core.h"

/* Which version of the framework's headers the mod was built against. The framework reads it before it
 * starts the mod, and does not start one built for a newer framework than it is. */
__attribute__((visibility("default"), used)) const uint32_t tsuchi_mod_built_for = TSUCHI_API_VERSION;

#define HIDDEN __attribute__((visibility("hidden")))

HIDDEN void *memcpy(void *to, const void *from, size_t count) {
    unsigned char *a = to;
    const unsigned char *b = from;
    while (count--) *a++ = *b++;
    return to;
}

HIDDEN void *memmove(void *to, const void *from, size_t count) {
    unsigned char *a = to;
    const unsigned char *b = from;
    if (a < b) {
        while (count--) *a++ = *b++;
    } else {
        a += count, b += count;
        while (count--) *--a = *--b;
    }
    return to;
}

HIDDEN void *memset(void *to, int value, size_t count) {
    unsigned char *a = to;
    while (count--) *a++ = (unsigned char)value;
    return to;
}

HIDDEN int memcmp(const void *one, const void *other, size_t count) {
    const unsigned char *a = one, *b = other;
    for (; count; --count, ++a, ++b)
        if (*a != *b) return *a < *b ? -1 : 1;
    return 0;
}

HIDDEN size_t strlen(const char *text) {
    size_t length = 0;
    while (text[length]) ++length;
    return length;
}

HIDDEN int strcmp(const char *one, const char *other) {
    while (*one && *one == *other) ++one, ++other;
    return (unsigned char)*one < (unsigned char)*other ? -1 : (unsigned char)*one > (unsigned char)*other;
}

/* The mod's global variables are set up once, when the loader starts the module. Nothing runs
 * when it is unloaded: what a mod holds through the framework is released by the framework. */
typedef void (*Setup)(void);
extern HIDDEN Setup __tsuchi_setup_start[], __tsuchi_setup_end[];
HIDDEN void tsuchi_sdk_setup(void) {
    for (Setup *each = __tsuchi_setup_start; each != __tsuchi_setup_end; ++each) (*each)();
}
HIDDEN void *__dso_handle = &__dso_handle;
HIDDEN int __cxa_atexit(void (*function)(void *), void *argument, void *module) {
    (void)function, (void)argument, (void)module;
    return 0;
}
HIDDEN void __cxa_pure_virtual(void) {
    for (;;) {
    }
}
