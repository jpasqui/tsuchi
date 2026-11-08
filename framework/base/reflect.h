// Reflection: the engine describes its own classes, their fields and their
// functions, and this reads those descriptions to reach a field by its path
// and to call a function by its name.
//
// A path is field names joined by dots, with [n] for an element of an array:
// "RootComponent.RelativeLocation.X", "Tags[2]". A dot after a field that
// refers to another object continues in that object. Field names ignore case,
// as the engine's own names do.
#pragma once
#include <stddef.h>
#include <stdint.h>

#include "names.h"
#include "../core/script_writes.h"
#include "tsuchi/core.h"

namespace tsuchi::game {

// Optional internal permit for a restoration that may reenter native code.
// The callback checks retained handles, never raw pointers into mutable tables.
struct CallGuard {
    bool (*allowed)(void*) = nullptr;
    void* context = nullptr;
    bool permits() const { return !allowed || allowed(context); }
};
// A value read from or written to a field. `object` is a handle.
enum class ValueKind : uint32_t { none, boolean, integer, number, object, text, structure, array };
struct Value {
    ValueKind kind = ValueKind::none;
    uint32_t count = 0; // array: its length; text: the bytes it takes
    union {
        bool boolean;
        int64_t integer = 0;
        double number;
        uint64_t object;
    };
};

// Where a path leads: the field's description and the memory it describes.
struct Place {
    uint64_t address = 0, property = 0;
    bool element = false; // one element of a fixed-length field
};

// The type that describes an object's fields: its class.
uint64_t type_of(uint64_t object);
// True when `type` is `wanted` or was made from it.
bool derives_from(uint64_t type, uint64_t wanted);
// Follows a path from the memory at `base`, which `type` describes.
// TSUCHI_NOT_FOUND for a name the type lacks, an index past the end, or an empty
// object reference along the way; TSUCHI_INVALID for a path that does not parse.
TSUCHI_Result find_place(uint64_t type, uint64_t base, const char* path, Place& out);
struct ArrayMemory;
struct WritePlan;
// Read paths through a detached array without swapping the live field's header.
TSUCHI_Result find_place_view(uint64_t type, uint64_t base, const char* path, uint64_t replaced, const ArrayMemory* view, Place& out, const WritePlan* pending = nullptr);

// TSUCHI_UNSUPPORTED for a kind of field this cannot read yet.
TSUCHI_Result read_value(const Place& place, Value& out);
// TSUCHI_INVALID when the value's kind does not suit the field.
TSUCHI_Result write_value(const Place& place, const Value& value);
// Prepared script writes retain old storage until every conversion/allocation succeeds.
struct PreparedField {
    Place destination;
    uint64_t scratch = 0, old_owned = 0;
    uint32_t bytes = 0;
    bool boolean = false;
    uint8_t bool_byte = 0, bool_mask = 0;
    ScriptText path;
    const ScriptWrite* wanted = nullptr;
};
struct PreparedArray {
    Place destination;
    uint64_t before_data = 0, after_data = 0;
    uint32_t before_count = 0, before_max = 0, after_count = 0, kept = 0;
};
struct WritePlan {
    List<PreparedField> fields;
    List<PreparedArray> arrays;
    bool committed = false;
    TSUCHI_Result (*field_ready)(void*, WritePlan&, uint32_t) = nullptr;
    void* field_context = nullptr;
    WritePlan() = default;
    WritePlan(const WritePlan&) = delete;
    WritePlan& operator=(const WritePlan&) = delete;
    ~WritePlan();
    TSUCHI_Result prepare(uint64_t type, uint64_t base, const char* path, const ScriptWrite& value, bool resize_arrays = true);
    TSUCHI_Result prepare_field(const Place& place, const char* path, const ScriptWrite& value);
    TSUCHI_Result replace_field(uint32_t index, const ScriptWrite& value);
    void commit(); // no validation, allocation, script callbacks, or fallible restoration
};

// An enum's stored number by its full name or unqualified member name.
// INVALID for a field without an enum, NOT_FOUND for an unknown name.
TSUCHI_Result enum_number(const Place& place, const char* name, int64_t& out);
// Direct and weak object references. Soft references keep paths as text.
bool object_property(const Place& place);
// The text of a name or string field, as UTF-8 with a terminator. `needed`
// gets the bytes it takes; TSUCHI_CAPACITY when that is more than `capacity`,
// and nothing is written.
TSUCHI_Result read_text(const Place& place, char* out, size_t capacity, size_t* needed);
// What a piece of the engine's text shows, the same way: text as the engine hands it around, which a widget
// keeps what it shows as. TSUCHI_INVALID for a field that holds no such text. This asks the game, which the
// reads above never do, so it is the framework's own to use and no part of what `reflect` reads.
TSUCHI_Result read_engine_text(const Place& place, char* out, size_t capacity, size_t* needed);

// Writes a name or string field from UTF-8. TSUCHI_INVALID for a field that holds
// neither or for text that is not UTF-8; TSUCHI_NO_MEMORY when the engine has no
// memory for it; TSUCHI_CAPACITY when a name is longer than the game stores.
TSUCHI_Result write_text(const Place& place, const char* text);
// The game's name for a piece of UTF-8 text, made if the game has none yet; the
// name of nothing for empty text. TSUCHI_INVALID for text that is not UTF-8,
// TSUCHI_CAPACITY for text longer than the game stores, TSUCHI_NOT_READY while the
// game cannot be asked.
TSUCHI_Result name_from_utf8(const char* text, Name& out);
// UTF-8 to the engine's 16-bit units. Counts every unit and writes those that
// fit, without a terminator. False for text that is not well-formed UTF-8.
bool utf8_to_units(const char* text, char16_t* out, size_t capacity, size_t& count);
// A string's 16-bit units as UTF-8 with a terminator. `needed` gets the bytes
// it takes; TSUCHI_CAPACITY when that is more than `capacity` or there is nowhere
// to write, and nothing is written.
TSUCHI_Result units_to_utf8(uint64_t data, int32_t units, char* out, size_t capacity, size_t* needed);
// Changes an array's length. New elements are zero, a reference by path to
// nothing, or as the engine makes a new struct; elements past the new length
// are destroyed. Nothing changes when it fails.
TSUCHI_Result resize(const Place& place, uint32_t count);

// An array as its field keeps it: where its elements are, how many there are,
// and how many there is room for.
struct ArrayMemory {
    uint64_t data = 0;
    int32_t count = 0, max = 0;
};
// The array a field holds. TSUCHI_INVALID for a field that is no array,
// TSUCHI_UNSUPPORTED for elements of a kind this cannot make.
TSUCHI_Result array_of(const Place& place, ArrayMemory& out);
// Gives an array field an array of `count` elements in memory of the engine's:
// copies of the first elements of `source`, an array of the field's kind that is
// left as it is, and past those, new ones. What the field held is given back in
// `before`, untouched, for the caller to keep and put back, or to free.
// TSUCHI_UNSUPPORTED for an array of structs, which this cannot copy. Nothing
// changes when it fails.
// Builds an owning copy without publishing it. The caller frees it on abort.
TSUCHI_Result copy_array(const Place& place, const ArrayMemory& source, uint32_t count, ArrayMemory& copied);
TSUCHI_Result replace_array(const Place& place, const ArrayMemory& source, uint32_t count, ArrayMemory& before);
// Puts an array in the field as it is. What the field held is the caller's.
TSUCHI_Result put_array(const Place& place, const ArrayMemory& array);
// Destroys the elements of an array of the field's kind that no field holds
// any more, and gives its memory back to the engine.
void free_array(const Place& place, const ArrayMemory& array);

// The type and memory of the struct a place holds. TSUCHI_INVALID when it holds something else.
TSUCHI_Result struct_at(const Place& place, uint64_t& type, uint64_t& base);
// The field of a type at a position, counting from 0, or 0 past the last. With
// `parents`, the fields of the type's parents follow its own.
uint64_t field_at(uint64_t type, uint32_t index, bool parents);
Name field_name(uint64_t property);
// Sets to null every direct object reference to one of `targets` that `object` holds in its fields, in structs
// inside them and in arrays of references or structs. Weak references already read as null for a destroyed
// object, and maps and sets are not looked into. `cleared` is told the path of each field it set and which
// target that field held. Gives the count.
uint32_t clear_references(uint64_t object, const uint64_t* targets, uint32_t count,
                          void (*cleared)(void* context, const char* path, uint32_t target), void* context);
// True for a value a function gives back: its result, or an argument it writes.
bool is_output(uint64_t property);

// A function of a class or its parents by name, or 0.
uint64_t find_function(uint64_t type, const char* name);
uint64_t find_function_guarded(uint64_t type, const char* name, CallGuard guard);
// The object a class keeps as the pattern for new ones, or 0.
uint64_t default_object(uint64_t type);

// Memory from the engine's own allocator, and giving it back. Null while the
// engine has no allocator yet, or has no memory to give.
void* engine_memory(uint64_t bytes);
void release_engine_memory(void* block);

// Memory for a function's arguments and results, set up as the engine expects.
TSUCHI_Result make_arguments(uint64_t function, uint64_t& memory);
TSUCHI_Result make_arguments_guarded(uint64_t function, uint64_t& memory, CallGuard guard);
void destroy_arguments(uint64_t function, uint64_t memory);
// Calls the function on the object. A function that belongs to its class and
// not to an object is called on the class's default object.
TSUCHI_Result call(uint64_t object, uint64_t function, uint64_t arguments);
TSUCHI_Result call_guarded(uint64_t object, uint64_t function, uint64_t arguments, CallGuard guard);
// The same through the call dispatch every object has, not the object's own. An actor's own drops every call
// until its world's actors are initialized, without saying so; this one runs the call then too.
// TSUCHI_UNSUPPORTED when that dispatch cannot be told for certain.
TSUCHI_Result call_object_dispatch(uint64_t object, uint64_t function, uint64_t arguments);
// Guarded restoration arguments are scalar or direct object references only.
TSUCHI_Result write_argument_guarded(const Place& place, const Value& value, CallGuard guard);

} // namespace tsuchi::game
