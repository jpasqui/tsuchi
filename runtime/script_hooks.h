// Shared limits and text conversion for the Lua and JavaScript hook adapters.
#pragma once
#include <stddef.h>
#include <stdint.h>

namespace tsuchi_script {
constexpr uint64_t kHookTurn = 1'000'000;
constexpr uint64_t kHookFrame = 20'000'000;
constexpr uint64_t kHookStack = 320 * 1024;
struct Allowance {
    uint64_t frame = UINT64_MAX, spent = 0, start = 0;
    unsigned depth = 0;
    uint64_t remaining(uint64_t now, uint64_t run) {
        if (frame != now) { frame = now; spent = 0; if (depth) start = run; }
        const uint64_t used = spent + (depth ? run - start : 0);
        return used < kHookFrame ? kHookFrame - used : 0;
    }
    void enter(uint64_t run) { if (!depth++) start = run; }
    void leave(uint64_t run) { if (!--depth) spent += run - start; }
};
// Bounded, allocation-free conversion. Invalid UTF-16 becomes U+FFFD; invalid UTF-8 is refused.
inline size_t utf8(const uint16_t* text, size_t units, char* out) {
    size_t used = 0;
    for (size_t i = 0; i < units; ++i) {
        uint32_t c = text[i];
        if (c >= 0xd800 && c <= 0xdbff && i + 1 < units && text[i + 1] >= 0xdc00 && text[i + 1] <= 0xdfff)
            c = 0x10000 + ((c - 0xd800) << 10) + (text[++i] - 0xdc00);
        else if (c >= 0xd800 && c <= 0xdfff) c = 0xfffd;
        if (c < 0x80) out[used++] = char(c);
        else if (c < 0x800) { out[used++] = char(0xc0 | (c >> 6)); out[used++] = char(0x80 | (c & 63)); }
        else if (c < 0x10000) {
            out[used++] = char(0xe0 | (c >> 12)); out[used++] = char(0x80 | ((c >> 6) & 63)); out[used++] = char(0x80 | (c & 63));
        } else {
            out[used++] = char(0xf0 | (c >> 18)); out[used++] = char(0x80 | ((c >> 12) & 63));
            out[used++] = char(0x80 | ((c >> 6) & 63)); out[used++] = char(0x80 | (c & 63));
        }
    }
    return used;
}
// True when UTF-8 text is what the game's UTF-16 text reads as. A script that passes a text16 argument on
// unchanged gives the game back its own pointer, so the game never sees a copy it didn't make.
inline bool same_text(const uint16_t* game, size_t units, const char* text, size_t bytes) {
    char decoded[256 * 3];
    if (units > 256) return false;
    const size_t length = utf8(game, units, decoded);
    if (length != bytes) return false;
    for (size_t i = 0; i < bytes; ++i)
        if (decoded[i] != text[i]) return false;
    return true;
}
inline bool utf16(const char* text, size_t bytes, uint16_t* out, size_t capacity) {
    size_t used = 0;
    for (size_t i = 0; i < bytes;) {
        const uint8_t first = uint8_t(text[i++]);
        uint32_t c = first, minimum = 0; unsigned more = 0;
        if (first >= 0xc2 && first <= 0xdf) { c &= 31; more = 1; minimum = 0x80; }
        else if (first >= 0xe0 && first <= 0xef) { c &= 15; more = 2; minimum = 0x800; }
        else if (first >= 0xf0 && first <= 0xf4) { c &= 7; more = 3; minimum = 0x10000; }
        else if (first >= 0x80) return false;
        if (i + more > bytes) return false;
        while (more--) { const uint8_t b = uint8_t(text[i++]); if ((b & 0xc0) != 0x80) return false; c = (c << 6) | (b & 63); }
        if (!c || c < minimum || c > 0x10ffff || (c >= 0xd800 && c <= 0xdfff)) return false;
        const size_t need = c >= 0x10000 ? 2 : 1;
        if (used + need >= capacity) return false;
        if (need == 2) { c -= 0x10000; out[used++] = uint16_t(0xd800 | (c >> 10)); out[used++] = uint16_t(0xdc00 | (c & 1023)); }
        else out[used++] = uint16_t(c);
    }
    out[used] = 0;
    return true;
}
} // namespace tsuchi_script
