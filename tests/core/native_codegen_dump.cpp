// Emits the core's gateway bytes for independent ARM64 execution checks.
#include "native_codegen.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
int main(int argc, char** argv) {
    if (argc != 6) return 2;
    const auto value = [](const char* text) { return std::strtoull(text, nullptr, 0); };
    tsuchi::native_code::Gateway code;
    const bool ok = !std::strcmp(argv[1], "inline") ?
        tsuchi::native_code::inline_hook(code, value(argv[3]), uint32_t(value(argv[2])), value(argv[4]), value(argv[5])) :
        tsuchi::native_code::function(code, value(argv[3]), uint32_t(value(argv[2])), value(argv[4]), value(argv[5]));
    if (!ok) return 1;
    for (uint8_t byte : reinterpret_cast<const uint8_t(&)[64]>(code)) std::printf("%02x", byte);
    return 0;
}
