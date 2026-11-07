// The behavior service: what an actor is doing, and telling mods when that changes.
//
// The game does not say when an actor starts doing something else, so while a
// mod is listening every actor in play is looked at once a frame and compared
// with what it was doing the frame before.
#include <string.h>

#include "actions.h"
#include "actors.h"
#include "behavior_service.h"
#include "frame.h"
#include "objects.h"
#include "registry.h"
#include "services.h"

namespace tsuchi {
namespace {

// What an actor was doing when it was last looked at.
struct Seen {
    uint64_t handle;
    uint64_t signature;
    char* path; // the same in words, for telling what it was doing before
    uint32_t capacity;
    uint32_t round; // the last look that found it in play
    bool changed;
};
Seen* seen = nullptr;
uint32_t count = 0, capacity = 0, round = 0;
char* scratch = nullptr;
uint64_t scratch_capacity = 0;

// What the actor is doing, in `scratch`. False when it cannot be told or there is no memory for it.
bool describe(uint64_t actor) {
    uint64_t needed = 0;
    TSUCHI_Result result = game::action_path(actor, scratch, scratch_capacity, &needed);
    if (result == TSUCHI_CAPACITY) {
        char* grown = static_cast<char*>(allocate(size_t(needed)));
        if (!grown) return false;
        release(scratch);
        scratch = grown;
        scratch_capacity = needed;
        result = game::action_path(actor, scratch, scratch_capacity, &needed);
    }
    return result == TSUCHI_OK;
}

// Keeps what `scratch` says as what the actor was last seen doing.
bool keep(Seen& entry) {
    const size_t bytes = strlen(scratch) + 1;
    if (bytes > entry.capacity) {
        char* grown = static_cast<char*>(allocate(bytes));
        if (!grown) return false;
        release(entry.path);
        entry.path = grown;
        entry.capacity = uint32_t(bytes);
    }
    memcpy(entry.path, scratch, bytes);
    return true;
}

Seen* find(uint64_t handle) {
    for (uint32_t i = 0; i < count; ++i)
        if (seen[i].handle == handle) return &seen[i];
    return nullptr;
}

bool look(void*, uint64_t actor) {
    uint64_t signature = 0;
    const uint64_t handle = game::to_handle(actor);
    if (!handle || !game::action_signature(actor, signature)) return true;
    if (Seen* entry = find(handle)) {
        entry->round = round;
        if (entry->signature != signature) entry->signature = signature, entry->changed = true;
        return true;
    }
    // Seen for the first time: what it is doing now is where it starts from.
    if (count == capacity) {
        const uint32_t next = capacity ? capacity * 2 : 256;
        auto* grown = static_cast<Seen*>(allocate(sizeof(Seen) * next));
        if (!grown) return true;
        if (seen) memcpy(static_cast<void*>(grown), seen, sizeof(Seen) * count);
        release(seen);
        seen = grown;
        capacity = next;
    }
    Seen entry{handle, signature, nullptr, 0, round, false};
    if (describe(actor) && keep(entry)) seen[count++] = entry;
    return true;
}

void forget_all() {
    for (uint32_t i = 0; i < count; ++i) release(seen[i].path);
    count = 0;
}

// Once a frame, while anyone is listening.
void step() {
    if (!behavior::changed_wanted()) return forget_all(); // a later listener starts from what actors are doing then
    ++round;
    game::each_actor(look, nullptr);
    // Actors that have left play are forgotten.
    for (uint32_t i = count; i--;) {
        if (seen[i].round == round) continue;
        release(seen[i].path);
        seen[i] = seen[--count];
    }
    // Told after the look, since a handler may change which actors there are.
    for (uint32_t i = 0; i < count; ++i) {
        // A change that cannot be put into words now, for want of memory, is told on a later frame.
        if (!seen[i].changed || !describe(game::from_handle(seen[i].handle))) continue;
        seen[i].changed = false;
        behavior::emit_changed({{seen[i].handle}, scratch, seen[i].path});
        if (i < count) keep(seen[i]); // a handler that stops listening does not empty the list until the next frame
    }
}

TSUCHI_Result get(TSUCHI_Actor actor, char* buffer, uint64_t capacity, uint64_t* out) {
    const uint64_t address = game::from_handle(actor.id);
    if (!address || !game::is_game_actor(address)) return TSUCHI_NOT_FOUND;
    return game::action_path(address, buffer, capacity, out);
}

const behavior::Backend service{get};

} // namespace

unsigned behavior_watched() { return count; }

void behavior_start() {
    behavior::set_backend(&service);
    each_frame(step);
}

} // namespace tsuchi
