#include "fake_game.h"
#include "mod_choices_game.h"
#include <cassert>
#include <set>
#include <string>
using namespace tsuchi;
namespace {
namespace t = game::table;
std::string file;
bool present = false, known = true, can_open = true, is_open = false;
unsigned opens = 0, closes = 0, reads = 0, writes = 0;
uint64_t thread = 1, measured_size = 0;
TSUCHI_Result read_result = TSUCHI_OK, write_result = TSUCHI_OK;
int allocations_left = -1;
std::set<void*> allocated;
void* watched_allocate(size_t bytes) {
    if (!allocations_left) return nullptr;
    if (allocations_left > 0) --allocations_left;
    void* block = std::malloc(bytes);
    if (block) allocated.insert(block);
    return block;
}
void watched_release(void* block) { allocated.erase(block); std::free(block); }
bool begin(int32_t mode, bool a, bool b) {
    assert(!mode && !a && !b && !is_open); ++opens; is_open = can_open; return is_open;
}
void end() { assert(is_open); is_open = false; ++closes; }
void path(const char* value) { assert(!strcmp(value, "save:/tsuchi/mods.json")); }
void seed(const std::string& value) { file = value; present = true; measured_size = value.size(); }
void previous() { assert(!mod_choices().enabled("bulbmin") && mod_choices().rank("zoo") == 0); }
}
namespace tsuchi::game {
bool files_known() { return known; }
TSUCHI_Result file_size(const char* name, uint64_t& size) {
    path(name); assert(is_open); size = measured_size;
    return present ? read_result : TSUCHI_NOT_FOUND;
}
TSUCHI_Result read_whole_file(const char* name, char*& out, uint64_t& bytes) {
    path(name); assert(is_open); ++reads; bytes = file.size();
    out = static_cast<char*>(allocate(bytes + 1));
    if (!out) return TSUCHI_NO_MEMORY;
    memcpy(out, file.c_str(), bytes + 1); return TSUCHI_OK;
}
}
namespace tsuchi {
TSUCHI_Result write_save_file(const char* name, const void* value, uint64_t bytes) {
    path(name); assert(!is_open); ++writes;
    if (write_result == TSUCHI_OK) seed(std::string(static_cast<const char*>(value), size_t(bytes)));
    return write_result;
}
}
int main() {
    set_thread_probe([] { return thread; }); note_game_thread();
    fake::main_image() = fake::memory(0x6000);
    t::fn::save_begin_session = begin; t::fn::save_end_session = end;
    mod_choices_start();
    known = false; assert(load_mod_choices() == TSUCHI_NOT_READY && !opens); known = true;
    for (const auto flag : {t::data::save_async_state, t::data::save_mounted, t::data::engine_save_mounted}) {
        fake::put<uint8_t>(fake::main_image() + flag, 1);
        assert(load_mod_choices() == TSUCHI_NOT_READY && !opens);
        fake::put<uint8_t>(fake::main_image() + flag, 0);
    }
    can_open = false; assert(load_mod_choices() == TSUCHI_NOT_READY && opens == 1 && !closes); can_open = true;
    assert(load_mod_choices() == TSUCHI_OK && mod_choices().enabled("bulbmin") && !reads);
    const std::string good = "{\"format\":1,\"activation\":\"p4a1:bulbmin:absent.mod:\",\"order\":\"p4o1:zoo:bulbmin:\"}";
    seed(good); assert(load_mod_choices() == TSUCHI_OK); previous();
    for (const char* bad : {
        "{}", "{\"format\":2,\"activation\":\"\",\"order\":\"\"}",
        "{\"format\":1,\"activation\":\"p4a1:x:x:\",\"order\":\"\"}",
        "{\"format\":1,\"activation\":\"\",\"order\":\"p4o1:x:x:\"}",
        "{\"format\":1,\"activation\":\"p4a1:x:\\u0000\",\"order\":\"\"}",
        "{\"format\":1,\"activation\":\"\",\"order\":\"\",\"extra\":1}",
        "{\"format\":1,\"activation\":\"\",\"activation\":\"\"}"
    }) { seed(bad); assert(load_mod_choices() == TSUCHI_INVALID); previous(); }
    measured_size = 256 * 1024 + 1;
    const unsigned before = reads; assert(load_mod_choices() == TSUCHI_CAPACITY && reads == before); previous();
    read_result = TSUCHI_IO; assert(load_mod_choices() == TSUCHI_IO); previous(); read_result = TSUCHI_OK;
    seed(good);
    ModChoices draft; assert(draft.load("p4a1:other:", "p4o1:bulbmin:zoo:") == TSUCHI_OK);
    thread = 2; assert(save_mod_choices(draft) == TSUCHI_INVALID && load_mod_choices() == TSUCHI_INVALID && !writes); thread = 1;
    set_allocator({[](size_t)->void* { return nullptr; }, std::free});
    assert(save_mod_choices(draft) == TSUCHI_NO_MEMORY && !writes);
    assert(load_mod_choices() == TSUCHI_NO_MEMORY); previous();
    set_allocator({std::malloc, std::free});
    // Refuse each allocation through file read, JSON document/blocks and staged choices. Even a valid file
    // remains unresolved, with the prior saved selection intact and no partial memory or save session left.
    bool loaded = false;
    for (int available = 0; available < 64; ++available) {
        allocations_left = available;
        set_allocator({watched_allocate, watched_release});
        const auto result = load_mod_choices();
        set_allocator({std::malloc, std::free});
        assert(!is_open && opens == closes + 1);
        previous();
        if (result == TSUCHI_OK) {
            assert(available >= 3);
            allocated.clear(); // the committed choices retain these blocks and release them normally later
            loaded = true;
            break;
        }
        assert(result == TSUCHI_NO_MEMORY && allocated.empty());
    }
    assert(loaded);
    for (auto failed : {TSUCHI_BUSY, TSUCHI_IO, TSUCHI_NOT_READY}) {
        write_result = failed; assert(save_mod_choices(draft) == failed && file == good); previous();
    }
    write_result = TSUCHI_OK; assert(save_mod_choices(draft) == TSUCHI_OK);
    assert(file == "{\"format\":1,\"activation\":\"p4a1:other:\",\"order\":\"p4o1:bulbmin:zoo:\"}\n");
    assert(mod_choices().enabled("bulbmin") && !mod_choices().enabled("other"));
    assert(load_mod_choices() == TSUCHI_OK && mod_choices().rank("bulbmin") == 0 && !mod_choices().enabled("other"));
    assert(!is_open && opens == closes + 1);
    draft.clear(); set_mod_choices_loader(nullptr);
}
