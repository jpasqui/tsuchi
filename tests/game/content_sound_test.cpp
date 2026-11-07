#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unordered_set>
#include <unordered_map>
#include <vector>
#include "actor_events.h"
#include "content_sound.h"
#include "content_sound_format.h"
#include "content_sound_fixture.h"
#include "frame.h"
#include "game.h"
#include "holds.h"
#include "objects.h"
#include "ordinary_lifecycle.h"
#include "parts.h"
#include "tsuchi/file.h"

using namespace tsuchi;
namespace {
int failures = 0, checks = 0;
#define CHECK(x) do { ++checks; if (!(x)) { ++failures; std::printf("line %d: %s\n", __LINE__, #x); } } while (0)
std::vector<void(*)()> steps;
std::unordered_set<uint32_t> native_banks, native_voices;
std::unordered_map<uint32_t, uint32_t> voice_banks;
bool sound_ready = true, load_refused = false, unload_refused = false, play_refused = false, stop_refused = false;
bool fail_allocate = false;
size_t allocation_limit = size_t(-1);
uint32_t file_sizes=0,file_reads=0;
TSUCHI_Result file_result=TSUCHI_OK;
bool corrupt_file=false;
uint32_t loads = 0, unloads = 0, posts = 0, stops = 0, next_voice = 1;
uint64_t thread = 1;
TSUCHI_Mod* stopping = nullptr;
enum class Callback { none, load, play, stop, unload, unload_during_play, file_size, file_read };
Callback callback = Callback::none;
uint64_t callback_bank = 0;
TSUCHI_Result callback_result = TSUCHI_OK;
void interrupted(Callback at) { if (callback == at) { callback = Callback::none; mod_stop(stopping); } }
TSUCHI_Result file_size(TSUCHI_Mod*,const char* path,uint64_t* out) {
    ++file_sizes;CHECK(!strcmp(path,"tone.bnk"));*out=sizeof large_fixture;
    interrupted(Callback::file_size);return file_result;
}
TSUCHI_Result file_read(TSUCHI_Mod*,const char* path,uint64_t offset,void* data,uint64_t room,uint64_t* out) {
    ++file_reads;CHECK(!strcmp(path,"tone.bnk")&&offset==0&&room==sizeof large_fixture);
    memcpy(data,large_fixture,sizeof large_fixture);if(corrupt_file)static_cast<uint8_t*>(data)[0]=0;
    *out=sizeof large_fixture;interrupted(Callback::file_read);return file_result;
}
const TSUCHI_FileService files=[] {TSUCHI_FileService value{};value.table_size=sizeof value;value.size=file_size;value.read=file_read;return value;}();
bool ready() { return sound_ready; }
TSUCHI_Result load(const void* bytes, uint32_t count, uint32_t* out) {
    ++loads; *out = 0;
    if (load_refused) return TSUCHI_UNSUPPORTED;
    sound_bank_format::Bank parsed{};
    CHECK(sound_bank_format::parse(bytes, count, parsed));
    *out = parsed.bank;
    CHECK(native_banks.insert(*out).second);
    interrupted(Callback::load);
    return TSUCHI_OK;
}
TSUCHI_Result unload(uint32_t id) {
    ++unloads;
    if (unload_refused) return TSUCHI_UNSUPPORTED;
    for (uint32_t voice : native_voices) CHECK(voice_banks[voice] != id);
    CHECK(native_banks.erase(id) == 1);
    interrupted(Callback::unload);
    return TSUCHI_OK;
}
TSUCHI_Result play(uint32_t id, const TSUCHI_Vec3& point, uint64_t emitter, uint32_t* out) {
    ++posts; *out = 0; CHECK(emitter != 0); CHECK(finite(&point)); CHECK(id != 0);
    if (play_refused) return TSUCHI_NOT_FOUND;
    *out = next_voice++;
    native_voices.insert(*out);
    voice_banks[*out] = id - 4;
    if (callback == Callback::unload_during_play) {
        callback = Callback::none;
        const uint32_t prior_stops = stops, prior_unloads = unloads;
        callback_result = content_sound_unload(stopping, callback_bank);
        CHECK(callback_result == TSUCHI_BUSY && stops == prior_stops && unloads == prior_unloads);
    }
    interrupted(Callback::play);
    return TSUCHI_OK;
}
bool active(uint32_t id) { return native_voices.count(id) != 0; }
TSUCHI_Result stop(uint32_t id) {
    ++stops;
    if (stop_refused) return TSUCHI_UNSUPPORTED;
    CHECK(native_voices.erase(id) == 1);
    voice_banks.erase(id);
    interrupted(Callback::stop);
    return TSUCHI_OK;
}
const ContentSoundBackend backend{ready, load, unload, play, active, stop};
void step() { for (auto fn : steps) fn(); }
std::vector<uint8_t> remap(uint32_t seed) {
    std::vector<uint8_t> result(fixture, fixture + sizeof fixture);
    for (size_t i = 0; i + 4 <= result.size(); ++i) {
        const uint32_t value = sound_bank_format::u32(result.data() + i);
        if (value >= 1000 && value <= 1004) {
            const uint32_t mapped = value - 1000 + seed;
            for (uint32_t j = 0; j < 4; ++j) result[i + j] = uint8_t(mapped >> (j * 8));
            i += 3;
        }
    }
    return result;
}
}
namespace tsuchi {
bool each_frame(void (*fn)()) { steps.push_back(fn); return true; }
bool on_actor_left(void (*)(uint64_t, uint64_t, ActorLeft)) { return true; }
void ordinary_lifecycle_listener(void (*)(uint64_t, uint64_t, bool)) {}
TSUCHI_Result ordinary_lifecycle_watch(uint64_t) { return TSUCHI_OK; }
uint64_t ordinary_lifecycle_restore_part(uint64_t value, uint64_t) { return value; }
}
namespace tsuchi::game {
uint64_t from_handle(uint64_t value) { return value; }
uint64_t to_handle(uint64_t value) { return value; }
uint64_t part_owner(uint64_t) { return 1; }
bool is_game_actor(uint64_t) { return false; }
}
int main() {
    content_sound_set_backend(&backend);
    set_thread_probe([] { return thread; }); note_game_thread();
    auto* mod = mod_create("sound.test");
    auto* other = mod_create("sound.other");
    set_allocator({[](size_t bytes) -> void* { return fail_allocate || bytes>allocation_limit ? nullptr : std::malloc(bytes); }, [](void* bytes) { std::free(bytes); }});
    uint64_t bank = 0, voice = 0, size = 0;
    const TSUCHI_Vec3 point{1, 2, 3};
    sound_bank_format::Bank parsed{};
    CHECK(sound_bank_format::parse(fixture, sizeof fixture, parsed));
    for (uint32_t n = 0; n < sizeof fixture; ++n) CHECK(!sound_bank_format::parse(fixture, n, parsed));
    auto damaged = remap(1000);
    for (size_t i = 0; i + 10 < damaged.size(); ++i) if (sound_bank_format::tag(damaged.data() + i, "fmt ")) { damaged[i + 8] = 1; damaged[i + 9] = 0; break; }
    CHECK(content_sound_load(mod, "sound.test:bad", damaged.data(), uint32_t(damaged.size()), &bank) == TSUCHI_INVALID && loads == 0);
    CHECK(content_sound_load(mod, "sound.other:bad", fixture, sizeof fixture, &bank) == TSUCHI_INVALID);
    CHECK(content_sound_load(mod, "sound.test:bad", nullptr, sizeof fixture, &bank) == TSUCHI_INVALID);
    CHECK(content_sound_load(mod, "sound.test:bad", fixture, 17u << 20, &bank) == TSUCHI_INVALID);
    fail_allocate = true;
    CHECK(content_sound_load(mod, "sound.test:bad", fixture, sizeof fixture, &bank) == TSUCHI_NO_MEMORY && loads == 0);
    fail_allocate = false;
    sound_ready = false;
    CHECK(content_sound_load(mod, "sound.test:tone", fixture, sizeof fixture, &bank) == TSUCHI_NOT_READY);
    sound_ready = true; load_refused = true;
    CHECK(content_sound_load(mod, "sound.test:tone", fixture, sizeof fixture, &bank) == TSUCHI_UNSUPPORTED && !bank && owned_count(mod) == 0);
    load_refused = false;
    CHECK(content_sound_load(mod, "sound.test:tone", fixture, sizeof fixture, &bank) == TSUCHI_OK && bank);
    const uint64_t original = bank;
    uint64_t found = 999;
    CHECK(content_sound_find(mod, "sound.test:tone", &found) == TSUCHI_OK && found == bank);
    CHECK(content_sound_find(other, "sound.test:tone", &found) == TSUCHI_DENIED && !found);
    CHECK(content_sound_find(mod, "sound.test:absent", &found) == TSUCHI_NOT_FOUND && !found);
    CHECK(content_sound_find(mod, "bad bank", &found) == TSUCHI_INVALID && !found);
    CHECK(content_sound_find(mod, nullptr, &found) == TSUCHI_INVALID && !found);
    CHECK(content_sound_find(mod, "sound.test:tone", nullptr) == TSUCHI_INVALID);
    CHECK(content_sound_load(other, "sound.other:tone", fixture, sizeof fixture, &voice) == TSUCHI_BUSY);
    CHECK(content_sound_events(other, bank, nullptr, 0, &size) == TSUCHI_DENIED);
    CHECK(content_sound_events(mod, bank, nullptr, 0, &size) == TSUCHI_CAPACITY && size == 7);
    char events[16] = "unchanged";
    CHECK(content_sound_events(mod, bank, events, 5, &size) == TSUCHI_CAPACITY && strcmp(events, "unchanged") == 0);
    CHECK(content_sound_events(mod, bank, events, sizeof events, &size) == TSUCHI_OK && strcmp(events, "#1004") == 0 && events[6] == 0);
    CHECK(content_sound_play(other, bank, "#1004", point, &voice) == TSUCHI_DENIED);
    CHECK(content_sound_play(mod, bank, "#1005", point, &voice) == TSUCHI_NOT_FOUND);
    CHECK(content_sound_play(mod, bank, "1004", point, &voice) == TSUCHI_NOT_FOUND);
    CHECK(content_sound_play(mod, bank, "#01004", point, &voice) == TSUCHI_INVALID);
    thread = 2;
    CHECK(content_sound_find(mod, "sound.test:tone", &found) == TSUCHI_INVALID && !found);
    CHECK(content_sound_load(mod, "sound.test:tone", fixture, sizeof fixture, &voice) == TSUCHI_INVALID);
    CHECK(content_sound_events(mod, bank, events, sizeof events, &size) == TSUCHI_INVALID);
    CHECK(content_sound_play(mod, bank, "#1004", point, &voice) == TSUCHI_INVALID);
    CHECK(content_sound_stop(mod, 1) == TSUCHI_INVALID);
    CHECK(content_sound_unload(mod, bank) == TSUCHI_INVALID);
    thread = 1;
    play_refused = true;
    CHECK(content_sound_play(mod, bank, "#1004", point, &voice) == TSUCHI_NOT_FOUND && !voice && owned_count(mod) == 1);
    play_refused = false;
    CHECK(content_sound_play(mod, bank, "#1004", point, &voice) == TSUCHI_OK && voice);
    CHECK(content_sound_stop(other, voice) == TSUCHI_DENIED);
    CHECK(content_sound_unload(other, bank) == TSUCHI_DENIED);
    stop_refused = true;
    CHECK(content_sound_stop(mod, voice) == TSUCHI_UNSUPPORTED);
    stop_refused = false;
    CHECK(content_sound_stop(mod, voice) == TSUCHI_OK);
    CHECK(content_sound_stop(mod, voice) == TSUCHI_NOT_FOUND);
    CHECK(content_sound_play(mod, bank, "#1004", point, &voice) == TSUCHI_OK);
    native_voices.clear(); step();
    CHECK(content_sound_stop(mod, voice) == TSUCHI_NOT_FOUND && owned_count(mod) == 1);
    for (uint32_t i = 0; i < 128; ++i) CHECK(content_sound_play(mod, bank, "#1004", point, &voice) == TSUCHI_OK);
    const uint64_t full_voice = voice;
    CHECK(content_sound_play(mod, bank, "#1004", point, &voice) == TSUCHI_CAPACITY && !voice);
    fail_allocate = true;
    const uint32_t stops_before_refusal = stops, unloads_before_refusal = unloads;
    CHECK(content_sound_unload(mod, bank) == TSUCHI_NO_MEMORY);
    CHECK(stops == stops_before_refusal && unloads == unloads_before_refusal && native_voices.size() == 128 && native_banks.size() == 1);
    CHECK(owned_count(mod) == 129 && content_sound_find(mod, "sound.test:tone", &found) == TSUCHI_OK && found == bank);
    fail_allocate = false;
    unload_refused = true;
    CHECK(content_sound_unload(mod, bank) == TSUCHI_OK && native_voices.empty() && native_banks.size() == 1 && owned_count(mod) == 0);
    CHECK(content_sound_stop(mod, full_voice) == TSUCHI_NOT_FOUND);
    CHECK(content_sound_unload(mod, bank) == TSUCHI_NOT_FOUND);
    CHECK(content_sound_find(mod, "sound.test:tone", &found) == TSUCHI_NOT_FOUND && !found);
    CHECK(content_sound_load(other, "sound.other:queued", fixture, sizeof fixture, &voice) == TSUCHI_BUSY);
    step(); CHECK(native_banks.size() == 1 && unloads > unloads_before_refusal);
    unload_refused = false;
    step(); CHECK(native_banks.empty());
    CHECK(content_sound_find(mod, "sound.test:tone", &found) == TSUCHI_NOT_FOUND && !found);
    CHECK(content_sound_unload(mod, original) == TSUCHI_NOT_FOUND);
    for (uint32_t i = 0; i < 16; ++i) {
        const auto bytes = remap(1000 + 100 * i);
        char id[96]; std::snprintf(id, sizeof id, "sound.test:bank%u", i);
        CHECK(content_sound_load(mod, id, bytes.data(), uint32_t(bytes.size()), &bank) == TSUCHI_OK);
    }
    const auto seventeenth = remap(3000);
    CHECK(content_sound_load(mod, "sound.test:extra", seventeenth.data(), uint32_t(seventeenth.size()), &voice) == TSUCHI_CAPACITY);
    CHECK(content_sound_play(mod, bank, "#2504", point, &voice) == TSUCHI_OK);
    const uint32_t full_stops_before = stops, full_unloads_before = unloads;
    fail_allocate = true;
    CHECK(content_sound_unload(mod, bank) == TSUCHI_NO_MEMORY);
    CHECK(native_banks.size() == 16 && native_voices.size() == 1 && owned_count(mod) == 17 && stops == full_stops_before && unloads == full_unloads_before);
    CHECK(content_sound_find(mod, "sound.test:bank15", &found) == TSUCHI_OK && found == bank);
    fail_allocate = false;
    stop_refused = true; unload_refused = true;
    CHECK(content_sound_unload(mod, bank) == TSUCHI_OK && native_banks.size() == 16 && native_voices.size() == 1 && owned_count(mod) == 15);
    CHECK(content_sound_stop(mod, voice) == TSUCHI_NOT_FOUND && content_sound_unload(mod, bank) == TSUCHI_NOT_FOUND);
    const auto blocked_extra = remap(3100);
    CHECK(content_sound_load(mod, "sound.test:still-full", blocked_extra.data(), uint32_t(blocked_extra.size()), &voice) == TSUCHI_CAPACITY);
    step(); CHECK(native_banks.size() == 16 && native_voices.size() == 1);
    stop_refused = false; unload_refused = false;
    mod_stop(mod); step(); step(); CHECK(native_banks.empty() && native_voices.empty());
    auto* delayed=mod_create("sound.startup");sound_ready=false;
    CHECK(content_sound_declare(delayed,"sound.startup:bank",fixture,sizeof fixture)==TSUCHI_OK);
    const uint32_t loads_before_ready=loads;step();
    CHECK(loads==loads_before_ready&&content_sound_find(delayed,"sound.startup:bank",&found)==TSUCHI_NOT_READY);
    sound_ready=true;step();
    CHECK(content_sound_find(delayed,"sound.startup:bank",&found)==TSUCHI_OK);
    mod_stop(delayed);step();CHECK(native_banks.empty());
    delayed=mod_create("sound.cancel-startup");sound_ready=false;
    CHECK(content_sound_declare(delayed,"sound.cancel-startup:bank",fixture,sizeof fixture)==TSUCHI_OK);
    const uint32_t loads_before_stop=loads;mod_stop(delayed);sound_ready=true;step();
    CHECK(loads==loads_before_stop&&native_banks.empty());
    delayed=mod_create("sound.owned-startup");sound_ready=false;
    auto temporary_bank=remap(3400);
    CHECK(content_sound_declare(delayed,"sound.owned-startup:bank",temporary_bank.data(),uint32_t(temporary_bank.size()))==TSUCHI_OK);
    std::fill(temporary_bank.begin(),temporary_bank.end(),0);
    sound_ready=true;step();
    CHECK(content_sound_find(delayed,"sound.owned-startup:bank",&found)==TSUCHI_OK);mod_stop(delayed);step();
    delayed=mod_create("sound.stopped-startup");sound_ready=false;
    CHECK(content_sound_declare(delayed,"sound.stopped-startup:bank",fixture,sizeof fixture)==TSUCHI_OK);
    stopping=delayed;callback=Callback::load;sound_ready=true;step();step();
    CHECK(!mod_running(delayed)&&native_banks.empty());
    // Only small metadata is admitted before the engine allocator exists.
    delayed=mod_create("sound.file-startup");sound_ready=false;allocation_limit=4096;
    CHECK(content_sound_declare_file(delayed,"sound.file-startup:bank","tone.bnk",&files)==TSUCHI_OK);
    step();CHECK(file_sizes==0&&file_reads==0);
    sound_ready=true;step();
    CHECK(file_sizes==1&&file_reads==0&&content_sound_find(delayed,"sound.file-startup:bank",&found)==TSUCHI_NO_MEMORY);
    allocation_limit=size_t(-1);step();
    CHECK(file_reads==1&&content_sound_find(delayed,"sound.file-startup:bank",&found)==TSUCHI_OK);
    auto* collision=mod_create("sound.file-collision");
    CHECK(content_sound_declare_file(collision,"sound.file-collision:bank","tone.bnk",&files)==TSUCHI_OK);
    step();CHECK(content_sound_find(collision,"sound.file-collision:bank",&bank)==TSUCHI_BUSY&&native_banks.size()==1);
    mod_stop(collision);CHECK(content_sound_unload(delayed,found)==TSUCHI_OK);mod_stop(delayed);step();CHECK(native_banks.empty());
    // A manifest restarts before the previous owner's bank has retired.
    // Keep its file deferred until native cleanup accepts the old bank.
    for (bool cancel_replacement : {false, true}) {
        delayed=mod_create("sound.file-restart");
        CHECK(content_sound_declare_file(delayed,"sound.file-restart:bank","tone.bnk",&files)==TSUCHI_OK);
        step();CHECK(content_sound_find(delayed,"sound.file-restart:bank",&found)==TSUCHI_OK);
        unload_refused=true;mod_stop(delayed);
        auto* replacement=mod_create("sound.file-restart");
        const auto admitted=content_sound_declare_file(replacement,"sound.file-restart:bank","tone.bnk",&files);
        CHECK(admitted==TSUCHI_OK);
        if (admitted==TSUCHI_OK) {
            const auto reads_before=file_reads,sizes_before=file_sizes,loads_before=loads;
            step();step();
            CHECK(content_sound_find(replacement,"sound.file-restart:bank",&found)==TSUCHI_NOT_READY);
            CHECK(file_reads==reads_before&&file_sizes==sizes_before&&loads==loads_before&&native_banks.size()==1);
            // A second active declaration still conflicts with the pending one.
            CHECK(content_sound_declare_file(replacement,"sound.file-restart:bank","tone.bnk",&files)==TSUCHI_BUSY);
            if (cancel_replacement) mod_stop(replacement);
            unload_refused=false;step();step();
            if (cancel_replacement) CHECK(file_reads==reads_before&&loads==loads_before&&native_banks.empty());
            else {
                CHECK(content_sound_find(replacement,"sound.file-restart:bank",&found)==TSUCHI_OK);
                CHECK(file_reads==reads_before+1&&loads==loads_before+1&&native_banks.size()==1);
                mod_stop(replacement);step();CHECK(native_banks.empty());
            }
        } else {
            mod_stop(replacement);unload_refused=false;step();CHECK(native_banks.empty());
        }
    }
    // All native slots can be retiring when a manifest is restarted.
    delayed=mod_create("sound.full-restart");
    for (uint32_t i=0;i<16;++i) {
        char id[96];std::snprintf(id,sizeof id,"sound.full-restart:bank%u",i);
        auto bytes=remap(20000+i*16);
        CHECK(content_sound_load(delayed,id,bytes.data(),uint32_t(bytes.size()),&found)==TSUCHI_OK);
    }
    CHECK(native_banks.size()==16);
    unload_refused=true;mod_stop(delayed);
    delayed=mod_create("sound.full-restart");
    const auto reads_before_replacement=file_reads,sizes_before_replacement=file_sizes;
    for (uint32_t i=0;i<16;++i) {
        char id[96];std::snprintf(id,sizeof id,"sound.full-restart:bank%u",i);
        CHECK(content_sound_declare_file(delayed,id,"tone.bnk",&files)==TSUCHI_OK);
    }
    step();CHECK(native_banks.size()==16&&owned_count(delayed)==16);
    CHECK(file_reads==reads_before_replacement&&file_sizes==sizes_before_replacement);
    mod_stop(delayed);unload_refused=false;step();step();CHECK(native_banks.empty());
    // A different ID with the same native IDs also waits for retirement.
    delayed=mod_create("sound.foreign-retiring");
    CHECK(content_sound_declare_file(delayed,"sound.foreign-retiring:bank","tone.bnk",&files)==TSUCHI_OK);
    step();CHECK(content_sound_find(delayed,"sound.foreign-retiring:bank",&found)==TSUCHI_OK);
    unload_refused=true;mod_stop(delayed);
    delayed=mod_create("sound.foreign-replacement");
    CHECK(content_sound_declare_file(delayed,"sound.foreign-replacement:bank","tone.bnk",&files)==TSUCHI_OK);
    step();CHECK(content_sound_find(delayed,"sound.foreign-replacement:bank",&found)==TSUCHI_NOT_READY);
    unload_refused=false;step();step();
    CHECK(content_sound_find(delayed,"sound.foreign-replacement:bank",&found)==TSUCHI_OK&&native_banks.size()==1);
    mod_stop(delayed);step();CHECK(native_banks.empty());
    for(const auto mode:{Callback::none,Callback::file_size,Callback::file_read,Callback::load}) {
        delayed=mod_create("sound.file-cancel");sound_ready=false;
        CHECK(content_sound_declare_file(delayed,"sound.file-cancel:bank","tone.bnk",&files)==TSUCHI_OK);
        const auto prior_reads=file_reads,prior_sizes=file_sizes;
        if(mode==Callback::none)mod_stop(delayed);else {stopping=delayed;callback=mode;}
        sound_ready=true;step();step();
        CHECK(!mod_running(delayed)&&native_banks.empty());
        if(mode==Callback::none)CHECK(file_reads==prior_reads&&file_sizes==prior_sizes);
    }
    for(const auto error:{TSUCHI_NOT_FOUND,TSUCHI_IO,TSUCHI_INVALID}) {
        delayed=mod_create("sound.file-error");file_result=error==TSUCHI_INVALID?TSUCHI_OK:error;corrupt_file=error==TSUCHI_INVALID;
        CHECK(content_sound_declare_file(delayed,"sound.file-error:bank","tone.bnk",&files)==TSUCHI_OK);
        step();CHECK(content_sound_find(delayed,"sound.file-error:bank",&found)==error&&native_banks.empty());mod_stop(delayed);
    }
    // Banks whose files failed hold nothing in the sound engine, so they leave the shared bank limit free.
    delayed=mod_create("sound.file-failed");corrupt_file=true;
    for(uint32_t i=0;i<16;++i) {
        char id[96];std::snprintf(id,sizeof id,"sound.file-failed:bank%u",i);
        CHECK(content_sound_declare_file(delayed,id,"tone.bnk",&files)==TSUCHI_OK);
    }
    step();corrupt_file=false;
    CHECK(content_sound_find(delayed,"sound.file-failed:bank15",&found)==TSUCHI_INVALID&&native_banks.empty());
    auto* after_failures=mod_create("sound.after-failures");
    CHECK(content_sound_load(after_failures,"sound.after-failures:bank",fixture,sizeof fixture,&bank)==TSUCHI_OK&&native_banks.size()==1);
    // The failed declarations do not keep the bank in use from being unloaded either.
    CHECK(content_sound_unload(after_failures,bank)==TSUCHI_OK);
    step();CHECK(native_banks.empty());
    CHECK(content_sound_load(after_failures,"sound.after-failures:bank",fixture,sizeof fixture,&bank)==TSUCHI_OK&&native_banks.size()==1);
    CHECK(content_sound_declare_file(delayed,"sound.file-failed:bank16","tone.bnk",&files)==TSUCHI_OK);
    mod_stop(after_failures);mod_stop(delayed);step();CHECK(native_banks.empty());
    file_result=TSUCHI_OK;corrupt_file=false;
    mod = mod_create("sound.deferred");
    CHECK(content_sound_load(mod, "sound.deferred:bank", fixture, sizeof fixture, &bank) == TSUCHI_OK);
    CHECK(content_sound_play(mod, bank, "#1004", point, &voice) == TSUCHI_OK);
    sound_ready = false; mod_stop(mod);
    CHECK(content_sound_find(other, "sound.deferred:bank", &found) == TSUCHI_NOT_FOUND && !found);
    CHECK(content_sound_find(mod, "sound.deferred:bank", &found) == TSUCHI_INVALID && !found);
    CHECK(content_sound_events(other, bank, nullptr, 0, &size) == TSUCHI_NOT_FOUND);
    CHECK(owned_count(mod) == 0 && native_banks.size() == 1 && native_voices.size() == 1);
    step(); CHECK(native_banks.size() == 1);
    sound_ready = true; unload_refused = true; stop_refused = true;
    CHECK(content_sound_load(other, "sound.other:retired", fixture, sizeof fixture, &bank) == TSUCHI_BUSY);
    step(); CHECK(native_banks.size() == 1 && native_voices.size() == 1);
    stop_refused = false; step(); CHECK(native_voices.empty() && native_banks.size() == 1);
    unload_refused = false; step(); CHECK(native_banks.empty());
    stopping = mod_create("sound.nested-unload");
    CHECK(content_sound_load(stopping, "sound.nested-unload:bank", fixture, sizeof fixture, &bank) == TSUCHI_OK);
    callback_bank = bank; callback = Callback::unload_during_play;
    CHECK(content_sound_play(stopping, bank, "#1004", point, &voice) == TSUCHI_OK);
    CHECK(callback_result == TSUCHI_BUSY && native_banks.size() == 1 && native_voices.size() == 1 && owned_count(stopping) == 2);
    sound_ready = false;
    const uint32_t unavailable_stops = stops, unavailable_unloads = unloads;
    CHECK(content_sound_unload(stopping, bank) == TSUCHI_NOT_READY && owned_count(stopping) == 2);
    CHECK(stops == unavailable_stops && unloads == unavailable_unloads && native_voices.size() == 1);
    sound_ready = true;
    CHECK(content_sound_unload(stopping, bank) == TSUCHI_OK && native_voices.empty() && owned_count(stopping) == 0);
    mod_stop(stopping); step(); CHECK(native_banks.empty());
    for (const auto mode : {Callback::load, Callback::play, Callback::stop, Callback::unload}) {
        stopping = mod_create("sound.callback"); callback = mode;
        const auto loaded = content_sound_load(stopping, "sound.callback:bank", fixture, sizeof fixture, &bank);
        if (mode == Callback::load) CHECK(loaded == TSUCHI_NOT_READY && !bank);
        else {
            CHECK(loaded == TSUCHI_OK);
            const auto played = content_sound_play(stopping, bank, "#1004", point, &voice);
            if (mode == Callback::play) CHECK(played == TSUCHI_NOT_READY && !voice);
            else {
                CHECK(played == TSUCHI_OK);
                if (mode == Callback::stop) CHECK(content_sound_stop(stopping, voice) == TSUCHI_NOT_READY);
                else CHECK(content_sound_unload(stopping, bank) == TSUCHI_OK);
            }
        }
        step();
        CHECK(native_banks.empty() && native_voices.empty() && owned_count(stopping) == 0);
    }
    // A stopped bank remains reserved until native unload accepts it.
    stopping = mod_create("sound.retired-callback");
    CHECK(content_sound_load(stopping, "sound.retired-callback:bank", fixture, sizeof fixture, &bank) == TSUCHI_OK);
    CHECK(content_sound_play(stopping, bank, "#1004", point, &voice) == TSUCHI_OK);
    sound_ready = false; mod_stop(stopping); sound_ready = true; step(); step();
    CHECK(native_banks.empty() && native_voices.empty());
    // Exercise the default adapter with original ABI-shaped function pointers.
    auto& f = game::table::fn::wwise_initialized;
    f = []() { return sound_ready; };
    game::table::fn::wwise_load_bank_copy = [](void* data, uint32_t bytes, void* out) -> uint32_t {
        return load(data, bytes, static_cast<uint32_t*>(out)) == TSUCHI_OK ? 1 : 52;
    };
    game::table::fn::wwise_unload_bank = [](uint32_t number, void* memory) -> uint32_t {
        CHECK(memory == nullptr); return unload(number) == TSUCHI_OK ? 1 : 52;
    };
    game::table::fn::wwise_register_object = [](uint64_t emitter) -> uint32_t { CHECK(emitter != 0); return 1; };
    game::table::fn::wwise_unregister_object = [](uint64_t emitter) -> uint32_t { CHECK(emitter != 0); return 1; };
    game::table::fn::wwise_set_position = [](uint64_t emitter, void* values) -> uint32_t {
        CHECK(emitter != 0); const auto* p = static_cast<float*>(values);
        CHECK(p[0] == 1 && p[5] == 1 && p[6] == 1 && p[7] == 2 && p[8] == 3); return 1;
    };
    game::table::fn::wwise_post_event = [](uint32_t event, uint64_t emitter, uint32_t flags, void* fn, void* cookie, uint32_t external, void* sources, uint32_t playing) -> uint32_t {
        CHECK(flags == 0 && !fn && !cookie && external == 0 && !sources && playing == 0);
        uint32_t out = 0; CHECK(play(event, TSUCHI_Vec3{1, 2, 3}, emitter, &out) == TSUCHI_OK); return out;
    };
    game::table::fn::wwise_event_from_playing = [](uint32_t id) -> uint32_t { return active(id) ? 1004 : 0; };
    game::table::fn::wwise_stop_playing = [](uint32_t id, int32_t milliseconds, uint32_t fade) { CHECK(milliseconds == 0 && fade == 4); CHECK(stop(id) == TSUCHI_OK); };
    content_sound_start();
    auto* native = mod_create("sound.native");
    CHECK(content_sound_load(native, "sound.native:bank", fixture, sizeof fixture, &bank) == TSUCHI_OK);
    CHECK(content_sound_play(native, bank, "#1004", point, &voice) == TSUCHI_OK);
    CHECK(content_sound_stop(native, voice) == TSUCHI_OK);
    CHECK(content_sound_unload(native, bank) == TSUCHI_OK);
    step(); CHECK(native_banks.empty());
    mod_stop(native);
    mod_stop(other);
    std::printf("content sound: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
