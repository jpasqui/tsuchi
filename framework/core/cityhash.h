// CityHash64 (version 1.1), the hash the engine makes package identities with.
#pragma once
#include <stddef.h>
#include <stdint.h>

namespace tsuchi {
namespace city {
constexpr uint64_t k0 = 0xc3a5c85c97cb3127ull, k1 = 0xb492b66fbe98f273ull, k2 = 0x9ae16a3b2f90404full;
constexpr uint64_t kMul = 0x9ddfea08eb382d69ull;
inline uint64_t fetch64(const uint8_t* p) {
    uint64_t v = 0;
    for (unsigned i = 0; i < 8; ++i) v |= uint64_t(p[i]) << (8 * i);
    return v;
}
inline uint32_t fetch32(const uint8_t* p) {
    return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
}
inline uint64_t rotate(uint64_t v, unsigned s) { return s ? (v >> s) | (v << (64 - s)) : v; }
inline uint64_t shift_mix(uint64_t v) { return v ^ (v >> 47); }
inline uint64_t swap64(uint64_t v) {
    uint64_t out = 0;
    for (unsigned i = 0; i < 8; ++i) out = (out << 8) | ((v >> (8 * i)) & 0xff);
    return out;
}
inline uint64_t hash16(uint64_t u, uint64_t v, uint64_t mul = kMul) {
    uint64_t a = (u ^ v) * mul;
    a ^= a >> 47;
    uint64_t b = (v ^ a) * mul;
    b ^= b >> 47;
    return b * mul;
}
inline uint64_t hash0to16(const uint8_t* s, size_t len) {
    if (len >= 8) {
        const uint64_t mul = k2 + len * 2, a = fetch64(s) + k2, b = fetch64(s + len - 8);
        const uint64_t c = rotate(b, 37) * mul + a, d = (rotate(a, 25) + b) * mul;
        return hash16(c, d, mul);
    }
    if (len >= 4) {
        const uint64_t mul = k2 + len * 2, a = fetch32(s);
        return hash16(len + (a << 3), fetch32(s + len - 4), mul);
    }
    if (len > 0) {
        const uint8_t a = s[0], b = s[len >> 1], c = s[len - 1];
        const uint32_t y = uint32_t(a) + (uint32_t(b) << 8), z = uint32_t(len) + (uint32_t(c) << 2);
        return shift_mix(y * k2 ^ z * k0) * k2;
    }
    return k2;
}
inline uint64_t hash17to32(const uint8_t* s, size_t len) {
    const uint64_t mul = k2 + len * 2, a = fetch64(s) * k1, b = fetch64(s + 8);
    const uint64_t c = fetch64(s + len - 8) * mul, d = fetch64(s + len - 16) * k2;
    return hash16(rotate(a + b, 43) + rotate(c, 30) + d, a + rotate(b + k2, 18) + c, mul);
}
inline uint64_t hash33to64(const uint8_t* s, size_t len) {
    const uint64_t mul = k2 + len * 2;
    uint64_t a = fetch64(s) * k2, b = fetch64(s + 8);
    const uint64_t c = fetch64(s + len - 24), d = fetch64(s + len - 32);
    const uint64_t e = fetch64(s + 16) * k2, f = fetch64(s + 24) * 9;
    const uint64_t g = fetch64(s + len - 8), h = fetch64(s + len - 16) * mul;
    const uint64_t u = rotate(a + g, 43) + (rotate(b, 30) + c) * 9;
    const uint64_t v = ((a + g) ^ d) + f + 1;
    const uint64_t w = swap64((u + v) * mul) + h;
    const uint64_t x = rotate(e + f, 42) + c;
    const uint64_t y = (swap64((v + w) * mul) + g) * mul;
    const uint64_t z = e + f + c;
    a = swap64((x + z) * mul + y) + b;
    b = shift_mix((z + a) * mul + d + h) * mul;
    return b + x;
}
struct Pair { uint64_t first, second; };
inline Pair weak32(uint64_t w, uint64_t x, uint64_t y, uint64_t z, uint64_t a, uint64_t b) {
    a += w;
    b = rotate(b + a + z, 21);
    const uint64_t c = a;
    a += x;
    a += y;
    b += rotate(a, 44);
    return {a + z, b + c};
}
inline Pair weak32(const uint8_t* s, uint64_t a, uint64_t b) {
    return weak32(fetch64(s), fetch64(s + 8), fetch64(s + 16), fetch64(s + 24), a, b);
}
}

inline uint64_t city_hash64(const uint8_t* s, size_t len) {
    using namespace city;
    if (len <= 32) return len <= 16 ? hash0to16(s, len) : hash17to32(s, len);
    if (len <= 64) return hash33to64(s, len);
    uint64_t x = fetch64(s + len - 40);
    uint64_t y = fetch64(s + len - 16) + fetch64(s + len - 56);
    uint64_t z = hash16(fetch64(s + len - 48) + len, fetch64(s + len - 24));
    Pair v = weak32(s + len - 64, len, z), w = weak32(s + len - 32, y + k1, x);
    x = x * k1 + fetch64(s);
    len = (len - 1) & ~size_t(63);
    do {
        x = rotate(x + y + v.first + fetch64(s + 8), 37) * k1;
        y = rotate(y + v.second + fetch64(s + 48), 42) * k1;
        x ^= w.second;
        y += v.first + fetch64(s + 40);
        z = rotate(z + w.first, 33) * k1;
        v = weak32(s, v.second * k1, x + w.first);
        w = weak32(s + 32, z + w.second, y + fetch64(s + 16));
        const uint64_t t = z; z = x; x = t;
        s += 64;
        len -= 64;
    } while (len != 0);
    return hash16(hash16(v.first, w.first) + shift_mix(y) * k1 + z, hash16(v.second, w.second) + x);
}
}
