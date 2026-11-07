// A made-up game in host memory, laid out as tests/fixtures/game_table.yaml says.
// Tests build objects, classes and names in it, then run the framework's base
// code against it. It also supplies the three functions game.h leaves open.
#pragma once
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "game.h"
#include "names.h"

namespace fake {

namespace t = tsuchi::game::table;

// Optional workload measurement: count reads of one selected address without changing its value.
inline uint64_t measured_read_address = 0;
inline uint64_t measured_reads = 0;

struct Range {
    uint64_t start, end;
    bool writable;
};

inline std::vector<Range>& ranges() {
    static std::vector<Range> list;
    return list;
}
inline std::vector<std::unique_ptr<unsigned char[]>>& blocks() {
    static std::vector<std::unique_ptr<unsigned char[]>> list;
    return list;
}

// Zeroed memory the framework is allowed to read (and write, unless told otherwise).
inline uint64_t memory(size_t bytes, bool writable = true) {
    blocks().push_back(std::make_unique<unsigned char[]>(bytes));
    std::memset(blocks().back().get(), 0, bytes);
    const auto start = reinterpret_cast<uint64_t>(blocks().back().get());
    ranges().push_back({start, start + bytes, writable});
    return start;
}
inline void protect(uint64_t address, bool writable) {
    for (Range& r : ranges())
        if (address >= r.start && address < r.end) r.writable = writable;
}
inline void unmap(uint64_t address) {
    for (size_t i = 0; i < ranges().size(); ++i)
        if (address >= ranges()[i].start && address < ranges()[i].end) return void(ranges().erase(ranges().begin() + i));
}

template <class T>
void put(uint64_t address, T value) {
    std::memcpy(reinterpret_cast<void*>(address), &value, sizeof(T));
}
template <class T>
T get(uint64_t address) {
    T value;
    std::memcpy(&value, reinterpret_cast<const void*>(address), sizeof(T));
    return value;
}

// Addresses the made-up game treats as functions.
inline std::vector<uint64_t>& code() {
    static std::vector<uint64_t> list;
    return list;
}

inline uint64_t& main_image() {
    static uint64_t image = 0;
    return image;
}

inline uint32_t& next_name() {
    static uint32_t index = 1;
    return index;
}

// The game's globals: an object table with room for max_objects, and the name blocks.
inline void start() {
    ranges().clear();
    blocks().clear();
    code().clear();
    next_name() = 1;
    main_image() = memory(0x4000);
    const uint64_t chunk_count = t::constant::max_objects / t::constant::objects_per_chunk;
    const uint64_t chunks = memory(8 * chunk_count);
    put<uint64_t>(main_image() + t::data::object_chunks, chunks);
    put<int32_t>(main_image() + t::data::object_count, 0);
    put<int32_t>(main_image() + t::data::object_serial_counter, 1000);
    for (uint64_t block = 0; block < t::constant::name_block_count; ++block)
        put<uint64_t>(main_image() + t::data::name_blocks + 8 * block,
                      memory((t::constant::name_stride << t::constant::names_per_block_shift) + 2304)); // room for the longest name to start in the last place
}

// Stores a name and returns it. Text with a character above 0x7F is stored 16-bit.
inline tsuchi::game::Name name(const std::u16string& text, uint32_t number = 0) {
    bool wide = false;
    for (char16_t c : text) wide = wide || c > 0x7F;
    const uint32_t index = next_name();
    const uint32_t per_block = 1u << t::constant::names_per_block_shift;
    const size_t bytes = t::name_entry::text + text.size() * (wide ? 2 : 1);
    const uint64_t places = (bytes + t::constant::name_stride - 1) / t::constant::name_stride;
    const uint64_t capacity = uint64_t(t::constant::name_block_count) * per_block;
    if (index >= capacity || places > capacity - index) {
        std::fputs("fake name pool exhausted\n", stderr);
        return {};
    }
    const uint64_t block = get<uint64_t>(main_image() + t::data::name_blocks + 8 * uint64_t(index / per_block));
    const uint64_t entry = block + t::constant::name_stride * uint64_t(index % per_block);
    if (!tsuchi::game::writable(entry, bytes)) {
        std::fputs("fake name record exceeds its block\n", stderr);
        return {};
    }
    put<uint16_t>(entry + t::name_entry::header,
                  uint16_t(text.size() << t::constant::name_length_shift | (wide ? t::constant::name_wide_bit : 0)));
    for (size_t i = 0; i < text.size(); ++i) {
        if (wide) put<uint16_t>(entry + t::name_entry::text + 2 * i, uint16_t(text[i]));
        else put<uint8_t>(entry + t::name_entry::text + i, uint8_t(text[i]));
    }
    next_name() += uint32_t(places);
    return {index, number};
}

inline uint64_t item_at(int32_t index) {
    if (index < 0 || uint64_t(index) >= t::constant::max_objects) {
        std::fprintf(stderr, "fake object capacity exhausted: index=%d max_objects=%llu object_count=%d next_name=%u\n",
                     index, static_cast<unsigned long long>(t::constant::max_objects),
                     get<int32_t>(main_image() + t::data::object_count), next_name());
        std::abort();
    }
    const uint64_t chunks = get<uint64_t>(main_image() + t::data::object_chunks);
    const uint64_t slot = chunks + 8 * (uint64_t(index) / t::constant::objects_per_chunk);
    if (!get<uint64_t>(slot)) put<uint64_t>(slot, memory(t::object_item::size * t::constant::objects_per_chunk));
    return get<uint64_t>(slot) + t::object_item::size * (uint64_t(index) % t::constant::objects_per_chunk);
}

// Puts an existing object into a table slot, as the game does when it creates one.
inline void place(uint64_t object_address, int32_t index) {
    put<int32_t>(object_address + t::object::index, index);
    const uint64_t item = item_at(index);
    put<uint64_t>(item + t::object_item::object, object_address);
    put<uint32_t>(item + t::object_item::flags, 0);
    put<int32_t>(item + t::object_item::serial, 0);
    const int32_t count = get<int32_t>(main_image() + t::data::object_count);
    if (index >= count) put<int32_t>(main_image() + t::data::object_count, index + 1);
}

// A new object at the next free index. `type` is its class; 0 makes it its own class.
inline uint64_t object(const std::u16string& object_name, uint64_t type, size_t bytes = 0x80) {
    const uint64_t address = memory(bytes < t::object::size ? t::object::size : bytes);
    put(address + t::object::name, name(object_name));
    put<uint64_t>(address + t::object::class_, type ? type : address);
    place(address, get<int32_t>(main_image() + t::data::object_count));
    return address;
}

// Removes an object from the table, as the game does when it destroys one.
inline void destroy(uint64_t object_address) {
    const uint64_t item = item_at(get<int32_t>(object_address + t::object::index));
    put<uint64_t>(item + t::object_item::object, 0);
    put<int32_t>(item + t::object_item::serial, 0);
}

inline void set_super(uint64_t type, uint64_t super) { put<uint64_t>(type + t::struct_type::super, super); }

// The start of the patch's block as a patch leaves it: the patch built from this table, or one that
// carries another hash. The framework sees it the next time it looks for the patch.
inline void patch_mark(uint64_t hash = t::stub::patch_hash) {
    const uint64_t mark[2] = {t::stub::patch_magic, hash};
    std::memcpy(reinterpret_cast<void*>(main_image() + t::stub::block), mark, sizeof mark);
}
// The same place in a game that has no patch, or one from before patches said what they were made from.
inline void no_patch_mark() { std::memset(reinterpret_cast<void*>(main_image() + t::stub::block), 0, 2 * sizeof(uint64_t)); }
// A code site as the game shipped it, or as the patch leaves it: a jump, or a call, to the site's entry in the block.
inline void patch_site(unsigned site, bool patched = true) {
    const t::CodeSite& info = t::code_sites[site];
    if (!t::present(info.address)) return; // a site of another's place (same_as) is not in the code
    const uint32_t jump = (info.call ? 0x94000000u : 0x14000000u) | (uint32_t((info.entry - info.address) >> 2) & 0x03FFFFFFu);
    put<uint32_t>(main_image() + info.address, patched ? jump : info.instruction);
}

} // namespace fake

// What game.h leaves to the build.
namespace tsuchi::game {
inline bool inside(uint64_t address, uint64_t bytes, bool need_write) {
    if (!address || bytes > ~uint64_t(0) - address) return false;
    for (const fake::Range& r : fake::ranges())
        if (address >= r.start && address + bytes <= r.end) return !need_write || r.writable;
    return false;
}
uint64_t main_base() { return fake::main_image(); }
bool executable(uint64_t address) {
    for (uint64_t known : fake::code())
        if (known == address) return true;
    return false;
}
bool readable(uint64_t address, uint64_t bytes) {
    if (address == fake::measured_read_address) ++fake::measured_reads;
    return inside(address, bytes, false);
}
bool writable(uint64_t address, uint64_t bytes) { return inside(address, bytes, true); }
} // namespace tsuchi::game
