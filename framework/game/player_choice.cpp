// The answer to the game's Number of Players question, held for a mod (DESIGN.md, "The Number of
// Players question").
//
// The title flow waits in one state for that dialog and reads whether it was answered and the answer's
// name. While a mod holds the choice, every answer is taken out before the flow reads it and told to the
// mod; the flow keeps waiting until the mod answers, and then reads the mod's answer as the game's own.
#include "player_choice.h"
#include "game_service.h"
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

void update(void* flow, float seconds) {
    const uint64_t at = reinterpret_cast<uint64_t>(flow);
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
    game_service::set_backend(&backend);
}
} // namespace tsuchi
