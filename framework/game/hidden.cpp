#include "hidden.h"

#include "actors.h"
#include "holds.h"
#include "objects.h"
#include "parts.h"

namespace tsuchi {
namespace {

// Two kinds of hold. A mod's hold is on the part it asked for, and covers that part and, when
// asked, the parts below it. Each covered part is held hidden once, by the framework itself,
// however many of the mods' holds cover it.

// A part that is hidden now: how many of the mods' holds cover it, and whether it was drawn before.
struct Part {
    uint32_t covers;
    bool was_visible;
};

// Hides the part with the first cover, and counts each one. TSUCHI_NOT_READY when the engine would not hide it.
TSUCHI_Result hide(const Hold& hold, Part& part, const HoldNothing&, bool fresh) {
    if (fresh) {
        const uint64_t component = game::from_handle(hold.object);
        bool visible = false;
        if (!component || !game::part_visible(component, visible)) return TSUCHI_NOT_READY;
        part.was_visible = visible;
        if (visible) {
            hold_apply_checkpoint(hold);
            if (!game::set_part_visible(component, false)) return TSUCHI_NOT_READY;
        }
    }
    ++part.covers;
    return TSUCHI_OK;
}

// Once a frame: a part the game has shown is hidden again.
bool hide_again(const Hold& hold, Part&) {
    const uint64_t component = game::from_handle(hold.object);
    bool visible = false;
    if (game::part_visible(component, visible) && visible) game::set_part_visible(component, false);
    return true;
}

// The part goes back to what it was. One that is being destroyed is left alone.
void show(const Hold& hold, Part& part, HoldEnd why) {
    if (why == HoldEnd::destroyed || !part.was_visible) return;
    HoldRestore check{hold, why};
    if (const uint64_t component = hold_part_to_restore(hold, why)) game::restore_part_visible(component, true, check.guard());
}

Holds<Part, HoldNothing, hide, hide_again, show> parts{"component.hidden_part", HoldOn::part, true};

// Puts one more cover on a part. False when the engine would not hide it.
bool cover(uint64_t handle) { return parts.take(nullptr, handle, 0, HoldNothing{}) == TSUCHI_OK; }

// Takes one cover off a part. With the last one it goes back to what it was.
void uncover(uint64_t handle) {
    Part* part = parts.find(handle, 0);
    if (part && !--part->covers) parts.release(nullptr, handle, 0);
}

// One mod's hold: every part it covers, as handles, and whether the parts below the one asked for are among them.
struct Covered {
    uint64_t* parts;
    uint32_t count;
    bool children;
};

// Covers the parts listed and keeps the list. A hold that is changed covers the new parts before
// it uncovers the old, so a part in both stays hidden throughout. When one part cannot be hidden,
// the ones before it are uncovered again and the list stays the caller's.
struct CoverRequest {
    Covered covers;
    bool* consumed;
};
TSUCHI_Result cover_all(const Hold& hold, Covered& entry, const CoverRequest& request, bool fresh) {
    const Covered wanted = request.covers;
    const game::CallGuard guard = hold_call_guard(hold);
    for (uint32_t i = 0; i < wanted.count; ++i) {
        if (guard.permits() && cover(wanted.parts[i])) {
            if (guard.permits()) continue;
            for (uint32_t before = 0; before <= i; ++before) uncover(wanted.parts[before]);
            return TSUCHI_NOT_READY;
        }
        for (uint32_t before = 0; before < i; ++before) uncover(wanted.parts[before]);
        return TSUCHI_NOT_READY;
    }
    const Covered previous = entry;
    entry = wanted;
    *request.consumed = true;
    hold_apply_checkpoint(hold);
    if (!fresh) {
        for (uint32_t i = 0; i < previous.count; ++i) uncover(previous.parts[i]);
        release(previous.parts);
    }
    return TSUCHI_OK;
}

// Uncovers what the hold covered, whatever ended it. When its actor is leaving play, the parts
// have been dealt with already as that says, and there is nothing left here to uncover.
void uncover_all(const Hold&, Covered& entry, HoldEnd) {
    for (uint32_t i = 0; i < entry.count; ++i) uncover(entry.parts[i]);
    release(entry.parts);
}

Holds<Covered, CoverRequest, cover_all, nullptr, uncover_all> holds{"component.hidden", HoldOn::part, false};

// The parts a hold would cover: counted, then listed.
struct Gather {
    uint64_t* into;
    uint32_t room, found;
};
void gather(void* context, uint64_t part) {
    Gather& g = *static_cast<Gather*>(context);
    if (g.found < g.room) g.into[g.found] = game::to_handle(part);
    ++g.found;
}
bool walk(uint64_t part, bool children, Gather& g) {
    if (children) return game::each_attached(part, gather, &g);
    gather(&g, part);
    return true;
}

} // namespace

// The parts first: when an actor leaves play its parts are dealt with before the mods' holds on them.
void hidden_start() {
    parts.start();
    holds.start();
}

TSUCHI_Result hide_part(TSUCHI_Mod* mod, uint64_t part, bool children) {
    const uint64_t root = game::to_handle(part);
    if (!root) return TSUCHI_NOT_FOUND;
    if (!game::is_scene_part(part)) return TSUCHI_UNSUPPORTED;
    const Covered* mine = holds.find(mod, root, 0);
    if (mine && mine->children == children) return holds.association_live(mod, root, 0) ? TSUCHI_OK : TSUCHI_NOT_READY;
    Gather counted{nullptr, 0, 0};
    if (!walk(part, children, counted)) return TSUCHI_UNSUPPORTED;
    if (!parts.reserve(counted.found) || !holds.reserve(1)) return TSUCHI_NO_MEMORY; // room is made before anything is changed
    Gather listed{static_cast<uint64_t*>(allocate(sizeof(uint64_t) * counted.found)), counted.found, 0};
    if (!listed.into) return TSUCHI_NO_MEMORY;
    TSUCHI_Result result = TSUCHI_UNSUPPORTED;
    bool consumed = false;
    if (walk(part, children, listed) && listed.found == counted.found)
        result = holds.take(mod, root, 0, CoverRequest{Covered{listed.into, listed.found, children}, &consumed});
    if (!consumed) release(listed.into);
    return result;
}

TSUCHI_Result show_part(TSUCHI_Mod* mod, uint64_t part) {
    const uint64_t root = game::to_handle(part);
    return root ? holds.release(mod, root, 0) : TSUCHI_NOT_FOUND;
}

bool part_hidden(uint64_t part) { return parts.find(game::to_handle(part), 0) != nullptr; }

HiddenCounts hidden_counts() { return HiddenCounts{parts.count(), holds.count()}; }

} // namespace tsuchi
