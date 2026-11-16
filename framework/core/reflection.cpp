// The reflect service: handles in, values out, and the calls a mod has begun.
#include "reflection.h"

#include <string.h>

#include "objects.h"
#include "reflect.h"
#include "reflection_service.h"
#include "registry.h"
#include "reflection_fields.h"

namespace tsuchi {
namespace {

using game::Place;
using game::Value;
using game::ValueKind;

static_assert(uint32_t(ValueKind::none) == TSUCHI_VALUE_NONE && uint32_t(ValueKind::boolean) == TSUCHI_VALUE_BOOLEAN &&
              uint32_t(ValueKind::integer) == TSUCHI_VALUE_INTEGER && uint32_t(ValueKind::number) == TSUCHI_VALUE_NUMBER &&
              uint32_t(ValueKind::object) == TSUCHI_VALUE_OBJECT && uint32_t(ValueKind::text) == TSUCHI_VALUE_TEXT &&
              uint32_t(ValueKind::structure) == TSUCHI_VALUE_STRUCT && uint32_t(ValueKind::array) == TSUCHI_VALUE_ARRAY,
              "the two lists of kinds are the same list");

// A call a mod has begun: the function, the object, and the memory its
// arguments and results are in. A call ended while the engine is still in it
// keeps its memory until the last of those invocations returns.
struct Call {
    uint64_t id;
    TSUCHI_Mod* mod;
    uint64_t function, function_handle, object_handle, memory;
    uint32_t running;
    bool ended;
};

const Kind kCall{"reflection.call"};
game::ClassRef class_class{"Class"};
Call* calls = nullptr;
uint32_t call_count = 0, call_capacity = 0;
uint64_t next_call = 1;

TSUCHI_Value to_public(const Value& value) {
    TSUCHI_Value out{};
    out.kind = uint32_t(value.kind);
    out.count = value.count;
    switch (value.kind) {
    case ValueKind::boolean: out.as.boolean = value.boolean; break;
    case ValueKind::integer: out.as.integer = value.integer; break;
    case ValueKind::number: out.as.number = value.number; break;
    case ValueKind::object: out.as.object.id = value.object; break;
    default: break;
    }
    return out;
}

// False for a value a field cannot be given: a kind that is only ever read, or
// a number that is not one.
bool from_public(const TSUCHI_Value& value, Value& out) {
    out = Value{};
    out.kind = ValueKind(value.kind);
    switch (value.kind) {
    case TSUCHI_VALUE_NONE: return true;
    case TSUCHI_VALUE_BOOLEAN: out.boolean = value.as.boolean; return true;
    case TSUCHI_VALUE_INTEGER: out.integer = value.as.integer; return true;
    case TSUCHI_VALUE_NUMBER: out.number = value.as.number; return value.as.number - value.as.number == 0;
    case TSUCHI_VALUE_OBJECT: out.object = value.as.object.id; return value.as.object.id != 0;
    default: return false;
    }
}

} // namespace

TSUCHI_Result read_at(uint64_t type, uint64_t base, const char* path, TSUCHI_Value* out) {
    Place place;
    TSUCHI_Result result = game::find_place(type, base, path, place);
    if (result != TSUCHI_OK) return result;
    Value value;
    result = game::read_value(place, value);
    if (result == TSUCHI_OK) *out = to_public(value);
    return result;
}

TSUCHI_Result write_at(uint64_t type, uint64_t base, const char* path, const TSUCHI_Value* value) {
    Value wanted;
    if (!from_public(*value, wanted)) return TSUCHI_INVALID;
    Place place;
    const TSUCHI_Result result = game::find_place(type, base, path, place);
    return result == TSUCHI_OK ? game::write_value(place, wanted) : result;
}

TSUCHI_Result text_at(uint64_t type, uint64_t base, const char* path, char* buffer, uint64_t capacity, uint64_t* out) {
    Place place;
    const TSUCHI_Result result = game::find_place(type, base, path, place);
    if (result != TSUCHI_OK) return result;
    size_t needed = 0;
    const TSUCHI_Result read = game::read_text(place, buffer, size_t(capacity), &needed);
    if (read == TSUCHI_OK || read == TSUCHI_CAPACITY) *out = needed;
    return read;
}

TSUCHI_Result write_text_at(uint64_t type, uint64_t base, const char* path, const char* text) {
    Place place;
    const TSUCHI_Result result = game::find_place(type, base, path, place);
    return result == TSUCHI_OK ? game::write_text(place, text) : result;
}

TSUCHI_Result resize_at(uint64_t type, uint64_t base, const char* path, uint32_t count) {
    Place place;
    const TSUCHI_Result result = game::find_place(type, base, path, place);
    return result == TSUCHI_OK ? game::resize(place, count) : result;
}

// The name of the field at a position in the struct a path leads to. An empty
// path is the type itself.
TSUCHI_Result field_name_at(uint64_t type, uint64_t base, const char* path, uint32_t index, bool parents, char* buffer, uint64_t capacity,
                         uint64_t* out) {
    if (*path) {
        Place place;
        TSUCHI_Result result = game::find_place(type, base, path, place);
        if (result == TSUCHI_OK) result = game::struct_at(place, type, base);
        if (result != TSUCHI_OK) return result;
        parents = true;
    }
    const uint64_t property = game::field_at(type, index, parents);
    return property ? game::name_into(game::field_name(property), buffer, capacity, out) : TSUCHI_NOT_FOUND;
}

namespace {

// ---- Objects ---------------------------------------------------------------

TSUCHI_Result find_class(const char* name, TSUCHI_Object* out) {
    game::ClassRef ref{name};
    const uint64_t found = game::find_class(ref);
    if (!found) return TSUCHI_NOT_FOUND;
    out->id = ref.handle;
    return TSUCHI_OK;
}

TSUCHI_Result class_of(TSUCHI_Object object, TSUCHI_Object* out) {
    const uint64_t address = game::from_handle(object.id);
    const uint64_t handle = address ? game::to_handle(game::class_of(address)) : 0;
    if (!handle) return TSUCHI_NOT_FOUND;
    out->id = handle;
    return TSUCHI_OK;
}

TSUCHI_Result is_a(TSUCHI_Object object, TSUCHI_Object type, bool* out) {
    const uint64_t address = game::from_handle(object.id), wanted = game::from_handle(type.id);
    if (!address || !wanted) return TSUCHI_NOT_FOUND;
    *out = game::derives_from(game::class_of(address), wanted);
    return TSUCHI_OK;
}

TSUCHI_Result name(TSUCHI_Object object, char* buffer, uint64_t capacity, uint64_t* out) {
    const uint64_t address = game::from_handle(object.id);
    return address ? game::name_into(game::name_of(address), buffer, capacity, out) : TSUCHI_NOT_FOUND;
}

TSUCHI_Result path(TSUCHI_Object object, char* buffer, uint64_t capacity, uint64_t* out) {
    const uint64_t address = game::from_handle(object.id);
    return address ? game::object_path(address, buffer, capacity, out) : TSUCHI_NOT_FOUND;
}

// ---- Fields ----------------------------------------------------------------

TSUCHI_Result get(TSUCHI_Object object, const char* path, TSUCHI_Value* out) {
    const uint64_t address = game::from_handle(object.id);
    return address ? read_at(game::type_of(address), address, path, out) : TSUCHI_NOT_FOUND;
}

TSUCHI_Result set(TSUCHI_Object object, const char* path, const TSUCHI_Value* value) {
    const uint64_t address = game::from_handle(object.id);
    return address ? write_at(game::type_of(address), address, path, value) : TSUCHI_NOT_FOUND;
}

TSUCHI_Result get_text(TSUCHI_Object object, const char* path, char* buffer, uint64_t capacity, uint64_t* out) {
    const uint64_t address = game::from_handle(object.id);
    return address ? text_at(game::type_of(address), address, path, buffer, capacity, out) : TSUCHI_NOT_FOUND;
}

TSUCHI_Result set_text(TSUCHI_Object object, const char* path, const char* text) {
    const uint64_t address = game::from_handle(object.id);
    return address ? write_text_at(game::type_of(address), address, path, text) : TSUCHI_NOT_FOUND;
}

TSUCHI_Result resize(TSUCHI_Object object, const char* path, uint32_t count) {
    const uint64_t address = game::from_handle(object.id);
    return address ? resize_at(game::type_of(address), address, path, count) : TSUCHI_NOT_FOUND;
}

TSUCHI_Result field_at(TSUCHI_Object object, const char* path, uint32_t index, char* buffer, uint64_t capacity, uint64_t* out) {
    const uint64_t address = game::from_handle(object.id);
    return address ? field_name_at(game::type_of(address), address, path, index, true, buffer, capacity, out) : TSUCHI_NOT_FOUND;
}

// ---- Calls -----------------------------------------------------------------

// Any call by its number, ended or not, or null.
Call* call_by_id(uint64_t id) {
    for (uint32_t i = 0; i < call_count; ++i)
        if (calls[i].id == id) return &calls[i];
    return nullptr;
}

// The mod's own call, or null. Another mod's call, or an ended one, is as good as none.
Call* find_call(TSUCHI_Mod* mod, uint64_t id) {
    Call* call = call_by_id(id);
    return call && call->mod == mod && !call->ended ? call : nullptr;
}

// The function a call is for, or 0 when the engine has let go of it.
uint64_t function_of(const Call& call) { return game::from_handle(call.function_handle) == call.function ? call.function : 0; }

// Frees a call's memory and forgets it. Also what the registry runs for each
// call a mod still has when it stops.
void drop_call(void*, uint64_t id) {
    Call* call = call_by_id(id);
    if (!call) return;
    call->ended = true;
    // The engine is still in the function: the last invocation to return frees the memory.
    if (call->running) return;
    // Without its function the memory cannot be taken apart safely, so it is left.
    if (const uint64_t function = function_of(*call)) game::destroy_arguments(function, call->memory);
    *call = calls[--call_count];
}

TSUCHI_Result begin_call(TSUCHI_Mod* mod, TSUCHI_Object object, const char* function_name, TSUCHI_PreparedCall* out) {
    const uint64_t address = game::from_handle(object.id);
    if (!address) return TSUCHI_NOT_FOUND;
    // A class stands for itself: the function is looked for in it, not in the class of classes.
    const uint64_t type = game::is_a(address, class_class) ? address : game::class_of(address);
    const uint64_t function = game::find_function(type, function_name);
    const uint64_t function_handle = game::to_handle(function);
    if (!function_handle) return TSUCHI_NOT_FOUND;
    if (call_count == call_capacity) {
        const uint32_t next = call_capacity ? call_capacity * 2 : 16;
        auto* grown = static_cast<Call*>(allocate(sizeof(Call) * next));
        if (!grown) return TSUCHI_NO_MEMORY;
        if (calls) memcpy(static_cast<void*>(grown), calls, sizeof(Call) * call_count);
        release(calls);
        calls = grown;
        call_capacity = next;
    }
    uint64_t memory = 0;
    TSUCHI_Result result = game::make_arguments(function, memory);
    if (result != TSUCHI_OK) return result;
    const uint64_t id = next_call++;
    result = own(mod, kCall, id, drop_call, nullptr);
    if (result != TSUCHI_OK) {
        game::destroy_arguments(function, memory);
        return result;
    }
    calls[call_count++] = Call{id, mod, function, function_handle, object.id, memory, 0, false};
    out->id = id;
    return TSUCHI_OK;
}

TSUCHI_Result call_set(TSUCHI_Mod* mod, TSUCHI_PreparedCall call, const char* path, const TSUCHI_Value* value) {
    const Call* found_call = find_call(mod, call.id);
    const uint64_t function = found_call ? function_of(*found_call) : 0;
    return function ? write_at(function, found_call->memory, path, value) : TSUCHI_NOT_FOUND;
}

TSUCHI_Result invoke(TSUCHI_Mod* mod, TSUCHI_PreparedCall call) {
    Call* found = find_call(mod, call.id);
    if (!found) return TSUCHI_NOT_FOUND;
    // The function may begin or end calls, this one too, which moves this table.
    const Call saved_call = *found;
    const uint64_t function = function_of(saved_call), object = game::from_handle(saved_call.object_handle);
    if (!function || !object) return TSUCHI_NOT_FOUND;
    ++found->running;
    const TSUCHI_Result result = game::call(object, function, saved_call.memory);
    // A running call stays in the table, so it is found again.
    Call* after = call_by_id(saved_call.id);
    --after->running;
    if (!after->running && after->ended) drop_call(nullptr, saved_call.id);
    return result;
}

TSUCHI_Result call_get(TSUCHI_Mod* mod, TSUCHI_PreparedCall call, const char* path, TSUCHI_Value* out) {
    const Call* found_call = find_call(mod, call.id);
    const uint64_t function = found_call ? function_of(*found_call) : 0;
    return function ? read_at(function, found_call->memory, path, out) : TSUCHI_NOT_FOUND;
}

TSUCHI_Result call_get_text(TSUCHI_Mod* mod, TSUCHI_PreparedCall call, const char* path, char* buffer, uint64_t capacity, uint64_t* out) {
    const Call* found_call = find_call(mod, call.id);
    const uint64_t function = found_call ? function_of(*found_call) : 0;
    return function ? text_at(function, found_call->memory, path, buffer, capacity, out) : TSUCHI_NOT_FOUND;
}

TSUCHI_Result call_set_text(TSUCHI_Mod* mod, TSUCHI_PreparedCall call, const char* path, const char* text) {
    const Call* found_call = find_call(mod, call.id);
    const uint64_t function = found_call ? function_of(*found_call) : 0;
    return function ? write_text_at(function, found_call->memory, path, text) : TSUCHI_NOT_FOUND;
}

TSUCHI_Result call_resize(TSUCHI_Mod* mod, TSUCHI_PreparedCall call, const char* path, uint32_t count) {
    const Call* found_call = find_call(mod, call.id);
    const uint64_t function = found_call ? function_of(*found_call) : 0;
    return function ? resize_at(function, found_call->memory, path, count) : TSUCHI_NOT_FOUND;
}

// A function's own list is its arguments and results; its parent, when it has
// one, lists the same ones again.
TSUCHI_Result call_field_at(TSUCHI_Mod* mod, TSUCHI_PreparedCall call, const char* path, uint32_t index, char* buffer, uint64_t capacity, uint64_t* out) {
    const Call* found_call = find_call(mod, call.id);
    const uint64_t function = found_call ? function_of(*found_call) : 0;
    return function ? field_name_at(function, found_call->memory, path, index, false, buffer, capacity, out) : TSUCHI_NOT_FOUND;
}

TSUCHI_Result call_output_at(TSUCHI_Mod* mod, TSUCHI_PreparedCall call, uint32_t index, char* buffer, uint64_t capacity, uint64_t* out) {
    const Call* found_call = find_call(mod, call.id);
    const uint64_t function = found_call ? function_of(*found_call) : 0;
    if (!function) return TSUCHI_NOT_FOUND;
    for (uint32_t at = 0;; ++at) {
        const uint64_t property = game::field_at(function, at, false);
        if (!property) return TSUCHI_NOT_FOUND;
        if (game::is_output(property) && !index--) return game::name_into(game::field_name(property), buffer, capacity, out);
    }
}

TSUCHI_Result end_call(TSUCHI_Mod* mod, TSUCHI_PreparedCall call) {
    if (disown(mod, kCall, call.id) != TSUCHI_OK) return TSUCHI_NOT_FOUND; // none, or another mod's
    drop_call(nullptr, call.id);
    return TSUCHI_OK;
}

TSUCHI_Result write_script(TSUCHI_Mod* mod, const ScriptTarget& target, const char* path, const ScriptWrite& value) {
    if (!mod_usable(mod)) return TSUCHI_INVALID;
    uint64_t type = 0, base = 0;
    if (target.call.id) {
        const Call* call = find_call(mod, target.call.id);
        type = call ? function_of(*call) : 0;
        base = type ? call->memory : 0;
    } else {
        base = game::from_handle(target.object.id);
        type = base ? game::type_of(base) : 0;
    }
    if (!type || !base) return TSUCHI_NOT_FOUND;
    game::WritePlan plan;
    const TSUCHI_Result result = plan.prepare(type, base, path, value);
    if (result == TSUCHI_OK) plan.commit();
    return result;
}

const ConsoleVariables* console = nullptr;
TSUCHI_Result console_read(const char* name, float* out) {
    return console ? console->read(name, out) : TSUCHI_NOT_READY;
}
TSUCHI_Result console_hold(TSUCHI_Mod* mod, const char* name, float value) {
    return console ? console->hold(mod, name, value) : TSUCHI_NOT_READY;
}
TSUCHI_Result console_release(TSUCHI_Mod* mod, const char* name) {
    return console ? console->release(mod, name) : TSUCHI_NOT_FOUND;
}
const reflection::Backend service{find_class, class_of,      is_a,        name,   path,   get,      set,           get_text,
                               set_text,   resize,        field_at,    begin_call, call_set, call_set_text, call_resize,
                               invoke,     call_get,      call_get_text, call_field_at, call_output_at, end_call,
                               console_read, console_hold, console_release,
                               field_holds::hold, field_holds::update, field_holds::end};

} // namespace

void set_console_variables(const ConsoleVariables* backend) { console = backend; }

void reflect_start() {
    reflection::set_backend(&service);
    object_script_writer = write_script;
}

} // namespace tsuchi
