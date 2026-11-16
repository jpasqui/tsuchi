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
#include "tsuchi_js.h"
#include "dialog_service.h"
#include "file_service.h"
#include "entity_service.h"
#include "precision_service.h"
#include <fstream>
#include <sstream>
#include <cstring>
#include "registry.h"


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

JSValue test_actor(JSContext* c, JSValueConst, int n, JSValueConst* a) { return tsuchi_js::push_handle(c, "actor", tsuchi_js::u64(c, tsuchi_js::arg(n, a, 0))); }
JSValue test_last(JSContext* c, JSValueConst, int, JSValueConst*) {
    tsuchi_js::Value v(c, JS_NewObject(c)); JS_SetPropertyStr(c, v, "kind", JS_NewInt64(c, last_kind)); JS_SetPropertyStr(c, v, "flags", JS_NewInt64(c, last_flags)); JS_SetPropertyStr(c, v, "velocity", tsuchi_js::push_vec3(c, last_velocity)); return v.take();
}
JSValue test_errors(JSContext* c, JSValueConst, int, JSValueConst*) { return JS_NewInt32(c, script_errors); }
JSValue test_steps(JSContext* c, JSValueConst, int, JSValueConst*) { return JS_NewInt32(c, steps_run); }
JSValue test_subscriptions(JSContext* c, JSValueConst, int, JSValueConst*) { return JS_NewInt32(c, tsuchi::owned_count(tsuchi_js::mod(c))); }
uint64_t captured = 0;
TSUCHI_Result capture(uint64_t value, uint64_t* out) { captured = value; *out = value; return TSUCHI_OK; }
TSUCHI_Result roundtrip(const TSUCHI_IdRecord* value, TSUCHI_IdRecord* out) { *out = *value; captured = value->placement; return TSUCHI_OK; }
TSUCHI_Result bounded(uint32_t value, uint32_t* out) { *out = value; return TSUCHI_OK; }
const tsuchi::precision::Backend precision_backend{capture, roundtrip, capture, capture, bounded};
JSValue test_captured(JSContext* c, JSValueConst, int, JSValueConst*) { return JS_NewBigUint64(c, captured); }
JSValue test_emit(JSContext* c, JSValueConst, int n, JSValueConst* a) {
    uint64_t id = tsuchi_js::u64(c, tsuchi_js::arg(n, a, 0));
    if (JS_HasException(c)) return JS_EXCEPTION;
    tsuchi::entity::emit_spawned({{3}, "Creature", 0, id, {1,2,3}}); return JS_TRUE;
}
TSUCHI_Result question(TSUCHI_Mod*, const char*, TSUCHI_Dialog* out) { static uint64_t id = 1; out->id = id++; return TSUCHI_OK; }
const tsuchi::dialog::Backend dialog_backend{question, question, nullptr, nullptr, nullptr};
JSValue test_answer(JSContext* c, JSValueConst, int n, JSValueConst* a) {
    auto id = tsuchi_js::handle(c, tsuchi_js::arg(n, a, 0), "dialog", false);
    bool yes = tsuchi_js::boolean(c, tsuchi_js::arg(n, a, 1));
    if (JS_HasException(c)) return JS_EXCEPTION;
    tsuchi::dialog::emit_answered({{id}, yes, yes ? 0u : 1u}); return JS_TRUE;
}
// One file a mod can read, with a zero byte, a byte past 127 and a line break in it.
const unsigned char known_file[] = {'T', 's', 'u', 0, 0xFF, 0x80, '\n', 'j', 's'};
TSUCHI_Result read_file(TSUCHI_Mod*, const char* path, uint64_t offset, void* buffer, uint64_t capacity, uint64_t* out) {
    if (std::string(path) != "known.bin") return TSUCHI_NOT_FOUND;
    const uint64_t left = offset < sizeof known_file ? sizeof known_file - offset : 0, n = left < capacity ? left : capacity;
    if (n) std::memcpy(buffer, known_file + offset, n);
    *out = n; return TSUCHI_OK;
}
TSUCHI_Result file_size(TSUCHI_Mod*, const char* path, uint64_t* out) {
    if (std::string(path) != "known.bin") return TSUCHI_NOT_FOUND;
    *out = sizeof known_file; return TSUCHI_OK;
}
const tsuchi::file::Backend file_backend{read_file, file_size, nullptr, nullptr};
JSValue test_handles(JSContext* c, JSValueConst, int, JSValueConst*) { return JS_NewInt64(c, tsuchi_js::handles_held(c)); }
int log_lines = 0; std::string last_line;
TSUCHI_Result log_line(TSUCHI_Mod*, const char* text) { ++log_lines; last_line = text; std::puts(text); return TSUCHI_OK; }
JSValue test_logged(JSContext* c, JSValueConst, int, JSValueConst*) { return JS_NewInt32(c, log_lines); }
JSValue test_deprecated(JSContext* c, JSValueConst, int n, JSValueConst* a) {
    tsuchi_js::String what(c, tsuchi_js::arg(n,a,0)), use(c, tsuchi_js::arg(n,a,2));
    unsigned v = static_cast<unsigned>(tsuchi_js::integer(c, tsuchi_js::arg(n,a,1),0,UINT32_MAX));
    if (JS_HasException(c)) return JS_EXCEPTION;
    tsuchi_js::deprecated(c, what, v, use); return JS_TRUE;
}
int fail(const char* what) { std::fprintf(stderr, "%s\n", what); return 1; }
void function(JSContext* c, JSValueConst object, const char* name, JSCFunction* fn, int n) { JS_SetPropertyStr(c, object, name, JS_NewCFunction(c, fn, name, n)); }
bool execute(JSContext* ctx, const std::string& source) { return tsuchi_js::evaluate(ctx, source.c_str(), source.size(), "script.js", false); }
} // namespace
// Hooks for the same script on the framework's own libc, with allocation headers as on ARM64.
extern "C" {
void* tsuchi_libc_allocate(size_t n) { auto* p = static_cast<size_t*>(std::malloc(n+16)); if (!p) return nullptr; p[0]=n; return reinterpret_cast<char*>(p)+16; }
void tsuchi_libc_release(void* p) { if (p) std::free(static_cast<char*>(p)-16); }
void* tsuchi_libc_reallocate(void* p, size_t n) { if (!p) return tsuchi_libc_allocate(n); auto* b=static_cast<char*>(p)-16; void* q=std::realloc(b,n+16); if (!q) return nullptr; *static_cast<size_t*>(q)=n; return static_cast<char*>(q)+16; }
void tsuchi_libc_write(const char*, size_t) {}
unsigned long long tsuchi_libc_ticks() { return 0; }
}
int main(int argc, char** argv) {
    if (argc != 2) return 2;
    actors[1] = {100,100,0,0}; actors[2] = {100,100,20,20}; actors[3] = {500,500,0,0};
    tsuchi::combat::set_backend(&backend); tsuchi::precision::set_backend(&precision_backend); tsuchi::dialog::set_backend(&dialog_backend);
    tsuchi::file::set_backend(&file_backend);
    static tsuchi::camera::Backend camera_backend{};
    camera_backend.hold_look = hold_look;
    tsuchi::camera::set_backend(&camera_backend);
    static tsuchi::mod::Backend mods_backend{}; mods_backend.log = log_line; tsuchi::mod::set_backend(&mods_backend);
    tsuchi::each_frame([] { ++steps_run; }); tsuchi::each_frame([] { ++steps_run; });
    TSUCHI_Mod* mod = tsuchi::mod_create("test.javascript");
    tsuchi_js::State* state = tsuchi_js::create(mod, &tsuchi::host); if (!state) return fail("no runtime");
    JSContext* ctx = tsuchi_js::context(state);
    tsuchi_js::set_error_handler([](const char* message) { ++script_errors; std::fprintf(stderr,"handler: %s\n",message); });
    tsuchi_js::Value global(ctx, JS_GetGlobalObject(ctx));
    tsuchi_js::Value test(ctx, JS_NewObject(ctx));
    function(ctx,test,"actor",test_actor,1); function(ctx,test,"last",test_last,0); function(ctx,test,"errors",test_errors,0);
    function(ctx,test,"steps",test_steps,0); function(ctx,test,"subscriptions",test_subscriptions,0); function(ctx,test,"captured",test_captured,0);
    function(ctx,test,"emit",test_emit,1); function(ctx,test,"answer",test_answer,2); function(ctx,test,"logged",test_logged,0); function(ctx,test,"deprecated",test_deprecated,3);
    function(ctx,test,"handles",test_handles,0);
    JS_SetPropertyStr(ctx,global,"test",test.take());
    std::ifstream file(argv[1], std::ios::binary); std::ostringstream source; source << file.rdbuf();
    if (!execute(ctx,source.str())) { tsuchi_js::report(ctx); return fail("script failed"); }
    if (std::string(argv[1]).find("combat.js") != std::string::npos) {
        // Cancelled subscriptions leave nothing behind in the runtime.
        const char churn[] = "for (let i = 0; i < 1000; ++i) combat.on('damaged', () => {}).cancel();";
        if (!execute(ctx, churn)) { tsuchi_js::report(ctx); return fail("churn failed"); }
        JS_RunGC(JS_GetRuntime(ctx));
        const size_t settled = tsuchi_js::memory_held(state);
        if (!execute(ctx, churn)) { tsuchi_js::report(ctx); return fail("churn failed"); }
        JS_RunGC(JS_GetRuntime(ctx));
        if (tsuchi_js::memory_held(state) > settled + 1024) { std::fprintf(stderr, "held %zu after %zu\n", tsuchi_js::memory_held(state), settled); return fail("cancelled subscriptions were kept"); }
        for (int i = 0; i < 3; ++i) tsuchi::frame(0.5f);
        if (!execute(ctx,"check(frames_seen === 3 && pictures_seen === 3 && steps_at_last_frame === 6 && last_frame_number === 3n); verifySlow();")) { tsuchi_js::report(ctx); return fail("frames failed"); }
        tsuchi::mod_stop(mod);
        if (tsuchi::combat::damaged_wanted() || tsuchi::combat::defeated_wanted() || tsuchi::dialog::answered_wanted()) return fail("subscription outlived mod");
        damage({2},1,0,{0});
        if (!execute(ctx,"check(handler_calls === 1);")) return fail("handler ran after stop");
    } else tsuchi::mod_stop(mod);
    // Drop C++ values before the state, which owns their context.
    JS_FreeValue(ctx,global.take());
    tsuchi_js::destroy(state);
    std::puts("javascript host passed"); return 0;
}
