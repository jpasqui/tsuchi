#include "tsuchi_js.h"
#include "dynamic_hooks.h"
#include "registry.h"

namespace {
using namespace tsuchi;
using namespace tsuchi_js;
struct Hook {
    JSContext* ctx = nullptr;
    JSValue fn = JS_UNDEFINED;
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
    bool called = false, original_open = false;
    Call* previous = nullptr;
};
Call* active = nullptr;
uint64_t next_call = 1;
JSValue push_text(JSContext* ctx, const uint16_t* text, size_t units) {
    char bytes[kWatchTextUnits * 3];
    const size_t count = tsuchi_script::utf8(text, units, bytes);
    return JS_NewStringLen(ctx, bytes, count);
}
JSValue push(JSContext* ctx, CallType type, uint64_t bits) {
    switch (type) {
    case CallType::void_: return JS_UNDEFINED;
    case CallType::boolean: return JS_NewBool(ctx, bits != 0);
    case CallType::i32: return JS_NewInt32(ctx, int32_t(bits));
    case CallType::u32: return JS_NewUint32(ctx, uint32_t(bits));
    case CallType::f32: {
        const uint32_t low = uint32_t(bits); float value; memcpy(&value, &low, sizeof value); return JS_NewFloat64(ctx, value);
    }
    case CallType::text16: {
        if (!bits) return JS_NULL;
        uint16_t text[kWatchTextUnits + 1];
        const uint32_t units = read_text16(bits, text, kWatchTextUnits + 1);
        if (units > kWatchTextUnits) return JS_ThrowRangeError(ctx, "hook text exceeds 256 UTF-16 units");
        return push_text(ctx, text, units);
    }
    default: return JS_NewBigUint64(ctx, bits);
    }
}
// `game` is what the game passed in the same place, for a text16 argument the script may hand on unchanged.
uint64_t read(JSContext* ctx, JSValueConst value, CallType type, uint16_t* text = nullptr, uint64_t game = 0) {
    switch (type) {
    case CallType::void_: return 0;
    case CallType::boolean: return boolean(ctx, value);
    case CallType::i32: return uint64_t(integer(ctx, value, INT32_MIN, INT32_MAX));
    case CallType::u32: return uint64_t(integer(ctx, value, 0, UINT32_MAX));
    case CallType::f32: {
        const float f = float(number(ctx, value)); uint32_t bits; memcpy(&bits, &f, sizeof bits); return bits;
    }
    case CallType::text16: {
        if (JS_IsNull(value)) return 0;
        if (!JS_IsString(value)) { JS_ThrowTypeError(ctx, "text16 needs a string"); return 0; }
        size_t bytes = 0; const char* source = JS_ToCStringLen(ctx, &bytes, value);
        if (!source) return 0;
        if (game) {
            uint16_t current[kWatchTextUnits + 1];
            const uint32_t units = read_text16(game, current, kWatchTextUnits + 1);
            if (tsuchi_script::same_text(current, units, source, bytes)) { JS_FreeCString(ctx, source); return game; }
        }
        const bool ok = text && tsuchi_script::utf16(source, bytes, text, kWatchTextUnits + 1);
        JS_FreeCString(ctx, source);
        if (!ok) { JS_ThrowRangeError(ctx, "original needs text of at most 256 UTF-16 units without embedded NUL"); return 0; }
        return reinterpret_cast<uint64_t>(text);
    }
    default: return u64(ctx, value);
    }
}
JSValue original(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv, int, JSValue* data) {
    const uint64_t serial = u64(ctx, data[0]);
    if (JS_HasException(ctx)) return JS_EXCEPTION;
    Call* call = active;
    while (call && (call->serial != serial || call->hook->ctx != ctx)) call = call->previous;
    if (!call || !call->original || !call->original_open) return JS_ThrowTypeError(ctx, "original is no longer active");
    const Signature& sig = call->hook->signature;
    if (argc != sig.count) return JS_ThrowTypeError(ctx, "original needs every argument in its signature");
    CallFrame frame = *call->frame;
    uint16_t text[kRegisterArguments][kWatchTextUnits + 1]; unsigned strings = 0;
    for (uint32_t i = 0; i < sig.count; ++i) {
        const bool is_text = sig.args[i] == CallType::text16;
        const uint64_t bits = read(ctx, argv[i], sig.args[i], is_text ? text[strings++] : nullptr,
                                   is_text ? frame_argument(*call->frame, sig, i) : 0);
        if (JS_HasException(ctx)) return JS_EXCEPTION;
        set_frame_argument(frame, sig, i, bits);
    }
    call_with_frame(call->original->target, frame, sig.result);
    call->last = frame_result(frame, sig.result); call->called = true;
    return push(ctx, sig.result, call->last);
}
bool synchronous(JSContext* ctx, JSValueConst returned) {
    if (JS_IsException(returned) || JS_HasException(ctx)) return false;
    if (!JS_IsObject(returned)) return true;
    // Check native promises even if a script replaces their `then` property.
    if (JS_PromiseState(ctx, returned) >= 0) {
        JS_ThrowTypeError(ctx, "a hook must return synchronously"); return false;
    }
    Value then(ctx, JS_GetPropertyStr(ctx, returned, "then"));
    if (JS_IsException(then)) return false;
    if (JS_IsFunction(ctx, then)) { JS_ThrowTypeError(ctx, "a hook must return synchronously"); return false; }
    return true;
}
struct Arguments {
    JSContext* ctx;
    JSValue values[kSignatureArguments + 1];
    explicit Arguments(JSContext* c) : ctx(c) { for (auto& v : values) v = JS_UNDEFINED; }
    ~Arguments() { for (auto v : values) JS_FreeValue(ctx, v); }
};
const char* flags[] = {"n", "z", "c", "v"};
bool invoke(Call& call) {
    Hook& hook = *call.hook; JSContext* ctx = hook.ctx;
    if (call.registers) {
        Value snapshot(ctx, JS_NewObject(ctx)); Value x(ctx, JS_NewArray(ctx));
        if (JS_IsException(snapshot) || JS_IsException(x)) return false;
        for (uint32_t i = 0; i < 31; ++i)
            if (JS_SetPropertyUint32(ctx, x, i, JS_NewBigUint64(ctx, call.registers->x[i])) < 0) return false;
        if (JS_SetPropertyStr(ctx, snapshot, "x", x.take()) < 0) return false;
        for (int i = 0; i < 4; ++i)
            if (JS_SetPropertyStr(ctx, snapshot, flags[i], JS_NewBool(ctx, (call.registers->nzcv >> (31 - i)) & 1)) < 0) return false;
        JSValue argument = snapshot.value;
        Value result(ctx, JS_Call(ctx, hook.fn, JS_UNDEFINED, 1, &argument));
        if (!synchronous(ctx, result)) return false;
        InlineRegisters staged{};
        Value registers(ctx, JS_GetPropertyStr(ctx, snapshot, "x"));
        if (JS_IsException(registers)) return false;
        for (uint32_t i = 0; i < 31; ++i) {
            Value value(ctx, JS_GetPropertyUint32(ctx, registers, i));
            if (JS_IsException(value)) return false;
            staged.x[i] = u64(ctx, value); if (JS_HasException(ctx)) return false;
        }
        for (int i = 0; i < 4; ++i) {
            Value value(ctx, JS_GetPropertyStr(ctx, snapshot, flags[i]));
            if (JS_IsException(value)) return false;
            if (boolean(ctx, value)) staged.nzcv |= uint64_t(1) << (31 - i);
            if (JS_HasException(ctx)) return false;
        }
        *call.registers = staged; return true;
    }
    if (call.watched) {
        const WatchRecord& record = *call.watched;
        Value snapshot(ctx, JS_NewObject(ctx)); Value args(ctx, JS_NewArray(ctx));
        if (JS_IsException(snapshot) || JS_IsException(args)) return false;
        for (uint32_t i = 0; i < hook.signature.count; ++i) {
            JSValue value = hook.signature.args[i] == CallType::text16
                ? push_text(ctx, record.text + record.text_start[i], record.text_units[i]) : push(ctx, hook.signature.args[i], record.args[i]);
            if (JS_IsException(value) || JS_SetPropertyUint32(ctx, args, i, value) < 0) return false;
        }
        if (JS_SetPropertyStr(ctx, snapshot, "args", args.take()) < 0 ||
            JS_SetPropertyStr(ctx, snapshot, "result", push(ctx, hook.signature.result, record.result)) < 0 ||
            JS_SetPropertyStr(ctx, snapshot, "thread", JS_NewString(ctx, record.game_thread ? "game" : "other")) < 0 ||
            JS_SetPropertyStr(ctx, snapshot, "dropped", JS_NewFloat64(ctx, double(record.dropped) + hook.missed)) < 0) return false;
        hook.missed = 0;
        JSValue argument = snapshot.value;
        Value result(ctx, JS_Call(ctx, hook.fn, JS_UNDEFINED, 1, &argument));
        return synchronous(ctx, result);
    }
    Arguments args(ctx);
    Value serial(ctx, JS_NewBigUint64(ctx, call.serial)); if (JS_IsException(serial)) return false;
    args.values[0] = JS_NewCFunctionData(ctx, original, hook.signature.count, 0, 1, &serial.value);
    if (JS_IsException(args.values[0])) return false;
    for (uint32_t i = 0; i < hook.signature.count; ++i) {
        args.values[i + 1] = push(ctx, hook.signature.args[i], frame_argument(*call.frame, hook.signature, i));
        if (JS_IsException(args.values[i + 1])) return false;
    }
    call.original_open = true;
    Value result(ctx, JS_Call(ctx, hook.fn, JS_UNDEFINED, hook.signature.count + 1, args.values));
    call.original_open = false;
    if (!synchronous(ctx, result)) return false;
    const uint64_t bits = read(ctx, result, hook.signature.result);
    if (JS_HasException(ctx)) return false;
    set_frame_result(*call.frame, hook.signature.result, bits); return true;
}
bool ready(void* user) {
    Hook& hook = *static_cast<Hook*>(user);
    return !hook.disabled && (!turn_active(hook.ctx) || !turn_over(hook.ctx)) && stack_room() >= tsuchi_script::kHookStack &&
           hook_allowance(hook.ctx).remaining(engine_frame_number(), turn_used(hook.ctx));
}
bool run(Call& call) {
    Hook& hook = *call.hook; JSContext* ctx = hook.ctx;
    Turn turn(ctx, tsuchi_script::kHookTurn);
    auto& allowance = hook_allowance(ctx);
    const uint64_t old_limit = turn_limit(ctx);
    // Every call gets at most its own allowance, including one that continues a turn already running. The hook
    // is switched off only when that allowance is what ran out, not the frame's or an outer turn's.
    const uint64_t start = turn_used(ctx);
    const uint64_t own = start + tsuchi_script::kHookTurn;
    const uint64_t frame = start + allowance.remaining(engine_frame_number(), start);
    const uint64_t cap = own < frame ? own : frame;
    if (cap < old_limit) set_turn_limit(ctx, cap);
    const bool own_limit = turn_limit(ctx) == own && own <= frame;
    allowance.enter(start);
    call.serial = next_call++; call.previous = active; active = &call;
    const bool ok = invoke(call);
    active = call.previous;
    const bool over = turn_over(ctx);
    if (!ok || over) {
        hook.disabled |= over && own_limit;
        // Converting an object or reading a property through a getter can run script code, so the hook path
        // reads only a thrown string, or the message an Error holds as a plain data property of its own.
        Value error(ctx, JS_GetException(ctx));
        const char* message = over ? "script hook exhausted its instruction budget" : "script hook raised an error or returned asynchronously";
        const char* string = nullptr;
        if (!over && JS_IsString(error)) string = JS_ToCString(ctx, error);
        else if (!over && JS_IsError(ctx, error)) {
            const JSAtom atom = JS_NewAtom(ctx, "message");
            JSPropertyDescriptor own{};
            if (atom != JS_ATOM_NULL && JS_GetOwnProperty(ctx, &own, error, atom) > 0) {
                if (!(own.flags & JS_PROP_GETSET) && JS_IsString(own.value)) string = JS_ToCString(ctx, own.value);
                JS_FreeValue(ctx, own.value); JS_FreeValue(ctx, own.getter); JS_FreeValue(ctx, own.setter);
            }
            if (atom != JS_ATOM_NULL) JS_FreeAtom(ctx, atom);
        }
        if (string) message = string;
        size_t n = strlen(message); if (n >= sizeof hook.error) n = sizeof hook.error - 1;
        memcpy(hook.error, message, n); hook.error[n] = 0;
        if (string) JS_FreeCString(ctx, string);
        if (JS_HasException(ctx)) { Value ignored(ctx, JS_GetException(ctx)); }
    }
    allowance.leave(turn_used(ctx)); set_turn_limit(ctx, old_limit);
    return ok && !over;
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
    Hook& hook = *static_cast<Hook*>(user); JSContext* ctx = hook.ctx;
    JS_FreeValue(ctx, hook.fn); hook = Hook{}; unpin_hook(ctx);
}
void report_errors() {
    for (Hook& hook : hooks) if (hook.ctx) {
        hook_allowance(hook.ctx).remaining(engine_frame_number(), turn_used(hook.ctx));
        if (!hook.error[0]) continue;
        if (mod_running(mod(hook.ctx))) report_hook_error(hook.ctx, hook.error);
        hook.error[0] = 0;
    }
}
JSValue install(JSContext* ctx, int argc, JSValueConst* argv, DynamicKind kind) {
    if (!mod_usable(mod(ctx))) return fail(ctx, TSUCHI_INVALID);
    DynamicTarget target{}; Signature sig{}; int fn = 1;
    if (kind == DynamicKind::inline_hook) {
        target = {DynamicSite::instruction, u64(ctx, arg(argc, argv, 0)), uint64_t(integer(ctx, arg(argc, argv, 1), 0, UINT32_MAX))}; fn = 2;
    } else {
        JSValueConst object = arg(argc, argv, 0);
        Value function(ctx, JS_GetPropertyStr(ctx, object, "function_at"));
        if (JS_IsException(function)) return JS_EXCEPTION;
        Value entry(ctx, JS_GetPropertyStr(ctx, object, "entry_at"));
        if (JS_IsException(entry)) return JS_EXCEPTION;
        const bool is_function = !JS_IsUndefined(function), is_entry = !JS_IsUndefined(entry);
        if (is_function == is_entry) return fail(ctx, TSUCHI_INVALID);
        target.site = is_function ? DynamicSite::function : DynamicSite::entry;
        target.offset = u64(ctx, is_function ? function.value : entry.value);
        if (JS_HasException(ctx)) return JS_EXCEPTION;
        Value check(ctx, JS_GetPropertyStr(ctx, object, is_function ? "expected" : "stock"));
        if (JS_IsException(check)) return JS_EXCEPTION;
        // `stock` may be left out when the function the entry holds isn't known.
        if (!is_function && JS_IsUndefined(check)) target.check = 0;
        else target.check = is_function ? uint64_t(integer(ctx, check, 0, UINT32_MAX)) : u64(ctx, check);
        if (JS_HasException(ctx)) return JS_EXCEPTION;
        Value signature(ctx, JS_GetPropertyStr(ctx, object, "signature"));
        if (JS_IsException(signature)) return JS_EXCEPTION;
        String text(ctx, signature);
        if (JS_HasException(ctx)) return JS_EXCEPTION;
        const TSUCHI_Result parsed = text.text && strlen(text.text) == text.size ? parse_signature(text, sig) : TSUCHI_INVALID;
        if (parsed != TSUCHI_OK) return fail(ctx, parsed);
    }
    if (JS_HasException(ctx)) return JS_EXCEPTION;
    if (!JS_IsFunction(ctx, arg(argc, argv, fn))) return fail(ctx, TSUCHI_INVALID);
    Hook* hook = nullptr; for (Hook& candidate : hooks) if (!candidate.ctx) { hook = &candidate; break; }
    if (!hook || !each_dynamic_frame(report_errors)) return fail(ctx, TSUCHI_CAPACITY);
    *hook = Hook{}; hook->ctx = ctx; hook->fn = JS_DupValue(ctx, argv[fn]); hook->signature = sig; pin_hook(ctx);
    uint64_t id = 0;
    const DynamicClient client{wrap, instruction, watch, ready, release_hook};
    const TSUCHI_Result result = dynamic_hook(mod(ctx), kind, target, sig, client, hook, &id);
    if (result != TSUCHI_OK) { release_hook(hook); return fail(ctx, result); }
    JSValue handle = push_handle(ctx, "hook_id", id);
    if (JS_IsException(handle)) dynamic_remove(mod(ctx), id);
    return handle;
}
} // namespace
JSValue tsuchi_js_hook_wrap(JSContext* ctx, int argc, JSValueConst* argv) { return install(ctx, argc, argv, tsuchi::DynamicKind::wrap); }
JSValue tsuchi_js_hook_watch(JSContext* ctx, int argc, JSValueConst* argv) { return install(ctx, argc, argv, tsuchi::DynamicKind::watch); }
JSValue tsuchi_js_hook_instruction(JSContext* ctx, int argc, JSValueConst* argv) { return install(ctx, argc, argv, tsuchi::DynamicKind::inline_hook); }
