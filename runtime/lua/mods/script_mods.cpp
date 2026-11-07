// A mod written in Lua: its `main.lua` is run in a state of its own when the mod
// starts, with every service as a table, and the state is closed when the mod stops.
//
// Of Lua's own libraries a script has the ones that need nothing of the machine:
// the base functions, coroutines, tables, strings, math and utf8. `print` writes
// to the framework's log. `require` finds a module among the mod's own files.

#include "script_mods.h"

#include <string.h>

#include "host.h"
#include "mods.h"
#include "tsuchi_lua.h"
#include "registry.h"

extern "C" {
#include "lualib.h"
}

namespace tsuchi_lua {
namespace {

const tsuchi::Kind kScript{"lua.state"};
const char kLoaded = 0; // the registry key of the table of modules `require` has run

// Lua's memory is the framework's, up to what one mod's scripts may hold.
void* allocate(void* user, void* block, size_t old_size, size_t new_size) {
    auto* const used = static_cast<Budget*>(user);
    if (!block) old_size = 0; // for a new block Lua says what kind of thing it is for, not a size
    if (!new_size) {
        tsuchi::release(block);
        used->held -= old_size;
        return nullptr;
    }
    if (new_size > old_size && used->held + (new_size - old_size) > kScriptMemory) return nullptr;
    void* grown = tsuchi::allocate(new_size);
    if (!grown) return nullptr; // the old block is still the script's, as Lua expects
    used->held = used->held - old_size + new_size;
    if (block) {
        memcpy(grown, block, old_size < new_size ? old_size : new_size);
        tsuchi::release(block);
    }
    return grown;
}

// print(...): the values as text with tabs between them, as one line of the log.
int print(lua_State* L) {
    const int count = lua_gettop(L);
    luaL_Buffer line;
    luaL_buffinit(L, &line);
    for (int i = 1; i <= count; ++i) {
        if (i > 1) luaL_addchar(&line, '\t');
        luaL_tolstring(L, i, nullptr);
        luaL_addvalue(&line);
    }
    luaL_pushresult(&line);
    host(L)->mod->log(mod(L), lua_tostring(L, -1));
    return 0;
}

// The text of one of the mod's files, pushed as a string. False, with the reason pushed instead, when it cannot be read.
bool push_file(lua_State* L, const char* path) {
    const TSUCHI_FileService& files = *host(L)->file;
    uint64_t size = 0, got = 0;
    TSUCHI_Result result = files.size(mod(L), path, &size);
    if (result == TSUCHI_OK) {
        void* buffer = lua_newuserdatauv(L, size ? size : 1, 0); // owned by the state
        result = files.read(mod(L), path, 0, buffer, size, &got);
        if (result == TSUCHI_OK && got == size) {
            lua_pushlstring(L, static_cast<const char*>(buffer), size);
            lua_remove(L, -2);
            return true;
        }
        lua_pop(L, 1);
        if (result == TSUCHI_OK) result = TSUCHI_IO;
    }
    lua_pushfstring(L, "%s: %s", path, result_name(result));
    return false;
}

// Loads one of the mod's files as a chunk named for the mod and the file. Leaves the function, or raises.
void load_file(lua_State* L, const char* path) {
    if (!push_file(L, path)) lua_error(L);
    size_t bytes = 0;
    const char* source = lua_tolstring(L, -1, &bytes);
    char id[64] = "";
    uint64_t needed = 0;
    host(L)->mod->id(mod(L), id, sizeof id, &needed);
    lua_pushfstring(L, "@%s/%s", id, path);
    if (luaL_loadbufferx(L, source, bytes, lua_tostring(L, -1), "t") != LUA_OK) lua_error(L); // text only: no compiled chunks
    lua_remove(L, -2);
    lua_remove(L, -2);
}

// require(name): the module `name.lua` among the mod's files, with dots in the name for folders. It is run once,
// and what it returns, or true, is what every later require of it gives.
int require(lua_State* L) {
    const char* name = luaL_checkstring(L, 1);
    lua_rawgetp(L, LUA_REGISTRYINDEX, &kLoaded);
    lua_getfield(L, -1, name);
    if (!lua_isnil(L, -1)) return 1;
    lua_pop(L, 1);
    luaL_Buffer path;
    luaL_buffinit(L, &path);
    for (const char* at = name; *at; ++at) luaL_addchar(&path, *at == '.' ? '/' : *at);
    luaL_addstring(&path, ".lua");
    luaL_pushresult(&path);
    load_file(L, lua_tostring(L, -1));
    lua_pushvalue(L, 1);
    lua_call(L, 1, 1);
    if (lua_isnil(L, -1)) {
        lua_pop(L, 1);
        lua_pushboolean(L, 1);
    }
    lua_pushvalue(L, -1);
    lua_setfield(L, 2, name);
    return 1;
}

int run_main(lua_State* L) {
    load_file(L, "main.lua");
    lua_call(L, 0, 0);
    return 0;
}

void close(void* context, uint64_t) {
    auto* const L = static_cast<lua_State*>(context);
    void* used = nullptr;
    lua_getallocf(L, &used);
    lua_close(L);
    tsuchi::release(used);
}

// Looks in every so many instructions, and stops a script that has used up its turn. A script can catch the
// error, so once the turn is over every instruction is looked at: the error is raised again at the very next
// one, wherever that is, until nothing of the script is left to catch it.
constexpr int kLookEvery = 1000;
void look(lua_State* L, lua_Debug*) {
    Budget* const used = budget(L);
    if (!used) return;
    const int every = lua_gethookcount(L);
    used->run += uint64_t(every);
    if (used->run > kTurn) {
        if (every != 1) lua_sethook(L, look, LUA_MASKCOUNT, 1);
        luaL_error(L, "it ran too long without giving the game its turn back");
    }
    if (every == 1) lua_sethook(L, look, LUA_MASKCOUNT, kLookEvery); // a new turn has begun
}

// `load` takes text only. Lua does not check a compiled chunk as it reads it, and a bad one takes the game down.
int load_text(lua_State* L) {
    if (lua_gettop(L) < 3) lua_settop(L, 3);
    lua_pushliteral(L, "t");
    lua_replace(L, 3);
    lua_pushvalue(L, lua_upvalueindex(1));
    lua_insert(L, 1);
    lua_call(L, lua_gettop(L) - 1, LUA_MULTRET);
    return lua_gettop(L);
}

// What a script finds when it starts: Lua's own libraries that need nothing of the machine, the services, and
// `print` and `require` of the framework's own. The mod is the one argument.
int prepare(lua_State* L) {
    void* used = nullptr;
    lua_getallocf(L, &used);
    set_budget(L, static_cast<Budget*>(used));
    static const luaL_Reg libraries[] = {{LUA_GNAME, luaopen_base},        {LUA_COLIBNAME, luaopen_coroutine}, {LUA_TABLIBNAME, luaopen_table},
                                         {LUA_STRLIBNAME, luaopen_string}, {LUA_MATHLIBNAME, luaopen_math},    {LUA_UTF8LIBNAME, luaopen_utf8}};
    for (const luaL_Reg& library : libraries) {
        luaL_requiref(L, library.name, library.func, 1);
        lua_pop(L, 1);
    }
    lua_getglobal(L, "load");
    lua_pushcclosure(L, load_text, 1);
    lua_setglobal(L, "load");
    const char* const gone[] = {"dofile", "loadfile"}; // a script's files are reached through `files` and `require`
    for (const char* name : gone) {
        lua_pushnil(L);
        lua_setglobal(L, name);
    }
    open(L, static_cast<TSUCHI_Mod*>(lua_touserdata(L, 1)), &tsuchi::host);
    lua_pushcfunction(L, print);
    lua_setglobal(L, "print");
    lua_newtable(L);
    lua_rawsetp(L, LUA_REGISTRYINDEX, &kLoaded);
    lua_pushcfunction(L, require);
    lua_setglobal(L, "require");
    return 0;
}

TSUCHI_Result start(TSUCHI_Mod* mod, tsuchi::Text& problem) {
    auto* const used = static_cast<Budget*>(tsuchi::allocate(sizeof(Budget)));
    if (!used) return TSUCHI_NO_MEMORY;
    *used = Budget{};
    lua_State* L = lua_newstate(allocate, used);
    if (!L) {
        tsuchi::release(used);
        return TSUCHI_NO_MEMORY;
    }
    lua_sethook(L, look, LUA_MASKCOUNT, kLookEvery);
    const TSUCHI_Result owned = tsuchi::own(mod, kScript, 0, close, L);
    if (owned != TSUCHI_OK) {
        close(L, 0);
        return owned;
    }
    // Everything from here can fail for want of memory, so it runs where such a failure is caught.
    lua_pushcfunction(L, prepare);
    lua_pushlightuserdata(L, mod);
    int status = lua_pcall(L, 1, 0, 0);
    if (status == LUA_OK) {
        begin_turn(L);
        lua_pushcfunction(L, run_main);
        status = lua_pcall(L, 0, 0, 0);
    }
    if (status == LUA_OK) return TSUCHI_OK;
    // Nothing here may ask Lua for memory: there is nobody left to catch it when there is none.
    problem.set(lua_type(L, -1) == LUA_TSTRING ? lua_tostring(L, -1) : "it raised an error that is no text");
    return status == LUA_ERRMEM ? TSUCHI_NO_MEMORY : TSUCHI_INVALID; // whoever asked stops the mod, and the state goes with it
}

// Compiles the mod's main.lua in a state of its own, without running it, as a restart does before it stops the
// mod. The chunk has the name it has when the mod starts, so an error names the file and the line the same way.
TSUCHI_Result check(const char* id, tsuchi::Text& problem) {
    char* source = nullptr;
    uint64_t size = 0;
    const TSUCHI_Result read = tsuchi::read_mod_file(id, "main.lua", source, size);
    if (read != TSUCHI_OK) {
        problem.set(read == TSUCHI_NO_MEMORY ? "main.lua: there is no memory to read it" : "main.lua: it could not be read");
        return read;
    }
    auto* const used = static_cast<Budget*>(tsuchi::allocate(sizeof(Budget)));
    if (used) *used = Budget{};
    lua_State* L = used ? lua_newstate(allocate, used) : nullptr;
    if (!L) {
        tsuchi::release(used);
        tsuchi::release(source);
        problem.set("main.lua: there is no memory to compile it");
        return TSUCHI_NO_MEMORY;
    }
    char name[tsuchi::kModIdBytes + 16];
    const size_t length = strlen(id);
    name[0] = '@';
    memcpy(name + 1, id, length);
    memcpy(name + 1 + length, "/main.lua", sizeof "/main.lua");
    const int status = luaL_loadbufferx(L, source, size_t(size), name, "t");
    tsuchi::release(source);
    if (status != LUA_OK) problem.set(lua_type(L, -1) == LUA_TSTRING ? lua_tostring(L, -1) : "main.lua: it does not compile");
    lua_close(L);
    tsuchi::release(used);
    if (status == LUA_OK) return TSUCHI_OK;
    return status == LUA_ERRMEM ? TSUCHI_NO_MEMORY : TSUCHI_INVALID;
}

} // namespace

void register_script_mods() { tsuchi::add_mod_runtime({"main.lua", start, check}); }

} // namespace tsuchi_lua
