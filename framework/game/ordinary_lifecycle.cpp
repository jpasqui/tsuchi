#include "ordinary_lifecycle.h"
#include "actors.h"
#include "hooks.h"
#include "objects.h"
#include "parts.h"
#include "registry.h"

namespace tsuchi {
namespace {
namespace t = game::table;
using EndPlay = decltype(t::ordinary_actor_vtable::end_play)::Function;
using Unregister = decltype(t::ordinary_actor_vtable::unregister)::Function;
using Destroyed = decltype(t::ordinary_actor_vtable::destroyed)::Function;
using RemoveLevel = decltype(t::code_site::world_remove_level)::Function;
struct OwnerTable {
    uint64_t table, end_id, unregister_id, destroyed_id;
    void* end;
    void* unregister;
    void* destroyed;
    OwnerTable* next;
    bool ready;
};
OwnerTable* tables = nullptr;
TSUCHI_Mod* framework = nullptr;
void* next_remove = nullptr;
uint64_t remove_id = 0;
// The hold tables and the camera each listen. A later listener must not replace an earlier one.
using Listener = void (*)(uint64_t, uint64_t, bool);
Listener listeners[4] = {};
uint32_t listener_count = 0;
bool locked = false;
struct Lock {
    Lock() { while (__atomic_test_and_set(&locked, __ATOMIC_ACQUIRE)) {} }
    ~Lock() { __atomic_clear(&locked, __ATOMIC_RELEASE); }
};
struct Removal {
    uint64_t world, level, world_handle, level_handle, thread, invocation;
    Removal* next;
};
Removal* removals = nullptr;
struct Cleanup {
    uint64_t handle, thread;
    Cleanup* next;
    bool destroying;
};
Cleanup* cleanups = nullptr;
OwnerTable snapshot(uint64_t actor) {
    const uint64_t table = game::read_or<uint64_t>(actor);
    Lock lock;
    for (OwnerTable* at = tables; at; at = at->next) if (at->table == table)
        return OwnerTable{at->table, __atomic_load_n(&at->end_id, __ATOMIC_ACQUIRE),
            __atomic_load_n(&at->unregister_id, __ATOMIC_ACQUIRE), __atomic_load_n(&at->destroyed_id, __ATOMIC_ACQUIRE),
            __atomic_load_n(&at->end, __ATOMIC_ACQUIRE), __atomic_load_n(&at->unregister, __ATOMIC_ACQUIRE),
            __atomic_load_n(&at->destroyed, __ATOMIC_ACQUIRE), at->next, at->ready};
    return {};
}
bool cleaning(uint64_t handle, uint64_t thread) {
    for (const Cleanup* at = cleanups; at; at = at->next)
        if (at->handle == handle && at->thread == thread) return true;
    return false;
}
class Teardown {
    Cleanup scope_{};
    uint64_t actor_ = 0;
    bool active_ = false;
  public:
    explicit Teardown(uint64_t actor, bool admitted = true, bool destroying = false) {
        if (!admitted || !on_game_thread() || !game::live(actor) || game::is_game_actor(actor)) return;
        scope_ = Cleanup{game::to_handle(actor), current_thread_token(), nullptr, destroying};
        if (!scope_.handle) return;
        Lock lock;
        for (Cleanup* at = cleanups; at; at = at->next) {
            if (at->handle != scope_.handle || at->thread != scope_.thread) continue;
            // An accepted Destroyed callback during level restoration ends the
            // remaining bookkeeping now, before forwarding native destruction.
            if (destroying && !at->destroying) {
                at->destroying = true;
                actor_ = actor;
            }
            return;
        }
        scope_.next = cleanups;
        cleanups = &scope_;
        actor_ = actor;
        active_ = true;
    }
    void restore(bool destroying) {
        if (!actor_) return;
        for (uint32_t i = 0; i < listener_count; ++i) listeners[i](actor_, scope_.handle, destroying);
    }
    ~Teardown() {
        if (!active_) return;
        Lock lock;
        Cleanup** link = &cleanups;
        while (*link && *link != &scope_) link = &(*link)->next;
        if (*link) *link = scope_.next;
    }
};
bool removal_admitted(uint64_t actor, uint64_t hook) {
    const HookInvocation call = hook_invocation(hook);
    if (!call.token || call.caller != game::main_base() + t::constant::removal_unregister_return) return false;
    const uint64_t level = game::read_or<uint64_t>(actor + t::object::outer);
    Lock lock;
    for (const Removal* at = removals; at; at = at->next)
        if (at->thread == call.thread && at->level == level && at->invocation &&
            game::from_handle(at->level_handle) == at->level && game::from_handle(at->world_handle) == at->world)
            return true;
    return false;
}
void on_end(uint64_t actor, uint8_t reason) {
    const OwnerTable at = snapshot(actor);
    // EndPlay is already admitted teardown. Incremental unregister is caught
    // earlier below, and has consumed the leases before this second callback.
    const uint8_t play = game::read_or<uint8_t>(actor + t::engine_actor::play_state);
    // LevelTransition and other EndPlay reasons can leave live components.
    // Only native accepted destruction makes the whole owner nonrestorable.
    // The part guard still rejects each destroying or stale component.
    const bool destroying = (play & t::constant::actor_destroying_mask) != 0;
    Teardown scope(actor, (play & t::constant::actor_play_state_mask) == t::constant::actor_play_state_begun, destroying);
    scope.restore(destroying);
    if (at.end) reinterpret_cast<EndPlay>(at.end)(actor, reason);
}
void on_destroyed(uint64_t actor) {
    const OwnerTable at = snapshot(actor);
    // Native DestroyActor sets BeingDestroyed only after its veto/deferral
    // paths. A direct call of this slot is not that admission by itself.
    const uint8_t play = game::read_or<uint8_t>(actor + t::engine_actor::play_state);
    Teardown scope(actor, (play & t::constant::actor_destroying_mask) != 0, true);
    scope.restore(true);
    if (at.destroyed) reinterpret_cast<Destroyed>(at.destroyed)(actor);
}
void on_unregister(uint64_t actor, bool only_tick) {
    const OwnerTable at = snapshot(actor);
    Teardown scope(actor, !only_tick && removal_admitted(actor, at.unregister_id));
    scope.restore(false);
    if (at.unregister) reinterpret_cast<Unregister>(at.unregister)(actor, only_tick);
}
void on_remove(uint64_t world, uint64_t level, uint32_t flags) {
    const HookInvocation call = hook_invocation(__atomic_load_n(&remove_id, __ATOMIC_ACQUIRE));
    Removal scope{world, level, game::to_handle(world), game::to_handle(level), call.thread, call.token, nullptr};
    {
        Lock lock;
        scope.next = removals;
        removals = &scope;
    }
    reinterpret_cast<RemoveLevel>(__atomic_load_n(&next_remove, __ATOMIC_ACQUIRE))(world, level, flags);
    {
        Lock lock;
        Removal** link = &removals;
        while (*link && *link != &scope) link = &(*link)->next;
        if (*link) *link = scope.next;
    }
}
} // namespace
uint64_t ordinary_lifecycle_restore_part(uint64_t part_handle, uint64_t owner_handle) {
    const uint64_t part = game::from_handle(part_handle);
    if (!part || !owner_handle) return part;
    const uint64_t actor = game::from_handle(owner_handle);
    if (!actor || game::part_owner(part) != actor) return 0;
    uint8_t actor_state = 0;
    uint32_t part_state = 0;
    if (!game::read(actor + t::engine_actor::play_state, actor_state) ||
        !game::read(part + t::actor_component::lifecycle_flags, part_state) ||
        (actor_state & t::constant::actor_destroying_mask) ||
        (part_state & t::constant::component_destroying_mask)) return 0;
    return part;
}
void ordinary_lifecycle_listener(void (*listener)(uint64_t, uint64_t, bool)) {
    if (!listener) return;
    for (uint32_t i = 0; i < listener_count; ++i) if (listeners[i] == listener) return;
    if (listener_count < sizeof(listeners) / sizeof(listeners[0])) listeners[listener_count++] = listener;
}
void ordinary_lifecycle_start(TSUCHI_Mod* self) {
    if (framework) return;
    if (hook_code_site(self, t::code_site::world_remove_level.number, reinterpret_cast<void*>(on_remove), &remove_id, &next_remove) == TSUCHI_OK) framework = self;
}
TSUCHI_Result ordinary_lifecycle_watch(uint64_t actor) {
    if (!actor || !game::live(actor)) return TSUCHI_INVALID;
    if (game::is_game_actor(actor)) return TSUCHI_OK;
    const uint64_t handle = game::to_handle(actor), thread = current_thread_token();
    {
        Lock lock;
        if (cleaning(handle, thread)) return TSUCHI_BUSY;
    }
    if (!framework || !listener_count) return TSUCHI_NOT_READY;
    const uint64_t table = game::read_or<uint64_t>(actor);
    const OwnerTable existing = snapshot(actor);

    const uint64_t main = game::main_base();
    const uint64_t end_entry = table + t::ordinary_actor_vtable::end_play.offset;
    const uint64_t unregister_entry = table + t::ordinary_actor_vtable::unregister.offset;
    const uint64_t destroyed_entry = table + t::ordinary_actor_vtable::destroyed.offset;
    if (existing.table) {
        if (!existing.ready) return TSUCHI_UNSUPPORTED;
        return hook_entry_owned(end_entry) && hook_entry_owned(unregister_entry) &&
            hook_entry_owned(destroyed_entry) ? TSUCHI_OK : TSUCHI_BUSY;
    }
    if (hook_entry_base(end_entry) != main + t::constant::ordinary_end_play ||
        hook_entry_base(unregister_entry) != main + t::constant::ordinary_unregister ||
        hook_entry_base(destroyed_entry) != main + t::constant::ordinary_destroyed) return TSUCHI_UNSUPPORTED;
    // A recorded chain remains qualified only while its permanent entry thunk
    // is still installed. Core-owned wrappers preserve their original caller.
    if ((game::read_or<uint64_t>(end_entry) != main + t::constant::ordinary_end_play && !hook_entry_owned(end_entry)) ||
        (game::read_or<uint64_t>(unregister_entry) != main + t::constant::ordinary_unregister && !hook_entry_owned(unregister_entry)) ||
        (game::read_or<uint64_t>(destroyed_entry) != main + t::constant::ordinary_destroyed && !hook_entry_owned(destroyed_entry))) return TSUCHI_BUSY;
    auto* made = static_cast<OwnerTable*>(allocate(sizeof(OwnerTable)));
    if (!made) return TSUCHI_NO_MEMORY;
    *made = OwnerTable{table, 0, 0, 0, reinterpret_cast<void*>(main + t::constant::ordinary_end_play),
        reinterpret_cast<void*>(main + t::constant::ordinary_unregister),
        reinterpret_cast<void*>(main + t::constant::ordinary_destroyed), nullptr, false};
    {
        Lock lock;
        made->next = tables;
        tables = made;
    }
    // Hook dispatch publishes these outputs under its lock before a selected
    // wrapper can run. Readers acquire them, including during installation.
    TSUCHI_Result result = hook_wrap(framework, end_entry, main + t::constant::ordinary_end_play, reinterpret_cast<void*>(on_end), kHookNeedsStock, &made->end_id, &made->end);
    if (result == TSUCHI_OK)
        result = hook_wrap(framework, unregister_entry, main + t::constant::ordinary_unregister, reinterpret_cast<void*>(on_unregister), kHookNeedsStock, &made->unregister_id, &made->unregister);
    if (result == TSUCHI_OK)
        result = hook_wrap(framework, destroyed_entry, main + t::constant::ordinary_destroyed, reinterpret_cast<void*>(on_destroyed), kHookNeedsStock, &made->destroyed_id, &made->destroyed);
    if (result == TSUCHI_OK) { Lock lock; made->ready = true; return result; }
    if (made->unregister_id) hook_remove(framework, made->unregister_id);
    if (made->end_id) hook_remove(framework, made->end_id);
    // Keep the record: an already dispatched wrapper may still be reading it.
    // A failed installation remains refused, never treated as fully qualified.
    // ready stays false, so further takes on this table answer unsupported.
    return result;
}
} // namespace tsuchi
