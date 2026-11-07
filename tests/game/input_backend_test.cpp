// The input service against a made-up game, with the real generated validation, registry, and holds: the import
// slots it wraps, the presenter's reads it is given, the system's keyboard, and the holds mods take.
#include "actor_events.h"
#include "frame.h"
#include "game.h"
#include "hooks.h"
#include "input_service.h"
#include "modules.h"
#include "objects.h"
#include "ordinary_lifecycle.h"
#include "people.h"
#include "presenter.h"
#include "registry.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
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
static std::vector<void (*)()> finishes;
static std::vector<std::pair<uint64_t, uint64_t>> ranges;
static std::set<uint64_t> live_objects;
static uint64_t thread = 1;
// The game's globals this test reads: the three import slots and the anchor in the system's library.
constexpr uint64_t smallest(uint64_t a, uint64_t b) { return a < b ? a : b; }
constexpr uint64_t largest(uint64_t a, uint64_t b) { return a > b ? a : b; }
const uint64_t globals_from = smallest(smallest(t::data::input_full_key.address, t::data::input_handheld.address),
                                           smallest(t::data::input_joy_dual.address, t::data::system_library_anchor));
const uint64_t globals_to = largest(largest(t::data::input_full_key.address, t::data::input_handheld.address),
                                        largest(t::data::input_joy_dual.address, t::data::system_library_anchor)) + 8;
static uint8_t* globals = nullptr;
static uint64_t& slot(uint64_t address) { return *reinterpret_cast<uint64_t*>(globals + (address - globals_from)); }
static uint32_t routed_pad = 0, raw_buttons = 0, pending = 0, foreign_calls = 0;
static int32_t routes[4] = {-1, -1, -1, -1};
static bool sampling = false, world_ready = true;
static uint64_t (*after_input)(uint32_t, uint64_t, bool);
using Poll = void (*)(void*, const void*);
static std::map<uint64_t, Poll> polls;
template <class T> void put(uint64_t at, const T& value) { std::memcpy(reinterpret_cast<void*>(at), &value, sizeof value); }
template <class T> T get(uint64_t at) {
    T v;
    std::memcpy(&v, reinterpret_cast<void*>(at), sizeof v);
    return v;
}
static void tick() {
    for (auto step : steps) step();
    for (auto finish : finishes) finish();
}
static uint64_t probe() { return thread; }
static void foreign(void*, const void*);

// The system's keyboard: a library that offers it, or not, and what it says.
static uint8_t library_code[0x100];
static bool keyboard_offered = false;
static unsigned keyboard_starts = 0, keyboard_reads = 0;
static uint32_t keyboard_words[8] = {};
static void initialize_keyboard() { ++keyboard_starts; }
static void get_keyboard_state(void* state) {
    CHECK(keyboard_starts == 1);
    ++keyboard_reads;
    const uint64_t at = reinterpret_cast<uint64_t>(state);
    put<int64_t>(at + t::keyboard_state::sampling, int64_t(keyboard_reads));
    put<uint32_t>(at + t::keyboard_state::modifiers, 0x5);
    put<uint32_t>(at + t::keyboard_state::attributes, 0x1);
    std::memcpy(reinterpret_cast<void*>(at + t::keyboard_state::keys), keyboard_words, sizeof keyboard_words);
}
static void key_down(uint32_t usage, bool down) {
    if (down) keyboard_words[usage / 32] |= 1u << (usage % 32);
    else keyboard_words[usage / 32] &= ~(1u << (usage % 32));
}

namespace tsuchi::game {
uint64_t main_base() { return reinterpret_cast<uint64_t>(globals) - globals_from; }
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
bool is_game_actor(uint64_t) { return false; }
uint64_t part_owner(uint64_t) { return 0; }
bool player_pad(uint32_t p, uint32_t& out) {
    if (p >= kPlayerSlots || !world_ready) return false;
    out = routes[p] < 0 ? routed_pad : uint32_t(routes[p]);
    return true;
}
uint64_t module_containing(uint64_t address) {
    const uint64_t code = reinterpret_cast<uint64_t>(library_code);
    return address >= code && address < code + sizeof library_code ? code : 0;
}
uint64_t module_symbol(uint64_t module, const char* name) {
    if (module != reinterpret_cast<uint64_t>(library_code) || !keyboard_offered) return 0;
    if (!std::strcmp(name, "_ZN2nn3hid18InitializeKeyboardEv")) return reinterpret_cast<uint64_t>(&initialize_keyboard);
    if (!std::strcmp(name, "_ZN2nn3hid16GetKeyboardStateEPNS0_13KeyboardStateE")) return reinterpret_cast<uint64_t>(&get_keyboard_state);
    return 0;
}
} // namespace tsuchi::game
namespace tsuchi {
static bool in_step = false;
bool frame_stepping() { return in_step; }
bool after_step(void (*f)()) { return finishes.push_back(f), true; }
bool each_frame(void (*f)()) { return steps.push_back(f), true; }
bool on_actor_left(void (*)(uint64_t, uint64_t, ActorLeft)) { return true; }
void ordinary_lifecycle_listener(void (*)(uint64_t, uint64_t, bool)) {}
TSUCHI_Result ordinary_lifecycle_watch(uint64_t) { return TSUCHI_UNSUPPORTED; }
uint64_t ordinary_lifecycle_restore_part(uint64_t h, uint64_t) { return game::from_handle(h); }
void presenter_input_after(uint64_t (*f)(uint32_t, uint64_t, bool)) { after_input = f; }
bool presenter_sampling_input() { return sampling; }
bool presenter_sample_input(uint32_t pad, void* state) {
    sampling = true;
    if (polls.count(t::data::input_full_key.address)) polls.at(t::data::input_full_key.address)(state, &pad);
    else {
        // The system binds the slot as the first read goes through it, before the read returns.
        slot(t::data::input_full_key.address) = reinterpret_cast<uint64_t>(&foreign);
        foreign(state, &pad);
    }
    sampling = false;
    const uint64_t at = reinterpret_cast<uint64_t>(state) + t::npad_state::buttons;
    put(at, get<uint64_t>(at) | pending);
    return true;
}
TSUCHI_Result hook_wrap(TSUCHI_Mod*, uint64_t entry, uint64_t, void* f, uint32_t flags, uint64_t* id, void** next) {
    CHECK(flags == 0);
    const uint64_t offset = entry - game::main_base();
    *next = reinterpret_cast<void*>(slot(offset));
    polls[offset] = reinterpret_cast<Poll>(f);
    *id = entry;
    return TSUCHI_OK;
}
void input_start(TSUCHI_Mod*);
} // namespace tsuchi
static void foreign(void* out, const void*) {
    ++foreign_calls;
    std::memset(out, 0, t::npad_state::size);
    const uint64_t a = reinterpret_cast<uint64_t>(out);
    put(a + t::npad_state::buttons, uint64_t(raw_buttons));
    put(a + t::input_state::connected, uint32_t(1));
    const int32_t axes[] = {-32767, 123, 456, 32767};
    std::memcpy(reinterpret_cast<void*>(a + t::input_state::sticks), axes, sizeof axes);
}

int main() {
    std::vector<uint8_t> memory(globals_to - globals_from);
    globals = memory.data();
    ranges.push_back({reinterpret_cast<uint64_t>(globals), memory.size()});
    for (uint64_t address : {t::data::input_full_key.address, t::data::input_handheld.address, t::data::input_joy_dual.address})
        slot(address) = reinterpret_cast<uint64_t>(&foreign);
    set_allocator({std::malloc, std::free});
    set_thread_probe(probe);
    note_game_thread();
    TSUCHI_Mod *a = mod_create("input.a"), *b = mod_create("input.b"), *self = mod_create("tsuchi.self");
    // An import slot that leads to the resolver is not bound yet, and is left alone.
    slot(t::data::input_full_key.address) = game::main_base() + t::constant::input_resolver;
    input_start(self);
    CHECK(polls.count(t::data::input_full_key.address) == 0 && polls.count(t::data::input_handheld.address) == 1 &&
          polls.count(t::data::input_joy_dual.address) == 1);
    const auto& i = input::table;
    TSUCHI_Buttons buttons = 0;
    TSUCHI_PadState pad{};
    bool key = true;
    CHECK(i.pad(a, 0, &pad) == TSUCHI_NOT_READY);
    CHECK(i.key(a, 4, &key) == TSUCHI_NOT_READY && key); // before the first step
    tick();
    CHECK(polls.count(t::data::input_full_key.address) == 1); // bound by the step's first read, then wrapped
    for (int n = 0; n < 9; ++n) tick();

    // The keyboard: nothing to be found, then a library that offers it, started once and read once a step.
    CHECK(i.key(a, 4, &key) == TSUCHI_NOT_READY && key);
    slot(t::data::system_library_anchor) = reinterpret_cast<uint64_t>(library_code) + 0x40;
    CHECK(i.key(a, 4, &key) == TSUCHI_NOT_READY && key && keyboard_starts == 0);
    keyboard_offered = true;
    key_down(4, true), key_down(44, true), key_down(255, true);
    CHECK(i.key(a, 4, &key) == TSUCHI_OK && key && keyboard_starts == 1 && keyboard_reads == 1);
    CHECK(i.key(a, 5, &key) == TSUCHI_OK && !key);
    CHECK(i.key(a, 44, &key) == TSUCHI_OK && key);
    CHECK(i.key(a, 255, &key) == TSUCHI_OK && key);
    CHECK(i.key(a, 0, &key) == TSUCHI_OK && !key && keyboard_reads == 1);
    key_down(4, false), key_down(43, true);
    CHECK(i.key(a, 4, &key) == TSUCHI_OK && key && keyboard_reads == 1); // the step's sample
    tick();
    CHECK(i.key(b, 4, &key) == TSUCHI_OK && !key && keyboard_reads == 2);
    CHECK(i.key(b, 43, &key) == TSUCHI_OK && key && i.key(b, 44, &key) == TSUCHI_OK && key && keyboard_reads == 2);
    tick();
    tick();
    CHECK(keyboard_reads == 2); // a step nobody asks about reads nothing
    CHECK(i.key(a, 43, &key) == TSUCHI_OK && key && keyboard_reads == 3 && keyboard_starts == 1);
    key = false;
    CHECK(i.key(a, 256, &key) == TSUCHI_INVALID && !key && i.key(a, 0xFFFFFFFF, &key) == TSUCHI_INVALID);

    // All three import chains call the foreign wrapper and agree about injection and hiding.
    CHECK(i.pad(a, 0, &pad) == TSUCHI_OK && pad.connected);
    CHECK(i.hold_pressed(a, 0, 1, 2) == TSUCHI_OK);
    CHECK(i.hold_hidden(b, 0, 1) == TSUCHI_BUSY);
    CHECK(i.hold_hidden(a, 0, 2) == TSUCHI_OK);
    CHECK(i.pad(a, 0, &pad) == TSUCHI_OK && pad.held == 1 && pad.pressed == 1);
    CHECK(i.pressed(a, 0, &buttons) == TSUCHI_OK && buttons == 1);
    CHECK(i.hidden(a, 0, &buttons) == TSUCHI_OK && buttons == 2);
    alignas(8) uint8_t state[t::npad_state::most] = {};
    raw_buttons = 2;
    CHECK(polls.size() == 3);
    for (const auto& poll : polls) {
        const unsigned before = foreign_calls;
        poll.second(state, &routed_pad);
        CHECK(foreign_calls == before + 1);
        CHECK(get<uint64_t>(reinterpret_cast<uint64_t>(state) + t::npad_state::buttons) == 1);
    }
    sampling = true;
    polls.at(t::data::input_full_key.address)(state, &routed_pad);
    sampling = false;
    // Hidden physical input survives, and the mod's own press stays out of the presenter's latch.
    CHECK(get<uint64_t>(reinterpret_cast<uint64_t>(state) + t::npad_state::buttons) == 2);
    pending = 2;
    raw_buttons = 0;
    tick();
    CHECK(i.pad(a, 0, &pad) == TSUCHI_OK && pad.held == 3 && pad.pressed == 3 && pad.left_x == -32767 && pad.left_y == 123 &&
          pad.right_x == 456 && pad.right_y == 32767);
    CHECK(after_input(0, 2, true) == 1);
    pending = 0;
    tick();
    CHECK(i.pressed(a, 0, &buttons) == TSUCHI_OK && buttons == 1);
    CHECK(after_input(0, 0, true) == 1);
    tick();
    CHECK(i.pressed(a, 0, &buttons) == TSUCHI_OK && buttons == 0);
    CHECK(i.pad(a, 0, &pad) == TSUCHI_OK && pad.released == 1);
    CHECK(i.hold_pressed(a, 0, 4, 1) == TSUCHI_OK);
    tick();
    tick();
    CHECK(i.pressed(a, 0, &buttons) == TSUCHI_OK && buttons == 0); // it ends even when the game reads no import slot
    tsuchi::in_step = true;
    CHECK(i.hold_pressed(a, 0, 4, 1) == TSUCHI_OK);
    for (auto finish : finishes) finish();
    tsuchi::in_step = false;
    tick();
    CHECK(i.pressed(a, 0, &buttons) == TSUCHI_OK && buttons == 0); // a press taken in a step ends before the next
    CHECK(i.release_hidden(a, 0) == TSUCHI_OK);
    CHECK(after_input(0, 2, true) == 2);
    CHECK(i.release_pressed(a, 0) == TSUCHI_NOT_FOUND);
    CHECK(i.hold_pressed(a, 0, 8, 1) == TSUCHI_OK);
    CHECK(i.pad(a, 0, &pad) == TSUCHI_OK && pad.held == 8);
    CHECK(presenter_sample_input(0, state));
    CHECK(get<uint64_t>(reinterpret_cast<uint64_t>(state) + t::npad_state::buttons) == 0);
    CHECK(i.release_pressed(a, 0) == TSUCHI_OK);
    CHECK(i.pad(a, 0, &pad) == TSUCHI_OK && pad.held == 0);
    CHECK(after_input(0, 0, true) == 0); // no press is left after a release, before the world reads

    // Bits of a controller the API does not name stay out of the state a mod reads, and reach the game as they are.
    raw_buttons = 0x10000 | 1;
    tick();
    CHECK(i.pad(a, 0, &pad) == TSUCHI_OK && pad.held == 1 && pad.pressed == 1);
    CHECK(i.hold_hidden(a, 0, 1) == TSUCHI_OK && after_input(0, 0x10001, true) == 0x10000);
    CHECK(i.release_hidden(a, 0) == TSUCHI_OK);
    raw_buttons = 0;
    tick();
    tick();

    // A controller's holds are one for every player routed to it, across both kinds of hold.
    const unsigned empty = owned_count(a);
    routes[0] = routes[1] = 0;
    routes[2] = routes[3] = 2;
    CHECK(i.hold_pressed(a, 0, 1, 2) == TSUCHI_OK);
    CHECK(owned_count(a) == empty + 1);
    CHECK(i.hold_pressed(b, 1, 2, 2) == TSUCHI_BUSY);
    CHECK(i.hold_hidden(b, 1, 2) == TSUCHI_BUSY);
    CHECK(i.hold_pressed(a, 1, 4, 2) == TSUCHI_OK);
    CHECK(owned_count(a) == empty + 1); // taken again through the other player, it is the same hold
    CHECK(i.pressed(a, 0, &buttons) == TSUCHI_OK && buttons == 4);
    CHECK(i.pressed(a, 1, &buttons) == TSUCHI_OK && buttons == 4);
    CHECK(after_input(0, 0, true) == 4);
    routes[1] = 1;
    CHECK(i.release_pressed(a, 1) == TSUCHI_OK); // a player releases what it took even after it moved
    CHECK(owned_count(a) == empty);
    CHECK(i.release_pressed(a, 0) == TSUCHI_NOT_FOUND);
    CHECK(after_input(0, 0, true) == 0);
    routes[1] = 0;
    CHECK(i.hold_hidden(a, 0, 1) == TSUCHI_OK);
    CHECK(i.hold_hidden(a, 1, 2) == TSUCHI_OK && owned_count(a) == empty + 1);
    CHECK(i.hold_pressed(b, 1, 4, 1) == TSUCHI_BUSY);
    routes[1] = 1;
    CHECK(i.hold_hidden(a, 1, 8) == TSUCHI_OK && owned_count(a) == empty + 2);
    CHECK(i.hidden(a, 0, &buttons) == TSUCHI_OK && buttons == 2);
    CHECK(i.release_hidden(a, 1) == TSUCHI_OK && owned_count(a) == empty + 1);
    CHECK(after_input(0, 2, true) == 0); // player zero keeps its hold
    CHECK(i.release_hidden(a, 0) == TSUCHI_OK && owned_count(a) == empty);
    CHECK(i.hold_pressed(a, 0, 1, 2) == TSUCHI_OK);
    routes[0] = 1;
    CHECK(i.hold_pressed(a, 0, 8, 2) == TSUCHI_OK && owned_count(a) == empty + 1);
    CHECK(after_input(0, 0, true) == 0 && after_input(1, 0, true) == 8);
    CHECK(i.release_pressed(a, 0) == TSUCHI_OK);
    routes[0] = routes[1] = 0;
    raw_buttons = 2;
    CHECK(i.hold_hidden(a, 0, 2) == TSUCHI_OK);
    tick();
    CHECK(i.pad(a, 0, &pad) == TSUCHI_OK && pad.held == 2 && pad.pressed == 2);
    TSUCHI_PadState alias{};
    CHECK(i.pad(a, 1, &alias) == TSUCHI_OK && alias.held == pad.held && alias.pressed == pad.pressed);
    CHECK(after_input(0, 2, true) == 0);
    tick(); // a hidden button the player holds is still a step's history when the game reads no controller
    CHECK(i.pad(a, 0, &pad) == TSUCHI_OK && pad.held == 2 && pad.pressed == 0 && pad.released == 0);
    CHECK(i.pad(a, 0, &pad) == TSUCHI_OK && pad.pressed == 0); // reading again changes no history
    routes[0] = routes[1] = routes[2] = routes[3] = 1;
    tick(); // controller zero is read while no player uses it
    routes[0] = routes[1] = 0;
    CHECK(i.pad(a, 0, &pad) == TSUCHI_OK && pad.held == 2 && pad.pressed == 0);
    tick();
    CHECK(i.pad(a, 0, &pad) == TSUCHI_OK && pad.held == 2 && pad.pressed == 0);
    CHECK(i.release_hidden(a, 0) == TSUCHI_OK);
    raw_buttons = 0;
    tick();
    CHECK(i.pad(a, 0, &pad) == TSUCHI_OK && pad.released == 2);
    CHECK(i.pad(a, 1, &alias) == TSUCHI_OK && alias.released == 2);
    tick();
    CHECK(i.pad(a, 0, &pad) == TSUCHI_OK && pad.released == 0);
    // A press a mod makes in its step handler is that step's history even if a menu reads no controller.
    tsuchi::in_step = true;
    CHECK(i.hold_pressed(a, 0, 4, 1) == TSUCHI_OK);
    CHECK(i.pad(a, 0, &pad) == TSUCHI_OK && pad.pressed == 4);
    for (auto finish : finishes) finish();
    tsuchi::in_step = false;
    tick();
    CHECK(i.pad(a, 0, &pad) == TSUCHI_OK && pad.held == 0 && pad.released == 4);
    // A release and a stop take the presses away and keep a tap the player made between two steps.
    pending = 2;
    CHECK(i.hold_pressed(a, 0, 8, 1) == TSUCHI_OK);
    CHECK(presenter_sample_input(0, state));
    CHECK(get<uint64_t>(reinterpret_cast<uint64_t>(state) + t::npad_state::buttons) == 2);
    CHECK(i.release_pressed(a, 0) == TSUCHI_OK);
    tick();
    CHECK(i.pad(a, 0, &pad) == TSUCHI_OK && pad.held == 2 && pad.pressed == 2);
    pending = 0;
    tick();
    CHECK(i.pad(a, 0, &pad) == TSUCHI_OK && pad.held == 0 && pad.released == 2);
    pending = 1;
    CHECK(i.hold_pressed(a, 0, 8, 1) == TSUCHI_OK);
    mod_stop(a);
    CHECK(after_input(0, 1, true) == 1);
    tick();
    CHECK(i.pad(b, 0, &pad) == TSUCHI_OK && pad.held == 1 && pad.pressed == 1);
    pending = 0;
    tick();
    CHECK(i.pad(b, 0, &pad) == TSUCHI_OK && pad.held == 0 && pad.released == 1);
    a = mod_create("input.routes");
    for (auto& route : routes) route = -1;

    // A hold stays with the controller it was taken on when the player moves to another.
    CHECK(i.hold_pressed(a, 0, 4, 1) == TSUCHI_OK);
    CHECK(i.hold_hidden(a, 0, 8) == TSUCHI_OK);
    routed_pad = 1;
    CHECK(i.release_pressed(a, 0) == TSUCHI_OK);
    CHECK(i.release_hidden(a, 0) == TSUCHI_OK);
    CHECK(after_input(0, 8, true) == 8);
    routed_pad = 0;
    CHECK(i.hold_pressed(a, 0, 4, 1) == TSUCHI_OK);
    CHECK(i.hold_hidden(a, 0, 8) == TSUCHI_OK);
    routed_pad = 1;
    mod_stop(a);
    CHECK(after_input(0, 8, true) == 8);
    CHECK(owned_count(a) == 0);
    routed_pad = 0;

    // Calls from another thread are refused, and leave the holds as they are.
    a = mod_create("input.thread");
    CHECK(i.hold_pressed(a, 0, 1, 1) == TSUCHI_OK);
    thread = 2;
    CHECK(i.release_pressed(a, 0) == TSUCHI_INVALID);
    CHECK(i.pad(a, 0, &pad) == TSUCHI_INVALID);
    CHECK(i.key(a, 4, &key) == TSUCHI_INVALID);
    thread = 1;
    CHECK(after_input(0, 0, true) == 1);
    mod_stop(a);
    CHECK(after_input(0, 0, true) == 0);
    CHECK(i.key(b, 256, &key) == TSUCHI_INVALID);
    CHECK(i.hold_pressed(b, 0, 0, 1) == TSUCHI_INVALID);
    CHECK(i.hold_pressed(b, 0, 1, 0) == TSUCHI_INVALID);
    CHECK(i.hold_pressed(b, 0, 1, 601) == TSUCHI_INVALID);
    CHECK(i.hold_pressed(b, 0, 0x10000, 1) == TSUCHI_INVALID);
    CHECK(i.hold_hidden(b, 0, 0) == TSUCHI_INVALID);
    CHECK(i.pad(b, 4, &pad) == TSUCHI_INVALID);
    CHECK(i.hold_pressed(b, 4, 1, 1) == TSUCHI_INVALID);
    CHECK(i.hold_hidden(b, 4, 1) == TSUCHI_INVALID);
    CHECK(i.release_hidden(b, 4) == TSUCHI_INVALID);
    CHECK(i.release_pressed(b, 4) == TSUCHI_INVALID);
    CHECK(i.hidden(b, 4, &buttons) == TSUCHI_INVALID);
    CHECK(i.pressed(b, 4, &buttons) == TSUCHI_INVALID);
    CHECK(i.release_pressed(b, 0) == TSUCHI_NOT_FOUND && i.release_hidden(b, 0) == TSUCHI_NOT_FOUND);
    world_ready = false;
    CHECK(i.pad(b, 0, &pad) == TSUCHI_NOT_READY);
    CHECK(i.hold_pressed(b, 0, 1, 1) == TSUCHI_NOT_READY);
    CHECK(i.hold_hidden(b, 0, 1) == TSUCHI_NOT_READY);
    CHECK(i.pressed(b, 0, &buttons) == TSUCHI_NOT_READY);
    CHECK(i.hidden(b, 0, &buttons) == TSUCHI_NOT_READY);
    std::printf("input: %u checks passed\n", checks);
}
