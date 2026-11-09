// Reuse the host gateways and made-up game already used to check the dynamic core.
#define main dynamic_core_main
#include "dynamic_hooks_test.cpp"
#undef main
#include "tsuchi_lua.h"
#include "tsuchi_js.h"
#include "hook_service.h"
#include <fstream>
#include <sstream>
extern "C" {
#include "lualib.h"
}
namespace {
uint64_t script_entry = 0, script_inline_entry = 0, script_text_entry = 0, script_pointer_entry = 0;
unsigned script_errors = 0;
std::string last_error;
bool lua_closed = false;
void script_error(const char* message) { ++script_errors; last_error = message ? message : ""; }
// The game's function gives back the text pointer it got.
uint64_t text_pointer(uint64_t pointer, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t) { return pointer; }
// 1 when the game's function got the very pointer the game passed.
uint64_t probe_same_pointer() {
    const uint16_t text[] = {u'A', 0xd83d, 0xde80, 0};
    const uint64_t pointer = fake::memory(sizeof text);
    std::memcpy(reinterpret_cast<void*>(pointer), text, sizeof text);
    return call_entry(script_pointer_entry, pointer, 0) == pointer;
}
const tsuchi::Kind script_kind{"test.script"};
void lua_close_counted(lua_State* L) { lua_closed = true; lua_close(L); }
void lua_stop(void* L, uint64_t) { tsuchi_lua::close_when_unused(static_cast<lua_State*>(L), lua_close_counted); }
void js_stop(void* state, uint64_t) { tsuchi_js::destroy(static_cast<tsuchi_js::State*>(state)); }
void count_lua(lua_State* L, lua_Debug*) {
    auto* b = tsuchi_lua::budget(L); b->run += uint64_t(lua_gethookcount(L));
    if (b->run > b->limit) { lua_sethook(L, count_lua, LUA_MASKCOUNT, 1); luaL_error(L, "hook budget"); }
    if (lua_gethookcount(L) == 1) lua_sethook(L, count_lua, LUA_MASKCOUNT, 1000);
}
uint64_t probe(uint64_t a, uint64_t b, bool other) {
    uint64_t result = 0;
    if (other) { std::thread worker([&] { result = call_entry(script_entry, a, b); }); worker.join(); }
    else result = call_entry(script_entry, a, b);
    return result;
}
uint64_t text_echo(uint64_t pointer, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t) {
    if (!pointer) return 0;
    auto* text = reinterpret_cast<uint16_t*>(pointer); const uint16_t first = text[0]; text[0] = u'X'; return first;
}
uint64_t probe_text(bool null) {
    const uint16_t text[] = {u'A', 0xd83d, 0xde80, 0};
    const uint64_t pointer = null ? 0 : fake::memory(sizeof text);
    if (pointer) std::memcpy(reinterpret_cast<void*>(pointer), text, sizeof text);
    return call_entry(script_text_entry, pointer, 0);
}
uint64_t probe_inline() {
    uint64_t x[31]{};
    TSUCHI_InlineContext registers{}; registers.registers = x;
    x[0] = 0xfedcba9876543210ull;
    registers.nzcv = 0x20000000;
    thunks::call(fake::get<uint64_t>(script_inline_entry), reinterpret_cast<uint64_t>(&registers), 0, 0, 0, 0, 0);
    CHECK(x[0] == 0xfedcba9876543210ull || x[0] == 0xffffffffffffffffull);
    return registers.nzcv;
}
int lua_call_game(lua_State* L) {
    const auto a = tsuchi_lua::check_u64(L, 1), b = tsuchi_lua::check_u64(L, 2);
    tsuchi_lua::push_u64(L, probe(a, b, lua_toboolean(L, 3))); return 1;
}
int lua_text(lua_State* L) { tsuchi_lua::push_u64(L, probe_text(lua_toboolean(L, 1))); return 1; }
int lua_frame(lua_State*) { tsuchi::dynamic_hooks_frame(); return 0; }
int lua_calls(lua_State* L) { lua_pushinteger(L, stock_calls); return 1; }
int lua_errors(lua_State* L) { lua_pushinteger(L, script_errors); return 1; }
int lua_inline(lua_State* L) { tsuchi_lua::push_u64(L, probe_inline()); return 1; }
int lua_same_pointer(lua_State* L) { tsuchi_lua::push_u64(L, probe_same_pointer()); return 1; }
int lua_last_error(lua_State* L) { lua_pushstring(L, last_error.c_str()); return 1; }
int lua_turn_run(lua_State* L) { lua_pushinteger(L, lua_Integer(tsuchi_lua::budget(L)->run)); return 1; }
JSValue js_call_game(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    const auto a = tsuchi_js::u64(ctx, tsuchi_js::arg(argc, argv, 0)), b = tsuchi_js::u64(ctx, tsuchi_js::arg(argc, argv, 1));
    if (JS_HasException(ctx)) return JS_EXCEPTION;
    return JS_NewBigUint64(ctx, probe(a, b, argc > 2 && JS_ToBool(ctx, argv[2])));
}
JSValue js_text(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) { return JS_NewBigUint64(ctx, probe_text(argc && JS_ToBool(ctx, argv[0]))); }
JSValue js_frame(JSContext*, JSValueConst, int, JSValueConst*) { tsuchi::dynamic_hooks_frame(); return JS_UNDEFINED; }
JSValue js_calls(JSContext* ctx, JSValueConst, int, JSValueConst*) { return JS_NewInt32(ctx, stock_calls); }
JSValue js_errors(JSContext* ctx, JSValueConst, int, JSValueConst*) { return JS_NewUint32(ctx, script_errors); }
JSValue js_inline(JSContext* ctx, JSValueConst, int, JSValueConst*) { return JS_NewBigUint64(ctx, probe_inline()); }
JSValue js_same_pointer(JSContext* ctx, JSValueConst, int, JSValueConst*) { return JS_NewBigUint64(ctx, probe_same_pointer()); }
JSValue js_last_error(JSContext* ctx, JSValueConst, int, JSValueConst*) { return JS_NewString(ctx, last_error.c_str()); }
JSValue js_turn_run(JSContext* ctx, JSValueConst, int, JSValueConst*) { return JS_NewFloat64(ctx, double(tsuchi_js::turn_used(ctx))); }
bool lua_run(lua_State* L, const char* source) {
    tsuchi_lua::Turn turn(L);
    if (luaL_dostring(L, source) == LUA_OK) return true;
    lua_pop(L, 1);
    return false;
}
bool lua_global_true(lua_State* L, const char* name) {
    lua_getglobal(L, name);
    const bool value = lua_toboolean(L, -1);
    lua_pop(L, 1);
    return value;
}
bool js_eval(JSContext* ctx, const std::string& source) {
    if (tsuchi_js::evaluate(ctx, source.c_str(), source.size(), "hook-test.js", false)) return true;
    tsuchi_js::Value error(ctx, JS_GetException(ctx));
    // Turning the error into text runs script code, which a used-up turn interrupts at once.
    if (tsuchi_js::turn_over(ctx)) { std::puts("JS failed: the turn ran out"); return false; }
    tsuchi_js::String text(ctx, error);
    std::printf("JS failed: %s\n", text.text ? text.text : "exception"); return false;
}
} // namespace
int main(int argc, char** argv) {
    if (argc != 3) return 2;
    CHECK(dynamic_core_main() == 0);
    tsuchi::hooks_start(); tsuchi::dynamic_hooks_start();
    function_stock = reinterpret_cast<uint64_t>(stock); inline_stock = reinterpret_cast<uint64_t>(inline_end);
    tsuchi::set_native_hooks(&native);
    script_entry = new_entry(stock); script_inline_entry = native_entry(0x900, inline_stock);
    fake::code().push_back(reinterpret_cast<uint64_t>(text_echo)); script_text_entry = new_entry(text_echo);
    fake::code().push_back(reinterpret_cast<uint64_t>(text_pointer)); script_pointer_entry = new_entry(text_pointer);
    TSUCHI_Host host{}; host.hook = &tsuchi::hook::table;
    tsuchi_lua::set_error_handler(script_error); tsuchi_js::set_error_handler(script_error);
    {
        std::puts("starting Lua hooks"); std::fflush(stdout);
        TSUCHI_Mod* mod = tsuchi::mod_create("script.lua");
        lua_State* L = luaL_newstate(); luaL_openlibs(L);
        tsuchi_lua::Budget budget; tsuchi_lua::set_budget(L, &budget); lua_sethook(L, count_lua, LUA_MASKCOUNT, 1000);
        tsuchi_lua::open(L, mod, &host);
        CHECK(tsuchi::own(mod, script_kind, 0, lua_stop, L) == TSUCHI_OK);
        tsuchi_lua::push_u64(L, offset_of(script_entry)); lua_setglobal(L, "entry");
        tsuchi_lua::push_u64(L, offset_of(script_text_entry)); lua_setglobal(L, "text_entry");
        tsuchi_lua::push_u64(L, offset_of(script_pointer_entry)); lua_setglobal(L, "pointer_entry");
        const luaL_Reg functions[] = {{"call_text", lua_text}, {"call_game", lua_call_game}, {"frame", lua_frame}, {"calls", lua_calls}, {"errors", lua_errors}, {"call_instruction", lua_inline},
                                      {"same_pointer", lua_same_pointer}, {"last_error", lua_last_error}, {"turn_run", lua_turn_run}};
        for (const auto& f : functions) { lua_pushcfunction(L, f.func); lua_setglobal(L, f.name); }
        if (luaL_dofile(L, argv[1]) != LUA_OK) { std::printf("Lua failed: %s\n", lua_tostring(L, -1)); ++failures; lua_pop(L, 1); }
        // The final script hook loops forever; an outermost native call must interrupt it once.
        const unsigned before = script_errors;
        CHECK(probe(2, 3, false) == 5); tsuchi::dynamic_hooks_frame(); CHECK(script_errors == before + 1);
        CHECK(probe(2, 3, false) == 5); tsuchi::dynamic_hooks_frame(); CHECK(script_errors == before + 1);
        {
            tsuchi_lua::Turn turn(L);
            CHECK(luaL_dostring(L, "budget_hook=assert(hook.wrap({entry_at=entry,stock=0,signature='u64(u64,u64)'},function() local n=0 for i=1,20000 do n=n+i end return 99 end))") == LUA_OK);
        }
        budget.hooks.frame = tsuchi::engine_frame_number(); budget.hooks.spent = tsuchi_script::kHookFrame - 1000;
        CHECK(probe(2, 3, false) == 5);
        tsuchi::dynamic_hooks_frame(); CHECK(probe(2, 3, false) == 99);
        {
            tsuchi_lua::Turn turn(L);
            CHECK(luaL_dostring(L, "assert(hook.remove(budget_hook))") == LUA_OK);
        }
        tsuchi::dynamic_hooks_frame();
        // A hook that runs away inside a turn already running stops at its own allowance, and the outer script
        // goes on. Its own allowance ran out, so the hook is switched off.
        const char* lua_nested = "heavy=false; nested_hook=assert(hook.wrap({entry_at=entry,signature='u64(u64,u64)'},"
                                 "function(o,a,b) if heavy then while true do end end return 42 end))";
        CHECK(lua_run(L, lua_nested)); tsuchi::dynamic_hooks_frame();
        CHECK(lua_run(L, "heavy=true; nested_result=call_game(2,3); heavy=false; outer_done=nested_result==5"));
        CHECK(lua_global_true(L, "outer_done"));
        tsuchi::dynamic_hooks_frame(); CHECK(probe(2, 3, false) == 5);
        CHECK(lua_run(L, "assert(hook.remove(nested_hook))")); tsuchi::dynamic_hooks_frame();
        // With less left in the outer turn than the hook's own allowance, the outer turn is what runs out, and
        // the hook stays on.
        CHECK(lua_run(L, lua_nested)); tsuchi::dynamic_hooks_frame();
        CHECK(!lua_run(L, "while turn_run() < 19600000 do for i=1,1000 do end end heavy=true; call_game(2,3)"));
        CHECK(lua_run(L, "heavy=false")); tsuchi::dynamic_hooks_frame();
        CHECK(probe(2, 3, false) == 42);
        CHECK(lua_run(L, "assert(hook.remove(nested_hook))")); tsuchi::dynamic_hooks_frame();
        const uint64_t pinned = new_entry(holding_stock);
        tsuchi_lua::push_u64(L, offset_of(pinned)); lua_setglobal(L, "pin_entry");
        {
            tsuchi_lua::Turn turn(L);
            CHECK(luaL_dostring(L, "assert(hook.wrap({entry_at=pin_entry,stock=0,signature='u64(u64,u64)'},function(o,a,b) return o(a,b) end))") == LUA_OK);
        }
        hold_stock = true; stock_entered = false;
        std::thread worker([&] { CHECK(call_entry(pinned, 2, 3) == 5); });
        while (!stock_entered) std::this_thread::yield();
        tsuchi::mod_stop(mod); tsuchi::dynamic_hooks_frame(); CHECK(!lua_closed);
        hold_stock = false; worker.join();
        tsuchi::dynamic_hooks_frame(); CHECK(lua_closed);
    }
    {
        TSUCHI_Mod* mod = tsuchi::mod_create("script.javascript");
        auto* state = tsuchi_js::create(mod, &host); JSContext* ctx = tsuchi_js::context(state);
        CHECK(tsuchi::own(mod, script_kind, 0, js_stop, state) == TSUCHI_OK);
        {
            tsuchi_js::Value global(ctx, JS_GetGlobalObject(ctx));
            JS_SetPropertyStr(ctx, global, "entry", JS_NewBigUint64(ctx, offset_of(script_entry)));
            JS_SetPropertyStr(ctx, global, "text_entry", JS_NewBigUint64(ctx, offset_of(script_text_entry)));
            JS_SetPropertyStr(ctx, global, "pointer_entry", JS_NewBigUint64(ctx, offset_of(script_pointer_entry)));
            struct Function { const char* name; JSCFunction* fn; };
            const Function functions[] = {{"call_text", js_text}, {"call_game", js_call_game}, {"frame", js_frame}, {"calls", js_calls}, {"errors", js_errors}, {"call_instruction", js_inline},
                                          {"same_pointer", js_same_pointer}, {"last_error", js_last_error}, {"turn_run", js_turn_run}};
            for (const auto& f : functions) JS_SetPropertyStr(ctx, global, f.name, JS_NewCFunction(ctx, f.fn, f.name, 0));
        }
        std::ifstream input(argv[2]); std::stringstream source; source << input.rdbuf(); CHECK(js_eval(ctx, source.str()));
        const unsigned before = script_errors;
        CHECK(probe(2, 3, false) == 5); tsuchi::dynamic_hooks_frame(); CHECK(script_errors == before + 1);
        CHECK(probe(2, 3, false) == 5); tsuchi::dynamic_hooks_frame(); CHECK(script_errors == before + 1);
        CHECK(js_eval(ctx, "const budget_hook=hook.wrap(target,()=>{let n=0;for(let i=0;i<20000;i++)n+=i;return 99n;});"));
        auto& budget = tsuchi_js::hook_allowance(ctx);
        budget.frame = tsuchi::engine_frame_number(); budget.spent = tsuchi_script::kHookFrame - 1000;
        CHECK(probe(2, 3, false) == 5);
        tsuchi::dynamic_hooks_frame(); CHECK(probe(2, 3, false) == 99);
        CHECK(js_eval(ctx, "hook.remove(budget_hook);")); tsuchi::dynamic_hooks_frame();
        // The same two cases as for Lua: a runaway hook inside a running turn, and an outer turn that runs out.
        const char* js_nested = "var heavy=false; var nested_hook=hook.wrap({entry_at:entry,signature:'u64(u64,u64)'},"
                                "(o,a,b)=>{if(heavy)for(;;){} return 42n;});";
        CHECK(js_eval(ctx, js_nested)); tsuchi::dynamic_hooks_frame();
        CHECK(js_eval(ctx, "heavy=true; var nested_result=call_game(2n,3n); heavy=false;"
                           "if (nested_result !== 5n) throw new Error('nested call');"));
        tsuchi::dynamic_hooks_frame(); CHECK(probe(2, 3, false) == 5);
        CHECK(js_eval(ctx, "hook.remove(nested_hook);")); tsuchi::dynamic_hooks_frame();
        CHECK(js_eval(ctx, js_nested)); tsuchi::dynamic_hooks_frame();
        std::puts("an outer turn runs out next, on purpose:");
        CHECK(!js_eval(ctx, "while (turn_run() < 19600000) { for (let i=0;i<1000;i++){} } heavy=true; call_game(2n,3n);"
                            "for (let i=0;i<100000;i++){}"));
        CHECK(js_eval(ctx, "heavy=false;")); tsuchi::dynamic_hooks_frame();
        CHECK(probe(2, 3, false) == 42);
        CHECK(js_eval(ctx, "hook.remove(nested_hook);")); tsuchi::dynamic_hooks_frame();
        const uint64_t pinned = new_entry(holding_stock);
        {
            tsuchi_js::Value global(ctx, JS_GetGlobalObject(ctx));
            JS_SetPropertyStr(ctx, global, "pin_entry", JS_NewBigUint64(ctx, offset_of(pinned)));
        }
        CHECK(js_eval(ctx, "hook.wrap({entry_at:pin_entry,stock:0n,signature:'u64(u64,u64)'},(o,a,b)=>o(a,b));"));
        hold_stock = true; stock_entered = false;
        std::thread worker([&] { CHECK(call_entry(pinned, 2, 3) == 5); });
        while (!stock_entered) std::this_thread::yield();
        tsuchi::mod_stop(mod); tsuchi::dynamic_hooks_frame(); CHECK(tsuchi_js::runtime_count() == 1);
        hold_stock = false; worker.join();
        tsuchi::dynamic_hooks_frame(); CHECK(tsuchi_js::runtime_count() == 0);
    }
    if (!failures) std::puts("script hooks passed");
    return failures ? 1 : 0;
}
