// Header-only production decoder, tested against the stdlib's compressed corpus.
#include "inflate.h"
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

int main(int argc, char** argv) {
    if (argc != 2) return 2;
    std::ifstream input(argv[1], std::ios::binary);
    std::vector<uint8_t> file((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    if (file.size() < 12 || file.size() > (8u << 20) || std::memcmp(file.data(), "INFLATE1", 8)) return 2;
    size_t at = 8;
    bool shape = true;
    const auto word = [&]() {
        if (at > file.size() || file.size() - at < 4) { shape = false; return uint32_t(0); }
        uint32_t value = 0;
        for (unsigned i = 0; i < 4; ++i) value |= uint32_t(file[at++]) << (8 * i);
        return value;
    };
    const uint32_t count = word();
    if (count > 4096) return 2;
    unsigned failures = 0;
    for (uint32_t i = 0; i < count; ++i) {
        const uint32_t name_size = word(), source_size = word(), visible = word();
        const uint32_t output_size = word(), expected_size = word(), good = word();
        if (!shape || name_size > 128 || source_size > (1u << 20) || visible > source_size ||
            output_size > (1u << 20) || expected_size > (1u << 20) || good > 1 ||
            uint64_t(name_size) + source_size + expected_size > file.size() - at) return 2;
        const std::string name(reinterpret_cast<char*>(file.data() + at), name_size); at += name_size;
        constexpr size_t guard = 32;
        std::vector<uint8_t> source(size_t(source_size) + 2 * guard, 0xa5);
        if (source_size) std::memcpy(source.data() + guard, file.data() + at, source_size);
        at += source_size;
        const std::vector<uint8_t> unchanged = source;
        std::vector<uint8_t> output(size_t(output_size) + 2 * guard, 0xa5);
        if (output_size) std::memset(output.data() + guard, 0xcd, output_size);
        const bool result = tsuchi::inflate_zlib(source.data() + guard, visible, output.data() + guard, output_size);
        bool valid = result == bool(good) && source == unchanged;
        for (size_t j = 0; j < guard; ++j)
            valid = valid && output[j] == 0xa5 && output[guard + output_size + j] == 0xa5;
        if (good) valid = valid && expected_size == output_size &&
            !std::memcmp(output.data() + guard, file.data() + at, expected_size);
        at += expected_size;
        if (!valid) { std::printf("inflate case %u %s: expected %u got %u or bounds mismatch\n", i, name.c_str(), good, result); ++failures; }
    }
    if (at != file.size()) return 2;
    const uint8_t empty[] = {0x78, 0x01, 0x03, 0x00, 0x00, 0x00, 0x00, 0x01};
    uint8_t byte = 0xa5;
    if (tsuchi::inflate_zlib(nullptr, 0, &byte, 0) ||
        tsuchi::inflate_zlib(nullptr, sizeof empty, &byte, 0) ||
        tsuchi::inflate_zlib(empty, sizeof empty, nullptr, 1) ||
        !tsuchi::inflate_zlib(empty, sizeof empty, nullptr, 0) || byte != 0xa5) {
        std::puts("inflate null/zero output contract failed"); ++failures;
    }
    if (!failures) std::printf("%u inflate corpus cases passed\n", count);
    return failures ? 1 : 0;
}
