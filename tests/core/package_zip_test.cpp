// The ZIP reader on a memory archive, the streaming SHA-256, and receipts.
//   package_zip_test zip <file> [cancel|midcancel|write|derived]   prints each extracted file and its SHA-256
//   package_zip_test hash <file>                                    compares streamed and whole hashes
//   package_zip_test receipt <file>                                 checks receipt sealing and validation
#define _CRT_SECURE_NO_WARNINGS
#include "mod_zip.h"
#include "package_receipt.h"
#include <cassert>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <string>
#include <vector>
using namespace tsuchi;
namespace {
std::string input;
std::map<std::string, std::vector<char>> outputs;
bool committed = false, aborted = false, cancelled_flag = false, fail_write = false;
uint32_t opened = 0;
void* alloc(uint64_t n) { return std::malloc(size_t(n)); }
uint32_t open_in(void*, const char*, void** f, uint64_t* n) { *f = &input; *n = input.size(); return 0; }
uint32_t read_in(void*, void*, uint64_t off, void* out, uint32_t n, uint32_t* got) { if (off > input.size() || n > input.size() - off) return 1; memcpy(out, input.data() + off, n); *got = n; return 0; }
void close_in(void*, void*) {}
uint32_t begin(void*, const char*, void** s) { *s = &input; return 0; }
uint32_t open_out(void*, void*, const char* name, uint64_t n, void** f) { ++opened; auto& v = outputs[name]; v.resize(size_t(n)); *f = &v; return 0; }
uint32_t write_out(void*, void* f, uint64_t off, const void* bytes, uint32_t n) { if (fail_write) return 1; auto& v = *static_cast<std::vector<char>*>(f); assert(off <= v.size() && n <= v.size() - off); memcpy(v.data() + off, bytes, n); return 0; }
uint32_t finish(void*, void*) { return 0; }
uint32_t commit(void*, void*) { committed = true; return 0; }
uint32_t abort(void*, void*) { aborted = true; outputs.clear(); return 0; }
bool stopped(void*) { return cancelled_flag; }
void progress(void* p) { if (p && opened) cancelled_flag = true; }
}
int main(int argc, char** argv) {
    assert(argc >= 3);
    std::ifstream stream(argv[2], std::ios::binary);
    input.assign(std::istreambuf_iterator<char>(stream), {});
    if (!strcmp(argv[1], "zip")) {
        const char* how = argc > 3 ? argv[3] : "";
        fail_write = !strcmp(how, "write");
        cancelled_flag = !strcmp(how, "cancel");
        mod_zip::Platform io{nullptr, alloc, std::free, open_in, read_in, close_in, begin, open_out, write_out, finish, commit, abort};
        mod_zip::Status s{};
        mod_zip::Request request{};
        request.program_id = 0x0100B7C00933A000;
        if (!strcmp(how, "derived")) strcpy(request.derived_id, "external.0123456789abcdef01234567");
        auto result = mod_zip::extract_zip(io, request, {stopped, nullptr}, s, progress, !strcmp(how, "midcancel") ? &io : nullptr);
        if (result.committed) {
            assert(committed && !aborted);
            for (const auto& file : outputs) {
                uint8_t hash[32];
                sha256(file.second.data(), file.second.size(), hash);
                std::cout << file.first << " ";
                for (auto c : hash) { const char* h = "0123456789abcdef"; std::cout << h[c >> 4] << h[c & 15]; }
                std::cout << "\n";
            }
            std::cout << "kind " << (s.assets ? "assets" : s.cheats ? "cheats" : "mods") << "\n";
            return 0;
        }
        assert(!committed && (!opened || aborted));
        std::cout << "refused " << int(result.error) << "\n";
        return 2;
    }
    if (!strcmp(argv[1], "hash")) {
        Sha256 streamed;
        for (size_t i = 0; i < input.size(); i += 17) streamed.add(input.data() + i, std::min(size_t(17), input.size() - i));
        uint8_t one[32], two[32];
        sha256(input.data(), input.size(), one);
        streamed.finish(two);
        assert(!memcmp(one, two, 32));
        return 0;
    }
    if (!strcmp(argv[1], "receipt")) {
        namespace ps = package_store;
        ps::Receipt r{};
        r.count = 1;
        strcpy(r.folder, "0123456789abcdef0123456789abcdef");
        memset(r.digest, 'a', 64);
        strcpy(r.source, "github"); strcpy(r.key, "author/mod"); strcpy(r.title, "Mod \xC3\xA9"); strcpy(r.mods[0], "author.mod");
        ps::seal(r);
        assert(ps::receipt_valid(r) && ps::read_receipt(&r, sizeof r, r));
        assert(!ps::read_receipt(&r, sizeof r - 1, r));
        auto changed = r; changed.checksum[0] ^= 1; assert(!ps::receipt_valid(changed));
        changed = r; changed.count = 2; assert(!ps::receipt_valid(changed));
        changed = r; changed.mods[0][0] = '.'; ps::seal(changed); assert(!ps::receipt_valid(changed));
        changed = r; strcpy(changed.mods[0], "tsuchi.self"); ps::seal(changed); assert(!ps::receipt_valid(changed));
        changed = r; memset(changed.key, 'a', sizeof changed.key); ps::seal(changed); assert(!ps::receipt_valid(changed));
        changed = r; strcpy(changed.source, "two words"); ps::seal(changed); assert(!ps::receipt_valid(changed));
        changed = r; strcpy(changed.title, "bad\xff"); ps::seal(changed); assert(!ps::receipt_valid(changed));
        changed = r; changed.kind = 1; changed.count = 1; ps::seal(changed); assert(ps::receipt_valid(changed));
        changed.count = 2; strcpy(changed.mods[1], "other.mod"); ps::seal(changed); assert(!ps::receipt_valid(changed)); // a cooked package has one mod
        changed = r; memcpy(changed.magic, "TSMOD02", 8); sha256(&changed, offsetof(ps::Receipt, checksum), changed.checksum); assert(!ps::receipt_valid(changed));
        // The same package: the same source and key, with a mod in common.
        auto other = r; assert(ps::same_package(r, other));
        strcpy(other.mods[0], "author.other"); assert(!ps::same_package(r, other));
        other.count = 2; strcpy(other.mods[1], "author.mod"); assert(ps::same_package(r, other));
        strcpy(other.key, "author/mod2"); assert(!ps::same_package(r, other));
        // Derived IDs depend on source and key alone, with a separator between them.
        char a[ps::kModIdBytes], b[ps::kModIdBytes], c[ps::kModIdBytes];
        ps::derived_id("ab", "c", a); ps::derived_id("a", "bc", b); ps::derived_id("ab", "c", c);
        assert(strcmp(a, b) && !strcmp(a, c) && ps::mod_id_valid(a) && strlen(a) == 33);
        assert(ps::text_valid("", 2, true) && !ps::text_valid("", 2, false) && !ps::text_valid("\xC0\x80", 8, true) && !ps::text_valid("\xED\xA0\x80", 8, true));
        return 0;
    }
    return 3;
}
