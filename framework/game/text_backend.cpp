// The game side of the text service: the engine's table of text by namespace and key.

#include <string.h>

#include "game.h"
#include "list.h"
#include "reflect.h"
#include "registry.h"
#include "services.h"
#include "shared.h"
#include "text.h"
#include "text_service.h"

namespace tsuchi {
namespace {

namespace t = game::table;

const Kind kReplacement{"text.replacement"};

// A mod's say on a text: what it wants shown under a namespace and a key, with what the game had there before any
// mod had a say. Several mods can each have a say on the same text: the say of the mod started last is in force,
// and when that mod takes its say back the next one's is.
struct Replacement {
    uint64_t id;
    TSUCHI_Mod* mod;
    Text name_space, key, before, wanted;
    bool had; // the game had text under the key
};
List<Replacement> replacements;
uint64_t next_replacement = 1;

// A mod's say on a text, or with no mod given, the say in force: that of the mod started last.
Replacement* find(TSUCHI_Mod* mod, const char* name_space, const char* key) {
    Replacement* found = nullptr;
    for (uint32_t i = 0; i < replacements.count; ++i) {
        Replacement& each = replacements[i];
        if (strcmp(each.name_space.get(), name_space) || strcmp(each.key.get(), key)) continue;
        if (mod ? each.mod == mod : !found || mod_rank(each.mod) > mod_rank(found->mod)) found = &each;
    }
    return found;
}

// The engine's keeper of text, or 0 until it has read the game's text.
uint64_t manager() {
    if (!(game::global<uint8_t>(t::data::text_manager_ready) & 1)) return 0;
    const uint64_t at = game::global<uint64_t>(t::data::text_manager);
    return at && game::read_or<uint8_t>(at + t::text_manager::ready) ? at : 0;
}

bool can_ask() {
    return manager() && t::bound(t::fn::text_key_from_string) && t::bound(t::fn::find_text) && t::bound(t::fn::text_display_string) && t::bound(t::fn::register_text) && t::fn::lock_mutex &&
           t::fn::unlock_mutex;
}

// A piece of text as the engine's 16-bit units, in memory of the framework's own, with
// the header the engine reads a string through. The terminator counts in a string's length.
struct Wide {
    char16_t* units = nullptr;
    alignas(8) uint8_t header[t::array::size] = {};

    // TSUCHI_INVALID for text that is not UTF-8, or is empty where `needed` says it may not be.
    TSUCHI_Result set(const char* text, bool needed) {
        size_t count = 0;
        if (!game::utf8_to_units(text, nullptr, 0, count) || (needed && !count)) return TSUCHI_INVALID;
        units = static_cast<char16_t*>(allocate((count + 1) * 2));
        if (!units) return TSUCHI_NO_MEMORY;
        game::utf8_to_units(text, units, count, count);
        units[count] = 0;
        const uint64_t data = reinterpret_cast<uint64_t>(units);
        const int32_t length = int32_t(count + 1);
        memcpy(header + t::array::data, &data, 8), memcpy(header + t::array::count, &length, 4), memcpy(header + t::array::max, &length, 4);
        return TSUCHI_OK;
    }
    ~Wide() { release(units); }
};

// The string every use of a key's text shares, which the engine's table keeps; 0 for a key the game does not have.
uint64_t display_string(const Wide& name_space, const Wide& key) {
    alignas(16) uint8_t space_key[t::text_key::most] = {}, key_key[t::text_key::most] = {}, found[t::text_value::size] = {};
    t::fn::text_key_from_string(space_key, const_cast<uint8_t*>(name_space.header));
    t::fn::text_key_from_string(key_key, const_cast<uint8_t*>(key.header));
    const bool has = t::fn::find_text(space_key, key_key, found, nullptr);
    const auto string = has ? reinterpret_cast<uint64_t>(t::fn::text_display_string(found)) : 0;
    uint64_t counter = 0;
    memcpy(&counter, found + t::text_value::counter, 8);
    game::drop_shared(counter); // the lookup's own hold; the table keeps its own
    return string;
}

// Gives the string new text under the keeper's lock, and the old memory back to the engine.
TSUCHI_Result replace(uint64_t string, const Wide& text) {
    int32_t length = 0;
    memcpy(&length, text.header + t::array::count, 4);
    void* fresh = game::engine_memory(uint64_t(length) * 2);
    if (!fresh) return TSUCHI_NO_MEMORY;
    memcpy(fresh, text.units, size_t(length) * 2);
    void* lock = reinterpret_cast<void*>(manager() + t::text_manager::lock);
    t::fn::lock_mutex(lock);
    const uint64_t old = game::read_or<uint64_t>(string + t::array::data);
    game::write(string + t::array::data, reinterpret_cast<uint64_t>(fresh)), game::write(string + t::array::count, length), game::write(string + t::array::max, length);
    t::fn::unlock_mutex(lock);
    game::release_engine_memory(reinterpret_cast<void*>(old));
    return TSUCHI_OK;
}

TSUCHI_Result get(const char* name_space, const char* key, char* buffer, uint64_t capacity, uint64_t* out) {
    Wide wide_space, wide_key;
    TSUCHI_Result result = wide_space.set(name_space, true);
    if (result == TSUCHI_OK) result = wide_key.set(key, true);
    if (result != TSUCHI_OK) return result;
    if (!can_ask()) return TSUCHI_NOT_READY;
    const uint64_t string = display_string(wide_space, wide_key);
    if (!string) return TSUCHI_NOT_FOUND;
    size_t needed = 0;
    result = game::units_to_utf8(game::read_or<uint64_t>(string + t::array::data), game::read_or<int32_t>(string + t::array::count), buffer, size_t(capacity), &needed);
    *out = needed;
    return result;
}

void forget(Replacement* replacement) {
    replacement->name_space.clear(), replacement->key.clear(), replacement->before.clear(), replacement->wanted.clear();
    replacements.remove(replacement);
}

// Shows a text under a key the game has. Nothing happens when it has none, or cannot be asked.
void show(const char* name_space, const char* key, const char* text) {
    Wide wide_space, wide_key, wide_text;
    if (!can_ask() || wide_space.set(name_space, true) != TSUCHI_OK || wide_key.set(key, true) != TSUCHI_OK || wide_text.set(text, false) != TSUCHI_OK) return;
    if (const uint64_t string = display_string(wide_space, wide_key)) replace(string, wide_text);
}

// Takes a mod's say out, and shows what is in force without it: the say of the mod started last of those left,
// or the game's own text, where the game had any.
void drop(Replacement* replacement) {
    Replacement gone = *replacement; // its texts are kept past the say itself, for the look at what is left
    replacements.remove(replacement);
    if (const Replacement* last = find(nullptr, gone.name_space.get(), gone.key.get())) show(gone.name_space.get(), gone.key.get(), last->wanted.get());
    else if (gone.had) show(gone.name_space.get(), gone.key.get(), gone.before.get());
    gone.name_space.clear(), gone.key.clear(), gone.before.clear(), gone.wanted.clear();
}

// The mod stopped while it had a say on the text.
void undo(void*, uint64_t id) {
    for (uint32_t i = 0; i < replacements.count; ++i) {
        if (replacements[i].id != id) continue;
        drop(&replacements[i]);
        return;
    }
}

TSUCHI_Result set(TSUCHI_Mod* mod, const char* name_space, const char* key, const char* text) {
    Wide wide_space, wide_key, wide_text;
    TSUCHI_Result result = wide_space.set(name_space, true);
    if (result == TSUCHI_OK) result = wide_key.set(key, true);
    if (result == TSUCHI_OK) result = wide_text.set(text, false);
    if (result != TSUCHI_OK) return result;
    if (!can_ask()) return TSUCHI_NOT_READY;
    Replacement* mine = find(mod, name_space, key);
    if (mine && !ownership_update_allowed(mod, kReplacement, mine->id)) return TSUCHI_BUSY;
    const Replacement* const other = find(nullptr, name_space, key);
    const uint64_t string = display_string(wide_space, wide_key);
    const bool fresh = !mine;
    if (fresh) {
        // The first say on a key remembers the game's own text. A later mod's say takes it from the say before it:
        // what the game shows by then is another mod's.
        Replacement replacement{next_replacement, mod, {}, {}, {}, {}, other ? other->had : string != 0};
        bool kept = replacement.name_space.set(name_space) && replacement.key.set(key);
        if (kept && other) {
            kept = replacement.before.set(other->before.get());
        } else if (kept && string) {
            size_t needed = 0;
            const uint64_t data = game::read_or<uint64_t>(string + t::array::data);
            const int32_t units = game::read_or<int32_t>(string + t::array::count);
            game::units_to_utf8(data, units, nullptr, 0, &needed);
            kept = replacement.before.reserve(needed ? needed : 1) && game::units_to_utf8(data, units, replacement.before.data, replacement.before.capacity, &needed) == TSUCHI_OK;
        }
        mine = kept ? replacements.add(replacement) : nullptr;
        if (!mine) {
            replacement.name_space.clear(), replacement.key.clear(), replacement.before.clear();
            return TSUCHI_NO_MEMORY;
        }
        result = own(mod, kReplacement, mine->id, undo, nullptr);
        if (result != TSUCHI_OK) {
            forget(mine);
            return result;
        }
        ++next_replacement;
    }
    if (!mine->wanted.set(text)) {
        if (fresh) {
            disown(mod, kReplacement, mine->id);
            forget(mine);
        }
        return TSUCHI_NO_MEMORY;
    }
    // What is shown is the say in force, which is this one only when no mod started later has one.
    const Replacement* const last = find(nullptr, name_space, key);
    if (string) {
        Wide shown;
        result = last == mine ? replace(string, wide_text) : shown.set(last->wanted.get(), false);
        if (result == TSUCHI_OK && last != mine) result = replace(string, shown);
        if (result != TSUCHI_OK && fresh) {
            disown(mod, kReplacement, mine->id);
            forget(mine);
        }
        return result;
    }
    // A key the game does not have: the text is handed over as a translation of its own.
    namespace p = t::polyglot_text;
    alignas(16) static uint8_t entry[p::most];
    memset(entry, 0, sizeof entry);
    const int32_t bits = int32_t(t::constant::polyglot_max_bits), none = -1;
    memcpy(entry + p::name_space, wide_space.header, t::array::size), memcpy(entry + p::key, wide_key.header, t::array::size);
    memcpy(entry + p::text, wide_text.header, t::array::size);
    memcpy(entry + p::max_bits, &bits, 4), memcpy(entry + p::first_free, &none, 4);
    t::fn::register_text(reinterpret_cast<void*>(manager()), entry, 1);
    return TSUCHI_OK;
}

TSUCHI_Result restore(TSUCHI_Mod* mod, const char* name_space, const char* key) {
    Replacement* mine = find(mod, name_space, key);
    if (!mine) return TSUCHI_NOT_FOUND;
    disown(mod, kReplacement, mine->id);
    drop(mine);
    return TSUCHI_OK;
}

const text::Backend service{get, set, restore};

} // namespace

void text_start() { text::set_backend(&service); }
unsigned text_replacements() { return replacements.count; }

} // namespace tsuchi
