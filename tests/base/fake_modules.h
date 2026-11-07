// A loaded module as the system keeps one, made up: the two words it starts with, its header, its
// dynamic table, and what it offers by name. Include after fake_game.h.
#pragma once
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace fake {

struct Offered {
    std::string name;
    uint64_t address;     // where it is, anywhere in memory
    bool defined = true;  // false for a name the module only refers to
    uint8_t binding = 1;  // 1 for others to use, 2 the same but weaker, 0 for the module's own use
};

enum class Hashing { plain, gnu, none };

constexpr uint64_t kModulePage = 0x1000;

// Makes the module in memory the framework may read and returns where it starts, on a page boundary.
inline uint64_t loaded_module(const std::vector<Offered>& offered, Hashing hashing = Hashing::plain, size_t pages = 4) {
    const uint64_t base = (memory((pages + 1) * kModulePage) + kModulePage - 1) & ~(kModulePage - 1);
    const uint64_t header = 0x80, dynamic = 0x100, hash = 0x200, symbols = 0x400, strings = 0x1000;
    put<uint32_t>(base + 4, uint32_t(header));
    std::memcpy(reinterpret_cast<void*>(base + header), "MOD0", 4);
    put<int32_t>(base + header + 4, int32_t(dynamic - header));
    uint64_t entry = base + dynamic;
    const auto tag = [&](uint64_t name, uint64_t value) {
        put<uint64_t>(entry, name), put<uint64_t>(entry + 8, value);
        entry += 16;
    };
    tag(0x15, 0); // one the framework has no use for
    tag(6, symbols), tag(5, strings);
    const uint32_t count = uint32_t(offered.size()) + 1; // the first symbol of any table is empty
    if (hashing == Hashing::plain) {
        tag(4, hash);
        put<uint32_t>(base + hash, 1), put<uint32_t>(base + hash + 4, count);
    } else if (hashing == Hashing::gnu) {
        // One bucket whose chain starts at the first real symbol; the last symbol's entry has its low bit set.
        tag(0x6FFFFEF5, hash);
        put<uint32_t>(base + hash, 1), put<uint32_t>(base + hash + 4, 1), put<uint32_t>(base + hash + 8, 1);
        put<uint32_t>(base + hash + 16 + 8, count > 1 ? 1 : 0);
        for (uint32_t i = 1; i < count; ++i) put<uint32_t>(base + hash + 16 + 8 + 4 + 4 * (i - 1), (i * 2) | (i == count - 1 ? 1 : 0));
    }
    uint64_t text = 1; // a name at 0 is no name
    for (uint32_t i = 0; i < offered.size(); ++i) {
        const uint64_t symbol = base + symbols + 24 * uint64_t(i + 1);
        put<uint32_t>(symbol, uint32_t(text));
        put<uint8_t>(symbol + 4, uint8_t(offered[i].binding << 4 | 2));
        put<uint16_t>(symbol + 6, offered[i].defined ? 1 : 0);
        put<uint64_t>(symbol + 8, offered[i].address - base);
        std::memcpy(reinterpret_cast<void*>(base + strings + text), offered[i].name.c_str(), offered[i].name.size() + 1);
        text += offered[i].name.size() + 1;
    }
    return base;
}

} // namespace fake
