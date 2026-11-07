// SHA-256 of a block of bytes.
#pragma once
#include <stddef.h>
#include <stdint.h>

namespace tsuchi {

constexpr size_t kSha256Bytes = 32;

void sha256(const void* data, size_t bytes, uint8_t out[kSha256Bytes]);

} // namespace tsuchi
