// The answer to the game's Number of Players question, held for a mod (DESIGN.md, "The Number of
// Players question").
//
// The title flow waits in one state for that dialog and reads whether it was answered and the answer's
// name. While a mod holds the choice, every answer is taken out before the flow reads it and told to the
// mod; the flow keeps waiting until the mod answers, and then reads the mod's answer as the game's own.
#include "player_choice.h"
#include "game_service.h"
#include "flow.h"
#include "frame.h"
#include "game.h"
#include "hooks.h"
#include "mods.h"
#include "registry.h"

namespace tsuchi {
namespace {
namespace t = game::table;
using Update = decltype(t::slot::title_flow_update)::Function;

TSUCHI_Mod* holder = nullptr;
Update next_update = nullptr;
bool ready = false;
TSUCHI_PlayerChoice told = TSUCHI_PLAYER_CHOICE_ONE;
bool telling = false;      // an answer was taken and waits to be told
bool waiting = false;      // the flow waits for the mod's answer
TSUCHI_PlayerChoice decided = TSUCHI_PLAYER_CHOICE_ONE;
bool answering = false;    // the mod answered; written at the flow's next update
int32_t load_slot = -1;    // a slot a mod asked to load, started at the flow's next update in its menu
int32_t asked_slot = -1;   // a slot the title was just asked to load, told at the next frame
constexpr float kLoadFadeSeconds = 0.3f;   // how long the save list's own fade to the loading screen takes
bool asked = false;        // the title's answer this time was told already

uint64_t name_of(TSUCHI_PlayerChoice choice) {
    const uint64_t at = choice == TSUCHI_PLAYER_CHOICE_ONE ? t::data::dialog_answer_yes
        : choice == TSUCHI_PLAYER_CHOICE_TWO ? t::data::dialog_answer_no
        : choice == TSUCHI_PLAYER_CHOICE_THIRD ? t::data::dialog_answer_cancel : t::data::dialog_answer_return;
    return game::read_or<uint64_t>(game::main_base() + at);
}
TSUCHI_PlayerChoice choice_of(uint64_t name) {
    if (name && name == name_of(TSUCHI_PLAYER_CHOICE_ONE)) return TSUCHI_PLAYER_CHOICE_ONE;
    if (name && name == name_of(TSUCHI_PLAYER_CHOICE_TWO)) return TSUCHI_PLAYER_CHOICE_TWO;
    if (name && name == name_of(TSUCHI_PLAYER_CHOICE_THIRD)) return TSUCHI_PLAYER_CHOICE_THIRD;
    return TSUCHI_PLAYER_CHOICE_BACK;
}

// A save slot's file as the title screen found it: its path, an engine string, is empty for none.
bool slot_has_file(int32_t slot) {
    const uint64_t at = game::main_base() + t::data::save_slot_paths + uint64_t(slot) * 16;
    return game::read_or<uint64_t>(at) && game::read_or<int32_t>(at + 8) > 1;
}

uint64_t config_save() {
    const uint64_t instance = game::global<uint64_t>(t::data::game_instance);
    return instance ? game::read_or<uint64_t>(instance + t::game_instance::config_save) : 0;
}

// The title's own answer from its save list, as if the player had picked the slot and Load: the flow goes on to
// load it. The slot gets its difficulty first, so the game doesn't stop to ask for one, and the game plays it
// with one player unless another mode is already set up for it (two people on the dog's mode).
void start_load(uint64_t at) {
    const int32_t slot = load_slot;
    load_slot = -1;
    const uint64_t config = config_save();
    const uint64_t load = game::read_or<uint64_t>(game::main_base() + t::data::title_answer_load);
    if (!config || !load) return;
    const uint64_t set = game::read_or<uint64_t>(config + t::config_save::difficulty_set);
    if (set && slot < game::read_or<int32_t>(config + t::config_save::difficulty_set_count) && !game::read_or<uint8_t>(set + uint64_t(slot)) &&
        t::bound(t::fn::set_slot_difficulty)) {
        const uint64_t chosen = game::read_or<uint64_t>(config + t::config_save::difficulty);
        const uint8_t difficulty = chosen && slot < game::read_or<int32_t>(config + t::config_save::difficulty_count) ? game::read_or<uint8_t>(chosen + uint64_t(slot)) : 0;
        t::fn::set_slot_difficulty(reinterpret_cast<void*>(config), difficulty, slot);
    }
    const uint8_t mode = game::read_or<uint8_t>(config + t::config_save::play_mode);
    if (mode != uint8_t(t::constant::play_mode_one) && mode != uint8_t(t::constant::play_mode_two)) game::write(config + t::config_save::play_mode, uint8_t(t::constant::play_mode_one));
    // The loading screen, asked for the way the save list asks as the player picks Load: the flow waits for
    // that fade before it loads. Without it the level loaded behind a black picture with no loading screen
    // (seen 2026-11-12: 65 s of black on a laptop).
    const uint64_t instance = game::global<uint64_t>(t::data::game_instance);
    const uint64_t fade = instance && t::present(t::game_instance::fade) ? game::read_or<uint64_t>(instance + t::game_instance::fade) : 0;
    if (fade && t::bound(t::fn::fade_request))
        t::fn::fade_request(reinterpret_cast<void*>(fade), uint32_t(t::constant::fade_out_in_front), uint32_t(t::constant::fade_role_title), false, kLoadFadeSeconds);
    game::write<uint64_t>(at + t::title_flow::answer, load);
    game::write<int32_t>(at + t::title_flow::slot, slot);
    game::write<uint8_t>(at + t::title_flow::answered, 1);
    game::write<int32_t>(at + t::title_flow::state, int32_t(t::constant::title_flow_story));
}

// The title, in the state where it waits for the save list's answer, has Load for a slot: it loads that slot
// next. Told once for each such answer, whoever gave it (the player, or load_story).
void notice_load(uint64_t at) {
    const bool loading = game::read_or<int32_t>(at + t::title_flow::state, -1) == int32_t(t::constant::title_flow_story) &&
        game::read_or<uint8_t>(at + t::title_flow::answered) &&
        game::read_or<uint64_t>(at + t::title_flow::answer) == game::read_or<uint64_t>(game::main_base() + t::data::title_answer_load);
    if (loading && !asked) asked_slot = game::read_or<int32_t>(at + t::title_flow::slot, -1);
    asked = loading;
}

void update(void* flow, float seconds) {
    const uint64_t at = reinterpret_cast<uint64_t>(flow);
    if (on_game_thread() && ready && load_slot >= 0 && game::read_or<int32_t>(at + t::title_flow::state, -1) == int32_t(t::constant::title_flow_menu) &&
        !game::read_or<uint8_t>(at + t::title_flow::answered) && !game::busy_with_saves_or_scenes())
        start_load(at);
    if (on_game_thread() && ready) notice_load(at);
    if (on_game_thread() && ready && game::read_or<int32_t>(at + t::title_flow::state, -1) == int32_t(t::constant::title_flow_players_wait)) {
        const bool held = holder && mod_usable(holder);
        if (answering && waiting) {
            // The mod's answer, as the game's own.
            game::write<uint64_t>(at + t::title_flow::answer, name_of(decided));
            game::write<uint8_t>(at + t::title_flow::answered, 1);
            answering = waiting = false;
        } else if (held && game::read_or<uint8_t>(at + t::title_flow::answered)) {
            told = choice_of(game::read_or<uint64_t>(at + t::title_flow::answer));
            game::write<uint8_t>(at + t::title_flow::answered, 0);   // the flow keeps waiting
            telling = waiting = true;
        } else if (!held && waiting) {
            // The mod stopped or let go while the flow waited: back to the question.
            game::write<uint64_t>(at + t::title_flow::answer, name_of(TSUCHI_PLAYER_CHOICE_BACK));
            game::write<uint8_t>(at + t::title_flow::answered, 1);
            waiting = answering = false;
        }
    }
    next_update(flow, seconds);
}

void tell() {
    if (asked_slot >= 0) {
        const uint32_t slot = uint32_t(asked_slot);
        asked_slot = -1;
        if (game_service::story_load_requested_wanted()) game_service::emit_story_load_requested({slot});
    }
    if (holder && !mod_usable(holder)) holder = nullptr;
    if (!telling) return;
    telling = false;
    if (holder) game_service::emit_player_choice_to(holder, {told});
}

TSUCHI_Result hold(TSUCHI_Mod* mod) {
    if (!mod) return TSUCHI_INVALID;
    if (!ready) return TSUCHI_UNSUPPORTED;
    if (holder && holder != mod && mod_usable(holder)) return TSUCHI_BUSY;
    holder = mod;
    return TSUCHI_OK;
}
TSUCHI_Result release(TSUCHI_Mod* mod) {
    if (!mod || holder != mod) return TSUCHI_NOT_FOUND;
    holder = nullptr;   // a choice still waiting is answered Back at the flow's next update
    return TSUCHI_OK;
}
TSUCHI_Result answer(TSUCHI_Mod* mod, TSUCHI_PlayerChoice choice) {
    if (!mod || choice < TSUCHI_PLAYER_CHOICE_ONE || choice > TSUCHI_PLAYER_CHOICE_BACK) return TSUCHI_INVALID;
    if (holder != mod) return TSUCHI_NOT_FOUND;
    if (!waiting) return TSUCHI_NOT_READY;
    decided = choice; answering = true;
    return TSUCHI_OK;
}
TSUCHI_Result load_story(uint32_t slot) {
    if (!ready || !t::bound(t::fn::set_slot_difficulty)) return TSUCHI_UNSUPPORTED;
    const uint64_t config = config_save();
    if (!config) return TSUCHI_NOT_READY;
    if (slot >= uint32_t(game::read_or<int32_t>(config + t::config_save::difficulty_set_count)) || !slot_has_file(int32_t(slot))) return TSUCHI_INVALID;
    if (load_slot >= 0) return TSUCHI_BUSY;
    if (!game::at_title() || game::busy_with_saves_or_scenes()) return TSUCHI_NOT_READY;
    load_slot = int32_t(slot);
    return TSUCHI_OK;
}
} // namespace

void player_choice_start(TSUCHI_Mod* framework) {
    static bool listening = false;
    if (!listening) listening = each_frame(tell);
    uint64_t id = 0;
    ready = listening && hook_slot(framework, t::slot::title_flow_update, update, &next_update, kHookNeedsStock, &id) == TSUCHI_OK;
    auto& backend = game_service_backend();
    backend.hold_player_choice = hold;
    backend.release_player_choice = release;
    backend.answer_player_choice = answer;
    backend.load_story = load_story;
    game_service::set_backend(&backend);
}
} // namespace tsuchi
