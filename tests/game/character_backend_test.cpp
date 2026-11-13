// The character service against a made-up game, with the real generated validation, registry, and holds: a captain
// and a dog with their steered parts and controller objects, the four controller entries the service wraps, and the
// inline hooks it puts on the action sites. Host evidence, not a game run.
#include "actor_events.h"
#include "character_service.h"
#include "frame.h"
#include "game.h"
#include "hooks.h"
#include "objects.h"
#include "ordinary_lifecycle.h"
#include "people.h"
#include "registry.h"
#include "tsuchi/hook.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace t = tsuchi::game::table;
using namespace tsuchi;

#define CHECK(x)                                                                                                       \
    do {                                                                                                               \
        ++checks;                                                                                                      \
        if (!(x)) {                                                                                                    \
            std::fprintf(stderr, "line %d: %s\n", __LINE__, #x);                                                       \
            std::exit(1);                                                                                              \
        }                                                                                                              \
    } while (0)
static unsigned checks = 0;
static std::vector<void (*)()> steps;
static void (*actor_left)(uint64_t, uint64_t, ActorLeft) = nullptr;
static std::vector<std::pair<uint64_t, uint64_t>> ranges;
static std::set<uint64_t> live_objects;

// ---- The made-up game -----------------------------------------------------------------------------------------

// A controller object: its function table, and the pad's own logical inputs as the game's readers see them.
struct Controller {
    uint64_t table;
    alignas(8) uint8_t body[0xB8];
    uint64_t pad_held = 0, pad_pressed = 0, pad_released = 0;
    uint64_t at() const { return reinterpret_cast<uint64_t>(this); }
};
static uint64_t functions[0x100 / 8];
static bool lock_all = false;
static unsigned stock_updates = 0;
static Controller* as_controller(void* c) { return static_cast<Controller*>(c); }
static bool locked(void*) { return lock_all; }
static bool stock_held(void* c, int32_t id) { return as_controller(c)->pad_held >> id & 1; }
static bool stock_pressed(void* c, int32_t id, void*, void*) { return as_controller(c)->pad_pressed >> id & 1; }
static bool stock_released(void* c, int32_t id) { return as_controller(c)->pad_released >> id & 1; }
// The game's update fills the press-age table by asking the controller's own pressed entry about each input
// (0x2044510): an input pressed this frame has its age set to 0. The stand-in asks about input 3 and keeps its age.
static float whistle_age = 100;
static bool (*pressed_through_table)(void*, int32_t, void*, void*) = nullptr;
static void stock_update(void* c, float seconds) {
    ++stock_updates;
    if (pressed_through_table) whistle_age = pressed_through_table(c, 3, nullptr, nullptr) ? 0 : whistle_age + seconds;
}

// What the service's wrappers are, by the entry they went on.
using Held = bool (*)(void*, int32_t);
using Pressed = bool (*)(void*, int32_t, void*, void*);
using Update = void (*)(void*, float);
static std::map<uint64_t, void*> wrapped;
static unsigned wraps = 0;

struct Part {
    alignas(8) uint8_t body[0x800];
    uint64_t at() const { return reinterpret_cast<uint64_t>(this); }
};
struct Actor {
    alignas(8) uint8_t body[0x400];
    bool captain = false;
    uint64_t at() const { return reinterpret_cast<uint64_t>(this); }
};
static Actor captain, dog, rock;
static Part steered_captain, steered_dog;
static Controller captain_pad, dog_pad;

template <class T> void put(uint64_t at, const T& value) { std::memcpy(reinterpret_cast<void*>(at), &value, sizeof value); }
template <class T> T get(uint64_t at) {
    T v;
    std::memcpy(&v, reinterpret_cast<void*>(at), sizeof v);
    return v;
}

namespace tsuchi::game {
uint64_t main_base() { return 0x10000000; }
bool readable(uint64_t at, uint64_t size) {
    for (auto r : ranges)
        if (at >= r.first && at - r.first <= r.second && size <= r.second - (at - r.first)) return true;
    return false;
}
bool writable(uint64_t at, uint64_t size) { return readable(at, size); }
bool executable(uint64_t at) { return at > 0x10000; }
bool live(uint64_t at) { return live_objects.count(at) != 0; }
uint64_t from_handle(uint64_t h) { return live(h) ? h : 0; }
uint64_t to_handle(uint64_t a) { return live(a) ? a : 0; }
bool is_game_actor(uint64_t a) { return a == captain.at() || a == dog.at() || a == rock.at(); }
uint64_t part_owner(uint64_t) { return 0; }
uint64_t captain_part(uint64_t a) { return a == captain.at() && live(a) ? steered_captain.at() : 0; }
uint64_t player_captain(uint32_t p) { return p == 0 && live(captain.at()) ? captain.at() : 0; }
uint64_t player_dog(uint32_t p) { return p == 0 && live(dog.at()) ? dog.at() : 0; }
} // namespace tsuchi::game

namespace tsuchi {
bool frame_stepping() { return false; }
bool each_frame(void (*f)()) { return steps.push_back(f), true; }
bool after_step(void (*)()) { return true; }
bool on_actor_left(void (*f)(uint64_t, uint64_t, ActorLeft)) { return actor_left = f, true; }
void ordinary_lifecycle_listener(void (*)(uint64_t, uint64_t, bool)) {}
TSUCHI_Result ordinary_lifecycle_watch(uint64_t) { return TSUCHI_UNSUPPORTED; }
uint64_t ordinary_lifecycle_restore_part(uint64_t h, uint64_t) { return game::from_handle(h); }
TSUCHI_Result hook_wrap(TSUCHI_Mod*, uint64_t entry, uint64_t stock, void* f, uint32_t flags, uint64_t* id, void** next) {
    CHECK(flags == 0);
    const uint64_t offset = entry - game::main_base(), stock_at = stock - game::main_base();
    CHECK(wrapped.count(offset) == 0);
    CHECK(offset >= t::slot::controller_held.table && offset < t::slot::controller_held.table + 0x100);
    // The stock function each entry holds: its address in the table says which.
    void* original = nullptr;
    if (stock_at == t::slot::controller_held.stock) original = reinterpret_cast<void*>(&stock_held);
    else if (stock_at == t::slot::controller_pressed.stock) original = reinterpret_cast<void*>(&stock_pressed);
    else if (stock_at == t::slot::controller_released.stock) original = reinterpret_cast<void*>(&stock_released);
    else if (stock_at == t::slot::controller_update.stock) original = reinterpret_cast<void*>(&stock_update);
    CHECK(original);
    wrapped[offset] = f;
    *next = original;
    *id = ++wraps;
    return TSUCHI_OK;
}

// Inline hooks, by site offset: the handler, and the word the service expected there.
struct Placed {
    void (*handler)(TSUCHI_InlineContext*);
    uint32_t expected;
};
static std::map<uint64_t, Placed> placed;
static unsigned inner_calls = 0;
static void inner(TSUCHI_InlineContext*) { ++inner_calls; }
static TSUCHI_Result refuse_inline = TSUCHI_OK;
static TSUCHI_Result inline_hook(TSUCHI_Mod*, uint64_t offset, uint32_t expected, void* f, uint64_t* id, void** next) {
    if (refuse_inline != TSUCHI_OK) return refuse_inline;
    CHECK(placed.count(offset) == 0);
    placed[offset] = {reinterpret_cast<void (*)(TSUCHI_InlineContext*)>(f), expected};
    *id = offset;
    *next = reinterpret_cast<void*>(&inner);
    return TSUCHI_OK;
}
static const NativeHooks native{nullptr, inline_hook, nullptr, nullptr, nullptr};
static const NativeHooks* native_now = nullptr;
const NativeHooks* native_hooks_backend() { return native_now; }
void characters_start(TSUCHI_Mod*);
// The framework's lines in the log.
static std::vector<std::string> notes;
void mod_note(const char* id, const char* text) {
    CHECK(!std::strcmp(id, "tsuchi"));
    notes.push_back(text);
}
} // namespace tsuchi

// ---- Helpers ----------------------------------------------------------------------------------------------------

static void step() {
    for (auto f : steps) f();
}
static Held held_entry() { return reinterpret_cast<Held>(wrapped.at(t::slot::controller_held.table + t::slot::controller_held.offset)); }
static Pressed pressed_entry() { return reinterpret_cast<Pressed>(wrapped.at(t::slot::controller_pressed.table + t::slot::controller_pressed.offset)); }
static Held released_entry() { return reinterpret_cast<Held>(wrapped.at(t::slot::controller_released.table + t::slot::controller_released.offset)); }
static Update update_entry() { return reinterpret_cast<Update>(wrapped.at(t::slot::controller_update.table + t::slot::controller_update.offset)); }
static bool held(Controller& c, int id) { return held_entry()(&c, id); }
static bool pressed(Controller& c, int id) { return pressed_entry()(&c, id, nullptr, nullptr); }
static bool released(Controller& c, int id) { return released_entry()(&c, id); }
// One frame of the game: the controller manager updates each controller, then the world steps.
static void frame() {
    update_entry()(&captain_pad, 1 / 60.f);
    update_entry()(&dog_pad, 1 / 60.f);
    step();
}

static uint64_t site_offset(unsigned site) { return uint64_t(t::tables::character_sites[site]); }
// Runs a site's hook with these registers. Returns the registers after.
struct Frame {
    uint64_t x[31] = {};
    uint64_t q[64] = {};
};
static Frame at_site(unsigned site, Frame frame) {
    CHECK(placed.count(site_offset(site)) == 1);
    TSUCHI_InlineContext context{frame.x, frame.q, 0, 0, 0, 0};
    const unsigned before = inner_calls;
    placed.at(site_offset(site)).handler(&context);
    CHECK(inner_calls == before + 1); // the chain goes on to the older handlers
    return frame;
}
static bool reader_stub(void*, int32_t, void*, void*) { return false; }
// A call of the controller's reader as the game makes it at a site: x0 the controller, w1 the input, the reader in
// the word's register.
static Frame call_frame(unsigned site, uint64_t part, uint64_t controller, uint32_t input) {
    Frame frame;
    const uint32_t reg = uint32_t(t::tables::character_site_registers[site]);
    if (reg < 31) frame.x[reg] = part;
    else if (reg > 31) frame.x[reg - 32] = controller;
    frame.x[0] = controller;
    frame.x[1] = input;
    frame.x[uint32_t(t::tables::character_site_words[site]) >> 5 & 31] = reinterpret_cast<uint64_t>(&reader_stub);
    return frame;
}
static uint64_t target_of(unsigned site, const Frame& frame) { return frame.x[uint32_t(t::tables::character_site_words[site]) >> 5 & 31]; }
static bool call_answer(uint64_t target) { return reinterpret_cast<Pressed>(target)(nullptr, 0, nullptr, nullptr); }

int main() {
    set_allocator({std::malloc, std::free});
    set_thread_probe([]() -> uint64_t { return 1; });
    note_game_thread();
    for (const uint64_t object : {captain.at(), dog.at(), rock.at(), steered_captain.at(), steered_dog.at(), captain_pad.at(), dog_pad.at()})
        live_objects.insert(object);
    ranges.push_back({captain.at(), sizeof captain.body});
    ranges.push_back({dog.at(), sizeof dog.body});
    ranges.push_back({rock.at(), sizeof rock.body});
    ranges.push_back({steered_captain.at(), sizeof steered_captain.body});
    ranges.push_back({steered_dog.at(), sizeof steered_dog.body});
    ranges.push_back({captain_pad.at(), sizeof(Controller)});
    ranges.push_back({dog_pad.at(), sizeof(Controller)});
    ranges.push_back({reinterpret_cast<uint64_t>(functions), sizeof functions});
    functions[uint64_t(t::game_controller_table::locked) / 8] = reinterpret_cast<uint64_t>(&locked);
    functions[uint64_t(t::game_controller_table::pressed) / 8] = reinterpret_cast<uint64_t>(&stock_pressed);
    for (Controller* c : {&captain_pad, &dog_pad}) {
        c->table = reinterpret_cast<uint64_t>(functions);
        put<int32_t>(c->at() + t::game_controller::unpaused_frames, 10);
    }
    put<uint64_t>(captain.at() + t::game_actor::player, steered_captain.at());
    put<uint64_t>(dog.at() + t::game_actor::player, steered_dog.at());
    put<uint8_t>(dog.at() + t::game_actor::sort, uint8_t(t::constant::sort_dog));
    put<uint64_t>(steered_captain.at() + t::character_part::controller, captain_pad.at());
    put<uint64_t>(steered_dog.at() + t::character_part::controller, dog_pad.at());

    TSUCHI_Mod *a = mod_create("character.a"), *b = mod_create("character.b"), *self = mod_create("tsuchi.self");
    characters_start(self);
    const auto& c = character::table;
    const TSUCHI_Actor cap{captain.at()}, pup{dog.at()}, stone{rock.at()}, gone{0x1234};
    bool on = true;
    TSUCHI_Input input = 0;

    // ---- Logical inputs ---------------------------------------------------------------------------------------
    CHECK(c.hold_input(a, stone, TSUCHI_INPUT_WHISTLE) == TSUCHI_NOT_FOUND);
    CHECK(c.hold_input(a, gone, TSUCHI_INPUT_WHISTLE) == TSUCHI_NOT_FOUND);
    CHECK(c.hold_input(a, cap, TSUCHI_INPUT_NONE) == TSUCHI_INVALID);
    CHECK(c.hold_input(a, cap, 40) == TSUCHI_INVALID); // not a value of the enum
    CHECK(wrapped.empty());                            // nothing is wrapped until a mod holds an input
    // An action held before any input: it presses and holds its input at the controller all the same, which
    // needs the controller's entries. A game run had a held whistle start nothing without them.
    native_now = &native;
    CHECK(c.hold_action(a, cap, TSUCHI_ACTION_SWITCH_TYPE_BACK) == TSUCHI_OK);
    CHECK(wrapped.size() == 4);
    // The log tells where the counters are, once.
    CHECK(notes.size() == 1 && notes[0].find("character: debug counters at 0x") == 0);
    const uint64_t counters = std::strtoull(notes[0].c_str() + std::strlen("character: debug counters at "), nullptr, 16);
    CHECK(get<uint32_t>(counters) == 0x44524843 && get<uint32_t>(counters + 4) == 23 && get<uint32_t>(counters + 8) == 1);
    pressed_through_table = pressed_entry();
    frame();
    CHECK(held(captain_pad, 12) && pressed(captain_pad, 12));
    CHECK(get<uint32_t>(counters + 12) >= 1); // the update latched it
    {
        Frame early = at_site(6, call_frame(6, steered_captain.at(), captain_pad.at(), 12));
        CHECK(call_answer(target_of(6, early)));
        // Read again in the same frame, the press is still down, as a button's lasts its frame; after the next
        // update it is up.
        early = at_site(6, call_frame(6, steered_captain.at(), captain_pad.at(), 12));
        CHECK(call_answer(target_of(6, early)));
        early = at_site(6, call_frame(6, steered_dog.at(), dog_pad.at(), 12));
        frame();
        early = at_site(6, call_frame(6, steered_captain.at(), captain_pad.at(), 12));
        CHECK(!call_answer(target_of(6, early)));
        // Site 6's counts: four reads, three for the captain, two answered down and one up.
        const uint64_t at = counters + 48 + 6 * 16;
        CHECK(get<uint32_t>(at) == 4 && get<uint32_t>(at + 4) == 3 && get<uint32_t>(at + 8) == 2 && get<uint32_t>(at + 12) == 1);
        CHECK(get<uint64_t>(counters + 32) == captain_pad.at() || get<uint64_t>(counters + 32) == dog_pad.at());
    }
    // Every 120 steps while an action is held, the log has what the sites did.
    for (int n = 0; n < 120; ++n) frame();
    CHECK(notes.size() == 2 && notes[1].find("character: updates ") == 0 && notes[1].find("; site 6 4/3/2/1") != std::string::npos);
    CHECK(c.release_action(a, cap, TSUCHI_ACTION_SWITCH_TYPE_BACK) == TSUCHI_OK);
    frame();
    frame();
    frame();
    CHECK(!held(captain_pad, 12));
    native_now = nullptr;
    CHECK(c.hold_input(a, cap, TSUCHI_INPUT_WHISTLE) == TSUCHI_OK);
    CHECK(wrapped.size() == 4 && notes.size() == 2);
    CHECK(c.input_held(a, cap, TSUCHI_INPUT_WHISTLE, &on) == TSUCHI_OK && on);
    CHECK(c.input_held(a, pup, TSUCHI_INPUT_WHISTLE, &on) == TSUCHI_OK && !on);
    CHECK(!held(captain_pad, 3)); // until the controller's update latches it
    const unsigned updates_before = stock_updates;
    update_entry()(&captain_pad, 0.016f);
    CHECK(stock_updates == updates_before + 1); // the game's own update still runs
    CHECK(held(captain_pad, 3) && pressed(captain_pad, 3) && !released(captain_pad, 3));
    CHECK(!held(dog_pad, 3) && !held(captain_pad, 4)); // only that character, only that input
    frame();
    CHECK(held(captain_pad, 3) && !pressed(captain_pad, 3)); // pressed for one frame
    // The pad's own inputs still reach the game.
    captain_pad.pad_held = 1u << 8;
    CHECK(held(captain_pad, 8) && held(captain_pad, 3));
    captain_pad.pad_held = 0;
    // Exclusive per character and input.
    CHECK(c.hold_input(b, cap, TSUCHI_INPUT_WHISTLE) == TSUCHI_BUSY);
    CHECK(c.press_input(b, cap, TSUCHI_INPUT_WHISTLE) == TSUCHI_BUSY);
    CHECK(c.press_input(a, cap, TSUCHI_INPUT_WHISTLE) == TSUCHI_BUSY);
    CHECK(c.hold_input(b, cap, TSUCHI_INPUT_DISMISS) == TSUCHI_OK);
    CHECK(c.release_input(b, cap, TSUCHI_INPUT_WHISTLE) == TSUCHI_NOT_FOUND);
    frame();
    CHECK(held(captain_pad, 8) && pressed(captain_pad, 8) && !pressed(captain_pad, 3));

    // The game's locks hide held inputs, as they hide the pad's.
    lock_all = true;
    CHECK(!held(captain_pad, 3) && !held(captain_pad, 8));
    lock_all = false;
    put<float>(captain_pad.at() + t::game_controller::lock_timer, 0.5f);
    put<uint64_t>(captain_pad.at() + t::game_controller::lock_mask, uint64_t(1) << 3);
    CHECK(!held(captain_pad, 3) && held(captain_pad, 8));
    put<float>(captain_pad.at() + t::game_controller::lock_timer, 0.0f);
    CHECK(held(captain_pad, 3));

    // Released: up on the next update, released for one frame.
    CHECK(c.release_input(a, cap, TSUCHI_INPUT_WHISTLE) == TSUCHI_OK);
    CHECK(c.input_held(a, cap, TSUCHI_INPUT_WHISTLE, &on) == TSUCHI_OK && !on);
    CHECK(held(captain_pad, 3)); // until the update
    frame();
    CHECK(!held(captain_pad, 3) && released(captain_pad, 3) && held(captain_pad, 8));
    frame();
    CHECK(!released(captain_pad, 3));

    // A press: pressed and held for one frame, released on the next, and its hold is over.
    CHECK(c.press_input(a, pup, TSUCHI_INPUT_DOG_JUMP) == TSUCHI_OK);
    CHECK(c.input_held(a, pup, TSUCHI_INPUT_DOG_JUMP, &on) == TSUCHI_OK && on);
    frame();
    CHECK(held(dog_pad, 20) && pressed(dog_pad, 20));
    frame();
    CHECK(!held(dog_pad, 20) && !pressed(dog_pad, 20) && released(dog_pad, 20));
    CHECK(c.input_held(a, pup, TSUCHI_INPUT_DOG_JUMP, &on) == TSUCHI_OK && !on);
    CHECK(c.press_input(a, pup, TSUCHI_INPUT_DOG_JUMP) == TSUCHI_OK); // and the input can be pressed again
    frame();
    CHECK(pressed(dog_pad, 20));
    frame();

    // A press is not read in the two frames after a pause ends.
    put<int32_t>(dog_pad.at() + t::game_controller::unpaused_frames, 1);
    CHECK(c.press_input(a, pup, TSUCHI_INPUT_DOG_THROW) == TSUCHI_OK);
    frame();
    CHECK(held(dog_pad, 18) && !pressed(dog_pad, 18));
    put<int32_t>(dog_pad.at() + t::game_controller::unpaused_frames, 10);
    frame();

    // A controller with no update latches at the step instead: the press is read on one step.
    CHECK(c.press_input(a, pup, TSUCHI_INPUT_WHISTLE) == TSUCHI_OK);
    unsigned presses = 0;
    for (int n = 0; n < 5; ++n) {
        step();
        presses += pressed(dog_pad, 3);
    }
    CHECK(presses == 1 && !held(dog_pad, 3));
    CHECK(c.input_held(a, pup, TSUCHI_INPUT_WHISTLE, &on) == TSUCHI_OK && !on);

    // Two characters on one controller object: refused, and a held input has no effect while they share it.
    put<uint64_t>(steered_dog.at() + t::character_part::controller, captain_pad.at());
    CHECK(c.hold_input(a, cap, TSUCHI_INPUT_WHISTLE) == TSUCHI_NOT_READY);
    CHECK(c.press_input(a, pup, TSUCHI_INPUT_WHISTLE) == TSUCHI_NOT_READY);
    frame();
    CHECK(!held(captain_pad, 8));
    put<uint64_t>(steered_dog.at() + t::character_part::controller, dog_pad.at());
    frame();
    CHECK(held(captain_pad, 8));
    // No controller object yet.
    put<uint64_t>(steered_dog.at() + t::character_part::controller, uint64_t(0));
    CHECK(c.hold_input(a, pup, TSUCHI_INPUT_WHISTLE) == TSUCHI_NOT_READY);
    put<uint64_t>(steered_dog.at() + t::character_part::controller, dog_pad.at());

    // A mod that stops lets go of its inputs.
    mod_stop(b);
    CHECK(c.input_held(a, cap, TSUCHI_INPUT_DISMISS, &on) == TSUCHI_OK && !on);
    frame();
    CHECK(!held(captain_pad, 8) && released(captain_pad, 8));
    frame();
    CHECK(!released(captain_pad, 8));
    b = mod_create("character.b2");

    // ---- Actions ----------------------------------------------------------------------------------------------
    CHECK(c.hold_action(a, cap, TSUCHI_ACTION_WHISTLE) == TSUCHI_NOT_READY); // no code access
    native_now = &native;
    CHECK(c.hold_action(a, cap, TSUCHI_ACTION_BITE) == TSUCHI_UNSUPPORTED);
    CHECK(c.hold_action(a, pup, TSUCHI_ACTION_DISMISS) == TSUCHI_UNSUPPORTED);
    CHECK(c.hold_action(a, pup, TSUCHI_ACTION_RIDE) == TSUCHI_UNSUPPORTED);
    CHECK(c.hold_action(a, stone, TSUCHI_ACTION_WHISTLE) == TSUCHI_NOT_FOUND);
    CHECK(placed.size() == 1); // the type switch's, from the first action hold
    CHECK(c.hold_action(a, cap, TSUCHI_ACTION_WHISTLE) == TSUCHI_OK);
    // Both whistle sites, with the game's own words there.
    CHECK(placed.size() == 3);
    for (unsigned site : {0u, 1u})
        CHECK(placed.at(site_offset(site)).expected == uint32_t(t::tables::character_site_words[site]));
    CHECK(site_offset(0) == 0x15AA0B0 && site_offset(1) == 0x15AA108);
    CHECK(c.hold_action(b, cap, TSUCHI_ACTION_WHISTLE) == TSUCHI_BUSY);
    CHECK(c.action_held(a, cap, TSUCHI_ACTION_WHISTLE, &on) == TSUCHI_OK && on);
    // Before the controller's update the action is not yet down.
    Frame seen = at_site(0, call_frame(0, steered_captain.at(), captain_pad.at(), 3));
    CHECK(target_of(0, seen) == reinterpret_cast<uint64_t>(&reader_stub));
    CHECK(!held(captain_pad, 3));
    // A held action reads as a held button: the whistle's two reads are down on every frame (the game ends a whistle
    // at the first frame its usual read is up), and the controller holds its input for the game's other reads.
    unsigned whistle_presses = 0, whistle_holds = 0, input_holds = 0, input_presses = 0;
    for (int n = 0; n < 40; ++n) {
        frame();
        seen = at_site(0, call_frame(0, steered_captain.at(), captain_pad.at(), 3));
        CHECK(target_of(0, seen) != reinterpret_cast<uint64_t>(&reader_stub)); // the pad doesn't start it meanwhile
        whistle_presses += call_answer(target_of(0, seen));
        seen = at_site(1, call_frame(1, steered_captain.at(), captain_pad.at(), 3));
        whistle_holds += call_answer(target_of(1, seen));
        input_holds += held(captain_pad, 3);
        input_presses += pressed(captain_pad, 3);
    }
    // The controller presses the action's input once too, so the press-age table the update fills sees the press.
    CHECK(whistle_presses == 40 && whistle_holds == 40 && input_holds == 40 && input_presses == 1);
    // The game's own repeat of a held input is not passed on as a press while only the mod holds it.
    captain_pad.pad_pressed = 1u << 3;
    CHECK(!pressed(captain_pad, 3));
    captain_pad.pad_held = 1u << 3; // the player presses it too
    CHECK(pressed(captain_pad, 3));
    captain_pad.pad_pressed = captain_pad.pad_held = 0;
    // The dog's whistle runs the same code with the dog's part: it is left alone.
    seen = at_site(0, call_frame(0, steered_dog.at(), dog_pad.at(), 3));
    CHECK(target_of(0, seen) == reinterpret_cast<uint64_t>(&reader_stub) && seen.x[1] == 3);
    CHECK(c.release_action(a, cap, TSUCHI_ACTION_WHISTLE) == TSUCHI_OK);
    CHECK(c.release_action(a, cap, TSUCHI_ACTION_WHISTLE) == TSUCHI_NOT_FOUND);
    // The frame after the release: the held read is up and the input with it.
    frame();
    seen = at_site(1, call_frame(1, steered_captain.at(), captain_pad.at(), 3));
    CHECK(!call_answer(target_of(1, seen)));
    seen = at_site(0, call_frame(0, steered_captain.at(), captain_pad.at(), 3));
    CHECK(!call_answer(target_of(0, seen)));
    CHECK(!held(captain_pad, 3));
    frame();
    seen = at_site(0, call_frame(0, steered_captain.at(), captain_pad.at(), 3));
    CHECK(target_of(0, seen) == reinterpret_cast<uint64_t>(&reader_stub));

    // A ride is pressed on the first frame, and released on the frame after the hold ends.
    CHECK(c.hold_action(a, cap, TSUCHI_ACTION_RIDE) == TSUCHI_OK);
    unsigned ride_presses = 0, ride_releases = 0;
    for (int n = 0; n < 10; ++n) {
        frame();
        seen = at_site(7, call_frame(7, steered_captain.at(), captain_pad.at(), 13));
        ride_presses += call_answer(target_of(7, seen));
        seen = at_site(8, call_frame(8, steered_captain.at(), captain_pad.at(), 13));
        ride_releases += call_answer(target_of(8, seen));
    }
    CHECK(ride_presses == 1 && ride_releases == 0);
    CHECK(held(captain_pad, 13) && !pressed(captain_pad, 13));
    CHECK(c.release_action(a, cap, TSUCHI_ACTION_RIDE) == TSUCHI_OK);
    frame();
    seen = at_site(8, call_frame(8, steered_captain.at(), captain_pad.at(), 13));
    CHECK(call_answer(target_of(8, seen)));
    seen = at_site(9, call_frame(9, steered_captain.at(), captain_pad.at(), 19));
    CHECK(!call_answer(target_of(9, seen))); // the release was read at the first of the two release reads
    frame();
    seen = at_site(8, call_frame(8, steered_captain.at(), captain_pad.at(), 13));
    CHECK(target_of(8, seen) == reinterpret_cast<uint64_t>(&reader_stub));

    // A held input is one press, then held, then one release, however long it is held.
    CHECK(c.hold_input(a, cap, TSUCHI_INPUT_DISMISS) == TSUCHI_OK);
    unsigned presses_seen = 0, holds_seen = 0, releases_seen = 0;
    for (int n = 0; n < 60; ++n) {
        frame();
        captain_pad.pad_pressed = n > 33 ? 1u << 8 : 0; // the game's repeat, from the held entry
        presses_seen += pressed(captain_pad, 8);
        holds_seen += held(captain_pad, 8);
        releases_seen += released(captain_pad, 8);
    }
    captain_pad.pad_pressed = 0;
    CHECK(presses_seen == 1 && holds_seen == 60 && releases_seen == 0);
    CHECK(c.release_input(a, cap, TSUCHI_INPUT_DISMISS) == TSUCHI_OK);
    for (int n = 0; n < 5; ++n) {
        frame();
        presses_seen += pressed(captain_pad, 8);
        holds_seen += held(captain_pad, 8);
        releases_seen += released(captain_pad, 8);
    }
    CHECK(presses_seen == 1 && holds_seen == 60 && releases_seen == 1);

    // A pressed action is down for one frame.
    CHECK(c.press_action(a, pup, TSUCHI_ACTION_JUMP) == TSUCHI_OK);
    CHECK(placed.count(site_offset(15)) && placed.count(site_offset(16)) && placed.count(site_offset(17)));
    CHECK(c.press_action(a, pup, TSUCHI_ACTION_JUMP) == TSUCHI_BUSY);
    frame();
    seen = at_site(17, call_frame(17, steered_dog.at(), dog_pad.at(), 20));
    CHECK(call_answer(target_of(17, seen)));
    frame();
    seen = at_site(17, call_frame(17, steered_dog.at(), dog_pad.at(), 20));
    CHECK(!call_answer(target_of(17, seen))); // the frame after: up
    frame();
    seen = at_site(17, call_frame(17, steered_dog.at(), dog_pad.at(), 20));
    CHECK(target_of(17, seen) == reinterpret_cast<uint64_t>(&reader_stub));
    CHECK(c.action_held(a, pup, TSUCHI_ACTION_JUMP, &on) == TSUCHI_OK && !on);

    // A site that has only the controller tells the captain from the dog by whose code it is, even when they share it.
    put<uint64_t>(steered_captain.at() + t::character_part::controller, dog_pad.at());
    CHECK(c.hold_action(a, pup, TSUCHI_ACTION_BITE) == TSUCHI_OK);
    CHECK(c.hold_action(a, cap, TSUCHI_ACTION_THROW_PIKMIN) == TSUCHI_OK);
    frame();
    seen = at_site(13, call_frame(13, 0, dog_pad.at(), 18));
    CHECK(call_answer(target_of(13, seen)));
    seen = at_site(3, call_frame(3, 0, dog_pad.at(), 0)); // the captain's held throw, on the shared controller
    CHECK(call_answer(target_of(3, seen)));
    seen = at_site(12, call_frame(12, 0, dog_pad.at(), 18)); // the dog's held throw is the dog's own
    CHECK(target_of(12, seen) == reinterpret_cast<uint64_t>(&reader_stub));
    CHECK(c.release_action(a, pup, TSUCHI_ACTION_BITE) == TSUCHI_OK);
    CHECK(c.release_action(a, cap, TSUCHI_ACTION_THROW_PIKMIN) == TSUCHI_OK);
    put<uint64_t>(steered_captain.at() + t::character_part::controller, captain_pad.at());
    frame();

    // The press waits for the site's first read: two updates, or the step's latch, can come before the game reads it.
    whistle_age = 100;
    CHECK(c.hold_action(a, cap, TSUCHI_ACTION_WHISTLE) == TSUCHI_OK);
    update_entry()(&captain_pad, 1 / 60.f);
    CHECK(whistle_age == 0); // the update's own press-age pass saw the press
    update_entry()(&captain_pad, 1 / 60.f);
    step();
    step();
    CHECK(whistle_age > 0 && !pressed(captain_pad, 3) && held(captain_pad, 3));
    // The whistle's reads are held reads: down at every read while the action is held, across updates.
    seen = at_site(0, call_frame(0, steered_captain.at(), captain_pad.at(), 3));
    CHECK(call_answer(target_of(0, seen)));
    update_entry()(&captain_pad, 1 / 60.f);
    seen = at_site(0, call_frame(0, steered_captain.at(), captain_pad.at(), 3));
    CHECK(call_answer(target_of(0, seen)));
    seen = at_site(1, call_frame(1, steered_captain.at(), captain_pad.at(), 3));
    CHECK(call_answer(target_of(1, seen)));
    CHECK(c.release_action(a, cap, TSUCHI_ACTION_WHISTLE) == TSUCHI_OK);
    frame();
    frame();
    frame();
    CHECK(!held(captain_pad, 3));
    seen = at_site(0, call_frame(0, steered_captain.at(), captain_pad.at(), 3));
    CHECK(target_of(0, seen) == reinterpret_cast<uint64_t>(&reader_stub));

    // A press is read at every site of the action until the next update, as a button's: the jump's two press
    // reads both see it in its frame, and neither after.
    CHECK(c.hold_action(a, pup, TSUCHI_ACTION_JUMP) == TSUCHI_OK);
    frame();
    seen = at_site(17, call_frame(17, steered_dog.at(), dog_pad.at(), 20));
    CHECK(call_answer(target_of(17, seen)));
    seen = at_site(15, call_frame(15, steered_dog.at(), dog_pad.at(), 20));
    CHECK(call_answer(target_of(15, seen)));
    frame();
    seen = at_site(15, call_frame(15, steered_dog.at(), dog_pad.at(), 20));
    CHECK(!call_answer(target_of(15, seen)));
    CHECK(c.release_action(a, pup, TSUCHI_ACTION_JUMP) == TSUCHI_OK);
    frame();
    frame();
    // A press nobody reads gives up after half a second of updates.
    CHECK(c.press_action(a, cap, TSUCHI_ACTION_DISMISS) == TSUCHI_OK);
    for (int n = 0; n < 40; ++n) frame();
    seen = at_site(4, call_frame(4, steered_captain.at(), captain_pad.at(), 8));
    CHECK(target_of(4, seen) == reinterpret_cast<uint64_t>(&reader_stub));
    // A pressed action is read once even when the game reads it a few frames late.
    CHECK(c.press_action(a, cap, TSUCHI_ACTION_DISMISS) == TSUCHI_OK);
    frame();
    frame();
    frame();
    seen = at_site(4, call_frame(4, steered_captain.at(), captain_pad.at(), 8));
    CHECK(call_answer(target_of(4, seen)));
    update_entry()(&captain_pad, 1 / 60.f);
    seen = at_site(4, call_frame(4, steered_captain.at(), captain_pad.at(), 8));
    CHECK(!call_answer(target_of(4, seen)));
    frame();
    frame();

    // ---- Bindings ---------------------------------------------------------------------------------------------
    CHECK(c.binding(a, cap, TSUCHI_ACTION_WHISTLE, &input) == TSUCHI_OK && input == TSUCHI_INPUT_WHISTLE);
    CHECK(c.binding(a, cap, TSUCHI_ACTION_THROW_PIKMIN, &input) == TSUCHI_OK && input == TSUCHI_INPUT_CAPTAIN_THROW);
    CHECK(c.binding(a, pup, TSUCHI_ACTION_THROW_PIKMIN, &input) == TSUCHI_OK && input == TSUCHI_INPUT_DOG_THROW);
    CHECK(c.binding(a, pup, TSUCHI_ACTION_DISMISS, &input) == TSUCHI_UNSUPPORTED);
    // A call site reads the bound input in place of its own.
    CHECK(c.hold_binding(a, cap, TSUCHI_ACTION_WHISTLE, TSUCHI_INPUT_DISMISS) == TSUCHI_OK);
    CHECK(c.hold_binding(b, cap, TSUCHI_ACTION_WHISTLE, TSUCHI_INPUT_DISMISS) == TSUCHI_BUSY);
    CHECK(c.binding(a, cap, TSUCHI_ACTION_WHISTLE, &input) == TSUCHI_OK && input == TSUCHI_INPUT_DISMISS);
    seen = at_site(0, call_frame(0, steered_captain.at(), captain_pad.at(), 3));
    CHECK(seen.x[1] == TSUCHI_INPUT_DISMISS && target_of(0, seen) == reinterpret_cast<uint64_t>(&reader_stub));
    // Bound to no input, the read answers up.
    CHECK(c.hold_binding(a, cap, TSUCHI_ACTION_WHISTLE, TSUCHI_INPUT_NONE) == TSUCHI_OK);
    seen = at_site(1, call_frame(1, steered_captain.at(), captain_pad.at(), 3));
    CHECK(!call_answer(target_of(1, seen)) && target_of(1, seen) != reinterpret_cast<uint64_t>(&reader_stub));
    // A held action wins over its binding.
    CHECK(c.hold_action(a, cap, TSUCHI_ACTION_WHISTLE) == TSUCHI_OK);
    frame();
    seen = at_site(1, call_frame(1, steered_captain.at(), captain_pad.at(), 3));
    CHECK(call_answer(target_of(1, seen)));
    seen = at_site(0, call_frame(0, steered_captain.at(), captain_pad.at(), 3));
    CHECK(call_answer(target_of(0, seen))); // its press, even though the action is bound to none
    CHECK(c.release_action(a, cap, TSUCHI_ACTION_WHISTLE) == TSUCHI_OK);
    CHECK(c.release_binding(a, cap, TSUCHI_ACTION_WHISTLE) == TSUCHI_OK);
    CHECK(c.release_binding(a, cap, TSUCHI_ACTION_WHISTLE) == TSUCHI_NOT_FOUND);
    frame();
    frame();
    seen = at_site(0, call_frame(0, steered_captain.at(), captain_pad.at(), 3));
    CHECK(seen.x[1] == 3 && target_of(0, seen) == reinterpret_cast<uint64_t>(&reader_stub));

    // The type switch after the game's own helper: the answer in w0 is the bound input's press.
    CHECK(c.hold_binding(a, cap, TSUCHI_ACTION_SWITCH_TYPE, TSUCHI_INPUT_DISMISS) == TSUCHI_OK);
    Frame result;
    result.x[19] = steered_captain.at();
    result.x[0] = 1; // the helper said the player switched
    seen = at_site(5, result);
    CHECK((seen.x[0] & 1) == 0);
    captain_pad.pad_pressed = 1u << 8;
    seen = at_site(5, result);
    CHECK((seen.x[0] & 1) == 1);
    captain_pad.pad_pressed = 0;
    CHECK(c.hold_binding(a, cap, TSUCHI_ACTION_SWITCH_TYPE, TSUCHI_INPUT_NONE) == TSUCHI_OK);
    seen = at_site(5, result);
    CHECK((seen.x[0] & 1) == 0);
    CHECK(c.release_binding(a, cap, TSUCHI_ACTION_SWITCH_TYPE) == TSUCHI_OK);
    CHECK(c.press_action(a, cap, TSUCHI_ACTION_SWITCH_TYPE) == TSUCHI_OK);
    frame();
    result.x[0] = 0;
    seen = at_site(5, result);
    CHECK((seen.x[0] & 1) == 1);
    frame();

    // The press-age read of the bite moves to the bound input's entry, or to an age no window takes.
    std::vector<uint8_t> ages(33 * 16);
    for (unsigned id = 0; id < 33; ++id) put<float>(reinterpret_cast<uint64_t>(ages.data()) + id * 16 + 4, float(id));
    put<int32_t>(dog_pad.at() + t::game_controller::press_age_count, 33);
    CHECK(c.hold_binding(a, pup, TSUCHI_ACTION_BITE, TSUCHI_INPUT_DOG_JUMP) == TSUCHI_OK);
    const uint32_t age_word = uint32_t(t::tables::character_site_words[14]);
    const unsigned base = age_word >> 5 & 31;
    const uint64_t offset = uint64_t(age_word >> 10 & 0xFFF) * 4;
    CHECK(offset == 18 * 16 + 4);
    Frame age;
    age.x[uint32_t(t::tables::character_site_registers[14]) - 32] = dog_pad.at();
    age.x[base] = reinterpret_cast<uint64_t>(ages.data());
    seen = at_site(14, age);
    CHECK(get<float>(seen.x[base] + offset) == 20.0f);
    seen = at_site(13, call_frame(13, 0, dog_pad.at(), 18));
    CHECK(seen.x[1] == 20);
    CHECK(c.hold_binding(a, pup, TSUCHI_ACTION_BITE, TSUCHI_INPUT_NONE) == TSUCHI_OK);
    seen = at_site(14, age);
    CHECK(get<float>(seen.x[base] + offset) > 1e20f);
    CHECK(c.release_binding(a, pup, TSUCHI_ACTION_BITE) == TSUCHI_OK);
    seen = at_site(14, age);
    CHECK(seen.x[base] == reinterpret_cast<uint64_t>(ages.data()));

    // A refused hook leaves nothing held.
    refuse_inline = TSUCHI_BUSY;
    CHECK(c.hold_action(a, pup, TSUCHI_ACTION_INTERACT) == TSUCHI_NOT_READY);
    CHECK(c.action_held(a, pup, TSUCHI_ACTION_INTERACT, &on) == TSUCHI_OK && !on);
    refuse_inline = TSUCHI_OK;

    // ---- Movement ---------------------------------------------------------------------------------------------
    const TSUCHI_Vec3 too_far{1.0f, 1.0f, 0}, way{0.6f, -0.8f, 5};
    CHECK(c.hold_move(a, cap, &too_far) == TSUCHI_INVALID);
    CHECK(c.hold_move(a, cap, &way) == TSUCHI_OK);
    CHECK(c.hold_move(b, cap, &way) == TSUCHI_BUSY);
    CHECK(placed.count(site_offset(21)) && placed.count(site_offset(22)));
    const float player[3] = {0.1f, 0.2f, 0};
    put(steered_captain.at() + t::character_part::move, player);
    Frame moved;
    moved.x[19] = steered_captain.at();
    at_site(21, moved);
    TSUCHI_Vec3 now{};
    CHECK(c.move(a, cap, &now) == TSUCHI_OK && now.x == 0.6f && now.y == -0.8f && now.z == 0);
    put(steered_captain.at() + t::character_part::move, player);
    at_site(22, moved);
    CHECK(c.move(a, cap, &now) == TSUCHI_OK && now.x == 0.6f);
    // Another character's part is left as the game wrote it.
    put(steered_dog.at() + t::character_part::move, player);
    moved.x[19] = steered_dog.at();
    at_site(21, moved);
    CHECK(c.move(a, pup, &now) == TSUCHI_OK && now.x == 0.1f && now.y == 0.2f);
    CHECK(c.release_move(a, cap) == TSUCHI_OK);
    CHECK(c.release_move(a, cap) == TSUCHI_NOT_FOUND);
    put(steered_captain.at() + t::character_part::move, player);
    moved.x[19] = steered_captain.at();
    at_site(21, moved);
    CHECK(c.move(a, cap, &now) == TSUCHI_OK && now.x == 0.1f);

    // ---- Everything ends with the mod, and with the character ------------------------------------------------
    CHECK(c.hold_input(a, cap, TSUCHI_INPUT_WHISTLE) == TSUCHI_OK);
    CHECK(c.hold_action(a, pup, TSUCHI_ACTION_RUSH) == TSUCHI_OK);
    CHECK(c.hold_action(a, cap, TSUCHI_ACTION_RIDE) == TSUCHI_OK);
    CHECK(c.hold_binding(a, cap, TSUCHI_ACTION_DISMISS, TSUCHI_INPUT_NONE) == TSUCHI_OK);
    CHECK(c.hold_move(a, cap, &way) == TSUCHI_OK);
    frame();
    CHECK(held(captain_pad, 3));
    seen = at_site(19, call_frame(19, 0, dog_pad.at(), 20));
    CHECK(call_answer(target_of(19, seen)));
    mod_stop(a);
    frame();
    CHECK(!held(captain_pad, 3) && released(captain_pad, 3));
    frame();
    seen = at_site(19, call_frame(19, 0, dog_pad.at(), 20));
    CHECK(target_of(19, seen) == reinterpret_cast<uint64_t>(&reader_stub));
    // A ride whose mod stopped is not released into the game's release window.
    seen = at_site(8, call_frame(8, steered_captain.at(), captain_pad.at(), 13));
    CHECK(target_of(8, seen) == reinterpret_cast<uint64_t>(&reader_stub));
    seen = at_site(4, call_frame(4, steered_captain.at(), captain_pad.at(), 8));
    CHECK(target_of(4, seen) == reinterpret_cast<uint64_t>(&reader_stub));
    put(steered_captain.at() + t::character_part::move, player);
    at_site(21, moved);
    CHECK(c.move(b, cap, &now) == TSUCHI_OK && now.x == 0.1f);
    frame();

    CHECK(c.hold_input(b, pup, TSUCHI_INPUT_DOG_THROW) == TSUCHI_OK);
    frame();
    CHECK(held(dog_pad, 18));
    CHECK(actor_left);
    actor_left(dog.at(), dog.at(), ActorLeft{});
    CHECK(c.input_held(b, pup, TSUCHI_INPUT_DOG_THROW, &on) == TSUCHI_OK && !on);
    frame();
    CHECK(!held(dog_pad, 18));
    // The sites stay on, and every site is placed once at most.
    CHECK(placed.size() <= sizeof t::tables::character_sites / sizeof t::tables::character_sites[0]);
    CHECK(wrapped.size() == 4);

    std::printf("character: %u checks passed\n", checks);
    return 0;
}
