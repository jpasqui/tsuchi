// hook.engine_function for JavaScript: the object each handler gets, and the changes it makes, on top of the
// engine function hooks in framework/core/engine_hooks.h and the script hook runner in script_hooks.cpp.
#include <stdio.h>

#include "tsuchi_js.h"
#include "dynamic_hooks.h"
#include "engine_hooks.h"
#include "registry.h"

namespace {
using namespace tsuchi;
using namespace tsuchi_js;

struct Hook {
    JSContext* ctx = nullptr;
    JSValue before = JS_UNDEFINED, after = JS_UNDEFINED;
    bool disabled = false;
    char error[256]{};
};
Hook hooks[kEngineHooks];

constexpr uint32_t kWrites = 64;
// One handler's call. The changes it makes are copied here while it runs, and written once it has returned.
struct Call {
    Hook* hook = nullptr;
    const EngineCall* engine = nullptr;
    bool after = false;
    ScriptWriteTree tree;
    EngineWrite writes[kWrites + 1]{};
    uint32_t count = 0;
};

// A name into `small` when it fits there, or into `buffer`. Null when there is no such name.
template <class Read>
const char* read_name(Buffer& buffer, char* small, size_t capacity, Read read) {
    uint64_t needed = 0;
    const TSUCHI_Result result = read(small, capacity, &needed);
    if (result != TSUCHI_CAPACITY) return result == TSUCHI_OK ? small : nullptr;
    if (!buffer.resize(needed)) return nullptr;
    return read(static_cast<char*>(buffer.data), needed, &needed) == TSUCHI_OK ? static_cast<char*>(buffer.data) : nullptr;
}

struct Keys {
    JSContext* ctx;
    JSPropertyEnum* entries = nullptr;
    uint32_t count = 0;
    explicit Keys(JSContext* c) : ctx(c) {}
    ~Keys() {
        for (uint32_t i = 0; i < count; ++i) JS_FreeAtom(ctx, entries[i].atom);
        js_free(ctx, entries);
    }
    bool read(JSValueConst value) { return JS_GetOwnPropertyNames(ctx, &entries, &count, value, JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) == 0; }
};

// True when two values hold the same: the same plain value or handle, or objects with the same keys and values.
bool same(JSContext* ctx, JSValueConst a, JSValueConst b, unsigned depth) {
    const bool objects = JS_IsObject(a) && JS_IsObject(b);
    if (!objects) return JS_SameValue(ctx, a, b);
    uint64_t first = 0, second = 0;
    const bool handle_a = to_handle(ctx, a, &first), handle_b = to_handle(ctx, b, &second);
    if (handle_a || handle_b) return handle_a && handle_b && first == second;
    if (depth > 16 || JS_IsArray(ctx, a) != JS_IsArray(ctx, b)) return false;
    Keys keys_a(ctx), keys_b(ctx);
    if (!keys_a.read(a) || !keys_b.read(b) || keys_a.count != keys_b.count) return false;
    for (uint32_t i = 0; i < keys_a.count; ++i) {
        Value left(ctx, JS_GetProperty(ctx, a, keys_a.entries[i].atom));
        Value right(ctx, JS_GetProperty(ctx, b, keys_a.entries[i].atom));
        if (JS_HasException(ctx) || !same(ctx, left, right, depth + 1)) return false;
    }
    return true;
}

// Copies `value` for writing to `name`, when it differs from what the call holds there now. False with an
// exception when it can't be copied.
bool note_change(JSContext* ctx, Call& call, const char* name, JSValueConst value) {
    JSValue fresh = JS_UNDEFINED;
    const bool readable = engine_value(ctx, *call.engine, name, &fresh) == TSUCHI_OK;
    Value held(ctx, fresh);
    if (JS_HasException(ctx)) return false;
    const bool unchanged = readable ? same(ctx, value, held, 0) : JS_IsUndefined(value);
    if (JS_HasException(ctx)) return false;
    if (unchanged) return true;
    if (call.count >= kWrites) {
        JS_ThrowRangeError(ctx, "a hook changed more than %u values", unsigned(kWrites));
        return false;
    }
    ScriptWrite* node = nullptr;
    const TSUCHI_Result copied = script_write(ctx, value, call.tree, &node);
    if (copied != TSUCHI_OK) {
        if (!JS_HasException(ctx)) JS_ThrowTypeError(ctx, "%s can't take that value: %s", name, result_name(copied));
        return false;
    }
    if (!node->name.set(name)) {
        JS_ThrowOutOfMemory(ctx);
        return false;
    }
    call.writes[call.count++] = {node->name.get(), node};
    return true;
}

bool invoke(void* data) {
    Call& call = *static_cast<Call*>(data);
    JSContext* ctx = call.hook->ctx;
    const EngineCall& engine = *call.engine;
    Value record(ctx, JS_NewObject(ctx)), args(ctx, JS_NewObject(ctx)), names(ctx, JS_NewArray(ctx));
    if (JS_IsException(record) || JS_IsException(args) || JS_IsException(names)) return false;
    const JSValue object = engine.object ? push_handle(ctx, engine_object_kind(engine.object), engine.object) : JS_NULL;
    if (JS_IsException(object) || JS_SetPropertyStr(ctx, record, "object", object) < 0) return false;
    char small[256];
    Buffer buffer(ctx);
    for (uint32_t i = 0;; ++i) {
        const char* name = read_name(buffer, small, sizeof small, [&](char* out, uint64_t capacity, uint64_t* needed) {
            return engine_field_at(engine, "", i, out, capacity, needed);
        });
        if (!name) break;
        if (JS_SetPropertyUint32(ctx, names, i, JS_NewString(ctx, name)) < 0) return false;
        // An argument that can't be read is left out, as reflection.get leaves out a struct's.
        JSValue value = JS_UNDEFINED;
        if (engine_value(ctx, engine, name, &value) == TSUCHI_OK && JS_SetPropertyStr(ctx, args, name, value) < 0) return false;
        if (JS_HasException(ctx)) return false;
    }
    if (JS_SetPropertyStr(ctx, record, "args", JS_DupValue(ctx, args)) < 0) return false;
    Buffer result_buffer(ctx);
    char result_small[256];
    const char* result = nullptr;
    if (call.after) {
        result = read_name(result_buffer, result_small, sizeof result_small, [&](char* out, uint64_t capacity, uint64_t* needed) {
            return engine_result_name(engine, out, capacity, needed);
        });
        JSValue value = JS_NULL;
        if (result && engine_value(ctx, engine, result, &value) != TSUCHI_OK) value = JS_NULL;
        if (JS_HasException(ctx) || JS_SetPropertyStr(ctx, record, "result", value) < 0) return false;
    }
    JSValue argument = record.value;
    Value returned(ctx, JS_Call(ctx, call.after ? call.hook->after : call.hook->before, JS_UNDEFINED, 1, &argument));
    if (!hook_returned(ctx, returned)) return false;
    Value now(ctx, JS_GetPropertyStr(ctx, record, "args"));
    if (JS_IsException(now)) return false;
    if (!JS_IsObject(now)) {
        JS_ThrowTypeError(ctx, "call.args has to stay an object");
        return false;
    }
    Value length(ctx, JS_GetPropertyStr(ctx, names, "length"));
    uint32_t count = 0;
    if (JS_ToUint32(ctx, &count, length) < 0) return false;
    for (uint32_t i = 0; i < count; ++i) {
        Value key(ctx, JS_GetPropertyUint32(ctx, names, i));
        String name(ctx, key);
        if (!name.text) return false;
        Value current(ctx, JS_GetPropertyStr(ctx, now, name));
        if (JS_IsException(current) || !note_change(ctx, call, name, current)) return false;
    }
    if (!JS_IsUndefined(returned)) {
        if (!result) {
            JS_ThrowTypeError(ctx, "the function has no result to replace");
            return false;
        }
        if (!note_change(ctx, call, result, returned)) return false;
    }
    return !JS_HasException(ctx);
}

bool handle(Hook& hook, EngineCall& engine, bool after) {
    Call call;
    call.hook = &hook;
    call.engine = &engine;
    call.after = after;
    if (!run_hook(hook.ctx, invoke, &call, hook.disabled, hook.error, sizeof hook.error)) return false;
    const TSUCHI_Result written = engine_write(engine, call.writes, call.count);
    if (written == TSUCHI_OK) return true;
    snprintf(hook.error, sizeof hook.error, "a hook's changes don't fit the function: %s", result_name(written));
    return false;
}
bool before(void* user, EngineCall& call) { return handle(*static_cast<Hook*>(user), call, false); }
bool after(void* user, EngineCall& call) { return handle(*static_cast<Hook*>(user), call, true); }
bool ready(void* user) {
    const Hook& hook = *static_cast<Hook*>(user);
    return hook_ready(hook.ctx, hook.disabled);
}
void report(void* user) {
    Hook& hook = *static_cast<Hook*>(user);
    hook_allowance(hook.ctx).remaining(engine_frame_number(), turn_used(hook.ctx));
    if (!hook.error[0]) return;
    if (mod_running(mod(hook.ctx))) report_hook_error(hook.ctx, hook.error);
    hook.error[0] = 0;
}
void release_hook(void* user) {
    Hook& hook = *static_cast<Hook*>(user);
    JSContext* ctx = hook.ctx;
    JS_FreeValue(ctx, hook.before);
    JS_FreeValue(ctx, hook.after);
    hook = Hook{};
    unpin_hook(ctx);
}
} // namespace

JSValue tsuchi_js_hook_engine_function(JSContext* ctx, int argc, JSValueConst* argv) {
    if (!mod_usable(mod(ctx))) return fail(ctx, TSUCHI_INVALID);
    String path(ctx, arg(argc, argv, 0));
    if (JS_HasException(ctx)) return JS_EXCEPTION;
    if (!path.text || strlen(path.text) != path.size) return fail(ctx, TSUCHI_INVALID);
    JSValueConst handlers = arg(argc, argv, 1);
    if (!JS_IsObject(handlers)) return fail(ctx, TSUCHI_INVALID);
    Value before_fn(ctx, JS_GetPropertyStr(ctx, handlers, "before"));
    if (JS_IsException(before_fn)) return JS_EXCEPTION;
    Value after_fn(ctx, JS_GetPropertyStr(ctx, handlers, "after"));
    if (JS_IsException(after_fn)) return JS_EXCEPTION;
    const bool has_before = !JS_IsUndefined(before_fn), has_after = !JS_IsUndefined(after_fn);
    if ((has_before && !JS_IsFunction(ctx, before_fn)) || (has_after && !JS_IsFunction(ctx, after_fn)) || (!has_before && !has_after))
        return fail(ctx, TSUCHI_INVALID);
    Hook* hook = nullptr;
    for (Hook& candidate : hooks)
        if (!candidate.ctx) {
            hook = &candidate;
            break;
        }
    if (!hook) return fail(ctx, TSUCHI_CAPACITY);
    *hook = Hook{};
    hook->ctx = ctx;
    hook->before = before_fn.take();
    hook->after = after_fn.take();
    pin_hook(ctx);
    EngineClient client;
    client.before = has_before ? before : nullptr;
    client.after = has_after ? after : nullptr;
    client.ready = ready;
    client.report = report;
    client.release = release_hook;
    uint64_t id = 0;
    const TSUCHI_Result result = engine_hook(mod(ctx), path, client, hook, &id);
    if (result != TSUCHI_OK) {
        release_hook(hook);
        return fail(ctx, result);
    }
    JSValue handle = push_handle(ctx, "hook_id", id);
    if (JS_IsException(handle)) engine_hook_remove(mod(ctx), id);
    return handle;
}
