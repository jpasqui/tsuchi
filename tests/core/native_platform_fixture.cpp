// Entry points for ARM64 execution of the platform adapter with a stand-in kernel.
#include "game.h"
#include "native_hooks.h"
#include "platform.h"
#include <stddef.h>
extern "C" void* memcpy(void* to, const void* from, size_t bytes) {
    auto* destination = static_cast<volatile unsigned char*>(to);
    const auto* source = static_cast<const volatile unsigned char*>(from);
    for (size_t i = 0; i < bytes; ++i) destination[i] = source[i];
    return to;
}
extern "C" void* memset(void* to, int byte, size_t bytes) {
    auto* destination = static_cast<volatile unsigned char*>(to);
    for (size_t i = 0; i < bytes; ++i) destination[i] = static_cast<unsigned char>(byte);
    return to;
}
namespace tsuchi {
CodeMemory captured{};
void native_hooks_start(CodeMemory memory) { captured = memory; }
}
namespace tsuchi::game {
uint64_t main_base() { return 0x100000; }
bool executable(uint64_t at) { return at >= main_base() && at < main_base() + table::constant::code_size; }
void* engine_memory(uint64_t) { return nullptr; }
void release_engine_memory(void*) {}
}
extern "C" uint64_t native_alias_test(uint64_t at, uint64_t bytes) {
    tsuchi::target::code_memory_start();
    return tsuchi::captured.alias ? reinterpret_cast<uint64_t>(tsuchi::captured.alias(at, bytes)) : 0;
}
extern "C" void native_flush_test(uint64_t alias, uint64_t at, uint64_t bytes) {
    if (tsuchi::captured.flush) tsuchi::captured.flush(reinterpret_cast<void*>(alias), at, bytes);
}
