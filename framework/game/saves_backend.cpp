// The game side of the saves service.
//
// The game reaches its saves through one object, the save system, by a few
// functions in its table: is a file there, read it, write it, delete it. A
// hold on saves and a stand-in for a slot are both made by wrapping those.
// Listing and reading a slot's files goes past them, through the file
// functions of the system the game runs on, with the save opened the way the
// game opens it.

#include <string.h>

#include "files.h"
#include "flow.h"
#include "game.h"
#include "holds.h"
#include "hooks.h"
#include "list.h"
#include "reflect.h"
#include "registry.h"
#include "save_restore.h"
#include "save_session.h"
#include "save_service.h"
#include "services.h"
#include "texts.h"

namespace tsuchi {
namespace {

namespace t = game::table;

const Kind kStandIn{"save.stand_in"};

SaveCounts counts{};
bool wrapped = false, reads_wrapped = false;
// The save system's own functions, as the game table gives them.
decltype(t::slot::save_write)::Function game_write = nullptr;
decltype(t::slot::save_delete)::Function game_delete = nullptr;
decltype(t::slot::save_exists)::Function game_exists = nullptr;
decltype(t::slot::save_exists_result)::Function game_exists_result = nullptr;
decltype(t::slot::save_read)::Function game_read = nullptr;

// One file of a stand-in: its path within the slot as the game spells its names, and what it holds.
struct StandFile {
    char16_t* name;
    size_t units;
    uint8_t* data;
    uint64_t size;
};
// Files a mod has put in a slot's place, or means to.
struct StandIn {
    TSUCHI_Mod* mod;
    uint32_t slot;
    List<StandFile> files;
    bool used;
} stand{};

// True for a file in a save slot: the game names those by the slot's letter, a slash, and the file.
// Its settings are named otherwise and are no slot's.
bool in_a_slot(const char16_t* name) {
    if (!name || !game::readable(reinterpret_cast<uint64_t>(name), 4)) return false;
    return name[0] >= u'A' && name[0] <= u'Z' && name[1] == u'/';
}

// What the game told to be written is noted, for whoever wants to see that a hold held.
void note(const char16_t* name) {
    uint32_t i = 0;
    for (; i + 1 < sizeof counts.last / sizeof counts.last[0] && game::readable(reinterpret_cast<uint64_t>(name + i), 2) && name[i] && name[i] < 0x7F; ++i)
        counts.last[i] = char(name[i]);
    counts.last[i] = 0;
}

// Set when the slots have to stay closed after what closed them is gone: a hold that ended
// because its mod stopped, or a stand-in the game may have loaded from. The game then holds a story
// that is not the slot's own, and its next save would put it there. At the title screen the game
// has let that story go, and the slots open again.
bool closed_until_title = false;

void close_until_title() { closed_until_title = !game::at_title(); }

// A hold on the save slots is on the game itself, and any number of mods hold at once. A mod that
// lets go ends its hold there. One that stops did not let go, so nothing says the game may save again.
void end_hold(const Hold&, HoldNothing&, HoldEnd why) {
    if (why == HoldEnd::stopped) close_until_title();
}

Holds<HoldNothing, HoldNothing, nullptr, nullptr, end_hold> holders{"save.hold", HoldOn::nothing, false};

bool held() {
    if (closed_until_title && game::at_title()) closed_until_title = false;
    return holders.count() || stand.used || closed_until_title;
}

bool on_write(void* system, bool with_screen, const char16_t* name, int32_t user, const void* bytes) {
    if (held() && in_a_slot(name)) {
        ++counts.held_back;
        note(name);
        return true; // as if it had been written
    }
    ++counts.passed;
    return game_write(system, with_screen, name, user, bytes);
}

bool on_delete(void* system, bool with_screen, const char16_t* name, int32_t user) {
    if (held() && in_a_slot(name)) {
        ++counts.held_back;
        note(name);
        return true;
    }
    ++counts.passed;
    return game_delete(system, with_screen, name, user);
}

// ---- The stand-in ------------------------------------------------------------------------------

// Whether the game's name is for a file in the slot a stand-in is in use for, and which of the stand-in's it is, if any.
const StandFile* stand_file(const char16_t* name, bool& ours) {
    ours = stand.used && in_a_slot(name) && name[0] == char16_t(u'A' + stand.slot);
    if (!ours) return nullptr;
    name += 2;
    for (uint32_t i = 0; i < stand.files.count; ++i) {
        const StandFile& file = stand.files[i];
        size_t same = 0; // unit by unit, so nothing past the end of the game's name is looked at
        while (same < file.units && name[same] == file.name[same]) ++same;
        if (same == file.units && !name[same]) return &file;
    }
    return nullptr;
}

bool on_exists(void* system, const char16_t* name, int32_t user) {
    bool ours = false;
    const StandFile* file = stand_file(name, ours);
    return ours ? file != nullptr : game_exists(system, name, user);
}

int32_t on_exists_result(void* system, const char16_t* name, int32_t user) {
    bool ours = false;
    const StandFile* file = stand_file(name, ours);
    return ours ? int32_t(file ? t::constant::save_there : t::constant::save_missing) : game_exists_result(system, name, user);
}

// The game reads a file into an array of bytes, after whatever the array holds already.
bool on_read(void* system, bool with_screen, const char16_t* name, int32_t user, void* out) {
    bool ours = false;
    const StandFile* file = stand_file(name, ours);
    if (!ours) return game_read(system, with_screen, name, user, out);
    const auto array = reinterpret_cast<uint64_t>(out);
    if (!file || !game::writable(array, t::array::size)) return false;
    const int32_t count = game::read_or<int32_t>(array + t::array::count), room = game::read_or<int32_t>(array + t::array::max);
    auto* bytes = reinterpret_cast<uint8_t*>(game::read_or<uint64_t>(array + t::array::data));
    if (count < 0 || file->size > uint64_t(0x7FFFFFFF - count)) return false;
    const int32_t total = count + int32_t(file->size);
    if (total > room) { // the engine's own memory, as the array's is
        auto* grown = static_cast<uint8_t*>(game::engine_memory(uint64_t(total)));
        if (!grown) return false;
        if (count) memcpy(grown, bytes, size_t(count));
        game::release_engine_memory(bytes);
        bytes = grown;
        game::write(array + t::array::data, reinterpret_cast<uint64_t>(bytes));
        game::write(array + t::array::max, total);
    }
    if (file->size) memcpy(bytes + count, file->data, size_t(file->size));
    game::write(array + t::array::count, total);
    ++counts.stand_in_reads;
    return true;
}

void clear_stand_in() {
    if (stand.used) close_until_title(); // what the game has loaded may be the stand-in's, however this ends
    for (uint32_t i = 0; i < stand.files.count; ++i) release(stand.files[i].name), release(stand.files[i].data);
    release(stand.files.items);
    stand = StandIn{};
}
void end_stand(void*, uint64_t) { clear_stand_in(); }

// ---- A slot's files ----------------------------------------------------------------------------

// Where the file functions find a slot's folder or one of its files, once the game has opened its save.
bool place(Texts& out, uint32_t slot, const char* within) {
    const char letter[2] = {char('A' + slot), 0};
    return out.add("save:/", letter, *within ? "/" : "", within);
}

using Session = game::SaveSession;

// What stands in the way of looking into a slot now.
TSUCHI_Result refusal(uint32_t slot) {
    if (slot >= t::constant::save_slots) return TSUCHI_INVALID;
    if (!game::save_session_known()) return TSUCHI_NOT_READY;
    return game::busy_with_saves_or_scenes() ? TSUCHI_BUSY : TSUCHI_OK;
}

// Opens one of a slot's files, with the save open around it, and hands it to `use` with its size.
template <typename Use>
TSUCHI_Result with_slot_file(uint32_t slot, const char* path, Use&& use) {
    TSUCHI_Result result = game::path_stays_within(path) ? refusal(slot) : TSUCHI_INVALID;
    if (result != TSUCHI_OK) return result;
    Session session;
    if (!session.open) return TSUCHI_IO;
    Texts where;
    if (!place(where, slot, path)) return TSUCHI_NO_MEMORY;
    result = game::with_file(where.data, use);
    where.clear();
    return result;
}

// ---- The service --------------------------------------------------------------------------------

TSUCHI_Result slot(uint32_t* out) {
    const uint64_t instance = game::global<uint64_t>(t::data::game_instance);
    const uint64_t config = instance ? game::read_or<uint64_t>(instance + t::game_instance::config_save) : 0;
    int32_t number = -1;
    if (!config || !game::read(config + t::config_save::slot, number)) return TSUCHI_NOT_READY;
    if (number < 0 || number >= int32_t(t::constant::save_slots)) return TSUCHI_NOT_FOUND;
    *out = uint32_t(number);
    return TSUCHI_OK;
}

TSUCHI_Result hold(TSUCHI_Mod* mod) {
    if (!wrapped) return TSUCHI_NOT_READY;
    return holders.take(mod, 0, 0, HoldNothing{});
}

TSUCHI_Result release(TSUCHI_Mod* mod) { return holders.release(mod, 0, 0); }

TSUCHI_Result files(uint32_t slot, char* buffer, uint64_t capacity, uint64_t* out) {
    const TSUCHI_Result refused = refusal(slot);
    if (refused != TSUCHI_OK) return refused;
    Session session;
    if (!session.open) return TSUCHI_IO;
    Texts found, folders; // the files, and the folders still to be read, each by its path within the slot: "" is the slot itself
    bool memory = folders.add("");
    for (size_t at = 0; memory && at < folders.used; at += strlen(folders.data + at) + 1) {
        Texts where, names, inside; // the folders inside this one are read once it is closed
        memory = place(where, slot, folders.data + at) && game::list_folder(where.data, names, inside) != TSUCHI_NO_MEMORY;
        where.clear();
        for (const char* name = names.first(); memory && name; name = names.after(name)) memory = found.add(folders.data + at, folders.data[at] ? "/" : "", name);
        for (const char* name = inside.first(); memory && name; name = inside.after(name)) memory = folders.add(folders.data + at, folders.data[at] ? "/" : "", name);
        names.clear(), inside.clear();
    }
    folders.clear();
    const TSUCHI_Result result = !memory ? TSUCHI_NO_MEMORY : !found.used ? TSUCHI_NOT_FOUND : found.give(buffer, capacity, out);
    found.clear();
    return result;
}

TSUCHI_Result exists(uint32_t slot, const char* path, bool* out) {
    const TSUCHI_Result result = with_slot_file(slot, path, [](void*, uint64_t) { return TSUCHI_OK; });
    if (result != TSUCHI_OK && result != TSUCHI_NOT_FOUND) return result;
    *out = result == TSUCHI_OK;
    return TSUCHI_OK;
}

TSUCHI_Result file_size(uint32_t slot, const char* path, uint64_t* out) {
    return with_slot_file(slot, path, [&](void*, uint64_t size) {
        *out = size;
        return TSUCHI_OK;
    });
}

TSUCHI_Result read(uint32_t slot, const char* path, uint64_t offset, void* buffer, uint64_t capacity, uint64_t* out) {
    return with_slot_file(slot, path, [&](void* file, uint64_t size) {
        *out = 0;
        if (offset >= size || !capacity) return TSUCHI_OK;
        const uint64_t wanted = size - offset < capacity ? size - offset : capacity;
        uint64_t got = 0;
        if (t::fn::fs_read_file(&got, file, int64_t(offset), buffer, wanted) != 0) return TSUCHI_IO;
        *out = got;
        return TSUCHI_OK;
    });
}

// What one replacement slot may hold. A real slot is about 260 files and 10 MB.
constexpr uint32_t kStandInFiles = 1024;
constexpr uint64_t kStandInBytes = 64ull * 1024 * 1024;

uint64_t stand_in_bytes() {
    uint64_t total = 0;
    for (uint32_t i = 0; i < stand.files.count; ++i) total += stand.files[i].size;
    return total;
}

TSUCHI_Result stand_in_file(TSUCHI_Mod* mod, uint32_t slot, const char* path, const void* data, uint64_t size) {
    size_t units = 0;
    if (slot >= t::constant::save_slots || !game::path_stays_within(path) || !game::utf8_to_units(path, nullptr, 0, units)) return TSUCHI_INVALID;
    if (stand.mod && (stand.mod != mod || stand.slot != slot)) return TSUCHI_BUSY;
    if (size > kStandInBytes || stand_in_bytes() + size > kStandInBytes || stand.files.count >= kStandInFiles) return TSUCHI_CAPACITY;
    auto* name = static_cast<char16_t*>(allocate((units + 1) * 2));
    auto* bytes = static_cast<uint8_t*>(allocate(size ? size : 1));
    const auto fail = [&](TSUCHI_Result result) {
        tsuchi::release(name), tsuchi::release(bytes);
        return result;
    };
    if (!name || !bytes) return fail(TSUCHI_NO_MEMORY);
    game::utf8_to_units(path, name, units, units);
    name[units] = 0;
    if (size) memcpy(bytes, data, size);
    for (uint32_t i = 0; i < stand.files.count; ++i) { // the same path again: what it holds is replaced
        StandFile& file = stand.files[i];
        if (file.units != units || memcmp(file.name, name, units * 2)) continue;
        tsuchi::release(file.data), tsuchi::release(name);
        file.data = bytes, file.size = size;
        return TSUCHI_OK;
    }
    const bool first = !stand.mod;
    if (first) {
        const TSUCHI_Result owned = own(mod, kStandIn, 0, end_stand, nullptr);
        if (owned != TSUCHI_OK) return fail(owned);
    }
    if (!stand.files.add(StandFile{name, units, bytes, size})) {
        if (first) disown(mod, kStandIn, 0);
        return fail(TSUCHI_NO_MEMORY);
    }
    stand.mod = mod, stand.slot = slot;
    return TSUCHI_OK;
}

TSUCHI_Result use_stand_in(TSUCHI_Mod* mod) {
    if (stand.mod != mod) return TSUCHI_NOT_FOUND;
    if (!wrapped || !reads_wrapped) return TSUCHI_NOT_READY;
    stand.used = true;
    return TSUCHI_OK;
}

TSUCHI_Result end_stand_in(TSUCHI_Mod* mod) {
    if (stand.mod != mod) return TSUCHI_NOT_FOUND;
    disown(mod, kStandIn, 0);
    clear_stand_in();
    return TSUCHI_OK;
}

TSUCHI_Result restore(TSUCHI_Object copy) { return restore_save(copy.id); }

const save::Backend service{slot, hold, release, files, exists, file_size, read, restore, stand_in_file, use_stand_in, end_stand_in};

// Wraps entries of the save system's function table, each only if it holds the game's own function. All or none:
// `ids` has what was wrapped so far, and with one refused those are taken off again.
template <class F>
bool wrap(TSUCHI_Mod* self, const t::SlotOf<F>& slot, F function, F* original, uint64_t* ids, uint32_t& made) {
    if (hook_slot(self, slot, function, original, kHookNeedsStock, &ids[made]) == TSUCHI_OK) return ++made, true;
    while (made) hook_remove(self, ids[--made]);
    return false;
}

} // namespace

void saves_start(TSUCHI_Mod* self) {
    save::set_backend(&service);
    holders.start();
    uint64_t ids[3] = {};
    uint32_t made = 0;
    if (!wrapped) wrapped = wrap(self, t::slot::save_write, &on_write, &game_write, ids, made) && wrap(self, t::slot::save_delete, &on_delete, &game_delete, ids, made);
    made = 0;
    if (!reads_wrapped)
        reads_wrapped = wrap(self, t::slot::save_exists, &on_exists, &game_exists, ids, made) &&
                        wrap(self, t::slot::save_exists_result, &on_exists_result, &game_exists_result, ids, made) &&
                        wrap(self, t::slot::save_read, &on_read, &game_read, ids, made);
}

SaveCounts save_counts() { return counts; }

} // namespace tsuchi
