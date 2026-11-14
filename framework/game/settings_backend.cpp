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

TSUCHI_Result controllers(TSUCHI_Mod* mod, const char* key, char* buffer, uint64_t capacity, uint64_t* out) {
    Setting* setting = nullptr;
    const TSUCHI_Result result = readable(mod, key, setting);
    if (result != TSUCHI_OK) return result;
    *out = strlen(setting_controllers(*setting)) + 1;
    if (capacity < *out) return TSUCHI_CAPACITY;
    memcpy(buffer, setting_controllers(*setting), *out);
    return TSUCHI_OK;
}

const Kind kIntegerRange{"settings.integer_range"};
void forget_integer_range(void* context, uint64_t) { static_cast<Setting*>(context)->ranged = false; }
TSUCHI_Result set_integer_range(TSUCHI_Mod* mod, const char* key, int64_t minimum, int64_t maximum) {
    ModRecord* record = nullptr; Setting* setting = nullptr;
    TSUCHI_Result result = changeable(mod, key, record, setting);
    if (result != TSUCHI_OK) return result;
    // Validate before recording ownership, and publish only after that succeeds.
    Setting proposed = *setting; bool changed_range = false;
    result = set_setting_integer_range(proposed, minimum, maximum, changed_range);
    if (result != TSUCHI_OK || !changed_range) return result;
    const uint64_t identity = reinterpret_cast<uint64_t>(setting);
    if (!ownership_update_allowed(mod, kIntegerRange, identity)) return TSUCHI_BUSY;
    result = own(mod, kIntegerRange, identity, forget_integer_range, setting, true);
    if (result != TSUCHI_OK) return result;
    setting->ranged = true; setting->runtime_minimum = minimum; setting->runtime_maximum = maximum;
    ++counts.changes; told.add(record->id, ":", setting->key());
    return TSUCHI_OK;
}

struct RuntimeHold {
    RuntimeHold* next;
    ModRecord* target;
    Setting* setting;
    TSUCHI_Mod* mod;
    uint64_t id;
    Json value;
    char controller[kModIdBytes];
};
RuntimeHold* runtime_holds = nullptr;
uint64_t next_runtime_hold = 1;
const Kind kRuntimeHold{"settings.hold"};
void tell_runtime(ModRecord& target, const Setting& setting) {
    ++counts.changes;
    if (target.state == ModState::running) told.add(target.id, ":", setting.key());
}
RuntimeHold* runtime_hold(uint64_t id) {
    for (auto* held = runtime_holds; held; held = held->next) if (held->id == id) return held;
    return nullptr;
}
void forget_runtime_hold(void* context, uint64_t) {
    auto* held = static_cast<RuntimeHold*>(context);
    RuntimeHold** at = &runtime_holds;
    while (*at && *at != held) at = &(*at)->next;
    if (*at) *at = held->next;
    if (held->setting->held_value == &held->value) {
        held->setting->held_value = nullptr; held->setting->held_controller = nullptr;
        tell_runtime(*held->target, *held->setting);
    }
    release(held);
}
TSUCHI_Result scalar(const Setting& setting, const TSUCHI_Value* value, Json& out) {
    out = {};
    if (!value) return TSUCHI_INVALID;
    if (setting.kind == SettingKind::toggle && value->kind == TSUCHI_VALUE_BOOLEAN) {
        out.kind = Json::Kind::boolean; out.boolean = value->as.boolean;
    } else if (setting.kind == SettingKind::integer && value->kind == TSUCHI_VALUE_INTEGER) {
        out.kind = Json::Kind::number; out.whole = true; out.integer = value->as.integer; out.number = double(out.integer);
    } else if (setting.kind == SettingKind::number &&
               (value->kind == TSUCHI_VALUE_NUMBER || value->kind == TSUCHI_VALUE_INTEGER)) {
        out.kind = Json::Kind::number;
        out.number = value->kind == TSUCHI_VALUE_INTEGER ? double(value->as.integer) : value->as.number;
    } else return TSUCHI_INVALID;
    return setting_value_allowed(setting, out) ? TSUCHI_OK : TSUCHI_INVALID;
}
TSUCHI_Result hold(TSUCHI_Mod* mod, const char* key, const TSUCHI_Value* value, TSUCHI_SettingHold* out) {
    ModRecord* target = nullptr; Setting* setting = nullptr;
    auto result = find(mod, key, target, setting);
    if (result != TSUCHI_OK) return result;
    const ModRecord* source = record_of(mod);
    const Json* required = source && source->manifest ? source->manifest->find("requires") : nullptr;
    if (!source || (target->context != mod && (!required || !required->find(target->id))) ||
        setting->hidden || !setting->overridable) return TSUCHI_DENIED;
    if (setting->override_value || setting->held_value) return TSUCHI_BUSY;
    Json wanted{};
    result = scalar(*setting, value, wanted);
    if (result != TSUCHI_OK) return result;
    auto* held = static_cast<RuntimeHold*>(allocate(sizeof(RuntimeHold)));
    if (!held) return TSUCHI_NO_MEMORY;
    *held = {}; held->target = target; held->setting = setting; held->mod = mod;
    held->value = wanted; held->id = next_runtime_hold++;
    memcpy(held->controller, source->id, strlen(source->id) + 1);
    result = own(mod, kRuntimeHold, held->id, forget_runtime_hold, held, true);
    if (result != TSUCHI_OK) { release(held); return result; }
    held->next = runtime_holds; runtime_holds = held;
    setting->held_value = &held->value; setting->held_controller = held->controller;
    out->id = held->id; tell_runtime(*target, *setting);
    return TSUCHI_OK;
}
TSUCHI_Result update_hold(TSUCHI_Mod* mod, TSUCHI_SettingHold handle, const TSUCHI_Value* value) {
    auto* held = runtime_hold(handle.id);
    if (!held) return TSUCHI_NOT_FOUND;
    if (held->mod != mod) return TSUCHI_DENIED;
    if (!ownership_update_allowed(mod, kRuntimeHold, handle.id) || held->setting->override_value) return TSUCHI_BUSY;
    Json wanted{};
    const auto result = scalar(*held->setting, value, wanted);
    if (result != TSUCHI_OK) return result;
    if (!setting_values_equal(*held->setting, held->value, wanted)) {
        held->value = wanted; tell_runtime(*held->target, *held->setting);
    }
    return TSUCHI_OK;
}
TSUCHI_Result release_hold(TSUCHI_Mod* mod, TSUCHI_SettingHold handle) {
    auto* held = runtime_hold(handle.id);
    if (!held) return TSUCHI_NOT_FOUND;
    if (held->mod != mod) return TSUCHI_DENIED;
    if (!ownership_update_allowed(mod, kRuntimeHold, handle.id)) return TSUCHI_BUSY;
    const auto result = disown(mod, kRuntimeHold, handle.id);
    if (result == TSUCHI_OK) forget_runtime_hold(held, handle.id);
    return result;
}

const settings::Backend service{toggle, integer, number, text, has, set_toggle, set_integer, set_number, set_text, set_has, reset, controllers, set_integer_range, hold, update_hold, release_hold};

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
    refresh_setting_overrides();
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
    set_setting_override_changed([](ModRecord& record, const Setting& setting) { arrived_late(setting, &record); });
    each_frame(step);
}

SettingsCounts settings_counts() { return counts; }

void settings_player_prepare(ModRecord& record) { read_values(record, false); }
void settings_player_changed(ModRecord& record, const Setting& setting) { note(record, setting); write_values(record); }

} // namespace tsuchi
