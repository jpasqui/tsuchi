// The game side of the settings service.
//
// The values are a mod's record's own. This finds whose setting a key names,
// tells listeners of changes, and keeps the values in the save: one file to a
// mod, `save:/tsuchi/<id>/settings.json`, beside the mod's saved data. The file is
// read before the mod's code first runs if the save can be reached then, and
// as soon as it can be otherwise; it is written in the frame after a value
// changes, or when the save can next be written.

#include <string.h>

#include "frame.h"
#include "ids.h"
#include "mods.h"
#include "registry.h"
#include "save_session.h"
#include "services.h"
#include "settings.h"
#include "settings_service.h"
#include "text.h"
#include "texts.h"

namespace tsuchi {

namespace {

constexpr uint32_t kFramesBetweenTries = 300; // after the save would not take a write

SettingsCounts counts{};
Texts told;              // the changes still to tell, each as `<mod id>:<key>`
uint32_t wait_frames = 0; // until writing is tried again

ModRecord* record_of(const TSUCHI_Mod* mod) { return find_mod(mod_id(mod)); }

// Whose setting a key names: the calling mod's own, or as `<mod id>:<key>` another's. TSUCHI_NOT_FOUND when there is
// no such mod running or it has no such setting.
TSUCHI_Result find(TSUCHI_Mod* mod, const char* key, ModRecord*& record, Setting*& setting) {
    char id[kModIdBytes];
    const char* own_key = key;
    while (*own_key && *own_key != ':') ++own_key;
    if (*own_key) { // another mod's, spelled as the ID of anything a mod adds is
        if (!id_split(key, id, &key)) return TSUCHI_NOT_FOUND;
        record = find_mod(id);
    } else {
        record = record_of(mod);
    }
    if (!record || record->state != ModState::running) return TSUCHI_NOT_FOUND;
    setting = find_setting(record->settings, key);
    return setting ? TSUCHI_OK : TSUCHI_NOT_FOUND;
}

// For reading: another mod's setting only if it is public.
TSUCHI_Result readable(TSUCHI_Mod* mod, const char* key, Setting*& setting) {
    ModRecord* record = nullptr;
    const TSUCHI_Result found = find(mod, key, record, setting);
    if (found != TSUCHI_OK) return found;
    return record->context == mod || setting->open ? TSUCHI_OK : TSUCHI_DENIED;
}

// For changing: the mod's own only.
TSUCHI_Result changeable(TSUCHI_Mod* mod, const char* key, ModRecord*& record, Setting*& setting) {
    const TSUCHI_Result found = find(mod, key, record, setting);
    if (found != TSUCHI_OK) return found;
    return record->context == mod ? TSUCHI_OK : TSUCHI_DENIED;
}

// A value changed: it is to be told, and written.
void note(ModRecord& record, const Setting& setting) {
    record.settings.unsaved = true;
    ++counts.changes;
    told.add(record.id, ":", setting.key()); // with no memory to tell it, the change still stands
}
TSUCHI_Result changed(TSUCHI_Result result, bool is_other, ModRecord& record, const Setting& setting) {
    if (result == TSUCHI_OK && is_other) note(record, setting);
    return result;
}

TSUCHI_Result toggle(TSUCHI_Mod* mod, const char* key, bool* out) {
    Setting* setting = nullptr;
    const TSUCHI_Result found = readable(mod, key, setting);
    return found == TSUCHI_OK ? setting_toggle(*setting, *out) : found;
}
TSUCHI_Result integer(TSUCHI_Mod* mod, const char* key, int64_t* out) {
    Setting* setting = nullptr;
    const TSUCHI_Result found = readable(mod, key, setting);
    return found == TSUCHI_OK ? setting_integer(*setting, *out) : found;
}
TSUCHI_Result number(TSUCHI_Mod* mod, const char* key, float* out) {
    Setting* setting = nullptr;
    const TSUCHI_Result found = readable(mod, key, setting);
    return found == TSUCHI_OK ? setting_number(*setting, *out) : found;
}
TSUCHI_Result text(TSUCHI_Mod* mod, const char* key, char* buffer, uint64_t capacity, uint64_t* out) {
    Setting* setting = nullptr;
    const char* value = nullptr;
    TSUCHI_Result result = readable(mod, key, setting);
    if (result == TSUCHI_OK) result = setting_text(*setting, value);
    if (result != TSUCHI_OK) return result;
    *out = strlen(value) + 1;
    if (capacity < *out) return TSUCHI_CAPACITY;
    memcpy(buffer, value, *out);
    return TSUCHI_OK;
}
TSUCHI_Result has(TSUCHI_Mod* mod, const char* key, const char* word, bool* out) {
    Setting* setting = nullptr;
    const TSUCHI_Result found = readable(mod, key, setting);
    return found == TSUCHI_OK ? setting_has(*setting, word, *out) : found;
}

TSUCHI_Result set_toggle(TSUCHI_Mod* mod, const char* key, bool value) {
    ModRecord* record = nullptr;
    Setting* setting = nullptr;
    bool is_other = false;
    const TSUCHI_Result found = changeable(mod, key, record, setting);
    if (found != TSUCHI_OK) return found;
    const TSUCHI_Result result = set_setting_toggle(*setting, value, is_other);
    return changed(result, is_other, *record, *setting);
}
TSUCHI_Result set_integer(TSUCHI_Mod* mod, const char* key, int64_t value) {
    ModRecord* record = nullptr;
    Setting* setting = nullptr;
    bool is_other = false;
    const TSUCHI_Result found = changeable(mod, key, record, setting);
    if (found != TSUCHI_OK) return found;
    const TSUCHI_Result result = set_setting_integer(*setting, value, is_other);
    return changed(result, is_other, *record, *setting);
}
TSUCHI_Result set_number(TSUCHI_Mod* mod, const char* key, float value) {
    ModRecord* record = nullptr;
    Setting* setting = nullptr;
    bool is_other = false;
    const TSUCHI_Result found = changeable(mod, key, record, setting);
    if (found != TSUCHI_OK) return found;
    const TSUCHI_Result result = set_setting_number(*setting, value, is_other);
    return changed(result, is_other, *record, *setting);
}
TSUCHI_Result set_text(TSUCHI_Mod* mod, const char* key, const char* value) {
    ModRecord* record = nullptr;
    Setting* setting = nullptr;
    bool is_other = false;
    const TSUCHI_Result found = changeable(mod, key, record, setting);
    if (found != TSUCHI_OK) return found;
    const TSUCHI_Result result = set_setting_text(*setting, value, is_other);
    return changed(result, is_other, *record, *setting);
}
TSUCHI_Result set_has(TSUCHI_Mod* mod, const char* key, const char* word, bool value) {
    ModRecord* record = nullptr;
    Setting* setting = nullptr;
    bool is_other = false;
    const TSUCHI_Result found = changeable(mod, key, record, setting);
    if (found != TSUCHI_OK) return found;
    const TSUCHI_Result result = set_setting_has(*setting, word, value, is_other);
    return changed(result, is_other, *record, *setting);
}
TSUCHI_Result reset(TSUCHI_Mod* mod, const char* key) {
    ModRecord* record = nullptr;
    Setting* setting = nullptr;
    bool is_other = false;
    const TSUCHI_Result found = changeable(mod, key, record, setting);
    if (found != TSUCHI_OK) return found;
    const TSUCHI_Result result = reset_setting(*setting, is_other);
    return changed(result, is_other, *record, *setting);
}

const settings::Backend service{toggle, integer, number, text, has, set_toggle, set_integer, set_number, set_text, set_has, reset};

// ---- Keeping the values in the save ---------------------------------------------------------------

bool place(Texts& out, const ModRecord& record) { return out.add(game::kSavedMods, "/", record.id, "/settings.json"); }

void arrived_late(const Setting& setting, void* user) {
    auto* record = static_cast<ModRecord*>(user);
    ++counts.changes;
    told.add(record->id, ":", setting.key());
}

// Takes in what the save holds for a mod, if the save can be reached now. `late` is false before the mod's code
// has run: what arrives then is simply how the mod finds its settings.
void read_values(ModRecord& record, bool late) {
    Settings& settings = record.settings;
    if (settings.read) return;
    if (!settings.list.count) return void(settings.read = true);
    Texts where;
    if (!place(where, record)) return;
    char* text = nullptr;
    uint64_t bytes = 0;
    const TSUCHI_Result result = read_save_file(where.data, text, bytes);
    where.clear();
    if (result == TSUCHI_NOT_READY || result == TSUCHI_BUSY || result == TSUCHI_NO_MEMORY) return; // tried again in a moment
    settings.read = true; // whatever came of it: a file that is not there or cannot be read leaves the defaults
    if (result != TSUCHI_OK) return void(counts.unread += result != TSUCHI_NOT_FOUND);
    if (take_settings(settings, text, bytes, late ? arrived_late : nullptr, &record)) ++counts.read;
    else ++counts.unread;
    release(text);
}

void write_values(ModRecord& record) {
    Settings& settings = record.settings;
    if (!settings.unsaved || !settings.read || wait_frames) return;
    Texts where;
    Text text;
    if (place(where, record) && settings_text(settings, text)) {
        const TSUCHI_Result result = write_save_file(where.data, text.get(), strlen(text.get()));
        if (result == TSUCHI_OK) settings.unsaved = false, ++counts.written;
        if (result == TSUCHI_IO) wait_frames = kFramesBetweenTries, ++counts.unwritten;
    }
    where.clear(), text.clear();
}

void before_start(ModRecord& record) { read_values(record, false); }

void step() {
    // What changed since the last frame is told first. A handler may change more, which is told next frame.
    Texts telling = told;
    told = Texts{};
    for (const char* change = telling.first(); change; change = telling.after(change)) {
        char id[kModIdBytes];
        const char* key = nullptr;
        if (id_split(change, id, &key)) settings::emit_changed({id, key});
    }
    telling.clear();
    if (wait_frames) --wait_frames;
    for (uint32_t i = 0; i < mod_count(); ++i) {
        ModRecord& record = *mod_at(i);
        if (record.state != ModState::running) continue;
        read_values(record, true);
        write_values(record);
    }
}

} // namespace

void settings_start() {
    settings::set_backend(&service);
    set_before_start(before_start);
    each_frame(step);
}

SettingsCounts settings_counts() { return counts; }

void settings_player_prepare(ModRecord& record) { read_values(record, false); }
void settings_player_changed(ModRecord& record, const Setting& setting) { note(record, setting); write_values(record); }

} // namespace tsuchi
