// The script side of the reflect service's functions that pass plain values:
// get, set and call. A script gives and gets values as what they are, so these
// turn Lua values into the service's one-at-a-time calls and back.
#include <stdio.h>
#include <string.h>

#include "tsuchi_lua.h"
#include "script_writes.h"

namespace {

using tsuchi_lua::host;
using tsuchi_lua::mod;

constexpr int kMaxDepth = 16; // structs inside structs; references to objects are not followed

// Where values are: an object's fields, the arguments and results of a call, or
// a row of a data table, where `object` is the table.
struct Target {
    lua_State* L;
    TSUCHI_Object object{};
    TSUCHI_PreparedCall call{};
    const char* row = nullptr;

    const TSUCHI_ReflectionService& api() const { return *host(L)->reflection; }
    const TSUCHI_DatatableService& tables() const { return *host(L)->datatable; }
    TSUCHI_Result get(const char* path, TSUCHI_Value* out) const {
        if (row) return tables().get(mod(L), object, row, path, out);
        return call.id ? api().call_get(mod(L), call, path, out) : api().get(mod(L), object, path, out);
    }
    TSUCHI_Result set(const char* path, const TSUCHI_Value& value) const {
        if (row) return tables().override(mod(L), object, row, path, &value);
        return call.id ? api().call_set(mod(L), call, path, &value) : api().set(mod(L), object, path, &value);
    }
    TSUCHI_Result get_text(const char* path, char* buffer, uint64_t capacity, uint64_t* needed) const {
        if (row) return tables().get_text(mod(L), object, row, path, buffer, capacity, needed);
        return call.id ? api().call_get_text(mod(L), call, path, buffer, capacity, needed)
                    : api().get_text(mod(L), object, path, buffer, capacity, needed);
    }
    TSUCHI_Result set_text(const char* path, const char* text) const {
        if (row) return tables().override_text(mod(L), object, row, path, text);
        return call.id ? api().call_set_text(mod(L), call, path, text) : api().set_text(mod(L), object, path, text);
    }
    // An array in a row keeps its length: a list of that length is taken, as for values side by side.
    TSUCHI_Result resize(const char* path, uint32_t count) const {
        if (row) return TSUCHI_INVALID;
        return call.id ? api().call_resize(mod(L), call, path, count) : api().resize(mod(L), object, path, count);
    }
    TSUCHI_Result field_at(const char* path, uint32_t index, char* buffer, uint64_t capacity, uint64_t* needed) const {
        if (row) return tables().field_at(mod(L), object, row, path, index, buffer, capacity, needed);
        return call.id ? api().call_field_at(mod(L), call, path, index, buffer, capacity, needed)
                    : api().field_at(mod(L), object, path, index, buffer, capacity, needed);
    }
};

// A path that grows and shrinks as the value is walked.
struct Path {
    char text[4096];
    size_t length = 0;

    bool set(const char* start) {
        length = strlen(start);
        if (length >= sizeof text) return false;
        memcpy(text, start, length + 1);
        return true;
    }
    bool field(const char* name) {
        const int written = snprintf(text + length, sizeof text - length, "%s%s", length ? "." : "", name);
        if (written < 0 || size_t(written) >= sizeof text - length) return false;
        length += size_t(written);
        return true;
    }
    bool element(uint32_t index) {
        const int written = snprintf(text + length, sizeof text - length, "[%u]", index);
        if (written < 0 || size_t(written) >= sizeof text - length) return false;
        length += size_t(written);
        return true;
    }
    void back_to(size_t earlier) { text[length = earlier] = 0; }
};

// Reads a name into `small` when it fits there, or into a buffer of the size it
// asked for, owned by the script state. Leaves one value on the stack either way.
// Null when there is no such name.
template <typename Read>
const char* read_name(lua_State* L, char* small, size_t capacity, Read read) {
    uint64_t needed = 0;
    TSUCHI_Result result = read(small, capacity, &needed);
    if (result != TSUCHI_CAPACITY) {
        lua_pushnil(L);
        return result == TSUCHI_OK ? small : nullptr;
    }
    char* big = static_cast<char*>(lua_newuserdatauv(L, static_cast<size_t>(needed), 0));
    return read(big, needed, &needed) == TSUCHI_OK ? big : nullptr;
}

// True when the object belongs to the class of that name. The class is looked up once and kept.
bool belongs(lua_State* L, uint64_t id, TSUCHI_Object& type, const char* name) {
    const TSUCHI_ReflectionService& api = *host(L)->reflection;
    bool is = false;
    TSUCHI_Result result = type.id ? api.is_a(mod(L), TSUCHI_Object{id}, type, &is) : TSUCHI_NOT_FOUND;
    if (result != TSUCHI_OK && api.find_class(mod(L), name, &type) == TSUCHI_OK) result = api.is_a(mod(L), TSUCHI_Object{id}, type, &is);
    return result == TSUCHI_OK && is;
}

// An actor comes back as an actor and a part of one as a part, so each can go straight to
// the services that take one.
void push_object(lua_State* L, uint64_t id) {
    static TSUCHI_Object actors{}, parts{};
    tsuchi_lua::push_handle(L, belongs(L, id, actors, "Actor") ? "actor" : belongs(L, id, parts, "ActorComponent") ? "component" : "object", id);
}

// Pushes the value at the path. TSUCHI_OK with one value pushed, or a failure with nothing pushed.
TSUCHI_Result push_value(const Target& target, Path& path, int depth) {
    lua_State* L = target.L;
    TSUCHI_Value value{};
    const TSUCHI_Result result = target.get(path.text, &value);
    if (result != TSUCHI_OK) return result;
    if (depth > kMaxDepth || !lua_checkstack(L, 8)) return TSUCHI_UNSUPPORTED;
    const size_t here = path.length;
    switch (value.kind) {
    case TSUCHI_VALUE_NONE: lua_pushnil(L); return TSUCHI_OK;
    case TSUCHI_VALUE_BOOLEAN: lua_pushboolean(L, value.as.boolean); return TSUCHI_OK;
    case TSUCHI_VALUE_INTEGER: lua_pushinteger(L, static_cast<lua_Integer>(value.as.integer)); return TSUCHI_OK;
    case TSUCHI_VALUE_NUMBER: lua_pushnumber(L, value.as.number); return TSUCHI_OK;
    case TSUCHI_VALUE_OBJECT: push_object(L, value.as.object.id); return TSUCHI_OK;
    case TSUCHI_VALUE_TEXT: {
        luaL_Buffer buffer;
        char* text = luaL_buffinitsize(L, &buffer, value.count ? value.count : 1);
        uint64_t needed = 0;
        const TSUCHI_Result read = target.get_text(path.text, text, value.count, &needed);
        luaL_pushresultsize(&buffer, read == TSUCHI_OK && needed ? size_t(needed - 1) : 0);
        if (read != TSUCHI_OK) lua_pop(L, 1);
        return read;
    }
    case TSUCHI_VALUE_STRUCT: {
        lua_newtable(L);
        char small[256];
        for (uint32_t index = 0;; ++index) {
            const char* name = read_name(L, small, sizeof small, [&](char* buffer, uint64_t capacity, uint64_t* needed) {
                return target.field_at(path.text, index, buffer, capacity, needed);
            });
            // A part that cannot be read is left out.
            if (name && path.field(name) && push_value(target, path, depth + 1) == TSUCHI_OK) lua_setfield(L, -3, name);
            path.back_to(here);
            lua_pop(L, 1);
            if (!name) break;
        }
        return TSUCHI_OK;
    }
    case TSUCHI_VALUE_ARRAY: {
        lua_createtable(L, static_cast<int>(value.count), 0);
        for (uint32_t index = 0; index < value.count; ++index) {
            const TSUCHI_Result element = path.element(index) ? push_value(target, path, depth + 1) : TSUCHI_INVALID;
            path.back_to(here);
            if (element != TSUCHI_OK) { // a list with a hole in it would lie about its length
                lua_pop(L, 1);
                return element;
            }
            lua_rawseti(L, -2, static_cast<lua_Integer>(index) + 1);
        }
        return TSUCHI_OK;
    }
    }
    return TSUCHI_UNSUPPORTED;
}

// Copy every input before native validation or mutation. Lua table iteration and
// raw array reads do not run metamethods, and the tree keeps all copied text.
using tsuchi::ScriptWrite;
using tsuchi::ScriptWriteKind;
using tsuchi::ScriptWriteTree;
TSUCHI_Result materialize(lua_State* L, int index, int depth, ScriptWriteTree& tree,
                          ScriptWrite** out, const void** ancestors, int max_depth = kMaxDepth) {
    if (depth > max_depth || !lua_checkstack(L, 8)) return TSUCHI_UNSUPPORTED;
    ScriptWrite* node = tree.add(ScriptWriteKind::value);
    if (!node) return TSUCHI_NO_MEMORY;
    *out = node;
    TSUCHI_Value& value = node->value;
    switch (lua_type(L, index)) {
    case LUA_TNIL: value.kind = TSUCHI_VALUE_NONE; return TSUCHI_OK;
    case LUA_TBOOLEAN:
        value.kind = TSUCHI_VALUE_BOOLEAN;
        value.as.boolean = lua_toboolean(L, index) != 0;
        return TSUCHI_OK;
    case LUA_TNUMBER:
        if (lua_isinteger(L, index)) {
            value.kind = TSUCHI_VALUE_INTEGER;
            value.as.integer = lua_tointeger(L, index);
        } else {
            value.kind = TSUCHI_VALUE_NUMBER;
            value.as.number = lua_tonumber(L, index);
        }
        return TSUCHI_OK;
    case LUA_TSTRING: {
        size_t length = 0;
        const char* text = lua_tolstring(L, index, &length);
        if (strlen(text) != length) return TSUCHI_INVALID;
        node->kind = ScriptWriteKind::text;
        return node->text.set(text) ? TSUCHI_OK : TSUCHI_NO_MEMORY;
    }
    case LUA_TUSERDATA:
        value.kind = TSUCHI_VALUE_OBJECT;
        return tsuchi_lua::to_handle(L, index, &value.as.object.id) ? TSUCHI_OK : TSUCHI_INVALID;
    case LUA_TTABLE: break;
    default: return TSUCHI_INVALID;
    }
    index = lua_absindex(L, index);
    const void* identity = lua_topointer(L, index);
    for (int i = 0; i < depth; ++i) if (ancestors[i] == identity) return TSUCHI_UNSUPPORTED;
    ancestors[depth] = identity;
    const lua_Unsigned count = lua_rawlen(L, index);
    if (count > tsuchi::kMaxScriptArrayCount) return TSUCHI_CAPACITY;
    bool strings = false, integers = false;
    uint64_t entries = 0;
    lua_pushnil(L);
    while (lua_next(L, index)) {
        ++entries;
        if (lua_type(L, -2) == LUA_TSTRING) strings = true;
        else if (lua_isinteger(L, -2) && lua_tointeger(L, -2) > 0) {
            if (static_cast<lua_Unsigned>(lua_tointeger(L, -2)) > tsuchi::kMaxScriptArrayCount) {
                lua_pop(L, 2); return TSUCHI_CAPACITY;
            }
            if (static_cast<lua_Unsigned>(lua_tointeger(L, -2)) > count) {
                lua_pop(L, 2); return TSUCHI_INVALID;
            }
            integers = true;
        }
        else { lua_pop(L, 2); return TSUCHI_INVALID; }
        lua_pop(L, 1);
    }
    if (strings && integers) return TSUCHI_INVALID;
    if (!entries) { node->kind = ScriptWriteKind::empty; return TSUCHI_OK; }
    node->kind = integers ? ScriptWriteKind::array : ScriptWriteKind::structure;
    if (integers) {
        if (entries != count) return TSUCHI_INVALID;
        for (lua_Unsigned i = 0; i < count; ++i) {
            lua_rawgeti(L, index, static_cast<lua_Integer>(i) + 1);
            ScriptWrite* child = nullptr;
            TSUCHI_Result result = materialize(L, -1, depth + 1, tree, &child, ancestors, max_depth);
            lua_pop(L, 1);
            if (result != TSUCHI_OK) return result;
            if (!node->children.add(child)) return TSUCHI_NO_MEMORY;
        }
    } else {
        lua_pushnil(L);
        while (lua_next(L, index)) {
            size_t length = 0;
            const char* name = lua_tolstring(L, -2, &length);
            ScriptWrite* child = nullptr;
            TSUCHI_Result result = strlen(name) == length
                ? materialize(L, -1, depth + 1, tree, &child, ancestors, max_depth) : TSUCHI_INVALID;
            if (result == TSUCHI_OK && (!child->name.set(name) || !node->children.add(child))) result = TSUCHI_NO_MEMORY;
            if (result != TSUCHI_OK) { lua_pop(L, 2); return result; }
            lua_pop(L, 1);
        }
    }
    return TSUCHI_OK;
}
TSUCHI_Result apply(const Target& target, const char* path, const ScriptWrite& node) {
    if (node.kind == ScriptWriteKind::value) return target.set(path, node.value);
    if (node.kind == ScriptWriteKind::text) return target.set_text(path, node.text.get());
    return tsuchi::apply_script_write(mod(target.L), {target.object, target.call, target.row}, path, node);
}
TSUCHI_Result write_value(const Target& target, Path& path, int index, int depth) {
    ScriptWriteTree tree;
    const void* ancestors[kMaxDepth + 1]{};
    TSUCHI_Result result = materialize(target.L, index, depth, tree, &tree.root, ancestors);
    return result == TSUCHI_OK ? apply(target, path.text, *tree.root) : result;
}

} // namespace

int tsuchi_lua_reflection_get(lua_State* L) {
    const Target target{L, {tsuchi_lua::check_handle(L, 1, "object", false)}, {}};
    Path path;
    if (!path.set(luaL_checkstring(L, 2))) return tsuchi_lua::fail(L, TSUCHI_INVALID);
    const TSUCHI_Result result = push_value(target, path, 0);
    return result == TSUCHI_OK ? 1 : tsuchi_lua::fail(L, result);
}

int tsuchi_lua_reflection_set(lua_State* L) {
    const Target target{L, {tsuchi_lua::check_handle(L, 1, "object", false)}, {}};
    Path path;
    if (!path.set(luaL_checkstring(L, 2))) return tsuchi_lua::fail(L, TSUCHI_INVALID);
    luaL_checkany(L, 3);
    const TSUCHI_Result result = write_value(target, path, 3, 0);
    if (result != TSUCHI_OK) return tsuchi_lua::fail(L, result);
    lua_pushboolean(L, 1);
    return 1;
}

namespace {
bool held_scalar(lua_State* L, int index, TSUCHI_Value& value) {
    luaL_checkany(L, index);
    if (lua_type(L, index) == LUA_TBOOLEAN) {
        value.kind = TSUCHI_VALUE_BOOLEAN; value.as.boolean = lua_toboolean(L, index); return true;
    }
    if (lua_type(L, index) != LUA_TNUMBER) return false;
    if (lua_isinteger(L, index)) { value.kind = TSUCHI_VALUE_INTEGER; value.as.integer = lua_tointeger(L, index); }
    else { value.kind = TSUCHI_VALUE_NUMBER; value.as.number = lua_tonumber(L, index); }
    return true;
}
// A float with a whole value is an integer here, as it is to settings.set_integer.
bool setting_scalar(lua_State* L, int index, TSUCHI_Value& value) {
    if (!held_scalar(L, index, value)) return false;
    int whole = 0; const lua_Integer integer = lua_tointegerx(L, index, &whole);
    if (value.kind == TSUCHI_VALUE_NUMBER && whole) { value.kind = TSUCHI_VALUE_INTEGER; value.as.integer = integer; }
    return true;
}
}
int tsuchi_lua_reflection_hold_field(lua_State* L) {
    const TSUCHI_Object object{tsuchi_lua::check_handle(L, 1, "object", false)};
    size_t path_size = 0, identity_size = 0;
    const char* path = luaL_checklstring(L, 2, &path_size);
    TSUCHI_Value value{};
    if (!held_scalar(L, 3, value)) return tsuchi_lua::fail(L, TSUCHI_INVALID);
    const char* identity = luaL_checklstring(L, 4, &identity_size);
    if (strlen(path) != path_size || strlen(identity) != identity_size) return tsuchi_lua::fail(L, TSUCHI_INVALID);
    TSUCHI_FieldHold hold{};
    const auto result = host(L)->reflection->hold_field(mod(L), object, path, &value, identity, &hold);
    if (result != TSUCHI_OK) return tsuchi_lua::fail(L, result);
    tsuchi_lua::push_handle(L, "field_hold", hold.id); return 1;
}
int tsuchi_lua_reflection_update_field(lua_State* L) {
    const TSUCHI_FieldHold hold{tsuchi_lua::check_handle(L, 1, "field_hold", false)};
    TSUCHI_Value value{};
    if (!held_scalar(L, 2, value)) return tsuchi_lua::fail(L, TSUCHI_INVALID);
    const auto result = host(L)->reflection->update_field(mod(L), hold, &value);
    if (result != TSUCHI_OK) return tsuchi_lua::fail(L, result);
    lua_pushboolean(L, 1); return 1;
}

int tsuchi_lua_settings_hold(lua_State* L) {
    size_t bytes = 0; const char* key = luaL_checklstring(L, 1, &bytes);
    TSUCHI_Value value{};
    if (strlen(key) != bytes || !setting_scalar(L, 2, value)) return tsuchi_lua::fail(L, TSUCHI_INVALID);
    TSUCHI_SettingHold hold{};
    const auto result = host(L)->settings->hold(mod(L), key, &value, &hold);
    if (result != TSUCHI_OK) return tsuchi_lua::fail(L, result);
    tsuchi_lua::push_handle(L, "setting_hold", hold.id); return 1;
}
int tsuchi_lua_settings_update_hold(lua_State* L) {
    const TSUCHI_SettingHold hold{tsuchi_lua::check_handle(L, 1, "setting_hold", false)};
    TSUCHI_Value value{};
    if (!setting_scalar(L, 2, value)) return tsuchi_lua::fail(L, TSUCHI_INVALID);
    const auto result = host(L)->settings->update_hold(mod(L), hold, &value);
    if (result != TSUCHI_OK) return tsuchi_lua::fail(L, result);
    lua_pushboolean(L, 1); return 1;
}

int tsuchi_lua_datatable_get(lua_State* L) {
    const Target target{L, {tsuchi_lua::check_handle(L, 1, "object", false)}, {}, luaL_checkstring(L, 2)};
    Path path;
    if (!path.set(luaL_checkstring(L, 3))) return tsuchi_lua::fail(L, TSUCHI_INVALID);
    const TSUCHI_Result result = push_value(target, path, 0);
    return result == TSUCHI_OK ? 1 : tsuchi_lua::fail(L, result);
}

int tsuchi_lua_datatable_override(lua_State* L) {
    const Target target{L, {tsuchi_lua::check_handle(L, 1, "object", false)}, {}, luaL_checkstring(L, 2)};
    Path path;
    if (!path.set(luaL_checkstring(L, 3))) return tsuchi_lua::fail(L, TSUCHI_INVALID);
    luaL_checkany(L, 4);
    const TSUCHI_Result result = write_value(target, path, 4, 0);
    if (result != TSUCHI_OK) return tsuchi_lua::fail(L, result);
    lua_pushboolean(L, 1);
    return 1;
}

int tsuchi_lua_reflection_call(lua_State* L) {
    const TSUCHI_Object object{tsuchi_lua::check_handle(L, 1, "object", false)};
    const char* function = luaL_checkstring(L, 2);
    if (!lua_isnoneornil(L, 3)) luaL_checktype(L, 3, LUA_TTABLE);
    const TSUCHI_ReflectionService& api = *host(L)->reflection;
    ScriptWriteTree arguments;
    const void* ancestors[kMaxDepth + 2]{};
    TSUCHI_Result result = TSUCHI_OK;
    if (lua_istable(L, 3)) {
        result = materialize(L, 3, 0, arguments, &arguments.root, ancestors, kMaxDepth + 1);
        if (result == TSUCHI_OK && arguments.root->kind != ScriptWriteKind::structure &&
            arguments.root->kind != ScriptWriteKind::empty) result = TSUCHI_INVALID;
        if (result != TSUCHI_OK) return tsuchi_lua::fail(L, result);
    }
    Target target{L, object, {}};
    result = api.begin_call(mod(L), object, function, &target.call);
    if (result != TSUCHI_OK) return tsuchi_lua::fail(L, result);

    Path path;
    if (arguments.root) {
        for (uint32_t i = 0; i < arguments.root->children.count && result == TSUCHI_OK; ++i) {
            const ScriptWrite& child = *arguments.root->children[i];
            result = path.set(child.name.get()) ? apply(target, path.text, child) : TSUCHI_INVALID;
        }
    }
    if (result == TSUCHI_OK) result = api.invoke(mod(L), target.call);
    if (result == TSUCHI_OK) {
        path.set("ReturnValue");
        const TSUCHI_Result returned = push_value(target, path, 0);
        if (returned == TSUCHI_NOT_FOUND) lua_pushnil(L); // the function has no result
        else result = returned;
    }
    if (result == TSUCHI_OK) {
        lua_newtable(L); // the arguments the function wrote
        char small[256];
        for (uint32_t index = 0;; ++index) {
            const char* name = read_name(L, small, sizeof small, [&](char* buffer, uint64_t capacity, uint64_t* needed) {
                return api.call_output_at(mod(L), target.call, index, buffer, capacity, needed);
            });
            if (name && strcmp(name, "ReturnValue") && path.set(name) && push_value(target, path, 0) == TSUCHI_OK) lua_setfield(L, -3, name);
            lua_pop(L, 1);
            if (!name) break;
        }
    }
    api.end_call(mod(L), target.call);
    return result == TSUCHI_OK ? 2 : tsuchi_lua::fail(L, result);
}
