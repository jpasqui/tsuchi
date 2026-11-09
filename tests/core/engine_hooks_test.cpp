// Engine function hooks from Lua and JavaScript, against the made-up engine of the reflection checks. Its functions
// get native entries here, and a stand-in for ProcessEvent calls them the way the engine does: with a frame whose
// locals hold the arguments, the list of the arguments a function writes, and where its result goes. Exits with
// the number of failures.
#include <atomic>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>

#include "callable_thunks.h"
#include "engine_hooks.h"
#include "hook_service.h"
#include "objects.h"
#include "reflect.h"
#include "reflect_world.h"
#include "registry.h"
#include "tsuchi_js.h"
#include "tsuchi_lua.h"

extern "C" {
#include "lualib.h"
}

namespace tsuchi {
// The frame tick isn't part of this program; the checks call engine_hooks_frame themselves.
bool each_engine_frame(void (*)()) { return true; }
} // namespace tsuchi

namespace {

namespace t = fake::t;
using Native = void (*)(void* object, void* frame, void* result);

int failures = 0;
#define CHECK(condition)                                                \
    do {                                                                \
        if (!(condition)) {                                             \
            std::printf("%s:%d: %s\n", __FILE__, __LINE__, #condition); \
            ++failures;                                                 \
        }                                                               \
    } while (0)

std::atomic<uint64_t> next_thread{100};
uint64_t thread_number() {
    thread_local uint64_t number = next_thread++;
    return number;
}

uint64_t package = 0, uber = 0, poke = 0, other = 0;
int uber_runs = 0, last_entry = 0;
unsigned script_errors = 0;
std::string last_error;
bool lua_closed = false;

void script_error(const char* message) {
    ++script_errors;
    last_error = message ? message : "";
}

// ---- The engine's side ----

// What the frame's list says about an argument the function writes: where it is.
uint64_t out_address(uint64_t frame, uint64_t property) {
    for (uint64_t record = fake::get<uint64_t>(frame + t::script_frame::out_parms); record; record = fake::get<uint64_t>(record + t::out_parm::next))
        if (fake::get<uint64_t>(record + t::out_parm::property) == property) return fake::get<uint64_t>(record + t::out_parm::address);
    return 0;
}
uint64_t property_named(uint64_t function, const char* name) {
    for (uint32_t i = 0;; ++i) {
        const uint64_t property = tsuchi::game::field_at(function, i, false);
        if (!property || tsuchi::game::name_is(tsuchi::game::field_name(property), name)) return property;
    }
}

// The functions' native entries, as the engine's generated code reads its frame.
void heal_native(void* object, void* frame, void* result) {
    const uint64_t self = reinterpret_cast<uint64_t>(object), locals = fake::get<uint64_t>(reinterpret_cast<uint64_t>(frame) + t::script_frame::locals);
    const float health = fake::get<uint8_t>(locals + 4) ? 100.0f : fake::get<float>(self + 0x40) + fake::get<float>(locals);
    fake::put(self + 0x40, health);
    if (result) fake::put(reinterpret_cast<uint64_t>(result), health);
}
void where_native(void* object, void* frame, void* result) {
    const uint64_t at = reinterpret_cast<uint64_t>(result);
    fake::put(at, 1.0f), fake::put(at + 4, 2.0f), fake::put(at + 8, 3.0f);
    fake::put(out_address(reinterpret_cast<uint64_t>(frame), property_named(w.where, "Found")), reinterpret_cast<uint64_t>(object));
}
void rename_native(void* object, void* frame, void* result) {
    (void)result;
    const uint64_t self = reinterpret_cast<uint64_t>(object), locals = fake::get<uint64_t>(reinterpret_cast<uint64_t>(frame) + t::script_frame::locals);
    fake::put(self + 0x60, fake::get<uint64_t>(locals));
    const uint64_t title = out_address(reinterpret_cast<uint64_t>(frame), property_named(w.rename, "Title"));
    w.heard.clear();
    const uint64_t data = fake::get<uint64_t>(title + t::array::data);
    for (int32_t i = 0; i + 1 < fake::get<int32_t>(title + t::array::count); ++i) w.heard.push_back(char16_t(fake::get<uint16_t>(data + 2 * uint64_t(i))));
}
void uber_native(void*, void* frame, void*) {
    ++uber_runs;
    last_entry = fake::get<int32_t>(fake::get<uint64_t>(reinterpret_cast<uint64_t>(frame) + t::script_frame::locals));
}

// The engine's ProcessEvent: an event in an event graph runs as its entry point; the arguments are copied into the
// frame's locals, the ones the function writes are listed where the caller has them, and the result goes there too.
void process_event(uint64_t object, uint64_t function, uint64_t parms) {
    uint64_t target = function, arguments = parms;
    if (const uint64_t graph = fake::get<uint64_t>(function + t::function::event_graph)) {
        target = graph;
        arguments = fake::memory(8);
        fake::put(arguments, fake::get<int32_t>(function + t::function::event_graph_entry));
    }
    const int32_t bytes = fake::get<int32_t>(target + t::struct_type::bytes);
    const uint64_t locals = fake::memory(size_t(bytes) + 16), frame = fake::memory(0x100);
    std::memcpy(reinterpret_cast<void*>(locals), reinterpret_cast<const void*>(arguments), size_t(bytes));
    fake::put(frame + t::script_frame::node, target);
    fake::put(frame + t::script_frame::object, object);
    fake::put(frame + t::script_frame::locals, locals);
    uint64_t result = 0, link = frame + t::script_frame::out_parms;
    for (uint32_t i = 0;; ++i) {
        const uint64_t property = tsuchi::game::field_at(target, i, false);
        if (!property) break;
        const uint64_t flags = fake::get<uint64_t>(property + t::property::flags), at = arguments + uint64_t(fake::get<int32_t>(property + t::property::offset));
        if (flags & t::constant::property_return) result = at;
        else if (flags & t::constant::property_out) {
            const uint64_t record = fake::memory(t::out_parm::size);
            fake::put(record + t::out_parm::property, property);
            fake::put(record + t::out_parm::address, at);
            fake::put(link, record);
            link = record + t::out_parm::next;
        }
    }
    reinterpret_cast<Native>(fake::get<uint64_t>(target + t::function::func))(reinterpret_cast<void*>(object), reinterpret_cast<void*>(frame), reinterpret_cast<void*>(result));
    // The arguments it doesn't write go back where they came from, as the engine copies them.
    for (uint32_t i = 0;; ++i) {
        const uint64_t property = tsuchi::game::field_at(target, i, false);
        if (!property) break;
        if (fake::get<uint64_t>(property + t::property::flags) & (t::constant::property_out | t::constant::property_return)) continue;
        const int32_t offset = fake::get<int32_t>(property + t::property::offset), size = fake::get<int32_t>(property + t::property::element_bytes);
        std::memcpy(reinterpret_cast<void*>(arguments + uint64_t(offset)), reinterpret_cast<const void*>(locals + uint64_t(offset)), size_t(size));
    }
}

float heal(float amount, bool full) {
    const uint64_t parms = fake::memory(16);
    fake::put(parms, amount), fake::put<uint8_t>(parms + 4, full);
    process_event(w.red, w.heal, parms);
    return fake::get<float>(parms + 8);
}
// A Blueprint that calls Heal gives it its own frame, with the arguments still script code.
void scripted_heal() {
    const uint64_t locals = fake::memory(16), frame = fake::memory(0x100);
    fake::put(locals, 1.0f);
    fake::put(frame + t::script_frame::node, w.describe);
    fake::put(frame + t::script_frame::object, w.red);
    fake::put(frame + t::script_frame::locals, locals);
    reinterpret_cast<Native>(fake::get<uint64_t>(w.heal + t::function::func))(reinterpret_cast<void*>(w.red), reinterpret_cast<void*>(frame), nullptr);
}
std::string tag() {
    char text[256];
    return tsuchi::game::name_text(fake::get<tsuchi::game::Name>(w.red + 0x60), text, sizeof text) ? text : "";
}
std::string heard() { return std::string(w.heard.begin(), w.heard.end()); }

void build_engine() {
    build_world();
    package = fake::engine_object(u"/Script/Test", w.root);
    fake::put<uint64_t>(w.actor + t::object::outer, package);
    fake::put<uint64_t>(w.creature + t::object::outer, package);
    fake::put<uint64_t>(w.heal + t::function::func, reinterpret_cast<uint64_t>(&heal_native));
    fake::put<uint64_t>(w.where + t::function::func, reinterpret_cast<uint64_t>(&where_native));
    fake::put<uint64_t>(w.rename + t::function::func, reinterpret_cast<uint64_t>(&rename_native));
    // A Blueprint class's event graph, and two events without arguments that run as its entry points.
    uber = fake::function(w.creature, w.function_class, u"ExecuteUbergraph_Creature", 4, 4, [](uint64_t, uint64_t) {});
    fake::flag(fake::field(uber, u"EntryPoint", u"IntProperty", 0, 4), t::constant::property_param);
    fake::put<uint64_t>(uber + t::function::func, reinterpret_cast<uint64_t>(&uber_native));
    poke = fake::function(w.creature, w.function_class, u"OnPoke", 0, 4, [](uint64_t, uint64_t) {});
    fake::put<uint64_t>(poke + t::function::event_graph, uber);
    fake::put<int32_t>(poke + t::function::event_graph_entry, 42);
    other = fake::function(w.creature, w.function_class, u"OnOther", 0, 4, [](uint64_t, uint64_t) {});
    fake::put<uint64_t>(other + t::function::event_graph, uber);
    fake::put<int32_t>(other + t::function::event_graph_entry, 7);
}

// ---- The scripts' side ----

int lua_heal(lua_State* L) {
    lua_pushnumber(L, heal(float(luaL_checknumber(L, 1)), lua_toboolean(L, 2)));
    return 1;
}
int lua_threaded_heal(lua_State* L) {
    float result = 0;
    std::thread worker([&] { result = heal(1, false); });
    worker.join();
    lua_pushnumber(L, result);
    return 1;
}
int lua_scripted_heal(lua_State*) {
    scripted_heal();
    return 0;
}
int lua_health(lua_State* L) {
    lua_pushnumber(L, fake::get<float>(w.red + 0x40));
    return 1;
}
int lua_where(lua_State* L) {
    const uint64_t parms = fake::memory(0x18);
    process_event(w.red, w.where, parms);
    for (int i = 0; i < 3; ++i) lua_pushnumber(L, fake::get<float>(parms + 4 * uint64_t(i)));
    lua_pushboolean(L, fake::get<uint64_t>(parms + 0x10) == w.red);
    return 4;
}
int lua_rename(lua_State* L) {
    const uint64_t parms = fake::memory(0x18);
    fake::put(parms, fake::name(std::u16string(luaL_checkstring(L, 1), luaL_checkstring(L, 1) + strlen(luaL_checkstring(L, 1)))));
    const std::string title = luaL_checkstring(L, 2);
    fake::put_string(parms + 8, std::u16string(title.begin(), title.end()));
    process_event(w.red, w.rename, parms);
    lua_pushstring(L, tag().c_str());
    lua_pushstring(L, heard().c_str());
    return 2;
}
int lua_event(lua_State* L) {
    process_event(w.red, lua_toboolean(L, 1) ? poke : other, fake::memory(8));
    lua_pushinteger(L, uber_runs);
    lua_pushinteger(L, last_entry);
    return 2;
}
int lua_frame(lua_State*) {
    tsuchi::engine_hooks_frame();
    return 0;
}
int lua_stats(lua_State* L) {
    const tsuchi::EngineStats stats = tsuchi::engine_stats();
    lua_createtable(L, 0, 6);
    const std::pair<const char*, uint32_t> values[] = {{"calls", stats.calls}, {"off_thread", stats.off_thread}, {"script_arguments", stats.script_arguments},
                                                       {"hooks", stats.hooks_used}, {"sites", stats.sites_used}, {"failed", stats.failed}};
    for (const auto& value : values) {
        lua_pushinteger(L, value.second);
        lua_setfield(L, -2, value.first);
    }
    return 1;
}
int lua_errors(lua_State* L) {
    lua_pushinteger(L, script_errors);
    lua_pushstring(L, last_error.c_str());
    return 2;
}

JSValue js_heal(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    return JS_NewFloat64(ctx, heal(float(tsuchi_js::number(ctx, tsuchi_js::arg(argc, argv, 0))), argc > 1 && JS_ToBool(ctx, argv[1])));
}
JSValue js_threaded_heal(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    float result = 0;
    std::thread worker([&] { result = heal(1, false); });
    worker.join();
    return JS_NewFloat64(ctx, result);
}
JSValue js_scripted_heal(JSContext*, JSValueConst, int, JSValueConst*) {
    scripted_heal();
    return JS_UNDEFINED;
}
JSValue js_health(JSContext* ctx, JSValueConst, int, JSValueConst*) { return JS_NewFloat64(ctx, fake::get<float>(w.red + 0x40)); }
JSValue js_where(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    const uint64_t parms = fake::memory(0x18);
    process_event(w.red, w.where, parms);
    JSValue out = JS_NewArray(ctx);
    for (uint32_t i = 0; i < 3; ++i) JS_SetPropertyUint32(ctx, out, i, JS_NewFloat64(ctx, fake::get<float>(parms + 4 * uint64_t(i))));
    JS_SetPropertyUint32(ctx, out, 3, JS_NewBool(ctx, fake::get<uint64_t>(parms + 0x10) == w.red));
    return out;
}
JSValue js_rename(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    tsuchi_js::String name(ctx, tsuchi_js::arg(argc, argv, 0)), title(ctx, tsuchi_js::arg(argc, argv, 1));
    if (!name.text || !title.text) return JS_EXCEPTION;
    const std::string n = name.text, s = title.text;
    const uint64_t parms = fake::memory(0x18);
    fake::put(parms, fake::name(std::u16string(n.begin(), n.end())));
    fake::put_string(parms + 8, std::u16string(s.begin(), s.end()));
    process_event(w.red, w.rename, parms);
    return JS_NewString(ctx, (tag() + "|" + heard()).c_str());
}
JSValue js_event(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    process_event(w.red, argc && JS_ToBool(ctx, argv[0]) ? poke : other, fake::memory(8));
    return JS_NewInt32(ctx, uber_runs * 1000 + last_entry);
}
JSValue js_frame(JSContext*, JSValueConst, int, JSValueConst*) {
    tsuchi::engine_hooks_frame();
    return JS_UNDEFINED;
}
JSValue js_stats(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    const tsuchi::EngineStats stats = tsuchi::engine_stats();
    JSValue out = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, out, "off_thread", JS_NewUint32(ctx, stats.off_thread));
    JS_SetPropertyStr(ctx, out, "script_arguments", JS_NewUint32(ctx, stats.script_arguments));
    JS_SetPropertyStr(ctx, out, "hooks", JS_NewUint32(ctx, stats.hooks_used));
    return out;
}
JSValue js_errors(JSContext* ctx, JSValueConst, int, JSValueConst*) { return JS_NewUint32(ctx, script_errors); }
JSValue js_last_error(JSContext* ctx, JSValueConst, int, JSValueConst*) { return JS_NewString(ctx, last_error.c_str()); }

void count_lua(lua_State* L, lua_Debug*) {
    auto* b = tsuchi_lua::budget(L);
    b->run += uint64_t(lua_gethookcount(L));
    if (b->run > b->limit) {
        lua_sethook(L, count_lua, LUA_MASKCOUNT, 1);
        luaL_error(L, "hook budget");
    }
    if (lua_gethookcount(L) == 1) lua_sethook(L, count_lua, LUA_MASKCOUNT, 1000);
}
const tsuchi::Kind script_kind{"test.script"};
void lua_close_counted(lua_State* L) {
    lua_closed = true;
    lua_close(L);
}
void lua_stop(void* L, uint64_t) { tsuchi_lua::close_when_unused(static_cast<lua_State*>(L), lua_close_counted); }
void js_stop(void* state, uint64_t) { tsuchi_js::destroy(static_cast<tsuchi_js::State*>(state)); }

} // namespace

int main(int argc, char** argv) {
    if (argc != 3) return 2;
    build_engine();
    tsuchi::set_thread_probe(thread_number);
    tsuchi::note_game_thread();
    tsuchi::hooks_start();
    tsuchi::engine_hooks_start();
    CHECK(tsuchi::find_function_path("/Script/Test.Actor:Heal") == w.heal);
    CHECK(tsuchi::find_function_path("/script/test.actor:heal") == w.heal);
    CHECK(tsuchi::find_function_path("/Script/Test.Creature:Heal") == 0);
    CHECK(tsuchi::find_function_path("Heal") == 0);
    // Nothing is hooked yet: the engine's own entries run.
    CHECK(heal(1, false) == 13.5f);
    fake::put(w.red + 0x40, 12.5f);

    TSUCHI_Host host{};
    host.hook = &tsuchi::hook::table;
    tsuchi_lua::set_error_handler(script_error);
    tsuchi_js::set_error_handler(script_error);
    {
        TSUCHI_Mod* mod = tsuchi::mod_create("engine.lua");
        lua_State* L = luaL_newstate();
        luaL_openlibs(L);
        tsuchi_lua::Budget budget;
        tsuchi_lua::set_budget(L, &budget);
        lua_sethook(L, count_lua, LUA_MASKCOUNT, 1000);
        tsuchi_lua::open(L, mod, &host);
        CHECK(tsuchi::own(mod, script_kind, 0, lua_stop, L) == TSUCHI_OK);
        tsuchi_lua::push_handle(L, "actor", tsuchi::game::to_handle(w.red));
        lua_setglobal(L, "red");
        tsuchi_lua::push_handle(L, "object", tsuchi::game::to_handle(w.body));
        lua_setglobal(L, "body");
        const luaL_Reg functions[] = {{"heal", lua_heal},   {"threaded_heal", lua_threaded_heal}, {"scripted_heal", lua_scripted_heal},
                                      {"health", lua_health}, {"where", lua_where}, {"rename", lua_rename}, {"event", lua_event},
                                      {"frame", lua_frame}, {"stats", lua_stats}, {"errors", lua_errors}};
        for (const auto& f : functions) {
            lua_pushcfunction(L, f.func);
            lua_setglobal(L, f.name);
        }
        {
            tsuchi_lua::Turn turn(L);
            if (luaL_dofile(L, argv[1]) != LUA_OK) {
                std::printf("Lua failed: %s\n", lua_tostring(L, -1));
                ++failures;
                lua_pop(L, 1);
            }
        }
        // The mod's hooks end as it stops, and its state closes once they are released.
        CHECK(tsuchi::engine_stats().hooks_used > 0);
        tsuchi::mod_stop(mod);
        tsuchi::engine_hooks_frame();
        CHECK(lua_closed);
        CHECK(tsuchi::engine_stats().hooks_used == 0);
    }
    fake::put(w.red + 0x40, 12.5f);
    fake::put(w.red + 0x60, fake::name(u"Leader"));
    {
        TSUCHI_Mod* mod = tsuchi::mod_create("engine.javascript");
        auto* state = tsuchi_js::create(mod, &host);
        JSContext* ctx = tsuchi_js::context(state);
        CHECK(tsuchi::own(mod, script_kind, 0, js_stop, state) == TSUCHI_OK);
        {
            tsuchi_js::Value global(ctx, JS_GetGlobalObject(ctx));
            JS_SetPropertyStr(ctx, global, "red", tsuchi_js::push_handle(ctx, "actor", tsuchi::game::to_handle(w.red)));
            JS_SetPropertyStr(ctx, global, "body", tsuchi_js::push_handle(ctx, "object", tsuchi::game::to_handle(w.body)));
            struct Function {
                const char* name;
                JSCFunction* fn;
            };
            const Function functions[] = {{"heal", js_heal},     {"threaded_heal", js_threaded_heal}, {"scripted_heal", js_scripted_heal},
                                          {"health", js_health}, {"where", js_where}, {"rename", js_rename}, {"event", js_event},
                                          {"frame", js_frame},   {"stats", js_stats}, {"errors", js_errors}, {"last_error", js_last_error}};
            for (const auto& f : functions) JS_SetPropertyStr(ctx, global, f.name, JS_NewCFunction(ctx, f.fn, f.name, 0));
        }
        std::ifstream input(argv[2]);
        std::stringstream source;
        source << input.rdbuf();
        if (!tsuchi_js::evaluate(ctx, source.str().c_str(), source.str().size(), "engine_hooks.js", false)) {
            tsuchi_js::Value error(ctx, JS_GetException(ctx));
            tsuchi_js::String text(ctx, error);
            std::printf("JS failed: %s\n", text.text ? text.text : "exception");
            ++failures;
        }
        CHECK(tsuchi::engine_stats().hooks_used > 0);
        tsuchi::mod_stop(mod);
        tsuchi::engine_hooks_frame();
        CHECK(tsuchi_js::runtime_count() == 0);
        CHECK(tsuchi::engine_stats().hooks_used == 0);
    }
    // With every hook gone, the entries the engine kept still lead to its own functions.
    fake::put(w.red + 0x40, 12.5f);
    CHECK(heal(1, false) == 13.5f);
    if (!failures) std::puts("engine function hooks passed");
    return failures ? 1 : 0;
}
