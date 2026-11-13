#include "blueprint_motions.h"

#include <string.h>

#include "actors.h"
#include "families.h"
#include "animation_service.h"
#include "frame.h"
#include "game.h"
#include "holds.h"
#include "names.h"
#include "objects.h"
#include "parts.h"
#include "reflect.h"
#include "registry.h"
#include "texts.h"

namespace tsuchi {
namespace {

namespace t = game::table;
using game::Name;

game::ClassRef blueprint_class{t::name::motion_blueprint_class};

// A name as the game passes one to a function and keeps one in a field: its number, then its suffix.
uint64_t packed(Name name) { return uint64_t(name.index) | uint64_t(name.number) << 32; }
Name unpacked(uint64_t value) { return Name{uint32_t(value), uint32_t(value >> 32)}; }

// The blueprint of the game's own that runs on a skeletal mesh now, or 0. A mesh a clip is held on has none, and the
// blueprint that runs once the clip's hold ends is another object.
uint64_t blueprint_of(uint64_t part) {
    const uint64_t blueprint = game::read_or<uint64_t>(part + t::skeletal_mesh::blueprint);
    return blueprint && game::is_a(blueprint, blueprint_class) ? blueprint : 0;
}

TSUCHI_Result resolve(uint64_t part, uint64_t& blueprint) {
    if (!game::is_skeletal_part(part)) return TSUCHI_UNSUPPORTED;
    if (!game::find_class(blueprint_class)) return TSUCHI_NOT_READY;
    blueprint = blueprint_of(part);
    return blueprint ? TSUCHI_OK : TSUCHI_UNSUPPORTED;
}

// Calls `visit` with each of the blueprint's state machines, as a motion_entry, until it gives false. False when the
// blueprint's list of them cannot be read.
template <class Visit>
bool each_machine(uint64_t blueprint, Visit&& visit) {
    namespace l = t::row_list;
    namespace e = t::motion_entry;
    constexpr int32_t kMostMachines = 4096;
    const uint64_t list = blueprint + t::motion_blueprint::motions;
    int32_t slot_count = 0, free_count = 0, bit_count = 0;
    uint64_t slots = 0, far = 0;
    if (!game::read(list + l::slots, slots) || !game::read(list + l::slot_count, slot_count) || !game::read(list + l::free_count, free_count) ||
        !game::read(list + l::bit_count, bit_count) || !game::read(list + l::used_bits_far, far))
        return false;
    if (slot_count < 0 || slot_count > kMostMachines || (slot_count && !game::readable(slots, uint64_t(slot_count) * e::size))) return false;
    const uint64_t bits = far ? far : list + l::used_bits;
    for (int32_t i = 0; i < slot_count; ++i) {
        // With no slot free, every slot is in use and no bit needs looking at.
        if (free_count > 0 && (i >= bit_count || !(game::read_or<uint32_t>(bits + 4 * uint64_t(i >> 5)) >> (i & 31) & 1))) continue;
        if (!visit(slots + uint64_t(i) * e::size)) break;
    }
    return true;
}

// The entry of a state machine, found by its name as a number or as text. 0 when the blueprint has no such machine.
uint64_t machine_entry(uint64_t blueprint, uint64_t machine) {
    uint64_t found = 0;
    each_machine(blueprint, [&](uint64_t entry) {
        if (game::read_or<uint64_t>(entry + t::motion_entry::state_machine) == machine) found = entry;
        return !found;
    });
    return found;
}
uint64_t machine_entry(uint64_t blueprint, const char* machine) {
    uint64_t found = 0;
    each_machine(blueprint, [&](uint64_t entry) {
        if (game::name_is(game::read_or<Name>(entry + t::motion_entry::state_machine), machine)) found = entry;
        return !found;
    });
    return found;
}

// A name given back as text: the name of nothing is empty text, where the game would say None.
TSUCHI_Result give_name(uint64_t name, char* buffer, uint64_t capacity, uint64_t* out) {
    if (name) return game::name_into(unpacked(name), buffer, capacity, out);
    *out = 1;
    if (capacity < 1) return TSUCHI_CAPACITY;
    buffer[0] = 0;
    return TSUCHI_OK;
}
const char* name_or_empty(uint64_t name, char* buffer, size_t capacity) {
    if (!name || !game::name_text(unpacked(name), buffer, capacity)) buffer[0] = 0;
    return buffer;
}

// Names a hold may add to the game's table in one run. The game never forgets a name, so holds that name
// motions from text a mod received cannot grow the table past this.
constexpr uint32_t kNewMotionNames = 1024;
uint32_t new_motion_names = 0;

// The game's name for a motion, made if the game has none yet and the budget allows. Empty text is the name
// of nothing.
TSUCHI_Result motion_name(const char* text, uint64_t& out) {
    Name name{};
    TSUCHI_Result found = game::existing_name_from_utf8(text, name);
    if (found == TSUCHI_NOT_FOUND) {
        if (new_motion_names >= kNewMotionNames) return TSUCHI_CAPACITY;
        found = game::name_from_utf8(text, name);
        if (found == TSUCHI_OK) ++new_motion_names;
    }
    if (found == TSUCHI_CAPACITY) return TSUCHI_INVALID;
    out = packed(name);
    return found;
}

// The blueprint's own functions, each of which some kinds of blueprint override. False when one cannot be reached.
bool set_motion_on(uint64_t blueprint, uint64_t machine, uint64_t motion) {
    const auto set = game::method(blueprint, t::motion_blueprint_table::set_motion);
    if (!set) return false;
    set(reinterpret_cast<void*>(blueprint), motion, machine);
    return true;
}
bool set_move_motion_on(uint64_t blueprint, uint64_t motion) {
    const auto set = game::method(blueprint, t::motion_blueprint_table::set_move_motion);
    if (!set) return false;
    set(reinterpret_cast<void*>(blueprint), motion);
    return true;
}
bool set_speed_on(uint64_t blueprint, float speed) {
    const auto set = game::method(blueprint, t::motion_blueprint_table::set_speed);
    if (!set) return false;
    set(reinterpret_cast<void*>(blueprint), speed);
    return true;
}
uint64_t move_motion_in(uint64_t blueprint) { return game::read_or<uint64_t>(blueprint + t::motion_blueprint::move_motion); }
float speed_in(uint64_t blueprint) { return game::read_or<float>(blueprint + t::motion_blueprint::speed); }

// ---- The holds ------------------------------------------------------------------------------------------------
//
// A hold of a state machine is on a mesh, and the thing of the mesh that is held is the machine's name as a number. A
// hold of a move motion or a speed is on the mesh itself. Each keeps the blueprint it last set its value on, so that
// a blueprint that runs anew, as one does when a clip's hold on the mesh ends, is given the value again; and what the
// game last wanted, to put back.

// Frames a hold goes on what its last full look found, while the mesh keeps the same blueprint: the blueprint's
// class, its list of state machines and its handle are looked at again only then (seen 11-10: 277 motion holds
// looked at in full each frame cost a laptop about 2 ms a frame).
constexpr uint8_t kFullLookEvery = 16;

struct NameHeld {
    uint64_t blueprint; // a handle; 0 while no blueprint of the game's own runs on the mesh
    uint64_t held, game; // names
    uint64_t raw;       // the blueprint's address at the last full look
    uint64_t at;        // where the held name is in it
    uint8_t since;      // frames since that look
};
struct SpeedHeld {
    uint64_t blueprint;
    float held, game;
    float stored; // what the blueprint kept the last time the hold set it, which a blueprint that eases need not be `held`
    uint64_t raw;
    uint8_t since;
};

// What the hold of a state machine or of a move motion reads and sets.
struct NamePlace {
    uint64_t (*read)(uint64_t blueprint, uint64_t what, bool& there);
    bool (*set)(uint64_t blueprint, uint64_t what, uint64_t name);
    uint64_t (*where)(uint64_t blueprint, uint64_t what); // where `read` finds the name, or 0
};
uint64_t read_machine(uint64_t blueprint, uint64_t what, bool& there) {
    const uint64_t entry = machine_entry(blueprint, what);
    there = entry != 0;
    return entry ? game::read_or<uint64_t>(entry + t::motion_entry::motion) : 0;
}
bool set_machine(uint64_t blueprint, uint64_t what, uint64_t name) { return set_motion_on(blueprint, what, name); }
uint64_t where_machine(uint64_t blueprint, uint64_t what) {
    const uint64_t entry = machine_entry(blueprint, what);
    return entry ? entry + t::motion_entry::motion : 0;
}
uint64_t read_move(uint64_t blueprint, uint64_t, bool& there) {
    there = game::readable(blueprint + t::motion_blueprint::move_motion, sizeof(uint64_t));
    return move_motion_in(blueprint);
}
bool set_move(uint64_t blueprint, uint64_t, uint64_t name) { return set_move_motion_on(blueprint, name); }
uint64_t where_move(uint64_t blueprint, uint64_t) { return blueprint + t::motion_blueprint::move_motion; }
constexpr NamePlace machine_place{read_machine, set_machine, where_machine}, move_place{read_move, set_move, where_move};

template <const NamePlace& place>
TSUCHI_Result apply_name(const Hold& hold, NameHeld& entry, const uint64_t& wanted, bool fresh) {
    const game::CallGuard guard = hold_call_guard(hold);
    if (!guard.permits()) return TSUCHI_NOT_READY;
    const uint64_t blueprint = blueprint_of(game::from_handle(hold.object));
    bool there = false;
    const uint64_t now = blueprint ? place.read(blueprint, hold.what, there) : 0;
    if (!blueprint || !there) {
        if (fresh) return blueprint ? TSUCHI_NOT_FOUND : TSUCHI_UNSUPPORTED;
        entry.held = wanted, entry.blueprint = 0; // set once a blueprint runs on the mesh again
        return TSUCHI_OK;
    }
    if (fresh) entry.game = now;
    entry.held = wanted, entry.blueprint = game::to_handle(blueprint);
    hold_apply_checkpoint(hold);
    if (!place.set(blueprint, hold.what, wanted)) return fresh ? TSUCHI_NOT_READY : TSUCHI_OK;
    return guard.permits() ? TSUCHI_OK : TSUCHI_NOT_READY;
}

template <const NamePlace& place>
bool keep_name(const Hold& hold, NameHeld& entry) {
    const game::CallGuard guard = hold_call_guard(hold);
    if (!guard.permits()) return true;
    const uint64_t part = game::from_handle(hold.object);
    // The same blueprint as at the last full look, and it still plays the held motion: nothing to do.
    const uint64_t raw = part ? game::read_or<uint64_t>(part + t::skeletal_mesh::blueprint) : 0;
    if (raw && raw == entry.raw && entry.at && entry.blueprint && ++entry.since < kFullLookEvery &&
        game::read_or<uint64_t>(entry.at) == entry.held)
        return true;
    entry.since = 0;
    const uint64_t blueprint = blueprint_of(part);
    bool there = false;
    const uint64_t now = blueprint ? place.read(blueprint, hold.what, there) : 0;
    if (!blueprint || !there) { // waits for a blueprint of the game's own, with that state machine
        entry.blueprint = 0, entry.raw = 0, entry.at = 0;
        return true;
    }
    const uint64_t handle = game::to_handle(blueprint);
    entry.raw = blueprint, entry.at = place.where(blueprint, hold.what);
    if (handle == entry.blueprint && now == entry.held) return true;
    entry.game = now, entry.blueprint = handle; // the game set this since, or a new blueprint starts with it
    place.set(blueprint, hold.what, entry.held);
    return true;
}

// Puts back what the game last wanted, on the blueprint the hold last set, unless the game has set another since.
template <const NamePlace& place>
void restore_name(const Hold& hold, NameHeld& entry, HoldEnd why) {
    HoldRestore check{hold, why};
    const uint64_t part = hold_part_to_restore(hold, why);
    const uint64_t blueprint = part ? blueprint_of(part) : 0;
    if (!blueprint || game::to_handle(blueprint) != entry.blueprint) return;
    bool there = false;
    if (place.read(blueprint, hold.what, there) != entry.held || !there || !check.guard().permits()) return;
    place.set(blueprint, hold.what, entry.game);
}

TSUCHI_Result apply_speed(const Hold& hold, SpeedHeld& entry, const float& wanted, bool fresh) {
    const game::CallGuard guard = hold_call_guard(hold);
    if (!guard.permits()) return TSUCHI_NOT_READY;
    const uint64_t blueprint = blueprint_of(game::from_handle(hold.object));
    if (!blueprint) {
        if (fresh) return TSUCHI_UNSUPPORTED;
        entry.held = wanted, entry.blueprint = 0;
        return TSUCHI_OK;
    }
    if (fresh) entry.game = speed_in(blueprint);
    entry.held = wanted, entry.blueprint = game::to_handle(blueprint);
    hold_apply_checkpoint(hold);
    if (!set_speed_on(blueprint, wanted)) return fresh ? TSUCHI_NOT_READY : TSUCHI_OK;
    if (!guard.permits()) return TSUCHI_NOT_READY;
    entry.stored = speed_in(blueprint);
    return TSUCHI_OK;
}

// Some blueprints ease toward the speed they are given, so a held speed is set every frame.
bool keep_speed(const Hold& hold, SpeedHeld& entry) {
    const game::CallGuard guard = hold_call_guard(hold);
    if (!guard.permits()) return true;
    const uint64_t part = game::from_handle(hold.object);
    const uint64_t raw = part ? game::read_or<uint64_t>(part + t::skeletal_mesh::blueprint) : 0;
    const bool same = raw && raw == entry.raw && entry.blueprint && ++entry.since < kFullLookEvery;
    // The same blueprint: its class and handle need no look this frame. Its speed is still set, as some ease.
    const uint64_t blueprint = same ? raw : blueprint_of(part);
    if (!blueprint) {
        entry.blueprint = 0, entry.raw = 0;
        return true;
    }
    if (!same) entry.since = 0, entry.raw = blueprint;
    const uint64_t handle = same ? entry.blueprint : game::to_handle(blueprint);
    const float now = speed_in(blueprint);
    if (handle != entry.blueprint || now != entry.stored) entry.game = now; // the game set this since
    entry.blueprint = handle;
    if (set_speed_on(blueprint, entry.held) && guard.permits()) entry.stored = speed_in(blueprint);
    return true;
}

void restore_speed(const Hold& hold, SpeedHeld& entry, HoldEnd why) {
    HoldRestore check{hold, why};
    const uint64_t part = hold_part_to_restore(hold, why);
    const uint64_t blueprint = part ? blueprint_of(part) : 0;
    if (!blueprint || game::to_handle(blueprint) != entry.blueprint || !check.guard().permits()) return;
    set_speed_on(blueprint, entry.game);
}

Holds<NameHeld, uint64_t, apply_name<machine_place>, keep_name<machine_place>, restore_name<machine_place>> machines{"animation.motion", HoldOn::part, true};
Holds<NameHeld, uint64_t, apply_name<move_place>, keep_name<move_place>, restore_name<move_place>> moves{"animation.move_motion", HoldOn::part, true};
Holds<SpeedHeld, float, apply_speed, keep_speed, restore_speed> speeds{"animation.speed", HoldOn::part, true};

// ---- Changes ---------------------------------------------------------------------------------------------------

constexpr uint32_t kWatchedMachines = 12; // more than any of the game's blueprints has; further ones are not looked at

// What the body of an actor in play was playing when it was last looked at.
struct Seen {
    uint64_t mesh, blueprint; // handles
    uint64_t machines[kWatchedMachines], motions[kWatchedMachines], previous[kWatchedMachines];
    uint64_t move, previous_move;
    uint32_t round; // the last look that found it in play
    uint32_t count; // machines
    uint32_t changed; // a bit for each machine, and the top one for the move motion
};
constexpr uint32_t kMoveChanged = 1u << 31;
Seen* seen = nullptr;
uint32_t seen_count = 0, seen_capacity = 0, round = 0;

Seen* find(uint64_t mesh) {
    for (uint32_t i = 0; i < seen_count; ++i)
        if (seen[i].mesh == mesh) return &seen[i];
    return nullptr;
}

// What the blueprint plays now, as a Seen starts from.
void take_in(Seen& entry, uint64_t blueprint) {
    entry.blueprint = game::to_handle(blueprint), entry.count = 0, entry.changed = 0;
    each_machine(blueprint, [&](uint64_t at) {
        if (entry.count == kWatchedMachines) return false;
        entry.machines[entry.count] = game::read_or<uint64_t>(at + t::motion_entry::state_machine);
        entry.motions[entry.count++] = game::read_or<uint64_t>(at + t::motion_entry::motion);
        return true;
    });
    entry.move = move_motion_in(blueprint);
}

bool look(void*, uint64_t actor) {
    const uint64_t mesh = game::read_or<uint64_t>(actor + t::game_actor::body);
    if (!mesh || !game::is_skeletal_part(mesh)) return true;
    const uint64_t blueprint = blueprint_of(mesh), handle = game::to_handle(mesh);
    if (!blueprint || !handle) return true;
    if (Seen* entry = find(handle)) {
        entry->round = round;
        // A blueprint that runs anew starts from its first motions: where it starts from is not told.
        if (entry->blueprint != game::to_handle(blueprint)) return take_in(*entry, blueprint), true;
        for (uint32_t i = 0; i < entry->count; ++i) {
            const uint64_t at = machine_entry(blueprint, entry->machines[i]);
            const uint64_t now = at ? game::read_or<uint64_t>(at + t::motion_entry::motion) : entry->motions[i];
            if (now == entry->motions[i]) continue;
            if (!(entry->changed & 1u << i)) entry->previous[i] = entry->motions[i]; // told once, from the first it had
            entry->motions[i] = now, entry->changed |= 1u << i;
        }
        const uint64_t move = move_motion_in(blueprint);
        if (move != entry->move) {
            if (!(entry->changed & kMoveChanged)) entry->previous_move = entry->move;
            entry->move = move, entry->changed |= kMoveChanged;
        }
        return true;
    }
    if (seen_count == seen_capacity) {
        const uint32_t next = seen_capacity ? seen_capacity * 2 : 64;
        auto* grown = static_cast<Seen*>(allocate(sizeof(Seen) * next));
        if (!grown) return true;
        if (seen) memcpy(static_cast<void*>(grown), seen, sizeof(Seen) * seen_count);
        release(seen);
        seen = grown;
        seen_capacity = next;
    }
    Seen& entry = seen[seen_count++];
    entry.mesh = handle, entry.round = round;
    take_in(entry, blueprint);
    return true;
}

// Once a frame, while anyone is listening.
void step() {
    if (!animation::motion_changed_wanted() && !animation::move_motion_changed_wanted()) {
        seen_count = 0; // a later listener starts from what meshes play then
        return;
    }
    ++round;
    game::each_actor(look, nullptr);
    for (uint32_t i = seen_count; i--;)
        if (seen[i].round != round) seen[i] = seen[--seen_count];
    // Told after the look, since a handler may change which actors there are and what they play. A handler that
    // stops listening does not empty the list until the next frame.
    static char machine[game::kNameBytes], motion[game::kNameBytes], previous[game::kNameBytes];
    for (uint32_t i = 0; i < seen_count; ++i) {
        for (uint32_t m = 0; m < seen[i].count && i < seen_count; ++m) {
            if (!(seen[i].changed & 1u << m)) continue;
            seen[i].changed &= ~(1u << m);
            const TSUCHI_AnimationMotionChanged event{{seen[i].mesh}, name_or_empty(seen[i].machines[m], machine, sizeof machine),
                                                      name_or_empty(seen[i].motions[m], motion, sizeof motion),
                                                      name_or_empty(seen[i].previous[m], previous, sizeof previous)};
            animation::emit_motion_changed(event);
        }
        if (i >= seen_count || !(seen[i].changed & kMoveChanged)) continue;
        seen[i].changed &= ~kMoveChanged;
        const TSUCHI_AnimationMoveMotionChanged event{{seen[i].mesh}, name_or_empty(seen[i].move, motion, sizeof motion),
                                                      name_or_empty(seen[i].previous_move, previous, sizeof previous)};
        animation::emit_move_motion_changed(event);
    }
}

} // namespace

void blueprint_motions_start() {
    // A build without the game blueprints' layout keeps only the clip calls of animation.
    if (family_absent(t::family::motions)) return;
    machines.start();
    moves.start();
    speeds.start();
    each_frame(step);
}

TSUCHI_Result state_machines_of(uint64_t part, char* buffer, uint64_t capacity, uint64_t* out) {
    uint64_t blueprint = 0;
    const TSUCHI_Result found = resolve(part, blueprint);
    if (found != TSUCHI_OK) return found;
    Texts names;
    bool whole = true;
    char text[game::kNameBytes];
    const bool read = each_machine(blueprint, [&](uint64_t entry) {
        whole = game::name_text(game::read_or<Name>(entry + t::motion_entry::state_machine), text, sizeof text) && names.add(text);
        return whole;
    });
    const TSUCHI_Result given = !read ? TSUCHI_NOT_READY : !whole ? TSUCHI_NO_MEMORY : names.give(buffer, capacity, out);
    names.clear();
    return given;
}

TSUCHI_Result motion_of(uint64_t part, const char* state_machine, char* buffer, uint64_t capacity, uint64_t* out) {
    uint64_t blueprint = 0;
    const TSUCHI_Result found = resolve(part, blueprint);
    if (found != TSUCHI_OK) return found;
    const uint64_t entry = machine_entry(blueprint, state_machine);
    if (!entry) return TSUCHI_NOT_FOUND;
    return give_name(game::read_or<uint64_t>(entry + t::motion_entry::motion), buffer, capacity, out);
}

TSUCHI_Result move_motion_of(uint64_t part, char* buffer, uint64_t capacity, uint64_t* out) {
    uint64_t blueprint = 0;
    const TSUCHI_Result found = resolve(part, blueprint);
    return found != TSUCHI_OK ? found : give_name(move_motion_in(blueprint), buffer, capacity, out);
}

TSUCHI_Result speed_of(uint64_t part, float* out) {
    uint64_t blueprint = 0;
    const TSUCHI_Result found = resolve(part, blueprint);
    if (found != TSUCHI_OK) return found;
    *out = speed_in(blueprint);
    return TSUCHI_OK;
}

TSUCHI_Result hold_motion(TSUCHI_Mod* mod, uint64_t part, const char* state_machine, const char* motion) {
    uint64_t blueprint = 0, name = 0;
    const TSUCHI_Result found = resolve(part, blueprint);
    if (found != TSUCHI_OK) return found;
    const uint64_t entry = machine_entry(blueprint, state_machine);
    if (!entry) return TSUCHI_NOT_FOUND;
    const TSUCHI_Result named = motion_name(motion, name);
    if (named != TSUCHI_OK) return named;
    return machines.take(mod, game::to_handle(part), game::read_or<uint64_t>(entry + t::motion_entry::state_machine), name);
}

// The mod's hold of a state machine is found by the machine's name, as a mod may let go while no blueprint runs.
TSUCHI_Result release_motion(TSUCHI_Mod* mod, uint64_t part, const char* state_machine) {
    const uint64_t handle = game::to_handle(part);
    for (uint32_t i = 0; i < machines.count(); ++i) {
        const Hold& hold = machines.hold_at(i);
        if (hold.mod == mod && hold.object == handle && game::name_is(unpacked(hold.what), state_machine))
            return machines.release(mod, handle, hold.what);
    }
    return TSUCHI_NOT_FOUND;
}

TSUCHI_Result hold_move_motion(TSUCHI_Mod* mod, uint64_t part, const char* motion) {
    uint64_t blueprint = 0, name = 0;
    const TSUCHI_Result found = resolve(part, blueprint);
    if (found != TSUCHI_OK) return found;
    const TSUCHI_Result named = motion_name(motion, name);
    return named != TSUCHI_OK ? named : moves.take(mod, game::to_handle(part), 0, name);
}

TSUCHI_Result release_move_motion(TSUCHI_Mod* mod, uint64_t part) { return moves.release(mod, game::to_handle(part), 0); }

TSUCHI_Result hold_speed(TSUCHI_Mod* mod, uint64_t part, float speed) {
    uint64_t blueprint = 0;
    const TSUCHI_Result found = resolve(part, blueprint);
    return found != TSUCHI_OK ? found : speeds.take(mod, game::to_handle(part), 0, speed);
}

TSUCHI_Result release_speed(TSUCHI_Mod* mod, uint64_t part) { return speeds.release(mod, game::to_handle(part), 0); }

uint32_t motion_holds() { return machines.count() + moves.count() + speeds.count(); }
uint32_t motion_watched() { return seen_count; }

} // namespace tsuchi
