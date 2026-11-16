#include "registry.h"

#include <string.h>
#if __STDC_HOSTED__
#include <stdlib.h>
#endif

namespace tsuchi {
namespace {

enum class State : uint8_t { usable, stopping, stopped };

struct Owned {
    const Kind* kind;
    uint64_t key;
    Release release;
    void* context;
    uint64_t sequence; // zero once ended, otherwise the acquisition order
};

} // namespace

struct Listener {
    uint64_t id; // 0 once ended
    TSUCHI_Mod* mod;
    void (*function)();
    void* user;
};

} // namespace tsuchi

struct TSUCHI_Mod {
    uint32_t magic;
    tsuchi::State state;
    char id[tsuchi::kModIdBytes];
    tsuchi::Owned* owned;
    uint32_t count, capacity;
    uint32_t rank; // where it comes among those that were started: later is higher
    uint64_t next_owned = 1;
    uint64_t ownership_floor = UINT64_MAX;
    const char* blocked; // why it cannot run, when something it asked for said so
};

namespace tsuchi {
namespace {

// Hook dispatch reads this while the game's thread stops a mod.
State mod_state(const TSUCHI_Mod* mod) {
    State state;
    __atomic_load(&mod->state, &state, __ATOMIC_ACQUIRE);
    return state;
}
void set_mod_state(TSUCHI_Mod* mod, State state) { __atomic_store(&mod->state, &state, __ATOMIC_RELEASE); }

constexpr uint32_t kMagic = 0x70346D21;
const Kind kSubscription{"subscription"};

#if __STDC_HOSTED__
Allocator allocator{malloc, free};
#else
Allocator allocator{nullptr, nullptr};
#endif

TSUCHI_Mod** mods = nullptr;
uint32_t mod_count = 0, mod_capacity = 0;
uint64_t (*thread_probe)() = nullptr;
void (*thread_pause)() = nullptr;
uint64_t game_thread = 0; // what the probe gave on the game's thread, or 0 while that is not known
uint32_t refused_off_thread = 0;
uint64_t next_subscription = 1; // ids are never used twice

// Grows a table of plain records to hold `need` of them. False when memory runs out.
template <class T>
bool reserve(T*& items, uint32_t count, uint32_t& capacity, uint32_t need) {
    if (need <= capacity) return true;
    if (!allocator.allocate) return false;
    uint32_t next = capacity ? capacity * 2 : 8;
    while (next < need) next *= 2;
    T* grown = static_cast<T*>(allocator.allocate(sizeof(T) * next));
    if (!grown) return false;
    if (items) {
        memcpy(static_cast<void*>(grown), items, sizeof(T) * count);
        allocator.release(items);
    }
    items = grown;
    capacity = next;
    return true;
}

Owned* find_owned(TSUCHI_Mod* mod, const Kind& kind, uint64_t key) {
    for (uint32_t i = mod->count; i-- > 0;) {
        Owned& o = mod->owned[i];
        if (o.sequence && o.kind == &kind && o.key == key) return &o;
    }
    return nullptr;
}

// Closes the gaps left in a mod's list by records it gave up, keeping the order of the rest.
void close_gaps(TSUCHI_Mod* mod) {
    uint32_t kept = 0;
    for (uint32_t i = 0; i < mod->count; ++i)
        if (mod->owned[i].sequence) mod->owned[kept++] = mod->owned[i];
    mod->count = kept;
}

void compact(Topic& topic) {
    uint32_t kept = 0;
    for (uint32_t i = 0; i < topic.count; ++i)
        if (topic.listeners[i].id) topic.listeners[kept++] = topic.listeners[i];
    topic.count = kept;
    topic.ended = 0;
}

// Ends a subscription inside its topic. The owner's record is handled by the caller.
void end_listener(void* context, uint64_t id) {
    Topic& topic = *static_cast<Topic*>(context);
    for (uint32_t i = 0; i < topic.count; ++i) {
        if (topic.listeners[i].id != id) continue;
        topic.listeners[i].id = 0;
        ++topic.ended;
        break;
    }
    if (!topic.emitting) compact(topic);
}

} // namespace

bool valid_mod_id(const char* id) {
    if (!id) return false;
    size_t length = 0;
    for (; length < kModIdBytes && id[length]; ++length) {
        const char c = id[length];
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-')) return false;
    }
    const auto plain = [](char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'); };
    return length >= 1 && length < kModIdBytes && plain(id[0]) && plain(id[length - 1]);
}

void set_allocator(Allocator next) { allocator = next; }
void* allocate(size_t bytes) { return allocator.allocate ? allocator.allocate(bytes) : nullptr; }
void release(void* block) {
    if (block && allocator.release) allocator.release(block);
}

TSUCHI_Mod* mod_create(const char* id, uint32_t rank) {
    if (!valid_mod_id(id) || !allocator.allocate) return nullptr;
    for (uint32_t i = 0; i < mod_count; ++i)
        if (mod_state(mods[i]) != State::stopped && !strcmp(mods[i]->id, id)) return nullptr;
    if (!reserve(mods, mod_count, mod_capacity, mod_count + 1)) return nullptr;
    auto* mod = static_cast<TSUCHI_Mod*>(allocator.allocate(sizeof(TSUCHI_Mod)));
    if (!mod) return nullptr;
    *mod = TSUCHI_Mod{};
    mod->magic = kMagic;
    set_mod_state(mod, State::usable);
    static uint32_t started = 0;
    mod->rank = rank ? rank : ++started;
    memcpy(mod->id, id, strlen(id) + 1); // valid_mod_id bounds the length
    mods[mod_count++] = mod;
    return mod;
}

void set_thread_probe(uint64_t (*current)()) {
    thread_probe = current;
    __atomic_store_n(&game_thread, uint64_t(0), __ATOMIC_RELEASE);
}

uint64_t current_thread_token() { return thread_probe ? thread_probe() : 1; }

void set_thread_pause(void (*pause)()) { thread_pause = pause; }

void lock_wait(uint32_t& tries) {
    if (++tries < 64 || !thread_pause) {
#if defined(__aarch64__)
        asm volatile("yield");
#endif
        return;
    }
    thread_pause();
}

void note_game_thread() {
    if (thread_probe) __atomic_store_n(&game_thread, thread_probe(), __ATOMIC_RELEASE);
}

bool on_game_thread() {
    const uint64_t thread = __atomic_load_n(&game_thread, __ATOMIC_ACQUIRE);
    return !thread_probe || !thread || thread_probe() == thread;
}

uint64_t known_game_thread_token() {
    const auto probe = thread_probe;
    const uint64_t noted = __atomic_load_n(&game_thread, __ATOMIC_ACQUIRE);
    if (!probe || !noted) return 0;
    const uint64_t current = probe();
    if (current != noted || probe != thread_probe ||
        noted != __atomic_load_n(&game_thread, __ATOMIC_ACQUIRE)) return 0;
    return current;
}

uint32_t off_thread_calls() { return __atomic_load_n(&refused_off_thread, __ATOMIC_RELAXED); }

bool mod_running(const TSUCHI_Mod* mod) { return mod && mod->magic == kMagic && mod_state(mod) == State::usable; }

namespace {
uint32_t hook_clients = 0; // game thread only
} // namespace

void hook_client_enter() { ++hook_clients; }
void hook_client_leave() {
    if (hook_clients) --hook_clients;
}
uint32_t hook_client_suspend() {
    const uint32_t depth = hook_clients;
    hook_clients = 0;
    return depth;
}
void hook_client_restore(uint32_t depth) { hook_clients = depth; }
bool in_hook_client() { return hook_clients && on_game_thread(); }

bool mod_usable(const TSUCHI_Mod* mod) {
    if (!mod_running(mod)) return false;
    if (in_hook_client()) return false;
    if (on_game_thread()) return true;
    __atomic_fetch_add(&refused_off_thread, 1, __ATOMIC_RELAXED); // counted from whichever thread it was
    return false;
}

bool mod_usable_in_hooks(const TSUCHI_Mod* mod) {
    if (!mod_running(mod)) return false;
    if (on_game_thread()) return true;
    __atomic_fetch_add(&refused_off_thread, 1, __ATOMIC_RELAXED);
    return false;
}

uint32_t mod_rank(const TSUCHI_Mod* mod) { return mod && mod->magic == kMagic ? mod->rank : 0; }

const char* mod_id(const TSUCHI_Mod* mod) { return mod && mod->magic == kMagic ? mod->id : ""; }

void mod_block(TSUCHI_Mod* mod, const char* why) {
    if (mod && mod->magic == kMagic && !mod->blocked) mod->blocked = why;
}

const char* mod_block_reason(const TSUCHI_Mod* mod) { return mod && mod->magic == kMagic ? mod->blocked : nullptr; }

void mod_stop(TSUCHI_Mod* mod) {
    if (!mod_usable(mod)) return;
    set_mod_state(mod, State::stopping); // nothing new can be owned from here on
    for (uint32_t i = mod->count; i-- > 0;) {
        if (!mod->owned[i].sequence) continue;
        const Owned record = mod->owned[i];
        mod->owned[i].sequence = 0;
        if (record.release) record.release(record.context, record.key);
    }
    if (mod->owned) allocator.release(mod->owned);
    mod->owned = nullptr;
    mod->count = mod->capacity = 0;
    set_mod_state(mod, State::stopped); // the context itself stays, so an old pointer is still safe to check
}

TSUCHI_Result reserve_owned(TSUCHI_Mod* mod, uint32_t additional) {
    if (!mod_usable(mod)) return TSUCHI_INVALID;
    if (additional > UINT32_MAX - mod->count) return TSUCHI_CAPACITY;
    return reserve(mod->owned, mod->count, mod->capacity, mod->count + additional) ? TSUCHI_OK : TSUCHI_NO_MEMORY;
}

TSUCHI_Result own(TSUCHI_Mod* mod, const Kind& kind, uint64_t key, Release release, void* context, bool exclusive) {
    if (!mod_usable(mod)) return TSUCHI_INVALID;
    if (mod->ownership_floor != UINT64_MAX) {
        const Owned* previous = find_owned(mod, kind, key);
        if (previous && previous->sequence < mod->ownership_floor) return TSUCHI_BUSY;
    }
    if (exclusive) {
        if (TSUCHI_Mod* current = owner(kind, key))
            return current == mod && ownership_update_allowed(mod, kind, key) ? TSUCHI_OK : TSUCHI_BUSY;
    }
    // A list that is full is first rid of the records given up, so that a mod which takes one thing
    // and gives up another, over and over, keeps a list as long as what it owns at once.
    if (mod->count == mod->capacity) close_gaps(mod);
    if (!reserve(mod->owned, mod->count, mod->capacity, mod->count + 1)) return TSUCHI_NO_MEMORY;
    mod->owned[mod->count++] = Owned{&kind, key, release, context, mod->next_owned++};
    return TSUCHI_OK;
}

TSUCHI_Result disown(TSUCHI_Mod* mod, const Kind& kind, uint64_t key) {
    if (!mod_usable(mod)) return TSUCHI_NOT_FOUND;
    Owned* record = find_owned(mod, kind, key);
    if (!record) return TSUCHI_NOT_FOUND;
    record->sequence = 0;
    while (mod->count && !mod->owned[mod->count - 1].sequence) --mod->count;
    return TSUCHI_OK;
}

TSUCHI_Mod* owner(const Kind& kind, uint64_t key) {
    for (uint32_t i = 0; i < mod_count; ++i)
        if (mod_state(mods[i]) == State::usable && find_owned(mods[i], kind, key)) return mods[i];
    return nullptr;
}

uint32_t owned_count(const TSUCHI_Mod* mod) {
    uint32_t n = 0;
    if (mod_usable(mod))
        for (uint32_t i = 0; i < mod->count; ++i) n += mod->owned[i].sequence != 0;
    return n;
}

uint64_t isolate_ownership(TSUCHI_Mod* mod, uint64_t first) {
    if (!mod || mod->magic != kMagic) return UINT64_MAX;
    const uint64_t before = mod->ownership_floor;
    mod->ownership_floor = first;
    return before;
}
bool ownership_update_allowed(const TSUCHI_Mod* mod, const Kind& kind, uint64_t key) {
    if (!mod) return true;
    if (!mod_running(mod)) return false;
    if (mod->ownership_floor == UINT64_MAX) return true;
    for (uint32_t i = mod->count; i-- > 0;) {
        const Owned& entry = mod->owned[i];
        if (entry.sequence && entry.kind == &kind && entry.key == key) return entry.sequence >= mod->ownership_floor;
    }
    return false;
}
bool ownership_isolated(const TSUCHI_Mod* mod) { return mod_running(mod) && mod->ownership_floor != UINT64_MAX; }
bool ownership_in_range(const TSUCHI_Mod* mod, const char* kind, uint64_t key, uint64_t first, uint64_t last) {
    if (!mod_usable(mod)) return false;
    for (uint32_t i = mod->count; i-- > 0;) {
        const Owned& entry = mod->owned[i];
        if (entry.sequence && entry.key == key && !strcmp(entry.kind->name, kind) && entry.sequence >= first && entry.sequence < last) return true;
    }
    return false;
}
uint64_t ownership_mark(const TSUCHI_Mod* mod) { return mod_running(mod) ? mod->next_owned : 0; }
void release_owned_between(TSUCHI_Mod* mod, uint64_t first, uint64_t last) {
    if (!mod_usable(mod) || first >= last) return;
    for (;;) {
        int32_t found = -1;
        for (uint32_t i = mod->count; i-- > 0;) {
            const Owned& entry = mod->owned[i];
            if (entry.sequence && entry.sequence >= first && entry.sequence < last) { found = int32_t(i); break; }
        }
        if (found < 0) break;
        const Owned captured = mod->owned[found];
        mod->owned[found].sequence = 0;
        if (captured.release) captured.release(captured.context, captured.key);
        if (!mod_usable(mod)) return;
    }
    close_gaps(mod);
}

TSUCHI_Result subscribe(Topic& topic, TSUCHI_Mod* mod, void (*function)(), void* user, TSUCHI_Subscription* out) {
    if (!mod_usable(mod) || !function || !out) return TSUCHI_INVALID;
    if (!reserve(topic.listeners, topic.count, topic.capacity, topic.count + 1)) return TSUCHI_NO_MEMORY;
    const uint64_t id = next_subscription;
    const TSUCHI_Result owned = own(mod, kSubscription, id, end_listener, &topic);
    if (owned != TSUCHI_OK) return owned;
    ++next_subscription;
    topic.listeners[topic.count++] = Listener{id, mod, function, user};
    out->id = id;
    return TSUCHI_OK;
}

TSUCHI_Result cancel(TSUCHI_Mod* mod, TSUCHI_Subscription subscription) {
    if (!mod_usable(mod)) return TSUCHI_INVALID;
    Owned* record = find_owned(mod, kSubscription, subscription.id);
    if (!record) return TSUCHI_NOT_FOUND; // ended already, or another mod's
    void* topic = record->context;
    disown(mod, kSubscription, subscription.id);
    end_listener(topic, subscription.id);
    return TSUCHI_OK;
}

static void deliver(Topic& topic, TSUCHI_Mod* recipient, bool targeted, const void* event) {
    if (!on_game_thread()) {
        __atomic_fetch_add(&refused_off_thread, 1, __ATOMIC_RELAXED);
        return;
    }
    ++topic.emitting;
    const uint32_t present = topic.count; // later subscribers first hear the next event
    for (uint32_t i = 0; i < present; ++i) {
        const Listener listener = topic.listeners[i]; // the table may move while a subscriber runs
        if (listener.id && (!targeted || listener.mod == recipient) && mod_usable(listener.mod)) topic.invoke(listener.function, listener.user, event);
    }
    if (!--topic.emitting && topic.ended) compact(topic);
}

void emit(Topic& topic, const void* event) { deliver(topic, nullptr, false, event); }
void emit_to(Topic& topic, TSUCHI_Mod* recipient, const void* event) { deliver(topic, recipient, true, event); }

uint32_t subscriber_count(const Topic& topic) { return topic.count - topic.ended; }

} // namespace tsuchi
