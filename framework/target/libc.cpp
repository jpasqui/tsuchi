// The C library routines declared in include/string.h. The loops go through
// volatile pointers so the compiler does not turn them back into calls to
// themselves.
#include <string.h>

extern "C" {

void* memcpy(void* to, const void* from, size_t bytes) {
    auto* out = static_cast<volatile unsigned char*>(to);
    auto* in = static_cast<const volatile unsigned char*>(from);
    for (size_t i = 0; i < bytes; ++i) out[i] = in[i];
    return to;
}

void* memmove(void* to, const void* from, size_t bytes) {
    auto* out = static_cast<volatile unsigned char*>(to);
    auto* in = static_cast<const volatile unsigned char*>(from);
    if (out < in) {
        for (size_t i = 0; i < bytes; ++i) out[i] = in[i];
    } else {
        for (size_t i = bytes; i-- > 0;) out[i] = in[i];
    }
    return to;
}

void* memset(void* to, int value, size_t bytes) {
    auto* out = static_cast<volatile unsigned char*>(to);
    for (size_t i = 0; i < bytes; ++i) out[i] = static_cast<unsigned char>(value);
    return to;
}

int memcmp(const void* a, const void* b, size_t bytes) {
    auto* x = static_cast<const volatile unsigned char*>(a);
    auto* y = static_cast<const volatile unsigned char*>(b);
    for (size_t i = 0; i < bytes; ++i) {
        const int difference = int(x[i]) - int(y[i]);
        if (difference) return difference;
    }
    return 0;
}

size_t strlen(const char* text) {
    auto* at = static_cast<const volatile char*>(text);
    size_t length = 0;
    while (at[length]) ++length;
    return length;
}

int strcmp(const char* a, const char* b) {
    auto* x = static_cast<const volatile unsigned char*>(static_cast<const void*>(a));
    auto* y = static_cast<const volatile unsigned char*>(static_cast<const void*>(b));
    size_t i = 0;
    while (x[i] && x[i] == y[i]) ++i;
    return int(x[i]) - int(y[i]);
}

} // extern "C"
