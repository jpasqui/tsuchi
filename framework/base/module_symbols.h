// The dynamic symbols of a program the system has loaded, read from its module header. This
// takes no number from the game table, so it works on a build the table does not describe.
#pragma once
#include <stdint.h>
#include <string.h>

#include "game.h"

namespace tsuchi::game {

struct LoadedModule {
    uint64_t base = 0;
    uint64_t symbols = 0, strings = 0;
    uint32_t symbol_count = 0;
    uint64_t jumps = 0, jump_bytes = 0;
};

// What a loaded module says of itself, or false when `base` is not the start of one.
inline bool read_module(uint64_t base, LoadedModule& out) {
    constexpr uint64_t kNull = 0, kPltBytes = 2, kHash = 4, kStrings = 5, kSymbols = 6, kJumps = 23;
    constexpr uint32_t kMost = 256;
    uint32_t header = 0, magic = 0;
    int32_t dynamic = 0;
    if (!read(base + 4, header) || !read(base + header, magic) || magic != 0x30444F4D || !read(base + header + 4, dynamic)) return false;
    out = LoadedModule{};
    out.base = base;
    uint64_t hash = 0;
    const uint64_t list = base + header + int64_t(dynamic);
    const auto at = [&](uint64_t value) { return value >= base ? value : base + value; };
    for (uint32_t i = 0; i < kMost; ++i) {
        uint64_t tag = 0, value = 0;
        if (!read(list + 16 * i, tag) || !read(list + 16 * i + 8, value)) return false;
        if (tag == kNull) break;
        if (tag == kHash) hash = at(value);
        if (tag == kStrings) out.strings = at(value);
        if (tag == kSymbols) out.symbols = at(value);
        if (tag == kJumps) out.jumps = at(value);
        if (tag == kPltBytes) out.jump_bytes = value;
    }
    if (!out.strings || !out.symbols) return false;
    if (hash) return read(hash + 4, out.symbol_count);
    // A module with only the GNU hash table, as the SDK of the Switch 2 builds has: its symbols end where its strings
    // start, in every linker's layout.
    if (out.strings <= out.symbols || (out.strings - out.symbols) % 24 || (out.strings - out.symbols) / 24 > UINT32_MAX) return false;
    out.symbol_count = uint32_t((out.strings - out.symbols) / 24);
    return true;
}

// Whether the name of a symbol of the module is `name`.
inline bool symbol_named(const LoadedModule& module, uint32_t symbol, const char* name) {
    uint32_t offset = 0;
    if (!read(module.symbols + 24ull * symbol, offset)) return false;
    const uint64_t bytes = strlen(name) + 1;
    return readable(module.strings + offset, bytes) && !memcmp(reinterpret_cast<const void*>(module.strings + offset), name, bytes);
}

// The address of what the module defines under `name`, or 0.
inline uint64_t module_export(const LoadedModule& module, const char* name) {
    for (uint32_t i = 0; i < module.symbol_count; ++i) {
        uint16_t section = 0;
        uint64_t value = 0;
        const uint64_t symbol = module.symbols + 24ull * i;
        if (read(symbol + 6, section) && section && read(symbol + 8, value) && symbol_named(module, i, name)) return module.base + value;
    }
    return 0;
}

// Where the module keeps the address of what it imports under `name`, or 0.
inline uint64_t module_import_slot(const LoadedModule& module, const char* name) {
    for (uint64_t at = 0; at + 24 <= module.jump_bytes; at += 24) {
        uint64_t slot = 0, info = 0;
        if (read(module.jumps + at, slot) && read(module.jumps + at + 8, info) && symbol_named(module, uint32_t(info >> 32), name)) return module.base + slot;
    }
    return 0;
}

} // namespace tsuchi::game
