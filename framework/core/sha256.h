// SHA-256 of a block of bytes.
#pragma once
#include <stddef.h>
#include <stdint.h>

namespace tsuchi {

constexpr size_t kSha256Bytes = 32;

struct Sha256 {
    uint32_t state[8] = {0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19};
    uint8_t pending[64]{};
    uint64_t bytes = 0;
    void add(const void*, size_t);
    void finish(uint8_t out[kSha256Bytes]) const;
};

void sha256(const void* data, size_t bytes, uint8_t out[kSha256Bytes]);

} // namespace tsuchi
