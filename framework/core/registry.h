// The core every service is built on: mods, what each mod owns, and events.
//
// A mod's context (TSUCHI_Mod) is made by the framework when the mod loads. Whatever
// the mod makes or holds through any service is recorded against that context, and
// stopping the mod releases all of it here, newest first. No service keeps its own
// list of what to clean up.
//
// All of it belongs to the game's thread: nothing here locks. A call that comes
// on another thread is refused before it touches anything (see "The game's
// thread" below).
#pragma once
#include <stddef.h>
#include <stdint.h>

#include "tsuchi/core.h"

namespace tsuchi {

// Memory for the core's own tables. Hosted builds default to malloc; the game
// build sets this before anything else runs.
struct Allocator {
    void* (*allocate)(size_t bytes);
    void (*release)(void* block);
};
void set_allocator(Allocator allocator);
// The same memory, for the rest of the framework. `allocate` gives null when there is none.
void* allocate(size_t bytes);
void release(void* block);

// ---- Mods -----------------------------------------------------------------

constexpr size_t kModIdBytes = 48; // the longest ID, 47 bytes, and the terminator

// How a mod's ID is spelled: 1 to 47 of a-z, 0-9, '.', '_' and '-', beginning and ending with a
// letter or a digit. The name a mod gives a thing it adds, and the key of a setting, are spelled
// the same way. This is the one place the rule is written; ids.h builds the rest of an ID on it.
bool valid_mod_id(const char* id);

// A new context for a mod that is starting. Null for a bad id, or one a mod that
// has not stopped already uses. A mod that starts again in its old place, as a restart
// does, passes the rank its first context had; 0 gives the next rank.
TSUCHI_Mod* mod_create(const char* id, uint32_t rank = 0);
// Releases everything the mod owns, newest first, and ends its subscriptions.
// Calls made with the context afterwards fail.
void mod_stop(TSUCHI_Mod* mod);
// True for a context made here that has not been stopped, asked on the game's thread. Every call a mod makes
// begins with this, so a call from another thread is refused the way a stopped mod's is, and changes nothing.
bool mod_usable(const TSUCHI_Mod* mod);
// The same whatever the thread. For the one thing that is answered on any thread: where a hooked call goes.
bool mod_running(const TSUCHI_Mod* mod);
const char* mod_id(const TSUCHI_Mod* mod);
// Where a mod comes among those that were started: one started later has a higher number. Where several mods
// have a say on the same thing, the say of the one started last is in force.
uint32_t mod_rank(const TSUCHI_Mod* mod);
// Notes that the mod cannot run, and why, in words the mod list can show: something it asked for was refused
// that it cannot do without. Whoever starts mods looks once the mod's start is over, and blocks it. `why` is
// kept as a pointer, so it has to last; the first reason given stays.
void mod_block(TSUCHI_Mod* mod, const char* why);
// What mod_block was told, or null.
const char* mod_block_reason(const TSUCHI_Mod* mod);

// ---- The game's thread ----------------------------------------------------

// The build says how to tell threads apart: a function that gives a number for the thread it is called on,
// never 0 and never the same for two threads. With none set, every thread passes. A hosted test sets none.
void set_thread_probe(uint64_t (*current)());
// Notes the calling thread as the game's. The per-frame call does this each frame, since the game's thread is
// the one its frames come on.
void note_game_thread();
// Internal identity for dispatch provenance. Hosted builds without a probe use 1.
uint64_t current_thread_token();
// True on the game's thread, and on any thread until one has been noted, which is the time before the game's
// first frame.
bool on_game_thread();
// Exact nonzero noted/current witness, or 0 before a frame and on a foreign thread.
uint64_t known_game_thread_token();
// How many calls have been refused for coming on another thread.
uint32_t off_thread_calls();
// The build says how a thread waiting on a lock lets others run: on the console, a short sleep, as a thread of
// higher priority spinning there would otherwise keep the lock's holder off its core for good. With none set, a
// waiter only spins.
void set_thread_pause(void (*pause)());
// A lock's waiter, once for each time it finds the lock held. It spins at first and then pauses the thread.
void lock_wait(uint32_t& tries);

// ---- Ownership ------------------------------------------------------------

// What kind of thing is owned. One static Kind per kind; its address identifies it.
struct Kind {
    const char* name;
};
// Called when the owner stops while it still owns the thing.
using Release = void (*)(void* context, uint64_t key);

// Records that `mod` owns the thing (kind, key). With `exclusive`, the thing has
// one owner: TSUCHI_BUSY while another mod owns it, and TSUCHI_OK without a second
// record when this mod already does. Without it, every call adds a record.
// Reserves room for a native transaction's ownership records without publishing them.
TSUCHI_Result reserve_owned(TSUCHI_Mod* mod, uint32_t additional);
TSUCHI_Result own(TSUCHI_Mod* mod, const Kind& kind, uint64_t key, Release release, void* context, bool exclusive = false);
// Removes the mod's newest record of the thing without calling its release: the
// mod gave it up itself, or the thing is gone. TSUCHI_NOT_FOUND when it has none.
// The room a record took is used again, so a mod's list is as long as what it
// has owned at one time, however much it has owned and given up.
TSUCHI_Result disown(TSUCHI_Mod* mod, const Kind& kind, uint64_t key);
// The mod that owns the thing, or null. For things owned exclusively.
TSUCHI_Mod* owner(const Kind& kind, uint64_t key);
// How many things the mod owns, subscriptions included.
uint32_t owned_count(const TSUCHI_Mod* mod);
// Captures the next ownership serial. Serial ranges remain stable through compaction.
uint64_t ownership_mark(const TSUCHI_Mod* mod);
// Releases only records acquired in [first, last). Later records are left alone.
void release_owned_between(TSUCHI_Mod* mod, uint64_t first, uint64_t last);
// During declaration admission, updates to older records are refused before mutation.
// Returns the previous floor, which the caller restores after its native apply.
uint64_t isolate_ownership(TSUCHI_Mod* mod, uint64_t first);
bool ownership_update_allowed(const TSUCHI_Mod* mod, const Kind& kind, uint64_t key);
bool ownership_isolated(const TSUCHI_Mod* mod);
bool ownership_in_range(const TSUCHI_Mod* mod, const char* kind, uint64_t key, uint64_t first, uint64_t last);

// ---- Events ---------------------------------------------------------------

struct Listener;
// One event. A service has one static Topic per event it emits.
struct Topic {
    const char* name;
    // Calls a subscriber's function with its own types.
    void (*invoke)(void (*function)(), void* user, const void* event);
    Listener* listeners = nullptr;
    uint32_t count = 0, capacity = 0, emitting = 0, ended = 0;
};

TSUCHI_Result subscribe(Topic& topic, TSUCHI_Mod* mod, void (*function)(), void* user, TSUCHI_Subscription* out);
// Ends one of the mod's own subscriptions. TSUCHI_NOT_FOUND for any other id.
TSUCHI_Result cancel(TSUCHI_Mod* mod, TSUCHI_Subscription subscription);
// Calls every subscriber in the order they subscribed. A subscriber may cancel
// any subscription while it runs. One added while the event is being delivered
// first hears the next event. An event emitted on another thread than the game's
// reaches nobody: a subscriber's code runs on the game's thread only.
void emit(Topic& topic, const void* event);
// Delivers only to this mod. Null or stopped recipients receive nothing.
void emit_to(Topic& topic, TSUCHI_Mod* recipient, const void* event);
// Lets a service skip work nobody is listening for.
uint32_t subscriber_count(const Topic& topic);

// ---- Checks the generated service code uses -------------------------------

inline bool finite(float value) { return value - value == 0; }
inline bool finite(const TSUCHI_Vec3* value) { return value && finite(value->x) && finite(value->y) && finite(value->z); }
inline bool finite(const TSUCHI_Rotation* value) { return value && finite(value->pitch) && finite(value->yaw) && finite(value->roll); }

} // namespace tsuchi
