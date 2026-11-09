// The game behind the cheat runtime: the running mods, their files, the save, the controllers and memory.
#include "cheat_runtime.h"
#include "cost_clock.h"

#include "files.h"
#include "game.h"
#include "mods.h"
#include "services.h"

namespace tsuchi {
namespace {

namespace t = game::table;

constexpr const char* kChoicesPath = "save:/tsuchi/cheats.json";
constexpr uint64_t kChoicesBytes = 1024 * 1024;
// The save is opened to read the choices at most this often while it can't be used yet.
constexpr uint32_t kChoicesRetryFrames = 60;

bool (*system_regions)(uint64_t heap_alias_aslr[3]) = nullptr;
uint32_t frames_since_try = kChoicesRetryFrames;

bool mod_at(uint32_t index, const char*& id, TSUCHI_Mod*& context) {
    uint32_t seen = 0;
    for (uint32_t i = 0; i < mod_count(); ++i) {
        ModRecord* record = tsuchi::mod_at(i);
        if (!record || record->state != ModState::running || !record->context) continue;
        if (seen++ == index) {
            id = record->id;
            context = record->context;
            return true;
        }
    }
    return false;
}

TSUCHI_Result load_choices(char*& data, uint64_t& size) {
    if (++frames_since_try < kChoicesRetryFrames) return TSUCHI_NOT_READY;
    frames_since_try = 0;
    const TSUCHI_Result result = read_save_file(kChoicesPath, data, size);
    if (result == TSUCHI_OK && size > kChoicesBytes) {
        release(data);
        data = nullptr;
        return TSUCHI_CAPACITY;
    }
    if (result == TSUCHI_OK || result == TSUCHI_NOT_FOUND) return result;
    return TSUCHI_NOT_READY;
}

TSUCHI_Result save_choices(const char* data, uint64_t size) {
    if (size > kChoicesBytes) return TSUCHI_CAPACITY;
    return write_save_file(kChoicesPath, data, size);
}

bool regions(uint64_t base[4]) {
    uint64_t others[3] = {};
    if (!system_regions || !system_regions(others)) return false;
    base[0] = game::main_base();
    base[1] = others[0];
    base[2] = others[1];
    base[3] = others[2];
    return base[0] != 0;
}

uint64_t keys() { return input_keys_held(); }

} // namespace

void cheats_game_start(bool (*heap_alias_aslr)(uint64_t[3])) {
    system_regions = heap_alias_aslr;
    CheatHost host;
    host.mod_at = mod_at;
    host.read_file = read_mod_file;
    host.load_choices = load_choices;
    host.save_choices = save_choices;
    host.regions = regions;
    host.keys = keys;
    host.readable = game::readable;
    host.writable = game::writable;
    host.code_start = game::main_base();
    host.code_bytes = t::builds[t::selected_build].code_size;
    host.build = t::builds[t::selected_build].build_id;
    host.note = mod_note;
    host.now_us = cost_now;
    set_cheat_host(host);
    cheats_start();
}

} // namespace tsuchi
