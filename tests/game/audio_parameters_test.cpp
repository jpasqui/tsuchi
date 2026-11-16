// The command queue is deliberately asynchronous. These are host checks, not audio measurements.
#include "fake_game.h"
#include "audio_parameters.h"
#include "audio_bus_binding.h"
#include "audio_service.h"
#include "frame.h"
#include "hooks.h"
#include "registry.h"
#include "services.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <limits>
#include <map>
#include <string>
#include <vector>

// Native mods built with the preceding audio service retain their four slots.
static_assert(offsetof(TSUCHI_AudioService, hold_parameter) == offsetof(TSUCHI_AudioService, parameter) + sizeof(void*));
static_assert(offsetof(TSUCHI_AudioService, release_parameter) == offsetof(TSUCHI_AudioService, hold_parameter) + sizeof(void*));
static_assert(offsetof(TSUCHI_AudioService, hold_bus_volume) > offsetof(TSUCHI_AudioService, release_parameter));

using namespace tsuchi;
namespace t = game::table;
namespace {
struct Parameter { float fallback; bool global; float value; };
struct Command { uint32_t id; float value; bool reset; uint64_t manager, instance, epoch; };
struct NativeCall { uint64_t manager, instance, epoch; };
std::map<std::string, uint32_t> names;
std::map<uint32_t, Parameter> parameters;
std::vector<Command> pending;
std::vector<void (*)()> steps;
std::vector<NativeCall> native_calls;
uint64_t manager = 0, instance = 0, native_epoch = 1, thread = 1;
uint64_t sound_system = 0;
uint32_t next_id = 100, next_mod = 0;
unsigned sets = 0, resets = 0, queries = 0, hashes = 0, drains = 0, terms = 0;
unsigned actor_sounds = 0;
bool initialized = true, sites_ready = false, refuse_value = false, drain_result = true;
uint32_t query_result_override = 0, query_scope_override = UINT32_MAX;
bool override_query_value = false;
float query_value = 0;
TSUCHI_Mod* framework = nullptr;
TSUCHI_Mod* stopping = nullptr;
TSUCHI_Mod* ticket_owner = nullptr;
TSUCHI_Mod* term_attempt_owner = nullptr;
bool term_attempted = false;
void (*set_callback)() = nullptr;
void (*reset_callback)() = nullptr;
void (*query_callback)() = nullptr;
void (*after_drain_callback)() = nullptr;
void (*term_callback)() = nullptr;
decltype(t::code_site::wwise_process_queue)::Function intercepted_queue = nullptr;
decltype(t::code_site::wwise_term)::Function intercepted_term = nullptr;
std::string replacement_name;
TSUCHI_Mod* replacement_owner = nullptr;
uint32_t replacement_id = 0;
bool replacement_created = false;

void invoke(void (*&callback)()) { const auto saved = callback; callback = nullptr; if (saved) saved(); }
std::string lower(const char* text) {
    std::string result;
    while (*text) { const unsigned char c = static_cast<unsigned char>(*text++); result += c >= 'A' && c <= 'Z' ? char(c + 32) : char(c); }
    return result;
}
uint32_t hash_name(const char* name) {
    ++hashes;
    const auto key = lower(name); const auto found = names.find(key);
    if (found != names.end()) return found->second;
    const uint32_t id = ++next_id; names[key] = id; return id;
}
uint32_t declare(const char* name, float fallback, bool global = false, float value = 0) {
    const uint32_t id = hash_name(name); parameters[id] = {fallback, global, value}; return id;
}
float current(uint32_t id) { const auto& parameter = parameters.at(id); return parameter.global ? parameter.value : parameter.fallback; }
void queue_positions() {
    const uint64_t read = fake::get<uint64_t>(manager + t::wwise_audio_queue::read);
    fake::put<uint64_t>(manager + t::wwise_audio_queue::write, read + pending.size());
}
uint32_t query(uint32_t id, uint64_t object, uint32_t playing, void* number, void* scope) {
    ++queries; assert(object == UINT64_MAX && playing == 0);
    auto* kind = static_cast<uint32_t*>(scope); auto* value = static_cast<float*>(number);
    assert(*kind == 1); // Request the global override, with the bank default as fallback.
    const auto found = parameters.find(id);
    uint32_t result = 1;
    if (found == parameters.end()) { *value = -321; *kind = 4; result = 15; }
    else { *value = current(id); *kind = found->second.global ? 1 : 0; }
    if (query_scope_override != UINT32_MAX) *kind = query_scope_override;
    if (override_query_value) *value = query_value;
    invoke(query_callback);
    return query_result_override ? query_result_override : result;
}
void assert_transition(uint64_t object, int32_t milliseconds, uint32_t curve, bool bypass) {
    assert(object == UINT64_MAX && milliseconds == 0 && curve == 4 && !bypass);
}
struct NativeUse {
    NativeCall identity{manager, instance, native_epoch};
    NativeUse() {
        for (const auto& active : native_calls)
            assert(active.manager != manager || active.instance != instance || active.epoch != native_epoch);
        native_calls.push_back(identity);
    }
    ~NativeUse() {
        assert(!native_calls.empty());
        const auto& active = native_calls.back();
        assert(active.manager == identity.manager && active.instance == identity.instance && active.epoch == identity.epoch);
        native_calls.pop_back();
    }
};
uint32_t set_parameter(uint32_t id, float value, uint64_t object, int32_t milliseconds, uint32_t curve, bool bypass) {
    assert_transition(object, milliseconds, curve, bypass);
    NativeUse call; ++sets;
    if (refuse_value && value == 999) return 95;
    // Native acceptance alone does not validate IDs or make the value visible.
    pending.push_back({id, value, false, manager, instance, native_epoch}); queue_positions();
    invoke(set_callback); return 1;
}
uint32_t reset_parameter(uint32_t id, uint64_t object, int32_t milliseconds, uint32_t curve, bool bypass) {
    assert_transition(object, milliseconds, curve, bypass);
    NativeUse call; ++resets;
    pending.push_back({id, 0, true, manager, instance, native_epoch}); queue_positions();
    invoke(reset_callback); return 1;
}
void consume() {
    const auto commands = pending; pending.clear();
    for (const auto& command : commands) {
        if (command.manager != manager || command.instance != instance || command.epoch != native_epoch) continue;
        const auto found = parameters.find(command.id);
        if (found == parameters.end()) continue;
        found->second.global = !command.reset;
        if (!command.reset) found->second.value = command.value;
    }
    const uint64_t write = fake::get<uint64_t>(manager + t::wwise_audio_queue::write);
    fake::put<uint64_t>(manager + t::wwise_audio_queue::read, write);
}
bool native_queue(void* supplied_manager, bool render, void* work) {
    assert(reinterpret_cast<uint64_t>(supplied_manager) == manager);
    assert(render && work == reinterpret_cast<void*>(uintptr_t(0x1234)));
    ++drains; consume(); invoke(after_drain_callback); return drain_result;
}
void native_term() {
    ++terms; invoke(term_callback);
    ++native_epoch; initialized = false; pending.clear();
    fake::put<uint64_t>(manager + t::wwise_audio_queue::read, 0);
    fake::put<uint64_t>(manager + t::wwise_audio_queue::write, 0);
}
bool play_actor_sound(void* actor, void* key, void* part, bool follows) {
    assert(reinterpret_cast<uint64_t>(actor) == 0x1234 && follows);
    const auto& sound = *static_cast<const game::Name*>(key);
    const auto& attachment = *static_cast<const game::Name*>(part);
    assert(sound.number == 0 && attachment.index == 0 && attachment.number == 0);
    ++actor_sounds; return sound.index == 1;
}
void process_queue() {
    assert(intercepted_queue); const unsigned before = drains;
    assert(intercepted_queue(reinterpret_cast<void*>(manager), true, reinterpret_cast<void*>(uintptr_t(0x1234))) == drain_result);
    assert(drains == before + 1);
}
void step() { for (const auto work : steps) work(); }
void settle() { for (unsigned i = 0; i < 5; ++i) { process_queue(); step(); } }
TSUCHI_Mod* make_mod() {
    const auto id = "audio.parameter.test." + std::to_string(++next_mod);
    auto* mod = mod_create(id.c_str()); assert(mod); return mod;
}
void install_instance(bool replace) {
    if (replace || !manager) manager = fake::memory(0x80);
    if (replace || !instance) instance = fake::memory(0x80);
    fake::put<uint64_t>(fake::main_image() + t::data::wwise_audio_manager, manager);
    fake::put<uint64_t>(fake::main_image() + t::data::wwise_parameter_manager, instance);
    fake::put<uint64_t>(manager + t::wwise_audio_queue::read, 0);
    fake::put<uint64_t>(manager + t::wwise_audio_queue::write, 0);
    fake::put<uint32_t>(manager + t::wwise_audio_queue::reservations, 0);
}
void stop_during_set() {
    const size_t queued = pending.size(); const unsigned reset_count = resets;
    mod_stop(stopping);
    assert(pending.size() == queued && resets == reset_count);
}
void stop_during_query() { mod_stop(stopping); }
void enqueue_after_ticket_capture() {
    // Model a producer interleaving with the native queue invocation. Its update
    // drains before return but belongs to a later ticket; retirement must wait.
    assert(audio_hold_parameter(ticket_owner, "Ticket", 0.9f) == TSUCHI_OK);
    assert(audio_release_parameter(ticket_owner, "Ticket") == TSUCHI_OK);
    consume();
}
void attempt_during_term() {
    assert(initialized && fake::get<uint64_t>(fake::main_image() + t::data::wwise_audio_manager) == manager);
    assert(fake::get<uint64_t>(fake::main_image() + t::data::wwise_parameter_manager) == instance);
    const unsigned before_queries = queries, before_sets = sets;
    float result = 777;
    assert(audio_parameter("Instance", &result) == TSUCHI_NOT_READY && result == 777);
    assert(audio_hold_parameter(term_attempt_owner, "Instance", 0.33f) == TSUCHI_NOT_READY);
    assert(!owned_count(term_attempt_owner) && queries == before_queries && sets == before_sets);
    term_attempted = true;
}
void replace_engine_and_hold() {
    assert(intercepted_term); intercepted_term();
    install_instance(true); initialized = true;
    replacement_id = declare(replacement_name.c_str(), 0.72f);
    replacement_owner = make_mod();
    assert(audio_hold_parameter(replacement_owner, replacement_name.c_str(), 0.28f) == TSUCHI_OK);
    replacement_created = true;
}
void become_unready() { initialized = false; }
void test_query_and_validation() {
    auto* owner = make_mod(); float result = 777;
    const uint32_t id = declare("Volume Test", 0.75f);
    assert(audio_parameter("vOlUmE tEsT", &result) == TSUCHI_OK && result == 0.75f);
    const unsigned before = sets;
    assert(audio_hold_parameter(owner, "Volume Test", 0.3f) == TSUCHI_NOT_READY && !owned_count(owner));
    sites_ready = true;
    result = 777; assert(audio_parameter("Missing", &result) == TSUCHI_NOT_FOUND && result == 777);
    assert(audio_hold_parameter(owner, "Missing", 0.3f) == TSUCHI_NOT_FOUND && sets == before && !owned_count(owner));
    assert(audio_parameter("", &result) == TSUCHI_INVALID);
    assert(audio_parameter("bad\nname", &result) == TSUCHI_INVALID);
    assert(audio_parameter("bad\x7fname", &result) == TSUCHI_INVALID);
    assert(audio_parameter("bad\xc3\xa9", &result) == TSUCHI_INVALID);
    const std::string too_long(128, 'p'), maximum(127, 'p');
    assert(audio_parameter(too_long.c_str(), &result) == TSUCHI_CAPACITY);
    declare(maximum.c_str(), 1);
    assert(audio_parameter(maximum.c_str(), &result) == TSUCHI_OK && result == 1);
    assert(audio_hold_parameter(owner, "Volume Test", std::numeric_limits<float>::infinity()) == TSUCHI_INVALID);
    assert(audio_hold_parameter(owner, "Volume Test", std::numeric_limits<float>::quiet_NaN()) == TSUCHI_INVALID);
    initialized = false; result = 777;
    assert(audio_parameter("Volume Test", &result) == TSUCHI_NOT_READY && result == 777);
    assert(audio_hold_parameter(owner, "Volume Test", 0.4f) == TSUCHI_NOT_READY);
    initialized = true;
    query_result_override = 2; result = 777;
    assert(audio_parameter("Volume Test", &result) == TSUCHI_NOT_READY && result == 777);
    query_result_override = 0; query_scope_override = 2;
    assert(audio_parameter("Volume Test", &result) == TSUCHI_NOT_READY && result == 777);
    query_scope_override = 4;
    assert(audio_parameter("Volume Test", &result) == TSUCHI_NOT_FOUND && result == 777);
    query_scope_override = UINT32_MAX; override_query_value = true;
    query_value = std::numeric_limits<float>::quiet_NaN();
    assert(audio_parameter("Volume Test", &result) == TSUCHI_NOT_READY && result == 777);
    query_value = std::numeric_limits<float>::infinity();
    assert(audio_parameter("Volume Test", &result) == TSUCHI_NOT_READY && result == 777);
    override_query_value = false;
    assert(current(id) == 0.75f && sets == before); mod_stop(owner);
}
void test_async_holds_and_restore() {
    auto* owner = make_mod(); auto* other = make_mod(); float result = 0;
    const uint32_t id = declare("Async", 0.25f);
    assert(audio_hold_parameter(owner, "Async", 0.5f) == TSUCHI_OK);
    assert(intercepted_queue && intercepted_term && owned_count(owner) == 1);
    assert(current(id) == 0.25f && pending.size() == 1);
    assert(audio_parameter("Async", &result) == TSUCHI_OK && result == 0.25f);
    const unsigned accepted = sets;
    for (unsigned i = 0; i < 3; ++i) step();
    assert(sets == accepted && pending.size() == 1); // No resubmission before a drain acknowledgement.
    assert(audio_hold_parameter(other, "ASYNC", 0.9f) == TSUCHI_BUSY);
    assert(audio_hold_parameter(owner, "ASYNC", 0.6f) == TSUCHI_OK && owned_count(owner) == 1);
    assert(current(id) == 0.25f);
    process_queue(); step(); assert(current(id) == 0.6f);
    const unsigned before_reset = resets;
    assert(audio_release_parameter(owner, "Async") == TSUCHI_OK && !owned_count(owner));
    assert(audio_hold_parameter(other, "Async", 0.9f) == TSUCHI_BUSY);
    settle(); assert(resets == before_reset + 1 && !parameters[id].global && current(id) == 0.25f);
    // An explicit global override equal to the default must remain an override.
    const uint32_t explicit_id = declare("Explicit", 0.8f, true, 0.8f);
    assert(audio_hold_parameter(owner, "Explicit", 0.3f) == TSUCHI_OK); settle();
    const unsigned explicit_resets = resets;
    assert(audio_release_parameter(owner, "Explicit") == TSUCHI_OK); settle();
    assert(resets == explicit_resets && parameters[explicit_id].global && current(explicit_id) == 0.8f);
    // A refused change cannot replace the last accepted desired value.
    assert(audio_hold_parameter(owner, "Async", 0.4f) == TSUCHI_OK); settle();
    refuse_value = true;
    assert(audio_hold_parameter(owner, "Async", 999) == TSUCHI_INVALID);
    refuse_value = false; settle(); assert(current(id) == 0.4f && owned_count(owner) == 1);
    assert(audio_release_parameter(owner, "Async") == TSUCHI_OK); settle();
    assert(!parameters[id].global); mod_stop(owner); mod_stop(other);
}
void test_foreign_writers() {
    auto* owner = make_mod(); auto* other = make_mod();
    const uint32_t id = declare("Foreign", 0.2f);
    assert(audio_hold_parameter(owner, "Foreign", 0.4f) == TSUCHI_OK); settle();
    // A frame observes a settled native writer and retains that newer baseline.
    parameters[id].global = true; parameters[id].value = 0.7f;
    const unsigned reassertions = sets;
    step(); assert(sets == reassertions + 1 && pending.size() == 1 && current(id) == 0.7f);
    for (unsigned i = 0; i < 3; ++i) step();
    assert(sets == reassertions + 1 && pending.size() == 1);
    settle(); assert(current(id) == 0.4f);
    assert(audio_release_parameter(owner, "Foreign") == TSUCHI_OK); settle();
    assert(parameters[id].global && current(id) == 0.7f);
    // A writer that changes the settled value immediately before release wins;
    // no frame reasserted our hold over it, so cleanup must leave it alone.
    assert(audio_hold_parameter(owner, "Foreign", 0.3f) == TSUCHI_OK); settle();
    parameters[id].value = 0.85f;
    const unsigned before_sets = sets, before_resets = resets;
    assert(audio_release_parameter(owner, "Foreign") == TSUCHI_OK);
    assert(sets == before_sets && resets == before_resets && pending.empty());
    assert(parameters[id].global && current(id) == 0.85f);
    assert(audio_hold_parameter(other, "Foreign", 0.6f) == TSUCHI_OK);
    mod_stop(owner); mod_stop(other); settle();
    assert(parameters[id].global && current(id) == 0.85f);
    // A native writer queued after our Set must win even when release happens
    // before either command is consumed. Cleanup waits for our accepted ticket
    // before deciding whether the eventual engine value is still ours.
    owner = make_mod(); other = make_mod();
    const uint32_t queued_id = declare("ForeignQueued", 0.1f);
    assert(audio_hold_parameter(owner, "ForeignQueued", 0.3f) == TSUCHI_OK);
    assert(set_parameter(queued_id, 0.8f, UINT64_MAX, 0, 4, false) == 1);
    const unsigned queued_resets = resets;
    assert(audio_release_parameter(owner, "ForeignQueued") == TSUCHI_OK);
    assert(resets == queued_resets && current(queued_id) == 0.1f);
    process_queue(); step();
    assert(parameters[queued_id].global && current(queued_id) == 0.8f && resets == queued_resets && pending.empty());
    assert(audio_hold_parameter(other, "ForeignQueued", 0.4f) == TSUCHI_OK);
    mod_stop(owner); mod_stop(other); settle();
    assert(parameters[queued_id].global && current(queued_id) == 0.8f);
}
void test_failed_acquisition_and_query_stop() {
    auto* owner = make_mod(); auto* other = make_mod();
    const uint32_t id = declare("FreshFailure", 0.45f);
    refuse_value = true;
    assert(audio_hold_parameter(owner, "FreshFailure", 999) == TSUCHI_INVALID && !owned_count(owner));
    refuse_value = false;
    assert(pending.empty() && !parameters[id].global);
    assert(audio_hold_parameter(other, "FreshFailure", 0.5f) == TSUCHI_OK);
    mod_stop(owner); mod_stop(other); settle(); assert(!parameters[id].global);
    owner = make_mod(); stopping = owner; other = make_mod();
    const uint32_t stopped_id = declare("QueryStop", 0.25f);
    const unsigned before_sets = sets, before_resets = resets;
    query_callback = stop_during_query;
    assert(audio_hold_parameter(owner, "QueryStop", 0.6f) == TSUCHI_NOT_READY);
    assert(!owned_count(owner) && sets == before_sets && resets == before_resets && pending.empty());
    assert(!parameters[stopped_id].global);
    assert(audio_hold_parameter(other, "QueryStop", 0.7f) == TSUCHI_OK);
    mod_stop(other); settle(); assert(!parameters[stopped_id].global);
}
void test_collisions_and_capacity() {
    auto* owner = make_mod(); auto* other = make_mod();
    const uint32_t collision = declare("Collision.A", 0.1f); names["collision.b"] = collision;
    assert(audio_hold_parameter(owner, "Collision.A", 0.2f) == TSUCHI_OK);
    assert(audio_hold_parameter(owner, "Collision.B", 0.3f) == TSUCHI_BUSY);
    assert(audio_hold_parameter(other, "Collision.B", 0.3f) == TSUCHI_BUSY);
    assert(audio_release_parameter(owner, "Collision.B") == TSUCHI_DENIED);
    assert(audio_release_parameter(owner, "Collision.A") == TSUCHI_OK); settle();
    std::vector<std::string> held;
    for (unsigned i = 0; i < 64; ++i) {
        const auto name = "Capacity." + std::to_string(i); declare(name.c_str(), 1);
        assert(audio_hold_parameter(owner, name.c_str(), 0.5f) == TSUCHI_OK); held.push_back(name);
    }
    declare("Capacity.Extra", 1);
    assert(audio_hold_parameter(other, "Capacity.Extra", 0.5f) == TSUCHI_CAPACITY);
    mod_stop(owner); assert(!owned_count(owner));
    assert(audio_hold_parameter(other, "Capacity.Extra", 0.5f) == TSUCHI_CAPACITY);
    settle(); assert(pending.empty());
    assert(audio_hold_parameter(other, "Capacity.Extra", 0.5f) == TSUCHI_OK);
    mod_stop(other); settle();
}
void test_cleanup_and_reentry() {
    auto* owner = make_mod(); stopping = owner;
    const uint32_t id = declare("Reentrant", 0.7f);
    set_callback = stop_during_set;
    assert(audio_hold_parameter(owner, "Reentrant", 0.2f) == TSUCHI_NOT_READY && !owned_count(owner));
    assert(current(id) == 0.7f); settle(); assert(!parameters[id].global && current(id) == 0.7f);
    owner = make_mod(); auto* other = make_mod();
    assert(audio_hold_parameter(owner, "Reentrant", 0.3f) == TSUCHI_OK);
    // Release before the write reaches the engine still cleans the eventual write.
    assert(audio_release_parameter(owner, "Reentrant") == TSUCHI_OK);
    assert(audio_hold_parameter(other, "Reentrant", 0.9f) == TSUCHI_BUSY);
    settle(); assert(!parameters[id].global);
    assert(audio_hold_parameter(owner, "Reentrant", 0.4f) == TSUCHI_OK); settle();
    initialized = false; const unsigned previous_resets = resets;
    mod_stop(owner); step(); assert(resets == previous_resets);
    initialized = true; settle(); assert(!parameters[id].global && resets == previous_resets + 1);
    // An empty queue with an unfinished producer reservation is not an acknowledgement.
    assert(audio_hold_parameter(other, "Reentrant", 0.5f) == TSUCHI_OK);
    fake::put<uint32_t>(manager + t::wwise_audio_queue::reservations, 1);
    process_queue();
    assert(audio_release_parameter(other, "Reentrant") == TSUCHI_OK); step();
    process_queue(); step();
    assert(audio_hold_parameter(other, "Reentrant", 0.9f) == TSUCHI_BUSY);
    fake::put<uint32_t>(manager + t::wwise_audio_queue::reservations, 0);
    settle(); assert(!parameters[id].global); mod_stop(other);
}
void test_pre_call_ticket_boundary() {
    ticket_owner = make_mod(); auto* other = make_mod();
    const uint32_t id = declare("Ticket", 0.15f);
    assert(audio_hold_parameter(ticket_owner, "Ticket", 0.3f) == TSUCHI_OK);
    after_drain_callback = enqueue_after_ticket_capture;
    process_queue(); step();
    assert(pending.empty() && parameters[id].global && current(id) == 0.9f);
    assert(audio_hold_parameter(other, "Ticket", 0.8f) == TSUCHI_BUSY);
    process_queue(); step();
    assert(pending.size() == 1 && parameters[id].global && current(id) == 0.9f);
    assert(audio_hold_parameter(other, "Ticket", 0.8f) == TSUCHI_BUSY);
    process_queue(); step();
    assert(pending.empty() && !parameters[id].global && current(id) == 0.15f);
    assert(audio_hold_parameter(other, "Ticket", 0.8f) == TSUCHI_OK);
    mod_stop(ticket_owner); mod_stop(other); settle();
}
void test_engine_identity() {
    auto* owner = make_mod(); const uint32_t id = declare("Instance", 0.2f);
    assert(audio_hold_parameter(owner, "Instance", 0.4f) == TSUCHI_OK); settle();
    initialized = false; mod_stop(owner); step();
    const unsigned previous_sets = sets, previous_resets = resets;
    pending.clear(); install_instance(true); parameters[id] = {0.9f, false, 0}; initialized = true;
    settle(); assert(sets == previous_sets && resets == previous_resets && current(id) == 0.9f);
    owner = make_mod(); assert(audio_hold_parameter(owner, "Instance", 0.5f) == TSUCHI_OK); settle();
    mod_stop(owner); settle(); assert(current(id) == 0.9f && !parameters[id].global);
    // Term changes identity even if both manager allocations are reused.
    owner = make_mod(); assert(audio_hold_parameter(owner, "Instance", 0.6f) == TSUCHI_OK);
    const uint64_t old_manager = manager, old_instance = instance;
    term_attempt_owner = make_mod(); term_callback = attempt_during_term;
    const unsigned term_count = terms; intercepted_term(); assert(terms == term_count + 1);
    assert(term_attempted && !owned_count(term_attempt_owner)); mod_stop(term_attempt_owner);
    assert(manager == old_manager && instance == old_instance);
    parameters[id] = {0.95f, true, 0.85f}; initialized = true;
    mod_stop(owner); const unsigned term_sets = sets, term_resets = resets;
    settle(); assert(sets == term_sets && resets == term_resets && parameters[id].global && current(id) == 0.85f);
}
void check_replacement_publication(TSUCHI_Mod* old_owner) {
    assert(replacement_created && replacement_owner && pending.size() == 1);
    const unsigned before_sets = sets, before_resets = resets;
    // Only callbacks from the replacement queue are needed to acknowledge its
    // write. The old native return must not republish the obsolete manager.
    process_queue(); step();
    assert(parameters[replacement_id].global && current(replacement_id) == 0.28f);
    assert(audio_release_parameter(replacement_owner, replacement_name.c_str()) == TSUCHI_OK);
    assert(resets == before_resets + 1 && sets == before_sets);
    process_queue(); step();
    assert(!parameters[replacement_id].global && current(replacement_id) == 0.72f);
    assert(pending.empty() && sets == before_sets && resets == before_resets + 1);
    auto* next_owner = make_mod();
    assert(audio_hold_parameter(next_owner, replacement_name.c_str(), 0.4f) == TSUCHI_OK);
    mod_stop(old_owner); mod_stop(replacement_owner); mod_stop(next_owner); settle();
    assert(!parameters[replacement_id].global && current(replacement_id) == 0.72f);
}
void test_publication_identity_during_native_calls() {
    auto* owner = make_mod(); declare("Publication.OldSet", 0.2f);
    replacement_name = "Publication.NewSet"; replacement_created = false;
    set_callback = replace_engine_and_hold;
    assert(audio_hold_parameter(owner, "Publication.OldSet", 0.3f) == TSUCHI_NOT_READY);
    check_replacement_publication(owner);
    owner = make_mod(); declare("Publication.OldReset", 0.4f);
    assert(audio_hold_parameter(owner, "Publication.OldReset", 0.5f) == TSUCHI_OK); settle();
    replacement_name = "Publication.NewReset"; replacement_created = false;
    reset_callback = replace_engine_and_hold;
    assert(audio_release_parameter(owner, "Publication.OldReset") == TSUCHI_OK);
    check_replacement_publication(owner);
    // IsInitialized may fall while a command accepted by this same instance is
    // still queued. Its journal must survive until readiness and cleanup return.
    owner = make_mod(); const uint32_t id = declare("Publication.Unready", 0.61f);
    const uint64_t saved_manager = manager, saved_instance = instance, saved_epoch = native_epoch;
    const unsigned before_resets = resets;
    set_callback = become_unready;
    assert(audio_hold_parameter(owner, "Publication.Unready", 0.12f) == TSUCHI_NOT_READY);
    assert(!initialized && manager == saved_manager && instance == saved_instance && native_epoch == saved_epoch);
    assert(pending.size() == 1 && !parameters[id].global && current(id) == 0.61f);
    mod_stop(owner); step(); assert(!owned_count(owner) && resets == before_resets);
    initialized = true; settle();
    assert(!parameters[id].global && current(id) == 0.61f && resets == before_resets + 1);
    auto* next_owner = make_mod();
    assert(audio_hold_parameter(next_owner, "Publication.Unready", 0.4f) == TSUCHI_OK);
    mod_stop(next_owner); settle(); assert(!parameters[id].global);
}
void test_restarted_mod_keeps_its_hold() {
    // A restart stops the old context and starts a new one with the same ID. No restore may be queued
    // before the new context reclaims the record in its first step handler.
    const uint32_t id = declare("Restarted", 0.4f);
    auto* first = mod_create("audio.restart.same", 0); assert(first);
    assert(audio_hold_parameter(first, "Restarted", 0.2f) == TSUCHI_OK);
    settle(); assert(parameters[id].global && current(id) == 0.2f);
    const uint32_t rank = mod_rank(first);
    mod_stop(first);
    // The audio thread may drain between stop and startup, and a mod that holds the parameter reclaims its
    // hold in its first on_step handler, after framework maintenance. Neither gap may submit the old baseline.
    process_queue(); assert(current(id) == 0.2f && pending.empty());
    auto* second = mod_create("audio.restart.same", rank); assert(second);
    step(); process_queue(); assert(current(id) == 0.2f && pending.empty());
    assert(audio_hold_parameter(second, "Restarted", 0.3f) == TSUCHI_OK && owned_count(second) == 1);
    settle(); assert(parameters[id].global && current(id) == 0.3f);
    // The same value again after a restart also keeps the original baseline for its eventual release.
    const uint32_t rank_two = mod_rank(second);
    mod_stop(second);
    auto* third = mod_create("audio.restart.same", rank_two); assert(third);
    assert(audio_hold_parameter(third, "Restarted", 0.3f) == TSUCHI_OK);
    settle(); assert(parameters[id].global && current(id) == 0.3f);
    // Its release still restores the value from before the first hold.
    assert(audio_release_parameter(third, "Restarted") == TSUCHI_OK); settle();
    assert(!parameters[id].global && current(id) == 0.4f);
    // Another mod still waits for a retiring hold it never owned.
    auto* owner = make_mod(); auto* other = make_mod();
    assert(audio_hold_parameter(owner, "Restarted", 0.6f) == TSUCHI_OK); settle();
    mod_stop(owner);
    assert(audio_hold_parameter(other, "Restarted", 0.7f) == TSUCHI_BUSY);
    settle(); assert(!parameters[id].global && current(id) == 0.4f);
    assert(audio_hold_parameter(other, "Restarted", 0.7f) == TSUCHI_OK); settle();
    assert(current(id) == 0.7f);
    mod_stop(other); mod_stop(third); settle(); assert(current(id) == 0.4f);
}
void test_public_service_composition() {
    auto* owner = make_mod(); audio_start();
    const uint32_t id = declare("Composition", 0.65f); float result = 777;
    assert(audio::table.parameter(owner, "Composition", &result) == TSUCHI_OK && result == 0.65f);
    assert(audio::table.hold_parameter(owner, "Composition", 0.25f) == TSUCHI_OK && current(id) == 0.65f);
    settle(); assert(current(id) == 0.25f);
    assert(audio::table.release_parameter(owner, "Composition") == TSUCHI_OK);
    settle(); assert(current(id) == 0.65f && !parameters[id].global);
    const unsigned before = actor_sounds;
    assert(audio::table.play_on(owner, {0x1234}, "Whistle_Charge") == TSUCHI_OK && actor_sounds == before + 1);
    assert(audio::table.play_on(owner, {0x5678}, "Whistle_Charge") == TSUCHI_NOT_FOUND && actor_sounds == before + 1);
    fake::put<uint8_t>(sound_system + t::sound_system::ready, 0);
    assert(audio::table.play_on(owner, {0x1234}, "Whistle_Charge") == TSUCHI_NOT_READY && actor_sounds == before + 1);
    fake::put<uint8_t>(sound_system + t::sound_system::ready, 1);
    assert(audio::table.play_on(owner, {0x1234}, "MissingSound") == TSUCHI_NOT_FOUND && actor_sounds == before + 2);
    assert(audio::table.play_on(owner, {0x1234}, "") == TSUCHI_INVALID && actor_sounds == before + 2);
    mod_stop(owner);
}

// Bus callbacks exercise the shipped backend and real ownership/Term observers.
// The made-up table changes every field offset from the original executables.
uint64_t bus_manager = 0, bus_entries = 0;
unsigned main_locks = 0, bus_locks = 0, registrations = 0;
uint32_t register_result = 1;
using BusCallback = void (*)(void*);
using BusPoint = audio_bus_binding::Point;
struct NativeCurve { uint32_t id, parameter; BusPoint points[4]; };
struct NativeBus {
    uint64_t node = 0, subscription = 0, entries = 0, graphs[4]{};
    unsigned references = 0;
    std::vector<NativeCurve> curves;
};
std::map<uint32_t, NativeBus> native_buses;
uint32_t bind_result = 1, missing_bus = 0;
bool partial_bind = false;
unsigned bindings = 0, unbindings = 0;
NativeBus& bus_from_node(uint64_t node) {
    for (auto& entry : native_buses) if (entry.second.node == node) return entry.second;
    assert(false); return native_buses.begin()->second;
}
void sync_curves(NativeBus& bus) {
    assert(bus.curves.size() <= 4);
    fake::put<uint64_t>(bus.subscription + t::wwise_bus_subscription::curves, bus.entries);
    fake::put<uint32_t>(bus.subscription + t::wwise_bus_subscription::count, uint32_t(bus.curves.size()));
    for (size_t i = 0; i < bus.curves.size(); ++i) {
        const uint64_t entry = bus.entries + i * t::wwise_bus_curve::size;
        const auto& curve = bus.curves[i];
        fake::put<uint32_t>(entry + t::wwise_bus_curve::id, curve.id);
        fake::put<uint32_t>(entry + t::wwise_bus_curve::parameter, curve.parameter);
        fake::put<uint64_t>(entry + t::wwise_bus_curve::points, bus.graphs[i]);
        fake::put<uint32_t>(entry + t::wwise_bus_curve::point_count, 4);
        fake::put<uint32_t>(entry + t::wwise_bus_curve::scaling, 0);
        std::memcpy(reinterpret_cast<void*>(bus.graphs[i]), curve.points, sizeof curve.points);
    }
}
uint64_t native_get_bus(uint64_t index, uint32_t id, uint32_t type) {
    assert(main_locks && bus_locks && index == 0x12345678 && type == 1);
    if (id == missing_bus) return 0;
    auto& bus = native_buses[id];
    if (!bus.node) {
        bus.node = fake::memory(0x100); bus.subscription = fake::memory(0x100);
        bus.entries = fake::memory(4 * t::wwise_bus_curve::size);
        for (auto& graph : bus.graphs) graph = fake::memory(4 * sizeof(BusPoint));
        sync_curves(bus);
    }
    ++bus.references; return bus.node;
}
int32_t native_release_bus(uint64_t node) {
    auto& bus = bus_from_node(node); assert(main_locks && bus.references); return int32_t(--bus.references);
}
uint64_t native_subscription(uint64_t rtpc, uint64_t subscriber, uint32_t property) {
    assert(main_locks && rtpc == instance && property == 5);
    auto& bus = bus_from_node(subscriber - t::wwise_bus_node::subscriber);
    return bus.curves.empty() ? 0 : bus.subscription;
}
uint32_t native_bind_curve(uint64_t node, uint32_t parameter, uint32_t type, uint32_t accumulation,
                           uint32_t property, uint32_t curve, uint32_t scaling, void* data, uint32_t count, bool notify) {
    assert(main_locks && bus_locks && type == 0 && accumulation == 2 && property == 5 && scaling == 0 && count == 2 && notify);
    assert(curve == parameter); ++bindings;
    const auto* points = static_cast<BusPoint*>(data);
    assert(points[0].x == 0 && points[1].x == 1 && points[0].y == 0 && points[1].y == 0);
    auto& bus = bus_from_node(node);
    if (bind_result == 1 || partial_bind) {
        for (const auto& old : bus.curves) assert(old.id != curve);
        bus.curves.push_back({curve, parameter, {{-FLT_MAX,0,9},points[0],points[1],{FLT_MAX,0,9}}});
        sync_curves(bus);
    }
    return bind_result;
}
void native_unbind_curve(uint64_t node, uint32_t property, uint32_t curve) {
    assert(main_locks && bus_locks && property == 5); ++unbindings;
    auto& bus = bus_from_node(node);
    for (size_t i = 0; i < bus.curves.size(); ++i) if (bus.curves[i].id == curve) {
        bus.curves.erase(bus.curves.begin() + i); sync_curves(bus); return;
    }
    assert(false);
}
uint32_t bus_count() { return fake::get<uint32_t>(bus_manager + t::wwise_bus_callbacks::count); }
uint64_t bus_entry(uint32_t at) { return bus_entries + uint64_t(at) * t::wwise_bus_callback_entry::size; }
BusCallback registered(uint32_t bus) {
    for (uint32_t i = 0; i < bus_count(); ++i)
        if (fake::get<uint32_t>(bus_entry(i) + t::wwise_bus_callback_entry::bus) == bus)
            return reinterpret_cast<BusCallback>(fake::get<uint64_t>(bus_entry(i) + t::wwise_bus_callback_entry::callback));
    return nullptr;
}
void bus_lock(void* pointer) {
    const auto address = reinterpret_cast<uint64_t>(pointer);
    if (address == fake::main_image() + t::data::wwise_main_lock) { assert(!bus_locks || main_locks); ++main_locks; }
    else { assert(address == bus_manager + t::wwise_bus_callbacks::mutex && main_locks); ++bus_locks; }
}
void bus_unlock(void* pointer) {
    if (reinterpret_cast<uint64_t>(pointer) == fake::main_image() + t::data::wwise_main_lock) { assert(main_locks && !bus_locks); --main_locks; }
    else { assert(bus_locks && main_locks); --bus_locks; }
}
uint32_t register_bus(uint32_t bus, void* function) {
    assert(main_locks && bus_locks); ++registrations;
    bus_lock(reinterpret_cast<void*>(bus_manager + t::wwise_bus_callbacks::mutex)); // recursive native setter
    if (register_result != 1) { bus_unlock(reinterpret_cast<void*>(bus_manager + t::wwise_bus_callbacks::mutex)); return register_result; }
    uint32_t count = bus_count(), at = count;
    for (uint32_t i = 0; i < count; ++i)
        if (fake::get<uint32_t>(bus_entry(i) + t::wwise_bus_callback_entry::bus) == bus) at = i;
    if (!function && at < count) {
        --count;
        if (at != count) std::memcpy(reinterpret_cast<void*>(bus_entry(at)), reinterpret_cast<void*>(bus_entry(count)), t::wwise_bus_callback_entry::size);
    } else if (function) {
        assert(at < 128);
        if (at == count) ++count;
        fake::put<uint32_t>(bus_entry(at) + t::wwise_bus_callback_entry::bus, bus);
        fake::put<uint64_t>(bus_entry(at) + t::wwise_bus_callback_entry::callback, reinterpret_cast<uint64_t>(function));
    }
    fake::put<uint32_t>(bus_manager + t::wwise_bus_callbacks::count, count);
    bus_unlock(reinterpret_cast<void*>(bus_manager + t::wwise_bus_callbacks::mutex));
    return 1;
}
float outgoing(uint32_t bus, float input) {
    const auto function = registered(bus);
    if (!function) return input;
    // Native Wwise flattens a bus with no BusVolume binding. Registering a
    // callback alone then succeeds but does not change its audible output.
    if (native_buses[bus].curves.empty()) return input;
    unsigned char info[128]{};
    auto* value = &input;
    std::memcpy(info + t::wwise_volume_callback::base_volume, &value, sizeof value);
    function(info);
    return input;
}
void foreign_bus_callback(void*) {}
void replace_native_bus(uint32_t bus) {
    bus_lock(reinterpret_cast<void*>(fake::main_image() + t::data::wwise_main_lock));
    bus_lock(reinterpret_cast<void*>(bus_manager + t::wwise_bus_callbacks::mutex));
    assert(register_bus(bus, reinterpret_cast<void*>(foreign_bus_callback)) == 1);
    bus_unlock(reinterpret_cast<void*>(bus_manager + t::wwise_bus_callbacks::mutex));
    bus_unlock(reinterpret_cast<void*>(fake::main_image() + t::data::wwise_main_lock));
}
void test_bus_volume() {
    bus_manager = fake::memory(0x100);
    bus_entries = fake::memory(128 * t::wwise_bus_callback_entry::size);
    fake::put<uint64_t>(fake::main_image() + t::data::wwise_bus_callback_manager, bus_manager);
    fake::put<uint64_t>(bus_manager + t::wwise_bus_callbacks::entries, bus_entries);
    t::fn::lock_mutex = bus_lock; t::fn::unlock_mutex = bus_unlock;
    t::fn::wwise_register_bus_volume_callback = register_bus;
    fake::put<uint64_t>(fake::main_image() + t::data::wwise_index, 0x12345678);
    t::fn::wwise_get_bus = native_get_bus;
    t::fn::wwise_release_bus = native_release_bus;
    t::fn::wwise_bus_subscription = native_subscription;
    t::fn::wwise_bind_bus_curve = native_bind_curve;
    t::fn::wwise_unbind_bus_curve = native_unbind_curve;
    audio_bus_volumes_start(framework);
    auto* owner = make_mod(); auto* other = make_mod();
    assert(audio::table.hold_bus_volume(owner, 0, 1) == TSUCHI_INVALID);
    for (float value : {-0.1f, 1.1f, std::numeric_limits<float>::infinity(), std::numeric_limits<float>::quiet_NaN()})
        assert(audio::table.hold_bus_volume(owner, 11, value) == TSUCHI_INVALID);
    initialized = false;
    assert(audio::table.hold_bus_volume(owner, 11, 0.5f) == TSUCHI_NOT_READY);
    initialized = true;
    missing_bus = 11;
    assert(audio::table.hold_bus_volume(owner, 11, 0.5f) == TSUCHI_NOT_FOUND && !owned_count(owner));
    missing_bus = 0;
    bind_result = 52; partial_bind = true;
    assert(audio::table.hold_bus_volume(owner, 11, 0.5f) == TSUCHI_NO_MEMORY && !owned_count(owner));
    assert(native_buses[11].curves.empty());
    bind_result = 1; partial_bind = false;
    register_result = 52;
    assert(audio::table.hold_bus_volume(owner, 11, 0.5f) == TSUCHI_NO_MEMORY && !owned_count(owner));
    register_result = 1;
    assert(audio::table.hold_bus_volume(owner, 11, 0.5f) == TSUCHI_OK);
    assert(owned_count(owner) == 1 && outgoing(11, 0.8f) == 0.4f && outgoing(12, 0.8f) == 0.8f);
    assert(audio::table.hold_bus_volume(other, 11, 0.8f) == TSUCHI_BUSY);
    const unsigned before = registrations;
    for (unsigned i = 0; i < 20; ++i) step();
    assert(registrations == before); // Successful callbacks stay installed without native rewrites.
    assert(audio::table.hold_bus_volume(owner, 11, 0) == TSUCHI_OK && outgoing(11, 0.8f) == 0);
    assert(audio::table.hold_bus_volume(owner, 11, 1) == TSUCHI_OK && outgoing(11, 0.8f) == 0.8f);
    assert(registrations == before);
    assert(audio::table.release_bus_volume(other, 11) == TSUCHI_NOT_FOUND);
    assert(audio::table.release_bus_volume(owner, 11) == TSUCHI_OK && !registered(11) && !owned_count(owner));
    assert(native_buses[11].curves.empty());

    const uint32_t marker = hash_name("tsuchi_bus_volume_neutral");
    auto foreign_marker = [&] (uint32_t bus) {
        const uint64_t node = native_buses[bus].node;
        assert(node);
        native_buses[bus].curves = {{marker, marker + 1, {{-FLT_MAX,0,9},{0,0,4},{1,0,4},{FLT_MAX,0,9}}}};
        sync_curves(native_buses[bus]);
    };
    foreign_marker(11);
    assert(audio::table.hold_bus_volume(owner, 11, 0.5f) == TSUCHI_BUSY);
    assert(native_buses[11].curves.size() == 1 && !registered(11));
    native_buses[11].curves.clear(); sync_curves(native_buses[11]);
    // An unrelated stock/native curve is retained byte-for-byte through a hold.
    const NativeCurve unrelated{marker + 3, marker + 4, {{-FLT_MAX,0,9},{0,0,4},{1,0,4},{FLT_MAX,0,9}}};
    native_buses[11].curves.push_back(unrelated); sync_curves(native_buses[11]);
    assert(audio::table.hold_bus_volume(owner, 11, 0.5f) == TSUCHI_OK && native_buses[11].curves.size() == 2);
    assert(audio::table.release_bus_volume(owner, 11) == TSUCHI_OK);
    assert(native_buses[11].curves.size() == 1 && std::memcmp(&native_buses[11].curves[0], &unrelated, sizeof unrelated) == 0);
    native_buses[11].curves.clear(); sync_curves(native_buses[11]);
    assert(audio::table.hold_bus_volume(owner, 11, 0.5f) == TSUCHI_OK);
    foreign_marker(11); // A later native writer using the marker must also survive cleanup.
    assert(audio::table.release_bus_volume(owner, 11) == TSUCHI_OK);
    assert(native_buses[11].curves.size() == 1 && native_buses[11].curves[0].parameter == marker + 1);
    native_buses[11].curves.clear(); sync_curves(native_buses[11]);
    assert(audio::table.hold_bus_volume(owner, 17, 0.5f) == TSUCHI_OK);
    native_buses[17].curves.clear(); sync_curves(native_buses[17]); // unload/reload with the same node address
    step(); assert(outgoing(17, 0.8f) == 0.4f && native_buses[17].curves.size() == 1);
    assert(audio::table.release_bus_volume(owner, 17) == TSUCHI_OK && native_buses[17].curves.empty());

    replace_native_bus(12);
    assert(audio::table.hold_bus_volume(owner, 12, 0.5f) == TSUCHI_BUSY && registered(12) == foreign_bus_callback);
    assert(audio::table.hold_bus_volume(owner, 13, 0.25f) == TSUCHI_OK);
    replace_native_bus(13); // A writer replacing our callback must survive release and stop.
    assert(audio::table.release_bus_volume(owner, 13) == TSUCHI_OK && registered(13) == foreign_bus_callback);

    assert(audio::table.hold_bus_volume(owner, 14, 0) == TSUCHI_OK);
    const auto paused_callback = registered(14);
    initialized = false; mod_stop(owner);
    assert(registered(14) == paused_callback && outgoing(14, 0.8f) == 0.8f);
    initialized = true; step(); assert(!registered(14));

    assert(audio::table.hold_bus_volume(other, 15, 0) == TSUCHI_OK);
    assert(intercepted_term); intercepted_term();
    // The replacement deliberately reuses every native address. Its foreign callback stays.
    fake::put<uint32_t>(bus_manager + t::wwise_bus_callbacks::count, 0);
    for (auto& bus : native_buses) { bus.second.curves.clear(); sync_curves(bus.second); }
    initialized = true; replace_native_bus(15); step();
    assert(registered(15) == foreign_bus_callback && !owned_count(other));
    assert(audio::table.hold_bus_volume(other, 15, 0.5f) == TSUCHI_BUSY);
    for (uint32_t bus = 100; bus < 164; ++bus)
        assert(audio::table.hold_bus_volume(other, bus, 0.5f) == TSUCHI_OK);
    assert(audio::table.hold_bus_volume(other, 164, 0.5f) == TSUCHI_CAPACITY);
    mod_stop(other);
    assert(bus_count() == 1 && registered(15) == foreign_bus_callback && !main_locks && !bus_locks);
    for (const auto& bus : native_buses) assert(bus.second.references == 0);
}
} // namespace

namespace tsuchi {
bool each_frame(void (*work)()) { steps.push_back(work); return true; }
bool code_site_patched(uint32_t site) {
    assert(site == t::code_site::wwise_process_queue.number || site == t::code_site::wwise_term.number);
    return sites_ready;
}
TSUCHI_Result hook_code_site(TSUCHI_Mod* owner, uint32_t site, void* function, uint64_t* id, void** original) {
    assert(owner == framework);
    if (!sites_ready) return TSUCHI_NOT_READY;
    if (site == t::code_site::wwise_process_queue.number) {
        intercepted_queue = reinterpret_cast<decltype(intercepted_queue)>(function);
        *original = reinterpret_cast<void*>(native_queue); *id = 1;
    } else {
        assert(site == t::code_site::wwise_term.number);
        intercepted_term = reinterpret_cast<decltype(intercepted_term)>(function);
        *original = reinterpret_cast<void*>(native_term); *id = 2;
    }
    return TSUCHI_OK;
}
TSUCHI_Result hook_remove(TSUCHI_Mod* owner, uint64_t id) {
    assert(owner == framework && (id == 1 || id == 2)); return TSUCHI_OK;
}
} // namespace tsuchi
namespace tsuchi::game {
uint64_t from_handle(uint64_t handle) { return handle; }
bool is_game_actor(uint64_t actor) { return actor == 0x1234; }
TSUCHI_Result name_from_utf8(const char* text, Name& out) {
    if (!text || !*text) return TSUCHI_INVALID;
    if (!std::strcmp(text, "Whistle_Charge")) { out = {1, 0}; return TSUCHI_OK; }
    if (!std::strcmp(text, "MissingSound")) { out = {2, 0}; return TSUCHI_OK; }
    return TSUCHI_INVALID;
}
} // namespace tsuchi::game

int main() {
    fake::start(); set_thread_probe([] { return thread; }); note_game_thread();
    install_instance(true);
    sound_system = fake::memory(0x20);
    fake::put<uint64_t>(fake::main_image() + t::data::sound_system, sound_system);
    fake::put<uint8_t>(sound_system + t::sound_system::started, 1);
    fake::put<uint8_t>(sound_system + t::sound_system::ready, 1);
    t::fn::wwise_initialized = [] { return initialized; };
    t::fn::wwise_parameter_id = hash_name; t::fn::wwise_get_parameter = query;
    t::fn::wwise_set_parameter = set_parameter; t::fn::wwise_reset_parameter = reset_parameter;
    t::fn::play_sound_on = play_actor_sound;
    framework = mod_create("audio.parameters.framework"); assert(framework); audio_parameters_start(framework);
    test_query_and_validation();
    test_async_holds_and_restore();
    test_foreign_writers();
    test_failed_acquisition_and_query_stop();
    test_collisions_and_capacity();
    test_cleanup_and_reentry();
    test_pre_call_ticket_boundary();
    test_engine_identity();
    test_publication_identity_during_native_calls();
    test_public_service_composition();
    test_restarted_mod_keeps_its_hold();
    test_bus_volume();
    settle(); assert(pending.empty()); mod_stop(framework);
    std::puts("audio parameters: asynchronous acceptance, ownership, restoration, drain and engine identity passed");
}
