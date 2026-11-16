// A stand-in for the game: a few actors with life, as the backend of the combat
// service. It runs one Lua script as a mod through the real core, the generated
// service code and the generated bindings, then stops the mod and checks that
// nothing of it is left. Exits with 0 when all of that holds.
#include <cstdio>
#include <map>
#include <string>

#include "combat_service.h"
#include "camera_service.h"
#include "frame.h"
#include "host.h"
#include "mod_service.h"
#include "tsuchi_lua.h"
#include "registry.h"
extern "C" {
#include "lualib.h"
}

namespace {

const uint64_t kCreature = 3; // actors 1 and 2 are captains; 3 is a creature
std::map<uint64_t, TSUCHI_Life> actors;
int script_errors = 0, steps_run = 0;
uint32_t last_kind = 0, last_flags = 0;
TSUCHI_Vec3 last_velocity{};

TSUCHI_Result get_life(TSUCHI_Actor actor, TSUCHI_Life* out) {
    const auto found = actors.find(actor.id);
    if (found == actors.end()) return TSUCHI_NOT_FOUND;
    *out = found->second;
    return TSUCHI_OK;
}

TSUCHI_Result set_life(TSUCHI_Actor actor, float life, TSUCHI_LifeFlags flags) {
    const auto found = actors.find(actor.id);
    if (found == actors.end()) return TSUCHI_NOT_FOUND;
    if (life < 0) return TSUCHI_INVALID;
    found->second.life = life;
    last_flags = flags;
    return TSUCHI_OK;
}

TSUCHI_Result damage(TSUCHI_Actor victim, float amount, TSUCHI_AttackKind kind, TSUCHI_Actor attacker) {
    const auto found = actors.find(victim.id);
    if (found == actors.end()) return TSUCHI_NOT_FOUND;
    if (amount < 0) return TSUCHI_INVALID;
    const float before = found->second.life;
    found->second.life = before - amount;
    last_kind = kind;
    tsuchi::combat::emit_damaged({victim, attacker, before, found->second.life});
    if (found->second.life <= 0) tsuchi::combat::emit_defeated({victim, attacker, before, found->second.life});
    return TSUCHI_OK;
}

TSUCHI_Result knockback(TSUCHI_Actor victim, const TSUCHI_Vec3* velocity, float, TSUCHI_Actor) {
    if (!actors.count(victim.id)) return TSUCHI_NOT_FOUND;
    if (victim.id == kCreature) return TSUCHI_UNSUPPORTED;
    last_velocity = *velocity;
    return TSUCHI_OK;
}

// The stand-in has no Pikmin.
TSUCHI_Result kill_pikmin(TSUCHI_Actor, TSUCHI_DeathCause, TSUCHI_Actor) { return TSUCHI_NOT_FOUND; }

// Nor a Pikmin to land.
TSUCHI_Result thrown_hit(TSUCHI_Actor, TSUCHI_Actor, const TSUCHI_Vec3*, bool) { return TSUCHI_NOT_FOUND; }

TSUCHI_Result refresh_gauge(TSUCHI_Actor, float, bool) { return TSUCHI_NOT_FOUND; }
const tsuchi::combat::Backend backend{get_life, set_life, damage, knockback, thrown_hit, kill_pikmin, refresh_gauge};

TSUCHI_Result hold_look(TSUCHI_Mod*, uint32_t player, const TSUCHI_Look* target) {
    return player == 0 && target->actor.id <= 1 && target->point.x == 1 &&
        target->point.y == 2 && target->point.z == 3 ? TSUCHI_OK : TSUCHI_INVALID;
}

// What the script uses to reach the stand-in.
int test_actor(lua_State* L) {
    tsuchi_lua::push_handle(L, "actor", static_cast<uint64_t>(luaL_checkinteger(L, 1)));
    return 1;
}

int test_last(lua_State* L) {
    lua_newtable(L);
    lua_pushinteger(L, last_kind);
    lua_setfield(L, -2, "kind");
    lua_pushinteger(L, last_flags);
    lua_setfield(L, -2, "flags");
    tsuchi_lua::push_vec3(L, last_velocity);
    lua_setfield(L, -2, "velocity");
    return 1;
}

int test_errors(lua_State* L) {
    lua_pushinteger(L, script_errors);
    return 1;
}

int test_steps(lua_State* L) {
    lua_pushinteger(L, steps_run);
    return 1;
}

int test_subscriptions(lua_State* L) {
    lua_pushinteger(L, tsuchi::owned_count(tsuchi_lua::mod(L)));
    return 1;
}

// What a script gets for a number of the game's, as a value of an enum and as a set of flags.
int test_told(lua_State* L) {
    static const char* const names[] = {"smooth_gauge", "both", nullptr};
    static const uint32_t values[] = {1, 6};
    const auto number = static_cast<uint32_t>(luaL_checkinteger(L, 1));
    tsuchi_lua::push_unnamed(L, number);
    tsuchi_lua::push_flags(L, number, names, values);
    return 2;
}

// The mod's log: how many lines it has, and the last.
int log_lines = 0;
std::string last_line;
TSUCHI_Result log_line(TSUCHI_Mod*, const char* text) {
    ++log_lines;
    last_line = text;
    return TSUCHI_OK;
}

int test_logged(lua_State* L) {
    lua_pushinteger(L, log_lines);
    lua_pushstring(L, last_line.c_str());
    return 2;
}

// What a generated binding does first for a function the schema marks as deprecated.
int test_deprecated(lua_State* L) {
    tsuchi_lua::deprecated(L, luaL_checkstring(L, 1), static_cast<unsigned>(luaL_checkinteger(L, 2)), luaL_checkstring(L, 3));
    lua_pushinteger(L, lua_gettop(L)); // the arguments are as they were
    return 1;
}

const luaL_Reg test_functions[] = {
    {"actor", test_actor}, {"last", test_last}, {"errors", test_errors}, {"steps", test_steps},
    {"subscriptions", test_subscriptions}, {"told", test_told}, {"logged", test_logged}, {"deprecated", test_deprecated},
    {nullptr, nullptr}};

lua_Integer global_integer(lua_State* L, const char* name) {
    lua_getglobal(L, name);
    const lua_Integer value = lua_tointeger(L, -1);
    lua_pop(L, 1);
    return value;
}

int fail(const char* what) {
    std::fprintf(stderr, "%s\n", what);
    return 1;
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 2) return 2;
    actors[1] = {100, 100, 0, 0};
    actors[2] = {100, 100, 20, 20};
    actors[kCreature] = {500, 500, 0, 0};
    tsuchi::combat::set_backend(&backend);
    static tsuchi::camera::Backend camera_backend{};
    camera_backend.hold_look = hold_look;
    tsuchi::camera::set_backend(&camera_backend);
    static tsuchi::mod::Backend mods_backend{};
    mods_backend.log = log_line;
    tsuchi::mod::set_backend(&mods_backend);
    tsuchi::each_frame([] { ++steps_run; });
    tsuchi::each_frame([] { ++steps_run; });

    TSUCHI_Mod* mod = tsuchi::mod_create("test.script");
    lua_State* L = luaL_newstate();
    luaL_openlibs(L);
    tsuchi_lua::open(L, mod, &tsuchi::host);
    tsuchi_lua::set_error_handler([](const char*) { ++script_errors; });
    luaL_newlib(L, test_functions);
    lua_setglobal(L, "test");
    if (luaL_dofile(L, argv[1]) != LUA_OK) return fail(luaL_tolstring(L, -1, nullptr));

    // The script leaves handlers subscribed. Stopping the mod ends them all: a
    // later hit reaches no handler, and the mod owns nothing.
    // Three frames. Each runs the framework's own steps first, then tells the script.
    const int errors_before_frames = script_errors;
    for (int i = 0; i < 3; ++i) tsuchi::frame(0.5f);
    if (global_integer(L, "frames_seen") != 3 || global_integer(L, "last_frame_number") != 3)
        return fail("the script did not hear three frames");
    if (global_integer(L, "steps_at_last_frame") != 6) return fail("a frame reached the script before the framework's steps ran");
    if (script_errors != errors_before_frames) return fail("the frame handler raised an error");

    if (!tsuchi::combat::damaged_wanted()) return fail("the script should leave a damaged handler subscribed");
    const lua_Integer before = global_integer(L, "handler_calls");
    tsuchi::mod_stop(mod);
    if (tsuchi::combat::damaged_wanted() || tsuchi::combat::defeated_wanted()) return fail("a subscription outlived its mod");
    damage({2}, 1, TSUCHI_ATTACK_KIND_NORMAL, {0});
    if (global_integer(L, "handler_calls") != before) return fail("a handler ran after its mod stopped");
    TSUCHI_Life life{};
    if (tsuchi::host.combat->life(mod, {1}, &life) != TSUCHI_INVALID) return fail("a stopped mod made a call");
    lua_close(L);

    // The same id starts again with nothing carried over.
    TSUCHI_Mod* again = tsuchi::mod_create("test.script");
    if (!again || tsuchi::owned_count(again) != 0) return fail("a restarted mod did not start empty");
    if (tsuchi::host.combat->life(again, {1}, &life) != TSUCHI_OK) return fail("a restarted mod could not call");
    return 0;
}
