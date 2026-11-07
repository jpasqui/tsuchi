// Clang emits this helper for QuickJS's unsigned 128-bit BigInt division.
// Binary long division needs only shifts, compares, and subtraction.
extern "C" unsigned __int128 __udivti3(unsigned __int128 numerator, unsigned __int128 denominator) {
    if (!denominator) __builtin_trap();
    unsigned __int128 quotient = 0, remainder = 0;
    for (unsigned bit = 128; bit--;) {
        const bool carry = (remainder >> 127) != 0;
        remainder = (remainder << 1) | ((numerator >> bit) & 1);
        if (carry || remainder >= denominator) {
            remainder -= denominator;
            quotient |= static_cast<unsigned __int128>(1) << bit;
        }
    }
    return quotient;
}