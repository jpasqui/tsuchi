// Runs a script against the made-up world through the reflect service's script
// bindings. Exits non-zero when the script fails or leaves something behind.
#include <cstdio>
#include <initializer_list>

#include "host.h"
#include "objects.h"
#include "tsuchi_lua.h"
#include "reflect_world.h"
#include "reflection.h"
#include "registry.h"

extern "C" {
#include "lualib.h"
}

namespace {

int fail(const char* what) {
    std::fprintf(stderr, "%s\n", what);
    return 1;
}

void global_handle(lua_State* L, const char* name, const char* kind, uint64_t object) {
    tsuchi_lua::push_handle(L, kind, tsuchi::game::to_handle(object));
    lua_setglobal(L, name);
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 2) return 2;
    build_world();
    tsuchi::reflect_start();
    TSUCHI_Mod* mod = tsuchi::mod_create("reflection.script");
    lua_State* L = luaL_newstate();
    luaL_openlibs(L);
    tsuchi_lua::open(L, mod, &tsuchi::host);
    global_handle(L, "red", "actor", w.red);
    global_handle(L, "body", "object", w.body);
    global_handle(L, "spare", "object", w.spare);
    global_handle(L, "wordy", "object", w.wordy);

    static_assert(sizeof(TSUCHI_Dialog) == 8 && sizeof(TSUCHI_PauseHold) == 8 &&
                  sizeof(TSUCHI_HookId) == 8 && sizeof(TSUCHI_PreparedCall) == 8 && sizeof(TSUCHI_Hook) == 16,
                  "resource handles retain their 64-bit ABI storage");
    for (const char* kind : {"dialog", "pause_hold", "hook_id", "prepared_call"}) {
        tsuchi_lua::push_handle(L, kind, tsuchi::game::to_handle(w.red));
        lua_setglobal(L, "resource");
        tsuchi_lua::push_handle(L, kind, UINT64_C(9007199254740993));
        lua_setglobal(L, "large");
        tsuchi_lua::push_handle(L, kind, UINT64_C(9007199254740993));
        lua_setglobal(L, "same_large");
        tsuchi_lua::push_handle(L, kind, UINT64_C(9007199254740992));
        lua_setglobal(L, "neighbor");
        if (luaL_dostring(L,
                "assert(type(resource) == 'userdata') "
                "assert(large == same_large and large ~= neighbor) "
                "assert(not pcall(reflection.name, resource)) "
                "local result, why = reflection.set(red, 'Root', resource) "
                "assert(result == nil and why == 'invalid')") != LUA_OK)
            return fail(luaL_tolstring(L, -1, nullptr));
        lua_getglobal(L, "resource");
        uint64_t id = 0;
        if (tsuchi_lua::to_handle(L, -1, &id)) return fail("a resource handle became an engine object");
        lua_pop(L, 1);
    }

    const fake::Engine before = fake::engine();
    if (luaL_dofile(L, argv[1]) != LUA_OK) return fail(luaL_tolstring(L, -1, nullptr));
    // Every call the script made was ended, whether it worked or not.
    if (tsuchi::owned_count(mod) != 0) return fail("the script left a call open");
    if (fake::engine().initialized == before.initialized) return fail("the script made no calls");
    if (w.heard != u"Captain") return fail("a string argument did not reach the function");
    lua_close(L);
    return 0;
}
