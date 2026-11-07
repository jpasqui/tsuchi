#pragma once
#include <stdint.h>
#include <string.h>

namespace tsuchi::https {
constexpr uint64_t HTTPS_BODY_CHUNK = 64u * 1024u;
constexpr uint64_t HTTPS_BODY_LIMIT = 4u * 1024u * 1024u;
struct HttpsBody {
    void* chunks[HTTPS_BODY_LIMIT / HTTPS_BODY_CHUNK]{};
    uint64_t allocated = 0;
};
inline void https_body_free(HttpsBody& body, void (*release)(void*)) {
    for (auto*& chunk : body.chunks) if (chunk) { release(chunk); chunk = nullptr; }
    body.allocated = 0;
}
inline bool https_body_append(HttpsBody& body, uint64_t offset, const void* source,
                              uint64_t bytes, uint64_t limit, void* (*allocate)(uint64_t)) {
    if ((!source && bytes) || !limit || limit > HTTPS_BODY_LIMIT || offset > limit || bytes > limit - offset) return false;
    const auto* input = static_cast<const unsigned char*>(source);
    while (bytes) {
        const uint64_t index = offset / HTTPS_BODY_CHUNK, within = offset % HTTPS_BODY_CHUNK;
        if (!body.chunks[index]) {
            const uint64_t remaining = limit - index * HTTPS_BODY_CHUNK;
            const uint64_t capacity = remaining < HTTPS_BODY_CHUNK ? remaining : HTTPS_BODY_CHUNK;
            body.chunks[index] = allocate(capacity);
            if (!body.chunks[index]) return false;
            body.allocated += capacity;
        }
        const uint64_t in_chunk = HTTPS_BODY_CHUNK - within;
        const uint64_t count = bytes < in_chunk ? bytes : in_chunk;
        memcpy(static_cast<unsigned char*>(body.chunks[index]) + within, input, size_t(count));
        input += count; offset += count; bytes -= count;
    }
    return true;
}
inline bool https_body_read(const HttpsBody& body, uint64_t size, uint64_t offset,
                            void* destination, uint64_t bytes) {
    if ((!destination && bytes) || size > HTTPS_BODY_LIMIT || offset > size || bytes > size - offset) return false;
    auto* output = static_cast<unsigned char*>(destination);
    while (bytes) {
        const uint64_t index = offset / HTTPS_BODY_CHUNK, within = offset % HTTPS_BODY_CHUNK;
        if (!body.chunks[index]) return false;
        const uint64_t in_chunk = HTTPS_BODY_CHUNK - within;
        const uint64_t count = bytes < in_chunk ? bytes : in_chunk;
        memcpy(output, static_cast<const unsigned char*>(body.chunks[index]) + within, size_t(count));
        output += count; offset += count; bytes -= count;
    }
    return true;
}
}
