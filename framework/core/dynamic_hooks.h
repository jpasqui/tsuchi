// Dynamic hooks: hooks whose function is described at run time by a signature, and whose
// handler gets the call's registers in a frame. The script runtimes build hook.wrap,
// hook.watch and hook.instruction on this (DESIGN.md, "Script hooks").
//
// A dynamic hook takes one of a fixed supply of gateways. A gateway is the native function
// the hook engine calls in place of the game's: it stores the argument registers in a
// CallFrame and calls tsuchi_dynamic_dispatch with its number. The dispatch decides whether
// the hook's client runs, and puts the result back in the frame.
//
// On ARM64 the gateways are assembly in start.S. Each keeps every register a call can use:
// x0 to x7, x8 for a result the caller gives memory for, and q0 to q7 whole, and returns
// x0, x1, x8 and q0 to q3 from the frame, so a function that returns a structure in memory,
// two registers or four floats gets them through a hook unchanged. tsuchi_dynamic_call calls
// the original the same way. Inline hooks have a fixed prototype of their own and need no
// platform part.
#pragma once
#include <stdint.h>

#include "tsuchi/core.h"
#include "tsuchi/hook.h"

namespace tsuchi {

constexpr uint32_t kDynamicGateways = 256;
constexpr uint32_t kDynamicDepth = 8;      // deeper calls of one hook go to the original
constexpr uint32_t kWatchRecords = 256;    // records per watch, between two frames
constexpr uint32_t kWatchesPerMod = 16;    // each keeps its records in about 180 KB of the game's memory
constexpr uint32_t kWatchTextUnits = 256;  // UTF-16 units of text16 arguments a watch record keeps
constexpr uint32_t kSignatureArguments = 16;
constexpr uint32_t kRegisterArguments = 8; // of each kind
// The game's thread has a stack of 1 MB: nnMain creates it with the size the command-line parser defaults to.
constexpr uint64_t kGameStackBytes = 0x100000;

// The registers of one call, as a gateway stores them, and its result afterward in x[0] (and
// x[1]) or q[0] to q[3]. A signature's f32 is the low 32 bits of q[n][0]. start.S knows this layout.
struct alignas(16) CallFrame {
    uint64_t x[kRegisterArguments];    // x0 to x7
    uint64_t x8;                       // where the caller wants a result it gives memory for
    uint64_t reserved;
    uint64_t q[kRegisterArguments][2]; // v0 to v7, low half first
};
static_assert(sizeof(CallFrame) == 208, "start.S stores the frame at fixed offsets");


enum class CallType : uint8_t { void_, boolean, i32, u32, u64, pointer, const_pointer, f32, text16 };

// Where each argument is: x<reg> for integer types, s<reg> for f32.
struct Signature {
    CallType result = CallType::void_;
    uint8_t count = 0;
    CallType args[kSignatureArguments]{};
    uint8_t reg[kSignatureArguments]{};
};
// TSUCHI_INVALID for text that doesn't parse, names no result, or returns text16; TSUCHI_UNSUPPORTED for a
// signature with an argument that would go on the stack.
TSUCHI_Result parse_signature(const char* text, Signature& out);
bool is_float(CallType type);
// Copies UTF-16 text from the game's memory, up to its terminator, `capacity` units, or a page that can't be
// read. Returns the units copied.
uint32_t read_text16(uint64_t address, uint16_t* out, uint32_t capacity);

// One argument or result in a frame, as bits: bool as 0 or 1, i32 sign-extended, u32 zero-extended, 64-bit
// types as they are, and f32 in the low 32 bits.
uint64_t frame_argument(const CallFrame& frame, const Signature& signature, uint32_t index);
void set_frame_argument(CallFrame& frame, const Signature& signature, uint32_t index, uint64_t bits);
uint64_t frame_result(const CallFrame& frame, CallType type);
void set_frame_result(CallFrame& frame, CallType type, uint64_t bits);

// Calls `target` with the frame's argument registers and stores its result in the frame. Lifts the barrier
// on mod calls (registry.h) while the game's function runs.
void call_with_frame(uint64_t target, CallFrame& frame, CallType result);

// What a dynamic hook is put on.
enum class DynamicSite : uint8_t {
    entry,       // a function-table entry, by its offset from the start of the game's code
    function,    // a function's first instruction, by its offset
    instruction, // one instruction, by its offset (an inline hook)
};
enum class DynamicKind : uint8_t { wrap, watch, inline_hook };
struct DynamicTarget {
    DynamicSite site;
    uint64_t offset;
    uint64_t check; // entry: the stock function's offset or 0; otherwise the expected instruction
};

// The registers an inline hook's client sees. Changes apply only when the client succeeds.
struct InlineRegisters {
    uint64_t x[31];
    uint64_t nzcv; // bits 31 to 28
};

// One watched call. The text16 arguments share one buffer: argument i's units start at
// text_start[i] and run for text_units[i]. A text past the buffer's end is cut short there.
struct WatchRecord {
    uint64_t args[kSignatureArguments];
    uint64_t result;
    uint16_t text[kWatchTextUnits];
    uint16_t text_start[kSignatureArguments];
    uint16_t text_units[kSignatureArguments];
    uint32_t dropped; // calls the ring had no room for since the record before this one
    bool game_thread;
};

// How a dynamic hook's continuation is called from inside a client.
struct DynamicOriginal {
    uint64_t target;
    const Signature* signature;
};

// What a script runtime supplies for one hook. Every function runs on the game's thread.
struct DynamicClient {
    // A wrap hook's call. The client calls the original as often as it likes through `original` and
    // call_with_frame, and writes the result into `frame`. False when it failed before it had a result: the
    // dispatch then calls the original with the game's registers. A client that fails after it has a result
    // writes the last one the original gave and returns true.
    bool (*call)(void* user, CallFrame& frame, const DynamicOriginal& original) = nullptr;
    // An inline hook's call. False drops the client's changes.
    bool (*inline_call)(void* user, InlineRegisters& registers) = nullptr;
    // A watch record, at the start of the frame after the call.
    void (*record)(void* user, const WatchRecord& record) = nullptr;
    // True when the client can be called now: it isn't disabled, and its runtime has budget and stack for a
    // call. Asked before every wrap and inline call. Null means always.
    bool (*ready)(void* user) = nullptr;
    // The hook has ended and no call is using it. The client frees what it holds.
    void (*release)(void* user) = nullptr;
};

// Puts a dynamic hook on, owned by `mod`. An inline hook needs DynamicSite::instruction and ignores
// `signature`; wrap and watch need an entry or a function. Fails as the hook engine or the native backend does,
// with TSUCHI_NOT_READY for a function or an instruction when there's no code access, and TSUCHI_CAPACITY
// when no gateway is free.
TSUCHI_Result dynamic_hook(TSUCHI_Mod* mod, DynamicKind kind, const DynamicTarget& target, const Signature& signature,
                           const DynamicClient& client, void* user, uint64_t* id);
// TSUCHI_NOT_FOUND when the mod has no dynamic hook with that id. Calls already running finish, and the
// client's release comes once the last one has returned.
TSUCHI_Result dynamic_remove(TSUCHI_Mod* mod, uint64_t id);
// True while the game's thread is inside a dynamic hook's client.
bool in_dynamic_hook();
// Bytes of the game thread's stack left below the caller, measured from where the engine frame last began.
// UINT64_MAX before the first frame.
uint64_t stack_room();
// How many engine frames have begun. A runtime keys its per-frame hook budget on it.
uint64_t engine_frame_number();

// Calls, and the calls that went straight to the original instead of to the client, by reason.
struct DynamicStats {
    uint32_t gateways_used, calls, off_thread, depth, not_ready, failed, dropped;
};
DynamicStats dynamic_stats();
// Sends watch records, frees ended hooks whose calls have all returned, then runs the work each_dynamic_frame
// added. The engine frame does this at its start; a test calls it.
void dynamic_hooks_frame();
// Adds a script runtime's work for the start of every engine frame, such as logging the errors its hooks met.
// Up to four; adding the same function again does nothing. False when there's no room.
bool each_dynamic_frame(void (*work)());
// Connects the dispatch to the frame tick. Called once at start-up.
void dynamic_hooks_start();

// The build supplies the wrap gateways and the routine that calls a function with a frame. On ARM64 the
// framework has its own; a host test supplies one that fits the signatures it uses.
struct DynamicPlatform {
    uint32_t gateways = 0;
    uint64_t (*gateway)(uint32_t number, bool float_result) = nullptr;
    void (*call)(uint64_t target, CallFrame* frame, bool float_result) = nullptr;
};
void set_dynamic_platform(const DynamicPlatform& platform);

// Where a named entry of the game table is, and the function the game keeps there, both as offsets from the start
// of the game's code. TSUCHI_NOT_FOUND for a name the table doesn't have, TSUCHI_UNSUPPORTED when the running
// build of the game has no such entry.
TSUCHI_Result hook_slot_entry(const char* name, uint64_t& entry, uint64_t& stock);

} // namespace tsuchi

// A gateway's way in. Returns nothing: the results are in the frame.
extern "C" void tsuchi_dynamic_dispatch(uint32_t gateway, tsuchi::CallFrame* frame);
