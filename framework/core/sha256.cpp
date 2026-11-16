#include "sha256.h"

#include <string.h>

namespace tsuchi {

namespace {

constexpr uint32_t kRound[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be,
    0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa,
    0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85,
    0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3,
    0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f,
    0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

uint32_t turned(uint32_t value, unsigned by) { return value >> by | value << (32 - by); }

void take(uint32_t state[8], const uint8_t block[64]) {
    uint32_t w[64];
    for (int i = 0; i < 16; ++i)
        w[i] = uint32_t(block[4 * i]) << 24 | uint32_t(block[4 * i + 1]) << 16 | uint32_t(block[4 * i + 2]) << 8 | uint32_t(block[4 * i + 3]);
    for (int i = 16; i < 64; ++i) {
        const uint32_t a = turned(w[i - 15], 7) ^ turned(w[i - 15], 18) ^ w[i - 15] >> 3;
        const uint32_t b = turned(w[i - 2], 17) ^ turned(w[i - 2], 19) ^ w[i - 2] >> 10;
        w[i] = w[i - 16] + a + w[i - 7] + b;
    }
    uint32_t s[8];
    memcpy(s, state, sizeof s);
    for (int i = 0; i < 64; ++i) {
        const uint32_t one = s[7] + (turned(s[4], 6) ^ turned(s[4], 11) ^ turned(s[4], 25)) + ((s[4] & s[5]) ^ (~s[4] & s[6])) + kRound[i] + w[i];
        const uint32_t two = (turned(s[0], 2) ^ turned(s[0], 13) ^ turned(s[0], 22)) + ((s[0] & s[1]) ^ (s[0] & s[2]) ^ (s[1] & s[2]));
        for (int j = 7; j > 0; --j) s[j] = s[j - 1];
        s[4] += one;
        s[0] = one + two;
    }
    for (int i = 0; i < 8; ++i) state[i] += s[i];
}

} // namespace

void Sha256::add(const void* data, size_t size) {
    const auto* at = static_cast<const uint8_t*>(data);
    while (size) {
        const size_t used = size_t(bytes % 64), room = 64 - used, count = size < room ? size : room;
        memcpy(pending + used, at, count); bytes += count; at += count; size -= count;
        if (bytes % 64 == 0) take(state, pending);
    }
}
void Sha256::finish(uint8_t out[kSha256Bytes]) const {
    Sha256 copy = *this;
    uint8_t tail[128]{}; const size_t used = size_t(bytes % 64);
    const size_t count = used < 56 ? 64 - used : 128 - used;
    tail[0] = 0x80;
    for (int i=0;i<8;++i) tail[count-1-i]=uint8_t((bytes*8)>>(8*i));
    copy.add(tail,count);
    for(int i=0;i<8;++i)for(int j=0;j<4;++j)out[4*i+j]=uint8_t(copy.state[i]>>(24-8*j));
}

void sha256(const void* data, size_t bytes, uint8_t out[kSha256Bytes]) {
    uint32_t state[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    const auto* at = static_cast<const uint8_t*>(data);
    size_t left = bytes;
    for (; left >= 64; at += 64, left -= 64) take(state, at);
    // What is left, a one bit, zeros, and the length in bits: one block, or two when the length does not fit in the first.
    uint8_t last[128] = {};
    if (left) memcpy(last, at, left);
    last[left] = 0x80;
    const size_t whole = left < 56 ? 64 : 128;
    const uint64_t bits = uint64_t(bytes) * 8;
    for (int i = 0; i < 8; ++i) last[whole - 1 - i] = uint8_t(bits >> (8 * i));
    take(state, last);
    if (whole == 128) take(state, last + 64);
    for (int i = 0; i < 8; ++i)
        for (int j = 0; j < 4; ++j) out[4 * i + j] = uint8_t(state[i] >> (24 - 8 * j));
}

} // namespace tsuchi
