// hook.engine_function for Lua: the table each handler gets, and the changes it makes, on top of the engine
// function hooks in framework/core/engine_hooks.h and the script hook runner in script_hooks.cpp.
#include <stdio.h>

#include "tsuchi_lua.h"
#include "dynamic_hooks.h"
#include "engine_hooks.h"
#include "registry.h"

namespace {
using namespace tsuchi;
using namespace tsuchi_lua;

struct Hook {
    lua_State* L = nullptr;
    int before = LUA_NOREF, after = LUA_NOREF;
    bool disabled = false;
    char error[256]{};
};
Hook hooks[kEngineHooks];

constexpr uint32_t kWrites = 64;
// One handler's call. The changes it makes are copied here while it runs, and written once it has returned.
struct Call {
    Hook* hook = nullptr;
    const EngineCall* engine = nullptr;
    bool after = false;
    ScriptWriteTree tree;
    EngineWrite writes[kWrites + 1]{};
    uint32_t count = 0;
};

lua_State* main_state(lua_State* L) {
    lua_rawgeti(L, LUA_REGISTRYINDEX, LUA_RIDX_MAINTHREAD);
    lua_State* main = lua_tothread(L, -1);
    lua_pop(L, 1);
    return main;
}

// A name into `small` when it fits there, or into a userdata the size it needs, left on the stack. Null when
// there is no such name; then nothing is left.
template <typename Read>
const char* read_name(lua_State* L, char* small, size_t capacity, Read read) {
    uint64_t needed = 0;
    const TSUCHI_Result result = read(small, capacity, &needed);
    if (result != TSUCHI_CAPACITY) return result == TSUCHI_OK ? small : nullptr;
    char* big = static_cast<char*>(lua_newuserdatauv(L, size_t(needed), 0));
    if (read(big, needed, &needed) == TSUCHI_OK) return big;
    lua_pop(L, 1);
    return nullptr;
}

// True when two values hold the same: the same plain value or handle, or tables with the same keys and values.
bool same(lua_State* L, int a, int b, int depth) {
    a = lua_absindex(L, a);
    b = lua_absindex(L, b);
    if (lua_type(L, a) != lua_type(L, b)) return false;
    if (lua_type(L, a) != LUA_TTABLE) return lua_rawequal(L, a, b);
    if (depth > 16 || !lua_checkstack(L, 4)) return false;
    size_t keys = 0;
    lua_pushnil(L);
    while (lua_next(L, a)) {
        ++keys;
        lua_pushvalue(L, -2);
        lua_rawget(L, b);
        const bool equal = same(L, -2, -1, depth + 1);
        lua_pop(L, 2);
        if (!equal) {
            lua_pop(L, 1);
            return false;
        }
    }
    lua_pushnil(L);
    while (lua_next(L, b)) {
        lua_pop(L, 1);
        if (!keys--) {
            lua_pop(L, 1);
            return false;
        }
    }
    return !keys;
}

// Copies the value at `value` for writing to `name`, when it differs from what the call holds there now.
void note_change(lua_State* L, Call& call, const char* name, int value) {
    value = lua_absindex(L, value);
    const int top = lua_gettop(L);
    const bool readable = push_engine_value(L, *call.engine, name) == TSUCHI_OK;
    const bool unchanged = readable ? same(L, value, -1, 0) : lua_isnil(L, value);
    lua_settop(L, top);
    if (unchanged) return;
    if (call.count >= kWrites) luaL_error(L, "a hook changed more than %d values", int(kWrites));
    ScriptWrite* node = nullptr;
    const TSUCHI_Result copied = script_write(L, value, call.tree, &node);
    if (copied != TSUCHI_OK) luaL_error(L, "%s can't take that value: %s", name, result_name(copied));
    if (!node->name.set(name)) luaL_error(L, "not enough memory");
    call.writes[call.count++] = {node->name.get(), node};
}

// The handler's call, with its Call as light userdata: 1 the Call, 2 the handler, 3 `call`, 4 the names of
// the arguments, 5 `args` as it was given.
int invoke(lua_State* L) {
    Call& call = *static_cast<Call*>(lua_touserdata(L, 1));
    const EngineCall& engine = *call.engine;
    lua_rawgeti(L, LUA_REGISTRYINDEX, call.after ? call.hook->after : call.hook->before);
    lua_createtable(L, 0, 3);
    lua_newtable(L);
    lua_newtable(L);
    push_handle(L, engine_object_kind(engine.object), engine.object);
    lua_setfield(L, 3, "object");
    char small[256];
    for (uint32_t i = 0;; ++i) {
        const int top = lua_gettop(L);
        const char* name = read_name(L, small, sizeof small, [&](char* buffer, uint64_t capacity, uint64_t* needed) {
            return engine_field_at(engine, "", i, buffer, capacity, needed);
        });
        if (!name) break;
        lua_pushstring(L, name);
        lua_rawseti(L, 4, lua_Integer(i) + 1);
        // An argument that can't be read is left out, as reflection.get leaves out a struct's.
        if (push_engine_value(L, engine, name) == TSUCHI_OK) lua_setfield(L, 5, name);
        lua_settop(L, top);
    }
    lua_pushvalue(L, 5);
    lua_setfield(L, 3, "args");
    const char* result = nullptr;
    if (call.after) {
        result = read_name(L, small, sizeof small, [&](char* buffer, uint64_t capacity, uint64_t* needed) {
            return engine_result_name(engine, buffer, capacity, needed);
        });
        if (result) {
            lua_pushstring(L, result);
            result = lua_tostring(L, -1); // kept on the stack while the handler runs
            if (push_engine_value(L, engine, result) == TSUCHI_OK) lua_setfield(L, 3, "result");
        }
    }
    const int result_index = lua_gettop(L);
    lua_pushvalue(L, 2);
    lua_pushvalue(L, 3);
    lua_call(L, 1, 1);
    const int returned = lua_gettop(L);
    lua_pushstring(L, "args");
    lua_rawget(L, 3);
    const int args = lua_gettop(L);
    if (!lua_istable(L, args)) return luaL_error(L, "call.args has to stay a table");
    const lua_Unsigned count = lua_rawlen(L, 4);
    for (lua_Unsigned i = 1; i <= count; ++i) {
        lua_rawgeti(L, 4, lua_Integer(i));
        const char* name = lua_tostring(L, -1);
        lua_pushvalue(L, -1);
        lua_rawget(L, args);
        note_change(L, call, name, -1);
        lua_pop(L, 2);
    }
    if (!lua_isnil(L, returned)) {
        if (!result) return luaL_error(L, "the function has no result to replace");
        (void)result_index;
        note_change(L, call, result, returned);
    }
    return 0;
}

bool handle(Hook& hook, EngineCall& engine, bool after) {
    Call call;
    call.hook = &hook;
    call.engine = &engine;
    call.after = after;
    if (!run_hook(hook.L, invoke, &call, hook.disabled, hook.error, sizeof hook.error)) return false;
    const TSUCHI_Result written = engine_write(engine, call.writes, call.count);
    if (written == TSUCHI_OK) return true;
    snprintf(hook.error, sizeof hook.error, "a hook's changes don't fit the function: %s", result_name(written));
    return false;
}
bool before(void* user, EngineCall& call) { return handle(*static_cast<Hook*>(user), call, false); }
bool after(void* user, EngineCall& call) { return handle(*static_cast<Hook*>(user), call, true); }
bool ready(void* user) {
    const Hook& hook = *static_cast<Hook*>(user);
    return hook_ready(hook.L, hook.disabled);
}
void report(void* user) {
    Hook& hook = *static_cast<Hook*>(user);
    if (Budget* used = budget(hook.L)) used->hooks.remaining(engine_frame_number(), used->run);
    if (!hook.error[0]) return;
    if (mod_running(mod(hook.L))) report_hook_error(hook.L, hook.error);
    hook.error[0] = 0;
}
void release_hook(void* user) {
    Hook& hook = *static_cast<Hook*>(user);
    lua_State* L = hook.L;
    luaL_unref(L, LUA_REGISTRYINDEX, hook.before);
    luaL_unref(L, LUA_REGISTRYINDEX, hook.after);
    hook = Hook{};
    unpin_hook(L);
}
int push_installed(lua_State* L) {
    push_handle(L, "hook_id", check_u64(L, 1));
    return 1;
}
// Pushes the handler under `name` in the table at 2: a function, or nil. Raises for anything else.
void push_handler(lua_State* L, const char* name) {
    lua_pushstring(L, name);
    lua_rawget(L, 2);
    if (!lua_isnil(L, -1) && !lua_isfunction(L, -1)) luaL_error(L, "handlers.%s has to be a function", name);
}
} // namespace

int tsuchi_lua_hook_engine_function(lua_State* L) {
    if (!mod_usable(mod(L))) return fail(L, TSUCHI_INVALID);
    size_t length = 0;
    const char* path = luaL_checklstring(L, 1, &length);
    luaL_checktype(L, 2, LUA_TTABLE);
    if (strlen(path) != length) return fail(L, TSUCHI_INVALID);
    Hook* hook = nullptr;
    for (Hook& candidate : hooks)
        if (!candidate.L) {
            hook = &candidate;
            break;
        }
    if (!hook) return fail(L, TSUCHI_CAPACITY);
    push_handler(L, "before");
    push_handler(L, "after");
    if (lua_isnil(L, -1) && lua_isnil(L, -2)) return fail(L, TSUCHI_INVALID);
    const int after_ref = lua_isnil(L, -1) ? (lua_pop(L, 1), LUA_NOREF) : luaL_ref(L, LUA_REGISTRYINDEX);
    const int before_ref = lua_isnil(L, -1) ? (lua_pop(L, 1), LUA_NOREF) : luaL_ref(L, LUA_REGISTRYINDEX);
    *hook = Hook{};
    hook->L = main_state(L);
    hook->before = before_ref;
    hook->after = after_ref;
    pin_hook(L);
    EngineClient client;
    client.before = before_ref != LUA_NOREF ? before : nullptr;
    client.after = after_ref != LUA_NOREF ? after : nullptr;
    client.ready = ready;
    client.report = report;
    client.release = release_hook;
    uint64_t id = 0;
    const TSUCHI_Result result = engine_hook(mod(L), path, client, hook, &id);
    if (result != TSUCHI_OK) {
        release_hook(hook);
        return fail(L, result);
    }
    lua_pushcfunction(L, push_installed);
    push_u64(L, id);
    if (lua_pcall(L, 1, 1, 0) != LUA_OK) {
        engine_hook_remove(mod(L), id);
        return lua_error(L);
    }
    return 1;
}
