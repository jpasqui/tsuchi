// The game service: where the game is, pauses a mod holds, and the random stream.
//
// The game does not say when it moves from one stage to another or loads a
// world, so while a mod is listening it is looked at once a frame and compared
// with the frame before.
#include <string.h>

#include "flow.h"
#include "frame.h"
#include "game.h"
#include "game_service.h"
#include "holds.h"
#include "names.h"
#include "new_maps.h"
#include "preview_scenes.h"
#include "services.h"
#include "text.h"

namespace tsuchi {
namespace {

namespace t = game::table;

uint32_t pause_kinds() { return uint32_t(t::constant::pause_mod_kinds); } // the kinds a mod may hold; the bits above are the game's own reasons

// The game's own names for the outermost stage of each mode, and for the stages in which a world is played.
struct ModeName {
    const char* stage;
    TSUCHI_Mode mode;
};
constexpr ModeName kModes[] = {{game::kTitleStage, TSUCHI_MODE_TITLE}, {t::name::stage_story, TSUCHI_MODE_STORY}, {t::name::stage_hero, TSUCHI_MODE_OLIMAR},
                               {t::name::stage_challenge, TSUCHI_MODE_CHALLENGE}, {t::name::stage_battle, TSUCHI_MODE_BATTLE}};
constexpr const char* kPlayStages[] = {t::name::stage_story_play, t::name::stage_hero_play, t::name::stage_battle_play,
                                      t::name::stage_special_play, t::name::stage_extra_play};

TSUCHI_Mode mode_now() {
    game::Name top;
    char name[game::kNameBytes];
    if (!game::top_stage(top) || !game::name_text(top, name, sizeof name)) return TSUCHI_MODE_NONE;
    for (const ModeName& each : kModes)
        if (!strcmp(name, each.stage)) return each.mode;
    return TSUCHI_MODE_OTHER;
}

uint32_t paused_now() {
    uint32_t kinds = 0;
    return game::paused_kinds(kinds) ? kinds & pause_kinds() : 0;
}

bool control_now() {
    game::Name leaf;
    char name[game::kNameBytes];
    if (!game::world_begun() || (paused_now() & t::constant::pause_stops_control) || !game::leaf_stage(leaf) || !game::name_text(leaf, name, sizeof name)) return false;
    for (const char* each : kPlayStages)
        if (!strcmp(name, each)) return true;
    return false;
}

// The loaded world's name in `out`, or "" when none has begun. False when there is no memory for it.
bool world_now(Text& out) {
    game::Name name;
    char text[game::kNameBytes] = "", id[kIdBytes];
    if (game::world_name(name) && !game::name_text(name, text, sizeof text)) text[0] = 0;
    return out.set(new_map_id(text, id) ? id : text); // a mod's map is told by its ID, not by the level of the area it was given
}

// The stages in `out`. False when they cannot be read or there is no memory.
bool stages_now(Text& out) {
    uint64_t needed = 0;
    if (game::stage_path(nullptr, 0, &needed) != TSUCHI_CAPACITY || !out.reserve(size_t(needed))) return false;
    return game::stage_path(out.data, out.capacity, &needed) == TSUCHI_OK;
}

TSUCHI_Result mode(TSUCHI_Mode* out) {
    *out = mode_now();
    return TSUCHI_OK;
}

TSUCHI_Result rule(int32_t* out) { return game::game_rule(*out) ? TSUCHI_OK : TSUCHI_NOT_READY; }

TSUCHI_Result stage(char* buffer, uint64_t capacity, uint64_t* out) { return game::stage_path(buffer, capacity, out); }

TSUCHI_Result world(char* buffer, uint64_t capacity, uint64_t* out) {
    game::Name name;
    char level[game::kNameBytes], id[kIdBytes];
    if (game::world_name(name)) {
        if (!game::name_text(name, level, sizeof level) || !new_map_id(level, id)) return game::name_into(name, buffer, capacity, out);
        *out = strlen(id) + 1;
        if (!buffer || capacity < *out) return TSUCHI_CAPACITY;
        memcpy(buffer, id, size_t(*out));
        return TSUCHI_OK;
    }
    *out = 1;
    if (!buffer || !capacity) return TSUCHI_CAPACITY;
    buffer[0] = 0;
    return TSUCHI_OK;
}

TSUCHI_Result paused(TSUCHI_Pause* out) {
    uint32_t kinds = 0;
    if (!game::paused_kinds(kinds)) return TSUCHI_NOT_READY;
    *out = kinds & pause_kinds();
    return TSUCHI_OK;
}

TSUCHI_Result has_control(bool* out) {
    *out = control_now();
    return TSUCHI_OK;
}

// ---- Pauses a mod holds ------------------------------------------------------------

// The game's pause manager counts each kind, so a hold adds one to the count of each of its kinds and
// takes it off again when it ends. Each hold is a thing of its own, named by the number its mod is given.
TSUCHI_Result pause(const Hold&, uint32_t& kinds, const uint32_t& wanted, bool) {
    kinds = wanted;
    game::change_pause(wanted, true);
    return TSUCHI_OK;
}

void unpause(const Hold&, uint32_t& kinds, HoldEnd) { game::change_pause(kinds, false); }

Holds<uint32_t, uint32_t, pause, nullptr, unpause> pauses{"game.pause", HoldOn::nothing, true};
uint64_t next_hold = 1;

TSUCHI_Result hold_pause(TSUCHI_Mod* mod, TSUCHI_Pause kinds, TSUCHI_PauseHold* out) {
    if (!kinds) return TSUCHI_INVALID;
    if (!game::pause_manager() || !t::bound(t::fn::pause_change)) return TSUCHI_NOT_READY;
    const uint64_t id = next_hold++;
    const TSUCHI_Result held = pauses.take(mod, 0, id, kinds);
    if (held == TSUCHI_OK) out->id = id;
    return held;
}

TSUCHI_Result release_pause(TSUCHI_Mod* mod, TSUCHI_PauseHold hold) { return pauses.release(mod, 0, hold.id); }

TSUCHI_Result return_to_title() {
    int32_t now = 0;
    if (!game::game_rule(now) || now != int32_t(t::constant::rule_story) || !t::bound(t::fn::return_to_title)) return TSUCHI_NOT_READY;
    if (game::busy_with_saves_or_scenes()) return TSUCHI_BUSY;
    t::fn::return_to_title(true);
    return TSUCHI_OK;
}

TSUCHI_Result get_random(TSUCHI_RandomState* out) {
    uint32_t words[4];
    for (int i = 0; i < 4; ++i) words[i] = game::global<uint32_t>(t::data::random_stream + 4 * i);
    *out = TSUCHI_RandomState{words[0], words[1], words[2], words[3]};
    return TSUCHI_OK;
}

TSUCHI_Result set_random(const TSUCHI_RandomState* state) {
    if (!(state->a | state->b | state->c | state->d)) return TSUCHI_INVALID;
    const uint32_t words[4] = {state->a, state->b, state->c, state->d};
    for (int i = 0; i < 4; ++i) game::write(game::main_base() + t::data::random_stream + 4 * i, words[i]);
    return TSUCHI_OK;
}

// ---- Telling mods what changed ------------------------------------------------------

struct Seen {
    bool known = false; // a first look has been taken
    TSUCHI_Mode mode = TSUCHI_MODE_NONE;
    uint64_t stages = 0;
    Text stage_text, world;
    bool in_world = false, control = false;
    uint32_t paused = 0;
} seen;
Text scratch;

bool anyone_listening() {
    return game_service::mode_changed_wanted() || game_service::stage_changed_wanted() || game_service::world_loaded_wanted() || game_service::world_unloaded_wanted() ||
           game_service::pause_changed_wanted() || game_service::control_changed_wanted();
}

// Once a frame, while anyone is listening.
void step() {
    if (!anyone_listening()) {
        seen.known = false; // a later listener starts from where the game is then
        return;
    }
    const TSUCHI_Mode mode_is = mode_now();
    const uint32_t paused_is = paused_now();
    const bool control_is = control_now(), in_world_is = game::world_begun();
    uint64_t stages_is = 0;
    game::stage_signature(stages_is);
    if (!seen.known) {
        if (!stages_now(seen.stage_text) || !world_now(seen.world)) return;
        seen.known = true, seen.mode = mode_is, seen.stages = stages_is, seen.in_world = in_world_is, seen.control = control_is, seen.paused = paused_is;
        return;
    }
    // In the order things happen when the game moves on: the old world goes, the mode and the
    // stages change, the new world comes, and then what is paused and who has control.
    const bool world_changed = [&] {
        if (!world_now(scratch)) return false;
        return in_world_is != seen.in_world || strcmp(scratch.get(), seen.world.get()) != 0;
    }();
    if (world_changed && seen.in_world) game_service::emit_world_unloaded({seen.world.get()});
    if (mode_is != seen.mode) {
        const TSUCHI_Mode before = seen.mode;
        seen.mode = mode_is;
        game_service::emit_mode_changed({mode_is, before});
    }
    if (stages_is != seen.stages && stages_now(scratch)) {
        seen.stages = stages_is;
        game_service::emit_stage_changed({scratch.get(), seen.stage_text.get()});
        stages_now(seen.stage_text);
    }
    if (world_changed && world_now(seen.world)) {
        seen.in_world = in_world_is;
        if (in_world_is) game_service::emit_world_loaded({seen.world.get()});
    }
    if (paused_is != seen.paused) {
        const uint32_t before = seen.paused;
        seen.paused = paused_is;
        game_service::emit_pause_changed({paused_is, before});
    }
    if (control_is != seen.control) {
        seen.control = control_is;
        game_service::emit_control_changed({control_is});
    }
}

game_service::Backend service{mode, rule, stage, world, paused, has_control, hold_pause, release_pause, return_to_title, get_random, set_random,
                              reserve_game_preview, game_preview_scene, admit_game_preview, prepare_game_preview, release_game_preview,
                              nullptr, nullptr, nullptr};

} // namespace

game_service::Backend& game_service_backend() { return service; }

void game_start() {
    game_service::set_backend(&service);
    pauses.start();
    each_frame(step);
}

unsigned game_pause_holds() { return pauses.count(); }

} // namespace tsuchi
