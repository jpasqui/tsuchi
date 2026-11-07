// SHA-1 of a block of bytes, for container and shader archive verification.
#pragma once
#include <stdint.h>

namespace tsuchi {
namespace sha1_detail {
inline uint32_t rol(uint32_t value, unsigned shift) {
    return (value << shift) | (value >> (32 - shift));
}
} // namespace sha1_detail

inline void sha1(const uint8_t* data, uint32_t size, uint8_t out[20]) {
    uint32_t h[5] = {0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476, 0xc3d2e1f0};
    const uint64_t padded = (uint64_t(size) + 9 + 63) & ~uint64_t(63);
    for (uint64_t at = 0; at < padded; at += 64) {
        uint32_t w[80]{};
        for (unsigned i = 0; i < 64; ++i) {
            const uint64_t offset = at + i;
            uint8_t byte = 0;
            if (offset < size)
                byte = data[offset];
            else if (offset == size)
                byte = 0x80;
            else if (offset >= padded - 8)
                byte = uint8_t((uint64_t(size) * 8) >> (8 * (padded - 1 - offset)));
            w[i / 4] |= uint32_t(byte) << (24 - 8 * (i % 4));
        }
        for (unsigned i = 16; i < 80; ++i)
            w[i] = sha1_detail::rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
        for (unsigned i = 0; i < 80; ++i) {
            uint32_t f, k;
            if (i < 20) {
                f = (b & c) | ((~b) & d);
                k = 0x5a827999;
            } else if (i < 40) {
                f = b ^ c ^ d;
                k = 0x6ed9eba1;
            } else if (i < 60) {
                f = (b & c) | (b & d) | (c & d);
                k = 0x8f1bbcdc;
            } else {
                f = b ^ c ^ d;
                k = 0xca62c1d6;
            }
            const uint32_t temp = sha1_detail::rol(a, 5) + f + e + k + w[i];
            e = d;
            d = c;
            c = sha1_detail::rol(b, 30);
            b = a;
            a = temp;
        }
        h[0] += a;
        h[1] += b;
        h[2] += c;
        h[3] += d;
        h[4] += e;
    }
    for (unsigned i = 0; i < 20; ++i)
        out[i] = uint8_t(h[i / 4] >> (24 - 8 * (i % 4)));
}

} // namespace tsuchi
