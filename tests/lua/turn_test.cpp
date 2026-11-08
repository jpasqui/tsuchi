// Checks that a call into a script that comes while the script is already running continues the turn it's in.
// The handlers here call back into the runtime through `fire`, the way a hook reached from a script's own call
// will. The count hook does what the mod runtime's does (runtime/lua/mods/script_mods.cpp).
#include "tsuchi_lua.h"

#include <cstdio>
#include <cstring>
extern "C" {
#include "lualib.h"
}

namespace {

tsuchi_lua::Budget used;
tsuchi_lua::Listener* listeners[4];
unsigned listener_count = 0, errors = 0;
char last_error[256];

void look(lua_State* L, lua_Debug*) {
    tsuchi_lua::Budget* const budget = tsuchi_lua::budget(L);
    if (!budget) return;
    const int every = lua_gethookcount(L);
    budget->run += uint64_t(every);
    if (budget->run > budget->limit) {
        if (every != 1) lua_sethook(L, look, LUA_MASKCOUNT, 1);
        luaL_error(L, "it ran too long without giving the game its turn back");
    }
    if (every == 1) lua_sethook(L, look, LUA_MASKCOUNT, 1000);
}

void error(const char* text) {
    ++errors;
    std::snprintf(last_error, sizeof last_error, "%s", text);
}

int call_handler(lua_State* L) {
    lua_pushvalue(L, 2);
    lua_call(L, 0, 0);
    return 0;
}

// subscribe(fn): a listener whose number is its order of subscription, from 1.
int subscribe(lua_State* L) {
    if (listener_count >= 4) return luaL_error(L, "too many listeners");
    tsuchi_lua::Listener* const listener = tsuchi_lua::listen(L, 1);
    listeners[listener_count++] = listener;
    return tsuchi_lua::subscribed(L, listener, TSUCHI_OK, TSUCHI_Subscription{listener_count});
}

// fire(n): calls listener n's handler from native code, as an event or a hook does.
int fire(lua_State* L) {
    const lua_Integer number = luaL_checkinteger(L, 1);
    if (number < 1 || number > lua_Integer(listener_count)) return luaL_error(L, "no such listener");
    tsuchi_lua::deliver(listeners[number - 1], call_handler, nullptr);
    return 0;
}

int turn_used(lua_State* L) {
    lua_pushinteger(L, lua_Integer(used.run));
    return 1;
}

bool global_true(lua_State* L, const char* name) {
    lua_getglobal(L, name);
    const bool value = lua_toboolean(L, -1);
    lua_pop(L, 1);
    return value;
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 2) return 2;
    TSUCHI_Host host{};
    lua_State* L = luaL_newstate();
    luaL_openlibs(L);
    tsuchi_lua::set_budget(L, &used);
    tsuchi_lua::set_error_handler(error);
    tsuchi_lua::open(L, reinterpret_cast<TSUCHI_Mod*>(uintptr_t(1)), &host);
    lua_pushcfunction(L, subscribe);
    lua_setglobal(L, "subscribe");
    lua_pushcfunction(L, fire);
    lua_setglobal(L, "fire");
    lua_pushcfunction(L, turn_used);
    lua_setglobal(L, "turn_used");
    if (luaL_dofile(L, argv[1]) != LUA_OK) {
        std::fprintf(stderr, "%s\n", lua_tostring(L, -1));
        return 1;
    }
    lua_sethook(L, look, LUA_MASKCOUNT, 1000);
    bool passed = true;

    // 1. A nested call doesn't reset the count of the turn it continues.
    tsuchi_lua::deliver(listeners[0], call_handler, nullptr);
    if (errors || !global_true(L, "nested_kept_count")) {
        std::fprintf(stderr, "nested call reset the turn (errors=%u: %s)\n", errors, last_error);
        passed = false;
    }

    // 2. A nested handler that uses up the shared turn stops the outer handler too, and both are switched off.
    errors = 0;
    tsuchi_lua::deliver(listeners[2], call_handler, nullptr);
    if (global_true(L, "outer_finished")) {
        std::fprintf(stderr, "the outer handler got a new allowance after the nested handler's error\n");
        passed = false;
    }
    if (errors != 2) {
        std::fprintf(stderr, "expected 2 errors, got %u\n", errors);
        passed = false;
    }

    // 3. The next call from the framework begins a turn of its own.
    errors = 0;
    tsuchi_lua::deliver(listeners[0], call_handler, nullptr);
    if (errors || !global_true(L, "nested_kept_count")) {
        std::fprintf(stderr, "a new outermost call didn't begin a fresh turn (errors=%u: %s)\n", errors, last_error);
        passed = false;
    }
    lua_close(L);
    if (!passed) return 1;
    std::puts("lua nested turns passed");
    return 0;
}
