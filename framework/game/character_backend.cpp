// The character service: a captain's or a dog's logical inputs, its move direction, and its actions, without a
// controller.
//
// A captain or a dog reads its buttons through its own controller object (character_part.controller), which turns
// the pad's buttons into logical inputs. Every gameplay controller shares one function table. The service wraps
// four of its entries once: held, pressed and released, which add the inputs mods hold on that controller, and the
// controller's update, which latches what is held once a frame so that a press is pressed for one frame. The
// update's own refresh and the press-age table read the other three entries, so the game's input buffer and the
// captain's idle check see the inputs too.
//
// An action reads its input at a few instructions of its own code (game table, character_sites). The service puts an
// inline hook on each, the first time a mod asks for that action. At a call of the controller's reader, the hook
// either points the call at an answer of its own (an action held, or bound to no input), or changes the input
// number the game asks for (an action bound to another input). At the instruction after a call to a helper of the
// game's, it changes the answer. At a read of the press-age table, it moves the read to the bound input's entry.
// After the game writes the move direction, it writes the held direction over it.
//
// Holds are per character and per input, action or binding, each exclusive, and kept in the holds tables. What the
// hooks read is a small table of records, one per character with any hold, rebuilt from the holds whenever one
// changes. Game thread only, apart from what the hooks read.
#include "actors.h"
#include "captain.h"
#include "character_service.h"
#include "frame.h"
#include "game.h"
#include "holds.h"
#include "hooks.h"
#include "mods.h"
#include "objects.h"
#include "people.h"
#include "services.h"
#include "tsuchi/hook.h"

#include <string.h>

namespace tsuchi {
namespace {
namespace t = game::table;

constexpr uint32_t kInputs = 33;  // the inputs a controller's press-age table has, 0 to 32
constexpr uint32_t kActions = 10; // as the schema's Action enum
constexpr uint32_t kRecords = 16;
constexpr uint8_t kOwn = 0xFE;    // a binding: the action reads its own input
constexpr uint8_t kNone = uint8_t(TSUCHI_INPUT_NONE);
// How many controller updates an action's press or release waits for its site to read it. The game reads a site
// only in some states and frames, and not always between two updates, so the press waits for the read. 30 updates
// are half a second at 60 frames a second.
constexpr uint8_t kPendingUpdates = 30;
enum : uint8_t { kCaptain = 1, kDog = 2, kBoth = 3 };

// ---- What each site of the game table is ----------------------------------------------------------------------

enum class Form : uint8_t {
    call,   // a call of the controller's reader: x0 the controller, w1 the input, the target in the word's register
    result, // the instruction after a call of a helper of the game's that answers in w0, bit 0
    age,    // a load from the press-age table; the word's base register holds the table
    move,   // after the move direction is written to the steered part
};
// How the game reads the input there, which says when a held action reads as down: a press on the first frame of
// the hold only, a hold on every frame of it, a release on the frame after it ends. As a button would.
enum class Read : uint8_t { pressed, held, released, none };
struct Site {
    uint8_t action; // kActions for the move sites
    uint8_t kinds;  // whose code this is
    Form form;
    uint8_t own;    // the input it reads, for `binding`; for the age form, the entry it reads
    Read read;
};
// In the order of the game table's character_sites.
constexpr Site kSites[] = {
    // 0 Orima_WhistleInput's usual read, IsTriggered(3). The whistle lasts only while that read answers down on
    // every frame (TryWhistle 0xDE3460 ends it at the first frame it doesn't), so for a held whistle it reads as
    // held. Read as a press, a held whistle was a one-frame call (seen 11-13: FueCall, then Control 0.3 s later).
    {TSUCHI_ACTION_WHISTLE, kBoth, Form::call, 3, Read::held},
    {TSUCHI_ACTION_WHISTLE, kBoth, Form::call, 3, Read::held},                // 1 its read while comp+0x12A0 is set
    {TSUCHI_ACTION_THROW_PIKMIN, kCaptain, Form::call, 0, Read::pressed},     // 2
    {TSUCHI_ACTION_THROW_PIKMIN, kCaptain, Form::call, 0, Read::held},        // 3
    {TSUCHI_ACTION_DISMISS, kCaptain, Form::call, 8, Read::pressed},          // 4
    {TSUCHI_ACTION_SWITCH_TYPE, kBoth, Form::result, 11, Read::pressed},      // 5
    {TSUCHI_ACTION_SWITCH_TYPE_BACK, kBoth, Form::call, 12, Read::pressed},   // 6
    {TSUCHI_ACTION_RIDE, kCaptain, Form::call, 13, Read::pressed},            // 7 4 with the direct Y menu option
    {TSUCHI_ACTION_RIDE, kCaptain, Form::call, 13, Read::released},           // 8
    {TSUCHI_ACTION_RIDE, kCaptain, Form::call, 19, Read::released},           // 9
    {TSUCHI_ACTION_THROW_PIKMIN, kDog, Form::call, 18, Read::pressed},        // 10 the riders
    {TSUCHI_ACTION_THROW_PIKMIN, kDog, Form::call, 18, Read::pressed},        // 11
    {TSUCHI_ACTION_THROW_PIKMIN, kDog, Form::call, 18, Read::held},           // 12
    {TSUCHI_ACTION_BITE, kDog, Form::call, 18, Read::pressed},                // 13
    {TSUCHI_ACTION_BITE, kDog, Form::age, 18, Read::none},                    // 14
    {TSUCHI_ACTION_JUMP, kDog, Form::call, 20, Read::pressed},                // 15 buffered
    {TSUCHI_ACTION_JUMP, kDog, Form::age, 20, Read::none},                    // 16
    {TSUCHI_ACTION_JUMP, kDog, Form::call, 20, Read::pressed},                // 17
    {TSUCHI_ACTION_RUSH, kDog, Form::call, 20, Read::pressed},                // 18
    {TSUCHI_ACTION_RUSH, kDog, Form::call, 20, Read::held},                   // 19
    {TSUCHI_ACTION_INTERACT, kDog, Form::call, 18, Read::pressed},            // 20
    {kActions, kBoth, Form::move, 0, Read::none},                             // 21
    {kActions, kBoth, Form::move, 0, Read::none},                             // 22
};
constexpr uint32_t kSiteCount = sizeof kSites / sizeof kSites[0];
static_assert(kSiteCount == sizeof t::tables::character_sites / sizeof t::tables::character_sites[0], "one Site for each site of the table");

// The actions with a site that reads this way.
uint32_t actions_reading(Read read) {
    uint32_t actions = 0;
    for (const Site& site : kSites)
        if (site.read == read && site.action < kActions) actions |= 1u << site.action;
    return actions;
}

// Counters for a game run. The log gives their address once, so a script can read them with `memory.read`: 48
// bytes of totals, then 16 bytes for each site of character_sites, in its order. Little-endian, as the game is.
struct SiteCounts {
    uint32_t reads;   // the game ran the site
    uint32_t matched; // for a character with a record
    uint32_t down;    // answered down for a held or pressed action
    uint32_t up;      // answered up: an action bound to no input, or driven by a mod and not down at this read
};
struct DebugCounts {
    uint32_t magic = 0x44524843; // "CHRD"
    uint32_t sites = 0;
    uint32_t wrapped = 0;        // 1 once the controller's four entries are wrapped
    uint32_t updates = 0;        // controller updates that latched a record
    uint32_t step_latches = 0;   // latches the step made because no update came
    uint32_t held_injected = 0;  // held reads answered from a mod's hold
    uint32_t pressed_injected = 0;
    uint32_t released_injected = 0;
    uint64_t controller = 0;     // the controller object of the record latched last
    uint64_t part = 0;           // and its character's steered part
    SiteCounts site[sizeof kSites / sizeof kSites[0]];
};
static_assert(sizeof(DebugCounts) == 48 + 16 * (sizeof kSites / sizeof kSites[0]), "the layout the log describes");
DebugCounts debug;
bool debug_told = false;

// Appends a number to a line, in hex or in decimal.
void append(char* line, uint32_t room, uint64_t value, bool hex) {
    char digits[20];
    uint32_t used = 0;
    do digits[used++] = "0123456789abcdef"[value % (hex ? 16 : 10)], value /= hex ? 16 : 10; while (value && used < sizeof digits);
    uint32_t at = uint32_t(strlen(line));
    if (hex && at + 2 < room) line[at++] = '0', line[at++] = 'x';
    while (used && at + 1 < room) line[at++] = digits[--used];
    line[at] = 0;
}
void append(char* line, uint32_t room, const char* text) {
    uint32_t at = uint32_t(strlen(line));
    while (*text && at + 1 < room) line[at++] = *text++;
    line[at] = 0;
}

// Once a run, the first time a mod takes a hold: where the counters are.
void tell_debug() {
    if (debug_told) return;
    debug_told = true;
    debug.sites = uint32_t(sizeof kSites / sizeof kSites[0]);
    char line[160] = "character: debug counters at ";
    append(line, sizeof line, reinterpret_cast<uint64_t>(&debug), true);
    append(line, sizeof line, ", 48 bytes of totals and then 16 bytes for each site (reads, matched, down, up)");
    mod_note(kFrameworkLogId, line);
}

uint8_t action_kinds(uint32_t action) {
    uint8_t kinds = 0;
    for (const Site& site : kSites)
        if (site.action == action) kinds |= site.kinds;
    return kinds;
}

// ---- One record for each character with a hold ----------------------------------------------------------------

struct Record {
    uint64_t handle, actor, part, controller;
    uint8_t kind;
    bool shared;             // another character has the same controller object
    bool latched;            // its controller's update ran since the last step
    bool empty;              // no hold is left: it goes once what it last latched has gone up
    uint64_t held, pressing; // inputs: held by mods, and pressed once and not yet latched
    uint64_t now, before;    // inputs as the controller reads them this frame and the frame before
    uint64_t used;           // pressed once and latched: the press's hold ends at the next step
    uint32_t act_held, act_pressing, act_now, act_before, act_used;
    uint32_t press_pending, release_pending; // actions whose press, or release, a site has yet to read
    // Actions whose press a site read since the controller's last update: every read until the next update sees
    // it, as a button's press lasts its frame. The game asks more than once a frame (a whistle's press was read
    // three times in its frame; answered at the first read only, the whistle never started). A release is still
    // read once: the ride's two release reads are of two inputs, and one release is one ride.
    uint32_t press_now;
    uint32_t act_quiet;      // actions whose hold ended with its mod or its character: no release is read
    uint8_t press_waited[kActions], release_waited[kActions];
    uint64_t act_inputs, act_inputs_before; // the inputs that the actions down this frame and the frame before read
    uint8_t bound[kActions];
    bool moving;
    float move[3];
};
Record records[kRecords];
uint32_t record_count = 0; // records in use are [0, record_count)

Record* record_of(uint64_t handle) {
    for (uint32_t i = 0; i < record_count; ++i)
        if (records[i].handle == handle) return &records[i];
    return nullptr;
}

// Who a handle names: a captain or a dog, with its steered part and controller. False for anything else.
struct Who {
    uint64_t actor, part, controller;
    uint8_t kind;
};
bool who(uint64_t handle, Who& out) {
    const uint64_t actor = game::from_handle(handle);
    if (!actor || !game::is_game_actor(actor)) return false;
    uint8_t kind = 0;
    if (game::captain_part(actor)) kind = kCaptain;
    else if (game::read_or<uint8_t>(actor + t::game_actor::sort) & t::constant::sort_dog) kind = kDog;
    if (!kind) return false;
    const uint64_t part = game::read_or<uint64_t>(actor + t::game_actor::player);
    out = {actor, part, part ? game::read_or<uint64_t>(part + t::character_part::controller) : 0, kind};
    return true;
}

// True when another captain or dog of the world reads the same controller object.
bool shares(uint64_t actor, uint64_t controller) {
    if (!controller) return false;
    for (uint32_t player = 0; player < game::kPlayerSlots; ++player) {
        const uint64_t characters[] = {game::player_captain(player), game::player_dog(player)};
        for (const uint64_t other : characters) {
            if (!other || other == actor) continue;
            const uint64_t part = game::read_or<uint64_t>(other + t::game_actor::player);
            if (part && game::read_or<uint64_t>(part + t::character_part::controller) == controller) return true;
        }
    }
    return false;
}

void refresh(Record& record) {
    Who now{};
    if (!who(record.handle, now)) {
        record.part = record.controller = 0;
        return;
    }
    record.actor = now.actor;
    record.kind = now.kind;
    record.part = now.part;
    record.controller = now.controller;
    record.shared = shares(now.actor, now.controller);
}

// ---- The holds ------------------------------------------------------------------------------------------------

struct Press {
    uint8_t once; // a press for one frame, not a hold
};
struct Bind {
    uint8_t input;
};
struct Move {
    float direction[3];
};

void sync(uint64_t handle);

template <class T>
TSUCHI_Result take_as(const Hold&, T& kept, const T& wanted, bool) { return kept = wanted, TSUCHI_OK; }
// A press ends once its controller has latched it.
bool keep_input(const Hold& hold, Press& kept) {
    const Record* record = record_of(hold.object);
    return !(kept.once && record && (record->used >> hold.what & 1));
}
bool keep_action(const Hold& hold, Press& kept) {
    const Record* record = record_of(hold.object);
    return !(kept.once && record && (record->act_used >> hold.what & 1));
}
void end_input(const Hold& hold, Press&, HoldEnd) {
    if (Record* record = record_of(hold.object)) record->used &= ~(uint64_t(1) << hold.what);
    sync(hold.object);
}
void end_action(const Hold& hold, Press&, HoldEnd why) {
    if (Record* record = record_of(hold.object)) {
        record->act_used &= ~(1u << hold.what);
        // A press released before the game read it is still read once, as a tap is. One whose mod stopped, or whose
        // character left, is not.
        if (why != HoldEnd::released && why != HoldEnd::lost) {
            record->press_pending &= ~(1u << hold.what);
            record->release_pending &= ~(1u << hold.what);
            record->press_now &= ~(1u << hold.what);
            record->act_quiet |= 1u << hold.what;
        }
    }
    sync(hold.object);
}
template <class T>
void end_other(const Hold& hold, T&, HoldEnd) { sync(hold.object); }

Holds<Press, Press, take_as<Press>, keep_input, end_input> inputs{"character.input", HoldOn::actor, true};
Holds<Press, Press, take_as<Press>, keep_action, end_action> actions{"character.action", HoldOn::actor, true};
Holds<Bind, Bind, take_as<Bind>, nullptr, end_other<Bind>> bindings{"character.binding", HoldOn::actor, true};
Holds<Move, Move, take_as<Move>, nullptr, end_other<Move>> moves{"character.move", HoldOn::actor, true};

// The record of a character, made again from its holds. A character with none has no record.
void sync(uint64_t handle) {
    Record* record = record_of(handle);
    uint64_t held = 0, pressing = 0;
    uint32_t act_held = 0, act_pressing = 0;
    uint8_t bound[kActions];
    memset(bound, kOwn, sizeof bound);
    bool moving = false, any = false;
    float move[3] = {};
    for (uint32_t i = 0; i < inputs.count(); ++i) {
        const Hold& hold = inputs.hold_at(i);
        if (hold.object != handle) continue;
        any = true;
        const uint64_t bit = uint64_t(1) << hold.what;
        if (!inputs.kept_at(i).once) held |= bit;
        else if (!(record && (record->used & bit))) pressing |= bit;
    }
    for (uint32_t i = 0; i < actions.count(); ++i) {
        const Hold& hold = actions.hold_at(i);
        if (hold.object != handle) continue;
        any = true;
        const uint32_t bit = 1u << hold.what;
        if (!actions.kept_at(i).once) act_held |= bit;
        else if (!(record && (record->act_used & bit))) act_pressing |= bit;
    }
    for (uint32_t i = 0; i < bindings.count(); ++i) {
        const Hold& hold = bindings.hold_at(i);
        if (hold.object != handle) continue;
        any = true;
        bound[hold.what] = bindings.kept_at(i).input;
    }
    for (uint32_t i = 0; i < moves.count(); ++i) {
        const Hold& hold = moves.hold_at(i);
        if (hold.object != handle) continue;
        any = moving = true;
        memcpy(move, moves.kept_at(i).direction, sizeof move);
    }
    if (!record) {
        if (!any || record_count == kRecords) return; // a full table is refused before a hold is taken: see room()
        record = &records[record_count];
        *record = Record{};
        record->handle = handle;
        record->latched = true; // its controller has a frame to show that it updates
        refresh(*record);
        ++record_count;
    }
    record->empty = !any;
    record->held = held;
    record->pressing = pressing;
    record->act_held = act_held;
    record->act_pressing = act_pressing;
    memcpy(record->bound, bound, sizeof bound);
    record->moving = moving;
    memcpy(record->move, move, sizeof move);
}

// A new character's record has room.
bool room(uint64_t handle) { return record_of(handle) || record_count < kRecords; }

// The input an action reads on this character: the one a mod bound it to, or its own. kNone for none.
uint8_t input_of(const Record& record, uint32_t action) {
    if (record.bound[action] != kOwn) return record.bound[action];
    for (const Site& site : kSites)
        if (site.action == action && (site.kinds & record.kind)) return site.own;
    return kNone;
}

// Once a controller update: what is held now, and what was held before. A press latched here ends its hold at the
// next step.
void latch(Record& record) {
    record.press_now = 0;
    record.before = record.now;
    record.now = record.held | record.pressing;
    record.used |= record.pressing;
    record.pressing = 0;
    record.act_before = record.act_now;
    record.act_now = record.act_held | record.act_pressing;
    record.act_used |= record.act_pressing;
    record.act_pressing = 0;
    // An action that went down has a press for its pressed sites, and one that went up a release for its released
    // sites. Each waits for the first read of it, which ends it at every site of the action, or gives up.
    const uint32_t went_down = record.act_now & ~record.act_before & actions_reading(Read::pressed),
                   went_up = record.act_before & ~record.act_now & actions_reading(Read::released) & ~record.act_quiet;
    record.act_quiet &= record.act_now; // a quiet end has been latched
    for (uint32_t action = 0; action < kActions; ++action) {
        const uint32_t bit = 1u << action;
        if (went_down & bit) record.press_pending |= bit, record.press_waited[action] = 0;
        else if ((record.press_pending & bit) && ++record.press_waited[action] > kPendingUpdates) record.press_pending &= ~bit;
        if (went_up & bit) record.release_pending |= bit, record.release_waited[action] = 0;
        else if ((record.release_pending & bit) && ++record.release_waited[action] > kPendingUpdates) record.release_pending &= ~bit;
    }
    // A new press ends a release nobody read. A press outlives its hold, as a tap does.
    record.release_pending &= ~(record.act_now & ~record.act_before);
    // A held action presses and holds the input it reads, as a held button would, so that the game's reads of that
    // input outside the action's own sites see it: the press-age table that the update fills, the idle check, and
    // whatever keeps the action going.
    record.act_inputs_before = record.act_inputs;
    record.act_inputs = 0;
    for (uint32_t action = 0; action < kActions; ++action) {
        const uint8_t input = record.act_now >> action & 1 ? input_of(record, action) : kNone;
        if (input < kInputs) record.act_inputs |= uint64_t(1) << input;
    }
    record.latched = true;
}

// ---- The controller's entries ---------------------------------------------------------------------------------

using Held = bool (*)(void*, int32_t);
using Pressed = bool (*)(void*, int32_t, void*, void*);
using Update = void (*)(void*, float);
Held next_held = nullptr, next_released = nullptr;
Pressed next_pressed = nullptr;
Update next_update = nullptr;
bool wrapped = false;
TSUCHI_Mod* self = nullptr;

// The record whose inputs a controller reads: one with inputs, alone on that controller.
const Record* reading(uint64_t controller) {
    for (uint32_t i = 0; i < record_count; ++i) {
        const Record& record = records[i];
        if (record.controller == controller && !record.shared && (record.now | record.before | record.act_inputs | record.act_inputs_before))
            return &record;
    }
    return nullptr;
}

// False while the game hides the input: every input while the controller is locked, and a masked one while its
// timer runs.
bool open(uint64_t controller, int32_t input) {
    if (const auto locked = game::method(controller, t::game_controller_table::locked); locked && locked(reinterpret_cast<void*>(controller)))
        return false;
    const float timer = game::read_or<float>(controller + t::game_controller::lock_timer);
    return !(timer > 0 && (game::read_or<uint64_t>(controller + t::game_controller::lock_mask) >> (uint32_t(input) & 63) & 1));
}

// The inputs that mods hold down on a controller this frame, by input or by action.
uint64_t down(const Record& record) { return record.now | record.act_inputs; }
uint64_t down_before(const Record& record) { return record.before | record.act_inputs_before; }

bool held(void* controller, int32_t input) {
    if (next_held(controller, input)) return true;
    const Record* record = input >= 0 && uint32_t(input) < kInputs ? reading(reinterpret_cast<uint64_t>(controller)) : nullptr;
    const bool injected = record && (down(*record) >> input & 1) && open(reinterpret_cast<uint64_t>(controller), input);
    debug.held_injected += injected;
    return injected;
}

bool pressed(void* controller, int32_t input, void* repeat, void* count) {
    const uint64_t at = reinterpret_cast<uint64_t>(controller);
    const Record* record = input >= 0 && uint32_t(input) < kInputs ? reading(at) : nullptr;
    if (next_pressed(controller, input, repeat, count)) {
        // The update repeats a held input as presses, from what the held entry says (0x2042C48: after 0.55 s, then
        // every 0.1 s, then every 0.05 s). An input that only a mod holds gives the game its one press, not repeats.
        if (!(record && (down(*record) >> input & 1) && !next_held(controller, input))) return true;
    }
    // The game reads no press for two frames after a pause ends.
    const bool injected = record && ((down(*record) & ~down_before(*record)) >> input & 1) &&
                          game::read_or<int32_t>(at + t::game_controller::unpaused_frames) >= 2 && open(at, input);
    debug.pressed_injected += injected;
    return injected;
}

bool released(void* controller, int32_t input) {
    if (next_released(controller, input)) return true;
    const Record* record = input >= 0 && uint32_t(input) < kInputs ? reading(reinterpret_cast<uint64_t>(controller)) : nullptr;
    const bool injected = record && ((down_before(*record) & ~down(*record)) >> input & 1) && open(reinterpret_cast<uint64_t>(controller), input);
    debug.released_injected += injected;
    return injected;
}

void update(void* controller, float seconds) {
    for (uint32_t i = 0; i < record_count; ++i)
        if (records[i].controller == reinterpret_cast<uint64_t>(controller)) {
            latch(records[i]);
            ++debug.updates;
            debug.controller = records[i].controller;
            debug.part = records[i].part;
        }
    next_update(controller, seconds);
}

TSUCHI_Result wrap_controller() {
    if (wrapped) return TSUCHI_OK;
    if (!self) return TSUCHI_NOT_READY;
    uint64_t id;
    Held held_next = nullptr, released_next = nullptr;
    Pressed pressed_next = nullptr;
    Update update_next = nullptr;
    // All four or none: an update without the readers would latch for nothing, and the readers without it would
    // never see a press end.
    TSUCHI_Result result = hook_slot(self, t::slot::controller_held, &held, &held_next, 0, &id);
    if (result == TSUCHI_OK) result = hook_slot(self, t::slot::controller_pressed, &pressed, &pressed_next, 0, &id);
    if (result == TSUCHI_OK) result = hook_slot(self, t::slot::controller_released, &released, &released_next, 0, &id);
    if (result == TSUCHI_OK) result = hook_slot(self, t::slot::controller_update, &update, &update_next, 0, &id);
    // A hook that was put on stays, and passes through until the rest are on.
    if (held_next) next_held = held_next;
    if (pressed_next) next_pressed = pressed_next;
    if (released_next) next_released = released_next;
    if (update_next) next_update = update_next;
    wrapped = next_held && next_pressed && next_released && next_update;
    debug.wrapped = wrapped;
    return wrapped ? TSUCHI_OK : (result == TSUCHI_OK ? TSUCHI_NOT_READY : result);
}

// ---- The action sites -----------------------------------------------------------------------------------------

using Inline = void (*)(TSUCHI_InlineContext*);
Inline next_site[kSiteCount] = {};
bool site_on[kSiteCount] = {};

// What a call is pointed at for an action that is held, or bound to no input. They take a reader's arguments.
bool answer_down(void*, int32_t, void*, void*) { return true; }
bool answer_up(void*, int32_t, void*, void*) { return false; }
// What a press-age read is moved to for an action bound to no input: pressed longer ago than any window.
const float never_pressed = 1e30f;

uint32_t register_of(uint32_t site) { return uint32_t(t::tables::character_site_registers[site]); }
uint32_t word_of(uint32_t site) { return uint32_t(t::tables::character_site_words[site]); }

// The record a site's registers name, among the characters the site's code is for.
Record* matching(uint32_t site, const uint64_t* x) {
    const uint32_t at = register_of(site);
    for (uint32_t i = 0; i < record_count; ++i) {
        Record& record = records[i];
        if (!(record.kind & kSites[site].kinds)) continue;
        if (at < 31 ? record.part == x[at] : record.controller == x[at == 31 ? 0 : at - 32]) {
            if (record.part && record.controller) return &record;
        }
    }
    return nullptr;
}

void at_site(uint32_t site, TSUCHI_InlineContext* context) {
    uint64_t* x = static_cast<uint64_t*>(context->registers);
    ++debug.site[site].reads;
    Record* record = matching(site, x);
    if (!record) return;
    ++debug.site[site].matched;
    const Site& what = kSites[site];
    const uint32_t word = word_of(site);
    if (what.form == Form::move) {
        if (!record->moving) return;
        const float direction[3] = {record->move[0], record->move[1], 0};
        game::write(record->part + t::character_part::move, direction);
        return;
    }
    // A held action reads as a held button does at each of its sites: pressed once, at the first read after it went
    // down; held on every frame; released once, at the first read after it went up. While a mod drives the action,
    // the pad doesn't start it.
    const uint32_t bit = 1u << what.action;
    bool down = false;
    if (what.read == Read::held) down = (record->act_now & bit) != 0;
    else if (what.read == Read::pressed && ((record->press_pending | record->press_now) & bit))
        down = true, record->press_pending &= ~bit, record->press_now |= bit;
    else if (what.read == Read::released && (record->release_pending & bit)) down = true, record->release_pending &= ~bit;
    const bool acting = ((record->act_now | record->act_before | record->press_pending | record->release_pending) & bit) != 0;
    const uint8_t bound = record->bound[what.action];
    switch (what.form) {
    case Form::call: {
        const uint32_t target = word >> 5 & 31;
        if (down) x[target] = reinterpret_cast<uint64_t>(&answer_down), ++debug.site[site].down;
        else if (bound == kNone || acting) x[target] = reinterpret_cast<uint64_t>(&answer_up), ++debug.site[site].up;
        else if (bound != kOwn) x[1] = bound;
        return;
    }
    case Form::result: {
        if (down) x[0] |= 1, ++debug.site[site].down;
        else if (bound == kNone || acting) x[0] &= ~uint64_t(1), ++debug.site[site].up;
        else if (bound != kOwn) {
            const auto read = game::method(record->controller, t::game_controller_table::pressed);
            const bool now = read && read(reinterpret_cast<void*>(record->controller), bound, nullptr, nullptr);
            x[0] = (x[0] & ~uint64_t(1)) | (now ? 1 : 0);
        }
        return;
    }
    case Form::age: {
        if (bound == kOwn) return;
        // ldr s<t>, [x<n>, #imm]: the entry's offset is the immediate, in words.
        const uint32_t base = word >> 5 & 31;
        const uint64_t offset = uint64_t(word >> 10 & 0xFFF) * 4;
        const int32_t count = game::read_or<int32_t>(record->controller + t::game_controller::press_age_count);
        if (bound == kNone || int32_t(bound) >= count) x[base] = reinterpret_cast<uint64_t>(&never_pressed) - offset;
        else x[base] += (int64_t(bound) - int64_t(what.own)) * int64_t(t::press_age::size);
        return;
    }
    case Form::move:
        return;
    }
}

template <uint32_t N>
void site_hook(TSUCHI_InlineContext* context) {
    at_site(N, context);
    if (next_site[N]) next_site[N](context);
}
template <uint32_t... N>
struct Hooks {
    static constexpr Inline all[] = {&site_hook<N>...};
};
template <uint32_t... N>
constexpr Inline Hooks<N...>::all[];
using SiteHooks = Hooks<0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22>;
static_assert(sizeof SiteHooks::all / sizeof SiteHooks::all[0] == kSiteCount, "one hook for each site");

// Puts the hooks on every site of an action, or of the movement for kActions. A site that is on stays on.
TSUCHI_Result hook_sites(uint32_t action) {
    const NativeHooks* native = native_hooks_backend();
    for (uint32_t site = 0; site < kSiteCount; ++site) {
        if (kSites[site].action != action || site_on[site]) continue;
        if (!native || !native->inline_hook || !self || !t::present(t::tables::character_sites[site])) return TSUCHI_NOT_READY;
        uint64_t id = 0;
        void* next = nullptr;
        const TSUCHI_Result result = native->inline_hook(self, uint64_t(t::tables::character_sites[site]), word_of(site),
                                                         reinterpret_cast<void*>(SiteHooks::all[site]), &id, &next);
        if (result != TSUCHI_OK) return result == TSUCHI_CAPACITY ? result : TSUCHI_NOT_READY;
        next_site[site] = reinterpret_cast<Inline>(next);
        site_on[site] = true;
    }
    return TSUCHI_OK;
}

// ---- The calls ------------------------------------------------------------------------------------------------

bool input_ok(TSUCHI_Input input) { return input < kInputs; }

// A character whose controller can take inputs: it has one, and no other character shares it.
TSUCHI_Result input_target(TSUCHI_Actor character, Who& out) {
    if (!who(character.id, out)) return TSUCHI_NOT_FOUND;
    if (!out.part || !out.controller || shares(out.actor, out.controller)) return TSUCHI_NOT_READY;
    return TSUCHI_OK;
}

TSUCHI_Result take_input(TSUCHI_Mod* mod, TSUCHI_Actor character, TSUCHI_Input input, bool once) {
    Who target;
    if (!input_ok(input)) return TSUCHI_INVALID;
    const TSUCHI_Result ready = input_target(character, target);
    if (ready != TSUCHI_OK) return ready;
    if (once && inputs.find(character.id, input)) return TSUCHI_BUSY;
    if (!room(character.id)) return TSUCHI_NO_MEMORY;
    if (const TSUCHI_Result result = wrap_controller(); result != TSUCHI_OK) return result;
    tell_debug();
    const TSUCHI_Result taken = inputs.take(mod, character.id, input, Press{uint8_t(once)});
    if (taken == TSUCHI_OK) sync(character.id);
    return taken;
}
TSUCHI_Result hold_input(TSUCHI_Mod* mod, TSUCHI_Actor character, TSUCHI_Input input) { return take_input(mod, character, input, false); }
TSUCHI_Result press_input(TSUCHI_Mod* mod, TSUCHI_Actor character, TSUCHI_Input input) { return take_input(mod, character, input, true); }
TSUCHI_Result release_input(TSUCHI_Mod* mod, TSUCHI_Actor character, TSUCHI_Input input) {
    return input_ok(input) ? inputs.release(mod, character.id, input) : TSUCHI_NOT_FOUND;
}
TSUCHI_Result input_held(TSUCHI_Actor character, TSUCHI_Input input, bool* out) {
    Who target;
    if (!who(character.id, target)) return TSUCHI_NOT_FOUND;
    *out = input_ok(input) && inputs.find(character.id, input);
    return TSUCHI_OK;
}

TSUCHI_Result hold_move(TSUCHI_Mod* mod, TSUCHI_Actor character, const TSUCHI_Vec3* direction) {
    Who target;
    if (!who(character.id, target)) return TSUCHI_NOT_FOUND;
    if (direction->x * direction->x + direction->y * direction->y > 1.0001f) return TSUCHI_INVALID;
    if (moves.holder(character.id, 0) && moves.holder(character.id, 0) != mod) return TSUCHI_BUSY;
    if (!target.part) return TSUCHI_NOT_READY;
    if (!room(character.id)) return TSUCHI_NO_MEMORY;
    if (const TSUCHI_Result result = hook_sites(kActions); result != TSUCHI_OK) return result;
    const TSUCHI_Result taken = moves.take(mod, character.id, 0, Move{{direction->x, direction->y, 0}});
    if (taken == TSUCHI_OK) sync(character.id);
    return taken;
}
TSUCHI_Result release_move(TSUCHI_Mod* mod, TSUCHI_Actor character) { return moves.release(mod, character.id, 0); }
TSUCHI_Result move(TSUCHI_Actor character, TSUCHI_Vec3* out) {
    Who target;
    if (!who(character.id, target)) return TSUCHI_NOT_FOUND;
    float direction[3];
    if (!target.part || !game::read(target.part + t::character_part::move, direction)) return TSUCHI_NOT_READY;
    *out = {direction[0], direction[1], 0};
    return TSUCHI_OK;
}

// A character an action belongs to, with the action's sites hooked.
TSUCHI_Result action_target(TSUCHI_Actor character, TSUCHI_Action action, Who& out) {
    if (!who(character.id, out)) return TSUCHI_NOT_FOUND;
    if (action >= kActions || !(action_kinds(action) & out.kind)) return TSUCHI_UNSUPPORTED;
    return TSUCHI_OK;
}

TSUCHI_Result take_action(TSUCHI_Mod* mod, TSUCHI_Actor character, TSUCHI_Action action, bool once) {
    Who target;
    const TSUCHI_Result result = action_target(character, action, target);
    if (result != TSUCHI_OK) return result;
    if (once ? actions.find(character.id, action) != nullptr : (actions.holder(character.id, action) && actions.holder(character.id, action) != mod))
        return TSUCHI_BUSY;
    if (!room(character.id)) return TSUCHI_NO_MEMORY;
    if (const TSUCHI_Result hooked = hook_sites(action); hooked != TSUCHI_OK) return hooked;
    // The action presses and holds its input at the controller too, so the controller's entries have to be on. A
    // game run with no input held before showed a held whistle start nothing without them.
    if (const TSUCHI_Result result = wrap_controller(); result != TSUCHI_OK) return result;
    tell_debug();
    const TSUCHI_Result taken = actions.take(mod, character.id, action, Press{uint8_t(once)});
    if (taken == TSUCHI_OK) sync(character.id);
    return taken;
}
TSUCHI_Result hold_action(TSUCHI_Mod* mod, TSUCHI_Actor character, TSUCHI_Action action) { return take_action(mod, character, action, false); }
TSUCHI_Result press_action(TSUCHI_Mod* mod, TSUCHI_Actor character, TSUCHI_Action action) { return take_action(mod, character, action, true); }
TSUCHI_Result release_action(TSUCHI_Mod* mod, TSUCHI_Actor character, TSUCHI_Action action) {
    return action < kActions ? actions.release(mod, character.id, action) : TSUCHI_NOT_FOUND;
}
TSUCHI_Result action_held(TSUCHI_Actor character, TSUCHI_Action action, bool* out) {
    Who target;
    if (!who(character.id, target)) return TSUCHI_NOT_FOUND;
    *out = action < kActions && actions.find(character.id, action);
    return TSUCHI_OK;
}

TSUCHI_Result hold_binding(TSUCHI_Mod* mod, TSUCHI_Actor character, TSUCHI_Action action, TSUCHI_Input input) {
    Who target;
    const TSUCHI_Result result = action_target(character, action, target);
    if (result != TSUCHI_OK) return result;
    if (!input_ok(input) && input != TSUCHI_INPUT_NONE) return TSUCHI_INVALID;
    if (bindings.holder(character.id, action) && bindings.holder(character.id, action) != mod) return TSUCHI_BUSY;
    if (!room(character.id)) return TSUCHI_NO_MEMORY;
    if (const TSUCHI_Result hooked = hook_sites(action); hooked != TSUCHI_OK) return hooked;
    const TSUCHI_Result taken = bindings.take(mod, character.id, action, Bind{uint8_t(input)});
    if (taken == TSUCHI_OK) sync(character.id);
    return taken;
}
TSUCHI_Result release_binding(TSUCHI_Mod* mod, TSUCHI_Actor character, TSUCHI_Action action) {
    return action < kActions ? bindings.release(mod, character.id, action) : TSUCHI_NOT_FOUND;
}
TSUCHI_Result binding(TSUCHI_Actor character, TSUCHI_Action action, TSUCHI_Input* out) {
    Who target;
    const TSUCHI_Result result = action_target(character, action, target);
    if (result != TSUCHI_OK) return result;
    if (const Bind* bound = bindings.find(character.id, action)) {
        *out = bound->input;
        return TSUCHI_OK;
    }
    for (const Site& site : kSites)
        if (site.action == action && (site.kinds & target.kind)) {
            *out = site.own;
            return TSUCHI_OK;
        }
    return TSUCHI_UNSUPPORTED;
}

// Once a step: each record follows its character's part and controller, which a ride can change, and a character
// whose controller had no update since the last step latches here. A record with no hold left goes once nothing it
// latched is down, so the game reads each input's release.
void log_sites() {
    static uint32_t steps = 0;
    bool acting = false;
    for (uint32_t i = 0; i < record_count; ++i) acting |= (records[i].act_now | records[i].act_held) != 0;
    if (!acting || ++steps % 120) return;
    char line[200] = "character: updates ";
    append(line, sizeof line, debug.updates, false);
    append(line, sizeof line, ", step latches ");
    append(line, sizeof line, debug.step_latches, false);
    append(line, sizeof line, ", held/pressed/released answered ");
    append(line, sizeof line, debug.held_injected, false);
    append(line, sizeof line, "/");
    append(line, sizeof line, debug.pressed_injected, false);
    append(line, sizeof line, "/");
    append(line, sizeof line, debug.released_injected, false);
    for (uint32_t site = 0; site < kSiteCount; ++site) {
        const SiteCounts& counts = debug.site[site];
        if (!counts.reads) continue;
        append(line, sizeof line, "; site ");
        append(line, sizeof line, site, false);
        append(line, sizeof line, " ");
        append(line, sizeof line, counts.reads, false);
        append(line, sizeof line, "/");
        append(line, sizeof line, counts.matched, false);
        append(line, sizeof line, "/");
        append(line, sizeof line, counts.down, false);
        append(line, sizeof line, "/");
        append(line, sizeof line, counts.up, false);
    }
    mod_note(kFrameworkLogId, line);
}

void step() {
    log_sites();
    for (uint32_t i = record_count; i--;) {
        Record& record = records[i];
        refresh(record);
        if (!record.latched) latch(record), ++debug.step_latches;
        record.latched = false;
        if (record.empty && !(record.now | record.before | record.act_inputs | record.act_inputs_before) &&
            !(record.act_now | record.act_before | record.press_pending | record.release_pending))
            records[i] = records[--record_count];
    }
}

const character::Backend service{hold_input,  press_input,    release_input, input_held,   hold_move,       release_move,
                                 move,        hold_action,    press_action,  release_action, action_held, hold_binding,
                                 release_binding, binding};

} // namespace

void characters_start(TSUCHI_Mod* owner) {
    self = owner;
    character::set_backend(&service);
    inputs.start();
    actions.start();
    bindings.start();
    moves.start();
    each_frame(step);
}

} // namespace tsuchi
