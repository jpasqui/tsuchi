// Checks exact backend bits through actual generated calls and event delivery.
#include "tsuchi_lua.h"
#include <cstdio>
#include <cstdint>
extern "C" {
#include "lualib.h"
}
namespace {
constexpr uint64_t cases[] = {0, UINT64_C(0x20000000000001), INT64_MAX, UINT64_C(0x8000000000000000), UINT64_MAX};
unsigned phase = 0, captured = 0, events = 0, errors = 0;
uint64_t expected = 0;
TSUCHI_EntitySpawnedFn spawned = nullptr;
void* spawned_user = nullptr;
TSUCHI_Result find_placed(TSUCHI_Mod*, uint64_t value, TSUCHI_Actor* out) {
    if (value != expected) return TSUCHI_INVALID;
    ++captured; *out = TSUCHI_Actor{42}; return TSUCHI_OK;
}
TSUCHI_Result placement(TSUCHI_Mod*, TSUCHI_Actor actor, uint64_t* out) {
    if (actor.id != 42) return TSUCHI_INVALID;
    *out = expected; return TSUCHI_OK;
}
TSUCHI_Result on_spawned(TSUCHI_Mod*, TSUCHI_EntitySpawnedFn fn, void* user, TSUCHI_Subscription* out) {
    spawned = fn; spawned_user = user; *out = TSUCHI_Subscription{1}; return TSUCHI_OK;
}
TSUCHI_Result capture(TSUCHI_Mod*, uint64_t value, uint64_t* out) {
    if (value != expected) return TSUCHI_INVALID;
    ++captured; *out = value; return TSUCHI_OK;
}
TSUCHI_Result roundtrip(TSUCHI_Mod*, const TSUCHI_IdRecord* value, TSUCHI_IdRecord* out) {
    if (value->placement != expected) return TSUCHI_INVALID;
    ++captured; *out = *value; return TSUCHI_OK;
}
TSUCHI_Result with_default(TSUCHI_Mod*, uint64_t value, uint64_t* out) {
    if (value != UINT64_MAX && value != expected) return TSUCHI_INVALID;
    *out = value; return TSUCHI_OK;
}
TSUCHI_Result echo(TSUCHI_Mod*, uint64_t value, uint64_t* out) { *out = value; return TSUCHI_OK; }
TSUCHI_Result bounded(TSUCHI_Mod*, uint32_t value, uint32_t* out) { *out = value; return TSUCHI_OK; }
TSUCHI_Result log(TSUCHI_Mod*, const char* text) { ++errors; std::fprintf(stderr, "%s\n", text); return TSUCHI_OK; }
void error(const char* text) { ++errors; std::fprintf(stderr, "%s\n", text); }
int next_case(lua_State* L) {
    if (phase >= sizeof(cases)/sizeof(cases[0])) return luaL_error(L, "too many cases");
    expected = cases[phase++]; return 0;
}
int send_spawned(lua_State* L) {
    if (!spawned) return luaL_error(L, "missing generated event callback");
    TSUCHI_EntitySpawned event{};
    event.actor = TSUCHI_Actor{42}; event.kind = "larva"; event.placement = expected;
    ++events; spawned(spawned_user, &event); return 0;
}
}
int main(int argc, char** argv) {
    if (argc != 2) return 2;
    TSUCHI_EntityService entity{};
    entity.find_placed = find_placed; entity.placement = placement; entity.on_spawned = on_spawned;
    TSUCHI_PrecisionService precision{};
    precision.capture = capture; precision.roundtrip = roundtrip;
    precision.with_default = with_default; precision.bounded = bounded; precision.echo = echo;
    TSUCHI_ModService mod_api{}; mod_api.log = log;
    TSUCHI_Host host{}; host.entity = &entity; host.precision = &precision; host.mod = &mod_api;
    lua_State* L = luaL_newstate(); luaL_openlibs(L);
    tsuchi_lua::set_error_handler(error);
    tsuchi_lua::open(L, reinterpret_cast<TSUCHI_Mod*>(uintptr_t(1)), &host);
    lua_pushcfunction(L, next_case); lua_setglobal(L, "next_case");
    lua_pushcfunction(L, send_spawned); lua_setglobal(L, "send_spawned");
    const int result = luaL_dofile(L, argv[1]);
    if (result != LUA_OK) std::fprintf(stderr, "%s\n", lua_tostring(L, -1));
    const bool passed = result == LUA_OK && !errors && phase == 5 && events == 5 && captured == 30;
    lua_close(L);
    if (!passed) { std::fprintf(stderr, "phase=%u events=%u captured=%u errors=%u\n", phase, events, captured, errors); return 1; }
    std::puts("u64 exact native calls, results, structs, events and refusals passed"); return 0;
}
