// The howlout service: phrases of mods' own beside the game's Howl-Outs.
//
// The game's recognizer is linked into the game and offers its functions by name (TMW_VT_*). The
// service keeps a second instance of it, with a dictionary that holds only mods' phrases
// (howlout_dictionary.h), and gives that instance every audio frame the game gives its own. The
// game's instance and its eleven entries are never touched.
//
// The game sets its instances up with settings of its own, so the service watches TMW_VT_getInfo and
// TMW_VT_init and gives its instance the same ones. It watches TMW_VT_exec for the audio. The game
// keeps an instance for each dictionary it has loaded and gives audio to one of them at a time, so
// the service takes its settings from one whose dictionary the sound rows were trained for. The
// recognizer may run on another thread than the game's frames: everything an instance does after it
// is set up happens inside the watched TMW_VT_exec, and the game thread hands instances over through
// one atomic pointer.
//
// The service calls the recognizer past its own watchers, and no function of the recognizer calls
// one of the four watched ones itself, so a watcher only ever sees the game's calls.
#include <string.h>

#include "frame.h"
#include "game.h"
#include "hooks.h"
#include "howlout_dictionary.h"
#include "howlout_service.h"
#include "modules.h"
#include "mods.h"
#include "registry.h"
#include "services.h"

namespace tsuchi {
namespace {

constexpr uint32_t kSlots = howlout::kMostPhrases;
constexpr uint32_t kSoundsBytes = 256;          // 42 sounds of at most four letters, a stress digit and a space
constexpr uint32_t kGameInstances = 8, kLoaders = 32, kHeard = 16;
constexpr uint64_t kResultBytes = 0x400, kInfoBytes = 0x100;

// ---- The recognizer ---------------------------------------------------------------------------------------

using GetInfo = int (*)(const void* dictionary, uint32_t tag, const void* mode, uint32_t a, uint32_t b, uint32_t c,
                        uint32_t d, uint64_t e, void* info);
using Init = int (*)(void** handle, const void* dictionary, const void* settings, void* work, uint32_t work_bytes,
                     uint32_t tag, const void* mode, uint32_t a, uint32_t b, uint32_t c);
using Exec = int (*)(void* handle, const void* pcm, void* a, void* heard, void* b, void* c);
using GetResult = int (*)(void* handle, void* result);
using Switch = int (*)(void* from, void* to);
using Free = int (*)(void* handle);

// The game's own functions, as they continue past the service's watchers.
GetInfo next_get_info = nullptr;
Init next_init = nullptr;
Exec next_exec = nullptr;
Free next_free = nullptr;
GetResult get_result = nullptr;
Switch switch_dictionary = nullptr;
bool looked = false, found = false, watching = false;
TSUCHI_Mod* self = nullptr;

template <class T> T load(const T& value) { return __atomic_load_n(&value, __ATOMIC_ACQUIRE); }
template <class T> void store(T& value, T to) { __atomic_store_n(&value, to, __ATOMIC_RELEASE); }
template <class T> T swap(T& value, T to) { return __atomic_exchange_n(&value, to, __ATOMIC_ACQ_REL); }

void note(const char* text) { mod_note(kFrameworkLogId, text); }

// ---- What the game set up ---------------------------------------------------------------------------------

// What the game last asked TMW_VT_getInfo with, and each instance it made with TMW_VT_init.
struct Asked { uint32_t tag = 0, a = 0, b = 0, c = 0, d = 0; uint64_t e = 0; const void* mode = nullptr; bool known = false; };
struct GameInstance {
    void* handle = nullptr;
    uint32_t serial = 0;             // which instance the game ever made this is: a later one can have the same handle
    const uint8_t* dictionary = nullptr;
    const void *settings = nullptr, *mode = nullptr;
    uint32_t tag = 0, a = 0, b = 0, c = 0;
    Asked asked;
};
Asked asked;
GameInstance games[kGameInstances];
void* game_now = nullptr;        // the instance the game last gave audio, or last made
uint32_t serials = 0;

const GameInstance* game_for(const void* handle) {
    for (const GameInstance& game : games)
        if (load(game.handle) && load(game.handle) == handle) return &game;
    return nullptr;
}
// The game still has the instance it made as `serial` with this handle.
bool still_there(const void* handle, uint32_t serial) {
    const GameInstance* game = game_for(handle);
    return game && game->serial == serial;
}

// A dictionary's size, from its own header: the matrix is last, a 16-bit flag for every pair of phrases.
uint64_t dictionary_bytes(const uint8_t* data) {
    uint32_t count = 0, matrix = 0;
    memcpy(&count, data + 0x50, 4);
    memcpy(&matrix, data + 0x58, 4);
    return count && count <= howlout::kMostPhrases ? uint64_t(matrix) + uint64_t(count) * count * 2 : 0;
}

// ---- The service's instance -------------------------------------------------------------------------------

struct Instance {
    void* handle = nullptr;                  // null: the instance before it is to be dropped, with none after
    uint8_t* dictionary = nullptr;
    void* work = nullptr;
    const void* made_from = nullptr;         // the game's instance whose settings it has
    uint32_t made_serial = 0;
    uint64_t phrases[kSlots] = {};           // the handle of each entry's phrase
    uint32_t count = 0;
    Instance* next = nullptr;                // in the list of instances to release
};
Instance* pending = nullptr;     // made on the game thread, taken inside TMW_VT_exec
Instance* active = nullptr;      // only inside TMW_VT_exec
Instance* retired = nullptr;     // left inside TMW_VT_exec, released on the game thread
bool wanted = false;             // an instance was handed over and not dropped since: game thread only
const void* wanted_from = nullptr;
uint32_t wanted_serial = 0;
constexpr uint32_t kNever = ~0u;
bool unbuilt = false;            // the phrases are not the ones the instance handed over has: game thread only
uint32_t refused_at = kNever;    // how many instances the game had made when the phrases were last refused

// Phrases heard inside TMW_VT_exec, for the next frame.
uint64_t heard[kHeard];
uint32_t heard_head = 0, heard_tail = 0;

void retire(Instance* instance) {
    Instance* first = load(retired);
    do instance->next = first; while (!__atomic_compare_exchange_n(&retired, &first, instance, true, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE));
}

void release_instance(Instance* instance) {
    release(instance->dictionary);
    release(instance->work);
    release(instance);
}

// Inside TMW_VT_exec, after the game's own instance heard the frame.
void listen(const void* pcm) {
    if (Instance* taken = swap(pending, static_cast<Instance*>(nullptr))) {
        if (active) {
            // With the same settings, the new one carries on from what the old one has heard so far.
            if (taken->handle && taken->made_from == active->made_from && taken->made_serial == active->made_serial)
                switch_dictionary(active->handle, taken->handle);
            next_free(active->handle);
            retire(active);
        }
        if (taken->handle) {
            active = taken;
        } else {
            active = nullptr;
            retire(taken);
        }
    }
    if (active && !still_there(active->made_from, active->made_serial)) {   // its settings were the game's to keep
        next_free(active->handle);
        retire(active);
        active = nullptr;
    }
    if (!active) return;
    alignas(8) int16_t flag[4] = {};
    if (next_exec(active->handle, pcm, nullptr, flag, nullptr, nullptr) || !flag[0]) return;
    alignas(8) static uint8_t result[kResultBytes];
    if (get_result(active->handle, result)) return;
    uint32_t entry = 0;
    memcpy(&entry, result, 4);                                    // an entry's ID is its place in the dictionary
    if (entry >= active->count) return;
    const uint32_t head = load(heard_head);
    if (head - load(heard_tail) >= kHeard) return;
    heard[head % kHeard] = active->phrases[entry];
    store(heard_head, head + 1);
}

int on_get_info(const void* dictionary, uint32_t tag, const void* mode, uint32_t a, uint32_t b, uint32_t c, uint32_t d,
                uint64_t e, void* info) {
    asked.tag = tag; asked.mode = mode; asked.a = a; asked.b = b; asked.c = c; asked.d = d; asked.e = e; asked.known = true;
    return next_get_info(dictionary, tag, mode, a, b, c, d, e, info);
}

int on_init(void** handle, const void* dictionary, const void* settings, void* work, uint32_t work_bytes, uint32_t tag,
            const void* mode, uint32_t a, uint32_t b, uint32_t c) {
    const int result = next_init(handle, dictionary, settings, work, work_bytes, tag, mode, a, b, c);
    if (result || !handle || !*handle || !asked.known) return result;
    GameInstance* slot = nullptr;
    for (GameInstance& game : games)
        if (game.handle == *handle || (!slot && !game.handle)) slot = &game;
    if (!slot) return result;
    slot->dictionary = static_cast<const uint8_t*>(dictionary);
    slot->settings = settings; slot->mode = mode;
    slot->tag = tag; slot->a = a; slot->b = b; slot->c = c;
    slot->asked = asked;
    slot->serial = __atomic_add_fetch(&serials, 1, __ATOMIC_ACQ_REL);
    store(slot->handle, *handle);
    store(game_now, *handle);
    return result;
}

int on_exec(void* handle, const void* pcm, void* a, void* flag, void* b, void* c) {
    const int result = next_exec(handle, pcm, a, flag, b, c);
    store(game_now, handle);
    if (pcm) listen(pcm);
    return result;
}

int on_free(void* handle) {
    for (GameInstance& game : games)
        if (load(game.handle) == handle) store(game.handle, static_cast<void*>(nullptr));
    return next_free(handle);
}

// Finds the recognizer by name and puts the watchers on. False on a build without Howl-Outs.
bool watch() {
    if (watching) return true;
    const NativeHooks* native = native_hooks_backend();
    if (!looked) {
        looked = true;
        const uint64_t main = game::module_containing(game::main_base()) ? game::module_containing(game::main_base()) : game::main_base();
        const char* names[] = {"TMW_VT_getInfo", "TMW_VT_init", "TMW_VT_exec", "TMW_VT_free", "TMW_VT_getResult", "TMW_VT_switchVtDic"};
        static uint64_t at[6];
        found = true;
        for (int i = 0; i < 6; ++i) found &= (at[i] = game::module_symbol(main, names[i])) != 0;
        if (found) {
            next_get_info = reinterpret_cast<GetInfo>(at[0]);    // until a watcher is on, the game's own
            get_result = reinterpret_cast<GetResult>(at[4]);
            switch_dictionary = reinterpret_cast<Switch>(at[5]);
        }
    }
    if (!found || !native || !self) return false;
    const uint64_t main = game::module_containing(game::main_base()) ? game::module_containing(game::main_base()) : game::main_base();
    struct Watcher { const char* name; void* function; void** next; };
    static uint64_t ids[4];
    const Watcher watchers[] = {{"TMW_VT_getInfo", reinterpret_cast<void*>(&on_get_info), reinterpret_cast<void**>(&next_get_info)},
                                {"TMW_VT_init", reinterpret_cast<void*>(&on_init), reinterpret_cast<void**>(&next_init)},
                                {"TMW_VT_exec", reinterpret_cast<void*>(&on_exec), reinterpret_cast<void**>(&next_exec)},
                                {"TMW_VT_free", reinterpret_cast<void*>(&on_free), reinterpret_cast<void**>(&next_free)}};
    for (int i = 0; i < 4; ++i) {
        if (ids[i]) continue;
        const uint64_t address = game::module_symbol(main, watchers[i].name);
        void* original = nullptr;
        if (native->function(self, address - game::main_base(), game::read_or<uint32_t>(address), watchers[i].function, &ids[i], &original) != TSUCHI_OK) {
            ids[i] = 0;
            return false;
        }
        *watchers[i].next = original;
    }
    watching = true;
    return true;
}

// ---- Phrases ----------------------------------------------------------------------------------------------

struct Slot {
    TSUCHI_Mod* owner = nullptr;
    uint64_t handle = 0;
    uint32_t threshold = 0;
    char sounds[kSoundsBytes] = {};
};
Slot slots[kSlots];
uint32_t made = 0;                // phrases ever added: a handle is that count above its slot's number
uint8_t* rows = nullptr;          // the sound rows file
uint64_t rows_bytes = 0;
TSUCHI_Mod* loaders[kLoaders];    // the mods that gave it

Slot* slot_of(uint64_t handle) {
    const uint64_t index = (handle & 0xff) - 1;
    return handle && index < kSlots && slots[index].handle == handle ? &slots[index] : nullptr;
}

bool any_phrase() {
    for (const Slot& slot : slots)
        if (slot.handle) return true;
    return false;
}

void hand_over(Instance* instance) {
    if (Instance* never_taken = swap(pending, instance)) {
        if (never_taken->handle) next_free(never_taken->handle);
        release_instance(never_taken);
    }
}

// The recognizer, or the assembler, would not take the phrases with the game's instance as it is.
TSUCHI_Result refused(const char* why) {
    refused_at = load(serials);
    note(why);
    return TSUCHI_INVALID;
}

// Makes an instance for the phrases there are now and hands it over; with no phrase, hands over the end of
// the one there is. TSUCHI_NOT_READY while the game has no instance of its own to take settings from: the
// phrases wait for one. After any failure `unbuilt` stays set, and each frame tries again.
TSUCHI_Result rebuild() {
    unbuilt = true;
    if (!any_phrase()) {
        if (wanted) {
            auto* end = static_cast<Instance*>(allocate(sizeof(Instance)));
            if (!end) return TSUCHI_NO_MEMORY;
            *end = Instance{};
            hand_over(end);
            wanted = false;
        }
        unbuilt = false;
        return TSUCHI_OK;
    }
    howlout::Phrase phrases[kSlots];
    uint64_t handles[kSlots];
    uint32_t count = 0;
    for (const Slot& slot : slots)
        if (slot.handle) {
            phrases[count] = {slot.sounds, slot.threshold};
            handles[count++] = slot.handle;
        }

    // Whose settings: the instance the game last gave audio, or else another of its instances, the first
    // whose dictionary the sound rows are for. With no buffer, the assembler says how much one needs.
    const GameInstance* now = game_for(load(game_now));
    const GameInstance* game = nullptr;
    howlout::Built built = howlout::Built::stock;
    uint64_t bytes = 0;
    bool any = false;
    for (uint32_t i = 0; i <= kGameInstances && !game; ++i) {
        const GameInstance* candidate = i ? &games[i - 1] : now;
        if (!candidate || !load(candidate->handle) || (i && candidate == now)) continue;
        any = true;
        built = howlout::assemble(candidate->dictionary, dictionary_bytes(candidate->dictionary), rows, rows_bytes, phrases, count, nullptr, 0, &bytes);
        if (built == howlout::Built::capacity) game = candidate;
        else if (built != howlout::Built::mismatch && built != howlout::Built::stock) break;   // the phrases themselves
    }
    if (!any || !rows) return TSUCHI_NOT_READY;
    if (!game)
        return refused(built == howlout::Built::mismatch || built == howlout::Built::stock
                           ? "howlout: the sound rows are not for a language the game listens in"
                           : "howlout: the phrases do not fit one dictionary");

    auto* instance = static_cast<Instance*>(allocate(sizeof(Instance)));
    if (!instance) return TSUCHI_NO_MEMORY;
    *instance = Instance{};
    instance->count = count;
    memcpy(instance->phrases, handles, count * sizeof handles[0]);
    instance->dictionary = static_cast<uint8_t*>(allocate(bytes));
    if (!instance->dictionary) { release_instance(instance); return TSUCHI_NO_MEMORY; }
    if (howlout::assemble(game->dictionary, dictionary_bytes(game->dictionary), rows, rows_bytes, phrases, count,
                          instance->dictionary, bytes, &bytes) != howlout::Built::ok) {
        release_instance(instance);
        return refused("howlout: the phrases do not fit one dictionary");
    }
    alignas(8) uint8_t info[kInfoBytes] = {};
    TSUCHI_Result result = TSUCHI_OK;
    const Asked& how = game->asked;
    if (next_get_info(instance->dictionary, how.tag, how.mode, how.a, how.b, how.c, how.d, how.e, info)) {
        result = TSUCHI_INVALID;
    } else {
        uint32_t work_bytes = 0;
        memcpy(&work_bytes, info, 4);
        instance->work = allocate(work_bytes);
        if (!instance->work) result = TSUCHI_NO_MEMORY;
        else if (next_init(&instance->handle, instance->dictionary, game->settings, instance->work, work_bytes, game->tag,
                           game->mode, game->a, game->b, game->c) || !instance->handle) result = TSUCHI_INVALID;
    }
    if (result != TSUCHI_OK) {
        release_instance(instance);
        return result == TSUCHI_INVALID ? refused("howlout: the recognizer refused the phrases' dictionary") : result;
    }
    instance->made_from = wanted_from = game->handle;
    instance->made_serial = wanted_serial = game->serial;
    hand_over(instance);
    wanted = true;
    unbuilt = false;
    refused_at = kNever;
    return TSUCHI_OK;
}

// A change of one phrase that the recognizer would not take is taken back, with what was known before it.
struct Attempt {
    bool unbuilt_before = unbuilt;
    uint32_t refused_before = refused_at;
    // True when the phrases are kept: an instance has them, or they wait for the game to make its own.
    bool kept(TSUCHI_Result result) {
        if (result == TSUCHI_OK || result == TSUCHI_NOT_READY) return true;
        unbuilt = unbuilt_before;
        refused_at = refused_before;
        return false;
    }
};

bool loaded_by(const TSUCHI_Mod* mod) {
    for (const TSUCHI_Mod* loader : loaders)
        if (loader == mod) return true;
    return false;
}

// ---- The service ------------------------------------------------------------------------------------------

TSUCHI_Result load_sounds(TSUCHI_Mod* mod, const void* data, uint64_t size) {
    if (!watch()) return TSUCHI_UNSUPPORTED;
    if (size < 0x40 || memcmp(data, "HWLR", 4)) return TSUCHI_INVALID;
    if (rows && (size != rows_bytes || memcmp(data, rows, size))) {
        if (any_phrase()) return TSUCHI_BUSY;
        release(rows);
        rows = nullptr;
        for (TSUCHI_Mod*& loader : loaders) loader = nullptr;
    }
    if (!rows) {
        rows = static_cast<uint8_t*>(allocate(size));
        if (!rows) return TSUCHI_NO_MEMORY;
        memcpy(rows, data, size);
        rows_bytes = size;
    }
    if (loaded_by(mod)) return TSUCHI_OK;
    for (TSUCHI_Mod*& loader : loaders)
        if (!loader) { loader = mod; return TSUCHI_OK; }
    return TSUCHI_NO_MEMORY;
}

TSUCHI_Result add(TSUCHI_Mod* mod, const char* sounds, uint32_t threshold, TSUCHI_Howlout* out) {
    if (!watch()) return TSUCHI_UNSUPPORTED;
    if (!rows || !loaded_by(mod)) return TSUCHI_NOT_FOUND;
    const size_t length = strlen(sounds);
    if (length >= kSoundsBytes || howlout::check(rows, rows_bytes, {sounds, threshold}) != howlout::Built::ok) return TSUCHI_INVALID;
    Slot* slot = nullptr;
    for (Slot& candidate : slots)
        if (!candidate.handle) { slot = &candidate; break; }
    if (!slot) return TSUCHI_CAPACITY;
    slot->owner = mod;
    slot->threshold = threshold;
    memcpy(slot->sounds, sounds, length + 1);
    slot->handle = (uint64_t(++made) << 8) | uint64_t(slot - slots + 1);
    Attempt attempt;
    const TSUCHI_Result result = rebuild();
    if (!attempt.kept(result)) {
        *slot = Slot{};
        return result;
    }
    out->id = slot->handle;
    return TSUCHI_OK;
}

TSUCHI_Result remove(TSUCHI_Mod* mod, TSUCHI_Howlout phrase) {
    Slot* slot = slot_of(phrase.id);
    if (!slot) return TSUCHI_NOT_FOUND;
    if (slot->owner != mod) return TSUCHI_DENIED;
    *slot = Slot{};
    rebuild();
    return TSUCHI_OK;
}

TSUCHI_Result set_threshold(TSUCHI_Mod* mod, TSUCHI_Howlout phrase, uint32_t threshold) {
    Slot* slot = slot_of(phrase.id);
    if (!slot) return TSUCHI_NOT_FOUND;
    if (slot->owner != mod) return TSUCHI_DENIED;
    if (threshold > 1000) return TSUCHI_INVALID;
    const uint32_t before = slot->threshold;
    slot->threshold = threshold;
    Attempt attempt;
    const TSUCHI_Result result = rebuild();
    if (attempt.kept(result)) return TSUCHI_OK;
    slot->threshold = before;
    return result;
}

TSUCHI_Result threshold(TSUCHI_Howlout phrase, uint32_t* out) {
    const Slot* slot = slot_of(phrase.id);
    if (!slot) return TSUCHI_NOT_FOUND;
    *out = slot->threshold;
    return TSUCHI_OK;
}

// Each frame: what was heard is reported, what a stopped mod held ends, and instances no longer in use are released.
void step() {
    for (uint32_t tail = load(heard_tail); tail != load(heard_head); store(heard_tail, ++tail))
        if (slot_of(heard[tail % kHeard])) howlout::emit_heard({{heard[tail % kHeard]}});
    for (Instance* instance = swap(retired, static_cast<Instance*>(nullptr)); instance;) {
        Instance* next = instance->next;
        release_instance(instance);
        instance = next;
    }
    bool changed = false;
    for (Slot& slot : slots)
        if (slot.handle && !mod_usable(slot.owner)) { slot = Slot{}; changed = true; }
    for (TSUCHI_Mod*& loader : loaders)
        if (loader && !mod_usable(loader)) loader = nullptr;
    // The game ended the instance the service's had its settings from: the next one it makes gives them again.
    if (wanted && !still_there(wanted_from, wanted_serial)) { wanted = false; unbuilt = true; }
    // After a refusal, only another instance of the game's can change the answer.
    if (changed || (unbuilt && load(serials) != refused_at)) rebuild();
    if (!watching && found) watch();
}

const howlout::Backend service{load_sounds, add, remove, set_threshold, threshold};

} // namespace

void howlout_start(TSUCHI_Mod* framework) {
    self = framework;
    howlout::set_backend(&service);
    watch();     // early, so that the game is seen setting up its own instance
    each_frame(step);
}

} // namespace tsuchi
