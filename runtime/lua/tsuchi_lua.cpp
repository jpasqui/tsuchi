#include "tsuchi_lua.h"

#include <stdio.h>
#include <string.h>

namespace tsuchi_lua {
namespace {

struct State {
    TSUCHI_Mod* mod;
    const TSUCHI_Host* host;
};
struct Handle {
    uint64_t id;
};
const char kStateKey = 0;
const char kBudgetKey = 0;
const char* const kSubscription = "tsuchi.subscription";
const char* const kIsHandle = "__tsuchi_handle"; // set only in engine object handle metatables

void (*report_error)(const char*) = nullptr;

State* state(lua_State* L) {
    lua_rawgetp(L, LUA_REGISTRYINDEX, &kStateKey);
    auto* s = static_cast<State*>(lua_touserdata(L, -1));
    lua_pop(L, 1);
    return s;
}

void type_name(char* out, size_t size, const char* kind) { snprintf(out, size, "tsuchi.%s", kind); }
void cache_name(char* out, size_t size, const char* kind) { snprintf(out, size, "tsuchi.cache.%s", kind); }

int handle_tostring(lua_State* L) {
    const auto* h = static_cast<const Handle*>(lua_touserdata(L, 1));
    luaL_getmetafield(L, 1, "__name");
    lua_pushfstring(L, "%s: %I", lua_tostring(L, -1), static_cast<lua_Integer>(h->id));
    return 1;
}

// Leaves the kind's metatable and its cache (weak values, keyed by id) on the stack.
void handle_tables(lua_State* L, const char* kind) {
    char name[64];
    type_name(name, sizeof name, kind);
    if (luaL_newmetatable(L, name)) {
        lua_pushcfunction(L, handle_tostring);
        lua_setfield(L, -2, "__tostring");
        lua_pushboolean(L, !strcmp(kind, "object") || !strcmp(kind, "actor") || !strcmp(kind, "component"));
        lua_setfield(L, -2, kIsHandle);
    }
    cache_name(name, sizeof name, kind);
    if (luaL_newmetatable(L, name)) {
        lua_pushliteral(L, "v");
        lua_setfield(L, -2, "__mode");
        lua_pushvalue(L, -1);
        lua_setmetatable(L, -2);
    }
}

} // namespace

struct Listener {
    lua_State* L;       // the main thread: events arrive outside any coroutine
    int function = LUA_NOREF;
    int self = LUA_NOREF; // keeps the subscription alive while it is live
    TSUCHI_Subscription subscription{};
    bool live = false;
};

namespace {

void release(Listener* listener) {
    luaL_unref(listener->L, LUA_REGISTRYINDEX, listener->function);
    luaL_unref(listener->L, LUA_REGISTRYINDEX, listener->self);
    listener->function = listener->self = LUA_NOREF;
    listener->live = false;
}

int subscription_cancel(lua_State* L) {
    auto* listener = static_cast<Listener*>(luaL_checkudata(L, 1, kSubscription));
    if (!listener->live) return fail(L, TSUCHI_NOT_FOUND);
    const TSUCHI_Result result = host(L)->cancel(mod(L), listener->subscription);
    release(listener);
    if (result != TSUCHI_OK) return fail(L, result);
    lua_pushboolean(L, 1);
    return 1;
}

} // namespace

void set_error_handler(void (*report)(const char*)) { report_error = report; }

TSUCHI_Mod* mod(lua_State* L) { return state(L)->mod; }
const TSUCHI_Host* host(lua_State* L) { return state(L)->host; }

void open(lua_State* L, TSUCHI_Mod* mod, const TSUCHI_Host* host) {
    auto* s = static_cast<State*>(lua_newuserdatauv(L, sizeof(State), 0));
    s->mod = mod;
    s->host = host;
    lua_rawsetp(L, LUA_REGISTRYINDEX, &kStateKey);
    if (luaL_newmetatable(L, kSubscription)) {
        lua_createtable(L, 0, 1);
        lua_pushcfunction(L, subscription_cancel);
        lua_setfield(L, -2, "cancel");
        lua_setfield(L, -2, "__index");
    }
    lua_pop(L, 1);
    open_services(L);
}

int fail(lua_State* L, TSUCHI_Result result) {
    lua_pushnil(L);
    lua_pushstring(L, result_name(result));
    return 2;
}

void deprecated(lua_State* L, const char* what, unsigned version, const char* use) {
    luaL_getsubtable(L, LUA_REGISTRYINDEX, "tsuchi.deprecated"); // the names this state has been told of
    const bool told = lua_getfield(L, -1, what) != LUA_TNIL;
    lua_pop(L, 1);
    if (!told) {
        lua_pushboolean(L, 1);
        lua_setfield(L, -2, what);
    }
    lua_pop(L, 1);
    if (told) return;
    char line[256];
    snprintf(line, sizeof line, "%s is deprecated since version %u of the framework: use %s", what, version, use);
    const TSUCHI_Host* const to = host(L);
    if (to->mod && to->mod->log) to->mod->log(mod(L), line);
}

bool to_handle(lua_State* L, int index, uint64_t* id) {
    if (lua_type(L, index) != LUA_TUSERDATA || luaL_getmetafield(L, index, kIsHandle) == LUA_TNIL) return false;
    const bool object = lua_toboolean(L, -1);
    lua_pop(L, 1);
    if (!object) return false;
    *id = static_cast<const Handle*>(lua_touserdata(L, index))->id;
    return true;
}

uint64_t check_handle(lua_State* L, int index, const char* kind, bool optional) {
    if (optional && lua_isnoneornil(L, index)) return 0;
    if (!strcmp(kind, "object")) {
        uint64_t id = 0;
        if (!to_handle(L, index, &id)) luaL_typeerror(L, index, "tsuchi.object");
        return id;
    }
    char name[64];
    type_name(name, sizeof name, kind);
    return static_cast<const Handle*>(luaL_checkudata(L, index, name))->id;
}

void push_handle(lua_State* L, const char* kind, uint64_t id) {
    if (!id) {
        lua_pushnil(L);
        return;
    }
    handle_tables(L, kind);                                         // metatable, cache
    if (lua_rawgeti(L, -1, static_cast<lua_Integer>(id)) == LUA_TUSERDATA) {
        lua_replace(L, -3);
        lua_pop(L, 1);
        return;
    }
    lua_pop(L, 1);
    auto* h = static_cast<Handle*>(lua_newuserdatauv(L, sizeof(Handle), 0));
    h->id = id;
    lua_pushvalue(L, -3);
    lua_setmetatable(L, -2);
    lua_pushvalue(L, -1);
    lua_rawseti(L, -3, static_cast<lua_Integer>(id));               // cache[id] = handle
    lua_replace(L, -3);
    lua_pop(L, 1);
}

TSUCHI_Vec3 check_vec3(lua_State* L, int index) {
    luaL_checktype(L, index, LUA_TTABLE);
    TSUCHI_Vec3 value{};
    float* const parts[] = {&value.x, &value.y, &value.z};
    const char* const names[] = {"x", "y", "z"};
    for (int i = 0; i < 3; ++i) {
        if (lua_getfield(L, index, names[i]) != LUA_TNUMBER) luaL_argerror(L, index, "a vector is a table with x, y and z");
        *parts[i] = static_cast<float>(lua_tonumber(L, -1));
        lua_pop(L, 1);
    }
    return value;
}

TSUCHI_Rotation check_rotation(lua_State* L, int index) {
    luaL_checktype(L, index, LUA_TTABLE);
    TSUCHI_Rotation value{};
    float* const parts[] = {&value.pitch, &value.yaw, &value.roll};
    const char* const names[] = {"pitch", "yaw", "roll"};
    for (int i = 0; i < 3; ++i) {
        if (lua_getfield(L, index, names[i]) != LUA_TNUMBER) luaL_argerror(L, index, "a rotation is a table with pitch, yaw and roll");
        *parts[i] = static_cast<float>(lua_tonumber(L, -1));
        lua_pop(L, 1);
    }
    return value;
}

void push_rotation(lua_State* L, const TSUCHI_Rotation& value) {
    lua_createtable(L, 0, 3);
    lua_pushnumber(L, value.pitch);
    lua_setfield(L, -2, "pitch");
    lua_pushnumber(L, value.yaw);
    lua_setfield(L, -2, "yaw");
    lua_pushnumber(L, value.roll);
    lua_setfield(L, -2, "roll");
}

void push_vec3(lua_State* L, const TSUCHI_Vec3& value) {
    lua_createtable(L, 0, 3);
    lua_pushnumber(L, value.x);
    lua_setfield(L, -2, "x");
    lua_pushnumber(L, value.y);
    lua_setfield(L, -2, "y");
    lua_pushnumber(L, value.z);
    lua_setfield(L, -2, "z");
}

uint32_t check_flags(lua_State* L, int index, const char* const names[], const uint32_t values[]) {
    if (lua_isnoneornil(L, index)) return 0;
    luaL_checktype(L, index, LUA_TTABLE);
    index = lua_absindex(L, index);
    uint32_t bits = 0;
    lua_pushnil(L);
    while (lua_next(L, index)) {
        const char* key = lua_type(L, -2) == LUA_TSTRING ? lua_tostring(L, -2) : nullptr;
        int found = -1;
        for (int i = 0; key && names[i]; ++i)
            if (!strcmp(key, names[i])) found = i;
        if (found < 0) luaL_argerror(L, index, "unknown flag");
        if (lua_toboolean(L, -1)) bits |= values[found];
        lua_pop(L, 1);
    }
    return bits;
}

void push_unnamed(lua_State* L, uint32_t value) { lua_pushfstring(L, "#%I", static_cast<lua_Integer>(value)); }

void push_flags(lua_State* L, uint32_t bits, const char* const names[], const uint32_t values[]) {
    lua_newtable(L);
    for (int i = 0; names[i]; ++i) {
        if (!(bits & values[i])) continue;
        lua_pushboolean(L, 1);
        lua_setfield(L, -2, names[i]);
        bits &= ~values[i];
    }
    // A bit the game set that has no name is told like any value without one, so it is not lost.
    for (uint32_t bit = 1; bits; bit <<= 1) {
        if (!(bits & bit)) continue;
        push_unnamed(L, bit);
        lua_pushboolean(L, 1);
        lua_settable(L, -3);
        bits &= ~bit;
    }
}

Listener* listen(lua_State* L, int function_index) {
    luaL_checktype(L, function_index, LUA_TFUNCTION);
    auto* listener = static_cast<Listener*>(lua_newuserdatauv(L, sizeof(Listener), 0));
    *listener = Listener{};
    luaL_setmetatable(L, kSubscription);
    lua_rawgeti(L, LUA_REGISTRYINDEX, LUA_RIDX_MAINTHREAD);
    listener->L = lua_tothread(L, -1);
    lua_pop(L, 1);
    lua_pushvalue(L, function_index);
    listener->function = luaL_ref(L, LUA_REGISTRYINDEX);
    lua_pushvalue(L, -1);
    listener->self = luaL_ref(L, LUA_REGISTRYINDEX);
    return listener;
}

int subscribed(lua_State* L, Listener* listener, TSUCHI_Result result, TSUCHI_Subscription subscription) {
    if (result != TSUCHI_OK) {
        release(listener);
        return fail(L, result);
    }
    listener->subscription = subscription;
    listener->live = true;
    return 1; // the subscription object listen() left on the stack
}

namespace {
int log_error(lua_State* L) {
    lua_pushfstring(L, "error: %s", luaL_tolstring(L, 1, nullptr)); // an error value need not be a string
    host(L)->mod->log(mod(L), lua_tostring(L, -1));
    return 0;
}
} // namespace

Budget* budget(lua_State* L) {
    lua_rawgetp(L, LUA_REGISTRYINDEX, &kBudgetKey);
    auto* const found = static_cast<Budget*>(lua_touserdata(L, -1));
    lua_pop(L, 1);
    return found;
}

void set_budget(lua_State* L, Budget* budget) {
    lua_pushlightuserdata(L, budget);
    lua_rawsetp(L, LUA_REGISTRYINDEX, &kBudgetKey);
}

Turn::Turn(lua_State* L, uint64_t limit) : L_(L), used_(budget(L)) {
    if (!used_ || used_->depth++) return;
    outermost_ = true;
    used_->run = 0;
    used_->limit = limit;
}

Turn::~Turn() {
    if (used_ && !--used_->depth) {
        used_->limit = kTurn;
        if (!used_->hook_pins && used_->close_pending) used_->close_pending(L_);
    }
}

void pin_hook(lua_State* L) { if (Budget* used = budget(L)) ++used->hook_pins; }
void unpin_hook(lua_State* L) {
    Budget* used = budget(L);
    if (used && used->hook_pins && !--used->hook_pins && used->close_pending && !used->depth)
        used->close_pending(L);
}
void close_when_unused(lua_State* L, void (*close)(lua_State*)) {
    Budget* used = budget(L);
    if (used && (used->hook_pins || used->depth)) used->close_pending = close;
    else close(L);
}
void report_hook_error(lua_State* L, const char* message) {
    if (report_error) report_error(message);
    else if (host(L)->mod && host(L)->mod->log) host(L)->mod->log(mod(L), message);
}

bool turn_over(lua_State* L) {
    const Budget* const used = budget(L);
    return used && used->run > used->limit;
}

lua_Integer check_integer(lua_State* L, int index, lua_Integer low, lua_Integer high) {
    const lua_Integer value = luaL_checkinteger(L, index);
    luaL_argcheck(L, value >= low && value <= high, index, "the number is out of range");
    return value;
}

static_assert(sizeof(lua_Integer) == sizeof(uint64_t) && LUA_MAXINTEGER == INT64_MAX && LUA_MININTEGER == INT64_MIN,
              "Lua integers must carry exactly 64 bits");

uint64_t check_u64(lua_State* L, int index) {
    // Conversion from signed to unsigned is defined modulo 2^64.
    if (lua_isinteger(L, index)) return static_cast<uint64_t>(lua_tointeger(L, index));
    // A numeric string is refused: it can name an ID that a double would round.
    if (lua_type(L, index) != LUA_TNUMBER) luaL_typeerror(L, index, "number");
    // Every whole number below 2^53 is exact as a float. Past that, a float
    // may already be a rounded ID. NaN fails both comparisons.
    const lua_Number number = lua_tonumber(L, index);
    luaL_argcheck(L, number >= 0 && number < 9007199254740992.0, index, "the number is out of range");
    const uint64_t value = static_cast<uint64_t>(number);
    luaL_argcheck(L, static_cast<lua_Number>(value) == number, index, "number has no integer representation");
    return value;
}

void push_u64(lua_State* L, uint64_t value) {
    // Both casts fit the signed range. Subtraction forms the negative value
    // without an overflowing cast from a high unsigned integer.
    const lua_Integer bits = value <= static_cast<uint64_t>(LUA_MAXINTEGER)
                                 ? static_cast<lua_Integer>(value)
                                 : -1 - static_cast<lua_Integer>(UINT64_MAX - value);
    lua_pushinteger(L, bits);
}

void deliver(Listener* listener, lua_CFunction call, const void* event) {
    if (!listener->live) return;
    lua_State* L = listener->L;
    if (!lua_checkstack(L, 3)) return; // no room to call anything: the event is lost
    Turn turn(L);
    lua_pushcfunction(L, call);
    lua_pushlightuserdata(L, const_cast<void*>(event));
    lua_rawgeti(L, LUA_REGISTRYINDEX, listener->function);
    if (lua_pcall(L, 2, 0, 0) == LUA_OK) return;
    // A handler that did not come back within its turn is not called again: it would hold the game still each time.
    if (turn_over(L)) listener->live = false;
    // What follows is the framework's own, and short, so it runs on a fresh count. Afterward the count goes back to
    // what the handler used, so a turn that this call continues isn't refilled by the handler's error.
    Budget* const used = budget(L);
    const uint64_t spent = used ? used->run : 0;
    if (used) used->run = 0;
    if (report_error) {
        report_error(lua_type(L, -1) == LUA_TSTRING ? lua_tostring(L, -1) : "an error that is no text");
        lua_pop(L, 1);
    } else {
        // Into the log, under the mod's name. Making the line can itself fail for want of memory, so it is done
        // where that is caught; then the line is lost and the handler's error with it.
        lua_pushcfunction(L, log_error);
        lua_insert(L, -2);
        if (lua_pcall(L, 1, 0, 0) != LUA_OK) lua_pop(L, 1);
    }
    if (used) used->run = spent;
}

} // namespace tsuchi_lua
