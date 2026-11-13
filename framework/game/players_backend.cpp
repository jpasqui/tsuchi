// The players service: whose captain and dog are whose, which each player steers, and their controllers.
#include "actors.h"
#include "frame.h"
#include "game.h"
#include "holds.h"
#include "objects.h"
#include "people.h"
#include "player_service.h"
#include "services.h"

namespace tsuchi {
namespace {

namespace t = game::table;

// While a player is held on a character and steers the other, the game is asked to switch back this often.
constexpr uint32_t kSwitchBackFrames = 15;

struct Seen {
    bool known = false;
    uint64_t steered[game::kPlayerSlots] = {}; // handles
} seen;

uint64_t actor_of(TSUCHI_Actor actor) {
    const uint64_t object = game::from_handle(actor.id);
    return object && game::is_game_actor(object) ? object : 0;
}

TSUCHI_Result count(uint32_t* out) {
    uint32_t players = 0;
    for (uint32_t player = 0; player < game::kPlayerSlots; ++player) players += game::player_captain(player) != 0;
    *out = players;
    return TSUCHI_OK;
}

TSUCHI_Result captain(uint32_t player, TSUCHI_Actor* out) {
    const uint64_t actor = game::player_captain(player);
    if (!actor) return TSUCHI_NOT_FOUND;
    out->id = game::to_handle(actor);
    return TSUCHI_OK;
}

TSUCHI_Result dog(uint32_t player, TSUCHI_Actor* out) {
    const uint64_t actor = game::player_dog(player);
    if (!actor && !game::player_captain(player)) return TSUCHI_NOT_FOUND;
    out->id = actor ? game::to_handle(actor) : 0; // a player without a dog has none
    return TSUCHI_OK;
}

TSUCHI_Result controlled(uint32_t player, TSUCHI_Actor* out) {
    uint64_t actor = 0;
    if (!game::player_controlled(player, actor)) return TSUCHI_NOT_READY;
    if (!actor) return TSUCHI_NOT_FOUND;
    out->id = game::to_handle(actor);
    return TSUCHI_OK;
}

TSUCHI_Result pad(uint32_t player, uint32_t* out) {
    if (player >= game::kPlayerSlots) return TSUCHI_INVALID;
    return game::player_pad(player, *out) ? TSUCHI_OK : TSUCHI_NOT_READY;
}

// True when the actor is the player's captain or their dog.
bool is_theirs(uint32_t player, uint64_t actor) { return actor == game::player_captain(player) || actor == game::player_dog(player); }

// Tells listeners of each player who steers another character than when last looked.
// The first look after nobody listened tells nothing: it is what later looks compare with.
void look() {
    if (!player::character_changed_wanted()) {
        seen.known = false;
        return;
    }
    for (uint32_t player = 0; player < game::kPlayerSlots; ++player) {
        uint64_t steered = 0;
        if (!game::player_controlled(player, steered)) continue;
        const uint64_t handle = steered ? game::to_handle(steered) : 0, before = seen.steered[player];
        seen.steered[player] = handle;
        if (seen.known && handle != before) player::emit_character_changed({player, {handle}, {before}});
    }
    seen.known = true;
}

TSUCHI_Result set_controlled(uint32_t player, TSUCHI_Actor actor, bool before_unlock) {
    const uint64_t address = actor_of(actor);
    if (!address || !game::player_captain(player)) return TSUCHI_NOT_FOUND;
    if (!is_theirs(player, address)) return TSUCHI_INVALID;
    look(); // the game switches at once, and a listener that began this frame is told of it
    return game::switch_controlled(player, address, false, before_unlock);
}

// ---- A player held on a character --------------------------------------------------------
//
// The hold is on the player, by number, and keeps which character it is.
struct Steered {
    uint64_t actor; // a handle
    uint32_t waited;
};

TSUCHI_Result steer(const Hold&, Steered& entry, const uint64_t& actor, bool) {
    entry = Steered{actor, 0};
    return TSUCHI_OK;
}

// Once a frame: the player's own switch is dropped, and one the game has made is asked back now and
// then. With the character gone, or no longer this player's, the hold is over.
bool keep_steered(const Hold& hold, Steered& entry) {
    const uint32_t player = uint32_t(hold.what);
    const uint64_t actor = game::from_handle(entry.actor);
    uint64_t steered = 0;
    if (!actor || !is_theirs(player, actor)) return false;
    game::drop_pending_switch(); // the player's own switch, fading in
    if (!game::player_controlled(player, steered) || steered == actor) {
        entry.waited = 0;
        return true;
    }
    // A hold keeps the player there whether or not the story has unlocked switching: the player was put there.
    if (++entry.waited % kSwitchBackFrames == 0 && !game::events_running()) game::switch_controlled(player, actor, true, true);
    return true;
}

// The game's switches stand once the hold ends, so there is nothing to restore.
Holds<Steered, uint64_t, steer, keep_steered, nullptr> characters{"player.controlled", HoldOn::nothing, true};

TSUCHI_Result hold_controlled(TSUCHI_Mod* mod, uint32_t player, TSUCHI_Actor actor) {
    const uint64_t address = actor_of(actor);
    uint64_t steered = 0;
    if (!address || !game::player_captain(player)) return TSUCHI_NOT_FOUND;
    if (!game::player_controlled(player, steered) || steered != address) return TSUCHI_NOT_READY;
    return characters.take(mod, 0, player, actor.id);
}

TSUCHI_Result release_controlled(TSUCHI_Mod* mod, uint32_t player) { return characters.release(mod, 0, player); }

// ---- A controller held on a player -------------------------------------------------------

bool is_pad(uint32_t pad) { return pad <= t::constant::pad_none || pad == t::constant::pad_handheld; }

// The hold is on the player, by number, and keeps the controller held and the one the player had.
struct Pad {
    uint32_t pad, before;
};

// A new hold remembers the player's own controller; a hold that moves keeps what it remembered.
TSUCHI_Result plug(const Hold& hold, Pad& entry, const uint32_t& pad, bool fresh) {
    const uint32_t player = uint32_t(hold.what);
    if (fresh && !game::player_pad(player, entry.before)) return TSUCHI_NOT_READY;
    entry.pad = pad;
    game::set_player_pad(player, pad);
    return TSUCHI_OK;
}

// Once a frame: a player the game has given its own controller again gets the held one back.
bool keep_plugged(const Hold& hold, Pad& entry) {
    uint32_t now = 0;
    if (game::player_pad(uint32_t(hold.what), now) && now != entry.pad) game::set_player_pad(uint32_t(hold.what), entry.pad);
    return true;
}

void unplug(const Hold& hold, Pad& entry, HoldEnd) { game::set_player_pad(uint32_t(hold.what), entry.before); }

Holds<Pad, uint32_t, plug, keep_plugged, unplug> pads{"player.pad", HoldOn::nothing, true};

TSUCHI_Result hold_pad(TSUCHI_Mod* mod, uint32_t player, uint32_t pad_number) {
    if (player >= game::kPlayerSlots || !is_pad(pad_number)) return TSUCHI_INVALID;
    uint32_t now = 0;
    if (!game::player_pad(player, now)) return TSUCHI_NOT_READY;
    return pads.take(mod, 0, player, pad_number);
}

TSUCHI_Result release_pad(TSUCHI_Mod* mod, uint32_t player) { return pads.release(mod, 0, player); }

// Once a frame: listeners are told who steers what.
void step() { look(); }

const player::Backend service{count, captain, dog, controlled, pad, set_controlled, hold_controlled, release_controlled, hold_pad, release_pad};

} // namespace

void players_start() {
    player::set_backend(&service);
    pads.start();
    characters.start();
    each_frame(step);
}

} // namespace tsuchi
