// Checks of the hash, and of what the framework reads of loaded modules and module files, against made-up ones.
// Exits with the number of failures.
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "fake_game.h"
#include "fake_modules.h"
#include "modules.h"
#include "sha256.h"

namespace {

namespace g = tsuchi::game;

int failures = 0;
#define CHECK(condition) \
    do { \
        if (!(condition)) { \
            std::printf("%s:%d: %s\n", __FILE__, __LINE__, #condition); \
            ++failures; \
        } \
    } while (0)

std::string hash_of(const std::string& bytes) {
    uint8_t hash[tsuchi::kSha256Bytes];
    tsuchi::sha256(bytes.data(), bytes.size(), hash);
    std::string hex;
    for (uint8_t byte : hash) {
        char two[3];
        std::snprintf(two, sizeof two, "%02x", byte);
        hex += two;
    }
    return hex;
}

void hashing() {
    CHECK(hash_of("") == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    CHECK(hash_of("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    CHECK(hash_of("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq") == "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
    // Around where the length stops fitting in the last block, and a second block starts.
    CHECK(hash_of(std::string(55, 'a')) == "9f4390f8d30c2dd92ec9f095b65e2b9ae9b0a925a5258e241c9f1e910f734318");
    CHECK(hash_of(std::string(56, 'a')) == "b35439a4ac6f0948b6d6f9e3c6af0f5f590ce20f1bde7090ef7970686ec6738a");
    CHECK(hash_of(std::string(63, 'a')) == "7d3e74a05d7db15bce4ad9ec0658ea98e3f06eeecf16b4c6fff2da457ddc2f34");
    CHECK(hash_of(std::string(64, 'a')) == "ffe054fe7ae0cb6dc65c3af9b61d5209f439851db43d0ba5997337df154668eb");
    CHECK(hash_of(std::string(119, 'a')) == "31eba51c313a5c08226adf18d4a359cfdfd8d2e816b13f4af952f7ea6584dcfb");
    CHECK(hash_of(std::string(120, 'a')) == "2f3d335432c70b580af0e8e1b3674a7c020d683aa5f73aaaedfdc55af904c21c");
    CHECK(hash_of(std::string(1000000, 'a')) == "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
    // Bytes above 127 are bytes, and the hash is of exactly as many as are given.
    const std::string high = std::string("\xff\x80\x00\x7f", 4);
    CHECK(hash_of(high) == "ae72d4fb6b85aa8fe2f5fc339cc306f67280357b1b8c583c55ebe725646449a6" && hash_of(high) != hash_of(high.substr(0, 3)));
}

void loaded() {
    fake::start();
    const uint64_t one = 0x1111111111110000, two = 0x2222000022220000;
    for (fake::Hashing hashing : {fake::Hashing::plain, fake::Hashing::gnu}) {
        const uint64_t module = fake::loaded_module({{"first", one}, {"second_name", two}, {"referred_to", 0x3333, false}, {"own", 0x4444, true, 0}, {"weak", 0x5555, true, 2},
                                                     {"last", 0x6666}},
                                                    hashing);
        CHECK(module % g::kPage == 0);
        // By name, exactly: every one it offers, from the first to the last, and none it only refers to or keeps to itself.
        CHECK(g::module_symbol(module, "first") == one && g::module_symbol(module, "second_name") == two && g::module_symbol(module, "last") == 0x6666);
        CHECK(g::module_symbol(module, "weak") == 0x5555 && g::module_symbol(module, "referred_to") == 0 && g::module_symbol(module, "own") == 0);
        CHECK(g::module_symbol(module, "firs") == 0 && g::module_symbol(module, "first_") == 0 && g::module_symbol(module, "") == 0 && g::module_symbol(module, "absent") == 0);
        // From any address in it: its own first page, the middle of a later one, its last byte.
        CHECK(g::module_containing(module) == module && g::module_containing(module + 0x2ABC) == module && g::module_containing(module + 4 * g::kPage - 1) == module);
        // What is not the start of a module is not taken for one.
        CHECK(g::module_symbol(module + g::kPage, "first") == 0 && g::module_symbol(module + 8, "first") == 0);
    }
    // A module that says nothing of how many names it has offers none; one with no names at all, none either.
    const uint64_t silent = fake::loaded_module({{"first", one}}, fake::Hashing::none);
    CHECK(g::module_symbol(silent, "first") == 0 && g::module_containing(silent + 0x10) == silent);
    for (fake::Hashing hashing : {fake::Hashing::plain, fake::Hashing::gnu}) CHECK(g::module_symbol(fake::loaded_module({}, hashing), "first") == 0);
    // One whose table says nothing of where its names are, or where their texts are, offers none.
    for (uint64_t entry : {1, 2}) {
        const uint64_t bare = fake::loaded_module({{"first", one}});
        CHECK(g::module_symbol(bare, "first") == one);
        fake::put<uint64_t>(bare + 0x100 + 16 * entry, 0x15);
        CHECK(g::module_symbol(bare, "first") == 0);
    }
    // Memory that is in no module, with nothing else the framework may read: the search ends where memory does, at nothing.
    fake::start();
    const uint64_t plain = fake::memory(3 * g::kPage);
    CHECK(g::module_containing(plain + 2 * g::kPage + 5) == 0 && g::module_containing(0) == 0 && g::module_containing(0x10) == 0);
    // A header that only looks like one: the mark is missing, or the word before it points outside the page.
    const uint64_t almost = (fake::memory(3 * g::kPage) + g::kPage - 1) & ~(g::kPage - 1);
    fake::put<uint32_t>(almost + 4, 0x80);
    CHECK(g::module_containing(almost + 0x100) == 0);
    std::memcpy(reinterpret_cast<void*>(almost + 0x80), "MOD0", 4);
    CHECK(g::module_containing(almost + 0x100) == almost);
    // The mark in the two words themselves, or running off the page, is no header either; at the page's end it is one.
    fake::put<uint32_t>(almost + 4, 0);
    std::memcpy(reinterpret_cast<void*>(almost), "MOD0", 4);
    CHECK(g::module_containing(almost + 0x100) == 0);
    fake::put<uint32_t>(almost, 0);
    fake::put<uint32_t>(almost + 4, uint32_t(g::kPage - 3));
    std::memcpy(reinterpret_cast<void*>(almost + g::kPage - 3), "MOD0", 4);
    CHECK(g::module_containing(almost + 0x100) == 0);
    fake::put<uint32_t>(almost + 4, uint32_t(g::kPage - 4));
    std::memcpy(reinterpret_cast<void*>(almost + g::kPage - 4), "MOD0", 4);
    CHECK(g::module_containing(almost + 0x100) == almost);
}

// A module file of `pages` pages, with a header that is right.
std::vector<uint8_t> file_of(uint32_t pages) {
    std::vector<uint8_t> image(pages * g::kPage, 0x5A);
    const auto put = [&](size_t at, uint32_t value) { std::memcpy(image.data() + at, &value, 4); };
    std::memcpy(image.data() + 0x10, "NRO0", 4);
    put(0x18, uint32_t(image.size()));
    put(0x20, 0), put(0x24, 0x1000);                                    // code
    put(0x28, 0x1000), put(0x2C, 0x1000);                               // what is only read
    put(0x30, 0x2000), put(0x34, uint32_t(image.size()) - 0x2000);      // data, to the end
    return image;
}

void files() {
    const auto valid = [](const std::vector<uint8_t>& image) { return g::module_file_valid(image.data(), image.size()); };
    const auto with = [](std::vector<uint8_t> image, size_t at, uint32_t value) {
        std::memcpy(image.data() + at, &value, 4);
        return image;
    };
    const std::vector<uint8_t> good = file_of(3);
    CHECK(valid(good) && valid(file_of(4)));
    // Not whole pages, or none; another kind of file; a size that is not the file's.
    CHECK(!g::module_file_valid(good.data(), good.size() - 1) && !g::module_file_valid(good.data(), good.size() - g::kPage + 0x80) && !g::module_file_valid(good.data(), 0));
    CHECK(!valid(with(good, 0x10, 0x3052524E)) && !valid(with(good, 0x18, uint32_t(good.size()) + 0x1000)) && !valid(with(good, 0x18, uint32_t(good.size()) - 0x1000)));
    // Half a page more, with a size that says so.
    std::vector<uint8_t> ragged = good;
    ragged.resize(good.size() + 0x800, 0x5A);
    CHECK(!valid(with(ragged, 0x18, uint32_t(ragged.size()))));
    // Each part on a page, and within the file.
    for (size_t part = 0; part < 3; ++part) {
        CHECK(!valid(with(good, 0x20 + 8 * part, 0x1001)) && !valid(with(good, 0x20 + 8 * part, 0x3000)));
        CHECK(!valid(with(with(good, 0x20 + 8 * part, 0x2000), 0x24 + 8 * part, 0x1001)) && valid(with(with(good, 0x20 + 8 * part, 0x2000), 0x24 + 8 * part, 0x1000)));
        CHECK(!valid(with(with(good, 0x20 + 8 * part, 0x1000), 0x24 + 8 * part, 0xFFFFF000))); // an end past what 32 bits hold
    }
    // A part of no bytes may sit at the very end.
    CHECK(valid(with(with(good, 0x30, 0x3000), 0x34, 0)));

    // The list: one page, naming the program twice, with every bit of its ID counted, and the one hash where the list says it is.
    std::vector<uint8_t> page(g::kPage + 16, 0xEE);
    uint8_t hash[tsuchi::kSha256Bytes];
    for (size_t i = 0; i < sizeof hash; ++i) hash[i] = uint8_t(0xA0 + i);
    g::write_module_list(page.data(), 0x0123456789ABCDEF, hash);
    const auto word = [&](size_t at) {
        uint32_t value;
        std::memcpy(&value, page.data() + at, 4);
        return value;
    };
    const auto wide = [&](size_t at) {
        uint64_t value;
        std::memcpy(&value, page.data() + at, 8);
        return value;
    };
    CHECK(!std::memcmp(page.data(), "NRR0", 4) && wide(0x10) == ~uint64_t(0) && wide(0x18) == 0x0123456789ABCDEF && wide(0x330) == 0x0123456789ABCDEF);
    CHECK(word(0x338) == g::kPage && word(0x33C) == 0 && word(0x340) == 0x350 && word(0x344) == 1 && !std::memcmp(page.data() + 0x350, hash, sizeof hash));
    // Nothing else is set, and nothing past the page is touched.
    size_t set = 0;
    for (size_t i = 0; i < g::kPage; ++i) set += page[i] != 0;
    CHECK(set == 4 + 8 + 8 + 8 + 1 + 2 + 1 + sizeof hash && page[g::kPage] == 0xEE);
}

} // namespace

int main() {
    hashing();
    loaded();
    files();
    if (failures) std::printf("%d failed\n", failures);
    return failures;
}
