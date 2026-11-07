// Bounded RFC1950/RFC1951 decoding for the stock container's Zlib header blocks.
// No dictionaries, allocation, engine calls, or output growth are involved.
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace tsuchi {
namespace inflate_detail {

struct Bits {
    const uint8_t* data;
    size_t size, at = 0;
    uint32_t held = 0;
    unsigned count = 0;

    bool read(unsigned number, uint32_t& value) {
        while (count < number) {
            if (at == size) return false;
            held |= uint32_t(data[at++]) << count;
            count += 8;
        }
        value = held & ((uint32_t(1) << number) - 1);
        held >>= number;
        count -= number;
        return true;
    }
    void align() { held = 0; count = 0; } // read() leaves fewer than eight unused bits
};

struct Tree {
    uint16_t counts[16]{}, first[16]{}, start[16]{}, symbols[288]{};
    unsigned longest = 0;

    bool build(const uint8_t* lengths, unsigned size, bool single, bool empty = false) {
        if (size > 288) return false;
        for (unsigned i = 0; i < size; ++i) {
            if (lengths[i] > 15) return false;
            if (lengths[i]) ++counts[lengths[i]];
        }
        unsigned total = 0, code = 0;
        int left = 1;
        for (unsigned length = 1; length <= 15; ++length) {
            left = left * 2 - counts[length];
            if (left < 0) return false;
            code = (code + counts[length - 1]) << 1;
            first[length] = uint16_t(code);
            start[length] = uint16_t(total);
            total += counts[length];
            if (counts[length]) longest = length;
        }
        if (!total) return empty;
        if (left && !(single && longest == 1 && counts[1] == 1)) return false;
        uint16_t next[16];
        memcpy(next, start, sizeof next);
        for (unsigned i = 0; i < size; ++i)
            if (lengths[i]) symbols[next[lengths[i]]++] = uint16_t(i);
        return true;
    }

    bool read(Bits& bits, uint32_t& symbol) const {
        uint32_t code = 0, bit = 0;
        for (unsigned length = 1; length <= longest; ++length) {
            if (!bits.read(1, bit)) return false;
            code = (code << 1) | bit;
            if (code >= first[length] && code - first[length] < counts[length]) {
                symbol = symbols[start[length] + code - first[length]];
                return true;
            }
        }
        return false;
    }
};

inline bool fixed(Tree& literal, Tree& distance) {
    uint8_t lengths[288];
    for (unsigned i = 0; i < 288; ++i) lengths[i] = i < 144 ? 8 : i < 256 ? 9 : i < 280 ? 7 : 8;
    if (!literal.build(lengths, 288, false)) return false;
    for (unsigned i = 0; i < 32; ++i) lengths[i] = 5;
    return distance.build(lengths, 32, false);
}

inline bool dynamic(Bits& bits, Tree& literal, Tree& distance) {
    uint32_t literals = 0, distances = 0, codes = 0;
    if (!bits.read(5, literals) || !bits.read(5, distances) || !bits.read(4, codes)) return false;
    literals += 257; distances += 1; codes += 4;
    if (literals > 286) return false;
    constexpr uint8_t order[] = {16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15};
    uint8_t code_lengths[19]{}, lengths[286 + 32]{};
    for (uint32_t i = 0, length = 0; i < codes; ++i) {
        if (!bits.read(3, length)) return false;
        code_lengths[order[i]] = uint8_t(length);
    }
    Tree code_tree;
    if (!code_tree.build(code_lengths, 19, false)) return false;
    for (uint32_t at = 0; at < literals + distances;) {
        uint32_t symbol = 0, extra = 0, repeated = 1;
        if (!code_tree.read(bits, symbol)) return false;
        uint8_t length = uint8_t(symbol);
        if (symbol == 16) {
            if (!at || !bits.read(2, extra)) return false;
            length = lengths[at - 1]; repeated = 3 + extra;
        } else if (symbol == 17) {
            if (!bits.read(3, extra)) return false;
            length = 0; repeated = 3 + extra;
        } else if (symbol == 18) {
            if (!bits.read(7, extra)) return false;
            length = 0; repeated = 11 + extra;
        } else if (symbol > 15) return false;
        if (repeated > literals + distances - at) return false;
        while (repeated--) lengths[at++] = length;
    }
    return lengths[256] && literal.build(lengths, literals, true) &&
           distance.build(lengths + literals, distances, true, true);
}

inline bool compressed(Bits& bits, const Tree& literal, const Tree& distance,
                       uint8_t* output, size_t size, size_t& used, uint32_t window) {
    constexpr uint16_t lengths[] = {3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,115,131,163,195,227,258};
    constexpr uint8_t length_bits[] = {0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0};
    constexpr uint16_t distances[] = {1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,1025,1537,2049,3073,4097,6145,8193,12289,16385,24577};
    constexpr uint8_t distance_bits[] = {0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13};
    for (;;) {
        uint32_t symbol = 0, extra = 0;
        if (!literal.read(bits, symbol)) return false;
        if (symbol < 256) {
            if (used == size) return false;
            output[used++] = uint8_t(symbol);
            continue;
        }
        if (symbol == 256) return true;
        if (symbol > 285) return false;
        symbol -= 257;
        if (!bits.read(length_bits[symbol], extra)) return false;
        const uint32_t length = lengths[symbol] + extra;
        if (!distance.read(bits, symbol) || symbol >= 30 || !bits.read(distance_bits[symbol], extra)) return false;
        const uint32_t back = distances[symbol] + extra;
        if (back > used || back > window || length > size - used) return false;
        // Forward copying is required when a match repeats its own output.
        for (uint32_t i = 0; i < length; ++i) output[used] = output[used - back], ++used;
    }
}

inline uint32_t adler32(const uint8_t* bytes, size_t size) {
    uint32_t a = 1, b = 0;
    for (size_t at = 0; at < size;) {
        const size_t left = size - at, count = left < 5552 ? left : 5552;
        for (size_t i = 0; i < count; ++i) a += bytes[at++], b += a;
        a %= 65521; b %= 65521;
    }
    return (b << 16) | a;
}
}

inline bool inflate_zlib(const void* input, size_t input_size, void* output, size_t output_size) {
    if (!input || input_size < 6 || (!output && output_size)) return false;
    const auto* bytes = static_cast<const uint8_t*>(input);
    auto* made = static_cast<uint8_t*>(output);
    if ((bytes[0] & 15) != 8 || (bytes[0] >> 4) > 7 ||
        ((uint32_t(bytes[0]) << 8) | bytes[1]) % 31 || (bytes[1] & 32)) return false;
    inflate_detail::Bits bits{bytes + 2, input_size - 6};
    size_t used = 0;
    uint32_t final = 0;
    do {
        uint32_t kind = 0;
        if (!bits.read(1, final) || !bits.read(2, kind)) return false;
        if (kind == 0) {
            bits.align();
            uint32_t length = 0, inverse = 0;
            if (!bits.read(16, length) || !bits.read(16, inverse) || (length ^ inverse) != 0xffff ||
                length > bits.size - bits.at || length > output_size - used) return false;
            if (length) memcpy(made + used, bits.data + bits.at, length);
            bits.at += length; used += length;
        } else if (kind <= 2) {
            inflate_detail::Tree literal, distance;
            if (!(kind == 1 ? inflate_detail::fixed(literal, distance) : inflate_detail::dynamic(bits, literal, distance)) ||
                !inflate_detail::compressed(bits, literal, distance, made, output_size, used, uint32_t(1) << ((bytes[0] >> 4) + 8))) return false;
        } else return false;
    } while (!final);
    if (used != output_size || bits.at != bits.size) return false;
    const uint8_t* sum = bytes + input_size - 4;
    const uint32_t expected = (uint32_t(sum[0]) << 24) | (uint32_t(sum[1]) << 16) | (uint32_t(sum[2]) << 8) | sum[3];
    return inflate_detail::adler32(made, output_size) == expected;
}
}
