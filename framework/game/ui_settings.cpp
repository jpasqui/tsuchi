#include "ui_settings.h"
#include "ui_service.h"
#include "ui_screens.h"
#include "holds.h"
#include "ids.h"
#include "frame.h"
#include <string.h>
#include <float.h>
namespace tsuchi {
void settings_player_prepare(ModRecord& record);
void settings_player_changed(ModRecord& record, const Setting& setting);
void setting_number_text(float value, char out[48]);
namespace {
enum class Action : uint8_t { toggle, integer, number, choice, mapped_choice, member, value, edit, controlled };
struct Entry { char control[TSUCHI_ID_SIZE], key[kModIdBytes], value[TSUCHI_ID_SIZE]; Text word; Action action; int64_t minimum, maximum; };
struct Binding { char mod[kModIdBytes]; List<Entry> entries; uint64_t request; char edit_key[kModIdBytes], edit_value[TSUCHI_ID_SIZE]; };
TSUCHI_Mod* settings_owner = nullptr;
UiSettingsTextProvider editor = nullptr;
uint64_t next_request = 1;
void free_binding(Binding* binding) {
    if (!binding) return;
    for (uint32_t i = 0; i < binding->entries.count; ++i) binding->entries[i].word.clear();
    binding->entries.clear(); release(binding);
}
TSUCHI_Result apply(const Hold&, Binding*& kept, Binding* const& wanted, bool fresh) {
    if (!fresh) return TSUCHI_BUSY;
    kept = wanted; return TSUCHI_OK;
}
bool keep(const Hold& hold, Binding*&) {
    bool open = false;
    return ui::table.open(hold.mod, {hold.what}, &open) == TSUCHI_OK;
}
void restore(const Hold&, Binding*& binding, HoldEnd) { free_binding(binding); binding = nullptr; }
Holds<Binding*, Binding*, apply, keep, restore> bindings{"ui.settings", HoldOn::nothing, true};
const char* const toggles = "[{\"id\":\"off\",\"label\":\"Off\"},{\"id\":\"on\",\"label\":\"On\"}]";
void control_id(char* out, uint32_t n) {
    memcpy(out, "setting-", 8); char digits[10]; uint32_t count = 0;
    do { digits[count++] = char('0' + n % 10); n /= 10; } while (n);
    for (uint32_t i = 0; i < count; ++i) out[8 + i] = digits[count - i - 1];
    out[8 + count] = 0;
}
void option_id(char* out, uint32_t n) {
    char control[TSUCHI_ID_SIZE]; control_id(control, n);
    memcpy(out, "option-", 7); memcpy(out + 7, control + 8, strlen(control + 8) + 1);
}
// Stock labels reserve square brackets for markup. Preview fullwidth brackets
// preserve their visible meaning, while the saved setting keeps its original bytes.
bool safe_text(Text& out, const char* text) {
    Text shown;
    if (!shown.reserve(641)) return false;
    char* to = shown.data; size_t bytes = 0;
    const unsigned char* at = reinterpret_cast<const unsigned char*>(text ? text : "");
    while (*at) {
        if (*at == '[' || *at == ']') {
            if (bytes + 3 > 640) break;
            *to++ = char(0xef); *to++ = char(0xbc); *to++ = *at == '[' ? char(0xbb) : char(0xbd);
            ++at; bytes += 3;
        } else {
            const size_t width = *at < 0x80 ? 1 : *at < 0xe0 ? 2 : *at < 0xf0 ? 3 : 4;
            if (bytes + width > 640) break;
            for (size_t i = 0; i < width; ++i) { if (!at[i]) { shown.clear(); return false; } }
            if (*at < 32) *to++ = ' ';
            else { memcpy(to, at, width); to += width; }
            at += width; bytes += width;
        }
    }
    *to = 0; out.clear(); out = shown; return true;
}
const char* label_of(const Setting& setting) {
    const Json* name = setting.declared->find("name");
    return name && name->text && *name->text ? name->text : setting.key();
}
bool option_text(const Json& choices, Text& out, bool mapped) {
    size_t bytes = 3;
    for (const Json* word = choices.first; word; word = word->next) bytes += 1400 + strlen(word->text);
    if (!out.reserve(bytes)) return false;
    char* at = out.data; *at++ = '['; uint32_t index = 0;
    for (const Json* word = choices.first; word; word = word->next, ++index) {
        Text label;
        if (!safe_text(label, word->text)) return false;
        char id[TSUCHI_ID_SIZE]; option_id(id, index);
        if (word != choices.first) *at++ = ',';
        const char* parts[] = {"{\"id\":\"", mapped ? id : word->text, "\",\"label\":\""};
        for (const char* part : parts) { const size_t n = strlen(part); memcpy(at, part, n); at += n; }
        for (const char* part = label.get(); *part; ++part) {
            if (*part == '\\' || *part == '"') *at++ = '\\';
            *at++ = *part;
        }
        *at++ = '"'; *at++ = '}'; label.clear();
    }
    *at++ = ']'; *at = 0; return true;
}
TSUCHI_Result entry(Binding& binding, const Setting& setting, Action action,
                   const char* word, const char* value, Entry*& out) {
    Entry item{}; item.action = action;
    control_id(item.control, binding.entries.count);
    memcpy(item.key, setting.key(), strlen(setting.key()) + 1);
    if (value) memcpy(item.value, value, strlen(value) + 1);
    if (word && !item.word.set(word)) return TSUCHI_NO_MEMORY;
    out = binding.entries.add(item);
    if (!out) { item.word.clear(); return TSUCHI_NO_MEMORY; }
    return TSUCHI_OK;
}
void notify(ModRecord& record, Setting& setting, TSUCHI_Result result, bool changed) {
    if (result == TSUCHI_OK && changed) settings_player_changed(record, setting);
}
Binding* opened_binding(TSUCHI_Screen screen) {
    if (!settings_owner || !mod_usable(settings_owner)) return nullptr;
    auto* slot = bindings.find(settings_owner, 0, screen.id);
    if (!slot) return nullptr;
    bool open = false;
    return ui::table.open(settings_owner, screen, &open) == TSUCHI_OK && open ? *slot : nullptr;
}
bool controlled_text(const Setting& setting, Text& out) {
    Text value; Texts names, text;
    bool ok = setting_display(setting, value);
    const char* at = setting_controllers(setting);
    while (ok && *at) {
        char id[kModIdBytes]{}; uint32_t n = 0;
        while (*at && *at != ',' && n + 1 < sizeof id) id[n++] = *at++;
        const ModRecord* owner = find_mod(id);
        const char* name = owner && *owner->name.get() ? owner->name.get() : id;
        if (names.used) --names.used;
        ok = names.add(names.used ? ", " : "", name);
        if (*at == ',') ++at;
        if (*at == ' ') ++at;
    }
    // Put ownership first so a long text value cannot clip its attribution away.
    ok = ok && text.add(setting_controlled_value(setting) ? "Controlled by " : "", names.used ? names.data : "",
                        setting_controlled_value(setting) ? ": " : "", value.get()) && safe_text(out, text.data);
    value.clear(); names.clear(); text.clear(); return ok;
}
// A controller can fail or restart while its dependency's form is open. Close that
// form when ownership changes so its next opening rebuilds passive/editable rows.
void ownership_changed() {
    for (uint32_t i = 0; i < bindings.count(); ++i) {
        Binding* binding = bindings.kept_at(i);
        const TSUCHI_Screen screen{bindings.hold_at(i).what};
        if (opened_binding(screen) != binding) continue;
        ModRecord* record = find_mod(binding->mod);
        bool changed = !record;
        for (uint32_t n = 0; record && !changed && n < binding->entries.count; ++n) {
            const Entry& entry = binding->entries[n];
            const Setting* setting = find_setting(record->settings, entry.key);
            if (setting && entry.action == Action::integer) {
                int64_t minimum = 0, maximum = 0, value = 0;
                setting_integer_range(*setting, minimum, maximum);
                if (entry.minimum != minimum || entry.maximum != maximum) {
                    setting_integer(*setting, value);
                    ui_screen_integer_range(settings_owner, screen, entry.control, minimum, maximum, value);
                    binding->entries[n].minimum = minimum; binding->entries[n].maximum = maximum;
                }
            }
            changed = !setting || (entry.action == Action::controlled ?
                strcmp(entry.word.get(), setting_controllers(*setting)) != 0 : setting_controlled_value(*setting) != nullptr);
        }
        if (changed) ui::table.release_open(settings_owner, screen);
    }
}
void synchronize(TSUCHI_Screen screen, Binding* expected, const char* mod) {
    for (uint32_t i = 0; ; ++i) {
        Binding* binding = opened_binding(screen);
        if (binding != expected || i >= binding->entries.count) return;
        const Entry& item = binding->entries[i];
        ModRecord* record = find_mod(mod);
        Setting* setting = record ? find_setting(record->settings, item.key) : nullptr;
        if (!setting) continue;
        char control[TSUCHI_ID_SIZE]; memcpy(control, item.control, sizeof control);
        if (item.action == Action::controlled) {
            Text shown;
            if (controlled_text(*setting, shown)) ui::table.set_text(settings_owner, screen, control, shown.get());
            shown.clear();
        } else if (item.action == Action::toggle) ui::table.set_choice(settings_owner, screen, control, setting->integer ? "on" : "off");
        else if (item.action == Action::integer) {
            int64_t value = 0; setting_integer(*setting, value);
            ui::table.set_integer(settings_owner, screen, control, value);
        }
        else if (item.action == Action::number) ui_screen_set_number(settings_owner, screen, control, setting->number);
        else if (item.action == Action::choice) ui::table.set_choice(settings_owner, screen, control, setting->text.get());
        else if (item.action == Action::mapped_choice) {
            uint32_t index = 0;
            for (const Json* word = setting->declared->find("choices")->first; word; word = word->next, ++index) {
                if (strcmp(word->text, setting->text.get())) continue;
                char option[TSUCHI_ID_SIZE]; option_id(option, index);
                ui::table.set_choice(settings_owner, screen, control, option); break;
            }
        } else if (item.action == Action::member) {
            bool has = false;
            if (setting_has(*setting, item.word.get(), has) == TSUCHI_OK)
                ui::table.set_choice(settings_owner, screen, control, has ? "on" : "off");
        } else if (item.action == Action::value) {
            char number[48]; Text shown;
            if (setting->kind == SettingKind::number) setting_number_text(static_cast<float>(setting->number), number);
            if (safe_text(shown, setting->kind == SettingKind::number ? number : setting->text.get()) && opened_binding(screen) == expected)
                ui::table.set_text(settings_owner, screen, control, shown.get());
            shown.clear();
        }
    }
}
void reset_requested(void*, const TSUCHI_UiResetRequested* event) {
    Binding* binding = opened_binding(event->screen);
    if (!binding) return;
    char mod[kModIdBytes]; memcpy(mod, binding->mod, sizeof mod);
    // A delayed text-editor completion must not replace the restored default.
    binding->request = 0;
    for (uint32_t i = 0; ; ++i) {
        if (opened_binding(event->screen) != binding) return;
        ModRecord* record = find_mod(mod);
        if (!record || i >= record->settings.list.count) break;
        Setting& setting = record->settings.list[i];
        if (setting.hidden) continue;
        bool changed = false;
        const TSUCHI_Result result = reset_setting(setting, changed);
        notify(*record, setting, result, changed);
    }
    synchronize(event->screen, binding, mod);
}
void completed(uint64_t request, TSUCHI_Result result, const char* text) {
    if (!on_game_thread()) return;
    for (uint32_t i = 0; i < bindings.count(); ++i) {
        Binding* binding = bindings.kept_at(i);
        if (binding->request != request) continue;
        const uint64_t screen = bindings.hold_at(i).what;
        char mod[kModIdBytes], key[kModIdBytes], control[TSUCHI_ID_SIZE];
        memcpy(mod, binding->mod, sizeof mod); memcpy(key, binding->edit_key, sizeof key);
        memcpy(control, binding->edit_value, sizeof control); binding->request = 0;
        if (result != TSUCHI_OK || !text) return;
        bool open = false;
        if (ui::table.open(settings_owner, {screen}, &open) != TSUCHI_OK || !open) return;
        ModRecord* record = find_mod(mod);
        Setting* setting = record ? find_setting(record->settings, key) : nullptr;
        if (!setting) return;
        bool changed = false;
        result = set_setting_text(*setting, text, changed);
        if (result == TSUCHI_OK) {
            Text shown;
            if (safe_text(shown, setting->text.get())) ui::table.set_text(settings_owner, {screen}, control, shown.get());
            shown.clear();
        }
        notify(*record, *setting, result, changed); return;
    }
}
void changed(void*, const TSUCHI_UiChanged* event) {
    auto* slot = bindings.find(0, event->screen.id);
    if (!slot) return;
    Binding* binding = *slot;
    const Entry* selected = nullptr;
    for (uint32_t i = 0; i < binding->entries.count; ++i)
        if (!strcmp(binding->entries[i].control, event->control)) { selected = &binding->entries[i]; break; }
    if (!selected) return;
    ModRecord* record = find_mod(binding->mod);
    Setting* setting = record ? find_setting(record->settings, selected->key) : nullptr;
    if (!setting) return;
    bool other = false; TSUCHI_Result result = TSUCHI_INVALID;
    if (selected->action == Action::number) {
        double value = 0; result = ui_screen_number(settings_owner, event->screen, event->control, &value);
        if (result == TSUCHI_OK) result = set_setting_number(*setting, static_cast<float>(value), other);
    } else if (selected->action == Action::integer) {
        int64_t value = 0; result = ui::table.integer(settings_owner, event->screen, event->control, &value);
        if (result == TSUCHI_OK) result = set_setting_integer(*setting, value, other);
    } else {
        char value[TSUCHI_ID_SIZE]; uint64_t bytes = 0;
        result = ui::table.choice(settings_owner, event->screen, event->control, value, sizeof value, &bytes);
        if (result == TSUCHI_OK) {
            if (selected->action == Action::toggle) result = set_setting_toggle(*setting, !strcmp(value, "on"), other);
            else if (selected->action == Action::choice) result = set_setting_text(*setting, value, other);
            else if (selected->action == Action::mapped_choice) {
                result = TSUCHI_NOT_FOUND; uint32_t index = 0;
                const Json* choices = setting->declared->find("choices");
                for (const Json* word = choices->first; word; word = word->next, ++index) {
                    char id[TSUCHI_ID_SIZE]; option_id(id, index);
                    if (!strcmp(id, value)) { result = set_setting_text(*setting, word->text, other); break; }
                }
            }
            else if (selected->action == Action::member) result = set_setting_has(*setting, selected->word.get(), !strcmp(value, "on"), other);
            else result = TSUCHI_INVALID;
        }
    }
    notify(*record, *setting, result, other);
}
void activated(void*, const TSUCHI_UiActivated* event) {
    auto* slot = bindings.find(0, event->screen.id);
    if (!slot) return;
    Binding* binding = *slot;
    const Entry* selected = nullptr;
    for (uint32_t i = 0; i < binding->entries.count; ++i)
        if (!strcmp(binding->entries[i].control, event->control)) { selected = &binding->entries[i]; break; }
    if (!selected) return;
    ModRecord* record = find_mod(binding->mod);
    Setting* setting = record ? find_setting(record->settings, selected->key) : nullptr;
    if (!setting) return;
    if (selected->action == Action::edit && editor && !binding->request) {
        Text title, current;
        if (!title.set(label_of(*setting)) || !current.set(setting->text.get())) { title.clear(); current.clear(); return; }
        const Json* longest = setting->declared->find("max_length");
        const uint32_t maximum = longest && longest->integer < UINT32_MAX ? uint32_t(longest->integer) : UINT32_MAX;
        const uint64_t request = next_request++;
        binding->request = request;
        memcpy(binding->edit_key, selected->key, sizeof binding->edit_key);
        memcpy(binding->edit_value, selected->value, sizeof binding->edit_value);
        const TSUCHI_Result result = editor(title.get(), current.get(), maximum, request, completed);
        title.clear(); current.clear();
        // The provider can complete or stop the framework before returning.
        if (result != TSUCHI_OK) completed(request, result, nullptr);
    }
}
void closed(void*, const TSUCHI_UiClosed* event) {
    if (bindings.release(settings_owner, 0, event->screen.id) == TSUCHI_OK)
        ui::table.destroy(settings_owner, event->screen);
}
} // namespace
void ui_settings_text_provider(UiSettingsTextProvider provider) { editor = provider; }
void ui_settings_start(TSUCHI_Mod* framework) {
    settings_owner = framework; bindings.start();
    each_frame(ownership_changed);
    TSUCHI_Subscription subscription{};
    ui::table.on_changed(settings_owner, changed, nullptr, &subscription);
    ui::table.on_activated(settings_owner, activated, nullptr, &subscription);
    ui::table.on_closed(settings_owner, closed, nullptr, &subscription);
    ui::table.on_reset_requested(settings_owner, reset_requested, nullptr, &subscription);
}
TSUCHI_Result ui_mod_settings(ModRecord& record, TSUCHI_Screen* out) {
    if (!out) return TSUCHI_INVALID;
    if (!on_game_thread()) return TSUCHI_INVALID;
    if (!settings_owner || !mod_usable(settings_owner)) return TSUCHI_NOT_READY;
    if (!has_visible_settings(record.settings)) return TSUCHI_NOT_FOUND;
    settings_player_prepare(record);
    auto* binding = static_cast<Binding*>(allocate(sizeof(Binding)));
    if (!binding) return TSUCHI_NO_MEMORY;
    *binding = {}; memcpy(binding->mod, record.id, strlen(record.id) + 1);
    Text title; TSUCHI_Screen screen{};
    TSUCHI_Result result = safe_text(title, *record.name.get() ? record.name.get() : record.id) ? ui::table.create(settings_owner, title.get(), &screen) : TSUCHI_NO_MEMORY;
    title.clear();
    for (uint32_t i = 0; result == TSUCHI_OK && i < record.settings.list.count; ++i) {
        const Setting& setting = record.settings.list[i]; Text label; Entry* item = nullptr;
        if (setting.hidden) continue;
        if (!safe_text(label, label_of(setting))) { result = TSUCHI_NO_MEMORY; break; }
        if (setting_controlled_value(setting)) {
            Text shown;
            result = entry(*binding, setting, Action::controlled, setting_controllers(setting), nullptr, item);
            if (result == TSUCHI_OK && !controlled_text(setting, shown)) result = TSUCHI_NO_MEMORY;
            if (result == TSUCHI_OK) result = ui::table.add_text(settings_owner, screen, item->control, label.get(), shown.get());
            shown.clear();
        } else if (setting.kind == SettingKind::toggle) {
            result = entry(*binding, setting, Action::toggle, nullptr, nullptr, item);
            if (result == TSUCHI_OK) result = ui::table.add_choice(settings_owner, screen, item->control, label.get(), toggles, setting.integer ? "on" : "off");
        } else if (setting.kind == SettingKind::integer) {
            int64_t minimum = 0, maximum = 0, value = 0;
            setting_integer_range(setting, minimum, maximum); setting_integer(setting, value);
            result = entry(*binding, setting, Action::integer, nullptr, nullptr, item);
            if (result == TSUCHI_OK) {
                item->minimum = minimum; item->maximum = maximum;
                result = ui::table.add_integer(settings_owner, screen, item->control, label.get(), minimum, maximum, 1, value);
            }
        } else if (setting.kind == SettingKind::number) {
            const Json* minimum = setting.declared->find("min"), *maximum = setting.declared->find("max"), *step = setting.declared->find("step");
            double increment = step ? step->number : 1;
            if (!step && minimum && maximum && maximum->number > minimum->number &&
                maximum->number - minimum->number <= 1) {
                increment = (maximum->number - minimum->number) / 20;
                if (!(static_cast<float>(increment) > 0)) increment = maximum->number - minimum->number;
            }
            result = entry(*binding, setting, Action::number, nullptr, nullptr, item);
            if (result == TSUCHI_OK) result = ui_screen_add_number(settings_owner, screen, item->control, label.get(),
                minimum ? minimum->number : -FLT_MAX, maximum ? maximum->number : FLT_MAX,
                increment, setting.number, setting_number_text);
        } else if (setting.kind == SettingKind::set) {
            const Json* choices = setting.declared->find("choices");
            for (const Json* word = choices->first; result == TSUCHI_OK && word; word = word->next) {
                Texts named; Text safe; bool has = false;
                if (!named.add(label.get(), ": ", word->text) || !safe_text(safe, named.first())) result = TSUCHI_NO_MEMORY;
                if (result == TSUCHI_OK) result = entry(*binding, setting, Action::member, word->text, nullptr, item);
                if (result == TSUCHI_OK) result = setting_has(setting, word->text, has);
                if (result == TSUCHI_OK) result = ui::table.add_choice(settings_owner, screen, item->control, safe.get(), toggles, has ? "on" : "off");
                named.clear(); safe.clear();
            }
        } else if (setting.kind == SettingKind::choice) {
            const Json* choices = setting.declared->find("choices"); const bool selectable = choices->count >= 2;
            bool mapped = false; uint32_t initial_index = 0, index = 0;
            for (const Json* word = choices->first; word; word = word->next, ++index) {
                mapped = mapped || !id_valid(word->text);
                if (!strcmp(word->text, setting.text.get())) initial_index = index;
            }
            result = entry(*binding, setting, selectable ? mapped ? Action::mapped_choice : Action::choice : Action::value, nullptr, nullptr, item);
            Text options, shown;
            if (result == TSUCHI_OK && selectable) {
                char initial[TSUCHI_ID_SIZE]; option_id(initial, initial_index);
                if (!option_text(*choices, options, mapped)) result = TSUCHI_NO_MEMORY;
                if (result == TSUCHI_OK) result = ui::table.add_choice(settings_owner, screen, item->control, label.get(), options.get(), mapped ? initial : setting.text.get());
            } else if (result == TSUCHI_OK) {
                if (!safe_text(shown, setting.text.get())) result = TSUCHI_NO_MEMORY;
                if (result == TSUCHI_OK) result = ui::table.add_text(settings_owner, screen, item->control, label.get(), shown.get());
            }
            options.clear(); shown.clear();
        } else {
            char value_id[TSUCHI_ID_SIZE], number[48]; Text shown;
            result = entry(*binding, setting, Action::value, nullptr, nullptr, item);
            if (result == TSUCHI_OK) memcpy(value_id, item->control, sizeof value_id);
            if (setting.kind == SettingKind::number) setting_number_text(static_cast<float>(setting.number), number);
            if (result == TSUCHI_OK && !safe_text(shown, setting.kind == SettingKind::number ? number : setting.text.get())) result = TSUCHI_NO_MEMORY;
            if (result == TSUCHI_OK) result = ui::table.add_text(settings_owner, screen, value_id, label.get(), shown.get());
            shown.clear();
            const uint32_t buttons = editor ? 1 : 0;
            for (uint32_t n = 0; result == TSUCHI_OK && n < buttons; ++n) {
                Texts named; Text button_label;
                const Action action = Action::edit;
                if (!named.add("Edit ", label.get())) result = TSUCHI_NO_MEMORY;
                if (result == TSUCHI_OK) result = entry(*binding, setting, action, nullptr, value_id, item);
                if (result == TSUCHI_OK && !safe_text(button_label, named.first())) result = TSUCHI_NO_MEMORY;
                if (result == TSUCHI_OK) result = ui::table.add_button(settings_owner, screen, item->control, button_label.get());
                named.clear(); button_label.clear();
            }
        }
        label.clear();
    }
    if (result == TSUCHI_OK) result = bindings.take(settings_owner, 0, screen.id, binding);
    if (result != TSUCHI_OK) { if (screen.id) ui::table.destroy(settings_owner, screen); free_binding(binding); return result; }
    ui_screen_resettable(screen);
    *out = screen; return TSUCHI_OK;
}
} // namespace tsuchi
