#include "modules.h"

#include <string.h>

#include "game.h"

namespace tsuchi::game {

namespace {

constexpr uint32_t kModuleMark = 0x30444F4D; // "MOD0"
constexpr uint32_t kFileMark = 0x304F524E;   // "NRO0"
constexpr uint32_t kListMark = 0x3052524E;   // "NRR0"
// A module's dynamic table: the tags of the entries read here.
constexpr uint64_t kTagHash = 4, kTagStrings = 5, kTagSymbols = 6, kTagGnuHash = 0x6FFFFEF5;
constexpr uint64_t kSymbolBytes = 24;

template <class T>
T at(uint64_t address) {
    T value;
    memcpy(&value, reinterpret_cast<const void*>(address), sizeof value);
    return value;
}

void put(uint8_t* to, uint32_t value) { memcpy(to, &value, sizeof value); }
void put(uint8_t* to, uint64_t value) { memcpy(to, &value, sizeof value); }

// A module starts with two words, the second of them how far on its header is. The SDK of the Switch 2 builds puts
// the header at the start of the read-only part (the demo's system library at +0x84101C, its program at +0x5B86070),
// so it is not always in the first page; `read` checks that it is there to be read.
uint64_t header_of(uint64_t module) {
    uint32_t offset = 0, mark = 0;
    if (!read(module + 4, offset) || offset < 8 || offset % 4) return 0;
    return read(module + offset, mark) && mark == kModuleMark ? module + offset : 0;
}

// How many symbols a module's table has, which only its hash tables say.
uint32_t symbol_count(uint64_t hash, uint64_t gnu_hash) {
    if (hash) return at<uint32_t>(hash + 4);
    if (!gnu_hash) return 0;
    // The highest symbol any bucket starts a chain at, and then to the end of that chain.
    const uint32_t buckets = at<uint32_t>(gnu_hash), first = at<uint32_t>(gnu_hash + 4), filters = at<uint32_t>(gnu_hash + 8);
    const uint64_t bucket = gnu_hash + 16 + uint64_t(filters) * 8, chain = bucket + uint64_t(buckets) * 4;
    uint32_t last = 0;
    for (uint32_t i = 0; i < buckets; ++i)
        if (at<uint32_t>(bucket + 4 * uint64_t(i)) > last) last = at<uint32_t>(bucket + 4 * uint64_t(i));
    if (last < first) return 0;
    while (!(at<uint32_t>(chain + 4 * uint64_t(last - first)) & 1)) ++last;
    return last + 1;
}

} // namespace

uint64_t module_containing(uint64_t address) {
    for (uint64_t page = address & ~(kPage - 1); page && readable(page, 8); page -= kPage)
        if (header_of(page)) return page;
    return 0;
}

uint64_t module_symbol(uint64_t module, const char* name) {
    const uint64_t header = header_of(module);
    if (!header) return 0;
    uint64_t symbols = 0, strings = 0, hash = 0, gnu_hash = 0;
    for (uint64_t entry = header + uint64_t(int64_t(at<int32_t>(header + 4))); at<uint64_t>(entry); entry += 16) {
        const uint64_t tag = at<uint64_t>(entry), value = module + at<uint64_t>(entry + 8);
        if (tag == kTagSymbols) symbols = value;
        if (tag == kTagStrings) strings = value;
        if (tag == kTagHash) hash = value;
        if (tag == kTagGnuHash) gnu_hash = value;
    }
    if (!symbols || !strings) return 0;
    const uint32_t count = symbol_count(hash, gnu_hash);
    for (uint32_t i = 1; i < count; ++i) {
        const uint64_t symbol = symbols + kSymbolBytes * i;
        const uint8_t binding = at<uint8_t>(symbol + 4) >> 4;
        // One the module defines itself, for others to use.
        if (!at<uint16_t>(symbol + 6) || (binding != 1 && binding != 2)) continue;
        if (!strcmp(reinterpret_cast<const char*>(strings + at<uint32_t>(symbol)), name)) return module + at<uint64_t>(symbol + 8);
    }
    return 0;
}

bool module_file_valid(const uint8_t* image, uint64_t bytes) {
    if (bytes < kPage || bytes % kPage) return false;
    uint32_t mark, size;
    memcpy(&mark, image + 0x10, 4), memcpy(&size, image + 0x18, 4);
    if (mark != kFileMark || size != bytes) return false;
    for (int part = 0; part < 3; ++part) { // code, what is only read, data
        uint32_t offset, length;
        memcpy(&offset, image + 0x20 + 8 * part, 4), memcpy(&length, image + 0x24 + 8 * part, 4);
        if (offset % kPage || uint64_t(offset) + length > bytes) return false;
    }
    return true;
}

void write_module_list(uint8_t* page, uint64_t program, const uint8_t hash[kSha256Bytes], bool wide) {
    memset(page, 0, kPage);
    put(page, kListMark);
    put(page + 0x10, ~uint64_t(0)); // every bit of the program's ID has to match
    put(page + 0x18, program);
    // The wide form says so in three bytes: the key sizes of the list's signature and of its certificate, and the
    // form's version, the only one the demo's system takes (it reads them at +5, +0x20 and +6).
    const uint32_t fields = wide ? 0x4B0 : 0x330;
    if (wide) page[5] = 1, page[6] = 1, page[0x20] = 1;
    put(page + fields, program);
    put(page + fields + 8, uint32_t(kPage));                 // the list's size
    put(page + fields + 0x10, uint32_t(fields + 0x20));      // where its hashes are: right after these fields
    put(page + fields + 0x14, uint32_t(1));                  // and how many
    memcpy(page + fields + 0x20, hash, kSha256Bytes);
}

} // namespace tsuchi::game
