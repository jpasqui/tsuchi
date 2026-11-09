// A mod's settings, as its manifest declares them and as they are now.
//
// A manifest declares each setting under "settings": a key, a kind, a default,
// and what the kind needs. This reads the declarations, holds the values,
// checks every change against what was declared, and turns the values into the
// text the save keeps and back. Where that text is kept is not its business.
#pragma once
#include <stddef.h>
#include <stdint.h>

#include "json.h"
#include "list.h"
#include "tsuchi/core.h"
#include "text.h"

namespace tsuchi {

enum class SettingKind : uint8_t { toggle, integer, number, text, choice, set, binding };

struct Setting {
    const Json* declared; // its object in the manifest, whose name there is its key
    SettingKind kind;
    bool open;            // other mods may read it
    bool touched;         // changed by a call since the mod started, so that what the save holds does not undo it
    int64_t integer;      // a toggle's value, 0 or 1, or an integer's
    double number;        // a number's
    Text text;            // a text's, the word a choice is at, or a binding's `pad:<button>` or `key:<usage>`
    uint8_t* chosen;      // a set's: one byte for each of its choices, in their order

    const char* key() const { return declared->name; }
};

struct Settings {
    List<Setting> list;
    bool read;    // what the save holds has been taken in
    bool unsaved; // a value has changed since they were last written
};

// Reads what a manifest declares under "settings" into `out`, each setting at its default. `declared` is null
// when the manifest has none. False when one is declared wrongly or there is no memory; `problem` says which, and
// `out` holds the settings before it, for the caller to forget.
bool declare_settings(Settings& out, const Json* declared, Text& problem);
// Gives back what the values take. The declarations are the manifest's own.
void forget_settings(Settings& settings);
Setting* find_setting(Settings& settings, const char* key);
void setting_number_text(float value, char out[48]);
// An integer's digits, with a minus sign when it is below zero.
void setting_integer_text(int64_t value, char out[24]);
// Typed text read as an integer or a number: digits with an optional sign, and for a number a decimal point and
// an exponent. Spaces around it are passed over. False when the text is not one, or the integer does not fit.
bool read_setting_integer(const char* text, int64_t& out);
bool read_setting_number(const char* text, double& out);

// A binding's value. `pad:<button>`, the button by its name among input's Buttons, which is bit `button` of them,
// or `key:<usage>`, a USB HID usage from 0 to 255.
enum class BindingDevice : uint8_t { pad = 1, key = 2 };
constexpr uint32_t kBindingButtons = 16;
struct BindingValue {
    BindingDevice device;
    uint32_t button; // the bit's number, for a pad
    uint32_t usage;  // for a key
};
// False when the text is not a binding as it is written: one of the names, or the usage in decimal with no leading zero.
bool read_binding(const char* text, BindingValue& out);
// A binding as it is written, into `out`, which holds 24 bytes.
void binding_text(const BindingValue& value, char out[24]);
// A binding as a player reads it: `ZL`, `Keyboard: T`. `out` holds 48 bytes. The text itself when it is no binding.
void binding_label(const char* text, char out[48]);
// The devices a binding setting takes, as BindingDevice bits: its `accepts`, or both.
uint32_t binding_accepts(const Setting& setting);

// Reading a value. TSUCHI_INVALID when the setting is of another kind, or a word is not one of its choices.
TSUCHI_Result setting_toggle(const Setting& setting, bool& out);
TSUCHI_Result setting_integer(const Setting& setting, int64_t& out);
TSUCHI_Result setting_number(const Setting& setting, float& out);
TSUCHI_Result setting_text(const Setting& setting, const char*& out);
TSUCHI_Result setting_has(const Setting& setting, const char* word, bool& out);

// Changing one. TSUCHI_INVALID when the setting is of another kind or its declaration does not allow the value,
// TSUCHI_NO_MEMORY when there is no memory for a text. `changed` tells whether the value is another than before.
TSUCHI_Result set_setting_toggle(Setting& setting, bool value, bool& changed);
TSUCHI_Result set_setting_integer(Setting& setting, int64_t value, bool& changed);
TSUCHI_Result set_setting_number(Setting& setting, float value, bool& changed);
TSUCHI_Result set_setting_text(Setting& setting, const char* value, bool& changed);
TSUCHI_Result set_setting_has(Setting& setting, const char* word, bool value, bool& changed);
// Back to its default.
TSUCHI_Result reset_setting(Setting& setting, bool& changed);

// The values that are not their defaults, as the JSON text the save keeps. False when there is no memory.
bool settings_text(const Settings& settings, Text& out);
// Takes in such a text. A value the declarations do not allow, or for a setting there is none of, is passed over,
// and so is one for a setting a call has changed since the mod started. `changed` is told each setting that got
// another value. False when the text is not JSON, or not an object; nothing is taken in then.
bool take_settings(Settings& settings, const char* text, size_t bytes, void (*changed)(const Setting& setting, void* user), void* user);

} // namespace tsuchi
