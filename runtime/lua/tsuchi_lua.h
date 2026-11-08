// What the generated Lua bindings are built on. Hand-written; the bindings
// themselves come from the schema (tools/gen/lua_binding.py).
#pragma once
#include <string.h>
#include "../script_hooks.h"

extern "C" {
#include "lauxlib.h"
#include "lua.h"
}
#include "tsuchi/tsuchi.h"

namespace tsuchi_lua {

// Sets up one Lua state for one mod: handle types, subscriptions and every service's table.
void open(lua_State* L, TSUCHI_Mod* mod, const TSUCHI_Host* host);
TSUCHI_Mod* mod(lua_State* L);
const TSUCHI_Host* host(lua_State* L);

// Where the errors of a script's handlers go. Unless one is set here, they go to the log under the mod's name.
void set_error_handler(void (*report)(const char* message));

// A failed call returns nil and the result's name ("not_found", "busy", ...).
int fail(lua_State* L, TSUCHI_Result result);
const char* result_name(TSUCHI_Result result); // generated
// Tells the mod, in its log, that what its script called is deprecated since `version` of the framework and
// what to use in its place. Once per name in each mod's script state. The call itself goes on as before.
void deprecated(lua_State* L, const char* what, unsigned version, const char* use);

// A handle is a userdata of its own type per kind. The same id gives the same
// userdata for as long as a script holds it, so handles work as table keys.
// The kind "object" accepts engine object handles, including actors and components.
// Resource handles keep their own kind and never stand in for engine objects.
uint64_t check_handle(lua_State* L, int index, const char* kind, bool optional);
void push_handle(lua_State* L, const char* kind, uint64_t id); // nil for 0
// True, with its id, when the value is an engine object handle. Raises nothing.
bool to_handle(lua_State* L, int index, uint64_t* id);

// A vector is a table with x, y and z.
TSUCHI_Vec3 check_vec3(lua_State* L, int index);
void push_vec3(lua_State* L, const TSUCHI_Vec3& value);
// A rotation is a table with pitch, yaw and roll, in degrees.
TSUCHI_Rotation check_rotation(lua_State* L, int index);
void push_rotation(lua_State* L, const TSUCHI_Rotation& value);

// What a script gets for a number of the game's that has no name, where a name is what it would get: "#" and the
// number, such as "#12". A value of an enum and a bit of a flag set are both told this way, so neither is hidden
// and a script always gets a string.
void push_unnamed(lua_State* L, uint32_t value);

// A flag set is a table of names set to true: {smooth_gauge = true}. nil is no flags.
uint32_t check_flags(lua_State* L, int index, const char* const names[], const uint32_t values[]);
void push_flags(lua_State* L, uint32_t bits, const char* const names[], const uint32_t values[]);

// Events. listen() leaves a subscription object on the stack; subscribed()
// completes it or turns it into a failure. An event callback hands the event
// to deliver() with the function that calls the handler with it: the event is
// its first argument, as light userdata, and the handler its second. That
// function runs where an error is caught, so that one made as the handler's
// arguments are, for want of memory, is no different from one in the handler.
// What a script may use, and what it has used. A state the framework makes for a mod has one of these; a state
// made any other way has none, and is not bounded.
// Instructions a script may run in one call from the framework: as it starts, or in a handler. A script that
// does not come back holds the game still, so past this it is stopped where it stands with an error.
constexpr uint64_t kTurn = 20'000'000;
struct Budget {
    size_t held = 0;        // bytes of memory the state holds
    uint64_t run = 0;       // instructions since the turn began
    uint64_t limit = kTurn; // instructions the turn may run; a hook's turn has less
    tsuchi_script::Allowance hooks;
    uint32_t hook_pins = 0;
    void (*close_pending)(lua_State*) = nullptr;
    uint32_t depth = 0;     // framework calls into the state that haven't returned yet
};
// Bytes of memory one mod's scripts may hold at once. Past this the script gets Lua's own memory error. It is
// more than a script that calls itself without end holds when Lua stops it for the depth, which is about 64 MB,
// so that script still gets the error the language gives.
constexpr size_t kScriptMemory = size_t(128) << 20;
Budget* budget(lua_State* L);
void set_budget(lua_State* L, Budget* budget);
// One call from the framework into a script, for as long as it lasts. The outermost call begins a turn. A call
// that comes while the script is already running, such as a hook that the script's own call reached, continues
// the turn it's in, so it gets no new allowance.
// An outermost call may give its turn a smaller limit, as a hook's call does.
class Turn {
public:
    explicit Turn(lua_State* L, uint64_t limit = kTurn);
    ~Turn();
    Turn(const Turn&) = delete;
    Turn& operator=(const Turn&) = delete;
    // True when this call began the turn rather than continuing one.
    bool outermost() const { return outermost_; }

private:
    lua_State* L_;
    Budget* used_;
    bool outermost_ = false;
};
// True when the script has used up its turn.
bool turn_over(lua_State* L);
// An integer argument, which has to be within what the call takes.
lua_Integer check_integer(lua_State* L, int index, lua_Integer low, lua_Integer high);
// Unsigned 64-bit values cross as signed Lua integers with the same 64 bits.
// A float is taken when it is a whole number from 0 through 2^53 - 1, which a
// double holds exactly. Other floats and numeric strings are refused.
uint64_t check_u64(lua_State* L, int index);
void push_u64(lua_State* L, uint64_t value);

struct Listener;
Listener* listen(lua_State* L, int function_index);
int subscribed(lua_State* L, Listener* listener, TSUCHI_Result result, TSUCHI_Subscription subscription);
void deliver(Listener* listener, lua_CFunction call, const void* event);

void pin_hook(lua_State* L);
void unpin_hook(lua_State* L);
void close_when_unused(lua_State* L, void (*close)(lua_State*));
void report_hook_error(lua_State* L, const char* message);
void open_services(lua_State* L); // generated

} // namespace tsuchi_lua
