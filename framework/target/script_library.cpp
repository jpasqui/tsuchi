// What the C library Lua is built on asks of the module it is in: memory, a clock,
// and somewhere for what would be printed to go.
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "registry.h"

extern "C" {

// A block's size is kept in front of it, since a block that grows has to be copied by whoever knows how much to copy.
void* tsuchi_libc_allocate(size_t bytes) {
    auto* block = static_cast<uint64_t*>(tsuchi::allocate(bytes + 16));
    if (!block) return nullptr;
    block[0] = bytes;
    return block + 2;
}

void tsuchi_libc_release(void* pointer) {
    if (pointer) tsuchi::release(static_cast<uint64_t*>(pointer) - 2);
}

void* tsuchi_libc_reallocate(void* pointer, size_t bytes) {
    if (!pointer) return tsuchi_libc_allocate(bytes);
    const size_t had = static_cast<uint64_t*>(pointer)[-2];
    if (bytes <= had) return pointer;
    void* grown = tsuchi_libc_allocate(bytes);
    if (!grown) return nullptr;
    memcpy(grown, pointer, had);
    tsuchi_libc_release(pointer);
    return grown;
}

// Nothing in the framework prints through the C library: scripts write to the log.
void tsuchi_libc_write(const char*, size_t) {}
void tsuchi_lua_write(const char*, unsigned long) {}
void tsuchi_lua_write_error(const char*, const char*) {}

// Microseconds since the C library was first asked, which is as the first mods start. The system counts
// 19.2 million ticks a second from when the console started, and is asked directly.
unsigned long long tsuchi_libc_ticks() {
    register uint64_t ticks __asm__("x0");
    __asm__ volatile("svc 0x1E" : "=r"(ticks) : : "x1", "x2", "x3", "x4", "x5", "x6", "x7", "memory");
    static uint64_t first = 0;
    if (!first) first = ticks;
    return (ticks - first) * 5 / 96;
}

} // extern "C"
