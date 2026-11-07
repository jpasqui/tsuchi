// Modules of code, as the system the game runs on keeps them: finding the loaded
// module an address is in and what it offers by name, and the two things the
// system's loader has to be given to take a module read from a file: the file's
// bytes, checked, and a list that vouches for them.
#pragma once
#include <stddef.h>
#include <stdint.h>

#include "sha256.h"

namespace tsuchi::game {

constexpr uint64_t kPage = 0x1000;

// Where the loaded module that holds `address` in its code starts, or 0. Only memory between the two is read.
uint64_t module_containing(uint64_t address);
// Where the function or variable a loaded module offers under `name` is, or 0.
uint64_t module_symbol(uint64_t module, const char* name);
// The system's file context that makes every failure an error its caller sees, instead of a stop of the game, from
// the system's library, or null. Older SDKs export it as nn::fs::g_AllReturnContext; the SDK of the Switch 2 builds
// keeps it inside nn::fs::ScopedAutoAbortDisabler, whose constructor makes it the thread's context.
inline const void* returning_fs_context(uint64_t library) {
    if (const uint64_t exported = module_symbol(library, "_ZN2nn2fs18g_AllReturnContextE")) return reinterpret_cast<const void*>(exported);
    const auto get = reinterpret_cast<const void* (*)()>(module_symbol(library, "_ZN2nn2fs25GetCurrentThreadFsContextEv"));
    const auto set = reinterpret_cast<void (*)(const void*)>(module_symbol(library, "_ZN2nn2fs25SetCurrentThreadFsContextEPKNS0_9FsContextE"));
    const auto disable = reinterpret_cast<void (*)(void*)>(module_symbol(library, "_ZN2nn2fs23ScopedAutoAbortDisablerC1Ev"));
    if (!get || !set || !disable) return nullptr;
    // The disabler keeps the context before it in its one word and makes the returning one current; this thread's
    // context is put back at once.
    const void* before = get();
    uint64_t kept = 0;
    disable(&kept);
    const void* returning = get();
    set(before);
    return returning != before ? returning : nullptr;
}

// True for the bytes of a module file the loader can be handed: whole pages, of the size the file says it has,
// with its three parts on page boundaries within it.
bool module_file_valid(const uint8_t* image, uint64_t bytes);

// The list that vouches for one module file by its hash, for the program whose ID is given. It is one page. `wide`
// lays it out with the 3072-bit keys and signatures of the Switch 2 builds' system, which puts the fields after them
// 0x180 bytes on.
void write_module_list(uint8_t* page, uint64_t program, const uint8_t hash[kSha256Bytes], bool wide = false);

} // namespace tsuchi::game
