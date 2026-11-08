#include "cheat_runtime.h"

#include <string.h>

#include "cheat_vm.h"
#include "frame.h"
#include "hooks.h"
#include "json.h"
#include "registry.h"

namespace tsuchi {
namespace {

CheatHost host{};

struct Patch {
    uint64_t offset;       // from the start of the game's code
    uint32_t original;     // the game's own instruction there
    uint32_t replacement;
    uint64_t id;           // the native backend's
    uint16_t owner;        // the cheat's index, or cheat::kMaster
    bool used;
};

// One mod's cheat file and what the player and the runs made of it.
struct Set {
    char mod_id[kModIdBytes];
    TSUCHI_Mod* context;
    cheat::File file;
    bool on[cheat::kCheats];
    char problem[cheat::kCheats][kCheatProblemBytes];
    cheat::State state;
    Patch patches[kCheatPatches];
    bool needs_code[cheat::kCheats + 1]; // a run found a write to code it couldn't make; the last is the master's
};

Set* sets[kCheatSets] = {};
// Contexts already looked at that had no cheat file, so that each frame doesn't read the files again.
TSUCHI_Mod* checked[256] = {};
uint32_t checked_count = 0;

const Json* saved = nullptr; // the choices as last loaded or saved: {"format":1,"on":{"<mod>":["<name>",...]}}
char* saved_text = nullptr;
bool choices_ready = false;
uint64_t previous_keys = 0;
CheatStats stats{};

void note(const char* mod_id, const char* text) {
    if (host.note) host.note(mod_id, text);
}

void set_problem(Set& set, uint32_t index, const char* text) {
    char* out = set.problem[index];
    size_t length = strlen(text);
    if (length >= kCheatProblemBytes) length = kCheatProblemBytes - 1;
    memcpy(out, text, length);
    out[length] = 0;
}

// ---- Instruction patches --------------------------------------------------

Patch* find_patch(Set& set, uint64_t offset) {
    for (Patch& patch : set.patches)
        if (patch.used && patch.offset == offset) return &patch;
    return nullptr;
}

Patch* free_patch(Set& set) {
    for (Patch& patch : set.patches)
        if (!patch.used) return &patch;
    return nullptr;
}

void drop_patch(Set& set, Patch& patch) {
    const NativeHooks* native = native_hooks_backend();
    if (native) native->remove(set.context, patch.id); // gives the game its own instruction back
    patch = Patch{};
    if (stats.patches) --stats.patches;
}

void drop_patches(Set& set, uint16_t owner) {
    for (Patch& patch : set.patches)
        if (patch.used && patch.owner == owner) drop_patch(set, patch);
}

uint32_t owner_slot(uint16_t owner) { return owner == cheat::kMaster ? cheat::kCheats : owner; }

// Writes code by holding a patch on each word the write covers.
bool patch_code(Set& set, uint64_t address, const void* value, uint32_t bytes, uint16_t owner) {
    const NativeHooks* native = native_hooks_backend();
    if (!native) {
        set.needs_code[owner_slot(owner)] = true;
        return false;
    }
    const uint8_t* source = static_cast<const uint8_t*>(value);
    for (uint64_t word = address & ~uint64_t(3); word < address + bytes; word += 4) {
        if (!host.readable || !host.readable(word, 4)) return false;
        uint32_t current;
        memcpy(&current, reinterpret_cast<const void*>(word), sizeof current);
        uint8_t merged[4];
        memcpy(merged, &current, sizeof merged);
        for (uint32_t i = 0; i < 4; ++i) {
            const uint64_t at = word + i;
            if (at >= address && at < address + bytes) merged[i] = source[at - address];
        }
        uint32_t wanted;
        memcpy(&wanted, merged, sizeof wanted);
        const uint64_t offset = word - host.code_start;
        uint32_t original = current;
        if (Patch* held = find_patch(set, offset)) {
            if (held->replacement == wanted) continue;
            if (held->owner != owner) return false; // another cheat of the file holds this instruction
            original = held->original;
            drop_patch(set, *held);
        }
        if (wanted == original) continue; // the game's own instruction: nothing to hold
        Patch* patch = free_patch(set);
        if (!patch) return false;
        uint64_t id = 0;
        const TSUCHI_Result result = native->patch(set.context, offset, original, wanted, &id);
        if (result == TSUCHI_NOT_READY) set.needs_code[owner_slot(owner)] = true;
        if (result != TSUCHI_OK) return false;
        *patch = Patch{offset, original, wanted, id, owner, true};
        ++stats.patches;
    }
    return true;
}

// ---- The interpreter's view of the game -------------------------------------

bool read_memory(void*, uint64_t address, void* out, uint32_t bytes) {
    if (!host.readable || !host.readable(address, bytes)) return false;
    memcpy(out, reinterpret_cast<const void*>(address), bytes);
    return true;
}

bool write_memory(void* user, uint64_t address, const void* value, uint32_t bytes, uint16_t owner) {
    Set& set = *static_cast<Set*>(user);
    if (host.writable && host.writable(address, bytes)) {
        memcpy(reinterpret_cast<void*>(address), value, bytes);
        return true;
    }
    if (host.code_bytes && address >= host.code_start && address - host.code_start < host.code_bytes &&
        bytes <= host.code_bytes - (address - host.code_start))
        return patch_code(set, address, value, bytes, owner);
    return false;
}

// ---- Choices ----------------------------------------------------------------

const Json* saved_list(const char* mod_id) {
    const Json* on = saved ? saved->find("on") : nullptr;
    const Json* list = on && on->kind == Json::Kind::object ? on->find(mod_id) : nullptr;
    return list && list->kind == Json::Kind::list ? list : nullptr;
}

bool saved_on(const char* mod_id, const char* name) {
    const Json* list = saved_list(mod_id);
    for (const Json* item = list ? list->first : nullptr; item; item = item->next)
        if (item->kind == Json::Kind::text && !strcmp(item->text, name)) return true;
    return false;
}

void forget_saved() {
    if (saved) release_json(saved);
    release(saved_text);
    saved = nullptr;
    saved_text = nullptr;
}

// Takes `text` as the choices if it holds them; false and nothing kept otherwise.
bool keep_saved(char* text, uint64_t size) {
    const Json* root = read_json(text, size_t(size));
    const Json* format = root ? root->find("format") : nullptr;
    const Json* on = root ? root->find("on") : nullptr;
    const bool valid = root && root->kind == Json::Kind::object && format && format->kind == Json::Kind::number &&
                       format->whole && format->integer == 1 && on && on->kind == Json::Kind::object;
    if (!valid) {
        if (root) release_json(root);
        release(text);
        return false;
    }
    forget_saved();
    saved = root;
    saved_text = text;
    return true;
}

bool load_choices() {
    if (!host.load_choices) return choices_ready = true;
    char* data = nullptr;
    uint64_t size = 0;
    const TSUCHI_Result result = host.load_choices(data, size);
    if (result == TSUCHI_NOT_READY) return false;
    if (result == TSUCHI_OK && !keep_saved(data, size)) note("tsuchi", "the saved cheat choices can't be read; every cheat starts off");
    return choices_ready = true;
}

const Set* loaded(const char* mod_id) {
    for (const Set* set : sets)
        if (set && !strcmp(set->mod_id, mod_id)) return set;
    return nullptr;
}

// Room for one name in quotes, its characters escaped.
size_t quoted_bytes(const char* text) {
    size_t bytes = 2;
    for (const char* c = text; *c; ++c) bytes += *c == '"' || *c == '\\' ? 2 : 1;
    return bytes;
}
char* put_quoted(char* out, const char* text) {
    *out++ = '"';
    for (const char* c = text; *c; ++c) {
        if (*c == '"' || *c == '\\') *out++ = '\\';
        *out++ = *c;
    }
    *out++ = '"';
    return out;
}

// One mod's list. `set` gives the loaded choices; without it the saved list is copied.
size_t list_bytes(const Set* set, const Json* list) {
    size_t bytes = 2;
    if (set) {
        for (uint32_t i = 0; i < set->file.count; ++i)
            if (set->on[i]) bytes += quoted_bytes(set->file.cheats[i].name) + 1;
    } else {
        for (const Json* item = list->first; item; item = item->next)
            if (item->kind == Json::Kind::text && strlen(item->text) == item->bytes) bytes += quoted_bytes(item->text) + 1;
    }
    return bytes;
}
char* put_list(char* out, const Set* set, const Json* list) {
    *out++ = '[';
    bool first = true;
    auto put = [&](const char* name) {
        if (!first) *out++ = ',';
        first = false;
        out = put_quoted(out, name);
    };
    if (set) {
        for (uint32_t i = 0; i < set->file.count; ++i)
            if (set->on[i]) put(set->file.cheats[i].name);
    } else {
        for (const Json* item = list->first; item; item = item->next)
            if (item->kind == Json::Kind::text && strlen(item->text) == item->bytes) put(item->text);
    }
    *out++ = ']';
    return out;
}

// Writes every loaded mod's choices, and keeps the saved ones of mods that aren't loaded now.
TSUCHI_Result save_choices() {
    if (!host.save_choices) return TSUCHI_OK;
    const Json* on = saved ? saved->find("on") : nullptr;
    size_t bytes = 32;
    for (const Set* set : sets)
        if (set) bytes += quoted_bytes(set->mod_id) + 1 + list_bytes(set, nullptr) + 1;
    for (const Json* mod = on ? on->first : nullptr; mod; mod = mod->next)
        if (mod->kind == Json::Kind::list && !loaded(mod->name)) bytes += quoted_bytes(mod->name) + 1 + list_bytes(nullptr, mod) + 1;
    char* text = static_cast<char*>(allocate(bytes));
    if (!text) return TSUCHI_NO_MEMORY;
    char* out = text;
    const char* head = "{\"format\":1,\"on\":{";
    memcpy(out, head, strlen(head));
    out += strlen(head);
    bool first = true;
    for (const Set* set : sets) {
        if (!set) continue;
        if (!first) *out++ = ',';
        first = false;
        out = put_quoted(out, set->mod_id);
        *out++ = ':';
        out = put_list(out, set, nullptr);
    }
    for (const Json* mod = on ? on->first : nullptr; mod; mod = mod->next) {
        if (mod->kind != Json::Kind::list || loaded(mod->name)) continue;
        if (!first) *out++ = ',';
        first = false;
        out = put_quoted(out, mod->name);
        *out++ = ':';
        out = put_list(out, nullptr, mod);
    }
    *out++ = '}';
    *out++ = '}';
    *out++ = '\n';
    const uint64_t size = uint64_t(out - text);
    const TSUCHI_Result result = host.save_choices(text, size);
    if (result != TSUCHI_OK || !keep_saved(text, size)) {
        if (result != TSUCHI_OK) release(text);
        return result;
    }
    return TSUCHI_OK;
}

// ---- Finding and forgetting files ---------------------------------------------

uint32_t program_words(const Set& set, int32_t extra) {
    uint32_t words = set.file.has_master ? set.file.master.count : 0;
    for (uint32_t i = 0; i < set.file.count; ++i)
        if (set.on[i] || int32_t(i) == extra) words += set.file.cheats[i].count;
    return words;
}

bool any_on(const Set& set) {
    for (uint32_t i = 0; i < set.file.count; ++i)
        if (set.on[i]) return true;
    return false;
}

void load_set(const char* mod_id, TSUCHI_Mod* context, char* text, uint64_t size) {
    uint32_t slot = 0;
    while (slot < kCheatSets && sets[slot]) ++slot;
    if (slot == kCheatSets) {
        note(mod_id, "its cheat file isn't used: too many mods have one");
        return;
    }
    Set* set = static_cast<Set*>(allocate(sizeof(Set)));
    if (!set) {
        note(mod_id, "its cheat file isn't used: there's no memory for it");
        return;
    }
    memset(set, 0, sizeof *set);
    char problem[128];
    if (cheat::parse(text, size, set->file, problem, sizeof problem) != TSUCHI_OK) {
        note(mod_id, problem);
        release(set);
        return;
    }
    memcpy(set->mod_id, mod_id, strlen(mod_id) + 1);
    set->context = context;
    uint32_t stopped = 0;
    const bool master_ok = !set->file.has_master || cheat::decodable(set->file.master.words, set->file.master.count, stopped);
    for (uint32_t i = 0; i < set->file.count; ++i) {
        if (!master_ok) set_problem(*set, i, "the master block has an instruction this interpreter can't read");
        else if (!cheat::decodable(set->file.cheats[i].words, set->file.cheats[i].count, stopped))
            set_problem(*set, i, "it has an instruction this interpreter can't read");
        else if (saved_on(mod_id, set->file.cheats[i].name) && program_words(*set, int32_t(i)) <= cheat::kProgramWords)
            set->on[i] = true;
    }
    sets[slot] = set;
}

bool running(const char* id, TSUCHI_Mod* context) {
    const char* other = nullptr;
    TSUCHI_Mod* other_context = nullptr;
    for (uint32_t i = 0; host.mod_at(i, other, other_context); ++i)
        if (other_context == context && !strcmp(other, id)) return true;
    return false;
}

void forget_stopped() {
    for (Set*& set : sets) {
        if (!set || running(set->mod_id, set->context)) continue;
        // The mod's stop took its instruction patches away with everything else it owned.
        for (Patch& patch : set->patches)
            if (patch.used && stats.patches) --stats.patches;
        release(set);
        set = nullptr;
    }
    for (uint32_t i = 0; i < checked_count;) {
        bool still = false;
        const char* id = nullptr;
        TSUCHI_Mod* context = nullptr;
        for (uint32_t k = 0; host.mod_at(k, id, context); ++k) still = still || context == checked[i];
        if (still) ++i;
        else checked[i] = checked[--checked_count];
    }
}

bool known(TSUCHI_Mod* context) {
    for (const Set* set : sets)
        if (set && set->context == context) return true;
    for (uint32_t i = 0; i < checked_count; ++i)
        if (checked[i] == context) return true;
    return false;
}

void find_new() {
    if (!host.read_file || !host.build) return;
    char path[64];
    const char* prefix = "cheats/";
    memcpy(path, prefix, strlen(prefix));
    const size_t build = strlen(host.build) < 16 ? strlen(host.build) : 16;
    memcpy(path + strlen(prefix), host.build, build);
    memcpy(path + strlen(prefix) + build, ".txt", 5);
    const char* id = nullptr;
    TSUCHI_Mod* context = nullptr;
    for (uint32_t i = 0; host.mod_at(i, id, context); ++i) {
        if (!context || known(context)) continue;
        char* text = nullptr;
        uint64_t size = 0;
        const TSUCHI_Result result = host.read_file(id, path, text, size);
        if (result == TSUCHI_OK) {
            load_set(id, context, text, size);
            release(text);
        }
        if (result == TSUCHI_NOT_READY) continue; // looked at again next frame
        if (!known(context) && checked_count < sizeof checked / sizeof checked[0]) checked[checked_count++] = context;
    }
}

// ---- Running ------------------------------------------------------------------

uint32_t program[cheat::kProgramWords];
uint16_t owners[cheat::kProgramWords];

void turn_off(Set& set, uint32_t index, const char* why) {
    set.on[index] = false;
    set_problem(set, index, why);
    drop_patches(set, uint16_t(index));
    note(set.mod_id, why);
}

void run_set(Set& set, const uint64_t base[4], uint64_t keys) {
    if (!any_on(set)) return;
    uint32_t count = 0;
    auto add = [&](const cheat::Block& block, uint16_t owner) {
        for (uint32_t i = 0; i < block.count && count < cheat::kProgramWords; ++i) {
            program[count] = block.words[i];
            owners[count++] = owner;
        }
    };
    if (set.file.has_master) add(set.file.master, cheat::kMaster);
    for (uint32_t i = 0; i < set.file.count; ++i)
        if (set.on[i]) add(set.file.cheats[i], uint16_t(i));
    cheat::Memory memory{{base[0], base[1], base[2], base[3]}, &set, read_memory, write_memory, keys, previous_keys};
    memset(set.needs_code, 0, sizeof set.needs_code);
    const cheat::Report report = cheat::run(program, owners, count, memory, set.state);
    ++stats.runs;
    stats.faults += report.faults;
    if (report.stopped || report.step_limit) {
        const char* why = report.step_limit ? "it ran too long without finishing" : "it has an instruction this interpreter can't read";
        if (report.stopped_owner == cheat::kMaster) {
            for (uint32_t i = 0; i < set.file.count; ++i)
                if (set.on[i]) turn_off(set, i, "the master block stopped it");
        } else if (report.stopped_owner < set.file.count) {
            turn_off(set, report.stopped_owner, why);
        }
    }
    const char* code = "it changes the game's code, and this install has no code access";
    for (uint32_t i = 0; i < set.file.count; ++i)
        if (set.on[i] && set.needs_code[i]) turn_off(set, i, code);
    if (set.needs_code[cheat::kCheats])
        for (uint32_t i = 0; i < set.file.count; ++i)
            if (set.on[i]) turn_off(set, i, code);
    if (!any_on(set)) drop_patches(set, cheat::kMaster);
}

Set* set_of(const char* mod_id) {
    for (Set* set : sets)
        if (set && !strcmp(set->mod_id, mod_id)) return set;
    return nullptr;
}

} // namespace

void set_cheat_host(const CheatHost& next) { host = next; }

void cheats_frame() {
    if (!host.mod_at) return;
    if (!choices_ready && !load_choices()) return;
    forget_stopped();
    find_new();
    uint64_t base[4] = {};
    if (!host.regions || !host.regions(base)) return;
    const uint64_t keys = host.keys ? host.keys() : 0;
    for (Set* set : sets)
        if (set) run_set(*set, base, keys);
    previous_keys = keys;
}

void cheats_start() { each_engine_frame(cheats_frame); }

uint32_t cheat_count() {
    uint32_t count = 0;
    for (const Set* set : sets)
        if (set) count += set->file.count;
    return count;
}

bool cheat_at(uint32_t index, CheatEntry& out) {
    for (const Set* set : sets) {
        if (!set) continue;
        if (index < set->file.count) {
            out = CheatEntry{set->mod_id, set->file.cheats[index].name, set->on[index], set->problem[index]};
            return true;
        }
        index -= set->file.count;
    }
    return false;
}

TSUCHI_Result set_cheat(const char* mod_id, const char* name, bool on) {
    if (!mod_id || !name || !on_game_thread()) return TSUCHI_INVALID;
    Set* set = set_of(mod_id);
    if (!set) return TSUCHI_NOT_FOUND;
    uint32_t index = 0;
    while (index < set->file.count && strcmp(set->file.cheats[index].name, name)) ++index;
    if (index == set->file.count) return TSUCHI_NOT_FOUND;
    if (set->on[index] == on) return TSUCHI_OK;
    if (on) {
        if (set->problem[index][0]) return TSUCHI_UNSUPPORTED;
        if (program_words(*set, int32_t(index)) > cheat::kProgramWords) return TSUCHI_CAPACITY;
        set->on[index] = true;
    } else {
        set->on[index] = false;
        drop_patches(*set, uint16_t(index));
        if (!any_on(*set)) drop_patches(*set, cheat::kMaster);
    }
    // The cheat runs, or stops, whether or not the save takes the choice.
    return save_choices();
}

CheatStats cheat_stats() { return stats; }

void cheats_reset() {
    for (Set*& set : sets) {
        release(set);
        set = nullptr;
    }
    checked_count = 0;
    forget_saved();
    choices_ready = false;
    previous_keys = 0;
    stats = {};
}

} // namespace tsuchi
