#include "materials.h"

#include "holds.h"
#include "objects.h"
#include "object_retention.h"
#include "parts.h"
#include "shader_materials.h"

namespace tsuchi {
namespace {

// What is kept for a held slot. Handles, all three.
struct Held {
    uint64_t original; // what to go back to, or 0 for the mesh's own
    uint64_t ours;
    uint64_t shown; // what is in the slot for it: the material, or what a material with shaders of a mod's own is drawn as
    bool occupied;  // a mod owns this framework fallback's slot; changed only when holds change
    uint32_t generation; // shader_generation() when shown was last resolved
};

bool occupied(uint64_t object, uint64_t slot);
uint64_t underlying_material(uint64_t object, uint64_t slot, uint64_t shown);
void mark_occupied(uint64_t object, uint64_t slot, bool occupied);

bool pin(const Held& entry) {
    if (!retain_game_object(entry.original)) return false;
    if (!retain_game_object(entry.ours)) { release_game_object(entry.original); return false; }
    if (!retain_game_object(entry.shown)) {
        release_game_object(entry.ours); release_game_object(entry.original); return false;
    }
    return true;
}
void unpin(const Held& entry) {
    release_game_object(entry.original); release_game_object(entry.ours); release_game_object(entry.shown);
}
// The resolver may run native creation/copy callbacks. Keep the candidate roots
// separate from the operation's current state until the exact hold permits it.
TSUCHI_Result resolve(Held& next, game::CallGuard guard) {
    if (!pin(next)) return TSUCHI_NO_MEMORY;
    uint64_t shown = game::from_handle(next.ours);
    if (!drawn_as_restoring(shown, guard, shown) || !guard.permits()) { unpin(next); return TSUCHI_NOT_READY; }
    const uint64_t handle = game::to_handle(shown);
    if (!retain_game_object(handle)) { unpin(next); return TSUCHI_NO_MEMORY; }
    release_game_object(next.shown);
    next.shown = handle;
    return TSUCHI_OK;
}

TSUCHI_Result apply(const Hold &hold, Held &entry, const uint64_t &material, bool fresh) {
    const game::CallGuard guard = hold_call_guard(hold);
    if (!guard.permits()) return TSUCHI_NOT_READY;
    const uint64_t part = game::from_handle(hold.object);
    const uint32_t slot = uint32_t(hold.what);
    uint64_t original = fresh ? game::to_handle(game::slot_override(part, slot)) : entry.original;
    if (!guard.permits()) return TSUCHI_NOT_READY;
    if (fresh && hold.mod) original = game::to_handle(underlying_material(hold.object, hold.what, game::from_handle(original)));
    const bool blocked = !hold.mod && (fresh ? occupied(hold.object, hold.what) : entry.occupied);
    Held next{original, game::to_handle(material), game::to_handle(material), blocked, shader_generation()};
    const TSUCHI_Result resolved = resolve(next, guard);
    if (resolved != TSUCHI_OK) return resolved;
    const Held previous = entry;
    entry = next;
    if (blocked) { unpin(previous); return TSUCHI_OK; }
    // Publish into the stable stack operation before the setter. An ended
    // operation restores this candidate after the native continuation returns.
    hold_apply_checkpoint(hold);
    const bool set = game::restore_slot_material(part, slot, game::from_handle(next.shown), guard);
    if (!guard.permits()) { unpin(previous); return TSUCHI_NOT_READY; }
    if (!set) { entry = previous; unpin(next); return TSUCHI_NOT_READY; }
    unpin(previous);
    if (hold.mod) mark_occupied(hold.object, hold.what, true);
    return TSUCHI_OK;
}

MaterialHoldCounts hold_counts{};

bool keep(const Hold &hold, Held &entry) {
    if (!hold.mod && entry.occupied) return true;
    const game::CallGuard guard = hold_call_guard(hold);
    if (!guard.permits()) return true;
    const uint64_t part = game::from_handle(hold.object);
    const uint32_t slot = uint32_t(hold.what);
    Held previous = entry;
    const uint64_t now = game::slot_override(part, slot);
    if (!guard.permits()) return true;
    ++hold_counts.kept;
    const uint32_t generation = shader_generation();
    if (now != game::from_handle(previous.shown)) {
        ++hold_counts.changed;
    } else if (now && previous.generation == generation && game::from_handle(previous.ours)) {
        ++hold_counts.unchanged; // the slot shows what it did, and nothing it could be drawn as has changed
        return true;
    }
    uint64_t ours = game::from_handle(previous.ours);
    if (!ours) return true;
    // Capture the game's latest assignment before any resolver callback can
    // stop the operation. A known stand-in belongs to its original source.
    if (now != game::from_handle(previous.shown) && (hold.mod || now != ours)) {
        const uint64_t original = game::to_handle(shader_source(now));
        if (!retain_game_object(original)) return true;
        release_game_object(entry.original);
        entry.original = original;
        previous = entry;
    }
    Held next = previous;
    next.generation = generation;
    uint64_t adopted_shown = 0;
    bool adopted = false;
    if (!hold.mod && now && now != ours && now != game::from_handle(previous.shown)) {
        const uint64_t source = shader_source(now);
        uint64_t candidate = source;
        if (!drawn_as_restoring(source, guard, candidate) || !guard.permits()) return true;
        if (candidate != source && track_shader_slot(source, part, slot)) {
            ours = source;
            next.ours = next.original = game::to_handle(source);
            adopted = true;
            adopted_shown = candidate;
        }
    }
    next.shown = adopted ? game::to_handle(adopted_shown) : next.ours;
    if (adopted ? !pin(next) : resolve(next, guard) != TSUCHI_OK) {
        if (adopted && guard.permits()) track_shader_slot(game::from_handle(previous.ours), part, slot);
        return true;
    }
    const uint64_t shown = game::from_handle(next.shown);
    entry = next;
    bool set = true;
    if (now != shown) ++hold_counts.set, set = game::restore_slot_material(part, slot, shown, guard);
    // Both the old roots and candidate survived the callback. Only the stable
    // operation owns the candidate now, including an operation stopped there.
    unpin(previous);
    return !guard.permits() || set;
}

void restore(const Hold &hold, Held &entry, HoldEnd why) {
    const Held retained = entry;
    struct Roots { const Held& entry; ~Roots() { unpin(entry); } } roots{retained};
    if (hold.mod) mark_occupied(hold.object, hold.what, false);
    if (why == HoldEnd::destroyed || (!hold.mod && retained.occupied)) return;
    HoldRestore check{hold, why};
    if (!hold_part_to_restore(hold, why)) return;
    const uint64_t original = game::from_handle(retained.original);
    uint64_t material = original;
    if (hold.mod && !drawn_as_restoring(original, check.guard(), material)) return;
    // Resolving the original can produce a different MID from any held root.
    // The provider may drop its cache inside the setter's native continuation.
    const uint64_t resolved = game::to_handle(material);
    if (!retain_game_object(resolved)) return;
    struct ResolvedRoot { uint64_t handle; ~ResolvedRoot() { release_game_object(handle); } } resolved_root{resolved};
    if (game::from_handle(resolved) != material) return;
    if (const uint64_t part = hold_part_to_restore(hold, why))
        game::restore_slot_material(part, uint32_t(hold.what), material, check.guard());
}

// A hold is on a mesh, and the thing of the mesh that is held is the slot of that number.
Holds<Held, uint64_t, apply, keep, restore> held{"component.material", HoldOn::part, false};
Holds<Held, uint64_t, apply, keep, restore> brushes{"shaders.image", HoldOn::object, false};
auto& holds_for(uint64_t object) { return game::is_material_image(object) ? brushes : held; }

void mark_occupied(uint64_t object, uint64_t slot, bool occupied) {
    if (Held *fallback = holds_for(game::from_handle(object)).find(static_cast<TSUCHI_Mod *>(nullptr), object, slot)) fallback->occupied = occupied;
}

uint64_t underlying_material(uint64_t object, uint64_t slot, uint64_t shown) {
    const Held *fallback = holds_for(game::from_handle(object)).find(static_cast<TSUCHI_Mod *>(nullptr), object, slot);
    return fallback && game::from_handle(fallback->shown) == shown ? game::from_handle(fallback->ours) : shown;
}
bool occupied(uint64_t object, uint64_t slot) {
    for (uint32_t i = 0; i < held.count(); ++i) {
        const Hold &h = held.hold_at(i);
        if (h.object == object && h.what == slot && h.mod) return true;
    }
    return false;
}
} // namespace

void materials_start() { held.start(); brushes.start(); }

TSUCHI_Result hold_material(TSUCHI_Mod *mod, uint64_t part, uint32_t slot, uint64_t material) {
    if (!(mod ? game::is_mesh_part(part) : game::is_shader_target(part))) return TSUCHI_UNSUPPORTED;
    if (!game::is_material(material)) return TSUCHI_INVALID;
    uint32_t slots = 0;
    if (!game::material_slots(part, slots)) return TSUCHI_NOT_READY;
    if (slot >= slots) return TSUCHI_INVALID;
    const uint64_t handle = game::to_handle(part);
    for (uint32_t i = 0; mod && i < held.count(); ++i) {
        const Hold &h = held.hold_at(i);
        if (h.object == handle && h.what == slot && h.mod && h.mod != mod) return TSUCHI_BUSY;
    }
    return holds_for(part).take(mod, handle, slot, material);
}

TSUCHI_Result restore_material(TSUCHI_Mod *mod, uint64_t part, uint32_t slot) { return holds_for(part).release(mod, game::to_handle(part), slot); }

uint64_t shader_slot_source(uint64_t part, uint32_t slot) {
    const Held* entry = holds_for(part).find(static_cast<TSUCHI_Mod*>(nullptr), game::to_handle(part), slot);
    return entry ? entry->ours : 0;
}
uint32_t material_holds() { return held.count() + brushes.count(); }
MaterialHoldCounts material_hold_counts() { return hold_counts; }

} // namespace tsuchi
