// Checks of reflection through the public service, against a made-up game that
// describes itself with its own layout. Exits with the number of failures.
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <string>

#include "host.h"
#include "objects.h"
#include "reflect.h"
#include "reflect_world.h"
#include "reflection.h"
#include "registry.h"

namespace {

using namespace tsuchi::game;

int failures = 0;
#define CHECK(condition) \
    do { \
        if (!(condition)) { \
            std::printf("%s:%d: %s\n", __FILE__, __LINE__, #condition); \
            ++failures; \
        } \
    } while (0)

const TSUCHI_ReflectionService& api = *tsuchi::host.reflection;
TSUCHI_Mod* mod = nullptr;
TSUCHI_Result last = TSUCHI_OK;


TSUCHI_Object handle(uint64_t object) { return {to_handle(object)}; }
TSUCHI_Value number(double v) {
    TSUCHI_Value value{TSUCHI_VALUE_NUMBER, 0, {}};
    value.as.number = v;
    return value;
}
TSUCHI_Value integer(int64_t v) {
    TSUCHI_Value value{TSUCHI_VALUE_INTEGER, 0, {}};
    value.as.integer = v;
    return value;
}
TSUCHI_Value boolean(bool v) {
    TSUCHI_Value value{TSUCHI_VALUE_BOOLEAN, 0, {}};
    value.as.boolean = v;
    return value;
}
TSUCHI_Value reference(uint64_t object) {
    TSUCHI_Value value{TSUCHI_VALUE_OBJECT, 0, {}};
    value.as.object = handle(object);
    return value;
}
TSUCHI_Value nothing() { return TSUCHI_Value{TSUCHI_VALUE_NONE, 0, {}}; }

// Reads a field of the red creature. `last` gets the result; a failed read leaves kind 99.
TSUCHI_Value get(const char* path, uint64_t object = 0) {
    TSUCHI_Value value{99, 99, {}};
    last = api.get(mod, handle(object ? object : w.red), path, &value);
    return value;
}
TSUCHI_Result set(const char* path, TSUCHI_Value value, uint64_t object = 0) {
    return api.set(mod, handle(object ? object : w.red), path, &value);
}
bool is_number(const TSUCHI_Value& value, double wanted) { return value.kind == TSUCHI_VALUE_NUMBER && value.as.number == wanted; }
bool is_integer(const TSUCHI_Value& value, int64_t wanted) { return value.kind == TSUCHI_VALUE_INTEGER && value.as.integer == wanted; }
bool is_boolean(const TSUCHI_Value& value, bool wanted) { return value.kind == TSUCHI_VALUE_BOOLEAN && value.as.boolean == wanted; }
bool is_object(const TSUCHI_Value& value, uint64_t wanted) { return value.kind == TSUCHI_VALUE_OBJECT && from_handle(value.as.object.id) == wanted; }
bool failed(const TSUCHI_Value& value, TSUCHI_Result with) { return last == with && value.kind == 99 && value.count == 99; }

std::string text(const char* path, uint64_t object = 0) {
    char buffer[64] = "untouched";
    uint64_t needed = 0;
    last = api.get_text(mod, handle(object ? object : w.red), path, buffer, sizeof buffer, &needed);
    return last == TSUCHI_OK && needed == std::string(buffer).size() + 1 ? buffer : "<failed>";
}

void build() {
    build_world();
    tsuchi::reflect_start();
    mod = tsuchi::mod_create("reflection.test");
}

void utf8_units() {
    const char* boundaries = "\xF0\x90\x80\x80\xF4\x8F\xBF\xBF";
    char16_t units[5] = {0x1234, 0x1234, 0x1234, 0x1234, 0x1234};
    size_t count = 99;
    CHECK(utf8_to_units(boundaries, nullptr, 0, count) && count == 4);
    CHECK(utf8_to_units(boundaries, units, 5, count) && count == 4);
    CHECK(units[0] == 0xD800 && units[1] == 0xDC00 && units[2] == 0xDBFF && units[3] == 0xDFFF && units[4] == 0x1234);
    units[0] = units[1] = 0x1234;
    CHECK(utf8_to_units(boundaries, units, 1, count) && count == 4 && units[0] == 0xD800 && units[1] == 0x1234);

    // F8 90 80 80 used to alias U+10000 after masking the invalid lead.
    for (unsigned lead = 0xF5; lead <= 0xFF; ++lead) {
        const char invalid[] = {char(lead), char(0x90), char(0x80), char(0x80), 0};
        units[0] = 0x1234;
        CHECK(!utf8_to_units(invalid, nullptr, 0, count) && count == 0);
        CHECK(!utf8_to_units(invalid, units, 5, count) && count == 0 && units[0] == 0x1234);
    }
    for (const char* invalid : {"\xF0\x90\x80", "\xF0\x80\x80\x80", "\xED\xA0\x80", "\xF4\x90\x80\x80"}) {
        units[0] = 0x1234;
        CHECK(!utf8_to_units(invalid, units, 5, count) && count == 0 && units[0] == 0x1234);
    }
}

void objects() {
    TSUCHI_Object found{}, type{};
    CHECK(api.find_class(mod, "Creature", &found) == TSUCHI_OK && from_handle(found.id) == w.creature);
    CHECK(api.find_class(mod, "Onion", &found) == TSUCHI_NOT_FOUND && from_handle(found.id) == w.creature);
    CHECK(api.find_class(mod, "Red", &found) == TSUCHI_NOT_FOUND); // an object of that name, but not a class
    CHECK(api.class_of(mod, handle(w.red), &type) == TSUCHI_OK && from_handle(type.id) == w.creature);
    CHECK(api.class_of(mod, TSUCHI_Object{12345}, &type) == TSUCHI_NOT_FOUND);

    bool yes = false;
    CHECK(api.is_a(mod, handle(w.red), handle(w.creature), &yes) == TSUCHI_OK && yes);
    CHECK(api.is_a(mod, handle(w.red), handle(w.actor), &yes) == TSUCHI_OK && yes);
    CHECK(api.is_a(mod, handle(w.red), handle(w.component), &yes) == TSUCHI_OK && !yes);
    CHECK(api.is_a(mod, handle(w.red), TSUCHI_Object{12345}, &yes) == TSUCHI_NOT_FOUND);

    char buffer[32] = "untouched";
    uint64_t needed = 0;
    CHECK(api.name(mod, handle(w.creature_default), buffer, sizeof buffer, &needed) == TSUCHI_OK && std::string(buffer) == "Default__Creature" &&
          needed == 18);
    char small[4] = "zzz";
    CHECK(api.name(mod, handle(w.creature_default), small, sizeof small, &needed) == TSUCHI_CAPACITY && needed == 18 && std::string(small) == "zzz");

    // The checks every call shares: a usable mod, and somewhere to put the answer.
    CHECK(api.class_of(nullptr, handle(w.red), &type) == TSUCHI_INVALID && api.class_of(mod, TSUCHI_Object{0}, &type) == TSUCHI_INVALID);
    CHECK(api.class_of(mod, handle(w.red), nullptr) == TSUCHI_INVALID && api.get(mod, handle(w.red), nullptr, nullptr) == TSUCHI_INVALID);
}

void paths() {
    CHECK(is_number(get("Health"), 12.5) && is_number(get("health"), 12.5) && is_number(get("HEALTH"), 12.5)); // names ignore case
    CHECK(is_integer(get("Anger"), 4) && is_integer(get("Lives"), -3)); // its own field, and its parent's
    CHECK(failed(get("Anger", w.body), TSUCHI_NOT_FOUND) && failed(get("Healt"), TSUCHI_NOT_FOUND) && failed(get("Healthy"), TSUCHI_NOT_FOUND));

    // Into a struct, and through a reference to another object.
    CHECK(is_number(get("Location.Y"), 7.0) && is_number(get("Location.X"), 0.0));
    CHECK(is_number(get("Root.Scale"), 2.0));
    CHECK(failed(get("Location.W"), TSUCHI_NOT_FOUND) && failed(get("Root.Health"), TSUCHI_NOT_FOUND));
    CHECK(get("Location").kind == TSUCHI_VALUE_STRUCT && last == TSUCHI_OK);

    // A fixed number of values side by side.
    TSUCHI_Value scores = get("Scores");
    CHECK(scores.kind == TSUCHI_VALUE_ARRAY && scores.count == 3);
    CHECK(is_integer(get("Scores[0]"), 10) && is_integer(get("Scores[2]"), 30) && failed(get("Scores[3]"), TSUCHI_NOT_FOUND));
    CHECK(is_number(get("Corners[1].Y"), 5.0) && failed(get("Corners.Y"), TSUCHI_INVALID) && failed(get("Corners[2].Y"), TSUCHI_NOT_FOUND));

    // An array that can grow.
    TSUCHI_Value points = get("Points");
    CHECK(points.kind == TSUCHI_VALUE_ARRAY && points.count == 2);
    CHECK(is_number(get("Points[1].Z"), 9.0) && get("Points[1]").kind == TSUCHI_VALUE_STRUCT && failed(get("Points[2].Z"), TSUCHI_NOT_FOUND));
    CHECK(text("Tags[0]") == "Big" && text("Tags[1]") == "Night_2" && failed(get("Tags[2]"), TSUCHI_NOT_FOUND));

    // A reference to nothing ends the path there.
    fake::put<uint64_t>(w.red + 0x78, 0);
    CHECK(failed(get("Root.Scale"), TSUCHI_NOT_FOUND) && get("Root").kind == TSUCHI_VALUE_NONE && last == TSUCHI_OK);
    fake::put<uint64_t>(w.red + 0x78, w.red + 8); // and so does one to something that is not an object
    CHECK(failed(get("Root.Scale"), TSUCHI_NOT_FOUND));
    const uint64_t old = fake::engine_object(u"Old", w.component); // or to an object the game has destroyed
    fake::put<float>(old + 0x40, 8.0f);
    fake::put<uint64_t>(w.red + 0x78, old);
    CHECK(is_number(get("Root.Scale"), 8.0));
    fake::destroy(old);
    CHECK(failed(get("Root.Scale"), TSUCHI_NOT_FOUND) && get("Root").kind == TSUCHI_VALUE_NONE && last == TSUCHI_OK);
    fake::put<uint64_t>(w.red + 0x78, w.body);

    // Paths that are not written correctly.
    for (const char* bad : {"", ".", "Health.", ".Health", "Location..X", "Tags[", "Tags[1", "Tags[x]", "Tags[-1]", "Tags[]", "Tags[1]x",
                            "Tags[12345678901]", "Health[0]", "Health.X", "Scores[1][0]", "Scores.X", "Tag.X"})
        CHECK(failed(get(bad), TSUCHI_INVALID));
    // A long name is only a name nothing has; one longer than any name can be is not a name.
    const std::string long_name(200, 'a'), endless(4000, 'a');
    CHECK(failed(get(long_name.c_str()), TSUCHI_NOT_FOUND) && failed(get(endless.c_str()), TSUCHI_INVALID));

    // An object that is gone.
    const uint64_t gone = fake::engine_object(u"Gone", w.creature);
    const TSUCHI_Object gone_handle = handle(gone);
    fake::destroy(gone);
    TSUCHI_Value value{};
    CHECK(api.get(mod, gone_handle, "Health", &value) == TSUCHI_NOT_FOUND && api.set(mod, gone_handle, "Health", &value) == TSUCHI_NOT_FOUND);
}

void values() {
    // Numbers. A whole number is welcome where a decimal is wanted, not the other way round.
    CHECK(set("Health", number(40.25)) == TSUCHI_OK && fake::get<float>(w.red + 0x40) == 40.25f);
    CHECK(set("Health", integer(3)) == TSUCHI_OK && is_number(get("Health"), 3.0));
    CHECK(set("Ratio", number(0.1)) == TSUCHI_OK && is_number(get("Ratio"), 0.1));
    CHECK(set("Lives", number(2.0)) == TSUCHI_INVALID && set("Health", boolean(true)) == TSUCHI_INVALID && is_integer(get("Lives"), -3));
    CHECK(set("Health", number(NAN)) == TSUCHI_INVALID && set("Health", number(INFINITY)) == TSUCHI_INVALID && is_number(get("Health"), 3.0));
    CHECK(set("Location.Z", number(-1.5)) == TSUCHI_OK && fake::get<float>(w.red + 0x88) == -1.5f);
    CHECK(set("Root.Scale", number(4)) == TSUCHI_OK && fake::get<float>(w.body + 0x40) == 4.0f);
    CHECK(set("Scores[1]", integer(77)) == TSUCHI_OK && fake::get<int32_t>(w.red + 0x90) == 77 && fake::get<int32_t>(w.red + 0x8C) == 10);
    CHECK(set("Points[0].X", number(6)) == TSUCHI_OK && is_number(get("Points[0].X"), 6.0));

    // Each whole-number field takes what fits in it and nothing else.
    CHECK(set("Small", integer(-128)) == TSUCHI_OK && is_integer(get("Small"), -128) && set("Small", integer(128)) == TSUCHI_INVALID);
    CHECK(set("Byte", integer(255)) == TSUCHI_OK && is_integer(get("Byte"), 255) && set("Byte", integer(-1)) == TSUCHI_INVALID &&
          set("Byte", integer(256)) == TSUCHI_INVALID && is_integer(get("Byte"), 255));
    CHECK(set("Short", integer(-32768)) == TSUCHI_OK && is_integer(get("Short"), -32768) && set("Short", integer(32768)) == TSUCHI_INVALID);
    CHECK(set("UShort", integer(65535)) == TSUCHI_OK && is_integer(get("UShort"), 65535) && set("UShort", integer(65536)) == TSUCHI_INVALID);
    CHECK(set("Lives", integer(INT32_MIN)) == TSUCHI_OK && is_integer(get("Lives"), INT32_MIN) && set("Lives", integer(int64_t(INT32_MAX) + 1)) == TSUCHI_INVALID);
    CHECK(set("Count", integer(UINT32_MAX)) == TSUCHI_OK && is_integer(get("Count"), UINT32_MAX) && set("Count", integer(-1)) == TSUCHI_INVALID);
    CHECK(set("Big", integer(INT64_MIN)) == TSUCHI_OK && is_integer(get("Big"), INT64_MIN));
    CHECK(set("Total", integer(-1)) == TSUCHI_OK && fake::get<uint64_t>(w.red + 0xD8) == UINT64_MAX);
    CHECK(is_integer(get("Mode"), 2) && set("Mode", integer(5)) == TSUCHI_OK && fake::get<uint8_t>(w.red + 0xB8) == 5 &&
          set("Mode", integer(256)) == TSUCHI_INVALID);

    // Yes-or-no values that share a byte.
    CHECK(is_boolean(get("bHidden"), false) && is_boolean(get("bCanBeDamaged"), true) && is_boolean(get("bFar"), false));
    CHECK(set("bHidden", boolean(true)) == TSUCHI_OK && fake::get<uint8_t>(w.red + 0x4A) == 0x05);
    CHECK(set("bCanBeDamaged", boolean(false)) == TSUCHI_OK && fake::get<uint8_t>(w.red + 0x4A) == 0x01);
    CHECK(set("bFar", boolean(true)) == TSUCHI_OK && fake::get<uint8_t>(w.red + 0x4B) == 0x80 && fake::get<uint8_t>(w.red + 0x4A) == 0x01);
    CHECK(set("bHidden", integer(0)) == TSUCHI_INVALID && is_boolean(get("bHidden"), true));

    // References to objects: the right class, or nothing.
    CHECK(is_object(get("Root"), w.body));
    CHECK(set("Root", reference(w.spare)) == TSUCHI_OK && fake::get<uint64_t>(w.red + 0x78) == w.spare);
    CHECK(set("Root", reference(w.red)) == TSUCHI_INVALID && fake::get<uint64_t>(w.red + 0x78) == w.spare); // not a component
    CHECK(set("Root", nothing()) == TSUCHI_OK && fake::get<uint64_t>(w.red + 0x78) == 0 && get("Root").kind == TSUCHI_VALUE_NONE);
    TSUCHI_Value stale = reference(w.spare);
    stale.as.object.id ^= 1;
    TSUCHI_Value empty = reference(w.spare);
    empty.as.object.id = 0; // a reference to nothing is given as nothing
    CHECK(set("Root", stale) == TSUCHI_NOT_FOUND && set("Root", empty) == TSUCHI_INVALID);
    CHECK(set("Root", reference(w.body)) == TSUCHI_OK && set("Health", nothing()) == TSUCHI_INVALID);
    // A reference the engine keeps weakly is the same to a mod: read, written and followed. One to an object that has
    // gone reads as nothing.
    const uint64_t target = w.red + 0x180;
    CHECK(get("Target").kind == TSUCHI_VALUE_NONE && last == TSUCHI_OK && failed(get("Target.Scale"), TSUCHI_NOT_FOUND));
    CHECK(set("Target", reference(w.spare)) == TSUCHI_OK && fake::get<uint64_t>(target) == to_weak_value(w.spare) && is_object(get("Target"), w.spare));
    fake::put(w.spare + 0x40, 3.0f);
    CHECK(is_number(get("Target.Scale"), 3.0) && set("Target.Scale", number(5)) == TSUCHI_OK && fake::get<float>(w.spare + 0x40) == 5.0f);
    CHECK(set("Target", reference(w.red)) == TSUCHI_INVALID && is_object(get("Target"), w.spare)); // not a component
    CHECK(set("Target", nothing()) == TSUCHI_OK && fake::get<uint64_t>(target) == to_weak_value(0) && get("Target").kind == TSUCHI_VALUE_NONE);
    const uint64_t passing = fake::engine_object(u"Passing", w.component);
    CHECK(set("Target", reference(passing)) == TSUCHI_OK && is_object(get("Target"), passing));
    fake::destroy(passing);
    CHECK(get("Target").kind == TSUCHI_VALUE_NONE && last == TSUCHI_OK && failed(get("Target.Scale"), TSUCHI_NOT_FOUND));

    // Kinds that are only read, kinds nobody knows, and fields of a kind not handled yet.
    for (uint32_t kind : {uint32_t(TSUCHI_VALUE_TEXT), uint32_t(TSUCHI_VALUE_STRUCT), uint32_t(TSUCHI_VALUE_ARRAY), 8u, 0xFFFFFFFFu})
        CHECK(set("Health", TSUCHI_Value{kind, 0, {}}) == TSUCHI_INVALID);
    CHECK(failed(get("Lookup"), TSUCHI_UNSUPPORTED) && set("Lookup", integer(1)) == TSUCHI_UNSUPPORTED);
    CHECK(set("Location", integer(1)) == TSUCHI_UNSUPPORTED && set("Scores", integer(1)) == TSUCHI_INVALID && set("Tag", integer(1)) == TSUCHI_UNSUPPORTED);

    // Memory the game has made read-only.
    fake::protect(w.body, false);
    CHECK(set("Root.Scale", number(1)) == TSUCHI_NOT_READY && is_number(get("Root.Scale"), 4.0));
    fake::protect(w.body, true);
}

void texts() {
    const TSUCHI_Value tag = get("Tag"), label = get("Label");
    CHECK(tag.kind == TSUCHI_VALUE_TEXT && tag.count == 7 && text("Tag") == "Leader");
    CHECK(label.kind == TSUCHI_VALUE_TEXT && label.count == 14 && text("Label") == "Olimar \xC3\xA9\xF0\x9F\x8C\xB1");
    CHECK(text("Health") == "<failed>" && last == TSUCHI_INVALID && text("Nope") == "<failed>" && last == TSUCHI_NOT_FOUND);

    // Too small a buffer gets nothing, and learns the size.
    char small[6] = "zzzzz";
    uint64_t needed = 0;
    CHECK(api.get_text(mod, handle(w.red), "Label", small, sizeof small, &needed) == TSUCHI_CAPACITY && needed == 14 && std::string(small) == "zzzzz");
    CHECK(api.get_text(mod, handle(w.red), "Tag", small, sizeof small, &needed) == TSUCHI_CAPACITY && needed == 7 && std::string(small) == "zzzzz");
    char exact[7];
    CHECK(api.get_text(mod, handle(w.red), "Tag", exact, sizeof exact, &needed) == TSUCHI_OK && std::string(exact) == "Leader");

    // A string the game never filled in is empty.
    fake::put<uint64_t>(w.red + 0x68 + fake::t::array::data, 0);
    fake::put<int32_t>(w.red + 0x68 + fake::t::array::count, 0);
    CHECK(text("Label").empty() && last == TSUCHI_OK && get("Label").count == 1);
    // One that points nowhere is refused.
    fake::put<uint64_t>(w.red + 0x68 + fake::t::array::data, 0x10);
    fake::put<int32_t>(w.red + 0x68 + fake::t::array::count, 5);
    CHECK(text("Label") == "<failed>" && last == TSUCHI_NOT_FOUND && failed(get("Label"), TSUCHI_NOT_FOUND));
}

void calls() {
    fake::Engine& engine = fake::engine();
    TSUCHI_PreparedCall call{};
    TSUCHI_Value value{};
    CHECK(api.begin_call(mod, handle(w.red), "Fly", &call) == TSUCHI_NOT_FOUND && call.id == 0 && engine.allocated == 0);
    CHECK(api.begin_call(mod, handle(w.body), "Heal", &call) == TSUCHI_NOT_FOUND); // a component has no such function

    // A function of the object's parent class, with arguments and a result.
    fake::put<float>(w.red + 0x40, 10.0f);
    CHECK(api.begin_call(mod, handle(w.red), "Heal", &call) == TSUCHI_OK && call.id != 0);
    CHECK(engine.allocated == 1 && engine.initialized == 1 && tsuchi::owned_count(mod) == 1);
    CHECK(api.call_get(mod, call, "Amount", &value) == TSUCHI_OK && is_number(value, 0.0)); // arguments start at zero
    value = number(2.5);
    CHECK(api.call_set(mod, call, "Amount", &value) == TSUCHI_OK && api.call_set(mod, call, "Speed", &value) == TSUCHI_NOT_FOUND);
    CHECK(api.call_set(mod, call, "bFull", &value) == TSUCHI_INVALID && api.call_set(mod, call, "Amount.", &value) == TSUCHI_INVALID);
    CHECK(engine.invoked == 0 && api.invoke(mod, call) == TSUCHI_OK && engine.invoked == 1 && engine.last_object == w.red);
    CHECK(api.call_get(mod, call, "ReturnValue", &value) == TSUCHI_OK && is_number(value, 12.5) && fake::get<float>(w.red + 0x40) == 12.5f);
    // Again, with one argument changed.
    value = boolean(true);
    CHECK(api.call_set(mod, call, "bFull", &value) == TSUCHI_OK && api.invoke(mod, call) == TSUCHI_OK);
    CHECK(api.call_get(mod, call, "returnvalue", &value) == TSUCHI_OK && is_number(value, 100.0));
    CHECK(api.call_get(mod, call, "Result", &value) == TSUCHI_NOT_FOUND);

    // Another mod cannot touch it.
    TSUCHI_Mod* other = tsuchi::mod_create("reflection.other");
    CHECK(api.invoke(other, call) == TSUCHI_NOT_FOUND && api.call_set(other, call, "Amount", &value) == TSUCHI_NOT_FOUND &&
          api.call_get(other, call, "Amount", &value) == TSUCHI_NOT_FOUND && api.end_call(other, call) == TSUCHI_NOT_FOUND);
    CHECK(engine.invoked == 2 && engine.destroyed == 0);

    // Ending it frees it, once.
    CHECK(api.end_call(mod, call) == TSUCHI_OK && engine.destroyed == 1 && engine.freed == 1 && tsuchi::owned_count(mod) == 0);
    CHECK(api.end_call(mod, call) == TSUCHI_NOT_FOUND && api.invoke(mod, call) == TSUCHI_NOT_FOUND && engine.freed == 1);

    // Results that are text, a struct, and an object.
    TSUCHI_PreparedCall describe{}, where{};
    char buffer[32];
    uint64_t needed = 0;
    CHECK(api.begin_call(mod, handle(w.red), "Describe", &describe) == TSUCHI_OK && api.invoke(mod, describe) == TSUCHI_OK);
    CHECK(api.call_get(mod, describe, "ReturnValue", &value) == TSUCHI_OK && value.kind == TSUCHI_VALUE_TEXT && value.count == 12);
    CHECK(api.call_get_text(mod, describe, "ReturnValue", buffer, sizeof buffer, &needed) == TSUCHI_OK && std::string(buffer) == "Red Bulborb" && needed == 12);
    CHECK(api.call_get_text(mod, describe, "ReturnValue", buffer, 4, &needed) == TSUCHI_CAPACITY && needed == 12);
    CHECK(api.begin_call(mod, handle(w.red), "Where", &where) == TSUCHI_OK && api.invoke(mod, where) == TSUCHI_OK);
    CHECK(api.call_get(mod, where, "ReturnValue", &value) == TSUCHI_OK && value.kind == TSUCHI_VALUE_STRUCT);
    CHECK(api.call_get(mod, where, "ReturnValue.Z", &value) == TSUCHI_OK && is_number(value, 3.0));
    CHECK(api.call_get(mod, where, "Found", &value) == TSUCHI_OK && is_object(value, w.red));
    CHECK(api.call_get(mod, where, "Found.Anger", &value) == TSUCHI_OK && is_integer(value, 4)); // and on into that object

    // A function with no arguments still gets memory of its own.
    TSUCHI_PreparedCall jump{};
    CHECK(api.begin_call(mod, handle(w.red), "Jump", &jump) == TSUCHI_OK && api.invoke(mod, jump) == TSUCHI_OK && fake::get<int32_t>(w.red + 0xF8) == 5);
    CHECK(api.end_call(mod, describe) == TSUCHI_OK && api.invoke(mod, jump) == TSUCHI_OK && fake::get<int32_t>(w.red + 0xF8) == 6);
    CHECK(api.end_call(mod, where) == TSUCHI_OK && api.end_call(mod, jump) == TSUCHI_OK && engine.freed == 4 && engine.allocated == 4);

    // A function of a class runs on the class's default object, whichever way it is asked for.
    TSUCHI_PreparedCall spawn{};
    CHECK(api.begin_call(mod, handle(w.creature), "Spawn", &spawn) == TSUCHI_OK);
    value = integer(21);
    CHECK(api.call_set(mod, spawn, "Count", &value) == TSUCHI_OK && api.invoke(mod, spawn) == TSUCHI_OK && engine.last_object == w.creature_default);
    CHECK(api.call_get(mod, spawn, "ReturnValue", &value) == TSUCHI_OK && is_integer(value, 42) && api.end_call(mod, spawn) == TSUCHI_OK);
    CHECK(api.begin_call(mod, handle(w.red), "Spawn", &spawn) == TSUCHI_OK && api.invoke(mod, spawn) == TSUCHI_OK && engine.last_object == w.creature_default);
    CHECK(api.end_call(mod, spawn) == TSUCHI_OK);
    // The class of actors has no default object here.
    TSUCHI_PreparedCall census{};
    const int invoked = engine.invoked;
    CHECK(api.begin_call(mod, handle(w.actor), "Census", &census) == TSUCHI_OK && api.invoke(mod, census) == TSUCHI_NOT_READY && engine.invoked == invoked);
    CHECK(api.end_call(mod, census) == TSUCHI_OK);
    // An object function cannot be begun on the class itself, and is refused on an object of another class.
    CHECK(api.begin_call(mod, handle(w.creature), "Heal", &call) == TSUCHI_OK && api.invoke(mod, call) == TSUCHI_INVALID && engine.invoked == invoked);
    CHECK(api.end_call(mod, call) == TSUCHI_OK);
    uint64_t memory = 0;
    CHECK(make_arguments(w.heal, memory) == TSUCHI_OK && tsuchi::game::call(w.body, w.heal, memory) == TSUCHI_INVALID && engine.invoked == invoked);
    destroy_arguments(w.heal, memory);
    CHECK(make_arguments(w.red, memory) == TSUCHI_INVALID && memory == 0); // not a function
}

TSUCHI_Result set_text(const char* path, const char* value) { return api.set_text(mod, handle(w.red), path, value); }
TSUCHI_Result resize(const char* path, uint32_t count) { return api.resize(mod, handle(w.red), path, count); }
std::u16string stored_string(uint64_t at) {
    std::u16string out;
    const uint64_t data = fake::get<uint64_t>(at + fake::t::array::data);
    for (int32_t i = 0; i + 1 < fake::get<int32_t>(at + fake::t::array::count); ++i) out.push_back(char16_t(fake::get<uint16_t>(data + 2 * uint64_t(i))));
    return out;
}

void written_text() {
    fake::Engine& engine = fake::engine();
    const uint64_t label = w.red + 0x68;
    fake::put<uint64_t>(label + fake::t::array::data, 0); // the earlier checks left it pointing nowhere
    fake::put<int32_t>(label + fake::t::array::count, 0);

    // A name.
    CHECK(set_text("Tag", "Captain") == TSUCHI_OK && text("Tag") == "Captain");
    CHECK(set_text("Tags[1]", "Dusk") == TSUCHI_OK && text("Tags[1]") == "Dusk" && text("Tags[0]") == "Big");
    CHECK(set_text("Tag", "") == TSUCHI_OK && fake::get<uint64_t>(w.red + 0x60) == 0); // the name of nothing
    CHECK(set_text("Tag", "exactly twenty chars") == TSUCHI_OK && text("Tag") == "exactly twenty chars");
    CHECK(set_text("Tag", "one more than it holds") == TSUCHI_CAPACITY && text("Tag") == "exactly twenty chars");

    // A string: new memory from the engine, the old memory back to it.
    int allocated = engine.allocated, freed = engine.freed;
    CHECK(set_text("Label", "Olimar \xC3\xA9\xF0\x9F\x8C\xB1") == TSUCHI_OK && stored_string(label) == u"Olimar \u00E9\U0001F331");
    CHECK(fake::get<int32_t>(label + fake::t::array::count) == 11 && fake::get<int32_t>(label + fake::t::array::max) == 11);
    CHECK(engine.allocated == allocated + 1 && engine.freed == freed && text("Label") == "Olimar \xC3\xA9\xF0\x9F\x8C\xB1");
    CHECK(set_text("Label", "Louie") == TSUCHI_OK && stored_string(label) == u"Louie" && engine.allocated == allocated + 2 && engine.freed == freed + 1);
    CHECK(set_text("Label", "") == TSUCHI_OK && fake::get<uint64_t>(label + fake::t::array::data) == 0 &&
          fake::get<int32_t>(label + fake::t::array::count) == 0 && engine.freed == freed + 2 && text("Label").empty());
    CHECK(set_text("Label", "Louie") == TSUCHI_OK);

    // Text that is not UTF-8 changes nothing.
    for (const char* bad : {"\xC3", "ab\xFF", "\xC0\x80", "\xED\xA0\x80", "\xF4\x90\x80\x80", "\xE2\x82"})
        CHECK(set_text("Label", bad) == TSUCHI_INVALID && set_text("Tag", bad) == TSUCHI_INVALID);
    CHECK(stored_string(label) == u"Louie" && text("Tag") == "exactly twenty chars");

    // A reference to an asset by its path reads and writes as the path. Written, it forgets
    // the object found for it before and any path inside the asset.
    {
        namespace s = fake::t::soft_object;
        namespace a = fake::t::array;
        const uint64_t icon = w.red + 0x120, kind = w.red + 0x150;
        CHECK(set_text("Icon", "/G/Idol.Idol") == TSUCHI_OK && text("Icon") == "/G/Idol.Idol" && get("Icon").kind == TSUCHI_VALUE_TEXT && get("Icon").count == 13);
        CHECK(fake::get<int32_t>(icon + s::index) == -1 && fake::get<int32_t>(icon + s::serial) == 0);
        fake::put<int32_t>(icon + s::index, 7), fake::put<int32_t>(icon + s::serial, 9);
        fake::put(icon + s::sub_path + a::data, tsuchi::game::engine_memory(8)), fake::put<int32_t>(icon + s::sub_path + a::count, 4), fake::put<int32_t>(icon + s::sub_path + a::max, 4);
        freed = engine.freed;
        CHECK(set_text("Icon", "/G/B.B") == TSUCHI_OK && text("Icon") == "/G/B.B" && fake::get<int32_t>(icon + s::index) == -1 && fake::get<int32_t>(icon + s::serial) == 0 &&
              fake::get<uint64_t>(icon + s::sub_path + a::data) == 0 && fake::get<int32_t>(icon + s::sub_path + a::count) == 0 &&
              fake::get<int32_t>(icon + s::sub_path + a::max) == 0 && engine.freed == freed + 1);
        CHECK(set_text("Icon", "\xC3") == TSUCHI_INVALID && set_text("Icon", "one more than it holds") == TSUCHI_CAPACITY && text("Icon") == "/G/B.B");
        CHECK(set_text("Icon", "") == TSUCHI_OK && fake::get<uint64_t>(icon + s::path) == 0);
        // Given an object, it takes the object's path, if the object is of the class the field takes.
        CHECK(set("Icon", reference(w.body)) == TSUCHI_OK && text("Icon") == "Body" && set("Icon", reference(w.red)) == TSUCHI_INVALID && text("Icon") == "Body");
        CHECK(set("Icon", integer(3)) == TSUCHI_INVALID && set("Icon", nothing()) == TSUCHI_OK && fake::get<uint64_t>(icon + s::path) == 0);
        const uint64_t passing = fake::engine_object(u"Passing", w.component);
        const TSUCHI_Value gone = reference(passing);
        fake::destroy(passing);
        CHECK(set("Icon", gone) == TSUCHI_NOT_FOUND && fake::get<uint64_t>(icon + s::path) == 0);
        // A reference to a class takes a class made from the one the field names.
        CHECK(set("Kind", reference(w.creature)) == TSUCHI_OK && text("Kind") == "Creature" && fake::get<int32_t>(kind + s::index) == -1);
        CHECK(set("Kind", reference(w.component)) == TSUCHI_INVALID && set("Kind", reference(w.red)) == TSUCHI_INVALID && text("Kind") == "Creature");
        CHECK(set("Kind", reference(w.actor)) == TSUCHI_OK && text("Kind") == "Actor");
    }

    // Fields that do not hold text, and trouble with memory.
    CHECK(set_text("Health", "1") == TSUCHI_INVALID && set_text("Scores", "1") == TSUCHI_INVALID && set_text("Location", "1") == TSUCHI_INVALID);
    CHECK(set_text("Nope", "1") == TSUCHI_NOT_FOUND && set_text("Tags[", "1") == TSUCHI_INVALID && api.set_text(mod, handle(w.red), "Tag", nullptr) == TSUCHI_INVALID);
    engine.out_of_memory = true;
    CHECK(set_text("Label", "Brittany") == TSUCHI_NO_MEMORY && stored_string(label) == u"Louie");
    engine.out_of_memory = false;
    fake::put<uint64_t>(fake::main_image() + fake::t::data::allocator, 0);
    CHECK(set_text("Label", "Brittany") == TSUCHI_NOT_READY && stored_string(label) == u"Louie");
    CHECK(!tsuchi::game::engine_memory(64)); // the engine has no allocator yet
    fake::put<uint64_t>(fake::main_image() + fake::t::data::allocator, engine.allocator);
    // The engine's memory for the framework's own use, and giving it back.
    {
        const int allocated = engine.allocated, freed = engine.freed;
        void* block = tsuchi::game::engine_memory(64);
        CHECK(block && reinterpret_cast<uint64_t>(block) == engine.last_block && engine.allocated == allocated + 1);
        tsuchi::game::release_engine_memory(block);
        CHECK(engine.freed == freed + 1);
        tsuchi::game::release_engine_memory(nullptr);
        CHECK(engine.freed == freed + 1);
        engine.out_of_memory = true;
        CHECK(!tsuchi::game::engine_memory(64));
        engine.out_of_memory = false;
    }
    fake::protect(w.red, false);
    CHECK(set_text("Label", "Brittany") == TSUCHI_NOT_READY && set_text("Tag", "Chief") == TSUCHI_NOT_READY);
    fake::protect(w.red, true);
    CHECK(stored_string(label) == u"Louie" && text("Tag") == "exactly twenty chars");
}

void resized() {
    fake::Engine& engine = fake::engine();
    const uint64_t tags = w.red + 0x98, points = w.red + 0xA8, notes = w.red + 0x100;
    const auto count_of = [](uint64_t array) { return fake::get<int32_t>(array + fake::t::array::count); };
    const auto data_of = [](uint64_t array) { return fake::get<uint64_t>(array + fake::t::array::data); };

    // Longer: what was there stays, what is new is zero, and the old memory goes back.
    int allocated = engine.allocated, freed = engine.freed;
    CHECK(resize("Tags", 4) == TSUCHI_OK && count_of(tags) == 4 && fake::get<int32_t>(tags + fake::t::array::max) == 4);
    CHECK(engine.allocated == allocated + 1 && engine.freed == freed + 1);
    CHECK(text("Tags[0]") == "Big" && text("Tags[1]") == "Dusk" && fake::get<uint64_t>(data_of(tags) + 16) == 0 && fake::get<uint64_t>(data_of(tags) + 24) == 0);
    CHECK(set_text("Tags[3]", "Last") == TSUCHI_OK && failed(get("Tags[4]"), TSUCHI_NOT_FOUND));
    // The same length is no work at all.
    CHECK(resize("Tags", 4) == TSUCHI_OK && engine.allocated == allocated + 1);
    // A list of weak references is made longer like any other, and its new places refer to nothing.
    CHECK(resize("Watchers", 2) == TSUCHI_OK && get("Watchers[0]").kind == TSUCHI_VALUE_NONE && set("Watchers[1]", reference(w.body)) == TSUCHI_OK &&
          is_object(get("Watchers[1]"), w.body) && get("Watchers[0]").kind == TSUCHI_VALUE_NONE && resize("Watchers", 0) == TSUCHI_OK);
    allocated += 1, freed += 1;
    // Shorter, and down to nothing.
    CHECK(resize("Tags", 1) == TSUCHI_OK && count_of(tags) == 1 && text("Tags[0]") == "Big" && failed(get("Tags[1]"), TSUCHI_NOT_FOUND));
    CHECK(resize("Tags", 0) == TSUCHI_OK && count_of(tags) == 0 && data_of(tags) == 0 && get("Tags").count == 0);
    CHECK(engine.allocated == allocated + 2 && engine.freed == freed + 3);
    CHECK(resize("Tags", 2) == TSUCHI_OK && set_text("Tags[0]", "Big") == TSUCHI_OK && set_text("Tags[1]", "Night") == TSUCHI_OK);

    // Structs are made and destroyed by their own type.
    int made = engine.made, unmade = engine.unmade;
    CHECK(resize("Points", 5) == TSUCHI_OK && engine.made == made + 3 && engine.unmade == unmade);
    CHECK(is_number(get("Points[1].Z"), 9.0) && is_number(get("Points[4].Z"), 0.0) && get("Points").count == 5);
    CHECK(resize("Points", 1) == TSUCHI_OK && engine.unmade == unmade + 4 && engine.made == made + 3 && failed(get("Points[1].Z"), TSUCHI_NOT_FOUND));
    CHECK(resize("Points", 2) == TSUCHI_OK && set("Points[1].Z", number(9)) == TSUCHI_OK);

    // A string that is cut off gives its memory back.
    CHECK(resize("Notes", 3) == TSUCHI_OK && set_text("Notes[0]", "one") == TSUCHI_OK && set_text("Notes[1]", "two") == TSUCHI_OK &&
          set_text("Notes[2]", "three") == TSUCHI_OK && text("Notes[2]") == "three");
    freed = engine.freed;
    CHECK(resize("Notes", 1) == TSUCHI_OK && engine.freed == freed + 3 && text("Notes[0]") == "one" && count_of(notes) == 1); // two strings and the old block
    CHECK(resize("Notes", 2) == TSUCHI_OK && text("Notes[1]").empty());

    // A list of references to assets by path: a new place refers to nothing yet, and one that is cut off gives
    // back the memory of the path inside its asset.
    {
        namespace s = fake::t::soft_object;
        const uint64_t pictures = w.red + 0x1A0;
        CHECK(resize("Pictures", 2) == TSUCHI_OK && count_of(pictures) == 2 && fake::get<int32_t>(data_of(pictures) + s::index) == -1 &&
              fake::get<int32_t>(data_of(pictures) + 0x30 + s::index) == -1 && fake::get<uint64_t>(data_of(pictures) + 0x30 + s::path) == 0);
        CHECK(set_text("Pictures[0]", "/G/A.A") == TSUCHI_OK && set_text("Pictures[1]", "/G/B.B") == TSUCHI_OK && text("Pictures[1]") == "/G/B.B" && text("Pictures[0]") == "/G/A.A");
        const uint64_t second = data_of(pictures) + 0x30 + s::sub_path;
        fake::put(second + fake::t::array::data, tsuchi::game::engine_memory(8)), fake::put<int32_t>(second + fake::t::array::count, 4), fake::put<int32_t>(second + fake::t::array::max, 4);
        freed = engine.freed;
        CHECK(resize("Pictures", 1) == TSUCHI_OK && engine.freed == freed + 2 && text("Pictures[0]") == "/G/A.A" && failed(get("Pictures[1]"), TSUCHI_NOT_FOUND)); // the path inside, and the old block
        CHECK(resize("Pictures", 0) == TSUCHI_OK && engine.freed == freed + 3 && data_of(pictures) == 0);
    }

    // What cannot be resized, and why.
    CHECK(resize("Odd", 1) == TSUCHI_UNSUPPORTED && resize("Health", 1) == TSUCHI_INVALID && resize("Scores", 2) == TSUCHI_INVALID);
    CHECK(resize("Nope", 1) == TSUCHI_NOT_FOUND && resize("Tags[", 1) == TSUCHI_INVALID);
    CHECK(resize("Tags", 1001) == TSUCHI_CAPACITY && count_of(tags) == 2);
    engine.out_of_memory = true;
    made = engine.made;
    CHECK(resize("Points", 6) == TSUCHI_NO_MEMORY && count_of(points) == 2 && engine.made == made && is_number(get("Points[1].Z"), 9.0));
    engine.out_of_memory = false;
    fake::protect(w.red, false);
    CHECK(resize("Tags", 3) == TSUCHI_NOT_READY && count_of(tags) == 2);
    fake::protect(w.red, true);
}

std::string field(const char* path, uint32_t index, uint64_t object = 0) {
    char buffer[64] = "untouched";
    uint64_t needed = 0;
    last = api.field_at(mod, handle(object ? object : w.red), path, index, buffer, sizeof buffer, &needed);
    return last == TSUCHI_OK && needed == std::string(buffer).size() + 1 ? buffer : "<failed>";
}
std::string call_text(TSUCHI_Result (*read)(TSUCHI_Mod*, TSUCHI_PreparedCall, uint32_t, char*, uint64_t, uint64_t*), TSUCHI_PreparedCall call, uint32_t index) {
    char buffer[64] = "untouched";
    uint64_t needed = 0;
    last = read(mod, call, index, buffer, sizeof buffer, &needed);
    return last == TSUCHI_OK ? buffer : "<failed>";
}

void listed() {
    // An object's fields: its class's own, then its parent's.
    CHECK(field("", 0) == "Anger" && field("", 1) == "Notes" && field("", 2) == "Odd" && field("", 3) == "Health" && field("", 4) == "Lives");
    uint32_t count = 0;
    while (field("", count) != "<failed>") ++count;
    CHECK(count == 31 && last == TSUCHI_NOT_FOUND);
    CHECK(field("", 0, w.body) == "Scale" && field("", 1, w.body) == "<failed>");
    // A struct's fields.
    CHECK(field("Location", 0) == "X" && field("Location", 2) == "Z" && field("Location", 3) == "<failed>" && last == TSUCHI_NOT_FOUND);
    CHECK(field("Points[1]", 1) == "Y" && field("Corners[0]", 0) == "X");
    CHECK(field("Health", 0) == "<failed>" && last == TSUCHI_INVALID && field("Root", 0) == "<failed>" && last == TSUCHI_INVALID);
    CHECK(field("Corners", 0) == "<failed>" && last == TSUCHI_INVALID && field("Nope", 0) == "<failed>" && last == TSUCHI_NOT_FOUND);
    char small[2] = "z";
    uint64_t needed = 0;
    CHECK(api.field_at(mod, handle(w.red), "", 0, small, sizeof small, &needed) == TSUCHI_CAPACITY && needed == 6 && std::string(small) == "z");

    // A function's arguments and results, and which of them it gives back. A function that
    // replaces its parent's has the parent's list behind its own, which is not listed again.
    fake::set_super(w.where, w.heal);
    TSUCHI_PreparedCall heal{}, where{}, rename{};
    CHECK(api.begin_call(mod, handle(w.red), "Heal", &heal) == TSUCHI_OK && api.begin_call(mod, handle(w.red), "Where", &where) == TSUCHI_OK &&
          api.begin_call(mod, handle(w.red), "Rename", &rename) == TSUCHI_OK);
    const auto all = [](TSUCHI_PreparedCall call, uint32_t index) {
        char buffer[64];
        uint64_t size = 0;
        last = api.call_field_at(mod, call, "", index, buffer, sizeof buffer, &size);
        return last == TSUCHI_OK ? std::string(buffer) : std::string("<failed>");
    };
    CHECK(all(heal, 0) == "Amount" && all(heal, 1) == "bFull" && all(heal, 2) == "ReturnValue" && all(heal, 3) == "<failed>" && last == TSUCHI_NOT_FOUND);
    char buffer[64];
    CHECK(api.call_field_at(mod, where, "ReturnValue", 1, buffer, sizeof buffer, &needed) == TSUCHI_OK && std::string(buffer) == "Y");
    CHECK(api.call_field_at(mod, where, "Found", 0, buffer, sizeof buffer, &needed) == TSUCHI_INVALID);
    CHECK(all(where, 0) == "ReturnValue" && all(where, 1) == "Found" && all(where, 2) == "<failed>");
    // A struct among them lists its parent's fields after its own, as a struct anywhere does.
    TSUCHI_PreparedCall measure{};
    TSUCHI_Value part{};
    CHECK(api.begin_call(mod, handle(w.red), "Measure", &measure) == TSUCHI_OK && api.invoke(mod, measure) == TSUCHI_OK);
    CHECK(api.call_field_at(mod, measure, "ReturnValue", 0, buffer, sizeof buffer, &needed) == TSUCHI_OK && std::string(buffer) == "Width");
    CHECK(api.call_field_at(mod, measure, "ReturnValue", 1, buffer, sizeof buffer, &needed) == TSUCHI_OK && std::string(buffer) == "Height");
    CHECK(api.call_field_at(mod, measure, "ReturnValue", 2, buffer, sizeof buffer, &needed) == TSUCHI_NOT_FOUND);
    CHECK(api.call_get(mod, measure, "ReturnValue.Height", &part) == TSUCHI_OK && is_number(part, 2.0) && api.end_call(mod, measure) == TSUCHI_OK);
    CHECK(call_text(api.call_output_at, heal, 0) == "ReturnValue" && call_text(api.call_output_at, heal, 1) == "<failed>" && last == TSUCHI_NOT_FOUND);
    CHECK(call_text(api.call_output_at, where, 0) == "ReturnValue" && call_text(api.call_output_at, where, 1) == "Found" &&
          call_text(api.call_output_at, where, 2) == "<failed>");
    CHECK(call_text(api.call_output_at, rename, 0) == "<failed>"); // an argument it only reads is not one it gives back
    TSUCHI_Mod* other = tsuchi::mod_create("reflection.lister");
    CHECK(api.call_field_at(other, heal, "", 0, buffer, sizeof buffer, &needed) == TSUCHI_NOT_FOUND &&
          api.call_output_at(other, heal, 0, buffer, sizeof buffer, &needed) == TSUCHI_NOT_FOUND);

    // Text and arrays as arguments.
    CHECK(api.call_set_text(mod, rename, "NewName", "Chief") == TSUCHI_OK && api.call_set_text(mod, rename, "Title", "Captain \xC3\xA9") == TSUCHI_OK);
    CHECK(api.call_set_text(mod, rename, "Nope", "x") == TSUCHI_NOT_FOUND && api.call_set_text(other, rename, "Title", "x") == TSUCHI_NOT_FOUND);
    CHECK(api.invoke(mod, rename) == TSUCHI_OK && text("Tag") == "Chief" && w.heard == u"Captain \u00E9");
    TSUCHI_PreparedCall sum{};
    TSUCHI_Value value{};
    CHECK(api.begin_call(mod, handle(w.red), "Sum", &sum) == TSUCHI_OK && api.call_resize(mod, sum, "Values", 3) == TSUCHI_OK);
    for (int i = 0; i < 3; ++i) {
        const std::string path = "Values[" + std::to_string(i) + "]";
        value = integer(10 * (i + 1));
        CHECK(api.call_set(mod, sum, path.c_str(), &value) == TSUCHI_OK);
    }
    CHECK(api.invoke(mod, sum) == TSUCHI_OK && api.call_get(mod, sum, "ReturnValue", &value) == TSUCHI_OK && is_integer(value, 60));
    CHECK(api.call_resize(mod, sum, "ReturnValue", 1) == TSUCHI_INVALID && api.call_resize(other, sum, "Values", 1) == TSUCHI_NOT_FOUND);
    for (TSUCHI_PreparedCall call : {heal, where, rename, sum}) CHECK(api.end_call(mod, call) == TSUCHI_OK);
    CHECK(tsuchi::owned_count(mod) == 0);
}

void lifetimes() {
    fake::Engine& engine = fake::engine();
    TSUCHI_Value value{};

    // Many calls at once, ended in another order.
    TSUCHI_PreparedCall many[40];
    const int allocated = engine.allocated, freed = engine.freed;
    for (int i = 0; i < 40; ++i) {
        CHECK(api.begin_call(mod, handle(w.red), "Spawn", &many[i]) == TSUCHI_OK);
        value = integer(i);
        CHECK(api.call_set(mod, many[i], "Count", &value) == TSUCHI_OK);
    }
    for (int i = 0; i < 40; i += 2) CHECK(api.end_call(mod, many[i]) == TSUCHI_OK);
    for (int i = 1; i < 40; i += 2)
        CHECK(api.invoke(mod, many[i]) == TSUCHI_OK && api.call_get(mod, many[i], "ReturnValue", &value) == TSUCHI_OK && is_integer(value, 2 * i));
    for (int i = 1; i < 40; i += 2) CHECK(api.end_call(mod, many[i]) == TSUCHI_OK);
    CHECK(engine.allocated == allocated + 40 && engine.freed == freed + 40 && tsuchi::owned_count(mod) == 0);

    // The object goes away while a call on it is open.
    const uint64_t blue = fake::engine_object(u"Blue", w.creature);
    TSUCHI_PreparedCall call{};
    CHECK(api.begin_call(mod, handle(blue), "Jump", &call) == TSUCHI_OK && api.invoke(mod, call) == TSUCHI_OK);
    fake::destroy(blue);
    const int invoked = engine.invoked;
    CHECK(api.invoke(mod, call) == TSUCHI_NOT_FOUND && engine.invoked == invoked && api.end_call(mod, call) == TSUCHI_OK);

    // The engine runs out of memory, or is not ready to give any.
    engine.out_of_memory = true;
    CHECK(api.begin_call(mod, handle(w.red), "Heal", &call) == TSUCHI_NO_MEMORY && tsuchi::owned_count(mod) == 0);
    engine.out_of_memory = false;
    fake::put<uint64_t>(fake::main_image() + fake::t::data::allocator, 0);
    CHECK(api.begin_call(mod, handle(w.red), "Heal", &call) == TSUCHI_NOT_READY);
    fake::put<uint64_t>(fake::main_image() + fake::t::data::allocator, engine.allocator);
    fake::put<int32_t>(w.heal + fake::t::struct_type::alignment, 3); // a size the engine would never give
    CHECK(api.begin_call(mod, handle(w.red), "Heal", &call) == TSUCHI_NOT_READY);
    fake::put<int32_t>(w.heal + fake::t::struct_type::alignment, 4);

    // A mod that stops with calls open has them ended for it.
    TSUCHI_Mod* careless = tsuchi::mod_create("reflection.careless");
    TSUCHI_PreparedCall left[3];
    for (TSUCHI_PreparedCall& one : left) CHECK(api.begin_call(careless, handle(w.red), "Heal", &one) == TSUCHI_OK);
    const int destroyed = engine.destroyed, freed_before = engine.freed;
    tsuchi::mod_stop(careless);
    CHECK(engine.destroyed == destroyed + 3 && engine.freed == freed_before + 3);
    CHECK(api.invoke(careless, left[0]) == TSUCHI_INVALID && api.invoke(mod, left[0]) == TSUCHI_NOT_FOUND);

    // The game unloads a function while a call of it is open: the call is dead, and
    // its memory is left alone, since only the function knew how to take it apart.
    CHECK(api.begin_call(mod, handle(w.red), "Describe", &call) == TSUCHI_OK && api.invoke(mod, call) == TSUCHI_OK);
    fake::destroy(w.describe);
    const int freed_now = engine.freed;
    CHECK(api.invoke(mod, call) == TSUCHI_NOT_FOUND && api.call_get(mod, call, "ReturnValue", &value) == TSUCHI_NOT_FOUND);
    CHECK(api.end_call(mod, call) == TSUCHI_OK && engine.freed == freed_now && tsuchi::owned_count(mod) == 0);
    CHECK(api.begin_call(mod, handle(w.red), "Describe", &call) == TSUCHI_NOT_FOUND); // what the engine hands back is no longer a function
}

// A call ended from inside its own function keeps its memory until the engine
// has returned from every invocation of it.
void ended_while_running() {
    fake::Engine& engine = fake::engine();
    auto heal = engine.bodies[w.heal];
    TSUCHI_PreparedCall call{};
    TSUCHI_Value value = number(1.0);
    int freed_inside = -1, depth = 0, freed = 0, destroyed = 0;
    TSUCHI_Result nested = TSUCHI_OK;

    // The mod ends the call while the engine is in it.
    engine.bodies[w.heal] = [&](uint64_t object, uint64_t arguments) {
        CHECK(api.end_call(mod, call) == TSUCHI_OK);
        freed_inside = engine.freed;
        if (freed_inside == freed) heal(object, arguments); // writes its result into the call's memory
    };
    CHECK(api.begin_call(mod, handle(w.red), "Heal", &call) == TSUCHI_OK && api.call_set(mod, call, "Amount", &value) == TSUCHI_OK);
    freed = engine.freed, destroyed = engine.destroyed;
    CHECK(api.invoke(mod, call) == TSUCHI_OK && freed_inside == freed);
    CHECK(engine.freed == freed + 1 && engine.destroyed == destroyed + 1 && tsuchi::owned_count(mod) == 0);
    CHECK(api.invoke(mod, call) == TSUCHI_NOT_FOUND && api.end_call(mod, call) == TSUCHI_NOT_FOUND && engine.freed == freed + 1);

    // The mod stops while the engine is in its call.
    TSUCHI_Mod* stopping = tsuchi::mod_create("reflection.stopping");
    engine.bodies[w.heal] = [&](uint64_t object, uint64_t arguments) {
        tsuchi::mod_stop(stopping);
        freed_inside = engine.freed;
        if (freed_inside == freed) heal(object, arguments);
    };
    CHECK(api.begin_call(stopping, handle(w.red), "Heal", &call) == TSUCHI_OK);
    freed = engine.freed, destroyed = engine.destroyed;
    CHECK(api.invoke(stopping, call) == TSUCHI_OK && freed_inside == freed);
    CHECK(engine.freed == freed + 1 && engine.destroyed == destroyed + 1);

    // The function calls itself again through the same call, and the inner one ends it.
    engine.bodies[w.heal] = [&](uint64_t object, uint64_t arguments) {
        if (++depth == 1) {
            nested = api.invoke(mod, call);
            freed_inside = engine.freed;
        } else {
            CHECK(api.end_call(mod, call) == TSUCHI_OK);
        }
        if (engine.freed == freed) heal(object, arguments);
    };
    CHECK(api.begin_call(mod, handle(w.red), "Heal", &call) == TSUCHI_OK);
    freed = engine.freed;
    CHECK(api.invoke(mod, call) == TSUCHI_OK && nested == TSUCHI_OK && depth == 2 && freed_inside == freed && engine.freed == freed + 1);
    engine.bodies[w.heal] = heal;
    CHECK(tsuchi::owned_count(mod) == 0);
}

// A name can be as long as the engine lets it be.
void long_names() {
    const std::string name(300, 'L'), shape = "Shape." + name, lower(300, 'l'), longest(1023, 'L'), too_long(1024, 'L');
    TSUCHI_Value value{};
    CHECK(api.get(mod, handle(w.wordy), name.c_str(), &value) == TSUCHI_OK && is_integer(value, 5));
    CHECK(api.get(mod, handle(w.wordy), lower.c_str(), &value) == TSUCHI_OK && is_integer(value, 5));
    CHECK(api.get(mod, handle(w.wordy), shape.c_str(), &value) == TSUCHI_OK && is_integer(value, 7));
    const TSUCHI_Value eleven = integer(11);
    CHECK(api.set(mod, handle(w.wordy), shape.c_str(), &eleven) == TSUCHI_OK && fake::get<int32_t>(w.wordy + 0x44) == 11);
    fake::put<int32_t>(w.wordy + 0x44, 7);
    TSUCHI_Object found{};
    CHECK(api.find_class(mod, name.c_str(), &found) == TSUCHI_OK && from_handle(found.id) == w.long_class);
    // Listed whole into a buffer that fits it, and the field after it is still reached.
    char small[64] = "z", big[512];
    uint64_t needed = 0;
    CHECK(api.field_at(mod, handle(w.wordy), "", 0, small, sizeof small, &needed) == TSUCHI_CAPACITY && needed == 301 && std::string(small) == "z");
    CHECK(api.field_at(mod, handle(w.wordy), "", 0, big, sizeof big, &needed) == TSUCHI_OK && needed == 301 && big == name);
    CHECK(api.field_at(mod, handle(w.wordy), "", 1, small, sizeof small, &needed) == TSUCHI_OK && std::string(small) == "Shape");
    // A function by a long name, and what it writes.
    TSUCHI_PreparedCall call{};
    CHECK(api.begin_call(mod, handle(w.wordy), name.c_str(), &call) == TSUCHI_OK && api.invoke(mod, call) == TSUCHI_OK);
    CHECK(api.call_get(mod, call, name.c_str(), &value) == TSUCHI_OK && is_integer(value, 41));
    CHECK(api.call_get(mod, call, "Tail", &value) == TSUCHI_OK && is_integer(value, 42) && api.end_call(mod, call) == TSUCHI_OK);
    // The engine's own limit: a name of 1,023 characters can be looked for; a longer one cannot exist.
    CHECK(api.get(mod, handle(w.wordy), longest.c_str(), &value) == TSUCHI_NOT_FOUND && api.begin_call(mod, handle(w.wordy), longest.c_str(), &call) == TSUCHI_NOT_FOUND);
    const int named = int(fake::engine().names.size());
    CHECK(api.begin_call(mod, handle(w.wordy), too_long.c_str(), &call) == TSUCHI_NOT_FOUND && int(fake::engine().names.size()) == named);
    CHECK(tsuchi::owned_count(mod) == 0);
}

// A field named as the engine names it is found by that name and remembered; the memory is checked each time,
// so a class whose fields were made again, or a parent's, is looked through again.
void engine_named_fields() {
    const auto engine_named = [](uint64_t type, const std::u16string& text, int32_t offset) {
        const uint64_t property = fake::field(type, text, u"FloatProperty", offset, 4);
        const uint64_t engine = fake::engine_name(const_cast<char16_t*>(text.data()), text.size(), 1);
        fake::put<Name>(property + fake::t::property::name, Name{uint32_t(engine), uint32_t(engine >> 32)});
        return property;
    };
    const uint64_t parent = fake::engine_object(u"Pose", w.root);
    fake::field(parent, u"Before", u"FloatProperty", 0x40, 4);
    const uint64_t first = engine_named(parent, u"UpperRatio", 0x44);
    const uint64_t child = fake::engine_object(u"PoseChild", w.root);
    fake::set_super(child, parent);
    fake::field(child, u"Own", u"FloatProperty", 0x50, 4);
    const uint64_t posed = fake::engine_object(u"Posed", parent, 0x80);
    const uint64_t child_posed = fake::engine_object(u"ChildPosed", child, 0x80);
    const TSUCHI_Value half = number(0.5), quarter = number(0.25), eighth = number(0.125);
    CHECK(set("UpperRatio", half, posed) == TSUCHI_OK && fake::get<float>(posed + 0x44) == 0.5f);
    CHECK(set("UpperRatio", quarter, posed) == TSUCHI_OK && fake::get<float>(posed + 0x44) == 0.25f);
    CHECK(is_number(get("UpperRatio", posed), 0.25));
    // From the child, the parent's field.
    CHECK(set("UpperRatio", eighth, child_posed) == TSUCHI_OK && fake::get<float>(child_posed + 0x44) == 0.125f);
    // The parent's fields made again at the same address: the new field is the one written.
    const uint64_t remade = engine_named(fake::memory(0x80), u"UpperRatio", 0x48);
    fake::put<uint64_t>(parent + fake::t::struct_type::properties, remade);
    CHECK(set("UpperRatio", half, posed) == TSUCHI_OK && fake::get<float>(posed + 0x48) == 0.5f && fake::get<float>(posed + 0x44) == 0.25f);
    CHECK(set("UpperRatio", half, child_posed) == TSUCHI_OK && fake::get<float>(child_posed + 0x48) == 0.5f);
    // A field renamed in place is not the remembered one any more.
    fake::put<Name>(remade + fake::t::property::name, fake::name(u"Renamed"));
    CHECK(set("UpperRatio", quarter, posed) == TSUCHI_NOT_FOUND && fake::get<float>(posed + 0x48) == 0.5f);
    CHECK(set("Renamed", quarter, posed) == TSUCHI_OK && fake::get<float>(posed + 0x48) == 0.25f);
    // The child no longer made from that parent: the parent's field is not its own.
    fake::set_super(child, 0);
    CHECK(set("Renamed", quarter, child_posed) == TSUCHI_NOT_FOUND);
    (void)first;
}

#include "reflect_atomic_checks.h"

} // namespace

int main() {
    build();
    utf8_units();
    objects();
    paths();
    values();
    texts();
    calls();
    written_text();
    resized();
    listed();
    long_names();
    engine_named_fields();
    atomic_writes();
    lifetimes();
    ended_while_running();
    if (failures) std::printf("%d failed\n", failures);
    return failures;
}
