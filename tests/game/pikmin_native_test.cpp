// Native helper policy checks with bounded game-memory and engine callbacks.
// This does not establish the native allocator or enum registry in the game.
#include <cstdio>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <string>
#include <vector>
#include "game.h"
#include "names.h"
#include "pikmin_native.h"
#include "reflect.h"
namespace {
namespace t = tsuchi::game::table;
using tsuchi::game::Name;
using tsuchi::game::ArrayMemory;
int failures = 0;
#define EXPECT(x) do { if (!(x)) { std::printf("%d: %s\n", __LINE__, #x); ++failures; } } while (0)
struct Region { std::unique_ptr<uint8_t[]> data; uint64_t address, size; bool write = true; };
std::deque<Region> regions;
uint64_t memory(uint64_t size, uint8_t fill = 0) {
    Region r{std::make_unique<uint8_t[]>(size), 0, size, true};
    memset(r.data.get(), fill, size);
    r.address = reinterpret_cast<uint64_t>(r.data.get());
    regions.push_back(std::move(r));
    return regions.back().address;
}
template <class T> void put(uint64_t at, T value) { memcpy(reinterpret_cast<void*>(at), &value, sizeof value); }
template <class T> T get(uint64_t at) { T out{}; memcpy(&out, reinterpret_cast<void*>(at), sizeof out); return out; }
bool mapped(uint64_t at, uint64_t size, bool write) {
    for (const auto& r : regions)
        if (at >= r.address && at - r.address <= r.size && size <= r.size - (at - r.address)) return !write || r.write;
    return false;
}
void writable(uint64_t at, bool write) { for (auto& r : regions) if (at >= r.address && at - r.address < r.size) r.write = write; }
bool bytes_are(uint64_t at, size_t size, uint8_t fill) {
    for (size_t i = 0; i < size; ++i) if (get<uint8_t>(at + i) != fill) return false;
    return true;
}
std::map<uint32_t, std::string> names;
Name intern(const std::string& text) {
    for (const auto& n : names) if (n.second == text) return Name{n.first, 0};
    const uint32_t id = uint32_t(names.size() + 1);
    names[id] = text;
    return Name{id, 0};
}
struct MapFixture {
    uint64_t object, header, property, kp, vp, kind, data;
    uint32_t stride = 32, vo = 16;
    MapFixture() {
        object = memory(64);
        header = memory(t::native_map::size + 32, 0xCD) + 16;
        memset(reinterpret_cast<void*>(header), 0, t::native_map::size);
        property = memory(0xB0); kp = memory(0x90); vp = memory(0x90); kind = memory(8);
        put(kind, intern("MapProperty"));
        put(property + t::property::kind, kind);
        put(property + t::map_property::key, kp);
        put(property + t::map_property::value, vp);
        put(kp + t::property::element_bytes, int32_t(1));
        put(vp + t::property::element_bytes, int32_t(8));
        put(property + t::map_property::stride, stride);
        put(property + t::map_property::value_offset, vo);
        data = memory(stride * 3, 0xEF);
        put(header + t::row_list::slots, data);
        put(header + t::row_list::slot_count, int32_t(3));
        put(header + t::row_list::bit_count, int32_t(3));
        put(header + t::row_list::used_bits, uint32_t(5));
        // The middle allocation is a hole with the same key as the lookup.
        put(data, uint8_t(2)); put(data + vo, uint64_t(200));
        put(data + stride, uint8_t(9)); put(data + stride + vo, uint64_t(999));
        put(data + 2 * stride, uint8_t(9)); put(data + 2 * stride + vo, uint64_t(900));
    }
};
std::map<uint64_t, tsuchi::game::Place> places;
int added = 0;
uint64_t add_header = 0, add_property = 0;
uint8_t added_key = 0;
uint64_t added_value = 0;
void engine_add(void* map, void* property, const void* key, const void* value) {
    ++added; add_header = reinterpret_cast<uint64_t>(map); add_property = reinterpret_cast<uint64_t>(property);
    memcpy(&added_key, key, 1); memcpy(&added_value, value, 8);
    const uint32_t stride = get<uint32_t>(add_property + t::map_property::stride);
    const uint32_t vo = get<uint32_t>(add_property + t::map_property::value_offset);
    const uint64_t block = memory(stride, 0xE1);
    memcpy(reinterpret_cast<void*>(block), key, 1);
    memcpy(reinterpret_cast<void*>(block + vo), value, 8);
    put(add_header + t::row_list::slots, block);
    put(add_header + t::row_list::slot_count, int32_t(1));
    put(add_header + t::row_list::bit_count, int32_t(1));
    put(add_header + t::row_list::used_bits, uint32_t(1));
    put(add_header + t::row_list::used_bits_far, uint64_t(0));
}
struct Pair { Name name; uint64_t value; };
static_assert(sizeof(Pair) == t::enum_name::size);
int enum_calls = 0;
uint32_t enum_form = 0, enum_flags = 0, enum_add_max = 0;
bool enum_accept = true;
std::vector<Pair> supplied;
bool engine_enum(void* object, const void* input, uint32_t form, uint32_t flags, uint32_t add_max) {
    ++enum_calls; enum_form = form; enum_flags = flags; enum_add_max = add_max;
    const ArrayMemory& array = *static_cast<const ArrayMemory*>(input);
    supplied.assign(reinterpret_cast<const Pair*>(array.data), reinterpret_cast<const Pair*>(array.data) + array.count);
    if (!enum_accept) return false;
    const uint64_t block = memory(uint64_t(array.count) * sizeof(Pair));
    memcpy(reinterpret_cast<void*>(block), supplied.data(), supplied.size() * sizeof(Pair));
    put(reinterpret_cast<uint64_t>(object) + t::native_enum::names, ArrayMemory{block, array.count, array.count});
    return true;
}
struct EnumFixture {
    uint64_t object, old;
    std::vector<Pair> stock;
    EnumFixture() {
        object = memory(0x80 + 32, 0xCD) + 16;
        memset(reinterpret_cast<void*>(object), 0, 0x80);
        old = memory(19 * sizeof(Pair) + 32, 0xAB) + 16;
        const char* const labels[] = {"Red", "Blue", "Yellow", "Rock", "Wing", "Purple", "White", "Ice", "Bean", "Photon", "Mix", "Minion", "Ninjin", "DrkMinion", "GenseiControl", "Random", "Undef", "Num", "EPikminColor_MAX"};
        for (uint32_t i = 0; i < 19; ++i) stock.push_back(Pair{intern(std::string(t::name::pikmin_enum) + "::" + labels[i]), i});
        memcpy(reinterpret_cast<void*>(old), stock.data(), stock.size() * sizeof(Pair));
        put(object + t::native_enum::names, ArrayMemory{old, 19, 19});
        put(object + t::native_enum::cpp_form, uint32_t(2));
        put(object + t::native_enum::flags, uint32_t(0xA8));
    }
};
void map_tests() {
    using tsuchi::native_map_get; using tsuchi::native_map_put;
    MapFixture m;
    places[m.object] = tsuchi::game::Place{m.header, m.property, false};
    t::fn::add_map_pair = engine_add;
    uint8_t key = 9; uint64_t value = 0x5555;
    EXPECT(native_map_get(m.object, "Map", &key, 1, &value, 8) == TSUCHI_OK && value == 900);
    key = 7; value = 0x5555;
    EXPECT(native_map_get(m.object, "Map", &key, 1, &value, 8) == TSUCHI_NOT_FOUND && value == 0x5555);
    key = 9;
    EXPECT(native_map_get(m.object, "Map", &key, 2, &value, 8) == TSUCHI_INVALID);
    EXPECT(native_map_get(m.object, "Map", &key, 1, &value, 4) == TSUCHI_INVALID);
    put(m.header + t::row_list::bit_count, int32_t(2));
    EXPECT(native_map_get(m.object, "Map", &key, 1, &value, 8) == TSUCHI_INVALID);
    put(m.header + t::row_list::bit_count, int32_t(3));
    put(m.property + t::map_property::stride, uint32_t(20));
    EXPECT(native_map_get(m.object, "Map", &key, 1, &value, 8) == TSUCHI_INVALID);
    put(m.property + t::map_property::stride, m.stride);
    put(m.property + t::map_property::value_offset, uint32_t(0xFFFFFFFC));
    EXPECT(native_map_get(m.object, "Map", &key, 1, &value, 8) == TSUCHI_INVALID);
    put(m.property + t::map_property::value_offset, m.vo);
    put(m.header + t::row_list::slot_count, int32_t(t::constant::max_array_count + 1));
    EXPECT(native_map_get(m.object, "Map", &key, 1, &value, 8) == TSUCHI_INVALID);
    put(m.header + t::row_list::slot_count, int32_t(-1));
    EXPECT(native_map_get(m.object, "Map", &key, 1, &value, 8) == TSUCHI_INVALID);
    put(m.header + t::row_list::slot_count, int32_t(3));
    const uint64_t far = memory(4); put(far, uint32_t(1));
    put(m.header + t::row_list::used_bits_far, far);
    EXPECT(native_map_get(m.object, "Map", &key, 1, &value, 8) == TSUCHI_NOT_FOUND);
    put(far, uint32_t(4));
    EXPECT(native_map_get(m.object, "Map", &key, 1, &value, 8) == TSUCHI_OK && value == 900);
    put(m.header + t::row_list::used_bits_far, uint64_t(1));
    EXPECT(native_map_get(m.object, "Map", &key, 1, &value, 8) == TSUCHI_NOT_READY);
    put(m.header + t::row_list::used_bits_far, uint64_t(0));
    put(m.header + t::row_list::slots, uint64_t(1));
    EXPECT(native_map_get(m.object, "Map", &key, 1, &value, 8) == TSUCHI_NOT_READY);
    // Holes need no cell allocation and must not be read.
    put(m.header + t::row_list::used_bits, uint32_t(0));
    EXPECT(native_map_get(m.object, "Map", &key, 1, &value, 8) == TSUCHI_NOT_FOUND);
    put(m.header + t::row_list::slots, m.data); put(m.header + t::row_list::used_bits, uint32_t(5));
    put(m.kind, intern("ArrayProperty"));
    EXPECT(native_map_put(m.object, "Map", &key, 1, &value, 8) == TSUCHI_INVALID && added == 0);
    EXPECT(native_map_get(m.object, "Map", &key, 1, &value, 8) == TSUCHI_INVALID);
    put(m.kind, intern("MapProperty"));
    EXPECT(native_map_put(m.object, "Map", &key, 2, &value, 8) == TSUCHI_INVALID && added == 0);
    EXPECT(native_map_put(m.object, "Map", &key, 1, &value, 4) == TSUCHI_INVALID && added == 0);
    writable(m.header, false);
    EXPECT(native_map_put(m.object, "Map", &key, 1, &value, 8) == TSUCHI_INVALID && added == 0);
    writable(m.header, true);
    const std::vector<uint8_t> property_before(reinterpret_cast<uint8_t*>(m.property), reinterpret_cast<uint8_t*>(m.property + 0xB0));
    value = 12345;
    EXPECT(native_map_put(m.object, "Map", &key, 1, &value, 8) == TSUCHI_OK && added == 1);
    EXPECT(add_header == m.header && add_property == m.property && added_key == key && added_value == value);
    EXPECT(get<uint64_t>(m.header + t::row_list::slots) != m.data);
    EXPECT(get<uint64_t>(m.data + 2 * m.stride + m.vo) == 900);
    EXPECT(memcmp(reinterpret_cast<void*>(m.property), property_before.data(), property_before.size()) == 0);
    EXPECT(bytes_are(m.header - 16, 16, 0xCD) && bytes_are(m.header + t::native_map::size, 16, 0xCD));
    EXPECT(native_map_get(m.object, "Map", &key, 1, &value, 8) == TSUCHI_OK && value == 12345);
    t::fn::add_map_pair = nullptr;
    EXPECT(native_map_put(m.object, "Map", &key, 1, &value, 8) == TSUCHI_NOT_READY);
}
void enum_tests() {
    using tsuchi::native_pikmin_enum;
    EnumFixture e;
    t::fn::set_enum_names = engine_enum;
    writable(e.old, false);
    EXPECT(native_pikmin_enum(e.object) == TSUCHI_OK && enum_calls == 1);
    EXPECT(enum_form == 2 && enum_flags == 0xA8 && enum_add_max == 0);
    EXPECT(get<uint32_t>(e.object + t::native_enum::cpp_form) == 2 && get<uint32_t>(e.object + t::native_enum::flags) == 0xA8);
    EXPECT(supplied.size() == 22 && memcmp(supplied.data(), e.stock.data(), e.stock.size() * sizeof(Pair)) == 0);
    const char* const extras[] = {t::name::pikmin_color_first, t::name::pikmin_color_reserved, t::name::pikmin_color_second};
    for (uint32_t i = 19; i < 22; ++i) {
        EXPECT(supplied[i].value == i);
        EXPECT(names[supplied[i].name.index] == std::string(t::name::pikmin_enum) + "::" + extras[i - 19]);
    }
    EXPECT(memcmp(reinterpret_cast<void*>(e.old), e.stock.data(), e.stock.size() * sizeof(Pair)) == 0);
    EXPECT(bytes_are(e.old - 16, 16, 0xAB) && bytes_are(e.old + 19 * sizeof(Pair), 16, 0xAB));
    EXPECT(bytes_are(e.object - 16, 16, 0xCD) && bytes_are(e.object + 0x80, 16, 0xCD));
    EXPECT(native_pikmin_enum(e.object) == TSUCHI_OK && enum_calls == 1);
    const ArrayMemory grown = get<ArrayMemory>(e.object + t::native_enum::names);
    Pair pair = get<Pair>(grown.data + 19 * sizeof(Pair));
    pair.name = intern("AnotherMod::Occupied19"); put(grown.data + 19 * sizeof(Pair), pair);
    EXPECT(native_pikmin_enum(e.object) == TSUCHI_BUSY && enum_calls == 1);
    put(grown.data + 19 * sizeof(Pair), supplied[19]);
    pair = supplied[21]; pair.value = 77; put(grown.data + 21 * sizeof(Pair), pair);
    EXPECT(native_pikmin_enum(e.object) == TSUCHI_BUSY && enum_calls == 1);
    put(grown.data + 21 * sizeof(Pair), supplied[21]);
    put(e.object + t::native_enum::names, ArrayMemory{e.old, 18, 19});
    EXPECT(native_pikmin_enum(e.object) == TSUCHI_UNSUPPORTED && enum_calls == 1);
    put(e.object + t::native_enum::names, ArrayMemory{e.old, 23, 23});
    EXPECT(native_pikmin_enum(e.object) == TSUCHI_UNSUPPORTED && enum_calls == 1);
    put(e.object + t::native_enum::names, ArrayMemory{1, 19, 19});
    EXPECT(native_pikmin_enum(e.object) == TSUCHI_NOT_READY && enum_calls == 1);
    put(e.object + t::native_enum::names, ArrayMemory{e.old, 19, 19});
    enum_accept = false;
    EXPECT(native_pikmin_enum(e.object) == TSUCHI_UNSUPPORTED && enum_calls == 2);
    EXPECT(get<ArrayMemory>(e.object + t::native_enum::names).data == e.old);
    t::fn::set_enum_names = nullptr;
    EXPECT(native_pikmin_enum(e.object) == TSUCHI_NOT_READY);
}
}
namespace tsuchi::game {
uint64_t main_base() { return 0; }
bool readable(uint64_t at, uint64_t size) { return mapped(at, size, false); }
bool writable(uint64_t at, uint64_t size) { return mapped(at, size, true); }
bool executable(uint64_t) { return false; }
uint64_t type_of(uint64_t object) { return readable(object, 1) ? object : 0; }
bool name_is(Name name, const char* text) { auto n = names.find(name.index); return n != names.end() && n->second == text && name.number == 0; }
TSUCHI_Result name_from_utf8(const char* text, Name& out) { out = intern(text); return TSUCHI_OK; }
TSUCHI_Result find_place(uint64_t type, uint64_t object, const char* path, Place& out) {
    if (!type || !path || strcmp(path, "Map") || !places.count(object)) return TSUCHI_NOT_FOUND;
    out = places[object]; return TSUCHI_OK;
}
}
int main() {
    map_tests(); enum_tests();
    std::printf("Pikmin native helper policy: %d failures\n", failures);
    return failures ? 1 : 0;
}
