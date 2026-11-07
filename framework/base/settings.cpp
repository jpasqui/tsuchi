#include "settings.h"

#include <string.h>

#include "registry.h"

namespace tsuchi {

namespace {

using JsonKind = Json::Kind;

const char* const kKinds[] = {"toggle", "integer", "number", "text", "choice", "set"};
const char* const kKnown[] = {"type", "default", "name", "description", "public", "min", "max", "max_length", "choices"};

bool is_text(const Json* value) { return value && value->kind == JsonKind::text && strlen(value->text) == value->bytes; }
bool is_whole(const Json* value) { return value && value->kind == JsonKind::number && value->whole; }

// Why a setting is declared wrongly: `the setting "<key>" <what><more>`.
bool wrong(Text& problem, const char* key, const char* what, const char* more = "") {
    static const char lead[] = "the setting \"";
    const size_t sizes[] = {sizeof lead - 1, strlen(key), 2, strlen(what), strlen(more)};
    const char* const parts[] = {lead, key, "\" ", what, more};
    size_t bytes = 1;
    for (size_t size : sizes) bytes += size;
    if (!problem.reserve(bytes)) return false;
    char* at = problem.data;
    for (int i = 0; i < 5; ++i) {
        memcpy(at, parts[i], sizes[i]);
        at += sizes[i];
    }
    *at = 0;
    return false;
}

const Json* choices_of(const Setting& setting) { return setting.declared->find("choices"); }

// Which of its choices a word is, or -1.
int64_t choice_at(const Setting& setting, const char* word) {
    int64_t index = 0;
    for (const Json* choice = choices_of(setting)->first; choice; choice = choice->next, ++index)
        if (!strcmp(choice->text, word)) return index;
    return -1;
}

bool integer_allowed(const Setting& setting, int64_t value) {
    const Json* least = setting.declared->find("min"), *most = setting.declared->find("max");
    return !(least && value < least->integer) && !(most && value > most->integer);
}
// A number setting is read as a float, so it holds only what a float holds: what rounds to a float short of
// infinity. The largest float is written as 3.4028235e38, which is past it and still rounds to it.
constexpr double kLargestFloat = 3.4028234663852886e38, kFloatRoundsUp = 3.4028235677973366e38;
bool fits_float(double value) { return value > -kFloatRoundsUp && value < kFloatRoundsUp; }
bool number_allowed(const Setting& setting, double value) {
    const Json* least = setting.declared->find("min"), *most = setting.declared->find("max");
    return fits_float(value) && !(least && value < least->number) && !(most && value > most->number);
}
bool text_allowed(const Setting& setting, const char* value) {
    const Json* longest = setting.declared->find("max_length");
    return !longest || strlen(value) <= uint64_t(longest->integer);
}

// The changes themselves. Each tells whether the value is another than before.
bool put_integer(Setting& setting, int64_t value) {
    const bool changed = setting.integer != value;
    setting.integer = value;
    return changed;
}
bool put_number(Setting& setting, double value) {
    const bool changed = setting.number != value;
    setting.number = value;
    return changed;
}
TSUCHI_Result put_text(Setting& setting, const char* value, bool& changed) {
    changed = strcmp(setting.text.get(), value) != 0;
    return setting.text.set(value) ? TSUCHI_OK : TSUCHI_NO_MEMORY;
}
bool put_chosen(Setting& setting, int64_t index, bool value) {
    const bool changed = (setting.chosen[index] != 0) != value;
    setting.chosen[index] = value;
    return changed;
}

// Puts a setting at its default, which its declaration was checked to have.
TSUCHI_Result to_default(Setting& setting, bool& changed) {
    const Json* value = setting.declared->find("default");
    changed = false;
    switch (setting.kind) {
    case SettingKind::toggle: changed = put_integer(setting, value->boolean); break;
    case SettingKind::integer: changed = put_integer(setting, value->integer); break;
    case SettingKind::number: changed = put_number(setting, value->number); break;
    case SettingKind::text:
    case SettingKind::choice: return put_text(setting, value->text, changed);
    case SettingKind::set: {
        int64_t index = 0;
        for (const Json* choice = choices_of(setting)->first; choice; choice = choice->next, ++index) {
            bool in = false;
            for (const Json* word = value->first; word; word = word->next) in = in || !strcmp(word->text, choice->text);
            changed = put_chosen(setting, index, in) || changed;
        }
        break;
    }
    }
    return TSUCHI_OK;
}

// True when the setting is at its default now.
bool at_default(const Setting& setting) {
    const Json* value = setting.declared->find("default");
    switch (setting.kind) {
    case SettingKind::toggle: return setting.integer == int64_t(value->boolean);
    case SettingKind::integer: return setting.integer == value->integer;
    case SettingKind::number: return setting.number == value->number;
    case SettingKind::text:
    case SettingKind::choice: return !strcmp(setting.text.get(), value->text);
    case SettingKind::set: {
        int64_t index = 0;
        for (const Json* choice = choices_of(setting)->first; choice; choice = choice->next, ++index) {
            bool in = false;
            for (const Json* word = value->first; word; word = word->next) in = in || !strcmp(word->text, choice->text);
            if (in != (setting.chosen[index] != 0)) return false;
        }
        return true;
    }
    }
    return true;
}

// One declaration, checked whole. The setting is not yet given its value.
bool declare(Setting& setting, const Json& declared, Text& problem) {
    const char* const key = declared.name;
    // A key is spelled as a mod's ID is, so that `<mod id>:<key>` is an ID like any other.
    if (!valid_mod_id(key)) return wrong(problem, key, "has a key that is not spelled as a mod's ID is");
    if (declared.kind != JsonKind::object) return wrong(problem, key, "is an object, in braces");
    for (const Json* member = declared.first; member; member = member->next) {
        bool known = false;
        for (const char* name : kKnown) known = known || !strcmp(member->name, name);
        if (!known) return wrong(problem, key, "has a key the framework does not know: ", member->name);
    }
    const Json* type = declared.find("type"), *value = declared.find("default"), *least = declared.find("min"), *most = declared.find("max");
    const Json* longest = declared.find("max_length"), *choices = declared.find("choices"), *open = declared.find("public");
    int kind = -1;
    for (int i = 0; i < 6; ++i)
        if (is_text(type) && !strcmp(type->text, kKinds[i])) kind = i;
    if (kind < 0) return wrong(problem, key, "needs a \"type\": toggle, integer, number, text, choice or set");
    setting.declared = &declared;
    setting.kind = SettingKind(kind);
    const char* const shown[] = {"name", "description"};
    for (const char* name : shown)
        if (declared.find(name) && !is_text(declared.find(name))) return wrong(problem, key, "has a name or a description that is not a text");
    if (open && open->kind != JsonKind::boolean) return wrong(problem, key, "has a \"public\" that is not true or false");
    setting.open = open && open->boolean;

    // What only some kinds have.
    const bool ranged = setting.kind == SettingKind::integer || setting.kind == SettingKind::number;
    const bool listed = setting.kind == SettingKind::choice || setting.kind == SettingKind::set;
    if (!ranged && (least || most)) return wrong(problem, key, "has a \"min\" or a \"max\", which only an integer or a number has");
    if (setting.kind != SettingKind::text && longest) return wrong(problem, key, "has a \"max_length\", which only a text has");
    if (!listed && choices) return wrong(problem, key, "has \"choices\", which only a choice or a set has");
    const Json* const bounds[] = {least, most};
    for (const Json* bound : bounds) {
        if (!bound) continue;
        if (bound->kind != JsonKind::number || (setting.kind == SettingKind::integer && !bound->whole) ||
            (setting.kind == SettingKind::number && !fits_float(bound->number)))
            return wrong(problem, key, "has a \"min\" or a \"max\" that is not a number of its kind");
    }
    if (least && most && (setting.kind == SettingKind::integer ? least->integer > most->integer : least->number > most->number))
        return wrong(problem, key, "has a \"min\" above its \"max\"");
    if (longest && (!is_whole(longest) || longest->integer < 0)) return wrong(problem, key, "has a \"max_length\" that is not a whole number of bytes");
    if (listed) {
        if (!choices || choices->kind != JsonKind::list || !choices->count) return wrong(problem, key, "needs \"choices\": a list of words");
        for (const Json* choice = choices->first; choice; choice = choice->next) {
            if (!is_text(choice) || !choice->bytes) return wrong(problem, key, "has a choice that is not a word");
            for (const Json* other = choices->first; other != choice; other = other->next)
                if (!strcmp(other->text, choice->text)) return wrong(problem, key, "has the same choice twice: ", choice->text);
        }
    }

    // The default, of its kind and allowed.
    const char* const no_default = "needs a \"default\" its kind and its limits allow";
    if (!value) return wrong(problem, key, no_default);
    switch (setting.kind) {
    case SettingKind::toggle:
        if (value->kind != JsonKind::boolean) return wrong(problem, key, no_default);
        break;
    case SettingKind::integer:
        if (!is_whole(value) || !integer_allowed(setting, value->integer)) return wrong(problem, key, no_default);
        break;
    case SettingKind::number:
        if (value->kind != JsonKind::number || !number_allowed(setting, value->number)) return wrong(problem, key, no_default);
        break;
    case SettingKind::text:
        if (!is_text(value) || !text_allowed(setting, value->text)) return wrong(problem, key, no_default);
        break;
    case SettingKind::choice:
        if (!is_text(value) || choice_at(setting, value->text) < 0) return wrong(problem, key, no_default);
        break;
    case SettingKind::set:
        if (value->kind != JsonKind::list) return wrong(problem, key, no_default);
        for (const Json* word = value->first; word; word = word->next)
            if (!is_text(word) || choice_at(setting, word->text) < 0) return wrong(problem, key, no_default);
        break;
    }
    return true;
}

// ---- Writing the values as JSON ---------------------------------------------------------------

struct Writer {
    char* data = nullptr;
    size_t used = 0, capacity = 0;
    bool whole = true; // false once memory ran out

    void add(const char* bytes, size_t count) {
        if (!whole) return;
        if (used + count + 1 > capacity) {
            size_t next = capacity ? capacity * 2 : 256;
            while (next < used + count + 1) next *= 2;
            char* grown = static_cast<char*>(allocate(next));
            if (!grown) return void(whole = false);
            if (used) memcpy(grown, data, used);
            release(data);
            data = grown, capacity = next;
        }
        memcpy(data + used, bytes, count);
        used += count;
        data[used] = 0;
    }
    void add(const char* text) { add(text, strlen(text)); }
    // A text in quotes, with what JSON wants written with a backslash written so.
    void quoted(const char* text) {
        add("\"");
        for (const char* at = text; *at; ++at) {
            const uint8_t c = uint8_t(*at);
            if (c == '"' || c == '\\') {
                const char two[] = {'\\', char(c)};
                add(two, 2);
            } else if (c < 0x20) {
                const char six[] = {'\\', 'u', '0', '0', "0123456789abcdef"[c >> 4], "0123456789abcdef"[c & 15]};
                add(six, 6);
            } else {
                add(at, 1);
            }
        }
        add("\"");
    }
};

// A whole number's digits. Returns where they start in `out`, which holds 24 bytes.
const char* whole_text(int64_t value, char* out) {
    char* at = out + 23;
    *at = 0;
    uint64_t left = value < 0 ? ~uint64_t(value) + 1 : uint64_t(value);
    do *--at = char('0' + left % 10);
    while (left /= 10);
    if (value < 0) *--at = '-';
    return at;
}

// A number as `digits` digits and a power of ten, written out: 1.5, 0.004, 12000, 1.2e+30.
void place_digits(char* out, bool negative, const char* digits, int count, int exponent) {
    char* at = out;
    if (negative) *at++ = '-';
    if (exponent >= 0 && exponent < 16) {
        for (int i = 0; i <= exponent; ++i) *at++ = i < count ? digits[i] : '0';
        if (count > exponent + 1) {
            *at++ = '.';
            for (int i = exponent + 1; i < count; ++i) *at++ = digits[i];
        }
    } else if (exponent < 0 && exponent >= -5) {
        *at++ = '0', *at++ = '.';
        for (int i = -1; i > exponent; --i) *at++ = '0';
        for (int i = 0; i < count; ++i) *at++ = digits[i];
    } else {
        *at++ = digits[0];
        if (count > 1) {
            *at++ = '.';
            for (int i = 1; i < count; ++i) *at++ = digits[i];
        }
        *at++ = 'e';
        char power[24];
        const char* text = whole_text(exponent, power);
        memcpy(at, text, strlen(text));
        at += strlen(text);
    }
    *at = 0;
}

// The shortest way to write a number that reads back as the same value a mod is given for it. `out` holds 48 bytes.
void number_text(double value, char* out) {
    // What no setting admits is still written as a number: no number is 0, and a too large one the largest float.
    if (!(value == value)) return void(memcpy(out, "0", 2));
    if (!fits_float(value)) value = value < 0 ? -kLargestFloat : kLargestFloat;
    const float wanted = float(value);
    const bool negative = wanted < 0;
    double scaled = negative ? -double(wanted) : double(wanted);
    if (scaled == 0) return void(memcpy(out, "0", 2));
    int exponent = 0; // of the first digit
    while (scaled >= 10) scaled /= 10, ++exponent;
    while (scaled < 1) scaled *= 10, --exponent;
    for (int count = 1; count <= 17; ++count) {
        double shifted = scaled;
        uint64_t limit = 10;
        for (int i = 1; i < count; ++i) shifted *= 10, limit *= 10;
        uint64_t whole = uint64_t(shifted + 0.5);
        int first = exponent;
        if (whole >= limit) whole /= 10, ++first; // rounding carried into one more digit
        char digits[24];
        for (int i = count; i--; whole /= 10) digits[i] = char('0' + whole % 10);
        place_digits(out, negative, digits, count, first);
        const Json* read = read_json(out, strlen(out));
        const bool same = read && read->kind == JsonKind::number && float(read->number) == wanted;
        release_json(read);
        if (same) return;
    }
}

void write_value(Writer& to, const Setting& setting) {
    char digits[48];
    switch (setting.kind) {
    case SettingKind::toggle: to.add(setting.integer ? "true" : "false"); break;
    case SettingKind::integer: to.add(whole_text(setting.integer, digits)); break;
    case SettingKind::number:
        number_text(setting.number, digits);
        to.add(digits);
        break;
    case SettingKind::text:
    case SettingKind::choice: to.quoted(setting.text.get()); break;
    case SettingKind::set: {
        to.add("[");
        bool first = true;
        int64_t index = 0;
        for (const Json* choice = choices_of(setting)->first; choice; choice = choice->next, ++index) {
            if (!setting.chosen[index]) continue;
            if (!first) to.add(", ");
            first = false;
            to.quoted(choice->text);
        }
        to.add("]");
        break;
    }
    }
}

// A value the save held, taken in if the declaration allows it. Tells whether the setting changed.
bool take(Setting& setting, const Json& value) {
    bool changed = false;
    switch (setting.kind) {
    case SettingKind::toggle: return value.kind == JsonKind::boolean && put_integer(setting, value.boolean);
    case SettingKind::integer: return is_whole(&value) && integer_allowed(setting, value.integer) && put_integer(setting, value.integer);
    case SettingKind::number: return value.kind == JsonKind::number && number_allowed(setting, value.number) && put_number(setting, value.number);
    case SettingKind::text: return is_text(&value) && text_allowed(setting, value.text) && put_text(setting, value.text, changed) == TSUCHI_OK && changed;
    case SettingKind::choice: return is_text(&value) && choice_at(setting, value.text) >= 0 && put_text(setting, value.text, changed) == TSUCHI_OK && changed;
    case SettingKind::set: {
        if (value.kind != JsonKind::list) return false;
        int64_t index = 0;
        for (const Json* choice = choices_of(setting)->first; choice; choice = choice->next, ++index) {
            bool in = false;
            for (const Json* word = value.first; word; word = word->next) in = in || (is_text(word) && !strcmp(word->text, choice->text));
            changed = put_chosen(setting, index, in) || changed;
        }
        return changed;
    }
    }
    return false;
}

} // namespace

bool declare_settings(Settings& out, const Json* declared, Text& problem) {
    out = Settings{};
    if (!declared) return true;
    if (declared->kind != JsonKind::object) return problem.set("\"settings\" is an object, in braces: each setting by its key"), false;
    for (const Json* each = declared->first; each; each = each->next) {
        Setting setting{};
        if (!declare(setting, *each, problem)) return false;
        bool changed = false;
        if (setting.kind == SettingKind::set) setting.chosen = static_cast<uint8_t*>(allocate(each->find("choices")->count));
        const bool made = (setting.kind != SettingKind::set || setting.chosen) && to_default(setting, changed) == TSUCHI_OK && out.list.add(setting);
        if (!made) {
            setting.text.clear();
            release(setting.chosen);
            return problem.set("there is no memory for its settings"), false;
        }
    }
    return true;
}

void forget_settings(Settings& settings) {
    for (uint32_t i = 0; i < settings.list.count; ++i) {
        settings.list[i].text.clear();
        release(settings.list[i].chosen);
    }
    release(settings.list.items);
    settings = Settings{};
}

Setting* find_setting(Settings& settings, const char* key) {
    for (uint32_t i = 0; i < settings.list.count; ++i)
        if (!strcmp(settings.list[i].key(), key)) return &settings.list[i];
    return nullptr;
}

TSUCHI_Result setting_toggle(const Setting& setting, bool& out) {
    if (setting.kind != SettingKind::toggle) return TSUCHI_INVALID;
    out = setting.integer != 0;
    return TSUCHI_OK;
}
TSUCHI_Result setting_integer(const Setting& setting, int64_t& out) {
    if (setting.kind != SettingKind::integer) return TSUCHI_INVALID;
    out = setting.integer;
    return TSUCHI_OK;
}
TSUCHI_Result setting_number(const Setting& setting, float& out) {
    if (setting.kind != SettingKind::number) return TSUCHI_INVALID;
    out = float(setting.number);
    return TSUCHI_OK;
}
TSUCHI_Result setting_text(const Setting& setting, const char*& out) {
    if (setting.kind != SettingKind::text && setting.kind != SettingKind::choice) return TSUCHI_INVALID;
    out = setting.text.get();
    return TSUCHI_OK;
}
TSUCHI_Result setting_has(const Setting& setting, const char* word, bool& out) {
    const int64_t index = setting.kind == SettingKind::set ? choice_at(setting, word) : -1;
    if (index < 0) return TSUCHI_INVALID;
    out = setting.chosen[index] != 0;
    return TSUCHI_OK;
}

TSUCHI_Result set_setting_toggle(Setting& setting, bool value, bool& changed) {
    if (setting.kind != SettingKind::toggle) return TSUCHI_INVALID;
    changed = put_integer(setting, value);
    setting.touched = true;
    return TSUCHI_OK;
}
TSUCHI_Result set_setting_integer(Setting& setting, int64_t value, bool& changed) {
    if (setting.kind != SettingKind::integer || !integer_allowed(setting, value)) return TSUCHI_INVALID;
    changed = put_integer(setting, value);
    setting.touched = true;
    return TSUCHI_OK;
}
TSUCHI_Result set_setting_number(Setting& setting, float value, bool& changed) {
    if (setting.kind != SettingKind::number || !number_allowed(setting, double(value))) return TSUCHI_INVALID;
    changed = put_number(setting, double(value));
    setting.touched = true;
    return TSUCHI_OK;
}
TSUCHI_Result set_setting_text(Setting& setting, const char* value, bool& changed) {
    const bool allowed = setting.kind == SettingKind::text ? text_allowed(setting, value) : setting.kind == SettingKind::choice && choice_at(setting, value) >= 0;
    if (!allowed) return TSUCHI_INVALID;
    const TSUCHI_Result result = put_text(setting, value, changed);
    if (result == TSUCHI_OK) setting.touched = true;
    return result;
}
TSUCHI_Result set_setting_has(Setting& setting, const char* word, bool value, bool& changed) {
    const int64_t index = setting.kind == SettingKind::set ? choice_at(setting, word) : -1;
    if (index < 0) return TSUCHI_INVALID;
    changed = put_chosen(setting, index, value);
    setting.touched = true;
    return TSUCHI_OK;
}
TSUCHI_Result reset_setting(Setting& setting, bool& changed) {
    const TSUCHI_Result result = to_default(setting, changed);
    if (result == TSUCHI_OK) setting.touched = true;
    return result;
}

bool settings_text(const Settings& settings, Text& out) {
    Writer to;
    to.add("{");
    bool first = true;
    for (uint32_t i = 0; i < settings.list.count; ++i) {
        const Setting& setting = const_cast<Settings&>(settings).list[i];
        if (at_default(setting)) continue;
        to.add(first ? "\n  " : ",\n  ");
        first = false;
        to.quoted(setting.key());
        to.add(": ");
        write_value(to, setting);
    }
    to.add(first ? "}\n" : "\n}\n");
    if (!to.whole) return release(to.data), false;
    out.clear();
    out.data = to.data, out.capacity = to.capacity;
    return true;
}

bool take_settings(Settings& settings, const char* text, size_t bytes, void (*changed)(const Setting& setting, void* user), void* user) {
    const Json* held = read_json(text, bytes);
    if (!held || held->kind != JsonKind::object) return release_json(held), false;
    for (const Json* value = held->first; value; value = value->next) {
        Setting* setting = find_setting(settings, value->name);
        if (setting && !setting->touched && take(*setting, *value) && changed) changed(*setting, user);
    }
    release_json(held);
    return true;
}

void setting_number_text(float value, char out[48]) { number_text(value, out); }

} // namespace tsuchi
