// Executes the real platform adapter with an ARM64 stand-in kernel and allocator.
#include "game.h"
#include "native_hooks.h"
#include "platform.h"
#include <stddef.h>

extern "C" {
uint64_t native_main_address = 0x40000000;
uint64_t native_alloc_pointer = 0x90000100;
uint64_t native_alloc_bytes = 0, native_alloc_count = 0, native_free_count = 0;
uint64_t native_nested_on = 0, native_nested_count = 0, native_nested_ok_count = 0;
uint64_t native_nested_allocate_result = ~uint64_t(0), native_nested_free_result = ~uint64_t(0);
uint64_t native_arena_test(tsuchi::CodeArena* out);
void native_arena_stats(tsuchi::CodeMemoryStats* out);

static void nested_arena(unsigned point) {
    if (!(native_nested_on & point)) return;
    tsuchi::CodeMemoryStats before{}, after{};
    native_arena_stats(&before);
    tsuchi::CodeArena out{~uint64_t(0), reinterpret_cast<void*>(~uint64_t(0)), ~uint64_t(0)};
    const uint64_t result = native_arena_test(&out);
    native_arena_stats(&after);
    ++native_nested_count;
    if (point == 1) native_nested_allocate_result = result;
    else native_nested_free_result = result;
    bool preserved = true;
    for (size_t i = 0; i < sizeof before; ++i)
        preserved = preserved && reinterpret_cast<const unsigned char*>(&before)[i] ==
                                 reinterpret_cast<const unsigned char*>(&after)[i];
    if (!result && !out.rx && !out.rw && !out.bytes && preserved) ++native_nested_ok_count;
}
void* memcpy(void* to, const void* from, size_t bytes) {
    auto* destination = static_cast<volatile unsigned char*>(to);
    const auto* source = static_cast<const volatile unsigned char*>(from);
    for (size_t i = 0; i < bytes; ++i) destination[i] = source[i];
    return to;
}
void* memset(void* to, int value, size_t bytes) {
    auto* destination = static_cast<volatile unsigned char*>(to);
    for (size_t i = 0; i < bytes; ++i) destination[i] = static_cast<unsigned char>(value);
    return to;
}
__attribute__((noinline)) void* native_engine_allocate(uint64_t bytes) {
    native_alloc_bytes = bytes;
    ++native_alloc_count;
    nested_arena(1);
    return reinterpret_cast<void*>(native_alloc_pointer);
}
__attribute__((noinline)) void native_engine_free(void* block) {
    nested_arena(2);
    // This read faults if code-memory rollback has not restored heap access.
    const uint64_t address = reinterpret_cast<uint64_t>(block);
    if (address >= 0x90000000 && address < 0x90006000)
        (void)*reinterpret_cast<const volatile unsigned char*>((address + 4095) & ~uint64_t(4095));
    ++native_free_count;
}
}

namespace tsuchi {
CodeMemory captured{};
void native_hooks_start(CodeMemory memory) { captured = memory; }
}
namespace tsuchi::game {
uint64_t main_base() { return native_main_address; }
bool executable(uint64_t at) {
    return at >= native_main_address && at - native_main_address < table::constant::code_size;
}
void* engine_memory(uint64_t bytes) { return native_engine_allocate(bytes); }
void release_engine_memory(void* block) { native_engine_free(block); }
}
extern "C" uint64_t native_arena_test(tsuchi::CodeArena* out) {
    tsuchi::target::code_memory_start();
    if (out) *out = {};
    return tsuchi::captured.gateway_arena && tsuchi::captured.gateway_arena(out);
}
extern "C" void native_arena_stats(tsuchi::CodeMemoryStats* out) {
    tsuchi::target::code_memory_start();
    if (tsuchi::captured.stats) tsuchi::captured.stats(out);
}
extern "C" uint64_t native_arena_alias(uint64_t at) {
    tsuchi::target::code_memory_start();
    return tsuchi::captured.alias ? reinterpret_cast<uint64_t>(tsuchi::captured.alias(at, 4)) : 0;
}
