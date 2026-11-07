#include "audio_parameters.h"
#include "frame.h"
#include "game.h"
#include "hooks.h"
#include "registry.h"
#include <float.h>
#include <string.h>

namespace tsuchi {
namespace {
namespace t = game::table;
constexpr uint64_t kGlobal = UINT64_MAX;
constexpr uint32_t kSuccess = 1, kMissing = 15, kInvalidFloat = 95;
const Kind parameter_kind{"audio.parameter"}, journal_kind{"audio.parameter.cleanup"};

struct Engine { uint64_t audio, parameters, epoch; };
struct Value { float number; bool global; };
struct Record {
    bool used, retiring, written, restoring, grace;
    uint32_t id, calls;
    char name[128];
    TSUCHI_Mod* owner;
    char owner_id[kModIdBytes]; // the holding mod's ID, kept after its context ends
    Engine engine;
    Value original;
    float wanted;
    uint64_t ticket;
};
// The journal is allocated before any native write. Releasing an owner never
// allocates, and a retiring ID stays reserved even after its mod has stopped.
Record records[64]{};
TSUCHI_Mod* framework = nullptr;
bool frames_ready = false;
uint64_t queue_hook = 0, term_hook = 0;
using Queue = decltype(t::code_site::wwise_process_queue)::Function;
using Term = decltype(t::code_site::wwise_term)::Function;
void* next_queue = nullptr;
void* next_term = nullptr;
uint64_t requested_ticket = 0, acknowledged_ticket = 0, engine_epoch = 1, observed_manager = 0;
uint32_t terminating = 0;

void maximum(uint64_t& target, uint64_t value) {
    uint64_t before = __atomic_load_n(&target, __ATOMIC_ACQUIRE);
    while (before < value && !__atomic_compare_exchange_n(&target, &before, value, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {}
}
// Both native callers hold the producer/consumer lock through this call and its
// return. Only these verified queue fields and framework atomics are touched on
// the audio thread. Capture BEFORE the original: a later enqueue is not drained
// merely because this invocation found the queue empty.
bool process_queue(void* manager, bool pressure, void* result) {
    const uint64_t ticket = __atomic_load_n(&requested_ticket, __ATOMIC_ACQUIRE);
    const uint64_t epoch = __atomic_load_n(&engine_epoch, __ATOMIC_ACQUIRE);
    const uint64_t observed = __atomic_load_n(&observed_manager, __ATOMIC_ACQUIRE);
    const auto original = reinterpret_cast<Queue>(__atomic_load_n(&next_queue, __ATOMIC_ACQUIRE));
    const bool accepted = original(manager, pressure, result);
    const uint64_t address = reinterpret_cast<uint64_t>(manager);
    if (address && address == observed && observed == __atomic_load_n(&observed_manager, __ATOMIC_ACQUIRE) &&
        epoch == __atomic_load_n(&engine_epoch, __ATOMIC_ACQUIRE) &&
        __atomic_load_n(reinterpret_cast<const uint64_t*>(address + t::wwise_audio_queue::read), __ATOMIC_ACQUIRE) ==
        __atomic_load_n(reinterpret_cast<const uint64_t*>(address + t::wwise_audio_queue::write), __ATOMIC_ACQUIRE) &&
        !__atomic_load_n(reinterpret_cast<const uint32_t*>(address + t::wwise_audio_queue::reservations), __ATOMIC_ACQUIRE))
        maximum(acknowledged_ticket, ticket);
    return accepted;
}
void terminate() {
    __atomic_add_fetch(&terminating, uint32_t(1), __ATOMIC_ACQ_REL);
    __atomic_add_fetch(&engine_epoch, uint64_t(1), __ATOMIC_ACQ_REL);
    const auto original = reinterpret_cast<Term>(__atomic_load_n(&next_term, __ATOMIC_ACQUIRE));
    original();
    __atomic_sub_fetch(&terminating, uint32_t(1), __ATOMIC_RELEASE);
}
bool ready(Engine& out) {
    if (__atomic_load_n(&terminating, __ATOMIC_ACQUIRE)) return false;
    const uint64_t epoch = __atomic_load_n(&engine_epoch, __ATOMIC_ACQUIRE);
    if (!t::bound(t::fn::wwise_initialized) || !t::fn::wwise_initialized()) return false;
    out = {game::global<uint64_t>(t::data::wwise_audio_manager),
           game::global<uint64_t>(t::data::wwise_parameter_manager), epoch};
    return out.audio && out.parameters && epoch == __atomic_load_n(&engine_epoch, __ATOMIC_ACQUIRE) &&
           !__atomic_load_n(&terminating, __ATOMIC_ACQUIRE);
}
bool same(const Engine& a, const Engine& b) { return a.audio == b.audio && a.parameters == b.parameters && a.epoch == b.epoch; }
// Successful native calls may have reentered teardown and admitted requests in
// a replacement engine. Never publish the old manager over those new tickets.
// Readiness alone may fall temporarily; an accepted same-instance command still
// needs its journal and fence even while IsInitialized reports false.
bool identity(const Engine& engine) {
    return engine.epoch == __atomic_load_n(&engine_epoch, __ATOMIC_ACQUIRE) &&
           engine.audio == game::global<uint64_t>(t::data::wwise_audio_manager) &&
           engine.parameters == game::global<uint64_t>(t::data::wwise_parameter_manager) &&
           engine.epoch == __atomic_load_n(&engine_epoch, __ATOMIC_ACQUIRE);
}
enum class Presence { same, unavailable, replaced };
Presence present(const Record& record) {
    if (record.engine.epoch != __atomic_load_n(&engine_epoch, __ATOMIC_ACQUIRE)) return Presence::replaced;
    Engine current{};
    if (!ready(current)) return Presence::unavailable;
    return same(record.engine, current) ? Presence::same : Presence::replaced;
}
bool number(float value) { return value == value && value < FLT_MAX && value > -FLT_MAX; }
TSUCHI_Result name_of(const char* name, char out[128]) {
    if (!name || !*name) return TSUCHI_INVALID;
    for (uint32_t i = 0;; ++i) {
        const unsigned char c = static_cast<unsigned char>(name[i]);
        if (!c) { out[i] = 0; return TSUCHI_OK; }
        if (i == 127) return TSUCHI_CAPACITY;
        if (c < 32 || c > 126) return TSUCHI_INVALID;
        out[i] = c >= 'A' && c <= 'Z' ? char(c + ('a' - 'A')) : char(c);
    }
}
TSUCHI_Result query(uint32_t id, Value& out) {
    if (!t::bound(t::fn::wwise_get_parameter)) return TSUCHI_NOT_READY;
    uint32_t scope = 1;
    float value = 0;
    const uint32_t result = t::fn::wwise_get_parameter(id, kGlobal, 0, &value, &scope);
    if (result == kMissing || scope == 4) return TSUCHI_NOT_FOUND;
    if (result != kSuccess || scope > 1 || !number(value)) return TSUCHI_NOT_READY;
    out = {value, scope == 1};
    return TSUCHI_OK;
}
TSUCHI_Result observers() {
    if (queue_hook && term_hook) return TSUCHI_OK;
    if (!frames_ready || !mod_usable(framework) ||
        !code_site_patched(t::code_site::wwise_process_queue.number) ||
        !code_site_patched(t::code_site::wwise_term.number)) return TSUCHI_NOT_READY;
    // Give the hook core the actual continuation storage. Its dispatch lock
    // publishes that storage before another thread can enter the new hook;
    // the typed helper copies a local continuation only after unlocking.
    const Term term = terminate;
    const Queue queue = process_queue;
    TSUCHI_Result result = hook_code_site(framework, t::code_site::wwise_term.number,
                                         reinterpret_cast<void*>(term), &term_hook, &next_term);
    if (result != TSUCHI_OK) return result;
    result = hook_code_site(framework, t::code_site::wwise_process_queue.number,
                            reinterpret_cast<void*>(queue), &queue_hook, &next_queue);
    if (result != TSUCHI_OK) { hook_remove(framework, term_hook); term_hook = 0; }
    return result;
}
Record* find(uint32_t id) { for (auto& record : records) if (record.used && record.id == id) return &record; return nullptr; }
uint64_t publish(Record& record) {
    __atomic_store_n(&observed_manager, record.engine.audio, __ATOMIC_RELEASE);
    return __atomic_add_fetch(&requested_ticket, uint64_t(1), __ATOMIC_RELEASE);
}
TSUCHI_Result set(Record& record, float value) {
    if (!t::bound(t::fn::wwise_set_parameter)) return TSUCHI_NOT_READY;
    const uint32_t result = t::fn::wwise_set_parameter(record.id, value, kGlobal, 0, 4, false);
    if (result != kSuccess) return result == kInvalidFloat ? TSUCHI_INVALID : TSUCHI_NOT_READY;
    if (!identity(record.engine)) return TSUCHI_NOT_READY;
    record.ticket = publish(record);
    record.wanted = value;
    record.written = true;
    return TSUCHI_OK;
}
void clear(Record& record) {
    if (record.owner && mod_usable(record.owner)) disown(record.owner, parameter_kind, record.id);
    if (mod_usable(framework)) disown(framework, journal_kind, record.id);
    record = {};
}
void maintain(Record& record);
void ended(void* context, uint64_t) {
    auto& record = *static_cast<Record*>(context);
    record.owner = nullptr;
    record.retiring = true;
    // Restarted scripts can reclaim in their first step handler. Do not enqueue a baseline that the
    // audio thread could consume before then. Explicit release clears owner_id and has no grace period.
    record.grace = record.owner_id[0] != 0;
    if (!record.calls && !record.grace) maintain(record);
}
void journal_ended(void* context, uint64_t) {
    auto& record = *static_cast<Record*>(context);
    record.retiring = true;
    record.grace = false;
    // Framework teardown cannot admit another request. The in-flight native
    // call, if any, still finishes before the slot can be retired.
    if (!record.calls) maintain(record);
}
bool expected(const Record& record, Value value) { return value.global && value.number == record.wanted; }
bool drained(const Record& record) { return record.ticket <= __atomic_load_n(&acknowledged_ticket, __ATOMIC_ACQUIRE); }
void maintain(Record& record) {
    if (!record.used || record.calls || record.grace) return;
    ++record.calls;
    bool done = false;
    const Presence presence = present(record);
    if (presence == Presence::replaced) done = true;
    else if (presence == Presence::same) {
        if (record.retiring) {
            if (!record.written) done = true;
            else if (record.restoring) done = drained(record);
            else if (drained(record)) {
                bool restore = true;
                // Wait for our accepted batch before choosing a restore. This
                // also lets a foreign command queued after ours become visible
                // rather than overwriting it with a premature restore.
                Value actual{};
                const TSUCHI_Result found = query(record.id, actual);
                if (present(record) != Presence::same) restore = false;
                else if (found == TSUCHI_NOT_FOUND || (found == TSUCHI_OK && !expected(record, actual))) { done = true; restore = false; }
                else if (found != TSUCHI_OK) restore = false;
                if (restore && present(record) == Presence::same) {
                    uint32_t result = 0;
                    if (record.original.global && t::fn::wwise_set_parameter)
                        result = t::fn::wwise_set_parameter(record.id, record.original.number, kGlobal, 0, 4, false);
                    else if (!record.original.global && t::fn::wwise_reset_parameter)
                        result = t::fn::wwise_reset_parameter(record.id, kGlobal, 0, 4, false);
                    if (result == kSuccess && identity(record.engine)) { record.ticket = publish(record); record.restoring = true; }
                }
            }
        } else if (record.written && drained(record)) {
            Value actual{};
            const TSUCHI_Result found = query(record.id, actual);
            if (present(record) == Presence::same && !record.retiring) {
                if (found == TSUCHI_NOT_FOUND) done = true;
                else if (found == TSUCHI_OK && !expected(record, actual)) {
                    record.original = actual;
                    set(record, record.wanted);
                }
            }
        }
    }
    --record.calls;
    if (done) clear(record);
}
void step() {
    for (auto& record : records) {
        if (record.grace) { record.grace = false; continue; }
        maintain(record);
    }
}
}

TSUCHI_Result audio_parameter(const char* name, float* out) {
    if (!on_game_thread() || !out) return TSUCHI_INVALID;
    char checked[128]{}; const TSUCHI_Result named = name_of(name, checked);
    if (named != TSUCHI_OK) return named;
    Engine engine{};
    if (!ready(engine) || !t::bound(t::fn::wwise_parameter_id)) return TSUCHI_NOT_READY;
    const uint32_t id = t::fn::wwise_parameter_id(checked);
    Value value{}; const TSUCHI_Result found = query(id, value);
    Engine after{};
    if (!ready(after) || !same(engine, after)) return TSUCHI_NOT_READY;
    if (found == TSUCHI_OK) *out = value.number;
    return found;
}

TSUCHI_Result audio_hold_parameter(TSUCHI_Mod* mod, const char* name, float value) {
    if (!mod_usable(mod)) return TSUCHI_INVALID;
    char checked[128]{}; const TSUCHI_Result named = name_of(name, checked);
    if (named != TSUCHI_OK) return named;
    if (!number(value)) return TSUCHI_INVALID;
    Engine engine{};
    if (!ready(engine) || !t::bound(t::fn::wwise_parameter_id)) return TSUCHI_NOT_READY;
    const uint32_t id = t::fn::wwise_parameter_id(checked);
    Record* record = find(id);
    if (record && !record->calls && present(*record) == Presence::replaced) { clear(*record); record = nullptr; }
    // A stopped mod's record remains reserved through the next step's handlers, without a restore queued.
    // The same mod takes it over and preserves the baseline from before its first hold.
    bool adopted = false;
    if (record && record->retiring && !record->owner && !record->calls && !strcmp(record->name, checked) &&
        !strcmp(record->owner_id, mod_id(mod)) && present(*record) == Presence::same) {
        if (own(mod, parameter_kind, id, ended, record, true) != TSUCHI_OK) return TSUCHI_BUSY;
        record->owner = mod; record->retiring = false; record->restoring = false; record->grace = false; adopted = true;
    }
    if (record && (strcmp(record->name, checked) || record->owner != mod || record->retiring || record->calls)) return TSUCHI_BUSY;
    if (record && !adopted && !ownership_update_allowed(mod, parameter_kind, id)) return TSUCHI_BUSY;
    const TSUCHI_Result observed = observers();
    if (observed != TSUCHI_OK) return observed;
    const bool fresh = !record;
    if (fresh) {
        for (auto& available : records) if (!available.used) { record = &available; break; }
        if (!record) return TSUCHI_CAPACITY;
        *record = {}; record->used = true; record->id = id; record->owner = mod; record->engine = engine;
        memcpy(record->name, checked, sizeof record->name);
        const char* const holder = mod_id(mod);
        if (holder && strlen(holder) < sizeof record->owner_id) memcpy(record->owner_id, holder, strlen(holder) + 1);
        TSUCHI_Result owned = own(mod, parameter_kind, id, ended, record, true);
        if (owned == TSUCHI_OK) owned = own(framework, journal_kind, id, journal_ended, record, true);
        if (owned != TSUCHI_OK) { clear(*record); return owned; }
    }
    ++record->calls;
    TSUCHI_Result result = TSUCHI_OK;
    bool needs_write = fresh || adopted || record->wanted != value;
    if (present(*record) != Presence::same) result = TSUCHI_NOT_READY;
    else if (fresh || drained(*record)) {
        Value actual{}; result = query(id, actual);
        if (result == TSUCHI_OK && (fresh || !expected(*record, actual))) {
            record->original = actual;
            needs_write = true;
        }
    }
    if (result == TSUCHI_OK && (!mod_usable(mod) || record->retiring || present(*record) != Presence::same)) result = TSUCHI_NOT_READY;
    if (result == TSUCHI_OK && needs_write) result = set(*record, value);
    if (!mod_usable(mod) || record->retiring || present(*record) != Presence::same) result = TSUCHI_NOT_READY;
    --record->calls;
    if (fresh && !record->written) clear(*record);
    else if (record->retiring || present(*record) == Presence::replaced) maintain(*record);
    return result;
}

TSUCHI_Result audio_release_parameter(TSUCHI_Mod* mod, const char* name) {
    if (!mod_usable(mod)) return TSUCHI_INVALID;
    char checked[128]{}; const TSUCHI_Result named = name_of(name, checked);
    if (named != TSUCHI_OK) return named;
    if (!t::bound(t::fn::wwise_parameter_id)) return TSUCHI_NOT_FOUND;
    Record* record = find(t::fn::wwise_parameter_id(checked));
    if (!record || record->owner != mod || record->retiring) return TSUCHI_NOT_FOUND;
    if (strcmp(record->name, checked)) return TSUCHI_DENIED;
    if (!ownership_update_allowed(mod, parameter_kind, record->id)) return TSUCHI_BUSY;
    disown(mod, parameter_kind, record->id);
    record->owner_id[0] = 0; // released on purpose: only a mod that stopped with its hold takes it over
    ended(record, record->id);
    return TSUCHI_OK;
}

void audio_parameters_start(TSUCHI_Mod* self) {
    framework = self;
    if (!frames_ready) frames_ready = each_frame(step);
}
}
