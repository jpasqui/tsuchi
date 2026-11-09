// Engine function hooks: a script's handlers run before and after one of the engine's reflected functions,
// which the script names by its path. The script runtimes build hook.engine_function on this (DESIGN.md,
// "Script hooks").
//
// The engine runs a reflected function through the native function its description keeps (UFunction::Func).
// For a Blueprint function that is the script interpreter; for a C++ function, the code generated for it.
// UObject::ProcessEvent calls it with the function's frame, whose locals hold the arguments. Tsuchi points that
// entry at a thunk of its own. The entry is data, so this needs no code access. The thunk runs the handlers,
// calls what the entry held, and runs the handlers that come after.
//
// A Blueprint event without arguments is run as an entry point into its class's event graph: ProcessEvent
// calls the event graph's function in its place. Its hook goes on the event graph's function and runs for that
// entry point only.
//
// The thunk stays in the entry until the game closes, the same as a table hook's entry thunk, because a call
// may have read the entry and not reached the thunk yet. With no hooks left the thunk goes straight to what
// the entry held. Hooks are put on, run and taken off on the game's thread; a call on another thread goes
// straight through, and touches only the thunk's own record.
//
// A Blueprint that calls a C++ function hands that function its own frame, with the arguments still script
// code to be run. Those calls go straight through and are counted. So are calls that come while the
// handlers can't run.
#pragma once
#include <stdint.h>

#include "script_writes.h"
#include "tsuchi/core.h"

namespace tsuchi {

constexpr uint32_t kEngineSites = 256;   // functions whose entry holds a thunk, for the rest of the run
constexpr uint32_t kEngineHooks = 256;
constexpr uint32_t kEngineRunning = 16;  // hooks that one call can run; later ones skip that call
constexpr uint32_t kEngineDepth = 8;     // deeper calls of one hook skip it

// One call of a hooked function, as its handlers see it. The values are read and written by path, the way the
// reflect service reads an object's fields, starting with an argument's name.
struct EngineCall {
    uint64_t function = 0; // the function's description
    uint64_t object = 0;   // a handle to the object it runs on, or 0
    uint64_t locals = 0;   // the frame's memory, with the arguments first
    uint64_t out_parms = 0; // the frame's list of the arguments it writes
    uint64_t result = 0;   // where its result goes, or 0
    bool finished = false; // the function has returned, so its result can be read
};
// A value, the text of a name or string value, and the name of a field, by path, as reflection.h has them for
// memory a type describes. An empty path lists the function's arguments, without its result. The result can be
// read only once the call has finished.
TSUCHI_Result engine_read(const EngineCall& call, const char* path, TSUCHI_Value* out);
TSUCHI_Result engine_text(const EngineCall& call, const char* path, char* buffer, uint64_t capacity, uint64_t* needed);
TSUCHI_Result engine_field_at(const EngineCall& call, const char* path, uint32_t index, char* buffer, uint64_t capacity, uint64_t* needed);
// The name of the function's result. TSUCHI_NOT_FOUND for a function without one.
TSUCHI_Result engine_result_name(const EngineCall& call, char* buffer, uint64_t capacity, uint64_t* needed);
// Writes values the way reflection.set does: all of them, or none when one doesn't fit.
struct EngineWrite {
    const char* path;
    const ScriptWrite* value;
};
TSUCHI_Result engine_write(const EngineCall& call, const EngineWrite* writes, uint32_t count);
// The handle kind a script gets for an object: "actor", "component" or "object". Asks no service, so it works
// inside a hook.
const char* engine_object_kind(uint64_t handle);

// What a script runtime supplies for one hook. Every function runs on the game's thread.
struct EngineClient {
    // Before and after the call. False when the handler failed: its changes are dropped, and the call goes on.
    bool (*before)(void* user, EngineCall& call) = nullptr;
    bool (*after)(void* user, EngineCall& call) = nullptr;
    // True when the handlers can be called now. Asked before every call. Null means always.
    bool (*ready)(void* user) = nullptr;
    // At the start of every frame while the hook is on, to report what its handlers met.
    void (*report)(void* user) = nullptr;
    // The hook has ended and no call is using it. The client frees what it holds.
    void (*release)(void* user) = nullptr;
};

// The function that a path names: the path of its class, a colon, and its name, as object_path writes it.
// Case doesn't matter. 0 when no such function is loaded.
uint64_t find_function_path(const char* path);
// Puts a hook on, owned by `mod`. TSUCHI_INVALID for a client without handlers, TSUCHI_NOT_FOUND for a path
// that names no function, TSUCHI_UNSUPPORTED when the build's table lacks what this needs, TSUCHI_NOT_READY
// when the function's entry can't be read or written, and TSUCHI_CAPACITY when no hook or thunk is free.
TSUCHI_Result engine_hook(TSUCHI_Mod* mod, const char* path, const EngineClient& client, void* user, uint64_t* id);
// TSUCHI_NOT_FOUND when the mod has no engine hook with that id. A call already running finishes without the
// hook's later handlers, and the client's release comes once it has returned.
TSUCHI_Result engine_hook_remove(TSUCHI_Mod* mod, uint64_t id);

// Calls, and the calls that went straight through instead of to the handlers, by reason.
struct EngineStats {
    uint32_t sites_used, hooks_used, calls, off_thread, script_arguments, not_ready, depth, failed;
};
EngineStats engine_stats();
// Frees ended hooks whose calls have all returned, and asks each live one to report. The engine frame does this at
// its start; a test calls it.
void engine_hooks_frame();
// Connects engine hooks to the frame tick and to hook.remove. Called once at start-up.
void engine_hooks_start();

} // namespace tsuchi
