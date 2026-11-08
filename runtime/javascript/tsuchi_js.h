// Shared support for schema-generated JavaScript bindings.
#pragma once
extern "C" {
#include "quickjs.h"
}
#include "tsuchi/tsuchi.h"
#include <string.h>
#include "../script_hooks.h"
namespace tsuchi_js {
constexpr uint64_t kTurn = 20'000'000;
constexpr size_t kMemory = size_t(128) << 20;
// The native stack QuickJS may use below where the framework calls in. The game thread has 1 MB; the
// engine and the framework use a few kilobytes above the frame hook, and the rest is left for what a
// binding calls in the engine at the deepest point. See DESIGN.md, "JavaScript".
constexpr size_t kStack = size_t(256) << 10;
struct State;
State* create(TSUCHI_Mod*, const TSUCHI_Host*);
void destroy(State*);
JSContext* context(State*);
uint32_t runtime_count();
size_t memory_held(State*);
// The handle objects a runtime's scripts hold now.
uint32_t handles_held(JSContext*);
TSUCHI_Mod* mod(JSContext*);
const TSUCHI_Host* host(JSContext*);
class Turn {
    State* s;
public:
    explicit Turn(JSContext*, uint64_t limit = kTurn);
    ~Turn();
    Turn(const Turn&) = delete;
    Turn& operator=(const Turn&) = delete;
};
void pin_hook(JSContext*);
void unpin_hook(JSContext*);
tsuchi_script::Allowance& hook_allowance(JSContext*);
bool turn_active(JSContext*);
uint64_t turn_used(JSContext*);
uint64_t turn_limit(JSContext*);
void set_turn_limit(JSContext*, uint64_t);
void report_hook_error(JSContext*, const char*);
void begin_turn(JSContext*);
bool turn_over(JSContext*);
bool drain(JSContext*);
bool evaluate(JSContext*, const char*, size_t, const char*, bool module);
void report(JSContext*);
void set_error_handler(void (*)(const char*));
void open_services(JSContext*);
const char* result_name(TSUCHI_Result);
const char* result_message(TSUCHI_Result);
JSValue fail(JSContext*, TSUCHI_Result, const char* message = nullptr);
void deprecated(JSContext*, const char*, unsigned, const char*);
struct Value {
    JSContext* ctx; JSValue value;
    Value(JSContext* c, JSValue v) : ctx(c), value(v) {}
    ~Value() { JS_FreeValue(ctx, value); }
    Value(const Value&) = delete;
    operator JSValue() const { return value; }
    JSValue take() { JSValue v = value; value = JS_UNDEFINED; return v; }
};
struct String {
    JSContext* ctx; const char* text = nullptr; size_t size = 0;
    String(JSContext*, JSValueConst, const char* fallback = nullptr, bool bytes = false);
    ~String() { if (text) JS_FreeCString(ctx, text); }
    String(const String&) = delete;
    operator const char*() const { return text; }
};
struct Buffer {
    JSContext* ctx; void* data = nullptr;
    explicit Buffer(JSContext* c) : ctx(c) {}
    ~Buffer() { js_free(ctx, data); }
    bool resize(uint64_t count, size_t width = 1);
};
JSValueConst arg(int, JSValueConst*, int);
bool absent(JSValueConst);
double number(JSContext*, JSValueConst);
int64_t integer(JSContext*, JSValueConst, int64_t, int64_t);
uint64_t u64(JSContext*, JSValueConst);
int64_t i64(JSContext*, JSValueConst);
bool boolean(JSContext*, JSValueConst);
uint64_t handle(JSContext*, JSValueConst, const char*, bool optional);
bool to_handle(JSContext*, JSValueConst, uint64_t*);
JSValue push_handle(JSContext*, const char*, uint64_t);
TSUCHI_Vec3 vec3(JSContext*, JSValueConst);
TSUCHI_Rotation rotation(JSContext*, JSValueConst);
JSValue push_vec3(JSContext*, const TSUCHI_Vec3&);
JSValue push_rotation(JSContext*, const TSUCHI_Rotation&);
uint32_t enumeration(JSContext*, JSValueConst, const char* const*, const uint32_t*);
JSValue push_enum(JSContext*, uint32_t, const char* const*, const uint32_t*);
uint32_t flags(JSContext*, JSValueConst, const char* const*, const uint32_t*);
JSValue push_flags(JSContext*, uint32_t, const char* const*, const uint32_t*);
JSValue push_bytes(JSContext*, const void*, size_t);
const uint8_t* bytes(JSContext*, JSValueConst, size_t*);
struct Listener;
Listener* listen(JSContext*, JSValueConst);
JSValue subscribed(JSContext*, Listener*, TSUCHI_Result, TSUCHI_Subscription, const char* message = nullptr);
JSContext* listener_context(Listener*);
bool listening(Listener*);
void deliver(Listener*, JSValue);
// A call that waits for an event of its service, as the schema's `waits` says: the event subscribed to once
// for each runtime, and the kind and field name of the handle that ties an event to its call.
struct Waiter {
    TSUCHI_Result (*subscribe)(TSUCHI_Mod*, const TSUCHI_Host*, void* user, TSUCHI_Subscription*);
    const char* kind;
    const char* field;
};
// A Promise for the event that will name `id`, which also carries the handle under the waiter's field name.
JSValue wait_for(JSContext*, const Waiter&, uint64_t id);
// The context of the runtime `user` when one of its calls waits for the event naming `id`, or null.
JSContext* waiting(void* user, const Waiter&, uint64_t id);
// Resolves that call's Promise with the event. Takes the value.
void settle(JSContext*, const Waiter&, uint64_t id, JSValue event);
// A Promise already resolved with the value. Takes the value.
JSValue resolved(JSContext*, JSValue);
void register_script_mods();
}