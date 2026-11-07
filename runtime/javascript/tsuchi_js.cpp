#include "tsuchi_js.h"
#include "registry.h"
#include <stdio.h>
#include <math.h>

namespace tsuchi_js {
// A handle object's opaque data. `object` is not counted as a reference: the table only finds a handle that
// a script still holds, and the object's finalizer takes it out of the table.
struct Handle { uint64_t id; const char* kind; JSValue object; Handle* next; };
// A listener goes when it is cancelled or its subscription fails, or after the event that cancelled it, when
// that happens while the listener is being called.
struct Listener {
    JSContext* ctx; JSValue function; TSUCHI_Subscription subscription{}; bool live = false, cancelled = false;
    uint32_t delivering = 0; Listener* next;
};
struct Rejection { JSValue promise, reason; Rejection* next; };
struct Pending { const Waiter* waiter; uint64_t id; JSValue resolve; Pending* next; };
struct Subscribed { const Waiter* waiter; Subscribed* next; };
struct State {
    JSRuntime* rt = nullptr; JSContext* ctx = nullptr;
    TSUCHI_Mod* mod; const TSUCHI_Host* host;
    size_t held = 0; uint64_t run = 0; unsigned depth = 0;
    Handle** handles = nullptr; uint32_t handle_buckets = 0, handle_count = 0;
    Listener* listeners = nullptr; Pending* pending = nullptr; Subscribed* waiters = nullptr;
    JSValue told = JS_UNDEFINED; Rejection* rejections = nullptr;
    bool exhausted = false;
};
namespace {
JSClassID handle_class = 0;
uint32_t live_runtimes = 0;
void (*error_handler)(const char*) = nullptr;
State* state(JSContext* ctx) { return static_cast<State*>(JS_GetContextOpaque(ctx)); }
struct Block { size_t size; uint64_t pad; };
size_t usable(const void* p) { return p ? (static_cast<const Block*>(p) - 1)->size : 0; }
void* alloc(JSMallocState* a, size_t n) {
    auto* s = static_cast<State*>(a->opaque);
    if (n > kMemory - s->held || sizeof(Block) > kMemory - s->held - n) return nullptr;
    auto* p = static_cast<Block*>(tsuchi::allocate(n + sizeof(Block)));
    if (!p) return nullptr;
    p->size = n; s->held += n + sizeof(Block); ++a->malloc_count; a->malloc_size = s->held;
    return p + 1;
}
void dealloc(JSMallocState* a, void* p) {
    if (!p) return;
    auto* s = static_cast<State*>(a->opaque);
    s->held -= usable(p) + sizeof(Block); --a->malloc_count; a->malloc_size = s->held;
    tsuchi::release(static_cast<Block*>(p) - 1);
}
void* realloc_block(JSMallocState* a, void* p, size_t n) {
    if (!n) { dealloc(a, p); return nullptr; }
    if (!p) return alloc(a, n);
    const size_t old = usable(p);
    if (n <= old) return p;
    auto* s = static_cast<State*>(a->opaque);
    if (n - old > kMemory - s->held) return nullptr;
    // Account the replacement instead of requiring twice the budget at the boundary.
    auto* next = static_cast<Block*>(tsuchi::allocate(n + sizeof(Block)));
    if (!next) return nullptr;
    next->size = n; memcpy(next + 1, p, old);
    tsuchi::release(static_cast<Block*>(p) - 1);
    s->held += n - old; a->malloc_size = s->held; return next + 1;
}
int interrupt(JSRuntime*, void* opaque) {
    auto* s = static_cast<State*>(opaque);
    s->run += 10000; // QuickJS's JS_INTERRUPT_COUNTER_INIT, in the vendored release.
    s->exhausted = s->exhausted || s->run > kTurn;
    return s->exhausted;
}
// The stack QuickJS may use is measured from where the framework first calls in, so a call that comes back
// into the same runtime does not get a fresh allowance.
struct Turn {
    State* s;
    explicit Turn(JSContext* ctx) : s(state(ctx)) { if (!s->depth++) { begin_turn(ctx); JS_UpdateStackTop(s->rt); } }
    ~Turn() { --s->depth; }
};
uint32_t bucket_of(const State* s, uint64_t id) { return uint32_t((id * 0x9E3779B97F4A7C15ull) >> 32) & (s->handle_buckets - 1); }
// Room for one more handle in the table, which doubles when it holds as many handles as it has buckets.
bool handle_room(State* s) {
    if (s->handle_count < s->handle_buckets) return true;
    const uint32_t count = s->handle_buckets ? s->handle_buckets * 2 : 64;
    const size_t bytes = size_t(count) * sizeof(Handle*);
    if (bytes > kMemory - s->held) return false;
    auto** next = static_cast<Handle**>(tsuchi::allocate(bytes));
    if (!next) return false;
    memset(next, 0, bytes);
    Handle** old = s->handles; const uint32_t old_count = s->handle_buckets;
    s->handles = next; s->handle_buckets = count; s->held += bytes;
    for (uint32_t i = 0; i < old_count; ++i)
        for (Handle* h = old[i], *after = nullptr; h; h = after) { after = h->next; Handle*& head = s->handles[bucket_of(s, h->id)]; h->next = head; head = h; }
    if (old) { tsuchi::release(old); s->held -= size_t(old_count) * sizeof(Handle*); }
    return true;
}
void finalize_handle(JSRuntime* rt, JSValue value) {
    auto* h = static_cast<Handle*>(JS_GetOpaque(value, handle_class));
    if (!h) return;
    auto* s = static_cast<State*>(JS_GetRuntimeOpaque(rt));
    if (s && s->handles) {
        for (Handle** link = &s->handles[bucket_of(s, h->id)]; *link; link = &(*link)->next)
            if (*link == h) { *link = h->next; --s->handle_count; break; }
    }
    js_free_rt(rt, h);
}
void rejected(JSContext* ctx, JSValueConst promise, JSValueConst reason, JS_BOOL handled, void*) {
    State* s = state(ctx); if (!s) return;
    Rejection** link = &s->rejections;
    while (*link && JS_VALUE_GET_PTR((*link)->promise) != JS_VALUE_GET_PTR(promise)) link = &(*link)->next;
    if (handled) {
        if (*link) { auto* p = *link; *link = p->next; JS_FreeValue(ctx, p->promise); JS_FreeValue(ctx, p->reason); js_free(ctx, p); }
    } else if (!*link) {
        auto* p = static_cast<Rejection*>(js_malloc(ctx, sizeof(Rejection)));
        if (p) { *p = Rejection{JS_DupValue(ctx, promise), JS_DupValue(ctx, reason), nullptr}; *link = p; }
    }
}
JSValue print(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    Buffer line(ctx); size_t used = 0;
    for (int i = 0; i < argc; ++i) {
        size_t n = 0; const char* text = JS_ToCStringLen(ctx, &n, argv[i]);
        if (!text) return JS_EXCEPTION;
        if (!line.resize(used + n + 2)) { JS_FreeCString(ctx, text); return JS_EXCEPTION; }
        auto* out = static_cast<char*>(line.data);
        if (i) out[used++] = '\t';
        memcpy(out + used, text, n); used += n; out[used] = 0; JS_FreeCString(ctx, text);
    }
    if (host(ctx)->mod && host(ctx)->mod->log) host(ctx)->mod->log(mod(ctx), used ? static_cast<char*>(line.data) : "");
    return JS_UNDEFINED;
}
// Takes a listener out of the runtime's list and frees it.
void drop(JSContext* ctx, Listener* gone) {
    for (Listener** link = &state(ctx)->listeners; *link; link = &(*link)->next) {
        if (*link != gone) continue;
        *link = gone->next;
        JS_FreeValue(ctx, gone->function);
        js_free(ctx, gone);
        return;
    }
}
JSValue cancel(JSContext* ctx, JSValueConst, int, JSValueConst*, int, JSValue* data) {
    const uint64_t id = u64(ctx, data[0]);
    if (JS_HasException(ctx)) return JS_EXCEPTION;
    for (Listener* l = state(ctx)->listeners; l; l = l->next) {
        if (l->subscription.id != static_cast<uint64_t>(id) || l->cancelled) continue;
        if (!l->live) return fail(ctx, TSUCHI_NOT_FOUND);
        const TSUCHI_Result r = host(ctx)->cancel(mod(ctx), l->subscription);
        l->live = false, l->cancelled = true;
        JS_FreeValue(ctx, l->function); l->function = JS_UNDEFINED;
        if (!l->delivering) drop(ctx, l);
        return r == TSUCHI_OK ? JS_TRUE : fail(ctx, r);
    }
    return fail(ctx, TSUCHI_NOT_FOUND);
}
JSModuleDef* load_module(JSContext* ctx, const char* name, void*) {
    uint64_t n = 0, got = 0;
    TSUCHI_Result r = host(ctx)->file->size(mod(ctx), name, &n);
    Buffer source(ctx);
    if (r != TSUCHI_OK) { fail(ctx, r); return nullptr; }
    if (n == UINT64_MAX || !source.resize(n + 1)) return nullptr;
    r = host(ctx)->file->read(mod(ctx), name, 0, source.data, n, &got);
    if (r != TSUCHI_OK || got != n) { fail(ctx, r == TSUCHI_OK ? TSUCHI_IO : r); return nullptr; }
    static_cast<char*>(source.data)[n] = 0;
    Value compiled(ctx, JS_Eval(ctx, static_cast<char*>(source.data), static_cast<size_t>(n), name, JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY));
    if (JS_IsException(compiled)) return nullptr;
    return static_cast<JSModuleDef*>(JS_VALUE_GET_PTR(compiled.value));
}
Pending** pending_link(State* s, const Waiter& waiter, uint64_t id) {
    Pending** link = &s->pending;
    while (*link && ((*link)->waiter != &waiter || (*link)->id != id)) link = &(*link)->next;
    return link;
}
} // namespace
TSUCHI_Mod* mod(JSContext* ctx) { return state(ctx)->mod; }
const TSUCHI_Host* host(JSContext* ctx) { return state(ctx)->host; }
JSContext* context(State* s) { return s->ctx; }
uint32_t runtime_count() { return live_runtimes; }
size_t memory_held(State* s) { return s->held; }
uint32_t handles_held(JSContext* ctx) { return state(ctx)->handle_count; }
State* create(TSUCHI_Mod* owner, const TSUCHI_Host* api) {
    auto* s = static_cast<State*>(tsuchi::allocate(sizeof(State)));
    if (!s) return nullptr;
    *s = State{}; s->held = sizeof(State); s->mod = owner; s->host = api;
    static const JSMallocFunctions mf{alloc, dealloc, realloc_block, usable};
    s->rt = JS_NewRuntime2(&mf, s);
    if (!s->rt) { tsuchi::release(s); return nullptr; }
    JS_SetRuntimeOpaque(s->rt, s);
    JS_SetInterruptHandler(s->rt, interrupt, s); JS_SetMaxStackSize(s->rt, kStack);
    JS_SetModuleLoaderFunc(s->rt, nullptr, load_module, nullptr);
    s->ctx = JS_NewContext(s->rt);
    if (!s->ctx) { JS_FreeRuntime(s->rt); tsuchi::release(s); return nullptr; }
    JS_SetContextOpaque(s->ctx, s);
    JS_SetHostPromiseRejectionTracker(s->rt, rejected, nullptr);
    if (!handle_class) JS_NewClassID(&handle_class);
    const JSClassDef definition{"tsuchi.handle", finalize_handle, nullptr, nullptr, nullptr};
    JS_NewClass(s->rt, handle_class, &definition);
    s->told = JS_NewObject(s->ctx);
    open_services(s->ctx);
    Value global(s->ctx, JS_GetGlobalObject(s->ctx));
    JS_SetPropertyStr(s->ctx, global, "print", JS_NewCFunction(s->ctx, print, "print", 1));
    Value console(s->ctx, JS_NewObject(s->ctx));
    JS_SetPropertyStr(s->ctx, console, "log", JS_NewCFunction(s->ctx, print, "log", 1));
    JS_SetPropertyStr(s->ctx, global, "console", console.take());
    ++live_runtimes;
    return s;
}
void destroy(State* s) {
    --live_runtimes;
    JSContext* ctx = s->ctx;
    while (s->listeners) { auto* l = s->listeners; s->listeners = l->next; JS_FreeValue(ctx, l->function); js_free(ctx, l); }
    while (s->pending) { auto* p = s->pending; s->pending = p->next; JS_FreeValue(ctx, p->resolve); js_free(ctx, p); }
    while (s->waiters) { auto* w = s->waiters; s->waiters = w->next; js_free(ctx, w); }
    JS_SetHostPromiseRejectionTracker(s->rt, nullptr, nullptr);
    while (s->rejections) { auto* p = s->rejections; s->rejections = p->next; JS_FreeValue(ctx, p->promise); JS_FreeValue(ctx, p->reason); js_free(ctx, p); }
    // Freeing the runtime finalizes the handles that are left, which take themselves out of the table.
    JS_FreeValue(ctx, s->told); JS_FreeContext(ctx); JS_FreeRuntime(s->rt);
    if (s->handles) tsuchi::release(s->handles);
    tsuchi::release(s);
}
void begin_turn(JSContext* ctx) { state(ctx)->run = 0; state(ctx)->exhausted = false; }
bool turn_over(JSContext* ctx) { return state(ctx)->exhausted; }
bool drain(JSContext* ctx) {
    State* s = state(ctx);
    while (JS_IsJobPending(s->rt)) {
        // Even a chain of native promise jobs must give the game its turn back.
        if (++s->run > kTurn || s->exhausted) { s->exhausted = true; JS_ThrowInternalError(ctx, "it ran too long without giving the game its turn back"); return false; }
        JSContext* job = nullptr;
        if (JS_ExecutePendingJob(s->rt, &job) < 0) return false;
    }
    if (s->rejections) {
        auto* p = s->rejections; s->rejections = p->next;
        JS_Throw(ctx, p->reason); JS_FreeValue(ctx, p->promise); js_free(ctx, p); return false;
    }
    return !JS_HasException(ctx);
}
bool evaluate(JSContext* ctx, const char* text, size_t n, const char* filename, bool module) {
    Turn turn(ctx);
    Value result(ctx, JS_Eval(ctx, text, n, filename, module ? JS_EVAL_TYPE_MODULE : JS_EVAL_TYPE_GLOBAL));
    if (JS_IsException(result) || !drain(ctx)) return false;
    if (JS_PromiseState(ctx, result.value) == JS_PROMISE_REJECTED) {
        JS_Throw(ctx, JS_PromiseResult(ctx, result.value)); return false;
    }
    return true;
}
void set_error_handler(void (*handler)(const char*)) { error_handler = handler; }
void report(JSContext* ctx) {
    Value error(ctx, JS_GetException(ctx));
    if (turn_over(ctx)) {
        const char* message = "it ran too long without giving the game its turn back";
        if (error_handler) error_handler(message);
        else if (host(ctx)->mod && host(ctx)->mod->log) host(ctx)->mod->log(mod(ctx), message);
        return;
    }
    Value stack(ctx, JS_GetPropertyStr(ctx, error, "stack"));
    const char* message = JS_ToCString(ctx, error);
    const char* trace = JS_IsString(stack) ? JS_ToCString(ctx, stack) : nullptr;
    if (error_handler) error_handler(message ? message : "script error");
    else if (host(ctx)->mod && host(ctx)->mod->log) {
        host(ctx)->mod->log(mod(ctx), message ? message : "script error");
        if (trace) host(ctx)->mod->log(mod(ctx), trace);
    }
    if (trace) JS_FreeCString(ctx, trace);
    if (message) JS_FreeCString(ctx, message);
}
JSValue fail(JSContext* ctx, TSUCHI_Result r, const char* message) {
    Value error(ctx, JS_NewError(ctx));
    if (JS_IsException(error)) return JS_EXCEPTION;
    JS_SetPropertyStr(ctx, error, "name", JS_NewString(ctx, result_name(r)));
    JS_SetPropertyStr(ctx, error, "message", JS_NewString(ctx, message ? message : result_message(r)));
    return JS_Throw(ctx, error.take());
}
void deprecated(JSContext* ctx, const char* what, unsigned version, const char* use) {
    Value prior(ctx, JS_GetPropertyStr(ctx, state(ctx)->told, what));
    if (!JS_IsUndefined(prior)) return;
    JS_SetPropertyStr(ctx, state(ctx)->told, what, JS_TRUE);
    char line[256]; snprintf(line, sizeof line, "%s is deprecated since version %u of the framework: use %s", what, version, use);
    if (host(ctx)->mod && host(ctx)->mod->log) host(ctx)->mod->log(mod(ctx), line);
}
JSValueConst arg(int argc, JSValueConst* argv, int i) { return i < argc ? argv[i] : JS_UNDEFINED; }
bool absent(JSValueConst v) { return JS_IsUndefined(v) || JS_IsNull(v); }
String::String(JSContext* c, JSValueConst v, const char* fallback, bool binary) : ctx(c) {
    if (fallback && absent(v)) { Value d(c, JS_NewString(c, fallback)); text = JS_ToCStringLen(c, &size, d); }
    else if (!JS_IsString(v)) JS_ThrowTypeError(c, "expected a string");
    else text = JS_ToCStringLen(c, &size, v);
    if (text && !binary && strlen(text) != size) JS_ThrowTypeError(c, "text contains a zero byte");
}
bool Buffer::resize(uint64_t count, size_t width) {
    if (count > kMemory / width) { JS_ThrowOutOfMemory(ctx); return false; }
    void* next = js_realloc(ctx, data, count ? static_cast<size_t>(count) * width : 1);
    if (!next) return false; data = next; return true;
}
double number(JSContext* ctx, JSValueConst v) {
    double out = 0;
    if (!JS_IsNumber(v)) JS_ThrowTypeError(ctx, "expected a number");
    else JS_ToFloat64(ctx, &out, v);
    return out;
}
int64_t integer(JSContext* ctx, JSValueConst v, int64_t low, int64_t high) {
    const double n = number(ctx, v);
    if (JS_HasException(ctx)) return 0;
    if (!isfinite(n) || n < static_cast<double>(low) || n > static_cast<double>(high) || floor(n) != n) {
        JS_ThrowRangeError(ctx, "the number is out of range"); return 0;
    }
    return static_cast<int64_t>(n);
}
uint64_t u64(JSContext* ctx, JSValueConst v) {
    if (!JS_IsBigInt(ctx, v)) { JS_ThrowTypeError(ctx, "expected a BigInt for u64"); return 0; }
    // Decimal parsing checks the range before QuickJS's modulo-64 conversion can truncate it.
    const char* text = JS_ToCString(ctx, v); if (!text) return 0;
    uint64_t out = 0; bool valid = *text != 0;
    for (const char* p = text; *p; ++p) {
        if (*p < '0' || *p > '9' || out > (UINT64_MAX - static_cast<unsigned>(*p - '0')) / 10) { valid = false; break; }
        out = out * 10 + static_cast<unsigned>(*p - '0');
    }
    JS_FreeCString(ctx, text);
    if (!valid) JS_ThrowRangeError(ctx, "the BigInt is out of range");
    return valid ? out : 0;
}
int64_t i64(JSContext* ctx, JSValueConst v) {
    if (!JS_IsBigInt(ctx, v)) { JS_ThrowTypeError(ctx, "expected a BigInt for i64"); return 0; }
    const char* text = JS_ToCString(ctx, v); if (!text) return 0;
    bool negative = *text == '-'; const char* p = text + negative;
    uint64_t out = 0, limit = negative ? uint64_t(1) << 63 : INT64_MAX; bool valid = *p != 0;
    for (; *p; ++p) {
        if (*p < '0' || *p > '9' || out > (limit - static_cast<unsigned>(*p - '0')) / 10) { valid = false; break; }
        out = out * 10 + static_cast<unsigned>(*p - '0');
    }
    JS_FreeCString(ctx, text);
    if (!valid) { JS_ThrowRangeError(ctx, "the BigInt is out of range"); return 0; }
    return negative ? -1 - static_cast<int64_t>(out - 1) : static_cast<int64_t>(out);
}
bool boolean(JSContext* ctx, JSValueConst v) {
    if (!JS_IsBool(v)) { JS_ThrowTypeError(ctx, "expected a boolean"); return false; }
    return JS_ToBool(ctx, v) != 0;
}
uint64_t handle(JSContext* ctx, JSValueConst v, const char* kind, bool optional) {
    if (optional && absent(v)) return 0;
    const auto* h = static_cast<Handle*>(JS_GetOpaque(v, handle_class));
    bool object = h && (!strcmp(h->kind, "object") || !strcmp(h->kind, "actor") || !strcmp(h->kind, "component"));
    if (!h || (strcmp(h->kind, kind) && !(object && !strcmp(kind, "object")))) {
        JS_ThrowTypeError(ctx, "expected a %s handle", kind); return 0;
    }
    return h->id;
}
bool to_handle(JSContext*, JSValueConst v, uint64_t* id) {
    const auto* h = static_cast<Handle*>(JS_GetOpaque(v, handle_class));
    if (!h || (strcmp(h->kind, "object") && strcmp(h->kind, "actor") && strcmp(h->kind, "component"))) return false;
    *id = h->id; return true;
}
// The same handle gives the same object for as long as a script holds it, so handles compare with ===.
JSValue push_handle(JSContext* ctx, const char* kind, uint64_t id) {
    if (!id) return JS_NULL;
    State* s = state(ctx);
    if (s->handle_buckets)
        for (Handle* h = s->handles[bucket_of(s, id)]; h; h = h->next)
            if (h->id == id && !strcmp(h->kind, kind)) return JS_DupValue(ctx, h->object);
    auto* h = static_cast<Handle*>(js_malloc(ctx, sizeof(Handle))); if (!h) return JS_EXCEPTION;
    JSValue value = JS_NewObjectClass(ctx, handle_class);
    if (JS_IsException(value)) { js_free(ctx, h); return value; }
    // Making the object can collect others, so the table is grown after it.
    *h = Handle{id, kind, value, nullptr};
    JS_SetOpaque(value, h);
    if (!handle_room(s)) { JS_FreeValue(ctx, value); return JS_ThrowOutOfMemory(ctx); }
    Handle*& head = s->handles[bucket_of(s, id)]; h->next = head; head = h; ++s->handle_count;
    JS_PreventExtensions(ctx, value); return value;
}
TSUCHI_Vec3 vec3(JSContext* ctx, JSValueConst v) {
    Value x(ctx, JS_GetPropertyStr(ctx, v, "x")), y(ctx, JS_GetPropertyStr(ctx, v, "y")), z(ctx, JS_GetPropertyStr(ctx, v, "z"));
    return {static_cast<float>(number(ctx, x)), static_cast<float>(number(ctx, y)), static_cast<float>(number(ctx, z))};
}
TSUCHI_Rotation rotation(JSContext* ctx, JSValueConst v) {
    Value p(ctx, JS_GetPropertyStr(ctx, v, "pitch")), y(ctx, JS_GetPropertyStr(ctx, v, "yaw")), r(ctx, JS_GetPropertyStr(ctx, v, "roll"));
    return {static_cast<float>(number(ctx, p)), static_cast<float>(number(ctx, y)), static_cast<float>(number(ctx, r))};
}
JSValue push_vec3(JSContext* ctx, const TSUCHI_Vec3& v) {
    Value out(ctx, JS_NewObject(ctx));
    JS_SetPropertyStr(ctx, out, "x", JS_NewFloat64(ctx, v.x)); JS_SetPropertyStr(ctx, out, "y", JS_NewFloat64(ctx, v.y)); JS_SetPropertyStr(ctx, out, "z", JS_NewFloat64(ctx, v.z));
    return out.take();
}
JSValue push_rotation(JSContext* ctx, const TSUCHI_Rotation& v) {
    Value out(ctx, JS_NewObject(ctx));
    JS_SetPropertyStr(ctx, out, "pitch", JS_NewFloat64(ctx, v.pitch)); JS_SetPropertyStr(ctx, out, "yaw", JS_NewFloat64(ctx, v.yaw)); JS_SetPropertyStr(ctx, out, "roll", JS_NewFloat64(ctx, v.roll));
    return out.take();
}
uint32_t enumeration(JSContext* ctx, JSValueConst v, const char* const* names, const uint32_t* values) {
    String text(ctx, v); if (JS_HasException(ctx)) return 0;
    for (int i = 0; names[i]; ++i) if (!strcmp(text, names[i])) return values[i];
    JS_ThrowTypeError(ctx, "unknown enum value"); return 0;
}
JSValue push_enum(JSContext* ctx, uint32_t value, const char* const* names, const uint32_t* values) {
    for (int i = 0; names[i]; ++i) if (values[i] == value) return JS_NewString(ctx, names[i]);
    char text[16]; snprintf(text, sizeof text, "#%u", value); return JS_NewString(ctx, text);
}
uint32_t flags(JSContext* ctx, JSValueConst v, const char* const* names, const uint32_t* values) {
    if (absent(v)) return 0;
    if (JS_IsArray(ctx, v) != 1) { JS_ThrowTypeError(ctx, "expected an array of flag names"); return 0; }
    Value length(ctx, JS_GetPropertyStr(ctx, v, "length")); uint32_t n = 0; JS_ToUint32(ctx, &n, length);
    uint32_t out = 0;
    for (uint32_t i = 0; i < n && !JS_HasException(ctx); ++i) { Value item(ctx, JS_GetPropertyUint32(ctx, v, i)); out |= enumeration(ctx, item, names, values); }
    return out;
}
JSValue push_flags(JSContext* ctx, uint32_t bits, const char* const* names, const uint32_t* values) {
    Value out(ctx, JS_NewArray(ctx)); uint32_t n = 0;
    for (int i = 0; names[i]; ++i) if (bits & values[i]) { JS_SetPropertyUint32(ctx, out, n++, JS_NewString(ctx, names[i])); bits &= ~values[i]; }
    for (uint32_t bit = 1; bits; bit <<= 1) if (bits & bit) { char text[16]; snprintf(text, sizeof text, "#%u", bit); JS_SetPropertyUint32(ctx, out, n++, JS_NewString(ctx, text)); bits &= ~bit; }
    return out.take();
}
JSValue push_bytes(JSContext* ctx, const void* data, size_t n) {
    Value buffer(ctx, JS_NewArrayBufferCopy(ctx, static_cast<const uint8_t*>(data), n));
    if (JS_IsException(buffer)) return JS_EXCEPTION;
    // QuickJS reads the offset and length arguments whatever argc says, so all three are passed.
    JSValue arguments[3] = {buffer.value, JS_UNDEFINED, JS_UNDEFINED};
    return JS_NewTypedArray(ctx, 3, arguments, JS_TYPED_ARRAY_UINT8);
}
const uint8_t* bytes(JSContext* ctx, JSValueConst v, size_t* n) {
    size_t offset = 0, width = 0; Value buffer(ctx, JS_GetTypedArrayBuffer(ctx, v, &offset, n, &width));
    if (JS_IsException(buffer)) return nullptr;
    if (width != 1) { JS_ThrowTypeError(ctx, "expected Uint8Array"); return nullptr; }
    size_t total = 0; uint8_t* data = JS_GetArrayBuffer(ctx, &total, buffer);
    if (!data && total) return nullptr; return data ? data + offset : nullptr;
}
Listener* listen(JSContext* ctx, JSValueConst fn) {
    if (!JS_IsFunction(ctx, fn)) { JS_ThrowTypeError(ctx, "expected a function"); return nullptr; }
    auto* l = static_cast<Listener*>(js_malloc(ctx, sizeof(Listener))); if (!l) return nullptr;
    *l = Listener{ctx, JS_DupValue(ctx, fn), {}, false, false, 0, state(ctx)->listeners}; state(ctx)->listeners = l; return l;
}
JSValue subscribed(JSContext* ctx, Listener* l, TSUCHI_Result r, TSUCHI_Subscription sub, const char* message) {
    if (r != TSUCHI_OK) { drop(ctx, l); return fail(ctx, r, message); }
    l->subscription = sub; l->live = true;
    Value out(ctx, JS_NewObject(ctx)); JSValue id = JS_NewBigUint64(ctx, sub.id);
    JS_SetPropertyStr(ctx, out, "cancel", JS_NewCFunctionData(ctx, cancel, 0, 0, 1, &id)); JS_FreeValue(ctx, id); return out.take();
}
JSContext* listener_context(Listener* l) { return l->ctx; }
bool listening(Listener* l) { return l->live; }
void deliver(Listener* l, JSValue event) {
    JSContext* ctx = l->ctx;
    {
        Value value(ctx, event); if (!l->live) return;
        Turn turn(ctx);
        if (JS_HasException(ctx)) { report(ctx); return; }
        ++l->delivering;
        Value function(ctx, JS_DupValue(ctx, l->function));
        JSValue a = value.value; Value result(ctx, JS_Call(ctx, function, JS_UNDEFINED, 1, &a));
        if (JS_IsException(result) || !drain(ctx)) { if (turn_over(ctx)) l->live = false; report(ctx); }
        else if (JS_PromiseState(ctx, result.value) == JS_PROMISE_REJECTED) { JS_Throw(ctx, JS_PromiseResult(ctx, result.value)); report(ctx); }
        --l->delivering;
    }
    if (!l->delivering && l->cancelled) drop(ctx, l);
}
JSValue resolved(JSContext* ctx, JSValue value) {
    Value owned(ctx, value); JSValue fn[2]; Value promise(ctx, JS_NewPromiseCapability(ctx, fn));
    if (JS_IsException(promise)) return JS_EXCEPTION;
    Value resolve(ctx, fn[0]), reject(ctx, fn[1]); JSValue a = owned.value;
    Value result(ctx, JS_Call(ctx, resolve, JS_UNDEFINED, 1, &a)); return JS_IsException(result) ? JS_EXCEPTION : promise.take();
}
JSValue wait_for(JSContext* ctx, const Waiter& waiter, uint64_t id) {
    State* s = state(ctx);
    // One native subscription for each waited event and runtime; registry cleanup precedes runtime cleanup.
    bool subscribed = false;
    for (Subscribed* w = s->waiters; w && !subscribed; w = w->next) subscribed = w->waiter == &waiter;
    if (!subscribed) {
        auto* w = static_cast<Subscribed*>(js_malloc(ctx, sizeof(Subscribed))); if (!w) return JS_EXCEPTION;
        TSUCHI_Subscription sub{};
        const TSUCHI_Result r = waiter.subscribe(mod(ctx), host(ctx), s, &sub);
        if (r != TSUCHI_OK) { js_free(ctx, w); return fail(ctx, r); }
        *w = Subscribed{&waiter, s->waiters}; s->waiters = w;
    }
    JSValue fn[2]; Value promise(ctx, JS_NewPromiseCapability(ctx, fn));
    if (JS_IsException(promise)) return JS_EXCEPTION;
    Value resolve(ctx, fn[0]), reject(ctx, fn[1]);
    auto* p = static_cast<Pending*>(js_malloc(ctx, sizeof(Pending))); if (!p) return JS_EXCEPTION;
    *p = Pending{&waiter, id, resolve.take(), s->pending}; s->pending = p;
    JS_SetPropertyStr(ctx, promise, waiter.field, push_handle(ctx, waiter.kind, id)); return promise.take();
}
JSContext* waiting(void* user, const Waiter& waiter, uint64_t id) {
    auto* s = static_cast<State*>(user);
    return *pending_link(s, waiter, id) ? s->ctx : nullptr;
}
void settle(JSContext* ctx, const Waiter& waiter, uint64_t id, JSValue event) {
    Value value(ctx, event);
    State* s = state(ctx);
    Pending** link = pending_link(s, waiter, id);
    if (!*link) return;
    Pending* p = *link; *link = p->next;
    Turn turn(ctx);
    JSValue argument = value.value;
    Value result(ctx, JS_Call(ctx, p->resolve, JS_UNDEFINED, 1, &argument));
    JS_FreeValue(ctx, p->resolve); js_free(ctx, p);
    if (JS_IsException(result) || !drain(ctx)) report(ctx);
}
} // namespace tsuchi_js