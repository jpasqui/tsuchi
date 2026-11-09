#include "hooks.h"

#include <string.h>

#include "game.h"
#include "hook_service.h"
#include "registry.h"

namespace tsuchi {
namespace {

namespace t = game::table;

const Kind kHook{"hook"};
const NativeHooks* native_hooks = nullptr;
constexpr int32_t kNone = -1;

// Something hooks are put on. The first kHookThunks places are for table entries, in the order they were first
// wrapped. After them comes one place for each code site of the game table, in the table's order.
struct Site {
    uint64_t entry;     // where the table entry is; 0 for a code site
    uint64_t base;      // what the entry held when it was first wrapped. A code site's way back is in the game table
    uint32_t outermost; // the newest hook's place in `records` plus one, or 0 for none
    bool stock;         // the game's own function is at the end of the chain
    uint32_t calls;     // calls through its entry since the last sample, for a test's log
};

struct Hook {
    uint64_t id; // zero marks reusable storage
    TSUCHI_Mod* mod;
    uint64_t function;
    uint32_t site, inner, thunk, calls;
    bool live;
    bool first; // kHookFirst: later hooks go inside it
};

// Entry thunks are permanent. Hook records and their originals stay until their
// running calls return. Chain selection and reuse share a lock, never held while
// calling a hook or game function.
Site sites[kHookThunks + t::code_site_count];
Hook records[kHookThunks];
uint32_t thunks[kHookThunks]; // zero is free; otherwise site+1 or kForHook|record+1
constexpr uint32_t kForHook = 0x80000000;
uint32_t entries = 0;
uint64_t next_id = 1;
bool locked = false;
struct Lock {
    Lock() {
        uint32_t tries = 0;
        while (__atomic_test_and_set(&locked, __ATOMIC_ACQUIRE)) lock_wait(tries);
    }
    ~Lock() { __atomic_clear(&locked, __ATOMIC_RELEASE); }
};

struct Frame {
    uint64_t stack, return_to, caller, token, thread;
    uint32_t hook, previous; // indices plus one
    bool top;
};
uint64_t next_frame_token = 1;
Frame frames[kHookThunks]; // a hook index of zero marks a free frame
// Every frame from this one on is free, so a call looks only at the frames below it: as many as calls are
// running at once, a few, and not all kHookThunks.
uint32_t frames_high = 0;
uint32_t frames_peak = 0;

uint32_t supply() { return hook_thunk_count() < kHookThunks ? hook_thunk_count() : kHookThunks; }
uint32_t thunks_left() {
    uint32_t left = 0;
    for (uint32_t i = 0; i < supply(); ++i) left += !thunks[i];
    return left;
}
uint32_t take_thunk(uint32_t stands_for) {
    for (uint32_t i = 0; i < supply(); ++i)
        if (!thunks[i]) {
            thunks[i] = stands_for;
            return i;
        }
    return kHookThunks; // callers check capacity under the same lock
}
int32_t find_site(uint64_t entry) {
    for (uint32_t i = 0; i < entries; ++i)
        if (sites[i].entry == entry) return int32_t(i);
    return kNone;
}
void reclaim(Hook& hook) {
    if (hook.live || hook.calls) return;
    thunks[hook.thunk] = 0;
    hook = Hook{};
}
void end_hook(void*, uint64_t id) {
    Lock lock;
    for (uint32_t i = 0; i < kHookThunks; ++i) {
        Hook& hook = records[i];
        if (hook.id != id) continue;
        hook.live = false;
        // Unlink from live chains and from continuations of removed but running
        // outer hooks before any record or original thunk can be reused.
        if (sites[hook.site].outermost == i + 1) sites[hook.site].outermost = hook.inner;
        for (Hook& outer : records)
            if (outer.id && outer.inner == i + 1) outer.inner = hook.inner;
        reclaim(hook);
        return;
    }
}
// Installs a hook after ownership allocation succeeds, under the dispatch lock.
TSUCHI_Result put_hook(TSUCHI_Mod* mod, uint32_t site, void* function, uint32_t flags, uint64_t* id, void** original) {
    uint32_t at = 0;
    while (records[at].id) ++at;
    const uint32_t thunk = take_thunk(kForHook | (at + 1));
    // The newest hook is outermost, except that one taken with kHookFirst stays outside those added after it.
    const bool first = flags & kHookFirst;
    uint32_t* link = &sites[site].outermost;
    if (!first)
        while (*link && records[*link - 1].first) link = &records[*link - 1].inner;
    records[at] = Hook{next_id, mod, reinterpret_cast<uint64_t>(function), site, *link, thunk, 0, true, first};
    *link = at + 1;
    __atomic_store_n(id, next_id++, __ATOMIC_RELEASE);
    __atomic_store_n(original, reinterpret_cast<void*>(hook_thunk_address(thunk)), __ATOMIC_RELEASE);
    return TSUCHI_OK;
}
TSUCHI_Result add_hook(TSUCHI_Mod* mod, uint32_t site, void* function, uint64_t* id, void** original) {
    if (next_id >= (uint64_t(1) << 62)) return TSUCHI_CAPACITY;
    {
        Lock lock;
        if (!thunks_left()) return TSUCHI_CAPACITY;
    }
    // Only the game's thread adds a hook. Dispatch can free storage while own()
    // runs, but cannot consume it. No allocator runs under the dispatch lock.
    const TSUCHI_Result owned = own(mod, kHook, next_id, end_hook, nullptr);
    if (owned != TSUCHI_OK) return owned;
    Lock lock;
    return put_hook(mod, site, function, 0, id, original);
}
uint64_t base_of(uint32_t site) {
    return site < kHookThunks ? sites[site].base : game::main_base() + t::code_sites[site - kHookThunks].original;
}
// Decodes an entry or original thunk while its owner is retained.
bool chain(uint32_t thunk, uint32_t& site, uint32_t& at) {
    if (thunk & kCodeSiteThunk) {
        site = thunk & ~kCodeSiteThunk;
        if (site >= t::code_site_count) return false;
        site += kHookThunks;
        at = sites[site].outermost;
    } else {
        const uint32_t what = thunk < supply() ? thunks[thunk] : 0;
        if (!what) return false;
        if (what & kForHook) {
            const Hook& hook = records[(what & ~kForHook) - 1];
            site = hook.site;
            at = hook.inner;
        } else {
            site = what - 1;
            at = sites[site].outermost;
        }
    }
    for (; at; at = records[at - 1].inner)
        if (records[at - 1].live && mod_running(records[at - 1].mod)) break;
    return true;
}

// The instruction the patch puts at a code site: a jump, or a call, to the site's entry in the block.
uint32_t patched_instruction(const t::CodeSite& site) {
    return (site.call ? 0x94000000u : 0x14000000u) | (uint32_t((site.entry - site.address) >> 2) & 0x03FFFFFFu);
}

Patch patch = Patch::missing;
const char* const kPatchMissing = "the framework's patch is not installed";
const char* const kPatchOther = "the framework's patch is from another version";
const char* const kModNeedsMissing = "it hooks the game's code, and the framework's patch is not installed";
const char* const kModNeedsOther = "it hooks the game's code, and the framework's patch is from another version";

Patch look_for_patch() {
    const uint64_t main = game::main_base();
    if (!main || !t::code_site_count) return Patch::missing;
    // The patch's mark, in two parts that need not be side by side (tools/gen/stubs.py).
    uint64_t magic = 0, hash = 0;
    if (game::read(main + t::stub::block, magic) && magic == t::stub::patch_magic)
        return game::read(main + t::stub::block_hash, hash) && hash == t::stub::patch_hash ? Patch::right : Patch::other;
    // No block that says what it is. A patch from before blocks were marked changed the game's code at the
    // sites all the same.
    for (unsigned i = 0; i < t::code_site_count; ++i) {
        uint32_t there = 0;
        if (t::present(t::code_sites[i].address) && game::read(main + t::code_sites[i].address, there) && there != t::code_sites[i].instruction) return Patch::other;
    }
    return Patch::missing;
}

} // namespace

TSUCHI_Result hook_wrap(TSUCHI_Mod* mod, uint64_t entry, uint64_t stock, void* function, uint32_t flags, uint64_t* id, void** original) {
    if (!mod_usable(mod) || !entry || !function || !id || !original) return TSUCHI_INVALID;
    if (entry >= t::absent_value) return TSUCHI_UNSUPPORTED; // an entry this build of the game does not have
    int32_t site;
    uint64_t held = 0;
    bool is_stock = false;
    {
        Lock lock;
        site = find_site(entry);
        if (site == kNone) {
            // Validate a new entry before either ownership or publication.
            if ((entry & 7) || !game::readable(entry, sizeof(uint64_t)) || !game::writable(entry, sizeof(uint64_t))) return TSUCHI_NOT_READY;
            held = __atomic_load_n(reinterpret_cast<const uint64_t*>(entry), __ATOMIC_ACQUIRE);
            if (!game::executable(held)) return TSUCHI_NOT_READY;
            is_stock = stock && held == stock;
            if ((flags & kHookNeedsStock) && !is_stock) return TSUCHI_BUSY;
            if (thunks_left() < 2) return TSUCHI_CAPACITY; // entry plus original
        } else {
            if ((flags & kHookNeedsStock) && !sites[site].stock) return TSUCHI_BUSY;
            if (!thunks_left()) return TSUCHI_CAPACITY;
        }
    }
    if (next_id >= (uint64_t(1) << 62)) return TSUCHI_CAPACITY;
    // Do the only potentially failing step before permanently publishing an
    // entry thunk. The game's thread is the only writer; dispatch only frees.
    const TSUCHI_Result owned = own(mod, kHook, next_id, end_hook, nullptr);
    if (owned != TSUCHI_OK) return owned;
    Lock lock;
    if (site == kNone) {
        sites[entries] = Site{entry, held, 0, is_stock, 0};
        const uint32_t thunk = take_thunk(entries + 1);
        site = int32_t(entries++);
        // The game reads this pointer before reaching our lock. Publish as one
        // aligned store rather than memcpy's byte stores.
        __atomic_store_n(reinterpret_cast<uint64_t*>(entry), hook_thunk_address(thunk), __ATOMIC_RELEASE);
    }
    return put_hook(mod, uint32_t(site), function, flags, id, original);
}

Patch find_patch() { return patch = look_for_patch(); }
Patch patch_found() { return patch; }
const char* patch_problem() { return patch == Patch::right ? nullptr : patch == Patch::other ? kPatchOther : kPatchMissing; }

bool code_site_patched(uint32_t site) {
    if (patch != Patch::right || site >= t::code_site_count || !game::main_base()) return false;
    const t::CodeSite& info = t::code_sites[site];
    uint32_t there = 0;
    return game::read(game::main_base() + info.address, there) && there == patched_instruction(info);
}

TSUCHI_Result hook_code_site(TSUCHI_Mod* mod, uint32_t site, void* function, uint64_t* id, void** original) {
    if (!mod_usable(mod) || !function || !id || !original || site >= t::code_site_count) return TSUCHI_INVALID;
    if (!t::present(t::code_sites[site].address)) return TSUCHI_UNSUPPORTED;
    if (patch != Patch::right) {
        // Another patch's block numbers the sites its own way and has its own ways back, so nothing is
        // put on a site. A mod that asked cannot do what it is for, and is told apart from one that never asked.
        mod_block(mod, patch == Patch::other ? kModNeedsOther : kModNeedsMissing);
        return TSUCHI_NOT_READY;
    }
    if (!code_site_patched(site)) return TSUCHI_NOT_READY;
    return add_hook(mod, kHookThunks + site, function, id, original);
}

TSUCHI_Result hook_remove(TSUCHI_Mod* mod, uint64_t id) {
    if (!mod_usable(mod)) return TSUCHI_INVALID;
    if (disown(mod, kHook, id) != TSUCHI_OK) return TSUCHI_NOT_FOUND; // none, or another mod's
    end_hook(nullptr, id);
    return TSUCHI_OK;
}

bool hook_retired(uint64_t id) {
    if (!id) return true;
    Lock lock;
    for (const Hook& hook : records)
        if (hook.id == id) return false;
    return true;
}

uint32_t hook_count(uint64_t entry) {
    Lock lock;
    const int32_t site = find_site(entry);
    uint32_t count = 0;
    for (uint32_t at = site == kNone ? 0 : sites[site].outermost; at; at = records[at - 1].inner)
        count += records[at - 1].live && mod_running(records[at - 1].mod);
    return count;
}

uint64_t hook_entry_base(uint64_t entry) {
    Lock lock;
    const int32_t site = find_site(entry);
    return site != kNone ? sites[site].base : game::read_or<uint64_t>(entry);
}

bool hook_entry_owned(uint64_t entry) {
    Lock lock;
    const int32_t site = find_site(entry);
    if (site == kNone || !game::readable(entry, sizeof(uint64_t))) return false;
    const uint64_t current = __atomic_load_n(reinterpret_cast<const uint64_t*>(entry), __ATOMIC_ACQUIRE);
    for (uint32_t i = 0; i < supply(); ++i)
        if (thunks[i] == uint32_t(site) + 1) return current == hook_thunk_address(i);
    return false;
}

bool hook_entry_was_stock(uint64_t entry) {
    Lock lock;
    const int32_t site = find_site(entry);
    return site != kNone && sites[site].stock;
}

unsigned hook_sample(HookSample* out, unsigned most, uint32_t* all) {
    const uint64_t main = game::main_base();
    unsigned found = 0;
    if (all) *all = 0;
    Lock lock;
    for (uint32_t site = 0; site < kHookThunks + t::code_site_count; ++site) {
        const uint32_t calls = sites[site].calls;
        sites[site].calls = 0;
        if (all) *all += calls;
        if (!calls || !most) continue;
        if (found == most && out[most - 1].calls >= calls) continue;
        const char* name = "a table entry";
        if (site >= kHookThunks) name = t::code_sites[site - kHookThunks].name;
        else
            for (unsigned i = 0; i < t::slot_count; ++i)
                if (main + t::slots[i].slot.table + t::slots[i].slot.offset == sites[site].entry) name = t::slots[i].name;
        // Kept in order, most calls first; the fewest falls off the end.
        unsigned at = found < most ? found++ : most - 1;
        while (at && out[at - 1].calls < calls) out[at] = out[at - 1], --at;
        out[at] = HookSample{name, calls};
    }
    return found;
}

HookInvocation hook_invocation(uint64_t id) {
    const uint64_t thread = current_thread_token();
    Lock lock;
    const Frame* latest = nullptr;
    for (uint32_t i = 0; i < frames_high; ++i)
        if (frames[i].hook && frames[i].thread == thread && records[frames[i].hook - 1].id == id &&
            (!latest || frames[i].token > latest->token)) latest = &frames[i];
    return latest ? HookInvocation{latest->caller, latest->token, latest->thread} : HookInvocation{};
}

// ---- The public service -----------------------------------------------------

namespace {

TSUCHI_Result service_wrap_address(TSUCHI_Mod* mod, uint64_t address, uint64_t stock, void* function, TSUCHI_Hook* out) {
    const uint64_t base = game::main_base();
    if (!base) return TSUCHI_NOT_READY;
    return hook_wrap(mod, base + address, stock ? base + stock : 0, function, 0, &out->id.id, &out->original);
}

TSUCHI_Result service_wrap(TSUCHI_Mod* mod, const char* slot, void* function, TSUCHI_Hook* out) {
    for (unsigned i = 0; i < t::slot_count; ++i)
        if (!strcmp(t::slots[i].name, slot))
            return t::present(t::slots[i].slot.table) ? service_wrap_address(mod, t::slots[i].slot.table + t::slots[i].slot.offset, t::slots[i].slot.stock, function, out) : TSUCHI_UNSUPPORTED;
    return TSUCHI_NOT_FOUND;
}

TSUCHI_Result (*dynamic_remove_function)(TSUCHI_Mod*, uint64_t) = nullptr;

TSUCHI_Result service_remove(TSUCHI_Mod* mod, TSUCHI_HookId id) {
    if (dynamic_remove_function) {
        const TSUCHI_Result result = dynamic_remove_function(mod, id.id);
        if (result != TSUCHI_NOT_FOUND) return result;
    }
    if (native_hooks) {
        const TSUCHI_Result result = native_hooks->remove(mod, id.id);
        if (result != TSUCHI_NOT_FOUND) return result;
    }
    return hook_remove(mod, id.id);
}

TSUCHI_Result service_function(TSUCHI_Mod* mod, uint64_t address, uint32_t expected, void* fn, TSUCHI_Hook* out) {
    return native_hooks ? native_hooks->function(mod, address, expected, fn, &out->id.id, &out->original) : TSUCHI_NOT_READY;
}
TSUCHI_Result service_inline(TSUCHI_Mod* mod, uint64_t address, uint32_t expected, void* fn, TSUCHI_Hook* out) {
    return native_hooks ? native_hooks->inline_hook(mod, address, expected, fn, &out->id.id, &out->original) : TSUCHI_NOT_READY;
}
TSUCHI_Result service_patch(TSUCHI_Mod* mod, uint64_t address, uint32_t expected, uint32_t replacement, TSUCHI_HookId* out) {
    return native_hooks ? native_hooks->patch(mod, address, expected, replacement, &out->id) : TSUCHI_NOT_READY;
}
TSUCHI_Result service_check(uint64_t address, const void* expected, uint64_t size) {
    return native_hooks ? native_hooks->check(address, expected, size) : TSUCHI_NOT_READY;
}
TSUCHI_Result service_call(TSUCHI_Mod* mod, uint64_t address, uint32_t expected, void* fn, TSUCHI_Hook* out) {
    return native_hooks && native_hooks->call ? native_hooks->call(mod, address, expected, fn, &out->id.id, &out->original) : TSUCHI_NOT_READY;
}

TSUCHI_Result service_wrap_function(TSUCHI_Mod* mod, const char* site, void* function, TSUCHI_Hook* out) {
    for (unsigned i = 0; i < t::code_site_count; ++i)
        if (!strcmp(t::code_sites[i].name, site)) return hook_code_site(mod, i, function, &out->id.id, &out->original);
    return TSUCHI_NOT_FOUND;
}

const hook::Backend service{service_wrap, service_wrap_address, service_remove, service_wrap_function,
                            service_function, service_inline, service_patch, service_check, service_call};

} // namespace

void hooks_start() { hook::set_backend(&service); }
void set_native_hooks(const NativeHooks* backend) { native_hooks = backend; }
const NativeHooks* native_hooks_backend() { return native_hooks; }
void set_dynamic_remove(TSUCHI_Result (*remove)(TSUCHI_Mod*, uint64_t)) { dynamic_remove_function = remove; }

HookStats hook_stats() {
    Lock lock;
    HookStats stats{};
    stats.thunks_limit = supply();
    stats.thunks_used = stats.thunks_limit - thunks_left();
    for (const Hook& hook : records) if (hook.id) {
        if (hook.live) ++stats.live_records;
        else ++stats.retired_records;
    }
    for (uint32_t i = 0; i < frames_high; ++i) stats.frames_active += frames[i].hook != 0;
    stats.frames_peak = frames_peak;
    return stats;
}

} // namespace tsuchi

// Untracked selection is used by host fixtures that inspect a thunk. Executable
// thunks use enter/leave so removal cannot reuse a running hook's continuation.
extern "C" uint64_t tsuchi_hook_target(uint32_t thunk) {
    using namespace tsuchi;
    Lock lock;
    uint32_t site = 0, at = 0;
    if (!chain(thunk, site, at)) return 0;
    return at ? records[at - 1].function : base_of(site);
}
extern "C" TsuchiHookDispatch tsuchi_hook_enter(uint32_t thunk, uint64_t stack, uint64_t return_to) {
    using namespace tsuchi;
    const uint64_t thread = current_thread_token();
    Lock lock;
    uint32_t site = 0, at = 0;
    if (!chain(thunk, site, at)) return {0, 0};
    // Only an original thunk denotes continuation of an existing chain. An
    // entry thunk reached recursively must retain its own caller instead.
    const uint32_t decoded = !(thunk & kCodeSiteThunk) && thunk < supply() ? thunks[thunk] : 0;
    if (!(decoded & kForHook)) ++sites[site].calls;
    if (!at) return {base_of(site), 0};
    uint64_t caller = return_to;
    if (decoded & kForHook) {
        const uint32_t outer = decoded & ~kForHook;
        const Frame* parent = nullptr;
        for (uint32_t i = 0; i < frames_high; ++i)
            if (frames[i].hook == outer && frames[i].thread == thread &&
                (!parent || frames[i].token > parent->token)) parent = &frames[i];
        caller = parent ? parent->caller : 0;
    }
    uint32_t free = kHookThunks, previous = 0;
    for (uint32_t i = 0; i < frames_high; ++i) {
        if (!frames[i].hook && free == kHookThunks) free = i;
        if (frames[i].hook && frames[i].stack == stack && frames[i].thread == thread && frames[i].top) previous = i + 1;
    }
    if (free == kHookThunks && frames_high < kHookThunks) free = frames_high;
    // With no return-tracking frame, do not call a hook whose storage could be
    // reused before it returns. Preserve the site's original game behavior.
    if (free == kHookThunks) return {base_of(site), 0};
    if (previous) frames[previous - 1].top = false;
    frames[free] = Frame{stack, return_to, caller, next_frame_token++, thread, at, previous, true};
    if (free == frames_high) ++frames_high;
    if (frames_high > frames_peak) frames_peak = frames_high;
    ++records[at - 1].calls;
    return {records[at - 1].function, 1};
}
extern "C" uint64_t tsuchi_hook_leave(uint64_t stack) {
    using namespace tsuchi;
    const uint64_t thread = current_thread_token();
    Lock lock;
    for (uint32_t i = 0; i < frames_high; ++i) {
        Frame& frame = frames[i];
        if (!frame.hook || !frame.top || frame.stack != stack || frame.thread != thread) continue;
        const uint64_t return_to = frame.return_to;
        if (frame.previous) frames[frame.previous - 1].top = true;
        Hook& hook = records[frame.hook - 1];
        --hook.calls;
        frame = Frame{};
        while (frames_high && !frames[frames_high - 1].hook) --frames_high;
        reclaim(hook);
        return return_to;
    }
    return 0; // only tracked calls return through the assembly trampoline
}
