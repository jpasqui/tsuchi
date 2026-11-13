// The made-up game's description of itself: fields, functions, and the engine
// pieces reflection calls. Builds on fake_game.h; laid out as the same table says.
//
// Build one world per program. The framework remembers what it learns about
// kinds of field, as it would for the life of the game.
#pragma once
#include <functional>
#include <map>

#include "fake_game.h"

namespace fake {

inline uint64_t packed(tsuchi::game::Name name) { return uint64_t(name.index) | uint64_t(name.number) << 32; }

// ---- Fields ----------------------------------------------------------------

// The small record a field points at to say what kind it is.
inline uint64_t kind_record(const std::u16string& kind) {
    static std::map<std::u16string, uint64_t> records;
    uint64_t& record = records[kind];
    if (!record) {
        record = memory(16);
        put(record, name(kind));
    }
    return record;
}

// A field description that is not in any type's list, such as an array's element.
inline uint64_t loose_field(const std::u16string& kind, int32_t bytes, uint64_t inner = 0) {
    const uint64_t property = memory(t::property::size);
    put<uint64_t>(property + t::property::kind, kind_record(kind));
    put<int32_t>(property + t::property::count, 1);
    put<int32_t>(property + t::property::element_bytes, bytes);
    put<uint64_t>(property + t::property::inner, inner);
    return property;
}

// Adds a field to the end of a type's list.
inline uint64_t field(uint64_t type, const std::u16string& field_name, const std::u16string& kind, int32_t offset, int32_t bytes,
                      uint64_t inner = 0, int32_t count = 1) {
    const uint64_t property = loose_field(kind, bytes, inner);
    put(property + t::property::name, name(field_name));
    put<int32_t>(property + t::property::count, count);
    put<int32_t>(property + t::property::offset, offset);
    uint64_t link = type + t::struct_type::properties;
    while (get<uint64_t>(link)) link = get<uint64_t>(link) + t::property::next;
    put<uint64_t>(link, property);
    return property;
}

// Marks a field as an argument, a result and so on. Gives the field back.
inline uint64_t flag(uint64_t property, uint64_t bits) {
    put<uint64_t>(property + t::property::flags, bits);
    return property;
}

// A yes-or-no field kept in some bits of one byte.
inline uint64_t bit_field(uint64_t type, const std::u16string& field_name, int32_t offset, uint8_t byte, uint8_t mask) {
    const uint64_t property = field(type, field_name, u"BoolProperty", offset, 1);
    put<uint8_t>(property + t::property::bool_byte, byte);
    put<uint8_t>(property + t::property::bool_field_mask, mask);
    return property;
}

// A string as the game stores one: 16-bit units with a zero at the end.
inline void put_string(uint64_t at, const std::u16string& text) {
    const uint64_t data = memory(2 * (text.size() + 1));
    for (size_t i = 0; i < text.size(); ++i) put<uint16_t>(data + 2 * i, uint16_t(text[i]));
    put<uint64_t>(at + t::array::data, data);
    put<int32_t>(at + t::array::count, int32_t(text.size() + 1));
}

// ---- The engine's own functions ----------------------------------------------

struct Engine {
    uint64_t vtable = 0, allocator = 0;
    void (*object_created)(uint64_t) = nullptr; // optional fixture-specific class table setup
    std::map<std::u16string, uint64_t> names;                       // text -> packed name
    std::map<std::pair<uint64_t, uint64_t>, uint64_t> functions;    // (class, packed name) -> function
    std::map<uint64_t, std::function<void(uint64_t object, uint64_t arguments)>> bodies;
    int allocated = 0, freed = 0, initialized = 0, destroyed = 0, invoked = 0;
    int made = 0, unmade = 0; // structs, counted one by one
    uint64_t last_object = 0, last_block = 0;
    bool out_of_memory = false;
    int fail_after = -1; // fail one selected native allocation boundary
};
inline Engine& engine() {
    static Engine instance;
    return instance;
}

inline void* engine_allocate(void*, uint64_t bytes, uint32_t) {
    if (engine().out_of_memory || engine().fail_after == 0) return nullptr;
    if (engine().fail_after > 0) --engine().fail_after;
    ++engine().allocated;
    const uint64_t block = memory(bytes);
    std::memset(reinterpret_cast<void*>(block), 0xCD, bytes); // the game's allocator does not clear
    return reinterpret_cast<void*>(engine().last_block = block);
}
inline void engine_free(void*, void* block) {
    ++engine().freed;
    unmap(reinterpret_cast<uint64_t>(block));
}
inline void engine_initialize(void*, void*, int32_t count) { ++engine().initialized, engine().made += count; }
inline void engine_destroy(void*, void*, int32_t count) { ++engine().destroyed, engine().unmade += count; }
inline void engine_process(void* object, void* function, void* arguments) {
    ++engine().invoked;
    engine().last_object = reinterpret_cast<uint64_t>(object);
    auto body = engine().bodies.find(reinterpret_cast<uint64_t>(function));
    if (body != engine().bodies.end()) body->second(reinterpret_cast<uint64_t>(object), reinterpret_cast<uint64_t>(arguments));
}
inline uint64_t engine_name(void* text, uint64_t length, uint64_t add) {
    const std::u16string wanted(static_cast<const char16_t*>(text), size_t(length));
    auto found = engine().names.find(wanted);
    if (found != engine().names.end()) return found->second;
    if (!add) return 0;
    const uint64_t made = packed(name(wanted));
    engine().names[wanted] = made;
    return made;
}
inline void* engine_find_function(void* type, uint64_t function_name, uint64_t include_parents) {
    for (auto at = reinterpret_cast<uint64_t>(type); at; at = include_parents ? get<uint64_t>(at + t::struct_type::super) : 0) {
        auto found = engine().functions.find({at, function_name});
        if (found != engine().functions.end()) return reinterpret_cast<void*>(found->second);
    }
    return nullptr;
}

// Call after start(): the allocator, one function table every object shares, and
// the two functions the game table names.
inline void start_engine() {
    engine() = Engine{};
    for (auto function : {reinterpret_cast<uint64_t>(&engine_allocate), reinterpret_cast<uint64_t>(&engine_free),
                          reinterpret_cast<uint64_t>(&engine_initialize), reinterpret_cast<uint64_t>(&engine_destroy),
                          reinterpret_cast<uint64_t>(&engine_process)})
        code().push_back(function);
    engine().vtable = memory(0x40);
    put(engine().vtable + t::object_vtable::process_event, &engine_process);
    put(engine().vtable + t::struct_vtable::initialize, &engine_initialize);
    put(engine().vtable + t::struct_vtable::destroy, &engine_destroy);
    const uint64_t allocator_table = memory(0x40);
    put(allocator_table + t::allocator_vtable::malloc, &engine_allocate);
    put(allocator_table + t::allocator_vtable::free, &engine_free);
    engine().allocator = memory(16);
    put<uint64_t>(engine().allocator, allocator_table);
    put<uint64_t>(main_image() + t::data::allocator, engine().allocator);
    t::fn::name_from_text = engine_name;
    t::fn::class_find_function = engine_find_function;
}

// An object with the engine's function table, so it can be called.
inline uint64_t engine_object(const std::u16string& object_name, uint64_t type, size_t bytes = 0x100) {
    const uint64_t address = object(object_name, type, bytes);
    put<uint64_t>(address, engine().vtable);
    if (engine().object_created) engine().object_created(address);
    return address;
}

// A function of a class. Add its arguments with field(); `bytes` is the memory they take together.
inline uint64_t function(uint64_t owner, uint64_t function_class, const std::u16string& function_name, int32_t bytes, int32_t alignment,
                         std::function<void(uint64_t object, uint64_t arguments)> body, uint32_t flags = 0) {
    const uint64_t address = engine_object(function_name, function_class);
    put<uint64_t>(address + t::object::outer, owner);
    put<int32_t>(address + t::struct_type::bytes, bytes);
    put<int32_t>(address + t::struct_type::alignment, alignment);
    put<uint32_t>(address + t::function::flags, flags);
    engine().functions[{owner, engine_name(const_cast<char16_t*>(function_name.data()), function_name.size(), 1)}] = address;
    engine().bodies[address] = std::move(body);
    return address;
}

} // namespace fake
