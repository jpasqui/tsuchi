#include "people.h"

#include "actors.h"
#include "flow.h"
#include "game.h"
#include "objects.h"

namespace tsuchi::game {
namespace {

namespace t = table;

uint64_t player_block() {
    const uint64_t context = game_context();
    return context ? context + t::game_context::players : 0;
}

// The first actor in one of the game's two lists that `wanted` takes.
template <typename Wanted>
uint64_t listed(uint64_t list_offset, Wanted&& wanted) {
    const uint64_t block = player_block();
    if (!block) return 0;
    const uint64_t data = read_or<uint64_t>(block + list_offset + t::array::data);
    const int32_t count = read_or<int32_t>(block + list_offset + t::array::count);
    if (!data || count <= 0 || !readable(data, 8 * uint64_t(count))) return 0;
    for (int32_t i = 0; i < count; ++i) {
        const uint64_t actor = read_or<uint64_t>(data + 8 * uint64_t(i));
        if (actor && is_game_actor(actor) && wanted(actor)) return actor;
    }
    return 0;
}

uint64_t controller_manager() {
    const uint64_t instance = global<uint64_t>(t::data::game_instance);
    return instance ? read_or<uint64_t>(instance + t::game_instance::controllers) : 0;
}

// True when the game's own rules let a player switch to `actor` now.
bool may_switch(uint64_t captain, uint64_t actor) {
    const uint64_t instance = global<uint64_t>(t::data::game_instance);
    const uint64_t config = instance ? read_or<uint64_t>(instance + t::game_instance::config_save) : 0;
    const uint64_t story = instance ? read_or<uint64_t>(instance + t::game_instance::story_save) : 0;
    if (!config || read_or<uint8_t>(config + t::config_save::play_mode) == t::constant::play_mode_two) return false;
    if (!story || !read_or<uint8_t>(story + t::story_save::switch_unlocked)) return false;
    return !read_or<uint8_t>(actor + t::game_actor::removed) && !is_riding(captain);
}

} // namespace

// In a battle a player's team is their number. In the story there are no sides, and the
// one player's captain is on the team that stands for none.
uint64_t player_captain(uint32_t player) {
    if (player >= kPlayerSlots) return 0;
    const uint64_t sided = listed(t::player_block::captains, [player](uint64_t actor) { return actor_team(actor) == int32_t(player); });
    if (sided || player != 0) return sided;
    return listed(t::player_block::captains, [](uint64_t actor) { return actor_team(actor) >= int32_t(t::constant::team_story); });
}

// The game pairs a dog with a captain by team, and so does this: its list of dogs holds
// other teams' too.
uint64_t player_dog(uint32_t player) {
    const uint64_t captain = player_captain(player);
    if (!captain) return 0;
    const int32_t team = actor_team(captain);
    return listed(t::player_block::dogs, [team](uint64_t actor) { return actor_team(actor) == team; });
}

bool player_controlled(uint32_t player, uint64_t& actor) {
    actor = 0;
    const uint64_t block = player_block(), captain = player_captain(player);
    if (!block || !t::bound(t::fn::controlled_actor)) return false;
    if (!captain) return true;
    // The game asks by team, which in the story is the captain's own and not the player's number.
    const auto steered = reinterpret_cast<uint64_t>(t::fn::controlled_actor(reinterpret_cast<void*>(block), actor_team(captain)));
    actor = steered && is_game_actor(steered) ? steered : 0;
    return true;
}

bool is_riding(uint64_t captain) {
    const uint64_t stick = read_or<uint64_t>(captain + t::game_actor::stick), steering = read_or<uint64_t>(captain + t::game_actor::player);
    const uint64_t held = stick ? read_or<uint64_t>(stick + t::stick_component::target) : 0;
    return held && steering && held == read_or<uint64_t>(steering + t::player_component::ridden_dog);
}

TSUCHI_Result switch_controlled(uint32_t player, uint64_t actor, bool at_once) {
    const uint64_t block = player_block(), captain = player_captain(player);
    uint64_t now = 0;
    if (!block || !captain || !t::bound(t::fn::request_control_switch) || !player_controlled(player, now)) return TSUCHI_NOT_READY;
    if (now == actor) return TSUCHI_OK;
    if (read_or<uint8_t>(block + t::player_block::switch_pending)) return TSUCHI_BUSY;
    if (!may_switch(captain, actor)) return TSUCHI_NOT_READY;
    return t::fn::request_control_switch(reinterpret_cast<void*>(block), actor_team(captain), false, at_once) ? TSUCHI_OK : TSUCHI_NOT_READY;
}

void drop_pending_switch() {
    const uint64_t block = player_block();
    if (block && read_or<uint8_t>(block + t::player_block::switch_pending)) write(block + t::player_block::switch_pending, uint8_t(0));
}

bool events_running() {
    const uint64_t framework = game_framework();
    const uint64_t events = framework ? read_or<uint64_t>(framework + t::game_framework::events) : 0;
    return events && (read_or<uint8_t>(events + t::event_manager::busy) || read_or<uint8_t>(events + t::event_manager::busy_too));
}

bool player_pad(uint32_t player, uint32_t& pad) {
    const uint64_t manager = controller_manager();
    return manager && player < kPlayerSlots &&
           read(manager + t::controller_manager::slots + uint64_t(player) * t::controller_slot::size + t::controller_slot::pad, pad);
}

// The caller has checked the player's number.
void set_player_pad(uint32_t player, uint32_t pad) {
    const uint64_t manager = controller_manager();
    if (!manager) return;
    // The game keeps two sets of slots, and reads both.
    const uint64_t sets[] = {t::controller_manager::slots, t::controller_manager::slots_too};
    for (uint64_t set : sets) write(manager + set + uint64_t(player) * t::controller_slot::size + t::controller_slot::pad, pad);
}

} // namespace tsuchi::game
