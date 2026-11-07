// Holds: what a mod holds on something of the game's until it lets go or stops.
//
// A part held hidden, a material held in a slot, an actor held suspended, a
// pause, a player's controller: each is a hold, and every service that offers
// one keeps it in a table of this one make. The table knows what each hold is
// on, whose it is, and what its service remembers for it. It records the hold
// against its mod, so a mod that stops gives it back; it watches for actors
// leaving play and for objects that are gone; and it calls the service's three
// steps:
//
//   apply    puts a hold in force, or changes one the mod already has
//   keep     once a frame, puts back what the game has undone meanwhile
//   restore  puts things back as the game had them, told why the hold ends
//
// Game thread only.
#pragma once
#include <stddef.h>
#include <stdint.h>

#include "cost_clock.h"
#include "registry.h"
#include "reflect.h"

namespace tsuchi {

enum class ActorLeft : uint8_t;

// Why a hold ends.
enum class HoldEnd : uint8_t {
    released,  // the mod let go of it
    stopped,   // the mod stopped
    lost,      // the step that keeps it found it cannot go on, though what it is on is still there
    pooled,    // the actor it is on leaves play and lives on to be used again: it leaves as the game made it
    detached,  // ordinary actor leaves its level while its components are still live
    destroying, // admitted ordinary destruction: forget leases before native component teardown
    destroyed, // what it is on is being destroyed, or is gone already: there is nothing to put back
};

// What a hold is on, which says when it ends by itself.
enum class HoldOn : uint8_t {
    nothing, // the game itself, or a player: it ends only when it is let go
    object,  // an object: it ends when the object is gone
    part,    // a part of an actor: it ends when the part is gone, and when its actor leaves play
    actor,   // an actor: it ends when the actor leaves play
    watched_actor, // also admits and watches ordinary actor teardown before native components leave
};

// One hold, as the steps see it.
struct Hold {
    uint64_t object; // a handle to what it is on, or 0 for a hold on nothing that can go
    uint64_t what;   // which thing of the object is held: a slot's number, a player's. 0 when there is one
    uint64_t id;     // what the mod's record of it is kept under
    TSUCHI_Mod* mod; // whose it is. Null for a hold of the framework's own, which no mod's stopping ends
    uint64_t ordinary_owner; // owner generation for ordinary component restoration, 0 for the gameplay path
};

// Resolves a surviving part immediately before a restoration call. Gameplay
// owners keep their existing restoration policies. Ordinary destruction is skipped.
uint64_t hold_part_to_restore(const Hold& hold, HoldEnd why);
// The time the steps that keep every table's holds have taken, a step at a time.
CostTotal hold_pass_cost();
struct HoldRestore {
    Hold hold;
    HoldEnd why;
    static bool allowed(void* context) {
        const auto& check = *static_cast<HoldRestore*>(context);
        return hold_part_to_restore(check.hold, check.why) != 0;
    }
    game::CallGuard guard() { return {allowed, this}; }
};

// A synchronous apply/keep guard. It requires the exact active record, calling
// mod and captured object/ordinary-owner generations. It cannot escape the call.
game::CallGuard hold_call_guard(const Hold& hold);
// Mark provisional native state before a call that may stop the hold. An ended
// fresh apply then restores its stack state once after that native call returns.
void hold_apply_checkpoint(const Hold& hold);

// The most a service can keep with one hold, in bytes. What it keeps is plain data.
constexpr uint32_t kHoldKeptBytes = 64;

// The table, with what a service keeps for a hold as bytes. Services use Holds, below.
class HoldTable {
  public:
    using Apply = TSUCHI_Result (*)(const Hold& hold, void* kept, const void* wanted, bool fresh);
    using Keep = bool (*)(const Hold& hold, void* kept);
    using Restore = void (*)(const Hold& hold, void* kept, HoldEnd why);

    constexpr HoldTable(const char* name, HoldOn on, bool exclusive, uint32_t kept_bytes, Apply apply, Keep keep, Restore restore)
        : kind_{name}, on_(on), exclusive_(exclusive), stride_(uint32_t(sizeof(Hold)) + ((kept_bytes + 7u) & ~7u)), apply_(apply), keep_(keep), restore_(restore) {}

    void start();
    bool reserve(uint32_t more);
    TSUCHI_Result take(TSUCHI_Mod* mod, uint64_t object, uint64_t what, const void* wanted);
    TSUCHI_Result release(TSUCHI_Mod* mod, uint64_t object, uint64_t what);
    void* find(uint64_t object, uint64_t what) const;
    void* find(const TSUCHI_Mod* mod, uint64_t object, uint64_t what) const;
    TSUCHI_Mod* holder(uint64_t object, uint64_t what) const;
    // A no-op update still requires the captured ordinary owner association.
    bool association_live(const TSUCHI_Mod* mod, uint64_t object, uint64_t what) const;
    uint32_t count() const { return count_; }
    uint32_t count_on(uint64_t object, uint64_t what) const;
    const Hold& hold_at(uint32_t index) const { return *reinterpret_cast<const Hold*>(slots_ + size_t(index) * stride_); }
    void* kept_at(uint32_t index) const;

  private:
    struct Operation {
        HoldTable* table;
        Hold hold;
        alignas(8) uint8_t kept[kHoldKeptBytes];
        Operation* next;
        bool ended = false, restorable = false;
        HoldEnd why = HoldEnd::released;
    };
    Operation* active_ = nullptr;
    static bool operation_allowed(void* context);
    TSUCHI_Result apply(uint32_t index, const void* wanted, bool fresh);
    bool active_on(uint64_t object, uint64_t what) const;
    friend game::CallGuard hold_call_guard(const Hold& hold);
    friend void hold_apply_checkpoint(const Hold& hold);
    Hold& slot(uint32_t index) { return *reinterpret_cast<Hold*>(slots_ + size_t(index) * stride_); }
    int32_t index_of(const TSUCHI_Mod* mod, bool anyones, uint64_t object, uint64_t what) const;
    int32_t index_of(uint64_t id) const;
    void remove(uint32_t index);
    void end(uint32_t index, HoldEnd why);
    void step();
    void left(uint64_t actor, uint64_t handle, HoldEnd why);
    static void on_stop(void* table, uint64_t id);
    static void step_all();
    static void detached_all(uint64_t actor, uint64_t handle, bool destroying);
    static void left_all(uint64_t actor, uint64_t handle, ActorLeft how);

    Kind kind_;
    HoldOn on_;
    bool exclusive_;
    bool started_ = false;
    uint32_t stride_;
    Apply apply_;
    Keep keep_;
    Restore restore_;
    uint8_t* slots_ = nullptr;
    uint32_t count_ = 0, capacity_ = 0;
    uint64_t next_id_ = 1;
    HoldTable* next_ = nullptr;
};

// For a hold with nothing to keep, or nothing to ask for.
struct HoldNothing {};

// One kind of hold: a name for it, what it is on, how many mods may hold the same
// thing, what the service keeps for each hold, what a mod asks for, and the three
// steps. One static Holds per kind.
//
//   exclusive  one mod at a time holds a thing. Another mod is refused with TSUCHI_BUSY.
//   counted    any number of mods hold the same thing, each once. A step can ask how
//              many holds are on a thing, to change the game with the first and put
//              it back with the last.
//
// A mod that takes what it already holds changes its hold: apply is called again
// with what is asked for now.
//
//   Apply    (hold, kept, wanted, fresh). With `fresh`, the hold is new and `kept`
//            is all zeros: remember what the game had and make the change. Any
//            result but TSUCHI_OK means nothing is held, and apply has put back
//            whatever it changed. An apply that transfers caller-owned data
//            reports that transfer separately: reentrant stop can consume and
//            restore a successful apply while take returns NOT_READY.
//            Without `fresh`, the mod is changing its hold,
//            and the hold stays whatever the result.
//   Keep     (hold, kept), once a frame for a hold whose object is still there.
//            False when the hold cannot go on: it ends, and restore is told `lost`.
//            A keep that can lead into a mod's code does that last, since the mod
//            may take and release holds there.
//   Restore  (hold, kept, why), once, when the hold ends for any reason. The hold
//            is out of the table by then.
//
// A step that is not needed is null.
template <typename Kept, typename Wanted, TSUCHI_Result (*Apply)(const Hold&, Kept&, const Wanted&, bool), bool (*Keep)(const Hold&, Kept&),
          void (*Restore)(const Hold&, Kept&, HoldEnd)>
class Holds {
    static_assert(sizeof(Kept) <= kHoldKeptBytes && alignof(Kept) <= 8 && __is_trivially_copyable(Kept), "what a hold keeps is a little plain data");

  public:
    constexpr Holds(const char* name, HoldOn on, bool exclusive) : table_(name, on, exclusive, sizeof(Kept), Apply != nullptr ? apply : nullptr, Keep != nullptr ? keep : nullptr, Restore != nullptr ? restore : nullptr) {}

    // Puts this table in the line of those that are kept each frame and told of actors leaving. Tables
    // are gone through in the order they were started; one that was not started starts with its first hold.
    void start() { table_.start(); }
    // Makes room for `more` further holds, so that taking them cannot fail for want of memory. False when there is none.
    bool reserve(uint32_t more) { return table_.reserve(more); }
    // Holds (object, what) for the mod as `wanted` says, or changes the hold the mod has on it. `object` is a
    // handle. TSUCHI_BUSY when it is another mod's and the kind is exclusive; TSUCHI_NO_MEMORY; or what Apply said.
    TSUCHI_Result take(TSUCHI_Mod* mod, uint64_t object, uint64_t what, const Wanted& wanted) {
        // The request can itself be a value in a table that a callback grows.
        const Wanted captured = wanted;
        return table_.take(mod, object, what, &captured);
    }
    // Ends the mod's hold as released. TSUCHI_NOT_FOUND when it has none on that.
    TSUCHI_Result release(TSUCHI_Mod* mod, uint64_t object, uint64_t what) { return table_.release(mod, object, what); }
    // What is kept for the hold on a thing: anyone's, the first when several mods hold it; or the mod's own. Null when there is none.
    Kept* find(uint64_t object, uint64_t what) const { return static_cast<Kept*>(table_.find(object, what)); }
    Kept* find(const TSUCHI_Mod* mod, uint64_t object, uint64_t what) const { return static_cast<Kept*>(table_.find(mod, object, what)); }
    // The mod that holds a thing, the first when several do. Null when nobody does, and for a hold of the framework's own.
    TSUCHI_Mod* holder(uint64_t object, uint64_t what) const { return table_.holder(object, what); }
    bool association_live(const TSUCHI_Mod* mod, uint64_t object, uint64_t what) const { return table_.association_live(mod, object, what); }
    // How many holds there are, and how many on one thing.
    uint32_t count() const { return table_.count(); }
    uint32_t count_on(uint64_t object, uint64_t what) const { return table_.count_on(object, what); }
    // Each hold in turn, by its place below count(). Places change when a hold ends.
    const Hold& hold_at(uint32_t index) const { return table_.hold_at(index); }
    Kept& kept_at(uint32_t index) const { return *static_cast<Kept*>(table_.kept_at(index)); }

  private:
    static TSUCHI_Result apply(const Hold& hold, void* kept, const void* wanted, bool fresh) {
        return Apply != nullptr ? Apply(hold, *static_cast<Kept*>(kept), *static_cast<const Wanted*>(wanted), fresh) : TSUCHI_OK;
    }
    static bool keep(const Hold& hold, void* kept) { return Keep != nullptr ? Keep(hold, *static_cast<Kept*>(kept)) : true; }
    static void restore(const Hold& hold, void* kept, HoldEnd why) {
        if (Restore != nullptr) Restore(hold, *static_cast<Kept*>(kept), why);
    }

    HoldTable table_;
};

} // namespace tsuchi
