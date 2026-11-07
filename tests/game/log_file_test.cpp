// The runtime logger with an SD filesystem and SDK context stand-in. No console or emulator result.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <string>

#include "frame.h"
#include "game.h"
#include "modules.h"
#include "registry.h"
#include "services.h"

namespace t = tsuchi::game::table;
static int failures = 0;
#define CHECK(value) do { if (!(value)) { ++failures; std::printf("line %d: %s\n", __LINE__, #value); } } while (0)

static std::string mode;
static void (*step)() = nullptr;
static int original_context, return_context;
static const void* context = &original_context;
static uint32_t mounts = 0, saves = 0, open_files = 0;
static bool refuse_write = false;
static std::map<std::string, std::string> files;
static std::set<std::string> folders;
static std::string opened;
static const std::string sd_path = "tsuchisd:/atmosphere/contents/0100B7C00933A000/tsuchi.log";
static const std::string save_path = "save:/tsuchi/tsuchi.log";

static const void* get_context() { return context; }
static void set_context(const void* value) { context = value; }
static uint32_t mount(const char* name) {
    CHECK(context == &return_context && std::strcmp(name, "tsuchisd") == 0);
    ++mounts;
    return mode == "mount_denied" ? 1 : 0;
}
static uint32_t entry_kind(void* out, void* path) {
    CHECK(context == &return_context);
    const auto name = static_cast<const char*>(path);
    if (folders.count(name)) { *static_cast<int32_t*>(out) = int32_t(t::constant::fs_entry_folder); return 0; }
    if (files.count(name)) { *static_cast<int32_t*>(out) = 1; return 0; }
    return uint32_t(t::constant::fs_no_such_path);
}
static uint32_t make_folder(void* path) {
    CHECK(context == &return_context);
    folders.insert(static_cast<const char*>(path));
    return 0;
}
static uint32_t make_file(void* path, int64_t size) {
    CHECK(context == &return_context && size >= 0);
    files[static_cast<const char*>(path)].resize(size_t(size));
    return 0;
}
static uint32_t open_file(void* out, void* path, int32_t flags) {
    CHECK(context == &return_context && uint32_t(flags) == t::constant::fs_write && !open_files);
    opened = static_cast<const char*>(path);
    *static_cast<void**>(out) = &opened;
    ++open_files;
    return 0;
}
static void close_file(void*) { CHECK(context == &return_context && open_files == 1); --open_files; }
static uint32_t file_size(void* out, void*) {
    CHECK(context == &return_context && open_files == 1);
    *static_cast<int64_t*>(out) = int64_t(files[opened].size());
    return 0;
}
static uint32_t resize_file(void*, int64_t size) {
    CHECK(context == &return_context && open_files == 1 && size >= 0);
    files[opened].resize(size_t(size));
    return 0;
}
static uint32_t write_file(void*, int64_t at, void* data, uint64_t bytes, void* how) {
    CHECK(context == &return_context && open_files == 1 && at >= 0 && *static_cast<uint32_t*>(how) == t::constant::fs_write_through);
    if (refuse_write || mode == "write_denied") return 1;
    auto& file = files[opened];
    CHECK(uint64_t(at) + bytes <= file.size());
    std::memcpy(&file[size_t(at)], data, size_t(bytes));
    return 0;
}
static uint32_t remove_file(void* path) {
    CHECK(context == &return_context && !open_files);
    files.erase(static_cast<const char*>(path));
    return 0;
}

namespace tsuchi {
void* allocate(size_t bytes) { return std::malloc(bytes); }
void release(void* data) { std::free(data); }
bool each_frame(void (*callback)()) { step = callback; return true; }
TSUCHI_Result append_save_file(const char* path, const void* data, uint64_t size, bool from_start) {
    CHECK(context == &original_context && !open_files);
    ++saves;
    if (from_start) files[path].clear();
    files[path].append(static_cast<const char*>(data), size_t(size));
    return TSUCHI_OK;
}
} // namespace tsuchi

namespace tsuchi::game {
uint64_t main_base() { return 0; }
bool readable(uint64_t, uint64_t) { return false; }
uint64_t module_containing(uint64_t) { return mode == "no_sdk" ? 0 : 0x1000; }
uint64_t module_symbol(uint64_t module, const char* name) {
    if (!module) return 0;
    if (!std::strcmp(name, "_ZN2nn2fs25GetCurrentThreadFsContextEv")) return reinterpret_cast<uint64_t>(&get_context);
    if (!std::strcmp(name, "_ZN2nn2fs25SetCurrentThreadFsContextEPKNS0_9FsContextE"))
        return mode == "no_context" ? 0 : reinterpret_cast<uint64_t>(&set_context);
    if (!std::strcmp(name, "_ZN2nn2fs18g_AllReturnContextE")) return reinterpret_cast<uint64_t>(&return_context);
    if (!std::strcmp(name, "_ZN2nn2fs11MountSdCardEPKc")) return reinterpret_cast<uint64_t>(&mount);
    CHECK(false);
    return 0;
}
} // namespace tsuchi::game

int main(int argc, char** argv) {
    CHECK(argc == 2);
    if (argc != 2) return 1;
    mode = argv[1];
    t::fn::fs_entry_kind = &entry_kind;
    t::fn::fs_create_folder = &make_folder;
    t::fn::fs_create_file = &make_file;
    t::fn::fs_open_file = &open_file;
    t::fn::fs_close_file = &close_file;
    t::fn::fs_file_size = &file_size;
    t::fn::fs_set_file_size = &resize_file;
    t::fn::fs_write_file = &write_file;
    t::fn::fs_delete_file = &remove_file;
    tsuchi::log_file_start();
    CHECK(step != nullptr);
    step(); // no mount and no save operation while nothing waits
    CHECK(!mounts && !saves && !open_files && context == &original_context);
    // Both the first write's truncation and a later append are exercised. The other destination stays untouched.
    files[sd_path] = "previous SD run has a longer log";
    files[save_path] = "previous save run";
    tsuchi::log_file_line("test", "hello");
    step();
    const bool uses_sd = mode == "sd" || mode == "later_refused";
    const auto& path = uses_sd ? sd_path : save_path;
    CHECK(files[path] == "[test] hello\n" && !open_files && context == &original_context);
    CHECK(uses_sd ? !saves && files[save_path] == "previous save run" : saves == 1);
    tsuchi::log_file_line("test", "second");
    for (unsigned i = 0; i < 60; ++i) step();
    CHECK(files[path] == "[test] hello\n");
    if (mode == "later_refused") {
        refuse_write = true;
        step();
        CHECK(files[path] == "[test] hello\n" && !saves && !open_files && context == &original_context);
        CHECK(tsuchi::log_file_counts().waiting_bytes == 14);
        refuse_write = false;
        for (unsigned i = 0; i < 300; ++i) step();
        CHECK(files[path] == "[test] hello\n");
    }
    step();
    CHECK(files[path] == "[test] hello\n[test] second\n" && !open_files && context == &original_context);
    CHECK(tsuchi::log_file_counts().waiting_bytes == 0 && tsuchi::log_file_counts().writes == 2);
    CHECK(uses_sd ? !saves && files[save_path] == "previous save run" : saves == 2);
    return failures ? 1 : 0;
}
