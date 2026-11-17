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
#include <utility>

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

// The script interpreter's handlers (GNatives), as many as the checks use, by their opcodes in the engine. Each
// reads what follows it in the code and moves past it.
constexpr uint8_t kFloatConst = 0x1E, kTrue = 0x27, kFalse = 0x28, kNameConst = 0x21, kLocalOut = 0x48;
int bad_opcodes = 0;
uint64_t take(uint64_t frame, uint64_t bytes) {
    const uint64_t code = fake::get<uint64_t>(frame + t::script_frame::code);
    fake::put(frame + t::script_frame::code, code + bytes);
    return code;
}
void float_const(void*, void* frame, void* result) {
    const float value = fake::get<float>(take(reinterpret_cast<uint64_t>(frame), 4));
    if (result) fake::put(reinterpret_cast<uint64_t>(result), value);
}
void true_const(void*, void*, void* result) { if (result) fake::put<uint8_t>(reinterpret_cast<uint64_t>(result), 1); }
void false_const(void*, void*, void* result) { if (result) fake::put<uint8_t>(reinterpret_cast<uint64_t>(result), 0); }
void name_const(void*, void* frame, void* result) {
    const uint64_t value = fake::get<uint64_t>(take(reinterpret_cast<uint64_t>(frame), 8));
    if (result) fake::put(reinterpret_cast<uint64_t>(result), value);
}
// A variable of the caller's that a call passes by reference: its address, which the frame keeps as the last named.
void local_out(void*, void* frame, void* result) {
    const uint64_t address = fake::get<uint64_t>(take(reinterpret_cast<uint64_t>(frame), 8));
    fake::put(reinterpret_cast<uint64_t>(frame) + t::script_frame::most_recent_address, address);
    if (result) fake::put(reinterpret_cast<uint64_t>(result), fake::get<uint64_t>(address));
}
void bad_opcode(void*, void*, void*) { ++bad_opcodes; }
void step(uint64_t frame, uint64_t result) {
    const uint8_t opcode = fake::get<uint8_t>(take(frame, 1));
    reinterpret_cast<Native>(fake::get<uint64_t>(fake::main_image() + t::data::script_natives + 8 * uint64_t(opcode)))(
        reinterpret_cast<void*>(fake::get<uint64_t>(frame + t::script_frame::object)), reinterpret_cast<void*>(frame), reinterpret_cast<void*>(result));
}

// What a C++ function's generated code does for each argument (StepCompiledIn): with script code left, it runs the
// next expression; without, it takes the next argument of the frame's chain, from its locals or from the list of
// those the function writes. Gives where the value is.
uint64_t argument(uint64_t frame, uint64_t temporary) {
    if (fake::get<uint64_t>(frame + t::script_frame::code)) {
        fake::put<uint64_t>(frame + t::script_frame::most_recent_address, 0);
        step(frame, temporary);
        const uint64_t address = fake::get<uint64_t>(frame + t::script_frame::most_recent_address);
        return address ? address : temporary;
    }
    uint64_t property = fake::get<uint64_t>(frame + t::script_frame::property_chain);
    // The engine lists a result last, so a function's code never reaches it; the made-up functions list it anywhere.
    while (fake::get<uint64_t>(property + t::property::flags) & t::constant::property_return) property = fake::get<uint64_t>(property + t::property::next);
    fake::put(frame + t::script_frame::property_chain, fake::get<uint64_t>(property + t::property::next));
    if (fake::get<uint64_t>(property + t::property::flags) & t::constant::property_out) {
        for (uint64_t record = fake::get<uint64_t>(frame + t::script_frame::out_parms); record; record = fake::get<uint64_t>(record + t::out_parm::next))
            if (fake::get<uint64_t>(record + t::out_parm::property) == property) return fake::get<uint64_t>(record + t::out_parm::address);
        return 0;
    }
    return fake::get<uint64_t>(frame + t::script_frame::locals) + uint64_t(fake::get<int32_t>(property + t::property::offset));
}
// P_FINISH: past EX_EndFunctionParms, when there is code.
void finish_arguments(uint64_t frame) {
    if (const uint64_t code = fake::get<uint64_t>(frame + t::script_frame::code)) {
        if (fake::get<uint8_t>(code) != t::constant::script_end_parms) ++bad_opcodes;
        fake::put(frame + t::script_frame::code, code + 1);
    }
}

// The functions' native entries, as the engine's generated code reads its frame.
void heal_native(void* object, void* frame_pointer, void* result) {
    const uint64_t self = reinterpret_cast<uint64_t>(object), frame = reinterpret_cast<uint64_t>(frame_pointer), temporary = fake::memory(16);
    const float amount = fake::get<float>(argument(frame, temporary));
    const bool full = fake::get<uint8_t>(argument(frame, temporary + 8)) != 0;
    finish_arguments(frame);
    const float health = full ? 100.0f : fake::get<float>(self + 0x40) + amount;
    fake::put(self + 0x40, health);
    if (result) fake::put(reinterpret_cast<uint64_t>(result), health);
}
void where_native(void* object, void* frame_pointer, void* result) {
    const uint64_t frame = reinterpret_cast<uint64_t>(frame_pointer), found = argument(frame, fake::memory(8));
    finish_arguments(frame);
    const uint64_t at = reinterpret_cast<uint64_t>(result);
    fake::put(at, 1.0f), fake::put(at + 4, 2.0f), fake::put(at + 8, 3.0f);
    fake::put(found, reinterpret_cast<uint64_t>(object));
}
void rename_native(void* object, void* frame_pointer, void*) {
    const uint64_t self = reinterpret_cast<uint64_t>(object), frame = reinterpret_cast<uint64_t>(frame_pointer);
    const uint64_t name = argument(frame, fake::memory(8)), title = argument(frame, fake::memory(16));
    finish_arguments(frame);
    fake::put(self + 0x60, fake::get<uint64_t>(name));
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
    fake::put(frame + t::script_frame::property_chain, fake::get<uint64_t>(target + t::struct_type::properties));
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
// A frame of another function with no script code left: nothing says where Heal's arguments are, so the call goes
// straight through and is counted.
void scripted_heal() {
    const uint64_t locals = fake::memory(16), frame = fake::memory(0x100);
    fake::put(locals, 1.0f);
    fake::put(frame + t::script_frame::node, w.describe);
    fake::put(frame + t::script_frame::object, w.red);
    fake::put(frame + t::script_frame::locals, locals);
    fake::put(frame + t::script_frame::property_chain, fake::get<uint64_t>(w.heal + t::struct_type::properties));
    reinterpret_cast<Native>(fake::get<uint64_t>(w.heal + t::function::func))(reinterpret_cast<void*>(w.red), reinterpret_cast<void*>(frame), nullptr);
}

// A Blueprint's call of a C++ function, as UObject::CallFunction makes it: the caller's own frame, its code at the
// call's arguments and EX_EndFunctionParms, and where the caller wants the result. Checks that the frame comes back
// as the function would leave it: the code past the arguments, and the caller's own fields.
struct BlueprintCall {
    uint64_t frame = fake::memory(0x100), code = fake::memory(64), locals = fake::memory(16), out_parms = fake::memory(8), chain = fake::memory(8);
    size_t used = 0;
    BlueprintCall() {
        fake::put(frame + t::script_frame::node, w.describe);
        fake::put(frame + t::script_frame::object, w.red);
        fake::put(frame + t::script_frame::code, code);
        fake::put(frame + t::script_frame::locals, locals);
        fake::put(frame + t::script_frame::out_parms, out_parms);
        fake::put(frame + t::script_frame::property_chain, chain);
    }
    template <class T>
    void put(const T& value) {
        fake::put(code + used, value);
        used += sizeof value;
    }
    void call(uint64_t function, uint64_t result) {
        put<uint8_t>(uint8_t(t::constant::script_end_parms));
        put<uint8_t>(0xAA); // the caller's next opcode
        reinterpret_cast<Native>(fake::get<uint64_t>(function + t::function::func))(reinterpret_cast<void*>(w.red), reinterpret_cast<void*>(frame), reinterpret_cast<void*>(result));
        CHECK(fake::get<uint64_t>(frame + t::script_frame::code) == code + used - 1);
        CHECK(fake::get<uint64_t>(frame + t::script_frame::node) == w.describe);
        CHECK(fake::get<uint64_t>(frame + t::script_frame::locals) == locals);
        CHECK(fake::get<uint64_t>(frame + t::script_frame::out_parms) == out_parms);
        CHECK(fake::get<uint64_t>(frame + t::script_frame::property_chain) == chain);
    }
};
float blueprint_heal(float amount, bool full) {
    BlueprintCall call;
    call.put(kFloatConst), call.put(amount), call.put(full ? kTrue : kFalse);
    const uint64_t result = fake::memory(8);
    call.call(w.heal, result);
    return fake::get<float>(result);
}
uint64_t found_variable = 0; // the caller's variable that Where writes by its reference
void blueprint_where(float* xyz) {
    BlueprintCall call;
    found_variable = fake::memory(8);
    call.put(kLocalOut), call.put(found_variable);
    const uint64_t result = fake::memory(16);
    call.call(w.where, result);
    for (int i = 0; i < 3; ++i) xyz[i] = fake::get<float>(result + 4 * uint64_t(i));
}
void blueprint_rename(const std::string& name, const std::string& title) {
    BlueprintCall call;
    const uint64_t variable = fake::memory(16);
    fake::put_string(variable, std::u16string(title.begin(), title.end()));
    call.put(kNameConst), call.put(fake::name(std::u16string(name.begin(), name.end())));
    call.put(kLocalOut), call.put(variable);
    call.call(w.rename, 0);
}
void install_natives() {
    const uint64_t table = fake::main_image() + t::data::script_natives;
    for (unsigned opcode = 0; opcode < 256; ++opcode) fake::put(table + 8 * opcode, reinterpret_cast<uint64_t>(&bad_opcode));
    const std::pair<uint8_t, Native> handlers[] = {{kFloatConst, float_const}, {kTrue, true_const}, {kFalse, false_const},
                                                   {kNameConst, name_const}, {kLocalOut, local_out}};
    for (const auto& handler : handlers) fake::put(table + 8 * uint64_t(handler.first), reinterpret_cast<uint64_t>(handler.second));
}
std::string tag() {
    char text[256];
    return tsuchi::game::name_text(fake::get<tsuchi::game::Name>(w.red + 0x60), text, sizeof text) ? text : "";
}
std::string heard() { return std::string(w.heard.begin(), w.heard.end()); }

void build_engine() {
    build_world();
    install_natives();
    package = fake::engine_object(u"/Script/Test", w.root);
    // Two of the engine's own libraries: one whose every function takes a value of any type, and one with a single such function.
    const uint64_t engine = fake::engine_object(u"/Script/Engine", w.root);
    const uint64_t arrays = fake::engine_object(u"KismetArrayLibrary", w.root), system = fake::engine_object(u"KismetSystemLibrary", w.root);
    fake::put<uint64_t>(arrays + t::object::outer, engine);
    fake::put<uint64_t>(system + t::object::outer, engine);
    for (const auto& made : {std::pair<uint64_t, const char16_t*>{arrays, u"Array_Add"}, {system, u"SetStructurePropertyByName"}, {system, u"PrintString"}})
        fake::put<uint64_t>(fake::function(made.first, w.function_class, made.second, 0, 4, [](uint64_t, uint64_t) {}) + t::function::func, reinterpret_cast<uint64_t>(&heal_native));
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
int lua_blueprint_heal(lua_State* L) {
    lua_pushnumber(L, blueprint_heal(float(luaL_checknumber(L, 1)), lua_toboolean(L, 2)));
    return 1;
}
int lua_blueprint_where(lua_State* L) {
    float xyz[3];
    blueprint_where(xyz);
    for (float value : xyz) lua_pushnumber(L, value);
    lua_pushboolean(L, fake::get<uint64_t>(found_variable) == w.red);
    return 4;
}
int lua_blueprint_rename(lua_State* L) {
    blueprint_rename(luaL_checkstring(L, 1), luaL_checkstring(L, 2));
    lua_pushstring(L, tag().c_str());
    lua_pushstring(L, heard().c_str());
    return 2;
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
                                                       {"hooks", stats.hooks_used}, {"sites", stats.sites_used}, {"failed", stats.failed},
                                                       {"script_calls", stats.script_calls}, {"bad_opcodes", uint32_t(bad_opcodes)}};
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
JSValue js_blueprint_heal(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    return JS_NewFloat64(ctx, blueprint_heal(float(tsuchi_js::number(ctx, tsuchi_js::arg(argc, argv, 0))), argc > 1 && JS_ToBool(ctx, argv[1])));
}
JSValue js_blueprint_where(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    float xyz[3];
    blueprint_where(xyz);
    JSValue out = JS_NewArray(ctx);
    for (uint32_t i = 0; i < 3; ++i) JS_SetPropertyUint32(ctx, out, i, JS_NewFloat64(ctx, xyz[i]));
    JS_SetPropertyUint32(ctx, out, 3, JS_NewBool(ctx, fake::get<uint64_t>(found_variable) == w.red));
    return out;
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
    JS_SetPropertyStr(ctx, out, "script_calls", JS_NewUint32(ctx, stats.script_calls));
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
    CHECK(blueprint_heal(1, false) == 14.5f); // the function runs its script code itself
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
                                      {"frame", lua_frame}, {"stats", lua_stats}, {"errors", lua_errors}, {"blueprint_heal", lua_blueprint_heal},
                                      {"blueprint_where", lua_blueprint_where}, {"blueprint_rename", lua_blueprint_rename}};
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
                                          {"frame", js_frame},   {"stats", js_stats}, {"errors", js_errors}, {"last_error", js_last_error},
                                          {"blueprint_heal", js_blueprint_heal}, {"blueprint_where", js_blueprint_where}};
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
    CHECK(blueprint_heal(1, false) == 14.5f);
    CHECK(bad_opcodes == 0);
    if (!failures) std::puts("engine function hooks passed");
    return failures ? 1 : 0;
}
