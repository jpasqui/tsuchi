#include "tsuchi_lua.h"
#include "dynamic_hooks.h"
#include "registry.h"
#include "../script_hooks.h"

namespace {
using namespace tsuchi;
using namespace tsuchi_lua;
struct Hook {
    lua_State* L = nullptr;
    int fn = LUA_NOREF;
    Signature signature;
    bool disabled = false;
    uint32_t missed = 0; // watch records that came while the function couldn't run, told with the next one
    char error[256]{};
};
Hook hooks[kDynamicGateways];
struct Call {
    Hook* hook;
    CallFrame* frame = nullptr;
    const DynamicOriginal* original = nullptr;
    InlineRegisters* registers = nullptr;
    const WatchRecord* watched = nullptr;
    uint64_t serial = 0, last = 0;
    bool called = false;
    Call* previous = nullptr;
};
Call* active = nullptr;
uint64_t next_call = 1;

void push_text(lua_State* L, const uint16_t* text, size_t units) {
    char bytes[kWatchTextUnits * 3];
    lua_pushlstring(L, bytes, tsuchi_script::utf8(text, units, bytes));
}
void push(lua_State* L, CallType type, uint64_t bits) {
    switch (type) {
    case CallType::void_: lua_pushnil(L); break;
    case CallType::boolean: lua_pushboolean(L, bits != 0); break;
    case CallType::i32: lua_pushinteger(L, int32_t(bits)); break;
    case CallType::u32: lua_pushinteger(L, uint32_t(bits)); break;
    case CallType::f32: {
        const uint32_t low = uint32_t(bits); float value; memcpy(&value, &low, sizeof value);
        lua_pushnumber(L, value); break;
    }
    case CallType::text16: {
        if (!bits) { lua_pushnil(L); break; }
        uint16_t text[kWatchTextUnits + 1];
        const uint32_t units = read_text16(bits, text, kWatchTextUnits + 1);
        if (units > kWatchTextUnits) luaL_error(L, "hook text exceeds 256 UTF-16 units");
        push_text(L, text, units); break;
    }
    default: push_u64(L, bits); break;
    }
}
// `game` is what the game passed in the same place, for a text16 argument the script may hand on unchanged.
uint64_t read(lua_State* L, int index, CallType type, uint16_t* text = nullptr, uint64_t game = 0) {
    switch (type) {
    case CallType::void_: return 0;
    case CallType::boolean:
        luaL_checktype(L, index, LUA_TBOOLEAN); return lua_toboolean(L, index) != 0;
    case CallType::i32: return uint64_t(check_integer(L, index, INT32_MIN, INT32_MAX));
    case CallType::u32: return uint64_t(check_integer(L, index, 0, UINT32_MAX));
    case CallType::f32: {
        luaL_checktype(L, index, LUA_TNUMBER);
        const float value = float(lua_tonumber(L, index)); uint32_t bits; memcpy(&bits, &value, sizeof bits); return bits;
    }
    case CallType::text16: {
        if (lua_isnil(L, index)) return 0;
        luaL_checktype(L, index, LUA_TSTRING);
        size_t bytes = 0; const char* value = lua_tolstring(L, index, &bytes);
        if (game) {
            uint16_t current[kWatchTextUnits + 1];
            const uint32_t units = read_text16(game, current, kWatchTextUnits + 1);
            if (tsuchi_script::same_text(current, units, value, bytes)) return game;
        }
        if (!text || !tsuchi_script::utf16(value, bytes, text, kWatchTextUnits + 1))
            luaL_error(L, "original needs text of at most 256 UTF-16 units without embedded NUL");
        return reinterpret_cast<uint64_t>(text);
    }
    default: return check_u64(L, index);
    }
}
lua_State* main_state(lua_State* L) {
    lua_rawgeti(L, LUA_REGISTRYINDEX, LUA_RIDX_MAINTHREAD);
    lua_State* main = lua_tothread(L, -1); lua_pop(L, 1); return main;
}
int original(lua_State* L) {
    const uint64_t serial = check_u64(L, lua_upvalueindex(1));
    Call* call = active;
    while (call && (call->serial != serial || call->hook->L != main_state(L))) call = call->previous;
    if (!call || !call->original) return luaL_error(L, "original is no longer active");
    const Signature& sig = call->hook->signature;
    if (lua_gettop(L) != sig.count) return luaL_error(L, "original needs every argument in its signature");
    CallFrame frame = *call->frame;
    uint16_t text[kRegisterArguments][kWatchTextUnits + 1]; unsigned strings = 0;
    for (uint32_t i = 0; i < sig.count; ++i) {
        const bool is_text = sig.args[i] == CallType::text16;
        set_frame_argument(frame, sig, i, read(L, int(i + 1), sig.args[i], is_text ? text[strings++] : nullptr,
                                               is_text ? frame_argument(*call->frame, sig, i) : 0));
    }
    call_with_frame(call->original->target, frame, sig.result);
    call->last = frame_result(frame, sig.result);
    call->called = true; // before pushing the return value, which can run out of memory
    push(L, sig.result, call->last);
    return 1;
}
void field(lua_State* L, int table, const char* name) {
    lua_pushstring(L, name); lua_rawget(L, table);
}
const char* flags[] = {"n", "z", "c", "v"};
int invoke(lua_State* L) {
    auto& call = *static_cast<Call*>(lua_touserdata(L, 1));
    Hook& hook = *call.hook;
    lua_rawgeti(L, LUA_REGISTRYINDEX, hook.fn);
    if (call.registers) {
        lua_createtable(L, 0, 5);
        const int snapshot = lua_gettop(L);
        lua_createtable(L, 31, 0);
        for (int i = 0; i < 31; ++i) { push_u64(L, call.registers->x[i]); lua_rawseti(L, -2, i); }
        lua_setfield(L, snapshot, "x");
        for (int i = 0; i < 4; ++i) { lua_pushboolean(L, (call.registers->nzcv >> (31 - i)) & 1); lua_setfield(L, snapshot, flags[i]); }
        // Keep the snapshot rooted while its callback runs.
        lua_pushvalue(L, snapshot - 1); lua_pushvalue(L, snapshot); lua_call(L, 1, 0);
        InlineRegisters staged{};
        field(L, snapshot, "x"); luaL_checktype(L, -1, LUA_TTABLE);
        for (int i = 0; i < 31; ++i) { lua_rawgeti(L, -1, i); staged.x[i] = check_u64(L, -1); lua_pop(L, 1); }
        lua_pop(L, 1);
        for (int i = 0; i < 4; ++i) {
            field(L, snapshot, flags[i]); luaL_checktype(L, -1, LUA_TBOOLEAN);
            if (lua_toboolean(L, -1)) staged.nzcv |= uint64_t(1) << (31 - i);
            lua_pop(L, 1);
        }
        *call.registers = staged;
    } else if (call.watched) {
        const WatchRecord& record = *call.watched;
        lua_createtable(L, 0, 4); lua_createtable(L, hook.signature.count, 0);
        for (uint32_t i = 0; i < hook.signature.count; ++i) {
            if (hook.signature.args[i] == CallType::text16) push_text(L, record.text + record.text_start[i], record.text_units[i]);
            else push(L, hook.signature.args[i], record.args[i]);
            lua_rawseti(L, -2, i + 1);
        }
        lua_setfield(L, -2, "args"); push(L, hook.signature.result, record.result); lua_setfield(L, -2, "result");
        lua_pushstring(L, record.game_thread ? "game" : "other"); lua_setfield(L, -2, "thread");
        lua_pushinteger(L, lua_Integer(record.dropped) + hook.missed); lua_setfield(L, -2, "dropped");
        hook.missed = 0;
        lua_call(L, 1, 0);
    } else {
        push_u64(L, call.serial); lua_pushcclosure(L, original, 1);
        for (uint32_t i = 0; i < hook.signature.count; ++i) push(L, hook.signature.args[i], frame_argument(*call.frame, hook.signature, i));
        lua_call(L, hook.signature.count + 1, 1);
        const uint64_t result = read(L, -1, hook.signature.result);
        set_frame_result(*call.frame, hook.signature.result, result);
    }
    return 0;
}
bool ready(void* user) {
    Hook& hook = *static_cast<Hook*>(user);
    Budget* used = budget(hook.L);
    return !hook.disabled && stack_room() >= tsuchi_script::kHookStack &&
           (!used || ((!used->depth || !turn_over(hook.L)) && used->hooks.remaining(engine_frame_number(), used->run)));
}
bool run(Call& call) {
    Hook& hook = *call.hook; lua_State* L = hook.L;
    if (!lua_checkstack(L, 5)) return false;
    Turn turn(L, tsuchi_script::kHookTurn);
    Budget* used = budget(L);
    const uint64_t old_limit = used ? used->limit : 0;
    // Every call gets at most its own allowance, including one that continues a turn already running. The hook
    // is switched off only when that allowance is what ran out, not the frame's or an outer turn's.
    bool own_limit = false;
    if (used) {
        const uint64_t start = used->run;
        const uint64_t own = start + tsuchi_script::kHookTurn;
        const uint64_t frame = start + used->hooks.remaining(engine_frame_number(), start);
        const uint64_t cap = own < frame ? own : frame;
        if (cap < used->limit) used->limit = cap;
        own_limit = used->limit == own && own <= frame;
        used->hooks.enter(start);
    }
    const int top = lua_gettop(L);
    call.serial = next_call++; call.previous = active; active = &call;
    lua_pushcfunction(L, invoke); lua_pushlightuserdata(L, &call);
    const int status = lua_pcall(L, 1, 0, 0);
    active = call.previous; // escaped originals can no longer find this call
    const bool over = turn_over(L);
    if (status != LUA_OK || over) {
        hook.disabled |= over && own_limit;
        const char* message = over ? "script hook exhausted its instruction budget" :
            (lua_type(L, -1) == LUA_TSTRING ? lua_tostring(L, -1) : "script hook raised an error");
        size_t n = strlen(message); if (n >= sizeof hook.error) n = sizeof hook.error - 1;
        memcpy(hook.error, message, n); hook.error[n] = 0;
    }
    lua_settop(L, top);
    if (used) { used->hooks.leave(used->run); used->limit = old_limit; }
    return status == LUA_OK && !over;
}
bool wrap(void* user, CallFrame& frame, const DynamicOriginal& next) {
    Call call{static_cast<Hook*>(user), &frame, &next};
    if (run(call)) return true;
    if (!call.called) return false;
    set_frame_result(frame, next.signature->result, call.last); return true;
}
bool instruction(void* user, InlineRegisters& registers) {
    Call call{static_cast<Hook*>(user)}; call.registers = &registers; return run(call);
}
void watch(void* user, const WatchRecord& record) {
    if (!ready(user)) {
        Hook& hook = *static_cast<Hook*>(user);
        if (hook.missed < UINT32_MAX - record.dropped) hook.missed += record.dropped + 1;
        return;
    }
    Call call{static_cast<Hook*>(user)}; call.watched = &record; run(call);
}
void release_hook(void* user) {
    Hook& hook = *static_cast<Hook*>(user); lua_State* L = hook.L;
    luaL_unref(L, LUA_REGISTRYINDEX, hook.fn); hook = Hook{};
    unpin_hook(L);
}
void report_errors() {
    for (Hook& hook : hooks) if (hook.L) {
        if (Budget* used = budget(hook.L)) used->hooks.remaining(engine_frame_number(), used->run);
        if (!hook.error[0]) continue;
        if (mod_running(mod(hook.L))) report_hook_error(hook.L, hook.error);
        hook.error[0] = 0;
    }
}
int push_installed(lua_State* L) { push_handle(L, "hook_id", check_u64(L, 1)); return 1; }
int install(lua_State* L, DynamicKind kind) {
    if (!mod_usable(mod(L))) return fail(L, TSUCHI_INVALID);
    DynamicTarget target{}; Signature sig{};
    int fn = 2;
    if (kind == DynamicKind::inline_hook) {
        target = {DynamicSite::instruction, check_u64(L, 1), uint64_t(check_integer(L, 2, 0, UINT32_MAX))}; fn = 3;
    } else {
        luaL_checktype(L, 1, LUA_TTABLE);
        field(L, 1, "function_at"); const bool function = !lua_isnil(L, -1); lua_pop(L, 1);
        field(L, 1, "entry_at"); const bool entry = !lua_isnil(L, -1); lua_pop(L, 1);
        field(L, 1, "slot"); const bool slot = !lua_isnil(L, -1); lua_pop(L, 1);
        if (int(function) + int(entry) + int(slot) != 1) return fail(L, TSUCHI_INVALID);
        if (slot) {
            // A name from the game table: the same entry on every build the table knows.
            field(L, 1, "slot"); const char* name = luaL_checkstring(L, -1);
            const TSUCHI_Result found = hook_slot_entry(name, target.offset, target.check); lua_pop(L, 1);
            if (found != TSUCHI_OK) return fail(L, found);
            target.site = DynamicSite::entry;
        } else {
            target.site = function ? DynamicSite::function : DynamicSite::entry;
            field(L, 1, function ? "function_at" : "entry_at"); target.offset = check_u64(L, -1); lua_pop(L, 1);
            field(L, 1, function ? "expected" : "stock");
            // `stock` may be left out when the function the entry holds isn't known.
            if (!function && lua_isnil(L, -1)) target.check = 0;
            else target.check = function ? uint64_t(check_integer(L, -1, 0, UINT32_MAX)) : check_u64(L, -1);
            lua_pop(L, 1);
        }
        field(L, 1, "signature"); luaL_checktype(L, -1, LUA_TSTRING);
        size_t size = 0; const char* text = lua_tolstring(L, -1, &size);
        const TSUCHI_Result parsed = strlen(text) == size ? parse_signature(text, sig) : TSUCHI_INVALID; lua_pop(L, 1);
        if (parsed != TSUCHI_OK) return fail(L, parsed);
    }
    luaL_checktype(L, fn, LUA_TFUNCTION);
    Hook* hook = nullptr; for (Hook& candidate : hooks) if (!candidate.L) { hook = &candidate; break; }
    if (!hook || !each_dynamic_frame(report_errors)) return fail(L, TSUCHI_CAPACITY);
    lua_pushvalue(L, fn); const int ref = luaL_ref(L, LUA_REGISTRYINDEX);
    *hook = Hook{}; hook->L = main_state(L); hook->fn = ref; hook->signature = sig; pin_hook(L);
    uint64_t id = 0;
    const DynamicClient client{wrap, instruction, watch, ready, release_hook};
    const TSUCHI_Result result = dynamic_hook(mod(L), kind, target, sig, client, hook, &id);
    if (result != TSUCHI_OK) { release_hook(hook); return fail(L, result); }
    lua_pushcfunction(L, push_installed); push_u64(L, id);
    if (lua_pcall(L, 1, 1, 0) != LUA_OK) { dynamic_remove(mod(L), id); return lua_error(L); }
    return 1;
}
} // namespace
int tsuchi_lua_hook_wrap(lua_State* L) { return install(L, tsuchi::DynamicKind::wrap); }
int tsuchi_lua_hook_watch(lua_State* L) { return install(L, tsuchi::DynamicKind::watch); }
int tsuchi_lua_hook_instruction(lua_State* L) { return install(L, tsuchi::DynamicKind::inline_hook); }
