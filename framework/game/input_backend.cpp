// The input service: a player's controller read once per world step, buttons pressed for a player, buttons hidden
// from the game, and the keyboard. The game reads a controller through its import slots of the system's readers.
// The service wraps those slots, and so keeps any hook that was there before. Taps between pictures are the
// presenter's (presenter.h): its latch, and its own read of a controller at each step.
//
// The game reads a controller only when the system says which style it is connected as. A controller a mod
// holds whole (hold_state) has one for as long as it is held, whether or not the system has a controller
// there: the slot of the system's style reader is wrapped too.
#include "frame.h"
#include "game.h"
#include "holds.h"
#include "hooks.h"
#include "input_service.h"
#include "modules.h"
#include "people.h"
#include "presenter.h"

#include <string.h>

namespace tsuchi {
namespace {
namespace t = game::table;

using Poll = void (*)(void*, const void*);
Poll next_poll[3] = {};
bool installed[3] = {};
using Style = uint32_t (*)(const void*);
Style next_style = nullptr;
bool style_installed = false;
TSUCHI_Mod* self = nullptr;
uint64_t step_number = 0;
constexpr uint32_t kButtons = 0xFFFF; // the bits of a controller's buttons the API names
constexpr uint32_t kSlots = 9;        // controllers 0 to 7, and the console held in the hands

// What a hold keeps: its buttons, the players it was taken through, and for a press the step it ends at.
struct Press {
    uint32_t buttons, players;
    uint64_t until;
};
struct Hidden {
    uint32_t buttons, players;
};
struct State {
    uint32_t buttons, players;
    uint64_t until;
    int32_t axes[4];
};
TSUCHI_Result apply_state(const Hold&, State& kept, const State& wanted, bool) { return kept = wanted, TSUCHI_OK; }
bool keep_state(const Hold&, State& kept) { return step_number < kept.until; }
void restore_state(const Hold&, State& kept, HoldEnd) { kept = {}; }
TSUCHI_Result apply_press(const Hold&, Press& kept, const Press& wanted, bool) { return kept = wanted, TSUCHI_OK; }
bool keep_press(const Hold&, Press& kept) { return step_number < kept.until; }
void restore_press(const Hold&, Press& kept, HoldEnd) { kept = {}; }
TSUCHI_Result apply_hidden(const Hold&, Hidden& kept, const Hidden& wanted, bool) { return kept = wanted, TSUCHI_OK; }
bool keep_hidden(const Hold&, Hidden&) { return true; }
void restore_hidden(const Hold&, Hidden& kept, HoldEnd) { kept = {}; }
// Both by controller. The two kinds share one owner for each controller.
Holds<Press, Press, apply_press, keep_press, restore_press> presses{"input.pressed", HoldOn::nothing, true};
Holds<Hidden, Hidden, apply_hidden, keep_hidden, restore_hidden> hides{"input.hidden", HoldOn::nothing, true};
Holds<State, State, apply_state, keep_state, restore_state> states{"input.state", HoldOn::nothing, true};

// One read of each controller per step, and what it held at the step before, which players routed to the same
// controller share.
struct Sample {
    TSUCHI_PadState physical{};
    uint64_t step = 0;
    uint64_t raw = 0; // every button bit the system gave, for cheats' button checks
    uint32_t previous = 0, last = 0;
    bool known = false;
} samples[kSlots];

// The system's keyboard, found by name in its library, and read at most once per step.
struct Keyboard {
    void (*initialize)() = nullptr;
    void (*get)(void* state) = nullptr;
    bool initialized = false;
    uint64_t step = 0;
    alignas(8) uint8_t state[t::keyboard_state::most] = {};
} keyboard;

bool player_ok(uint32_t player) { return player < game::kPlayerSlots; }
bool pad_ok(uint32_t pad) { return pad < 8 || pad == t::constant::pad_handheld; }
uint32_t slot_of(uint32_t pad) { return pad < 8 ? pad : 8; }
uint32_t pad_of(uint32_t slot) { return slot < 8 ? slot : uint32_t(t::constant::pad_handheld); }

bool foreign(TSUCHI_Mod* mod, uint32_t pad) {
    const TSUCHI_Mod* const holders[] = {presses.holder(0, pad), hides.holder(0, pad), states.holder(0, pad)};
    for (const TSUCHI_Mod* holder : holders)
        if (holder && holder != mod) return true;
    return false;
}

uint64_t inject(uint32_t pad, uint64_t buttons) {
    if (const State* state = states.find(0, pad)) buttons = state->buttons;
    if (const Press* press = presses.find(0, pad)) buttons |= press->buttons;
    return buttons;
}

// What the game is given of a controller: what it holds, with the presses added and the hidden buttons taken out.
uint64_t filter(uint32_t pad, uint64_t buttons, bool) {
    buttons = inject(pad, buttons);
    if (const Hidden* hidden = hides.find(0, pad)) buttons &= ~uint64_t(hidden->buttons);
    return buttons;
}

template <unsigned I>
void poll(void* state, const void* pad_at) {
    next_poll[I](state, pad_at);
    if (!state || !pad_at || !on_game_thread()) return;
    uint32_t pad;
    memcpy(&pad, pad_at, sizeof pad);
    if (!pad_ok(pad)) return;
    // While the presenter reads a controller itself, it is given what the system says. A press that ended leaves
    // no tap behind, and a tap the player made is kept.
    if (presenter_sampling_input()) return;
    uint8_t* const at = static_cast<uint8_t*>(state) + t::npad_state::buttons;
    uint64_t buttons;
    memcpy(&buttons, at, sizeof buttons);
    buttons = filter(pad, buttons, false);
    memcpy(at, &buttons, sizeof buttons);
    if (const State* replacement = states.find(0, pad)) {
        auto* raw = static_cast<uint8_t*>(state);
        memcpy(raw + t::input_state::sticks, replacement->axes, sizeof replacement->axes);
        uint32_t attributes;
        memcpy(&attributes, raw + t::input_state::connected, sizeof attributes);
        attributes |= 1;
        memcpy(raw + t::input_state::connected, &attributes, sizeof attributes);
    }
}

// The styles the system says a controller is connected as, as the game is given them. One that a mod holds
// whole, and that the system has no style for, reads as a Pro Controller: the game then asks for its state,
// which is the held one.
uint32_t style(const void* pad_at) {
    const uint32_t styles = next_style(pad_at);
    if (!pad_at || !on_game_thread()) return styles;
    if (styles & (t::constant::npad_full_key | t::constant::npad_handheld | t::constant::npad_joy_dual)) return styles;
    uint32_t pad;
    memcpy(&pad, pad_at, sizeof pad);
    return pad_ok(pad) && states.find(0, pad) ? styles | uint32_t(t::constant::npad_full_key) : styles;
}

// The import slot of the system's style reader. The table has the game's call of it, which is a stub: it loads
// the slot's address from its own page and an offset, and jumps to what the slot holds. 0 when the stub is not
// of that form.
uint64_t style_slot() {
    if (!t::bound(t::fn::npad_style_set)) return 0;
    const uint64_t stub = reinterpret_cast<uint64_t>(t::fn::npad_style_set);
    const uint32_t page_of = game::read_or<uint32_t>(stub), load = game::read_or<uint32_t>(stub + 4);
    // adrp x16, <page>; ldr x17, [x16, #<offset>]
    if ((page_of & 0x9F00001F) != 0x90000010 || (load & 0xFFC003FF) != 0xF9400211) return 0;
    const uint64_t field = uint64_t(page_of >> 5 & 0x7FFFF) << 2 | (page_of >> 29 & 3);
    const int64_t pages = int64_t(field << 43) >> 43; // 21 bits, with their sign
    return (stub & ~uint64_t(0xFFF)) + uint64_t(pages * 0x1000) + uint64_t(load >> 10 & 0xFFF) * 8;
}

// Wraps each import slot once the system has bound it. Until the game's first call through a slot, the slot
// leads to the resolver.
void install() {
    if (!self) return;
    if (!style_installed) {
        const uint64_t entry = style_slot(), target = entry ? game::read_or<uint64_t>(entry) : 0;
        uint64_t id;
        void* next = nullptr;
        if (target && target != game::main_base() + t::constant::input_resolver && game::executable(target) &&
            hook_wrap(self, entry, 0, reinterpret_cast<void*>(&style), 0, &id, &next) == TSUCHI_OK) {
            next_style = reinterpret_cast<Style>(next);
            style_installed = true;
        }
    }
    const uint64_t slots[] = {t::data::input_full_key.address, t::data::input_handheld.address, t::data::input_joy_dual.address};
    const Poll wrappers[] = {poll<0>, poll<1>, poll<2>};
    for (unsigned i = 0; i < 3; ++i) {
        if (installed[i]) continue;
        const uint64_t entry = game::main_base() + slots[i], target = game::read_or<uint64_t>(entry);
        if (!target || target == game::main_base() + t::constant::input_resolver || !game::executable(target)) continue;
        uint64_t id;
        void* next = nullptr;
        if (hook_wrap(self, entry, 0, reinterpret_cast<void*>(wrappers[i]), 0, &id, &next) != TSUCHI_OK) continue;
        next_poll[i] = reinterpret_cast<Poll>(next);
        installed[i] = true;
    }
}

TSUCHI_Result routed(uint32_t player, uint32_t& pad) {
    if (!player_ok(player)) return TSUCHI_INVALID;
    return game::player_pad(player, pad) && pad_ok(pad) ? TSUCHI_OK : TSUCHI_NOT_READY;
}

Sample* sampled(uint32_t pad) {
    if (!step_number) return nullptr;
    Sample& sample = samples[slot_of(pad)];
    if (sample.known && sample.step == step_number) return &sample;
    alignas(8) uint8_t raw[t::npad_state::most] = {};
    if (!presenter_sample_input(pad, raw)) {
        sample = {};
        return nullptr;
    }
    sample.previous = sample.known && sample.step + 1 == step_number ? sample.last : 0;
    sample.known = true;
    sample.step = step_number;
    uint64_t buttons;
    uint32_t connected;
    int32_t axes[4];
    memcpy(&buttons, raw + t::npad_state::buttons, sizeof buttons);
    memcpy(&connected, raw + t::input_state::connected, sizeof connected);
    memcpy(axes, raw + t::input_state::sticks, sizeof axes);
    sample.physical.connected = (connected & 1) != 0;
    sample.raw = buttons;
    sample.physical.held = uint32_t(buttons) & kButtons;
    sample.physical.left_x = axes[0];
    sample.physical.left_y = axes[1];
    sample.physical.right_x = axes[2];
    sample.physical.right_y = axes[3];
    return &sample;
}

void step() {
    ++step_number;
    install();
    // A controller keeps its history while no player uses it, so a player routed away and back keeps its edges.
    for (uint32_t i = 0; i < kSlots; ++i)
        if (samples[i].known) sampled(pad_of(i));
    for (uint32_t player = 0; player < game::kPlayerSlots; ++player) {
        uint32_t pad;
        if (routed(player, pad) == TSUCHI_OK) sampled(pad);
    }
    // The first read can bind a slot. It is wrapped before the game reads the controller in this step.
    install();
}

// After every mod has heard the step: what each controller held, with the presses mods made in their handlers.
void commit() {
    for (uint32_t i = 0; i < kSlots; ++i) {
        Sample& sample = samples[i];
        if (sample.known && sample.step == step_number) sample.last = uint32_t(inject(pad_of(i), sample.physical.held));
    }
}

TSUCHI_Result pad(uint32_t player, TSUCHI_PadState* out) {
    uint32_t id;
    const TSUCHI_Result result = routed(player, id);
    if (result != TSUCHI_OK) return result;
    const Sample* sample = sampled(id);
    if (!sample) return TSUCHI_NOT_READY;
    *out = sample->physical;
    if (const State* replacement = states.find(0, id)) {
        out->connected = true;
        out->left_x = replacement->axes[0]; out->left_y = replacement->axes[1];
        out->right_x = replacement->axes[2]; out->right_y = replacement->axes[3];
    }
    out->held = uint32_t(inject(id, out->held));
    out->pressed = out->held & ~sample->previous;
    out->released = sample->previous & ~out->held;
    return TSUCHI_OK;
}

bool keyboard_found() {
    if (keyboard.get) return true;
    const uint64_t library = game::module_containing(game::global<uint64_t>(t::data::system_library_anchor));
    if (!library) return false;
    const uint64_t initialize = game::module_symbol(library, "_ZN2nn3hid18InitializeKeyboardEv");
    const uint64_t get = game::module_symbol(library, "_ZN2nn3hid16GetKeyboardStateEPNS0_13KeyboardStateE");
    if (!initialize || !get) return false;
    keyboard.initialize = reinterpret_cast<void (*)()>(initialize);
    keyboard.get = reinterpret_cast<void (*)(void*)>(get);
    return true;
}

TSUCHI_Result key(uint32_t usage, bool* out) {
    if (usage > 255) return TSUCHI_INVALID;
    if (!step_number || !keyboard_found()) return TSUCHI_NOT_READY;
    if (!keyboard.initialized) keyboard.initialize(), keyboard.initialized = true;
    if (keyboard.step != step_number) {
        memset(keyboard.state, 0, sizeof keyboard.state);
        keyboard.get(keyboard.state);
        keyboard.step = step_number;
    }
    // The keys are a set of 256 bits, by usage, kept in 32-bit words from the lowest bit up.
    *out = (keyboard.state[t::keyboard_state::keys + usage / 8] >> (usage % 8) & 1) != 0;
    return TSUCHI_OK;
}

// A player may have moved to another controller since a hold was taken through it. The hold stays with the
// controller it was taken on, and the player leaves it.
template <class Table>
void move_association(Table& table, TSUCHI_Mod* mod, uint32_t player, uint32_t except_pad) {
    const uint32_t bit = 1u << player;
    for (uint32_t i = table.count(); i--;) {
        const Hold hold = table.hold_at(i);
        if (hold.mod != mod || hold.what == except_pad) continue;
        auto& kept = table.kept_at(i);
        if (!(kept.players & bit)) continue;
        kept.players &= ~bit;
        if (!kept.players) table.release(mod, 0, hold.what);
    }
}

TSUCHI_Result hold_pressed(TSUCHI_Mod* mod, uint32_t player, TSUCHI_Buttons buttons, uint32_t steps) {
    if (!buttons || !steps || steps > 600) return TSUCHI_INVALID;
    uint32_t id;
    const TSUCHI_Result result = routed(player, id);
    if (result != TSUCHI_OK) return result;
    if (foreign(mod, id)) return TSUCHI_BUSY;
    const Press* prior = presses.find(mod, 0, id);
    const uint32_t players = (prior ? prior->players : 0) | (1u << player);
    // A press taken during a step counts that step.
    const TSUCHI_Result taken = presses.take(mod, 0, id, {buttons, players, step_number + steps - (frame_stepping() ? 1u : 0u)});
    if (taken == TSUCHI_OK) move_association(presses, mod, player, id);
    return taken;
}

TSUCHI_Result hold_hidden(TSUCHI_Mod* mod, uint32_t player, TSUCHI_Buttons buttons) {
    if (!buttons) return TSUCHI_INVALID;
    uint32_t id;
    const TSUCHI_Result result = routed(player, id);
    if (result != TSUCHI_OK) return result;
    if (foreign(mod, id)) return TSUCHI_BUSY;
    const Hidden* prior = hides.find(mod, 0, id);
    const uint32_t players = (prior ? prior->players : 0) | (1u << player);
    const TSUCHI_Result taken = hides.take(mod, 0, id, {buttons, players});
    if (taken == TSUCHI_OK) move_association(hides, mod, player, id);
    return taken;
}

template <class Table>
TSUCHI_Result release_player(Table& table, TSUCHI_Mod* mod, uint32_t player) {
    if (!player_ok(player)) return TSUCHI_INVALID;
    for (uint32_t i = table.count(); i--;) {
        const Hold hold = table.hold_at(i);
        if (hold.mod == mod && (table.kept_at(i).players & (1u << player))) return table.release(mod, 0, hold.what);
    }
    return TSUCHI_NOT_FOUND;
}
TSUCHI_Result release_pressed(TSUCHI_Mod* mod, uint32_t player) { return release_player(presses, mod, player); }
TSUCHI_Result release_hidden(TSUCHI_Mod* mod, uint32_t player) { return release_player(hides, mod, player); }

TSUCHI_Result hold_state(TSUCHI_Mod* mod, uint32_t player, TSUCHI_Buttons buttons,
                        int32_t left_x, int32_t left_y, int32_t right_x, int32_t right_y, uint32_t steps) {
    if ((buttons & ~kButtons) || !steps || steps > 600) return TSUCHI_INVALID;
    const int32_t axes[] = {left_x, left_y, right_x, right_y};
    for (const int32_t axis : axes)
        if (axis < -32767 || axis > 32767) return TSUCHI_INVALID;
    uint32_t id;
    const TSUCHI_Result result = routed(player, id);
    if (result != TSUCHI_OK) return result;
    if (foreign(mod, id)) return TSUCHI_BUSY;
    const State* prior = states.find(mod, 0, id);
    const uint32_t players = (prior ? prior->players : 0) | (1u << player);
    const State wanted{buttons, players, step_number + steps - (frame_stepping() ? 1u : 0u),
                       {left_x, left_y, right_x, right_y}};
    const TSUCHI_Result taken = states.take(mod, 0, id, wanted);
    if (taken == TSUCHI_OK) move_association(states, mod, player, id);
    return taken;
}
TSUCHI_Result release_state(TSUCHI_Mod* mod, uint32_t player) { return release_player(states, mod, player); }

TSUCHI_Result pressed(uint32_t player, TSUCHI_Buttons* out) {
    uint32_t id;
    const TSUCHI_Result result = routed(player, id);
    if (result != TSUCHI_OK) return result;
    const Press* press = presses.find(0, id);
    *out = press ? press->buttons : 0;
    return TSUCHI_OK;
}

TSUCHI_Result hidden(uint32_t player, TSUCHI_Buttons* out) {
    uint32_t id;
    const TSUCHI_Result result = routed(player, id);
    if (result != TSUCHI_OK) return result;
    const Hidden* hidden = hides.find(0, id);
    *out = hidden ? hidden->buttons : 0;
    return TSUCHI_OK;
}

const input::Backend service{pad, key, hold_pressed, release_pressed, pressed, hold_hidden, release_hidden, hidden,
                             hold_state, release_state};

} // namespace

uint64_t input_keys_held() {
    uint64_t keys = 0;
    for (const Sample& sample : samples)
        if (sample.known && sample.step == step_number) keys |= sample.raw;
    return keys;
}

void input_start(TSUCHI_Mod* owner) {
    self = owner;
    input::set_backend(&service);
    presses.start();
    hides.start();
    states.start();
    presenter_input_after(filter);
    each_frame(step);
    after_step(commit);
    install();
}

} // namespace tsuchi
