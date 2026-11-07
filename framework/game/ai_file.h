// An AI file: values for creatures the game places in one map, which a `maps` entry of a manifest names.
//
//   "P4AI", u32 version 1, u32 the file's size, u32 placements, then
//   u16 + text   the game's map, by the path of its package. The size counts a terminator, which the text has.
//   For each placement:
//     u64        its placement number
//     u16 + text its class, by path, written as the map's path is
//     u8         the kind of AI part it has: 1 any creature's, 2 a Bulborb's, 3 a Bulborb Larva's
//     u8 groups, then for each: u8 which, u32 size, and the bytes the game's serializer writes for that group
//
// A creature's AI part writes what a level's maker set for it as one stream: groups of values one after another
// in a fixed order, with nothing between them and no names. This knows the shape of each group as the version
// of the serializer the game table names writes it, which is enough to find where each group lies in a stream,
// to refuse bytes that are not shaped as a group is, and to put one group in the place of another. The names
// of the groups below are this file's own, for whoever reads it. What the values mean is the game's to know.
//
// A file with another mark at its start, or with anything out of range, is refused whole. No library is called,
// and the caller owns every buffer.
#pragma once
#include <stdint.h>

#include "content_path.h"

namespace tsuchi::ai {

constexpr uint32_t kMark = 0x49413450, kVersion = 1; // "P4AI"
constexpr uint32_t kFileLimit = 4u << 20;       // a whole file
constexpr uint32_t kPlacementLimit = 4096;      // placements in one file
constexpr uint32_t kStreamLimit = 1u << 20;     // one stream, and one group
constexpr uint32_t kItemLimit = 4096;           // records in one group, and units in one string
constexpr uint32_t kTextLimit = 256;            // a path, with its terminator

// The kinds of AI part, as a file numbers them.
enum : uint32_t { kAnyCreature = 1, kBulborb = 2, kLarva = 3 };

// The groups, in the order of the stream.
enum Group : uint32_t {
    kTerritory,              // five numbers
    kDrops,                  // a count, then that many drops: see `drops`
    kDropIndex,              // four bytes
    kDropBone,               // a string
    kDropOffset,             // three numbers
    kDropVelocity,           // three numbers
    kDropRandomVelocity,     // three numbers
    kDropOption,             // two bytes
    kFixedDropCount,         // four bytes
    kOverridePlaceEnabled,   // four bytes
    kOverridePlace,          // three numbers
    kDropNumbers,            // a count, then that many of eight bytes
    kFreezeBothDrops,        // four bytes
    kCarryUsesTerritory,     // four bytes
    kCarrySearchArea,        // seven numbers
    kInvasionStartRatio,     // one number
    kNotifyCarry,            // four bytes
    kCullSearch,             // four bytes
    kUseLastRenderTime,      // four bytes
    kOtherPointsEnabled,     // four bytes
    kOtherPoints,            // a count, then that many of three numbers
    kOtherPriorities,        // a count, then that many of four bytes
    kPatrolEnabled,          // four bytes; a larva's part only
    kPatrolRoute,            // a string; a larva's part only
    kGroups
};
// The two kinds of value in a drop that are one of an enum of the game's, a byte each.
enum : uint32_t { kDropCondition, kDemoFlag };

// Whether a kind of AI part writes a group at all.
inline bool has_group(uint32_t group, uint32_t kind) { return group < kPatrolEnabled || (group < kGroups && kind == kLarva); }

namespace detail {
inline uint32_t u16_at(const uint8_t* p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8; }
inline uint32_t u32_at(const uint8_t* p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }
inline uint64_t u64_at(const uint8_t* p) { return uint64_t(u32_at(p)) | uint64_t(u32_at(p + 4)) << 32; }

// Walks bytes the serializer wrote. Once something does not fit, nothing after it does.
struct Walk {
    const uint8_t* data;
    uint32_t bytes, at = 0;
    bool good = true;

    bool take(uint32_t count) {
        if (!good || count > bytes - at) return good = false;
        at += count;
        return true;
    }
    uint32_t number() { return take(4) ? u32_at(data + at - 4) : 0; }
    bool count(uint32_t& out) {
        out = number();
        return good = good && out <= kItemLimit;
    }
    // Numbers with a fraction, each of four bytes: none of them infinite, and none that is no number.
    bool numbers(uint32_t count) {
        for (uint32_t i = 0; i < count && good; ++i)
            if ((number() & 0x7F800000u) == 0x7F800000u) good = false;
        return good;
    }
    // A string as the engine stores one: a count of units with the terminator, then the units, of one byte each
    // when the count is above 0 and of two when it is below. One-byte text is plain ASCII, and two-byte text has
    // its pairs whole.
    bool string() {
        const int32_t counted = int32_t(number());
        if (!good || counted < -int32_t(kItemLimit) || counted > int32_t(kItemLimit)) return good = false;
        if (!counted) return true;
        const uint32_t units = uint32_t(counted < 0 ? -counted : counted), start = at;
        if (!take(units * (counted < 0 ? 2u : 1u))) return false;
        if (counted > 0) {
            for (uint32_t i = 0; i < units; ++i)
                if (i + 1 == units ? data[start + i] != 0 : !data[start + i] || data[start + i] >= 128) return good = false;
            return true;
        }
        const auto unit = [&](uint32_t i) { return u16_at(data + start + 2 * i); };
        if (unit(units - 1)) return good = false;
        for (uint32_t i = 0; i + 1 < units; ++i) {
            const uint32_t c = unit(i);
            if (!c || (c >= 0xDC00 && c <= 0xDFFF)) return good = false;
            if (c < 0xD800 || c > 0xDBFF) continue;
            if (i + 2 >= units || unit(i + 1) < 0xDC00 || unit(i + 1) > 0xDFFF) return good = false;
            ++i;
        }
        return true;
    }
};

// The drops: sixteen bytes, a number, and four bytes each; then its conditions, each a condition's byte, four
// bytes, a string, and a demo flag's byte; then two strings, a number, two bytes, and a territory of its own
// when the four bytes before it are not zero. `known`, when given, is asked about each byte that is one of an
// enum of the game's.
template <typename Known>
bool drops(Walk& walk, Known&& known) {
    uint32_t count = 0;
    if (!walk.count(count)) return false;
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t conditions = 0;
        if (!walk.take(16) || !walk.numbers(1) || !walk.take(4) || !walk.count(conditions)) return false;
        for (uint32_t c = 0; c < conditions; ++c) {
            if (!walk.take(1) || !known(kDropCondition, walk.data[walk.at - 1])) return walk.good = false;
            if (!walk.take(4) || !walk.string() || !walk.take(1) || !known(kDemoFlag, walk.data[walk.at - 1])) return walk.good = false;
        }
        if (!walk.string() || !walk.string() || !walk.numbers(1) || !walk.take(2)) return false;
        const uint32_t own_territory = walk.number();
        if (!walk.good || (own_territory && !walk.numbers(5))) return false;
    }
    return true;
}

inline bool group(Walk& walk, uint32_t which) {
    uint32_t count = 0;
    switch (which) {
    case kTerritory: return walk.numbers(5);
    case kDrops: return drops(walk, [](uint32_t, uint8_t) { return true; });
    case kDropIndex: case kFixedDropCount: case kOverridePlaceEnabled: case kFreezeBothDrops: case kCarryUsesTerritory: case kNotifyCarry:
    case kCullSearch: case kUseLastRenderTime: case kOtherPointsEnabled: case kPatrolEnabled: return walk.take(4);
    case kDropBone: case kPatrolRoute: return walk.string();
    case kDropOffset: case kDropVelocity: case kDropRandomVelocity: case kOverridePlace: return walk.numbers(3);
    case kDropOption: return walk.take(2);
    case kDropNumbers: return walk.count(count) && walk.take(count * 8);
    case kOtherPoints: return walk.count(count) && walk.numbers(count * 3);
    case kOtherPriorities: return walk.count(count) && walk.take(count * 4);
    case kCarrySearchArea: return walk.numbers(7);
    case kInvasionStartRatio: return walk.numbers(1);
    default: return walk.good = false;
    }
}

// A path with its terminator at `text`, of `size` bytes: plain characters, and none but the last a zero.
inline bool path(const uint8_t* text, uint32_t size, bool with_object) {
    if (size < 3 || size > kTextLimit || text[0] != '/' || text[size - 1]) return false;
    for (uint32_t i = 0; i + 1 < size; ++i) {
        const uint8_t c = text[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '/' || c == '.')) return false;
    }
    return !content_path::wrong(reinterpret_cast<const char*>(text), size - 1, with_object ? content_path::Names::object : content_path::Names::package);
}
} // namespace detail

// Whether bytes are one group, whole, as the serializer writes it.
inline bool is_group(uint32_t which, const uint8_t* data, uint32_t bytes) {
    detail::Walk walk{data, bytes};
    return bytes && bytes <= kStreamLimit && detail::group(walk, which) && walk.at == bytes;
}

// Whether every byte in a group of drops that is one of an enum of the game's is a value `known` knows.
template <typename Known>
bool drops_known(const uint8_t* data, uint32_t bytes, Known&& known) {
    detail::Walk walk{data, bytes};
    return detail::drops(walk, known) && walk.at == bytes;
}

// Where a group lies in a stream.
struct Span {
    uint32_t start, bytes;
};
// Finds every group of a stream an AI part of a kind wrote. `spans` gets one for each group, empty for a group
// that kind does not write. False for a stream that is not shaped as such a part writes one.
inline bool find_groups(const uint8_t* stream, uint32_t bytes, uint32_t kind, Span spans[kGroups]) {
    if (!stream || !bytes || bytes > kStreamLimit || kind < kAnyCreature || kind > kLarva) return false;
    detail::Walk walk{stream, bytes};
    for (uint32_t which = 0; which < kGroups; ++which) {
        spans[which] = Span{walk.at, 0};
        if (!has_group(which, kind)) continue;
        if (!detail::group(walk, which)) return false;
        spans[which].bytes = walk.at - spans[which].start;
    }
    return walk.at == bytes;
}

// One group a file changes: the bytes are the file's own.
struct Change {
    const uint8_t* data;
    uint32_t bytes;
};
// The stream with some of its groups replaced: `changes` has one for each group, with no bytes for a group that
// stays. Returns the bytes the result takes, and writes it when `out` is given.
inline uint32_t replace_groups(const uint8_t* stream, const Span spans[kGroups], const Change changes[kGroups], uint8_t* out) {
    uint32_t at = 0;
    for (uint32_t which = 0; which < kGroups; ++which) {
        const bool changed = changes[which].bytes && spans[which].bytes;
        const uint8_t* from = changed ? changes[which].data : stream + spans[which].start;
        const uint32_t count = changed ? changes[which].bytes : spans[which].bytes;
        for (uint32_t i = 0; out && i < count; ++i) out[at + i] = from[i];
        at += count;
    }
    return at;
}

// One placement of a file. The pointers lead into the file.
struct Placement {
    uint64_t number;
    const char* type; // its class, by path
    uint32_t kind;
    Change changes[kGroups];
};
enum class Read : uint32_t {
    ok,
    not_ai,    // another mark at its start, another version, or another size than it says
    cut_short,
    bad_value, // a count, a path or a number out of range
    bad_group, // a group's bytes are not shaped as the serializer writes that group, or its kind of part writes none
    twice,     // a placement, or a group of one placement, is there twice
    bad_path,  // a path breaks the shared content-path grammar
    no_package, // an object path where the map package is wanted
    no_object,  // a package path where the placement class object is wanted
};

// Reads the placement at `at`, which is moved past it.
inline Read read_placement(const uint8_t* file, uint32_t size, uint32_t& at, Placement& out) {
    using namespace detail;
    out = Placement{};
    if (size - at < 10) return Read::cut_short;
    out.number = u64_at(file + at);
    const uint32_t text = u16_at(file + at + 8);
    at += 10;
    if (!out.number || out.number == ~uint64_t(0)) return Read::bad_value;
    if (text > size - at) return Read::cut_short;
    if (!path(file + at, text, true)) {
        const char* why = content_path::wrong(reinterpret_cast<const char*>(file + at), text ? text - 1 : 0, content_path::Names::object);
        return why == content_path::kNoObject ? Read::no_object : Read::bad_path;
    }
    out.type = reinterpret_cast<const char*>(file + at);
    at += text;
    if (size - at < 2) return Read::cut_short;
    out.kind = file[at];
    const uint32_t groups = file[at + 1];
    at += 2;
    if (out.kind < kAnyCreature || out.kind > kLarva || !groups || groups > kGroups) return Read::bad_value;
    for (uint32_t i = 0; i < groups; ++i) {
        if (size - at < 5) return Read::cut_short;
        const uint32_t which = file[at], bytes = u32_at(file + at + 1);
        at += 5;
        if (which >= kGroups) return Read::bad_value;
        if (bytes > size - at) return Read::cut_short;
        if (out.changes[which].bytes) return Read::twice;
        if (!has_group(which, out.kind) || !is_group(which, file + at, bytes)) return Read::bad_group;
        out.changes[which] = Change{file + at, bytes};
        at += bytes;
    }
    return Read::ok;
}

// Reads the start of a file. `map` gets the map it is for, `first` where its placements begin, and `count` how
// many there are: each is then read with `read_placement`, and the last has to end where the file does.
inline Read read_head(const uint8_t* file, uint64_t size, const char*& map, uint32_t& first, uint32_t& count) {
    using namespace detail;
    if (!file || size < 18 || size > kFileLimit) return Read::not_ai;
    const uint32_t bytes = uint32_t(size);
    if (u32_at(file) != kMark || u32_at(file + 4) != kVersion || u32_at(file + 8) != bytes) return Read::not_ai;
    count = u32_at(file + 12);
    const uint32_t text = u16_at(file + 16);
    if (count > kPlacementLimit) return Read::bad_value;
    if (text > bytes - 18) return Read::cut_short;
    if (!path(file + 18, text, false)) {
        const char* why = content_path::wrong(reinterpret_cast<const char*>(file + 18), text ? text - 1 : 0, content_path::Names::package);
        return why == content_path::kNoPackage ? Read::no_package : Read::bad_path;
    }
    map = reinterpret_cast<const char*>(file + 18);
    first = 18 + text;
    return Read::ok;
}

} // namespace tsuchi::ai
