/* Reads the calling mod's own files, whole and in parts, and refuses paths outside its folder. */
#include <string.h>
#include "tsuchi/tsuchi.h"
#include "tsuchi_check.h"

TSUCHI_Result tsuchi_mod_start(const TSUCHI_Host *host, TSUCHI_Mod *self) {
    const TSUCHI_FileService *files = host->file;
    const char *const every[] = {"read", "parts", "exists", "listed", "refused"};
    char text[64] = {0}, names[512] = {0};
    unsigned char bytes[256] = {0};
    uint64_t size = 0, got = 0;
    bool there = false;
    tsuchi_check_start(host, self);
    for (unsigned i = 0; i < sizeof every / sizeof *every; ++i) tsuchi_check_plan(every[i]);

    tsuchi_check_begin("read");
    tsuchi_expect_result(files->size(self, "data/hello.txt", &size), TSUCHI_OK, "the text file's size");
    tsuchi_expect_number((int64_t)size, 25, "the text file's bytes");
    tsuchi_expect_result(files->read(self, "data/hello.txt", 0, text, sizeof text, &got), TSUCHI_OK, "reading the text file");
    if (tsuchi_expect_number((int64_t)got, 25, "the bytes read")) tsuchi_expect_bytes(text, "hello from a mod's folder", 25, "the text file");
    tsuchi_expect_result(files->size(self, "data/count.bin", &size), TSUCHI_OK, "the binary file's size");
    tsuchi_expect_number((int64_t)size, 256, "the binary file's bytes");
    tsuchi_expect_result(files->read(self, "data/count.bin", 0, bytes, sizeof bytes, &got), TSUCHI_OK, "reading the binary file");
    bool right = got == sizeof bytes;
    for (unsigned i = 0; right && i < sizeof bytes; ++i) right = bytes[i] == i;
    tsuchi_expect(right, "the binary file holds the bytes from zero through 255");
    tsuchi_check_end();

    tsuchi_check_begin("parts");
    tsuchi_expect_result(files->read(self, "data/hello.txt", 0, text, 5, &got), TSUCHI_OK, "reading the start");
    if (tsuchi_expect_number((int64_t)got, 5, "the start's bytes")) tsuchi_expect_bytes(text, "hello", 5, "the start");
    tsuchi_expect_result(files->read(self, "data/count.bin", 253, bytes, 8, &got), TSUCHI_OK, "reading to the end");
    if (tsuchi_expect_number((int64_t)got, 3, "only the bytes left")) tsuchi_expect(bytes[0] == 253 && bytes[1] == 254 && bytes[2] == 255, "the last three bytes");
    tsuchi_expect_result(files->read(self, "data/count.bin", 257, bytes, 8, &got), TSUCHI_OK, "reading past the end");
    tsuchi_expect_number((int64_t)got, 0, "no bytes past the end");
    tsuchi_check_end();

    tsuchi_check_begin("exists");
    tsuchi_expect_result(files->exists(self, "mod.json", &there), TSUCHI_OK, "asking about the manifest");
    tsuchi_expect(there, "the manifest is there");
    tsuchi_expect_result(files->exists(self, "data/absent.txt", &there), TSUCHI_OK, "asking about an absent file");
    tsuchi_expect(!there, "the absent file is not there");
    tsuchi_expect_result(files->size(self, "data/absent.txt", &size), TSUCHI_NOT_FOUND, "the size of an absent file");
    tsuchi_check_end();

    tsuchi_check_begin("listed");
    tsuchi_expect_result(files->list(self, "", names, sizeof names, &size), TSUCHI_OK, "listing the mod's folder");
    bool manifest = false, data = false;
    for (const char *name = names; *name; name += strlen(name) + 1) {
        manifest = manifest || !strcmp(name, "mod.json");
        data = data || !strcmp(name, "data/");
    }
    tsuchi_expect(manifest && data, "the manifest and data folder are listed");
    tsuchi_expect_result(files->list(self, "data", names, sizeof names, &size), TSUCHI_OK, "listing the data folder");
    bool hello = false, count = false;
    for (const char *name = names; *name; name += strlen(name) + 1) {
        hello = hello || !strcmp(name, "hello.txt");
        count = count || !strcmp(name, "count.bin");
    }
    tsuchi_expect(hello && count, "both data files are listed");
    tsuchi_check_end();

    tsuchi_check_begin("refused");
    tsuchi_expect_result(files->size(self, "../check.library/mod.json", &size), TSUCHI_INVALID, "another mod's manifest through a parent path");
    tsuchi_expect_result(files->read(self, "/mod.json", 0, text, sizeof text, &got), TSUCHI_INVALID, "an absolute path");
    tsuchi_expect_result(files->list(self, "..", names, sizeof names, &size), TSUCHI_INVALID, "listing outside the mod's folder");
    tsuchi_check_end();
    return TSUCHI_OK;
}
