// ARM64 gateways for one checked instruction. No runtime allocation or decoder
// guesses: a relocation is emitted only when its complete contract fits here.
#pragma once
#include <stdint.h>
#include <string.h>

namespace tsuchi::native_code {

constexpr unsigned kGatewayBytes = 64;
struct Gateway { uint32_t words[kGatewayBytes / 4]{}; };

inline int64_t signed_bits(uint64_t value, unsigned bits) {
    const uint64_t sign = uint64_t(1) << (bits - 1);
    return int64_t((value ^ sign) - sign);
}
inline bool branch(uint64_t from, uint64_t to, uint32_t& word, bool link = false) {
    if ((from | to) & 3) return false;
    if ((to >= from && to - from >= (1ull << 27)) ||
        (to < from && from - to > (1ull << 27))) return false;
    word = (link ? 0x94000000u : 0x14000000u) | (uint32_t((to - from) >> 2) & 0x03ffffffu);
    return true;
}
inline bool relative(uint32_t word) {
    return (word & 0x7c000000) == 0x14000000 || // B, BL
           (word & 0x1f000000) == 0x10000000 || // ADR, ADRP
           (word & 0x7e000000) == 0x34000000 || // CBZ, CBNZ
           (word & 0x7e000000) == 0x36000000 || // TBZ, TBNZ
           (word & 0xff000010) == 0x54000000 || // B.cond
           (word & 0x3b000000) == 0x18000000;   // LDR literal
}
inline void literal(Gateway& out, uint64_t value) { memcpy(&out.words[12], &value, 8); }

// Replay starts at `first`. Space at +24 is a conditional branch tail, and +48
// holds a constant. The original instruction runs before returning to `at+4`.
inline bool replay(Gateway& out, uint32_t word, uint64_t at, uint64_t gateway, unsigned first) {
    const uint64_t here = gateway + 4 * first;
    if ((word & 0x7c000000) == 0x14000000) {
        const uint64_t target = at + 4 * signed_bits(word & 0x03ffffffu, 26);
        if (word & 0x80000000u) { // BL: the callee must see the stock return address.
            out.words[first] = 0x58000000u | (uint32_t((48 - 4 * first) / 4) << 5) | 30;
            literal(out, at + 4);
            return branch(here + 4, target, out.words[first + 1]);
        }
        return branch(here, target, out.words[first]);
    }
    if ((word & 0x1f000000) == 0x10000000) {
        const int64_t value = signed_bits(((word >> 29) & 3) | uint64_t((word >> 5) & 0x7ffff) << 2, 21);
        const uint64_t target = word & 0x80000000u ? (at & ~4095ull) + uint64_t(value) * 4096 : at + value;
        out.words[first] = 0x58000000u | (uint32_t((48 - 4 * first) / 4) << 5) | (word & 31);
        literal(out, target);
    } else if ((word & 0x7e000000) == 0x34000000 ||
               (word & 0x7e000000) == 0x36000000 ||
               (word & 0xff000010) == 0x54000000) {
        const unsigned bits = (word & 0x7e000000) == 0x36000000 ? 14 : 19;
        const uint32_t mask = (1u << bits) - 1;
        const uint64_t target = at + 4 * signed_bits((word >> 5) & mask, bits);
        out.words[first] = (word & ~(mask << 5)) | (uint32_t((24 - 4 * first) / 4) << 5);
        if (!branch(gateway + 24, target, out.words[6])) return false;
    } else if ((word & 0x3b000000) == 0x18000000) {
        const uint64_t target = at + 4 * signed_bits((word >> 5) & 0x7ffff, 19);
        const int64_t delta = int64_t(target - here);
        if (!(delta % 4) && delta >= -(1 << 20) && delta < (1 << 20)) {
            out.words[first] = (word & ~(0x7ffffu << 5)) | ((uint32_t(delta / 4) & 0x7ffff) << 5);
        } else {
            const unsigned opc = word >> 30, reg = word & 31;
            const bool vector = word & (1u << 26);
            if (vector && opc == 3) return false; // unallocated SIMD literal encoding
            const uint32_t integers[] = {0xb9400000u, 0xf9400000u, 0xb9800000u, 0xf9800000u};
            const uint32_t vectors[] = {0xbd400000u, 0xfd400000u, 0x3dc00000u};
            const bool scratch = vector || opc == 3 || reg == 31;
            unsigned position = first;
            if (scratch) out.words[position++] = 0xa9bf7ff0; // STP x16, xzr, [sp, #-16]!
            const unsigned address_reg = scratch ? 16 : reg;
            out.words[position] = 0x58000000u | (uint32_t((48 - 4 * position) / 4) << 5) | address_reg;
            ++position;
            literal(out, target);
            out.words[position++] = (vector ? vectors[opc] : integers[opc]) | (address_reg << 5) | reg;
            if (scratch) out.words[position++] = 0xa8c17ff0; // LDP x16, xzr, [sp], #16
            return branch(gateway + 4 * position, at + 4, out.words[position]);
        }
    } else {
        if ((word & 0xfffffc1fu) == 0xd63f0000u) {
            const unsigned reg = word >> 5 & 31;
            if (reg == 30) return false; // source and destination of LR would alias
            out.words[first] = 0x58000000u | (uint32_t((48 - 4 * first) / 4) << 5) | 30;
            literal(out, at + 4);
            out.words[first + 1] = 0xd61f0000u | (reg << 5); // BR with the stock LR
            return true;
        }
        out.words[first] = word;
    }
    return branch(here + 4, at + 4, out.words[first + 1]);
}

inline bool function(Gateway& out, uint64_t at, uint32_t word, uint64_t gateway, uint64_t thunk) {
    if (relative(word) || (word & 0xfffffc1fu) == 0xd63f0000u) return false;
    out.words[0] = 0x58000050; // LDR x16, +8
    out.words[1] = 0xd61f0200; // BR x16; IP0 is caller-saved at a function entry.
    memcpy(&out.words[2], &thunk, 8);
    return replay(out, word, at, gateway, 4);
}

inline bool call(Gateway& out, uint64_t thunk) {
    // The patched BL has already supplied the call site's return address.
    // IP0 is caller-saved across this call, as at a function-entry gateway.
    out.words[0] = 0x58000050; // LDR x16, +8
    out.words[1] = 0xd61f0200; // BR x16
    memcpy(&out.words[2], &thunk, 8);
    return true;
}

inline bool inline_hook(Gateway& out, uint64_t at, uint32_t word, uint64_t gateway, uint64_t shared) {
    out.words[0] = 0xa9bf43fe; // STP x30, x16, [sp, #-16]!
    if (!branch(gateway + 4, shared, out.words[1], true)) return false;
    out.words[2] = 0xa8c143fe; // LDP x30, x16, [sp], #16
    return replay(out, word, at, gateway, 3);
}

} // namespace tsuchi::native_code
