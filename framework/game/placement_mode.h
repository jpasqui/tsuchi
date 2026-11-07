// The objects of a maps entry default to story; ground without a mode is shared.
#pragma once
#include <string.h>
#include "actors.h"
#include "content_path.h"
#include "flow.h"
#include "game.h"
#include "json.h"
#include "reflect.h"
#include "objects.h"
#include "text.h"

namespace tsuchi {
enum class PlacementMode : uint8_t { story, night, olimar, invalid };

inline PlacementMode placement_mode(const Json& entry) {
    const Json* mode = entry.find("mode");
    if (!mode) return PlacementMode::story;
    if (mode->kind != Json::Kind::text || strlen(mode->text) != mode->bytes) return PlacementMode::invalid;
    if (!strcmp(mode->text, "story")) return PlacementMode::story;
    if (!strcmp(mode->text, "night")) return PlacementMode::night;
    if (!strcmp(mode->text, "olimar")) return PlacementMode::olimar;
    return PlacementMode::invalid;
}
inline bool placement_mode_well_formed(const Json& entry, Text& wrong) {
    if (placement_mode(entry) != PlacementMode::invalid) return true;
    wrong.set("\"maps\": \"mode\" is \"story\", \"night\" or \"olimar\"; objects default to \"story\" and ground applies in every mode");
    return false;
}
inline bool placement_mode_matches(PlacementMode wanted) {
    namespace t = game::table;
    int32_t rule = 0;
    const uint64_t context = game::game_context(), framework = game::game_framework();
    if (!context || !framework || !game::game_rule(rule) || !t::bound(t::fn::story_flags)) return false;
    const uint64_t clock = game::read_or<uint64_t>(framework + t::game_framework::clock);
    uint8_t night = 0;
    if (!clock || !game::read(clock + t::clock::night, night)) return false;
    const bool olimar = (t::fn::story_flags(reinterpret_cast<void*>(context)) & t::constant::story_of_olimar) != 0;
    // Story caves and expeditions can use another of the game's story-store rules.
    bool story_rule = false;
    for (uint64_t known : t::tables::story_rules) story_rule = story_rule || rule == int32_t(known);
    if (!story_rule) return false;
    if (olimar) return !night && wanted == PlacementMode::olimar;
    return wanted == (night ? PlacementMode::night : PlacementMode::story);
}
inline bool placement_mode_matches(const Json& entry) { return placement_mode_matches(placement_mode(entry)); }

// The entry's objects still default to story. Ground without a mode is shared.
inline bool map_has_ground(const Json& entry) {
    return entry.find("levels") || entry.find("changes") || entry.find("navigation");
}
inline bool map_ground_matches(const Json& entry, PlacementMode current) {
    return current != PlacementMode::invalid && (!entry.find("mode") || placement_mode(entry) == current);
}
inline bool map_ground_overrides(const Json& shared, const Json& specific, PlacementMode current) {
    return !shared.find("mode") && specific.find("mode") && map_has_ground(specific) &&
           map_ground_matches(specific, current) &&
           content_path::same(shared.find("map")->text, specific.find("map")->text);
}
inline PlacementMode current_placement_mode() {
    const PlacementMode modes[] = {PlacementMode::story, PlacementMode::night, PlacementMode::olimar};
    for (PlacementMode mode : modes)
        if (placement_mode_matches(mode)) return mode;
    return PlacementMode::invalid;
}
// The story's mode from its own state: Olimar's tale, a night, or the day. A rule other than the daytime story's
// has no night. Invalid when that state cannot be read.
inline PlacementMode story_mode_from_state(bool night_possible) {
    namespace t = game::table;
    const uint64_t context = game::game_context();
    if (!context || !t::bound(t::fn::story_flags)) return PlacementMode::invalid;
    if (t::fn::story_flags(reinterpret_cast<void*>(context)) & t::constant::story_of_olimar) return PlacementMode::olimar;
    if (!night_possible) return PlacementMode::story;
    const uint64_t instance = game::global<uint64_t>(t::data::game_instance);
    const uint64_t story = game::read_or<uint64_t>(instance + t::game_instance::story_save);
    uint8_t night = 0, starting_night = 0;
    if (!game::read(story + t::story_save::night, night) || !game::read(context + t::game_context::night_start, starting_night)) return PlacementMode::invalid;
    return night || starting_night ? PlacementMode::night : PlacementMode::story;
}
// The clock is created during BeginPlay. Before that, read the serialized rule
// and the two night inputs that GameClock_Setup will use after arrival.
inline PlacementMode map_mode_before_play(uint64_t world) {
    namespace t = game::table;
    static game::ClassRef rule_class{t::name::map_rule_class};
    const uint64_t persistent = game::read_or<uint64_t>(world + t::world::persistent_level);
    const uint64_t levels = game::read_or<uint64_t>(world + t::world::levels + t::array::data);
    int32_t level_count = game::read_or<int32_t>(world + t::world::levels + t::array::count, -1);
    if (level_count < 0 || level_count > 4096 || (level_count && !game::readable(levels, uint64_t(level_count) * 8))) level_count = 0;
    bool playing = false;
    int64_t rule = -1;
    // A game's rule may live in one of the initial sublevels. Check the world
    // levels as well as the persistent level without scanning all objects.
    for (int32_t l = 0; l <= level_count; ++l) {
        const uint64_t level = l == level_count ? persistent : game::read_or<uint64_t>(levels + uint64_t(l) * 8);
        if (!game::live(level) || game::read_or<uint64_t>(level + t::engine_level::owning_world) != world) continue;
        const uint64_t actors = game::read_or<uint64_t>(level + t::engine_level::actors + t::array::data);
        const int32_t count = game::read_or<int32_t>(level + t::engine_level::actors + t::array::count, -1);
        if (count < 0 || uint32_t(count) > t::constant::max_objects || (count && !game::readable(actors, uint64_t(count) * 8))) continue;
        for (int32_t i = 0; i < count; ++i) {
            const uint64_t actor = game::read_or<uint64_t>(actors + uint64_t(i) * 8);
            playing = playing || (actor && (game::read_or<uint8_t>(actor + t::actor_play::state) & t::constant::actor_play_state_mask));
            if (!game::is_a(actor, rule_class)) continue;
            game::Place place;
            game::Value value;
            if (game::find_place(game::type_of(actor), actor, "GameRule", place) == TSUCHI_OK &&
                game::read_value(place, value) == TSUCHI_OK && value.kind == game::ValueKind::integer) rule = value.integer;
        }
    }
    if (playing && game::loaded_world() == world) {
        const PlacementMode mode = current_placement_mode();
        if (mode != PlacementMode::invalid) return mode;
    }
    bool story_rule = false;
    for (uint64_t known : t::tables::story_rules) story_rule = story_rule || rule == int32_t(known);
    if (!story_rule) return PlacementMode::invalid;
    return story_mode_from_state(rule == int64_t(t::constant::rule_story));
}

} // namespace tsuchi