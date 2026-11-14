// Hooks: running a function in place of one entry in the game's function tables.
//
// The entry is pointed at a small piece of framework code, a thunk, and stays
// that way. The thunk jumps to the newest live hook, or to what the entry held
// when there is none. Each hook gets a thunk of its own to call as its
// `original`, which jumps to the next live hook inward. So hooks can come and
// go in any order, and anything that wrapped the entry after the framework did
// keeps working.
//
// Hooks are put on and taken off on the game's thread, like everything a mod
// asks for. The game may call a hooked function on any of its threads, and
// dispatch chooses and retains its target on that thread until the call returns.
//
// A function the game calls directly has no table entry. For those, a patch
// changes one instruction of the game's code to jump into the framework (see
// tools/gen/stubs.py); from there the chain works the same way.
//
// The patch and the framework are made from one game table, and the patch
// carries a hash of what it was made from. The framework uses code sites only
// when the patch in the running game carries the hash this build has.
#pragma once
#include <stdint.h>

#include "game.h"
#include "tsuchi/core.h"

namespace tsuchi {

enum : uint32_t {
    kHookNeedsStock = 1, // refuse an entry that does not hold the game's own function
    kHookFirst = 2,      // stays outermost: hooks added later to the same entry go inside it, so it runs before them
};

// Puts `function` in place of the entry at `entry`, an address in the game's
// memory. `stock` is what the entry holds in an untouched game, or 0 when not
// known. `*original` gets what `function` calls to continue. The hook belongs
// to `mod` and ends with it.
TSUCHI_Result hook_wrap(TSUCHI_Mod* mod, uint64_t entry, uint64_t stock, void* function, uint32_t flags, uint64_t* id, void** original);
// What the running game has of the framework's patch.
enum class Patch : uint8_t {
    missing, // nothing: the game's code is as it shipped
    right,   // the patch this build of the framework was made with
    other,   // a patch made from another table, or one from before patches said what they were made from
};
// Looks at the game's code and keeps what it finds for the rest of the run. Called once at start-up, before
// anything hooks a code site and before the framework tells the patch's block where to find it.
Patch find_patch();
// What find_patch found. `missing` until it has run.
Patch patch_found();
// Why code sites cannot be hooked, in words the mod list can show. Null when they can.
const char* patch_problem();
// Puts `function` in front of a function the game calls directly, at a code site of
// the game table (its number there). The right patch has to be in place in the running
// game. When it is not, this fails with TSUCHI_NOT_READY and the mod is noted as one that
// cannot run (mod_block), with the patch as the reason. Otherwise as hook_wrap.
TSUCHI_Result hook_code_site(TSUCHI_Mod* mod, uint32_t site, void* function, uint64_t* id, void** original);
// True when a code site can be hooked: the right patch is there, and the site's instruction is the patch's.
bool code_site_patched(uint32_t site);
// The same three with the types the game table gives: `function` and what `*next` gets are the function the
// table says is there, so a hook that takes or returns anything else does not compile. `*next` is set only when
// the hook is in place. A slot's entry and the function the game puts there are the table's, from the start of
// the game's code.
template <class F>
TSUCHI_Result hook_slot(TSUCHI_Mod* mod, const game::table::SlotOf<F>& slot, F function, F* next, uint32_t flags, uint64_t* id) {
    if (!function || !next || !id) return TSUCHI_INVALID;
    const uint64_t main = game::main_base();
    void* original = nullptr;
    const TSUCHI_Result result = hook_wrap(mod, main + slot.table + slot.offset, main + slot.stock, reinterpret_cast<void*>(function), flags, id, &original);
    if (result == TSUCHI_OK) *next = reinterpret_cast<F>(original);
    return result;
}
// One table of a group: its entry is wrapped whatever it holds.
template <class F>
TSUCHI_Result hook_group_entry(TSUCHI_Mod* mod, const game::table::SlotGroupOf<F>& group, unsigned table, F function, F* next, uint64_t* id) {
    void* original = nullptr;
    if (!function || !next || !id || table >= group.table_count) return TSUCHI_INVALID;
    if (uint64_t(group.tables[table]) == game::table::absent_value) return TSUCHI_UNSUPPORTED; // a class this build does not have
    const TSUCHI_Result result = hook_wrap(mod, game::main_base() + group.tables[table] + group.offset, 0, reinterpret_cast<void*>(function), 0, id, &original);
    if (result == TSUCHI_OK) *next = reinterpret_cast<F>(original);
    return result;
}
template <class F>
TSUCHI_Result hook_site(TSUCHI_Mod* mod, game::table::SiteOf<F> site, F function, F* next, uint64_t* id) {
    void* original = nullptr;
    if (!function || !next || !id) return TSUCHI_INVALID;
    const TSUCHI_Result result = hook_code_site(mod, site.number, reinterpret_cast<void*>(function), id, &original);
    if (result == TSUCHI_OK) *next = reinterpret_cast<F>(original);
    return result;
}
// Ends one of the mod's own hooks. Calls already running it may continue until they return.
TSUCHI_Result hook_remove(TSUCHI_Mod* mod, uint64_t id);
// True once no hook has this id: it was removed and every call that selected it has returned, so nothing will
// call its function again. Any thread.
bool hook_retired(uint64_t id);
// How many hooks are live on an entry.
uint32_t hook_count(uint64_t entry);
// What the entry held before anything here wrapped it, or what it holds now when nothing has. 0 when it cannot be read.
uint64_t hook_entry_base(uint64_t entry);
// True when a recorded entry still points at its permanent core thunk.
bool hook_entry_owned(uint64_t entry);
// True when the entry held the game's own function when it was first wrapped.
bool hook_entry_was_stock(uint64_t entry);
// Provenance of the newest active invocation of this hook on this thread.
// A continuation inherits its caller only from a tracked invocation of its owning
// outer hook. Calling an original without that invocation has no provenance.
// Entry calls, including recursion, begin a new invocation. Missing frames give zeros.
struct HookInvocation { uint64_t caller, token, thread; };
HookInvocation hook_invocation(uint64_t id);
// The places hooks are on that were called most since the last sample, most calls first: up to `most` of them,
// by name (a slot's or a code site's, or "a table entry"), each with its calls through its entry, and in `all` the
// calls through every place. The counts start again. For a test's log.
struct HookSample { const char* name; uint32_t calls; };
unsigned hook_sample(HookSample* out, unsigned most, uint32_t* all);
// Connects the public hooks service to this. Called once at start-up.
void hooks_start();

// Native code hooks are an optional backend. Host tests of table hooks don't
// need code-writing facilities. The game installs this before mods start.
struct NativeHooks {
    TSUCHI_Result (*function)(TSUCHI_Mod*, uint64_t, uint32_t, void*, uint64_t*, void**);
    TSUCHI_Result (*inline_hook)(TSUCHI_Mod*, uint64_t, uint32_t, void*, uint64_t*, void**);
    TSUCHI_Result (*patch)(TSUCHI_Mod*, uint64_t, uint32_t, uint32_t, uint64_t*);
    TSUCHI_Result (*remove)(TSUCHI_Mod*, uint64_t);
    TSUCHI_Result (*check)(uint64_t, const void*, uint64_t);
    TSUCHI_Result (*call)(TSUCHI_Mod*, uint64_t, uint32_t, void*, uint64_t*, void**) = nullptr;
};
void set_native_hooks(const NativeHooks* backend);
// The backend set_native_hooks installed, or null.
const NativeHooks* native_hooks_backend();
// How the service's `remove` reaches script hooks (dynamic_hooks.h), which have ids of their own. It's tried
// first; TSUCHI_NOT_FOUND from it goes on to the other kinds.
void set_dynamic_remove(TSUCHI_Result (*remove)(TSUCHI_Mod*, uint64_t));

struct HookStats {
    uint32_t thunks_limit, thunks_used, live_records, retired_records, frames_active, frames_peak;
};
// Copies shared budgets under the dispatch lock; never admits a hook or frame.
HookStats hook_stats();

// The build supplies fixed thunks. Entry thunks stay in place; a removed hook's
// original thunk is reused after its running calls return. The tables have room
// for kHookThunks; fewer supplied thunks reduce capacity, and extras are unused.
constexpr uint32_t kHookThunks = 1024;
uint32_t hook_thunk_count();
uint64_t hook_thunk_address(uint32_t thunk);
// The patch's block reaches the framework with a code site's number; tsuchi_hook_target
// takes it with this bit set.
constexpr uint32_t kCodeSiteThunk = 0x80000000;

} // namespace tsuchi

extern "C" uint64_t tsuchi_hook_target(uint32_t thunk);

// Internal assembly dispatch: target and whether to return through the tracker.
// A stack address identifies a call chain, including tail calls on the same stack.
struct TsuchiHookDispatch {
    uint64_t target, tracked;
};
extern "C" TsuchiHookDispatch tsuchi_hook_enter(uint32_t thunk, uint64_t stack, uint64_t return_to);
extern "C" uint64_t tsuchi_hook_leave(uint64_t stack);
