#include "instance_cull.h"

#include <float.h>
#include <string.h>

#include "game.h"
#include "holds.h"
#include "objects.h"
#include "ordinary_lifecycle.h"
#include "parts.h"

namespace tsuchi {
namespace {
namespace t = game::table;

bool native_instance_type(uint64_t type) {
    const uint64_t meta = game::class_of(type), core = game::outer_of(meta);
    const uint64_t package_type = game::class_of(core), engine = game::outer_of(type);
    return game::live(meta) && game::class_of(meta) == meta && game::name_is(game::name_of(meta), "Class") &&
        game::live(core) && !game::outer_of(core) && game::name_is(game::name_of(core), "/Script/CoreUObject") &&
        game::live(package_type) && game::class_of(package_type) == meta && game::outer_of(package_type) == core &&
        game::name_is(game::name_of(package_type), "Package") &&
        game::live(engine) && game::class_of(engine) == package_type && !game::outer_of(engine) &&
        game::name_is(game::name_of(engine), "/Script/Engine");
}
bool instance_component(uint64_t part) {
    if (!game::live(part)) return false;
    // A short-name lookup can find a Blueprint impostor first. Only the
    // canonical native Engine ancestor authorizes the layout and fixed setter.
    uint64_t type = game::class_of(part);
    for (uint32_t depth = 0; type && depth < 64; ++depth) {
        if (!game::live(type)) return false;
        if (game::name_is(game::name_of(type), "InstancedStaticMeshComponent") && native_instance_type(type)) return true;
        const uint64_t parent = game::read_or<uint64_t>(type + t::struct_type::super);
        if (parent == type) return false;
        type = parent;
    }
    return false;
}

struct Pair { int32_t start = 0, end = 0; };
struct Kept {
    Pair baseline, applied;
    uint64_t owner = 0;
    float scale = 1;
    bool written = false, confirmed = false;
};
bool same(const Pair& a, const Pair& b) { return a.start == b.start && a.end == b.end; }
bool positive_finite(float scale) { return scale > 0 && scale <= FLT_MAX; }
bool read_pair(uint64_t part, Pair& pair) {
    Pair value;
    if (!part || !game::read(part + t::instanced_static_mesh_component::start_cull_distance, value.start) ||
        !game::read(part + t::instanced_static_mesh_component::end_cull_distance, value.end)) return false;
    pair = value;
    return true;
}
bool scaled_distance(int32_t original, float scale, int32_t& out) {
    if (original <= 0) { out = original; return true; }
    // Exact binary32 mantissa * int32 fits in 55 bits. Binary64 can round a
    // just-below-half result to a tie near INT32_MAX, so round as integers.
    uint32_t bits = 0;
    memcpy(&bits, &scale, sizeof bits);
    const uint32_t exponent = (bits >> 23) & 255;
    const uint32_t mantissa = (bits & 0x7FFFFF) | (exponent ? 0x800000 : 0);
    const uint64_t product = uint64_t(uint32_t(original)) * mantissa;
    const int shift = exponent ? int(exponent) - 150 : -149;
    uint64_t rounded = 0;
    if (shift >= 0) {
        if (shift >= 31 || product > (uint64_t(2147483647) >> shift)) return false;
        rounded = product << shift;
    } else {
        const unsigned right = unsigned(-shift);
        if (right >= 64) return false; // a 55-bit numerator rounds to zero
        const uint64_t half = uint64_t(1) << (right - 1);
        rounded = (product >> right) + ((product & ((uint64_t(1) << right) - 1)) >= half);
    }
    if (!rounded || rounded > 2147483647) return false;
    out = int32_t(rounded);
    return true;
}
bool scaled_pair(const Pair& baseline, float scale, Pair& out) {
    return positive_finite(scale) && scaled_distance(baseline.start, scale, out.start) &&
        scaled_distance(baseline.end, scale, out.end) &&
        (baseline.start >= baseline.end || out.start < out.end);
}

struct TargetGuard {
    game::CallGuard lifetime;
    uint64_t part, owner;
    static bool allowed(void* context) {
        const auto& check = *static_cast<TargetGuard*>(context);
        return check.owner && check.lifetime.permits() &&
            ordinary_lifecycle_restore_part(check.part, check.owner) != 0;
    }
    game::CallGuard guard() { return {allowed, this}; }
};
void cancel(const Hold& hold);
bool write_pair(uint64_t part, const Pair& pair, game::CallGuard guard) {
    if (!guard.permits() || !t::bound(t::fn::set_instance_cull_distances)) return false;
    // Exact native void setter: two int32 stores, then render-state notification.
    // No resolver/allocation callback occurs between guard and these stores.
    t::fn::set_instance_cull_distances(reinterpret_cast<void*>(part), pair.start, pair.end);
    return guard.permits();
}
void confirm(uint64_t part, Kept& entry) {
    Pair observed;
    entry.confirmed = read_pair(part, observed) && same(observed, entry.applied);
}
TSUCHI_Result apply(const Hold& hold, Kept& entry, const float& scale, bool fresh) {
    const auto lifetime = hold_call_guard(hold);
    if (!lifetime.permits() || !t::bound(t::fn::set_instance_cull_distances)) return TSUCHI_NOT_READY;
    const uint64_t part = game::from_handle(hold.object);
    const uint64_t owner = fresh ? game::to_handle(game::part_owner(part)) : entry.owner;
    // Gameplay part holds omit ordinary_owner. Capture their association too,
    // including owner/component destroying flags, without changing Holds itself.
    TargetGuard target{lifetime, hold.object, owner};
    const auto guard = target.guard();
    if (!guard.permits()) { cancel(hold); return TSUCHI_NOT_READY; }
    Pair before;
    if (!read_pair(part, before)) return TSUCHI_NOT_READY;
    const Kept previous = entry;
    Kept next = previous;
    next.owner = owner;
    if (fresh || !previous.written || !same(before, previous.applied)) next.baseline = before;
    if (!scaled_pair(next.baseline, scale, next.applied)) return TSUCHI_INVALID;
    next.scale = scale; next.written = true; next.confirmed = false;
    entry = next;
    if (same(before, next.applied)) { entry.confirmed = true; return TSUCHI_OK; }
    hold_apply_checkpoint(hold);
    const bool accepted = write_pair(part, next.applied, guard);
    if (!guard.permits()) { cancel(hold); return TSUCHI_NOT_READY; }
    if (!accepted) { entry = previous; return TSUCHI_NOT_READY; } // pre-mutation unavailability
    // Completion accepts ownership, not a synchronous readback/render promise.
    // Unreadable state retains our witness; a later different complete pair is
    // adopted by Keep, never mistaken for our own baseline or blindly rolled back.
    confirm(part, entry);
    return TSUCHI_OK;
}
bool keep(const Hold& hold, Kept& entry) {
    TargetGuard target{hold_call_guard(hold), hold.object, entry.owner};
    const auto guard = target.guard();
    if (!guard.permits()) return false;
    const uint64_t part = game::from_handle(hold.object);
    Pair now;
    if (!read_pair(part, now)) { entry.confirmed = false; return true; }
    entry.confirmed = same(now, entry.applied);
    if (entry.confirmed) return true;
    entry.baseline = now;
    Pair desired;
    if (!scaled_pair(now, entry.scale, desired)) return false;
    const Pair previous_applied = entry.applied;
    entry.applied = desired; entry.written = true;
    if (same(now, desired)) { entry.confirmed = true; return true; }
    hold_apply_checkpoint(hold);
    const bool accepted = write_pair(part, desired, guard);
    if (!guard.permits()) { cancel(hold); return true; }
    if (!accepted) { entry.applied = previous_applied; return true; }
    confirm(part, entry);
    return true;
}
bool held_again(uint64_t object);
void restore(const Hold& hold, Kept& entry, HoldEnd why) {
    if (!entry.written || why == HoldEnd::destroyed || held_again(hold.object)) return;
    HoldRestore check{hold, why};
    TargetGuard target{check.guard(), hold.object, entry.owner};
    const auto guard = target.guard();
    if (!guard.permits()) return;
    const uint64_t part = game::from_handle(hold.object);
    Pair now;
    if (!read_pair(part, now) || !same(now, entry.applied) || same(now, entry.baseline)) return;
    write_pair(part, entry.baseline, guard);
}
Holds<Kept, float, apply, keep, restore> holds{"component.instance_cull_scale", HoldOn::part, true};
bool held_again(uint64_t object) { return holds.find(object, 0) != nullptr; }
void cancel(const Hold& hold) { holds.release(hold.mod, hold.object, hold.what); }
} // namespace

void instance_cull_start() { holds.start(); }
TSUCHI_Result hold_instance_cull_scale(TSUCHI_Mod* mod, uint64_t part, float scale) {
    if (!positive_finite(scale)) return TSUCHI_INVALID;
    if (!instance_component(part)) return TSUCHI_UNSUPPORTED;
    if (!t::bound(t::fn::set_instance_cull_distances)) return TSUCHI_NOT_READY;
    return holds.take(mod, game::to_handle(part), 0, scale);
}
TSUCHI_Result release_instance_cull_scale(TSUCHI_Mod* mod, uint64_t part) {
    return holds.release(mod, game::to_handle(part), 0);
}
} // namespace tsuchi
