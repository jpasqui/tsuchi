// The howlout service's runtime against a made-up recognizer: the watchers, the service's own instance,
// what is heard, and what becomes of instances when phrases or the game's own instance change. The
// recognizer here only records what it is given, so this says nothing about recognition, and everything
// runs on one thread. The dictionaries are the made-up ones of tests/fixtures/howlout_vectors.json.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <deque>
#include <set>
#include <string>
#include <vector>

#include "frame.h"
#include "game.h"
#include "hooks.h"
#include "howlout_service.h"
#include "howlout_vectors.h"   // written by tests/test_howlout.py
#include "modules.h"
#include "mods.h"
#include "registry.h"
#include "services.h"

using namespace tsuchi;

namespace {

int failures = 0;
#define CHECK(what) do { if (!(what)) { printf("FAILED line %d: %s\n", __LINE__, #what); ++failures; } } while (0)

// ---- Memory: counted, refusable, and never touched while the game is inside the recognizer ----------------

std::set<void*> blocks;
int refuse_after = -1;     // how many allocations are still granted before the rest are refused; -1 grants all
bool in_exec = false;      // the game is inside TMW_VT_exec, which may be another thread than the frames'

void* counted_allocate(size_t bytes) {
    CHECK(!in_exec);
    if (refuse_after == 0) return nullptr;
    if (refuse_after > 0) --refuse_after;
    void* block = malloc(bytes ? bytes : 1);
    blocks.insert(block);
    return block;
}

// ---- The made-up recognizer -------------------------------------------------------------------------------

struct Made {
    void* handle = nullptr;
    const void *settings = nullptr, *mode = nullptr, *last_pcm = nullptr;
    uint32_t tag = 0, a = 0, b = 0, c = 0, result = 0;
    std::vector<uint8_t> dictionary;   // as it was given
    unsigned frames = 0, carried = 0;
    int says = -1;                     // the entry to report at the next frame of audio
    bool freed = false, games = false;
};
std::deque<Made> made;                 // every instance ever made, the game's and the service's

Made* instance(void* handle) {
    for (auto at = made.rbegin(); at != made.rend(); ++at)
        if (at->handle == handle) return &*at;
    return nullptr;
}
Made* live(void* handle) {
    Made* found = instance(handle);
    CHECK(found && !found->freed);
    return found && !found->freed ? found : nullptr;
}

void counted_release(void* block) {
    CHECK(!in_exec);
    CHECK(blocks.erase(block) == 1);
    const Made* worked_in = instance(block);
    CHECK(!worked_in || worked_in->freed);   // the recognizer was told before an instance's memory went
    free(block);
}

uint32_t u32(const uint8_t* at) { uint32_t value; memcpy(&value, at, 4); return value; }
uint32_t count_of(const uint8_t* dictionary) { return u32(dictionary + 0x50); }
uint32_t size_of(const uint8_t* dictionary) { return u32(dictionary + 0x58) + count_of(dictionary) * count_of(dictionary) * 2; }
uint32_t threshold_of(const Made* made, uint32_t entry) { return u32(made->dictionary.data() + u32(made->dictionary.data() + 0x54) + 20 * entry + 4); }
uint32_t work_for(const void* dictionary) { return 0x40 + 0x10 * count_of(static_cast<const uint8_t*>(dictionary)); }

struct Asked { const void* dictionary; uint32_t tag; const void* mode; uint32_t a, b, c, d; uint64_t e; };
Asked last_asked{};
unsigned switches = 0;
void *switched_from = nullptr, *switched_to = nullptr;
bool refuse_info = false, refuse_init = false;

int vt_get_info(const void* dictionary, uint32_t tag, const void* mode, uint32_t a, uint32_t b, uint32_t c, uint32_t d,
                uint64_t e, void* info) {
    last_asked = {dictionary, tag, mode, a, b, c, d, e};
    if (refuse_info) return 7;
    const uint32_t bytes = work_for(dictionary);
    memcpy(info, &bytes, 4);
    return 0;
}
int vt_init(void** handle, const void* dictionary, const void* settings, void* work, uint32_t work_bytes, uint32_t tag,
            const void* mode, uint32_t a, uint32_t b, uint32_t c) {
    if (refuse_init) return 9;
    CHECK(work && work_bytes == work_for(dictionary));
    Made next;
    next.handle = work; next.settings = settings; next.mode = mode;
    next.tag = tag; next.a = a; next.b = b; next.c = c;
    const auto* bytes = static_cast<const uint8_t*>(dictionary);
    next.dictionary.assign(bytes, bytes + size_of(bytes));
    made.push_back(next);
    *handle = work;
    return 0;
}
int vt_exec(void* handle, const void* pcm, void*, void* flag, void*, void*) {
    Made* heard_by = live(handle);
    if (!heard_by) return 1;
    ++heard_by->frames;
    heard_by->last_pcm = pcm;
    if (heard_by->says >= 0) {
        const int16_t one = 1;
        memcpy(flag, &one, 2);
        heard_by->result = uint32_t(heard_by->says);
        heard_by->says = -1;
    }
    return 0;
}
int vt_get_result(void* handle, void* result) {
    const Made* asked = live(handle);
    if (!asked) return 1;
    memcpy(result, &asked->result, 4);
    return 0;
}
int vt_switch(void* from, void* to) {
    const Made* old = live(from);
    Made* next = live(to);
    ++switches; switched_from = from; switched_to = to;
    if (old && next) next->carried = old->frames;
    return 0;
}
int vt_free(void* handle) {
    if (Made* ended = live(handle)) ended->freed = true;
    return 0;
}

// ---- The made-up game: the recognizer by name, and its calls as they go through the watchers ---------------

struct Symbol { const char* name; void* stock; void* watcher; };
Symbol symbols[] = {{"TMW_VT_getInfo", reinterpret_cast<void*>(&vt_get_info), nullptr},
                    {"TMW_VT_init", reinterpret_cast<void*>(&vt_init), nullptr},
                    {"TMW_VT_exec", reinterpret_cast<void*>(&vt_exec), nullptr},
                    {"TMW_VT_free", reinterpret_cast<void*>(&vt_free), nullptr},
                    {"TMW_VT_getResult", reinterpret_cast<void*>(&vt_get_result), nullptr},
                    {"TMW_VT_switchVtDic", reinterpret_cast<void*>(&vt_switch), nullptr}};
bool absent = false, refuse_exec_watcher = false;
unsigned watchers = 0;
TSUCHI_Mod* framework = nullptr;
char image[16];
NativeHooks native{};
std::vector<void (*)()> frame_steps;
std::vector<std::string> notes;
std::vector<uint64_t> heard;

template <class Function> Function through(Function stock) {
    for (const Symbol& symbol : symbols)
        if (symbol.stock == reinterpret_cast<void*>(stock)) return reinterpret_cast<Function>(symbol.watcher ? symbol.watcher : symbol.stock);
    return stock;
}

TSUCHI_Result watch_function(TSUCHI_Mod* mod, uint64_t offset, uint32_t expected, void* function, uint64_t* id, void** original) {
    CHECK(mod == framework && expected == 0);
    for (Symbol& symbol : symbols) {
        if (reinterpret_cast<uint64_t>(symbol.stock) != game::main_base() + offset) continue;
        if (refuse_exec_watcher && !strcmp(symbol.name, "TMW_VT_exec")) return TSUCHI_BUSY;
        CHECK(!symbol.watcher);            // each function is watched once
        symbol.watcher = function;
        *id = ++watchers;
        *original = symbol.stock;
        return TSUCHI_OK;
    }
    CHECK(false);
    return TSUCHI_NOT_FOUND;
}

uint8_t game_work[0x400], second_work[0x400], third_work[0x400], settings[0x40], other_settings[0x40], mode[0x10];
void* game_handle = nullptr;       // the instance the game listens with
int16_t pcm[128], flag[4];

// The game sets up an instance of its own, the way it asks for the work size first, and listens with it.
Made* game_sets_up(const uint8_t* dictionary, const void* with, uint32_t a, uint64_t e, uint8_t* work = game_work) {
    alignas(8) uint8_t info[0x100] = {};
    CHECK(through(&vt_get_info)(dictionary, 0x1000090, mode, 11, 22, 600, 1, e, info) == 0);
    CHECK(u32(info) == work_for(dictionary));
    CHECK(through(&vt_init)(&game_handle, dictionary, with, work, work_for(dictionary), 0x1000090, mode, a, a + 1, a + 2) == 0);
    CHECK(game_handle == work);
    made.back().games = true;
    return &made.back();
}
void game_gives_audio(const void* audio = pcm) {
    in_exec = true;
    CHECK(through(&vt_exec)(game_handle, audio, nullptr, flag, nullptr, nullptr) == 0);
    in_exec = false;
}
void frame() {
    const auto steps = frame_steps;
    for (auto step : steps) step();
}
void on_heard(void* user, const TSUCHI_HowloutHeard* event) {
    CHECK(user == &heard && !in_exec);
    heard.push_back(event->howlout.id);
}
size_t unfreed() {
    size_t count = 0;
    for (const Made& one : made) count += !one.games && !one.freed;
    return count;
}

} // namespace

namespace tsuchi {
bool each_frame(void (*step)()) { frame_steps.push_back(step); return true; }
void mod_note(const char* id, const char* text) { CHECK(!strcmp(id, kFrameworkLogId)); notes.push_back(text); }
const NativeHooks* native_hooks_backend() { return &native; }
namespace game {
uint64_t main_base() { return reinterpret_cast<uint64_t>(image); }
bool readable(uint64_t, uint64_t) { return false; }
uint64_t module_containing(uint64_t address) { return address == main_base() ? address : 0; }
uint64_t module_symbol(uint64_t module, const char* name) {
    CHECK(module == main_base());
    for (const Symbol& symbol : symbols)
        if (!absent && !strcmp(symbol.name, name)) return reinterpret_cast<uint64_t>(symbol.stock);
    return 0;
}
} // namespace game
} // namespace tsuchi

int main(int argc, char** argv) {
    absent = argc > 1 && !strcmp(argv[1], "absent");
    set_allocator({counted_allocate, counted_release});
    native.function = watch_function;
    framework = mod_create("tsuchi.self");
    TSUCHI_Mod* mod = mod_create("howlout.check");
    TSUCHI_Mod* other = mod_create("howlout.other");
    CHECK(framework && mod && other);
    const TSUCHI_HowloutService& service = howlout::table;
    TSUCHI_Howlout first{}, second{}, third{}, theirs{}, none{};
    uint32_t value = 0;

    // Before the service starts there is nothing behind its table.
    CHECK(service.load_sounds(mod, rows, sizeof rows) == TSUCHI_UNSUPPORTED);

    if (absent) {
        // A build without Howl-Outs: nothing is watched, and every call says so.
        howlout_start(framework);
        frame();
        CHECK(watchers == 0);
        CHECK(service.load_sounds(mod, rows, sizeof rows) == TSUCHI_UNSUPPORTED);
        CHECK(service.add(mod, "K AH0", 300, &first) == TSUCHI_UNSUPPORTED && !first.id);
        if (!failures) printf("howlout runtime passed without Howl-Outs\n");
        return failures ? 1 : 0;
    }

    // One watcher is refused at first. Nothing works until all four are on, and each is put on once.
    refuse_exec_watcher = true;
    howlout_start(framework);
    CHECK(frame_steps.size() == 1 && watchers == 2);
    CHECK(service.load_sounds(mod, rows, sizeof rows) == TSUCHI_UNSUPPORTED);
    refuse_exec_watcher = false;
    frame();
    CHECK(watchers == 4);
    TSUCHI_Subscription subscription{};
    CHECK(service.on_heard(mod, on_heard, &heard, &subscription) == TSUCHI_OK);

    // Sound rows come first, and each mod gives them itself.
    CHECK(service.add(mod, "K AH0", 400, &first) == TSUCHI_NOT_FOUND);
    CHECK(service.load_sounds(mod, stock, sizeof stock) == TSUCHI_INVALID);
    CHECK(service.load_sounds(mod, rows, sizeof rows) == TSUCHI_OK);
    CHECK(service.load_sounds(mod, rows, sizeof rows) == TSUCHI_OK);
    CHECK(service.add(other, "K AH0", 400, &first) == TSUCHI_NOT_FOUND);
    const size_t idle = blocks.size();

    // What a phrase can be is known from the sound rows alone.
    std::string long_phrase;
    for (int i = 0; i < 43; ++i) long_phrase += i % 2 ? "AH0 " : "K ";
    CHECK(service.add(mod, "", 300, &first) == TSUCHI_INVALID);
    CHECK(service.add(mod, "K T", 300, &first) == TSUCHI_INVALID);
    CHECK(service.add(mod, "K AH0", 1001, &first) == TSUCHI_INVALID);
    CHECK(service.add(mod, long_phrase.c_str(), 300, &first) == TSUCHI_INVALID);
    CHECK(!first.id && made.empty() && blocks.size() == idle);

    // Before the game has set up its own recognizer, a phrase waits.
    CHECK(service.add(mod, "K AH0", 400, &first) == TSUCHI_OK && first.id);
    CHECK(service.threshold(mod, first, &value) == TSUCHI_OK && value == 400);
    frame();
    CHECK(made.empty());

    // The game sets up its own, and the watchers pass everything through as it was.
    Made* games = game_sets_up(stock, settings, 31, 5);
    CHECK(made.size() == 1 && games->settings == settings && games->a == 31 && games->c == 33);
    CHECK(last_asked.dictionary == stock && last_asked.a == 11 && last_asked.d == 1 && last_asked.e == 5);

    // The next frame gives the waiting phrase an instance, set up the way the game set up its own.
    frame();
    CHECK(made.size() == 2);
    Made* one = &made.back();
    CHECK(one->settings == settings && one->mode == mode && one->tag == 0x1000090 && one->a == 31 && one->b == 32 && one->c == 33);
    CHECK(last_asked.tag == 0x1000090 && last_asked.mode == mode && last_asked.a == 11 && last_asked.b == 22 &&
          last_asked.c == 600 && last_asked.d == 1 && last_asked.e == 5);
    CHECK(count_of(one->dictionary.data()) == 1 && threshold_of(one, 0) == 400 && one->frames == 0);
    frame();
    CHECK(made.size() == 2);

    // It hears the audio the game gives its own instance, and only when there is audio.
    game_gives_audio();
    CHECK(games->frames == 1 && one->frames == 1 && one->last_pcm == pcm && !flag[0]);
    game_gives_audio(nullptr);
    CHECK(games->frames == 2 && one->frames == 1);

    // Heard inside the recognizer, reported at the next frame, once. The game's own results stay the game's.
    one->says = 0;
    game_gives_audio();
    CHECK(heard.empty() && !flag[0]);
    frame();
    CHECK(heard.size() == 1 && heard[0] == first.id);
    frame();
    CHECK(heard.size() == 1);
    games->says = 3;
    game_gives_audio();
    CHECK(flag[0] == 1);
    flag[0] = 0;
    frame();
    CHECK(heard.size() == 1);

    // A second phrase makes an instance with both. It takes over inside the game's next call and carries on
    // from what the first has heard; the first one's memory goes at the frame after.
    const size_t steady = blocks.size();
    CHECK(service.add(mod, "AH0 K AH0", 250, &second) == TSUCHI_OK && second.id && second.id != first.id);
    CHECK(made.size() == 3);
    Made* both = &made.back();
    CHECK(both->dictionary.size() == sizeof expected && !memcmp(both->dictionary.data(), expected, sizeof expected));
    CHECK(!one->freed && switches == 0 && blocks.size() == steady + 3);
    const unsigned heard_by_one = one->frames;
    game_gives_audio();
    CHECK(switches == 1 && switched_from == one->handle && switched_to == both->handle && both->carried == heard_by_one);
    CHECK(one->freed && one->frames == heard_by_one && both->frames == 1 && blocks.size() == steady + 3);
    frame();
    CHECK(blocks.size() == steady);
    both->says = 1;
    game_gives_audio();
    frame();
    CHECK(heard.size() == 2 && heard[1] == second.id);

    // A phrase is its owner's to change, and anyone's to read.
    CHECK(service.remove(other, first) == TSUCHI_DENIED && service.set_threshold(other, first, 100) == TSUCHI_DENIED);
    CHECK(service.set_threshold(mod, first, 1001) == TSUCHI_INVALID);
    CHECK(service.threshold(other, first, &value) == TSUCHI_OK && value == 400);

    // Two changes before the game gives audio again: the instance between them is never used.
    CHECK(service.set_threshold(mod, first, 0) == TSUCHI_OK);
    Made* never = &made.back();
    CHECK(threshold_of(never, 0) == 0 && threshold_of(never, 1) == 250);
    CHECK(service.set_threshold(mod, first, 500) == TSUCHI_OK);
    Made* latest = &made.back();
    CHECK(never != latest && never->freed && never->frames == 0 && !both->freed);
    CHECK(service.threshold(mod, first, &value) == TSUCHI_OK && value == 500);
    game_gives_audio();
    CHECK(switches == 2 && switched_from == both->handle && switched_to == latest->handle && both->freed);
    frame();
    CHECK(blocks.size() == steady && unfreed() == 1);

    // A phrase removed after it was heard and before the frame is not reported, and its handle ends.
    latest->says = 0;
    game_gives_audio();
    CHECK(service.remove(mod, first) == TSUCHI_OK);
    frame();
    CHECK(heard.size() == 2);
    CHECK(service.remove(mod, first) == TSUCHI_NOT_FOUND && service.threshold(mod, first, &value) == TSUCHI_NOT_FOUND &&
          service.set_threshold(mod, first, 300) == TSUCHI_NOT_FOUND);
    Made* alone = &made.back();
    CHECK(count_of(alone->dictionary.data()) == 1 && threshold_of(alone, 0) == 250);
    alone->says = 0;
    game_gives_audio();
    frame();
    CHECK(heard.size() == 3 && heard[2] == second.id);

    // Its place is used again under another handle, and entries follow the places.
    CHECK(service.add(mod, "K AH0", 300, &third) == TSUCHI_OK && third.id != first.id && (third.id & 0xff) == (first.id & 0xff));
    CHECK(service.threshold(mod, first, &value) == TSUCHI_NOT_FOUND);
    Made* pair = &made.back();
    CHECK(count_of(pair->dictionary.data()) == 2 && threshold_of(pair, 0) == 300 && threshold_of(pair, 1) == 250);
    pair->says = 0;
    game_gives_audio();
    frame();
    CHECK(heard.size() == 4 && heard[3] == third.id);

    // The recognizer refusing a dictionary, or memory running out at any point, leaves everything as it was,
    // and nothing is tried again by itself.
    const size_t instances = made.size();
    refuse_info = true;
    CHECK(service.add(mod, "K AH0", 300, &none) == TSUCHI_INVALID);
    refuse_info = false;
    refuse_init = true;
    CHECK(service.add(mod, "K AH0", 300, &none) == TSUCHI_INVALID);
    CHECK(service.set_threshold(mod, third, 700) == TSUCHI_INVALID);
    refuse_init = false;
    for (int granted = 0; granted < 3; ++granted) {
        refuse_after = granted;
        CHECK(service.add(mod, "K AH0", 300, &none) == TSUCHI_NO_MEMORY);
        refuse_after = -1;
    }
    CHECK(!none.id && notes.size() == 3 && blocks.size() == steady && made.size() == instances);
    CHECK(service.threshold(mod, third, &value) == TSUCHI_OK && value == 300);
    frame();
    game_gives_audio();
    CHECK(made.size() == instances && !pair->freed && unfreed() == 1);

    // Fifty phrases at most.
    std::vector<TSUCHI_Howlout> many;
    for (TSUCHI_Result result = TSUCHI_OK; result == TSUCHI_OK && many.size() < 60;) {
        TSUCHI_Howlout next{};
        result = service.add(mod, "K AH0", 300, &next);
        if (result == TSUCHI_OK) many.push_back(next);
        else CHECK(result == TSUCHI_CAPACITY);
    }
    CHECK(many.size() == 48 && count_of(made.back().dictionary.data()) == 50);
    for (TSUCHI_Howlout extra : many) CHECK(service.remove(mod, extra) == TSUCHI_OK);
    game_gives_audio();
    frame();
    CHECK(blocks.size() == steady && unfreed() == 1 && count_of(made.back().dictionary.data()) == 2);

    // One sound rows file at a time, whoever gives it.
    static uint8_t other_rows[sizeof rows];
    memcpy(other_rows, rows, sizeof rows);
    other_rows[sizeof rows - 1] ^= 1;
    CHECK(service.load_sounds(other, other_rows, sizeof other_rows) == TSUCHI_BUSY);
    CHECK(service.load_sounds(other, rows, sizeof rows) == TSUCHI_OK);
    CHECK(service.add(other, "K AH0", 350, &theirs) == TSUCHI_OK && service.remove(mod, theirs) == TSUCHI_DENIED);
    game_gives_audio();
    frame();
    Made* three = &made.back();
    CHECK(count_of(three->dictionary.data()) == 3 && three->frames == 1 && unfreed() == 1);

    // The game keeps an instance for each dictionary it has loaded and gives audio to one at a time. While it
    // listens with one the sound rows are not for, the service's still hears, and a change of phrases takes
    // its settings from the one the rows are for, as before.
    static uint8_t other_stock[sizeof stock];
    memcpy(other_stock, stock, sizeof stock);
    other_stock[0x80 + 0x30] ^= 1;
    void* const first_handle = game_handle;
    Made* foreign = game_sets_up(other_stock, other_settings, 51, 7, second_work);
    game_gives_audio();
    CHECK(foreign->frames == 1 && three->frames == 2);
    CHECK(service.set_threshold(other, theirs, 360) == TSUCHI_OK);
    Made* same_source = &made.back();
    CHECK(same_source != foreign && same_source->settings == settings && same_source->a == 31 && notes.size() == 3);
    unsigned carried_over = switches;
    game_gives_audio();
    CHECK(switches == carried_over + 1 && three->freed && same_source->frames == 1);

    // With a second instance the rows are for, set up differently and now the one listened with, a change
    // takes its settings from that one, and nothing is carried over between the two.
    Made* twin = game_sets_up(stock, other_settings, 61, 8, third_work);
    game_gives_audio();
    CHECK(twin->frames == 1 && same_source->frames == 2);
    CHECK(service.set_threshold(other, theirs, 350) == TSUCHI_OK);
    Made* from_twin = &made.back();
    CHECK(from_twin->settings == other_settings && from_twin->a == 61 && last_asked.e == 8);
    carried_over = switches;
    game_gives_audio();
    CHECK(switches == carried_over && same_source->freed && from_twin->frames == 1);

    // The game ends those two. The service's goes with the one it had its settings from, and the next frame
    // makes one from the instance that is left.
    CHECK(through(&vt_free)(second_work) == 0 && through(&vt_free)(third_work) == 0);
    game_handle = first_handle;
    game_gives_audio();
    CHECK(from_twin->freed);
    frame();
    three = &made.back();
    CHECK(three->settings == settings && count_of(three->dictionary.data()) == 3 && threshold_of(three, 2) == 350);
    game_gives_audio();
    frame();
    CHECK(three->frames == 1 && unfreed() == 1 && blocks.size() == steady);

    // More heard between two frames than is kept: the first sixteen are reported.
    for (int i = 0; i < 20; ++i) {
        three->says = 2;
        game_gives_audio();
    }
    frame();
    CHECK(heard.size() == 20 && heard[4] == theirs.id && heard[19] == theirs.id);

    // The game ends its own instance and makes another at the same address. The service's stops being used
    // at once, inside the game's next call, because its settings were the game's to keep.
    CHECK(through(&vt_free)(game_handle) == 0 && games->freed);
    frame();
    frame();
    CHECK(made.back().handle == three->handle && !three->freed);     // nothing to take settings from: the phrases wait

    // The game's next dictionary has another network than the sound rows were trained for. That is noted once,
    // and no frame tries again until the game makes another instance.
    const unsigned heard_by_three = three->frames;
    const size_t made_before = made.size();
    foreign = game_sets_up(other_stock, settings, 31, 5);
    game_gives_audio();
    CHECK(three->freed && three->frames == heard_by_three && foreign->frames == 1);
    frame();
    frame();
    game_gives_audio();
    CHECK(made.size() == made_before + 1 && notes.size() == 4 && unfreed() == 0 && blocks.size() == steady - 3);

    // Then it listens in the rows' language again, with other settings. The service's new instance has them,
    // and there is nothing to carry over.
    CHECK(through(&vt_free)(game_handle) == 0 && foreign->freed);
    Made* games_again = game_sets_up(stock, other_settings, 41, 6);
    frame();
    Made* renewed = &made.back();
    CHECK(renewed != games_again && renewed->settings == other_settings && renewed->a == 41 && renewed->c == 43 && last_asked.e == 6);
    CHECK(count_of(renewed->dictionary.data()) == 3);
    carried_over = switches;
    game_gives_audio();
    CHECK(renewed->frames == 1 && switches == carried_over);
    renewed->says = 1;
    game_gives_audio();
    frame();
    CHECK(heard.size() == 21 && heard[20] == second.id);

    // A stopped mod's phrases end with it. With none left, the instance ends inside the game's next call.
    const unsigned switched = switches;
    mod_stop(other);
    frame();
    Made* without = &made.back();
    CHECK(without != renewed && count_of(without->dictionary.data()) == 2 && !without->freed);
    mod_stop(mod);
    frame();
    CHECK(&made.back() == without && without->freed && without->frames == 0 && !renewed->freed);
    const unsigned heard_by_renewed = renewed->frames;
    game_gives_audio();
    CHECK(renewed->freed && renewed->frames == heard_by_renewed && switches == switched && unfreed() == 0);
    frame();
    game_gives_audio();
    frame();
    CHECK(blocks.size() <= idle && heard.size() == 21);
    for (const Made& gone : made) CHECK(gone.games || !blocks.count(gone.handle));
    CHECK(service.add(mod, "K AH0", 300, &none) == TSUCHI_INVALID && !none.id);

    if (!failures) printf("howlout runtime passed\n");
    return failures ? 1 : 0;
}
