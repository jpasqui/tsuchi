// The receipt of an installed package: the file that makes a package folder count.
//
// A package folder is published when its receipt is written, and only then. The receipt says where the
// package came from, in the installing mod's own terms (source and key), which mods it holds, and its
// revision among the packages it replaced. Tsuchi compares source and key byte for byte and never reads
// anything into them. No engine calls.
#pragma once
#include "sha256.h"
#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace tsuchi::package_store {

constexpr uint32_t kMods = 16, kModIdBytes = 48;
constexpr size_t kSourceBytes = 64, kKeyBytes = 128, kTitleBytes = 161, kReleaseBytes = 128, kFilenameBytes = 161;
constexpr char kMagic[8] = "TSPKG01";

enum class Kind : uint32_t { mods = 0, assets = 1, cheats = 2 };

struct Receipt {
    char magic[8];
    uint32_t kind, count;
    char folder[33];                 // the package's own folder name: 32 lowercase hexadecimal digits
    char source[kSourceBytes], key[kKeyBytes], title[kTitleBytes], release[kReleaseBytes], filename[kFilenameBytes];
    char digest[65];                 // SHA-256 of the downloaded file, lowercase hexadecimal
    char mods[kMods][kModIdBytes];
    uint64_t updated;                // seconds since 1970 UTC when it was installed, or 0
    uint64_t revision;               // one more than the highest of the packages it replaced
    uint8_t checksum[32];
};
static_assert(sizeof(Receipt) == 1576, "the receipt's layout is a file format: change kMagic with it");

inline bool hex_text(const char* s, size_t digits) {
    if (strlen(s) != digits) return false;
    for (; *s; ++s) if (!((*s >= '0' && *s <= '9') || (*s >= 'a' && *s <= 'f'))) return false;
    return true;
}
inline bool folder_valid(const char* s) { return hex_text(s, 32); }
inline bool digest_valid(const char* s) { return hex_text(s, 64); }
// 1 to 63 bytes of printable ASCII.
inline bool source_valid(const char* s) {
    const size_t n = strlen(s);
    if (!n || n >= kSourceBytes) return false;
    for (; *s; ++s) if (uint8_t(*s) <= 32 || uint8_t(*s) >= 127) return false;
    return true;
}
// Well-formed UTF-8 below `room` bytes, without control characters.
inline bool text_valid(const char* s, size_t room, bool empty_ok) {
    const size_t n = strlen(s);
    if (n >= room || (!n && !empty_ok)) return false;
    for (const auto* at = reinterpret_cast<const uint8_t*>(s); *at;) {
        const uint8_t c = *at;
        const uint32_t more = c < 0x80 ? 0 : (c & 0xE0) == 0xC0 ? 1 : (c & 0xF0) == 0xE0 ? 2 : (c & 0xF8) == 0xF0 ? 3 : 4;
        if (more == 4 || (more == 1 && c < 0xC2) || c < 32 || c == 127) return false;
        uint32_t point = more ? c & (0x3F >> more) : c;
        for (uint32_t i = 1; i <= more; ++i) {
            if ((at[i] & 0xC0) != 0x80) return false;
            point = (point << 6) | (at[i] & 0x3F);
        }
        if ((more == 2 && (point < 0x800 || (point >= 0xD800 && point <= 0xDFFF))) || (more == 3 && (point < 0x10000 || point > 0x10FFFF))) return false;
        at += more + 1;
    }
    return true;
}
inline bool terminated(const char* s, size_t size) {
    for (size_t i = 0; i < size; ++i) if (!s[i]) return true;
    return false;
}
inline bool mod_id_valid(const char* id) {
    const size_t n = strlen(id);
    if (!n || n >= kModIdBytes || id[0] == '.') return false;
    for (const char* c = id; *c; ++c) if (!((*c >= 'a' && *c <= 'z') || (*c >= '0' && *c <= '9') || *c == '.' || *c == '_' || *c == '-')) return false;
    return strcmp(id, "tsuchi") && !(n >= 7 && !memcmp(id, "tsuchi.", 7));
}

inline void seal(Receipt& r) {
    memcpy(r.magic, kMagic, sizeof r.magic);
    sha256(&r, offsetof(Receipt, checksum), r.checksum);
}
inline bool receipt_valid(const Receipt& r) {
    // Bounded terminators before any persisted text is used.
    if (memcmp(r.magic, kMagic, sizeof r.magic) || r.kind > uint32_t(Kind::cheats) || !r.count || r.count > kMods) return false;
    if (!terminated(r.folder, sizeof r.folder) || !terminated(r.source, sizeof r.source) || !terminated(r.key, sizeof r.key) ||
        !terminated(r.title, sizeof r.title) || !terminated(r.release, sizeof r.release) || !terminated(r.filename, sizeof r.filename) ||
        !terminated(r.digest, sizeof r.digest)) return false;
    if (!folder_valid(r.folder) || !digest_valid(r.digest) || !source_valid(r.source) || !text_valid(r.key, kKeyBytes, false) ||
        !text_valid(r.title, kTitleBytes, false) || !text_valid(r.release, kReleaseBytes, true) || !text_valid(r.filename, kFilenameBytes, true)) return false;
    if ((r.kind != uint32_t(Kind::mods)) && r.count != 1) return false;
    for (uint32_t i = 0; i < r.count; ++i) {
        if (!terminated(r.mods[i], kModIdBytes) || !mod_id_valid(r.mods[i])) return false;
        for (uint32_t j = 0; j < i; ++j) if (!strcmp(r.mods[i], r.mods[j])) return false;
    }
    uint8_t sum[32];
    sha256(&r, offsetof(Receipt, checksum), sum);
    return !memcmp(sum, r.checksum, 32);
}
inline bool read_receipt(const void* bytes, size_t size, Receipt& r) {
    if (size != sizeof(Receipt)) return false;
    memcpy(&r, bytes, size);
    return receipt_valid(r);
}
inline bool has_mod(const Receipt& r, const char* id) {
    for (uint32_t i = 0; i < r.count; ++i) if (!strcmp(r.mods[i], id)) return true;
    return false;
}
// The same package: the same source and key, and at least one mod in common. A newer one replaces an older.
inline bool same_package(const Receipt& a, const Receipt& b) {
    if (strcmp(a.source, b.source) || strcmp(a.key, b.key)) return false;
    for (uint32_t i = 0; i < a.count; ++i) if (has_mod(b, a.mods[i])) return true;
    return false;
}
// The mod ID a cooked container or cheat files from this source and key are installed under:
// `external.` and 24 hexadecimal digits of SHA-256 over the source, a zero byte and the key.
inline void derived_id(const char* source, const char* key, char (&out)[kModIdBytes]) {
    Sha256 hash;
    hash.add(source, strlen(source));
    const char zero = 0;
    hash.add(&zero, 1);
    hash.add(key, strlen(key));
    uint8_t digest[32];
    hash.finish(digest);
    constexpr char digits[] = "0123456789abcdef";
    memcpy(out, "external.", 9);
    for (int i = 0; i < 12; ++i) { out[9 + 2 * i] = digits[digest[i] >> 4]; out[10 + 2 * i] = digits[digest[i] & 15]; }
    out[33] = 0;
}

} // namespace tsuchi::package_store
