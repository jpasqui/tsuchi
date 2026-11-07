// The made-up world the reflection tests run in: a few classes with fields of
// every kind, some objects, and functions of each shape. Build it once.
#pragma once
#include <string>

#include "fake_engine.h"

struct World {
    uint64_t root, function_class, vector, component, actor, creature;
    uint64_t red, body, spare, creature_default;
    uint64_t heal, describe, where, census, spawn, jump, rename, sum, measure;
    uint64_t shape, box;
    uint64_t wide, verbose, wordy, recite, long_class;
    std::u16string long_name; // far longer than names usually are
    std::u16string heard; // the title Rename was last given
};
inline World w;

inline void build_world() {
    const uint64_t kIn = fake::t::constant::property_param;
    const uint64_t kOut = kIn | fake::t::constant::property_out;
    const uint64_t kResult = kOut | fake::t::constant::property_return;
    fake::start();
    fake::start_engine();
    w.root = fake::engine_object(u"Class", 0);
    w.function_class = fake::engine_object(u"Function", w.root);
    w.vector = fake::memory(0x80); // a struct's type: its list of fields, and how the engine makes and destroys one
    fake::put<uint64_t>(w.vector, fake::engine().vtable);
    fake::field(w.vector, u"X", u"FloatProperty", 0, 4);
    fake::field(w.vector, u"Y", u"FloatProperty", 4, 4);
    fake::field(w.vector, u"Z", u"FloatProperty", 8, 4);

    w.component = fake::engine_object(u"Component", w.root);
    fake::field(w.component, u"Scale", u"FloatProperty", 0x40, 4);

    w.actor = fake::engine_object(u"Actor", w.root);
    fake::field(w.actor, u"Health", u"FloatProperty", 0x40, 4);
    fake::field(w.actor, u"Lives", u"IntProperty", 0x44, 4);
    fake::field(w.actor, u"Small", u"Int8Property", 0x48, 1);
    fake::field(w.actor, u"Byte", u"ByteProperty", 0x49, 1);
    fake::bit_field(w.actor, u"bHidden", 0x4A, 0, 0x01);
    fake::bit_field(w.actor, u"bCanBeDamaged", 0x4A, 0, 0x04);
    fake::bit_field(w.actor, u"bFar", 0x4A, 1, 0x80); // its byte is one past the field's place
    fake::field(w.actor, u"Short", u"Int16Property", 0x4C, 2);
    fake::field(w.actor, u"UShort", u"UInt16Property", 0x4E, 2);
    fake::field(w.actor, u"Big", u"Int64Property", 0x50, 8);
    fake::field(w.actor, u"Ratio", u"DoubleProperty", 0x58, 8);
    fake::field(w.actor, u"Tag", u"NameProperty", 0x60, 8);
    fake::field(w.actor, u"Label", u"StrProperty", 0x68, 0x10);
    fake::field(w.actor, u"Root", u"ObjectProperty", 0x78, 8, w.component);
    fake::field(w.actor, u"Location", u"StructProperty", 0x80, 12, w.vector);
    fake::field(w.actor, u"Scores", u"IntProperty", 0x8C, 4, 0, 3);
    fake::field(w.actor, u"Tags", u"ArrayProperty", 0x98, 0x10, fake::loose_field(u"NameProperty", 8));
    fake::field(w.actor, u"Points", u"ArrayProperty", 0xA8, 0x10, fake::loose_field(u"StructProperty", 12, w.vector));
    fake::field(w.actor, u"Mode", u"EnumProperty", 0xB8, 1, fake::loose_field(u"ByteProperty", 1));
    fake::field(w.actor, u"Lookup", u"MapProperty", 0xC0, 0x10);
    fake::field(w.actor, u"Count", u"UInt32Property", 0xD0, 4);
    fake::field(w.actor, u"Total", u"UInt64Property", 0xD8, 8);
    fake::field(w.actor, u"Corners", u"StructProperty", 0xE0, 12, w.vector, 2);
    // References the engine keeps weakly, by an object's number and serial: one, and a list of them.
    fake::field(w.actor, u"Target", u"WeakObjectProperty", 0x180, 8, w.component);
    fake::field(w.actor, u"Watchers", u"ArrayProperty", 0x188, 0x10, fake::loose_field(u"WeakObjectProperty", 8, w.component));
    // References to assets by path: to a component, and to a class made from Actor.
    fake::field(w.actor, u"Icon", u"SoftObjectProperty", 0x120, 0x30, w.component);
    fake::put<uint64_t>(fake::field(w.actor, u"Kind", u"SoftClassProperty", 0x150, 0x30) + fake::t::property::meta_class, w.actor);
    fake::field(w.actor, u"Pictures", u"ArrayProperty", 0x1A0, 0x10, fake::loose_field(u"SoftObjectProperty", 0x30, w.component));

    w.creature = fake::engine_object(u"Creature", w.root);
    fake::set_super(w.creature, w.actor);
    fake::field(w.creature, u"Anger", u"IntProperty", 0xF8, 4);
    fake::field(w.creature, u"Notes", u"ArrayProperty", 0x100, 0x10, fake::loose_field(u"StrProperty", 0x10));
    fake::field(w.creature, u"Odd", u"ArrayProperty", 0x110, 0x10, fake::loose_field(u"MapProperty", 0x10));
    w.creature_default = fake::engine_object(u"Default__Creature", w.creature);
    fake::put<uint64_t>(w.creature + fake::t::class_type::default_object, w.creature_default);

    w.red = fake::engine_object(u"Red", w.creature, 0x1B0);
    w.body = fake::engine_object(u"Body", w.component);
    w.spare = fake::engine_object(u"Spare", w.component);
    fake::put<float>(w.red + 0x40, 12.5f);
    fake::put<int32_t>(w.red + 0x44, -3);
    fake::put<uint8_t>(w.red + 0x4A, 0x04);
    fake::put(w.red + 0x60, fake::name(u"Leader"));
    fake::put_string(w.red + 0x68, u"Olimar é\U0001F331");
    fake::put<uint64_t>(w.red + 0x78, w.body);
    fake::put<float>(w.body + 0x40, 2.0f);
    fake::put<float>(w.red + 0x84, 7.0f);
    for (int i = 0; i < 3; ++i) fake::put<int32_t>(w.red + 0x8C + 4 * i, 10 * (i + 1));
    // Arrays with room for more than they hold, as the game's have.
    const uint64_t tags = fake::memory(64), points = fake::memory(64);
    fake::put(tags + 16, fake::name(u"Leftover"));
    fake::put(tags, fake::name(u"Big"));
    fake::put(tags + 8, fake::name(u"Night", 3));
    fake::put<uint64_t>(w.red + 0x98 + fake::t::array::data, tags);
    fake::put<int32_t>(w.red + 0x98 + fake::t::array::count, 2);
    fake::put<int32_t>(w.red + 0x98 + fake::t::array::max, 8);
    fake::put<float>(points + 12 + 8, 9.0f);
    fake::put<uint64_t>(w.red + 0xA8 + fake::t::array::data, points);
    fake::put<int32_t>(w.red + 0xA8 + fake::t::array::count, 2);
    fake::put<int32_t>(w.red + 0xA8 + fake::t::array::max, 5);
    fake::put<uint8_t>(w.red + 0xB8, 2);
    fake::put<float>(w.red + 0xE0 + 12 + 4, 5.0f);
    fake::put<int32_t>(w.red + 0xF8, 4);

    // Heal(Amount, bFull) -> the new health.
    w.heal = fake::function(w.actor, w.function_class, u"Heal", 12, 4, [](uint64_t object, uint64_t arguments) {
        const float health = fake::get<uint8_t>(arguments + 4) ? 100.0f : fake::get<float>(object + 0x40) + fake::get<float>(arguments);
        fake::put(object + 0x40, health);
        fake::put(arguments + 8, health);
    });
    fake::flag(fake::field(w.heal, u"Amount", u"FloatProperty", 0, 4), kIn);
    fake::flag(fake::bit_field(w.heal, u"bFull", 4, 0, 0xFF), kIn);
    fake::flag(fake::field(w.heal, u"ReturnValue", u"FloatProperty", 8, 4), kResult);

    w.describe = fake::function(w.actor, w.function_class, u"Describe", 0x10, 8,
                                [](uint64_t, uint64_t arguments) { fake::put_string(arguments, u"Red Bulborb"); });
    fake::flag(fake::field(w.describe, u"ReturnValue", u"StrProperty", 0, 0x10), kResult);

    // Where() -> a vector, and the object itself through an argument the function writes.
    w.where = fake::function(w.actor, w.function_class, u"Where", 0x18, 8, [](uint64_t object, uint64_t arguments) {
        fake::put(arguments, 1.0f), fake::put(arguments + 4, 2.0f), fake::put(arguments + 8, 3.0f);
        fake::put(arguments + 0x10, object);
    });
    fake::flag(fake::field(w.where, u"ReturnValue", u"StructProperty", 0, 12, w.vector), kResult);
    fake::flag(fake::field(w.where, u"Found", u"ObjectProperty", 0x10, 8, w.actor), kOut);

    // Functions that belong to a class, not to an object.
    w.census = fake::function(w.actor, w.function_class, u"Census", 4, 4, [](uint64_t, uint64_t) {}, fake::t::constant::function_static);
    w.spawn = fake::function(w.creature, w.function_class, u"Spawn", 8, 4,
                             [](uint64_t, uint64_t arguments) { fake::put(arguments + 4, fake::get<int32_t>(arguments) * 2); },
                             fake::t::constant::function_static);
    fake::flag(fake::field(w.spawn, u"Count", u"IntProperty", 0, 4), kIn);
    fake::flag(fake::field(w.spawn, u"ReturnValue", u"IntProperty", 4, 4), kResult);

    w.jump = fake::function(w.creature, w.function_class, u"Jump", 0, 0,
                            [](uint64_t object, uint64_t) { fake::put(object + 0xF8, fake::get<int32_t>(object + 0xF8) + 1); });

    // Rename(NewName, Title): text both ways in. The title is passed the way the engine passes
    // big arguments it only reads, which looks like one the function writes but is not.
    w.rename = fake::function(w.actor, w.function_class, u"Rename", 0x18, 8, [](uint64_t object, uint64_t arguments) {
        fake::put(object + 0x60, fake::get<uint64_t>(arguments));
        w.heard.clear();
        const uint64_t data = fake::get<uint64_t>(arguments + 8 + fake::t::array::data);
        for (int32_t i = 0; i + 1 < fake::get<int32_t>(arguments + 8 + fake::t::array::count); ++i)
            w.heard.push_back(char16_t(fake::get<uint16_t>(data + 2 * uint64_t(i))));
    });
    fake::flag(fake::field(w.rename, u"NewName", u"NameProperty", 0, 8), kIn);
    fake::flag(fake::field(w.rename, u"Title", u"StrProperty", 8, 0x10), kOut | fake::t::constant::property_const);

    // Measure() -> a struct whose type has a parent: a box is a shape with a width.
    w.shape = fake::memory(0x80);
    fake::put<uint64_t>(w.shape, fake::engine().vtable);
    fake::field(w.shape, u"Height", u"FloatProperty", 0, 4);
    w.box = fake::memory(0x80);
    fake::put<uint64_t>(w.box, fake::engine().vtable);
    fake::set_super(w.box, w.shape);
    fake::field(w.box, u"Width", u"FloatProperty", 4, 4);
    w.measure = fake::function(w.actor, w.function_class, u"Measure", 8, 4,
                               [](uint64_t, uint64_t arguments) { fake::put(arguments, 2.0f), fake::put(arguments + 4, 3.0f); });
    fake::flag(fake::field(w.measure, u"ReturnValue", u"StructProperty", 0, 8, w.box), kResult);

    // An array that holds two of something reflection cannot read.
    fake::put<uint64_t>(w.red + 0x110 + fake::t::array::data, fake::memory(0x20));
    fake::put<int32_t>(w.red + 0x110 + fake::t::array::count, 2);

    // Sum(Values) -> the total of an array.
    w.sum = fake::function(w.actor, w.function_class, u"Sum", 0x18, 8, [](uint64_t, uint64_t arguments) {
        int32_t total = 0;
        const uint64_t data = fake::get<uint64_t>(arguments + fake::t::array::data);
        for (int32_t i = 0; i < fake::get<int32_t>(arguments + fake::t::array::count); ++i) total += fake::get<int32_t>(data + 4 * uint64_t(i));
        fake::put(arguments + 0x10, total);
    });
    fake::flag(fake::field(w.sum, u"Values", u"ArrayProperty", 0, 0x10, fake::loose_field(u"IntProperty", 4)), kIn);
    fake::flag(fake::field(w.sum, u"ReturnValue", u"IntProperty", 0x10, 4), kResult);

    // A class whose field, function and struct each have a very long name, with something after each.
    w.long_name = std::u16string(300, u'L');
    w.wide = fake::memory(0x80);
    fake::put<uint64_t>(w.wide, fake::engine().vtable);
    fake::field(w.wide, w.long_name, u"IntProperty", 0, 4);
    fake::field(w.wide, u"After", u"IntProperty", 4, 4);
    w.long_class = fake::engine_object(w.long_name, w.root);
    w.verbose = fake::engine_object(u"Verbose", w.root);
    fake::field(w.verbose, w.long_name, u"IntProperty", 0x40, 4);
    fake::field(w.verbose, u"Shape", u"StructProperty", 0x44, 8, w.wide);
    w.wordy = fake::engine_object(u"Wordy", w.verbose, 0x80);
    fake::put<int32_t>(w.wordy + 0x40, 5), fake::put<int32_t>(w.wordy + 0x44, 7), fake::put<int32_t>(w.wordy + 0x48, 9);
    w.recite = fake::function(w.verbose, w.function_class, w.long_name, 8, 4,
                              [](uint64_t, uint64_t arguments) { fake::put<int32_t>(arguments, 41), fake::put<int32_t>(arguments + 4, 42); });
    fake::flag(fake::field(w.recite, w.long_name, u"IntProperty", 0, 4), kOut);
    fake::flag(fake::field(w.recite, u"Tail", u"IntProperty", 4, 4), kOut);
}
