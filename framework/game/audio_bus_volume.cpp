// Native bus gain uses Wwise's outgoing-volume callback, without replacing a bank.
#include "audio_bus_volume.h"
#include "audio_parameters.h"
#include "audio_bus_binding.h"
#include "frame.h"
#include "game.h"
#include "registry.h"
#include <string.h>
#include <stddef.h>

namespace tsuchi {
namespace {
namespace t = game::table;
constexpr uint32_t limit = 64;
const Kind owner_kind{"audio.bus_volume"}, cleanup_kind{"audio.bus_volume.cleanup"};
using Callback = void (*)(void*);
struct Record {
    bool used, retiring, binding;
    uint32_t bus, gain, curve;
    uint64_t manager, epoch;
    TSUCHI_Mod* owner;
};
Record records[limit]{};
TSUCHI_Mod* framework = nullptr;
bool frames_ready = false;

uint32_t bits(float value) { uint32_t result; memcpy(&result, &value, sizeof result); return result; }
template<size_t Index> void volume_callback(void* info) {
    // The engine holds the callback-manager mutex. Static slots survive mod stop.
    // All layout values are selected before the audio thread can enter here.
    const uint32_t raw = __atomic_load_n(&records[Index].gain, __ATOMIC_ACQUIRE);
    float volume; memcpy(&volume, &raw, sizeof volume);
    if (info && volume != 1.0f) {
        auto* gain = *reinterpret_cast<float**>(static_cast<char*>(info) + t::wwise_volume_callback::base_volume);
        if (gain) *gain *= volume;
    }
}
template<size_t... Index> struct Sequence {};
template<size_t Count, size_t... Index> struct MakeSequence : MakeSequence<Count - 1, Count - 1, Index...> {};
template<size_t... Index> struct MakeSequence<0, Index...> { using Type = Sequence<Index...>; };
template<size_t... Index> constexpr auto callbacks(Sequence<Index...>) {
    struct List { Callback values[limit]; };
    return List{{volume_callback<Index>...}};
}
constexpr auto callback = callbacks(MakeSequence<limit>::Type{});
uint32_t index_of(const Record& record) { return uint32_t(&record - records); }
void neutral(Record& record) { __atomic_store_n(&record.gain, bits(1.0f), __ATOMIC_RELEASE); }

// The main lock precedes the manager lock, matching the audio thread. SetVolumeCallback
// reenters the manager's recursive mutex and enables existing mix connections before
// either outer lock is released. This makes inspect/register/unregister indivisible.
struct Locked {
    uint64_t manager = 0, epoch = 0;
    bool main_locked = false, manager_locked = false;
    Locked() {
        if (!frames_ready || !t::bound(t::fn::lock_mutex) || !t::bound(t::fn::unlock_mutex) ||
            !t::bound(t::fn::wwise_register_bus_volume_callback) || !t::bound(t::fn::wwise_get_bus) ||
            !t::bound(t::fn::wwise_release_bus) || !t::bound(t::fn::wwise_bind_bus_curve) ||
            !t::bound(t::fn::wwise_unbind_bus_curve) || !t::bound(t::fn::wwise_bus_subscription) ||
            !t::bound(t::fn::wwise_parameter_id) || !audio_parameter_engine_epoch()) return;
        t::fn::lock_mutex(reinterpret_cast<void*>(game::main_base() + t::data::wwise_main_lock));
        main_locked = true;
        epoch = audio_parameter_engine_epoch();
        manager = epoch ? game::global<uint64_t>(t::data::wwise_bus_callback_manager) : 0;
        if (!manager || !game::readable(manager, t::wwise_bus_callbacks::mutex + 0x20)) { manager = 0; return; }
        t::fn::lock_mutex(reinterpret_cast<void*>(manager + t::wwise_bus_callbacks::mutex));
        manager_locked = true;
    }
    ~Locked() {
        if (manager_locked) t::fn::unlock_mutex(reinterpret_cast<void*>(manager + t::wwise_bus_callbacks::mutex));
        if (main_locked) t::fn::unlock_mutex(reinterpret_cast<void*>(game::main_base() + t::data::wwise_main_lock));
    }
    bool current() const {
        return manager_locked && epoch && epoch == audio_parameter_engine_epoch() &&
               manager == game::global<uint64_t>(t::data::wwise_bus_callback_manager);
    }
    bool find(uint32_t bus, Callback& out) const {
        out = nullptr;
        if (!current()) return false;
        const uint64_t entries = game::read_or<uint64_t>(manager + t::wwise_bus_callbacks::entries);
        const uint32_t count = game::read_or<uint32_t>(manager + t::wwise_bus_callbacks::count);
        if (count > 65536 || (count && (!entries || !game::readable(entries, uint64_t(count) * t::wwise_bus_callback_entry::size)))) return false;
        for (uint32_t i = 0; i < count; ++i) {
            const uint64_t entry = entries + uint64_t(i) * t::wwise_bus_callback_entry::size;
            if (game::read_or<uint32_t>(entry + t::wwise_bus_callback_entry::bus) == bus) {
                out = reinterpret_cast<Callback>(game::read_or<uint64_t>(entry + t::wwise_bus_callback_entry::callback));
                return true;
            }
        }
        return true;
    }
};
TSUCHI_Result ensure_binding(Record& record) {
    using namespace audio_bus_binding;
    Bus bus(record.bus);
    if (!bus) return TSUCHI_NOT_FOUND;
    const Curve before = bus.curve(record.curve);
    if (before == Curve::unavailable) return TSUCHI_NOT_READY;
    if (before == Curve::foreign || (before == Curve::ours && !record.binding)) return TSUCHI_BUSY;
    if (before == Curve::ours) return TSUCHI_OK;
    const uint32_t result = bus.add(record.curve);
    const Curve after = bus.curve(record.curve);
    // A failed allocation can leave a partially installed native curve. Retain
    // its cleanup journal before returning the native refusal.
    record.binding = after != Curve::absent && after != Curve::foreign;
    if (result == 1 && after == Curve::ours) return TSUCHI_OK;
    return result == 52 ? TSUCHI_NO_MEMORY : TSUCHI_NOT_READY;
}
bool remove_binding(Record& record) {
    using namespace audio_bus_binding;
    if (!record.binding) return true;
    Bus bus(record.bus);
    if (bus) {
        const Curve current = bus.curve(record.curve);
        if (current == Curve::unavailable) return false;
        if (current == Curve::ours) {
            bus.remove(record.curve);
            if (bus.curve(record.curve) == Curve::ours || bus.curve(record.curve) == Curve::unavailable) return false;
        }
    }
    record.binding = false;
    return true;
}
bool retire_locked(Record& record, Callback installed) {
    neutral(record);
    if (installed == callback.values[index_of(record)] &&
        t::fn::wwise_register_bus_volume_callback(record.bus, nullptr) != 1) return false;
    return remove_binding(record);
}
void clear(Record& record) {
    if (record.owner && mod_usable(record.owner)) disown(record.owner, owner_kind, record.bus);
    if (mod_usable(framework)) disown(framework, cleanup_kind, record.bus);
    // Clear only after the old callback has left its manager's locked dispatch.
    neutral(record);
    record.used = record.retiring = record.binding = false;
    record.owner = nullptr;
}
void maintain(Record& record) {
    if (!record.used) return;
    const uint64_t epoch = audio_parameter_engine_epoch();
    if (!epoch) return; // Never read a manager while native Term is running.
    if (epoch != record.epoch) { neutral(record); clear(record); return; }
    Locked lock;
    if (!lock.current()) return;
    // An unexplained manager swap is not proof that the old dispatch is gone.
    // Keep its slot neutral and reserved until a completed Term epoch proves it.
    if (lock.manager != record.manager) { neutral(record); record.retiring = true; return; }
    Callback installed = nullptr;
    if (!lock.find(record.bus, installed)) return;
    if (installed != callback.values[index_of(record)] || record.retiring) {
        if (retire_locked(record, installed)) clear(record);
    } else {
        const TSUCHI_Result bound = ensure_binding(record);
        if (bound == TSUCHI_BUSY) {
            record.retiring = true;
            if (retire_locked(record, installed)) clear(record);
        }
    }
}
void ended(void* context, uint64_t) {
    auto& record = *static_cast<Record*>(context);
    neutral(record);
    record.owner = nullptr;
    record.retiring = true;
    maintain(record);
}
void journal_ended(void* context, uint64_t key) {
    auto& record = *static_cast<Record*>(context);
    if (record.owner && mod_usable(record.owner)) disown(record.owner, owner_kind, key);
    ended(context, key);
}
void step() { for (auto& record : records) maintain(record); }
}

TSUCHI_Result audio_hold_bus_volume(TSUCHI_Mod* mod, uint32_t bus, float volume) {
    if (!mod_usable(mod) || !bus || !(volume >= 0.0f && volume <= 1.0f)) return TSUCHI_INVALID;
    const TSUCHI_Result observed = audio_parameter_observers();
    if (observed != TSUCHI_OK) return observed;
    for (auto& record : records) if (record.used && record.bus == bus) maintain(record);
    Locked lock;
    if (!lock.current()) return TSUCHI_NOT_READY;
    Callback installed = nullptr;
    if (!lock.find(bus, installed)) return TSUCHI_NOT_READY;
    for (auto& record : records) if (record.used && record.bus == bus) {
        if (record.retiring || record.owner != mod || installed != callback.values[index_of(record)] ||
            !ownership_update_allowed(mod, owner_kind, bus)) return TSUCHI_BUSY;
        __atomic_store_n(&record.gain, bits(volume), __ATOMIC_RELEASE);
        return TSUCHI_OK;
    }
    if (installed) return TSUCHI_BUSY;
    Record* record = nullptr;
    for (auto& candidate : records) if (!candidate.used) { record = &candidate; break; }
    if (!record) return TSUCHI_CAPACITY;
    record->used = true; record->retiring = record->binding = false; record->owner = mod;
    record->bus = bus; record->manager = lock.manager; record->epoch = lock.epoch;
    record->curve = t::fn::wwise_parameter_id("tsuchi_bus_volume_neutral");
    neutral(*record);
    TSUCHI_Result owned = own(mod, owner_kind, bus, ended, record, true);
    if (owned == TSUCHI_OK) owned = own(framework, cleanup_kind, bus, journal_ended, record, true);
    if (owned != TSUCHI_OK) { clear(*record); return owned; }
    const TSUCHI_Result bound = ensure_binding(*record);
    if (bound != TSUCHI_OK) {
        record->retiring = true;
        if (retire_locked(*record, nullptr)) clear(*record);
        return bound;
    }
    __atomic_store_n(&record->gain, bits(volume), __ATOMIC_RELEASE);
    const uint32_t result = t::fn::wwise_register_bus_volume_callback(bus, reinterpret_cast<void*>(callback.values[index_of(*record)]));
    if (result != 1) {
        record->retiring = true;
        if (retire_locked(*record, nullptr)) clear(*record);
        return result == 52 ? TSUCHI_NO_MEMORY : TSUCHI_NOT_READY;
    }
    return TSUCHI_OK;
}

TSUCHI_Result audio_release_bus_volume(TSUCHI_Mod* mod, uint32_t bus) {
    if (!mod_usable(mod) || !bus) return TSUCHI_INVALID;
    for (auto& record : records) if (record.used && record.bus == bus && record.owner == mod && !record.retiring) {
        if (!ownership_update_allowed(mod, owner_kind, bus)) return TSUCHI_BUSY;
        disown(mod, owner_kind, bus);
        ended(&record, bus);
        return TSUCHI_OK;
    }
    return TSUCHI_NOT_FOUND;
}
void audio_bus_volumes_start(TSUCHI_Mod* self) {
    framework = self;
    if (!frames_ready) frames_ready = each_frame(step);
}
}
