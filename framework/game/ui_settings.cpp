#include "ui_settings.h"
#include "ui_service.h"
#include "ui_screens.h"
#include "input_service.h"
#include "frame.h"
#include "holds.h"
#include "ids.h"
#include <string.h>
#include <float.h>
namespace tsuchi {
void settings_player_prepare(ModRecord& record);
void settings_player_changed(ModRecord& record, const Setting& setting);
void setting_number_text(float value, char out[48]);
namespace {
// A binding names a controller's buttons in the order of input's Buttons.
static_assert(TSUCHI_BUTTONS_A == 1u << 0 && TSUCHI_BUTTONS_LEFT_STICK == 1u << 4 && TSUCHI_BUTTONS_ZL == 1u << 8 &&
              TSUCHI_BUTTONS_PLUS == 1u << 10 && TSUCHI_BUTTONS_DOWN == 1u << (kBindingButtons - 1), "binding names follow Buttons");
enum class Action : uint8_t { toggle, integer, choice, mapped_choice, member, value, less, more, edit, bind };
struct Entry { char control[TSUCHI_ID_SIZE], key[kModIdBytes], value[TSUCHI_ID_SIZE]; Text word; Action action; };
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
// What a value row shows: a number's shortest text, a binding as a player reads it, or the text itself.
bool shown_value(Text& out, const Setting& setting) {
    char written[48];
    const char* value = setting.text.get();
    if (setting.kind == SettingKind::number) setting_number_text(static_cast<float>(setting.number), written), value = written;
    else if (setting.kind == SettingKind::binding) binding_label(setting.text.get(), written), value = written;
    return safe_text(out, value);
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
void synchronize(TSUCHI_Screen screen, Binding* expected, const char* mod) {
    for (uint32_t i = 0; ; ++i) {
        Binding* binding = opened_binding(screen);
        if (binding != expected || i >= binding->entries.count) return;
        const Entry& item = binding->entries[i];
        ModRecord* record = find_mod(mod);
        Setting* setting = record ? find_setting(record->settings, item.key) : nullptr;
        if (!setting) continue;
        char control[TSUCHI_ID_SIZE]; memcpy(control, item.control, sizeof control);
        if (item.action == Action::toggle) ui::table.set_choice(settings_owner, screen, control, setting->integer ? "on" : "off");
        else if (item.action == Action::integer) ui::table.set_integer(settings_owner, screen, control, setting->integer);
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
            Text shown;
            if (shown_value(shown, *setting) && opened_binding(screen) == expected)
                ui::table.set_text(settings_owner, screen, control, shown.get());
            shown.clear();
        }
    }
}

// ---- Choosing a binding's button ----------------------------------------------------------------
//
// One capture at a time. It waits for a button or key to go down that was not down the step before, so the A
// that chose the row counts only once it has been let go and pressed again. Every player's controller has its
// buttons hidden from the game meanwhile, so the press moves nothing in the menu; they are given back once the
// controllers' buttons are all up after the capture, or after kReleaseSteps.
constexpr uint32_t kPlayers = 4, kCaptureSteps = 150, kReleaseSteps = 60, kEscape = 41, kFirstKey = 4;
constexpr TSUCHI_Buttons kAllButtons = (1u << kBindingButtons) - 1;
struct Capture {
    bool active, releasing;
    uint64_t screen;
    char mod[kModIdBytes], key[kModIdBytes], control[TSUCHI_ID_SIZE];
    uint32_t accepts;               // BindingDevice bits
    uint32_t held[kPlayers];        // each player's buttons at the step before
    uint8_t keys[32];               // the keys down at the step before, a bit each
    uint32_t steps;                 // since it began, or since it ended while the buttons are still hidden
    uint32_t hidden;                // the players whose controllers have their buttons hidden, a bit each
} capture{};

bool key_was_down(uint32_t usage) { return (capture.keys[usage / 8] >> (usage % 8)) & 1; }
void note_key(uint32_t usage, bool down) {
    const uint8_t bit = uint8_t(1u << (usage % 8));
    capture.keys[usage / 8] = uint8_t(down ? capture.keys[usage / 8] | bit : capture.keys[usage / 8] & ~bit);
}
// One step's look at the controllers and the keyboard. `chosen` gets the binding's text for a new press it
// accepts; `cancel` is set for a new press of Escape.
void look(char chosen[24], bool& cancel) {
    for (uint32_t player = 0; player < kPlayers; ++player) {
        TSUCHI_PadState state{};
        if (input::table.pad(settings_owner, player, &state) != TSUCHI_OK) continue; // what it held last stays
        const uint32_t held = state.held & kAllButtons, pressed = held & ~capture.held[player];
        capture.held[player] = held;
        if (!pressed || chosen[0] || !(capture.accepts & uint32_t(BindingDevice::pad))) continue;
        uint32_t bit = 0;
        while (!(pressed & (1u << bit))) ++bit;
        binding_text({BindingDevice::pad, bit, 0}, chosen);
    }
    const bool keys = (capture.accepts & uint32_t(BindingDevice::key)) != 0;
    for (uint32_t usage = keys ? kFirstKey : kEscape; usage < (keys ? 256u : kEscape + 1); ++usage) {
        bool down = false;
        if (input::table.key(settings_owner, usage, &down) != TSUCHI_OK) {
            if (usage == kFirstKey || usage == kEscape) return; // no keyboard to read
            continue;
        }
        const bool pressed = down && !key_was_down(usage);
        note_key(usage, down);
        if (!pressed) continue;
        if (usage == kEscape) cancel = true;
        else if (keys && !chosen[0]) binding_text({BindingDevice::key, 0, usage}, chosen);
    }
}
// The row shows the setting's value again, and the footer its own guides.
void show_capture_value() {
    ModRecord* record = find_mod(capture.mod);
    Setting* setting = record ? find_setting(record->settings, capture.key) : nullptr;
    Text shown;
    if (setting && shown_value(shown, *setting)) ui::table.set_text(settings_owner, {capture.screen}, capture.control, shown.get());
    shown.clear();
    ui::table.set_footer(settings_owner, {capture.screen}, "");
}
void give_back_buttons() {
    for (uint32_t player = 0; player < kPlayers; ++player)
        if (capture.hidden & (1u << player)) input::table.release_hidden(settings_owner, player);
    capture = {};
}
// Ends the capture: with the text of the binding chosen, or none when it was cancelled.
void finish_capture(const char* chosen, bool shown) {
    if (chosen) {
        ModRecord* record = find_mod(capture.mod);
        Setting* setting = record ? find_setting(record->settings, capture.key) : nullptr;
        bool changed = false;
        if (setting) {
            const TSUCHI_Result result = set_setting_text(*setting, chosen, changed);
            notify(*record, *setting, result, changed);
        }
    }
    if (shown) show_capture_value();
    capture.active = false;
    capture.steps = 0;
    capture.releasing = capture.hidden != 0;
    if (!capture.releasing) capture = {};
}
void begin_capture(TSUCHI_Screen screen, const char* mod, const Entry& item, const Setting& setting) {
    if (capture.active || capture.releasing) return;
    capture = {};
    capture.active = true;
    capture.screen = screen.id;
    memcpy(capture.mod, mod, sizeof capture.mod);
    memcpy(capture.key, item.key, sizeof capture.key);
    memcpy(capture.control, item.value, sizeof capture.control);
    capture.accepts = binding_accepts(setting);
    // What is down now is passed over until it has been let go.
    char ignored[24] = {}; bool cancel = false;
    const uint32_t accepts = capture.accepts;
    capture.accepts = 0;
    look(ignored, cancel);
    capture.accepts = accepts;
    if (accepts & uint32_t(BindingDevice::key))
        for (uint32_t usage = kFirstKey; usage < 256; ++usage) {
            bool down = false;
            if (input::table.key(settings_owner, usage, &down) != TSUCHI_OK) break;
            note_key(usage, down);
        }
    for (uint32_t player = 0; player < kPlayers; ++player)
        if (input::table.hold_hidden(settings_owner, player, kAllButtons) == TSUCHI_OK) capture.hidden |= 1u << player;
    const bool pad = accepts & uint32_t(BindingDevice::pad), key = accepts & uint32_t(BindingDevice::key);
    ui::table.set_text(settings_owner, screen, capture.control, pad && key ? "Press a button or key..." : pad ? "Press a button..." : "Press a key...");
    ui::table.set_footer(settings_owner, screen, pad && !key ? "Wait 5 seconds to cancel" : "Press Esc or wait 5 seconds to cancel");
}
void capture_step() {
    if (!capture.active && !capture.releasing) return;
    if (!settings_owner || !mod_usable(settings_owner)) { capture = {}; return; } // the hidden buttons went with the framework
    ++capture.steps;
    if (capture.active) {
        bool open = false;
        if (ui::table.open(settings_owner, {capture.screen}, &open) != TSUCHI_OK || !open) return finish_capture(nullptr, false);
        char chosen[24] = {}; bool cancel = false;
        look(chosen, cancel);
        if (cancel) return finish_capture(nullptr, true);
        if (chosen[0]) return finish_capture(chosen, true);
        if (capture.steps >= kCaptureSteps) return finish_capture(nullptr, true);
        return;
    }
    bool up = true;
    for (uint32_t player = 0; player < kPlayers; ++player) {
        TSUCHI_PadState state{};
        if ((capture.hidden & (1u << player)) && input::table.pad(settings_owner, player, &state) == TSUCHI_OK && (state.held & kAllButtons)) up = false;
    }
    if (up || capture.steps >= kReleaseSteps) give_back_buttons();
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
        bool changed = false;
        const TSUCHI_Result result = reset_setting(setting, changed);
        notify(*record, setting, result, changed);
    }
    synchronize(event->screen, binding, mod);
}
// Typed text set as the setting's value, through the checks its kind has. An integer's control is its arrows.
TSUCHI_Result set_typed(Setting& setting, const char* text, bool& changed) {
    if (setting.kind == SettingKind::integer) {
        int64_t value = 0;
        return read_setting_integer(text, value) ? set_setting_integer(setting, value, changed) : TSUCHI_INVALID;
    }
    if (setting.kind == SettingKind::number) {
        double value = 0;
        if (!read_setting_number(text, value) || value < -FLT_MAX || value > FLT_MAX) return TSUCHI_INVALID;
        return set_setting_number(setting, static_cast<float>(value), changed);
    }
    return set_setting_text(setting, text, changed);
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
        result = set_typed(*setting, text, changed);
        if (result == TSUCHI_OK && setting->kind == SettingKind::integer) {
            ui::table.set_integer(settings_owner, {screen}, control, setting->integer);
        } else if (result == TSUCHI_OK) {
            Text shown;
            if (shown_value(shown, *setting)) ui::table.set_text(settings_owner, {screen}, control, shown.get());
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
    if (selected->action == Action::integer) {
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
// What the keyboard opens with for a setting: its value as text, its limit, and whether the number pad will do.
bool edit_start(const Setting& setting, Text& current, uint32_t& maximum, bool& number_pad) {
    char written[48];
    number_pad = false;
    if (setting.kind == SettingKind::integer) {
        const Json* minimum = setting.declared->find("min");
        number_pad = minimum && minimum->integer >= 0; // the pad has no minus sign
        setting_integer_text(setting.integer, written);
        maximum = 20;
        return current.set(written);
    }
    if (setting.kind == SettingKind::number) {
        setting_number_text(static_cast<float>(setting.number), written);
        maximum = 48;
        return current.set(written);
    }
    const Json* longest = setting.declared->find("max_length");
    maximum = longest && longest->integer < UINT32_MAX ? uint32_t(longest->integer) : UINT32_MAX;
    return current.set(setting.text.get());
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
    if (selected->action == Action::less || selected->action == Action::more) {
        double value = setting->number + (selected->action == Action::less ? -1.0 : 1.0);
        const Json* minimum = setting->declared->find("min"), *maximum = setting->declared->find("max");
        if (minimum && value < minimum->number) value = minimum->number;
        if (maximum && value > maximum->number) value = maximum->number;
        if (value < -FLT_MAX) value = -FLT_MAX;
        if (value > FLT_MAX) value = FLT_MAX;
        bool other = false;
        const TSUCHI_Result result = set_setting_number(*setting, static_cast<float>(value), other);
        if (result == TSUCHI_OK) { char text[48]; setting_number_text(static_cast<float>(setting->number), text); ui::table.set_text(settings_owner, event->screen, selected->value, text); }
        notify(*record, *setting, result, other);
    } else if (selected->action == Action::bind && setting->kind == SettingKind::binding) {
        begin_capture(event->screen, binding->mod, *selected, *setting);
    } else if (selected->action == Action::edit && editor && !binding->request) {
        Text title, current; uint32_t maximum = 0; bool number_pad = false;
        if (!title.set(label_of(*setting)) || !edit_start(*setting, current, maximum, number_pad)) { title.clear(); current.clear(); return; }
        const uint64_t request = next_request++;
        binding->request = request;
        memcpy(binding->edit_key, selected->key, sizeof binding->edit_key);
        memcpy(binding->edit_value, selected->value, sizeof binding->edit_value);
        const TSUCHI_Result result = editor(title.get(), current.get(), maximum, number_pad, request, completed);
        title.clear(); current.clear();
        // The provider can complete or stop the framework before returning.
        if (result != TSUCHI_OK) completed(request, result, nullptr);
    }
}
void closed(void*, const TSUCHI_UiClosed* event) {
    if (capture.active && capture.screen == event->screen.id) finish_capture(nullptr, false);
    if (bindings.release(settings_owner, 0, event->screen.id) == TSUCHI_OK)
        ui::table.destroy(settings_owner, event->screen);
}
// A button row that acts on the setting of the row before it: `value` is that row's control.
TSUCHI_Result add_action(Binding& binding, TSUCHI_Screen screen, const Setting& setting, Action action, const char* label, const char* value) {
    Texts named; Text button_label; Entry* item = nullptr;
    const char* lead = action == Action::less ? "Decrease " : action == Action::more ? "Increase " : action == Action::bind ? "Change " : "Edit ";
    TSUCHI_Result result = named.add(lead, label) ? TSUCHI_OK : TSUCHI_NO_MEMORY;
    if (result == TSUCHI_OK) result = entry(binding, setting, action, nullptr, value, item);
    if (result == TSUCHI_OK && !safe_text(button_label, named.first())) result = TSUCHI_NO_MEMORY;
    if (result == TSUCHI_OK) result = ui::table.add_button(settings_owner, screen, item->control, button_label.get());
    named.clear(); button_label.clear();
    return result;
}
} // namespace
void ui_settings_text_provider(UiSettingsTextProvider provider) { editor = provider; }
void ui_settings_start(TSUCHI_Mod* framework) {
    settings_owner = framework; bindings.start();
    static bool stepping = false;
    if (!stepping) stepping = each_frame(capture_step);
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
    if (!record.settings.list.count) return TSUCHI_NOT_FOUND;
    settings_player_prepare(record);
    auto* binding = static_cast<Binding*>(allocate(sizeof(Binding)));
    if (!binding) return TSUCHI_NO_MEMORY;
    *binding = {}; memcpy(binding->mod, record.id, strlen(record.id) + 1);
    Text title; TSUCHI_Screen screen{};
    TSUCHI_Result result = safe_text(title, *record.name.get() ? record.name.get() : record.id) ? ui::table.create(settings_owner, title.get(), &screen) : TSUCHI_NO_MEMORY;
    title.clear();
    for (uint32_t i = 0; result == TSUCHI_OK && i < record.settings.list.count; ++i) {
        const Setting& setting = record.settings.list[i]; Text label; Entry* item = nullptr;
        if (!safe_text(label, label_of(setting))) { result = TSUCHI_NO_MEMORY; break; }
        if (setting.kind == SettingKind::toggle) {
            result = entry(*binding, setting, Action::toggle, nullptr, nullptr, item);
            if (result == TSUCHI_OK) result = ui::table.add_choice(settings_owner, screen, item->control, label.get(), toggles, setting.integer ? "on" : "off");
        } else if (setting.kind == SettingKind::integer) {
            const Json* minimum = setting.declared->find("min"), *maximum = setting.declared->find("max");
            char integer_id[TSUCHI_ID_SIZE] = {};
            result = entry(*binding, setting, Action::integer, nullptr, nullptr, item);
            if (result == TSUCHI_OK) memcpy(integer_id, item->control, sizeof integer_id);
            if (result == TSUCHI_OK) result = ui::table.add_integer(settings_owner, screen, integer_id, label.get(), minimum ? minimum->integer : INT64_MIN, maximum ? maximum->integer : INT64_MAX, 1, setting.integer);
            if (result == TSUCHI_OK && editor) result = add_action(*binding, screen, setting, Action::edit, label.get(), integer_id);
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
            // A number, a text or a binding: its value as text, then the buttons that change it.
            char value_id[TSUCHI_ID_SIZE] = {}; Text shown;
            result = entry(*binding, setting, Action::value, nullptr, nullptr, item);
            if (result == TSUCHI_OK) memcpy(value_id, item->control, sizeof value_id);
            if (result == TSUCHI_OK && !shown_value(shown, setting)) result = TSUCHI_NO_MEMORY;
            if (result == TSUCHI_OK) result = ui::table.add_text(settings_owner, screen, value_id, label.get(), shown.get());
            shown.clear();
            Action actions[3]; uint32_t count = 0;
            if (setting.kind == SettingKind::number) actions[count++] = Action::less, actions[count++] = Action::more;
            if ((setting.kind == SettingKind::number || setting.kind == SettingKind::text) && editor) actions[count++] = Action::edit;
            if (setting.kind == SettingKind::binding) actions[count++] = Action::bind;
            for (uint32_t n = 0; result == TSUCHI_OK && n < count; ++n) result = add_action(*binding, screen, setting, actions[n], label.get(), value_id);
        }
        label.clear();
    }
    if (result == TSUCHI_OK) result = bindings.take(settings_owner, 0, screen.id, binding);
    if (result != TSUCHI_OK) { if (screen.id) ui::table.destroy(settings_owner, screen); free_binding(binding); return result; }
    ui_screen_resettable(screen);
    *out = screen; return TSUCHI_OK;
}
} // namespace tsuchi
