#include "holds.h"

#include <string.h>

#include "actor_events.h"
#include "actors.h"
#include "frame.h"
#include "objects.h"
#include "ordinary_lifecycle.h"
#include "parts.h"

namespace tsuchi {
namespace {

// The tables that were started, in that order.
HoldTable* first_table = nullptr;
HoldTable** last_link = &first_table;

} // namespace

void HoldTable::start() {
    if (started_) return;
    started_ = true;
    if (!first_table) {
        each_frame(step_all);
        on_actor_left(left_all);
        ordinary_lifecycle_listener(detached_all);
    }
    *last_link = this;
    last_link = &next_;
}

bool HoldTable::reserve(uint32_t more) {
    if (count_ + more <= capacity_) return true;
    uint32_t room = capacity_ ? capacity_ * 2 : 16;
    while (room < count_ + more) room *= 2;
    auto* grown = static_cast<uint8_t*>(allocate(size_t(room) * stride_));
    if (!grown) return false;
    if (slots_) memcpy(grown, slots_, size_t(count_) * stride_);
    tsuchi::release(slots_);
    slots_ = grown;
    capacity_ = room;
    return true;
}

int32_t HoldTable::index_of(const TSUCHI_Mod* mod, bool anyones, uint64_t object, uint64_t what) const {
    for (uint32_t i = 0; i < count_; ++i) {
        const Hold& hold = hold_at(i);
        if (hold.object == object && hold.what == what && (anyones || hold.mod == mod)) return int32_t(i);
    }
    return -1;
}

int32_t HoldTable::index_of(uint64_t id) const {
    for (uint32_t i = 0; i < count_; ++i)
        if (hold_at(i).id == id) return int32_t(i);
    return -1;
}

void* HoldTable::find(uint64_t object, uint64_t what) const {
    const int32_t at = index_of(nullptr, true, object, what);
    return at < 0 ? nullptr : kept_at(uint32_t(at));
}

void* HoldTable::find(const TSUCHI_Mod* mod, uint64_t object, uint64_t what) const {
    const int32_t at = index_of(mod, false, object, what);
    return at < 0 ? nullptr : kept_at(uint32_t(at));
}

TSUCHI_Mod* HoldTable::holder(uint64_t object, uint64_t what) const {
    const int32_t at = index_of(nullptr, true, object, what);
    return at < 0 ? nullptr : hold_at(uint32_t(at)).mod;
}

bool HoldTable::association_live(const TSUCHI_Mod* mod, uint64_t object, uint64_t what) const {
    const int32_t at = index_of(mod, false, object, what);
    if (at < 0) return false;
    const Hold& hold = hold_at(uint32_t(at));
    return !hold.ordinary_owner || ordinary_lifecycle_restore_part(hold.object, hold.ordinary_owner) != 0;
}

uint32_t HoldTable::count_on(uint64_t object, uint64_t what) const {
    uint32_t found = 0;
    for (uint32_t i = 0; i < count_; ++i) found += hold_at(i).object == object && hold_at(i).what == what;
    return found;
}

bool HoldTable::active_on(uint64_t object, uint64_t what) const {
    for (const Operation* at = active_; at; at = at->next)
        if (at->hold.object == object && at->hold.what == what) return true;
    return false;
}

void* HoldTable::kept_at(uint32_t index) const {
    const uint64_t id = hold_at(index).id;
    for (Operation* at = active_; at; at = at->next)
        if (at->hold.id == id) return at->kept;
    return slots_ + size_t(index) * stride_ + sizeof(Hold);
}

bool HoldTable::operation_allowed(void* context) {
    const auto& call = *static_cast<Operation*>(context);
    return call.table->allowed_at(call, call.table->index_after(call.at, call.hold.id));
}

// The slot a hold was at, when the table still has it there; otherwise where it is now, or -1. A keep or an apply
// may end holds and take new ones, so the slot is checked, and the table searched only when it moved.
int32_t HoldTable::index_after(uint32_t was, uint64_t id) const {
    if (was < count_ && hold_at(was).id == id) return int32_t(was);
    return index_of(id);
}

bool HoldTable::allowed_at(const Operation& call, int32_t index) const {
    if (!on_game_thread()) return false;
    if (call.ended || (call.hold.mod && !mod_usable(call.hold.mod))) return false;
    if (index < 0) return false;
    const Hold& now = call.table->hold_at(uint32_t(index));
    if (now.mod != call.hold.mod || now.object != call.hold.object || now.what != call.hold.what ||
        now.ordinary_owner != call.hold.ordinary_owner) return false;
    if (call.table->on_ != HoldOn::nothing && !game::from_handle(call.hold.object)) return false;
    return !call.hold.ordinary_owner || ordinary_lifecycle_restore_part(call.hold.object, call.hold.ordinary_owner) != 0;
}

game::CallGuard hold_call_guard(const Hold& hold) {
    if (on_game_thread())
        for (HoldTable* table = first_table; table; table = table->next_)
            for (HoldTable::Operation* at = table->active_; at; at = at->next)
                if (&at->hold == &hold) return {HoldTable::operation_allowed, at};
    return {[](void*) { return false; }, nullptr};
}

void hold_apply_checkpoint(const Hold& hold) {
    if (!on_game_thread()) return;
    for (HoldTable* table = first_table; table; table = table->next_)
        for (HoldTable::Operation* at = table->active_; at; at = at->next)
            if (&at->hold == &hold) { at->restorable = true; return; }
}

TSUCHI_Result HoldTable::apply(uint32_t index, const void* wanted, bool fresh) {
    Operation call{this, hold_at(index), {}, active_};
    call.at = index;
    memcpy(call.kept, kept_at(index), stride_ - sizeof(Hold));
    active_ = &call;
    const TSUCHI_Result applied = apply_ ? apply_(call.hold, call.kept, wanted, fresh) : TSUCHI_OK;
    int32_t now = index_after(index, call.hold.id);
    if (!call.ended && !operation_allowed(&call) && now >= 0) {
        end(uint32_t(now), call.hold.mod && !mod_running(call.hold.mod) ? HoldEnd::stopped : HoldEnd::destroyed);
        now = -1;
    }
    if (call.ended || now < 0) {
        // The record ended inside a native callback. The original call has now
        // returned, so restore from stable final state, never a reused slot.
        if (restore_ && (!fresh || applied == TSUCHI_OK || call.restorable))
            restore_(call.hold, call.kept, call.why);
        active_ = call.next;
        return TSUCHI_NOT_READY;
    }
    if (fresh && applied != TSUCHI_OK) {
        remove(uint32_t(now));
        if (call.hold.mod) disown(call.hold.mod, kind_, call.hold.id);
    } else {
        // Native callbacks can move this record or grow/free the table.
        memcpy(slots_ + size_t(now) * stride_ + sizeof(Hold), call.kept, stride_ - sizeof(Hold));
    }
    active_ = call.next;
    return applied;
}

TSUCHI_Result HoldTable::take(TSUCHI_Mod* mod, uint64_t object, uint64_t what, const void* wanted) {
    if (mod && !mod_usable(mod)) return TSUCHI_INVALID;
    if (!started_) start();
    if (active_on(object, what)) return TSUCHI_BUSY;
    uint64_t ordinary_owner = 0;
    if (on_ == HoldOn::watched_actor) {
        const uint64_t actor = game::from_handle(object);
        if (!actor) return TSUCHI_INVALID;
        const TSUCHI_Result watched = ordinary_lifecycle_watch(actor);
        if (watched != TSUCHI_OK) return watched;
    }
    if (on_ == HoldOn::part) {
        const uint64_t part = game::from_handle(object);
        if (!part) return TSUCHI_INVALID;
        const uint64_t owner = game::part_owner(part);
        const TSUCHI_Result qualified = ordinary_lifecycle_watch(owner);
        if (qualified != TSUCHI_OK) return qualified;
        if (!game::is_game_actor(owner)) {
            ordinary_owner = game::to_handle(owner);
            if (!ordinary_owner || !ordinary_lifecycle_restore_part(object, ordinary_owner)) return TSUCHI_NOT_READY;
        }
    }
    const int32_t held = index_of(mod, exclusive_, object, what);
    if (held >= 0) {
        if (hold_at(uint32_t(held)).mod != mod) return TSUCHI_BUSY;
        if (mod && !ownership_update_allowed(mod, kind_, hold_at(uint32_t(held)).id)) return TSUCHI_BUSY;
        // A live component can move to a different owner or owner generation.
        // An old ordinary lease must not be applied to that new association.
        if (hold_at(uint32_t(held)).ordinary_owner &&
            hold_at(uint32_t(held)).ordinary_owner != ordinary_owner) return TSUCHI_NOT_READY;
        return apply(uint32_t(held), wanted, false);
    }
    if (!reserve(1)) return TSUCHI_NO_MEMORY;
    const Hold made{object, what, next_id_, mod, ordinary_owner};
    if (mod) {
        const TSUCHI_Result owned = own(mod, kind_, made.id, on_stop, this);
        if (owned != TSUCHI_OK) return owned;
    }
    ++next_id_;
    const uint32_t at = count_++;
    slot(at) = made;
    memset(kept_at(at), 0, stride_ - sizeof(Hold));
    return apply(at, wanted, true);
}

TSUCHI_Result HoldTable::release(TSUCHI_Mod* mod, uint64_t object, uint64_t what) {
    const int32_t held = index_of(mod, false, object, what);
    if (held < 0) return TSUCHI_NOT_FOUND;
    end(uint32_t(held), HoldEnd::released);
    return TSUCHI_OK;
}

// The last takes its place.
void HoldTable::remove(uint32_t index) {
    --count_;
    if (index != count_) memcpy(slots_ + size_t(index) * stride_, slots_ + size_t(count_) * stride_, stride_);
}

// Takes the hold out of the table, then has the service put things back. In that order, so
// that whatever restore leads to finds a table with no half-ended hold in it.
void HoldTable::end(uint32_t index, HoldEnd why) {
    const Hold ended = hold_at(index);
    for (Operation* at = active_; at; at = at->next) {
        if (at->hold.id != ended.id) continue;
        remove(index);
        if (ended.mod && why != HoldEnd::stopped) disown(ended.mod, kind_, ended.id);
        at->ended = true;
        at->why = why;
        return;
    }
    alignas(8) uint8_t kept[kHoldKeptBytes];
    memcpy(kept, kept_at(index), stride_ - sizeof(Hold));
    remove(index);
    if (ended.mod && why != HoldEnd::stopped) disown(ended.mod, kind_, ended.id); // a mod that stops has given its record up already
    if (restore_) restore_(ended, kept, why);
}

void HoldTable::on_stop(void* table, uint64_t id) {
    auto& self = *static_cast<HoldTable*>(table);
    const int32_t held = self.index_of(id);
    if (held >= 0) self.end(uint32_t(held), HoldEnd::stopped);
}

// Once a frame: a hold on something that is gone ends, and the rest are kept. A step may end
// holds and take new ones, so each turn looks the table up afresh.
void HoldTable::step() {
    for (uint32_t i = count_; i--;) {
        if (i >= count_) continue;
        const uint64_t id = hold_at(i).id;
        if ((on_ != HoldOn::nothing && !game::from_handle(hold_at(i).object)) ||
            (hold_at(i).ordinary_owner &&
             !ordinary_lifecycle_restore_part(hold_at(i).object, hold_at(i).ordinary_owner))) {
            end(i, HoldEnd::destroyed);
            continue;
        }
        if (!keep_) continue;
        Operation call{this, hold_at(i), {}, active_};
        call.at = i;
        memcpy(call.kept, kept_at(i), stride_ - sizeof(Hold));
        active_ = &call;
        const bool kept = keep_(call.hold, call.kept);
        int32_t now = index_after(i, id);
        if (!call.ended && now >= 0 && (!kept || !allowed_at(call, now))) {
            end(uint32_t(now), kept ? HoldEnd::destroyed : HoldEnd::lost);
            now = -1;
        }
        if (call.ended || now < 0) {
            if (restore_) restore_(call.hold, call.kept, call.why);
        } else {
            memcpy(slots_ + size_t(now) * stride_ + sizeof(Hold), call.kept, stride_ - sizeof(Hold));
        }
        active_ = call.next;
    }
}

// An actor leaves play: the holds on it, and on its parts, end.
void HoldTable::left(uint64_t actor, uint64_t handle, HoldEnd why) {
    for (uint32_t i = count_; i--;) {
        if (i >= count_) continue;
        bool theirs = hold_at(i).object == handle;
        if (on_ == HoldOn::part) {
            const uint64_t part = game::from_handle(hold_at(i).object);
            theirs = part && game::part_owner(part) == actor &&
                (!hold_at(i).ordinary_owner || hold_at(i).ordinary_owner == handle);
        }
        if (theirs) end(i, why);
    }
}

namespace {
CostTotal pass_cost{};
} // namespace
CostTotal hold_pass_cost() { return pass_cost; }

void HoldTable::step_all() {
    const uint64_t began = cost_now();
    for (HoldTable* table = first_table; table; table = table->next_) table->step();
    pass_cost.add(began);
}

uint64_t hold_part_to_restore(const Hold& hold, HoldEnd why) {
    if (hold.ordinary_owner && why == HoldEnd::destroying) return 0;
    return ordinary_lifecycle_restore_part(hold.object, hold.ordinary_owner);
}

void HoldTable::detached_all(uint64_t actor, uint64_t handle, bool destroying) {
    const HoldEnd why = destroying ? HoldEnd::destroying : HoldEnd::detached;
    for (HoldTable* table = first_table; table; table = table->next_)
        if (table->on_ == HoldOn::part || table->on_ == HoldOn::watched_actor) table->left(actor, handle, why);
}

void HoldTable::left_all(uint64_t actor, uint64_t handle, ActorLeft how) {
    const HoldEnd why = how == ActorLeft::put_away ? HoldEnd::pooled : HoldEnd::destroyed;
    for (HoldTable* table = first_table; table; table = table->next_)
        if (table->on_ == HoldOn::part || table->on_ == HoldOn::actor || table->on_ == HoldOn::watched_actor)
            table->left(actor, handle, why);
}

} // namespace tsuchi
