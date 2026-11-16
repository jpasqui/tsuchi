// The hud service's action prompts: which players' screens show the game's prompts that point at an actor, and holds
// that hide them. The prompts themselves are kept by the presenter's markers, at the game's placement of each one
// (presenter_markers.h); here are the calls and the holds.
#include "hud_prompts.h"

#include "game.h"
#include "holds.h"
#include "objects.h"
#include "presenter_markers.h"

namespace tsuchi {
namespace {

namespace t = game::table;

TSUCHI_Mod* framework = nullptr;
constexpr uint32_t kEveryScreen = (1u << uint32_t(t::constant::player_slots)) - 1;

struct HideKept { uint64_t key; };
struct HideWanted { uint64_t actor; uint32_t players; };
TSUCHI_Result apply_hide(const Hold& hold, HideKept& kept, const HideWanted& wanted, bool) {
    if (!markers_watch_prompts(framework)) return TSUCHI_NOT_READY;
    if (!markers_hide_prompts(hold.id, wanted.actor, wanted.players)) return TSUCHI_CAPACITY;
    kept.key = hold.id;
    return TSUCHI_OK;
}
void restore_hide(const Hold&, HideKept& kept, HoldEnd) { markers_show_prompts(kept.key); }
// Several mods may hide the same actor's prompts: each hold is its own.
Holds<HideKept, HideWanted, apply_hide, nullptr, restore_hide> hidden{"hud.hidden_prompts", HoldOn::actor, false};

} // namespace

void hud_prompts_start(TSUCHI_Mod* self) {
    framework = self;
    hidden.start();
}

TSUCHI_Result hud_prompts_shown(TSUCHI_Actor actor, uint32_t* out) {
    if (!out) return TSUCHI_INVALID;
    const uint64_t object = game::from_handle(actor.id);
    if (!object) return TSUCHI_NOT_FOUND;
    const int watched = markers_watch_prompts(framework);
    if (watched != 1) return TSUCHI_NOT_READY;
    *out = markers_prompt_screens(object);
    return TSUCHI_OK;
}

TSUCHI_Result hud_hold_hidden_prompts(TSUCHI_Mod* mod, TSUCHI_Actor actor, uint32_t players) {
    if (!players || (players & ~kEveryScreen)) return TSUCHI_INVALID;
    const uint64_t object = game::from_handle(actor.id);
    if (!object) return TSUCHI_NOT_FOUND;
    return hidden.take(mod, actor.id, 0, {object, players});
}

TSUCHI_Result hud_release_hidden_prompts(TSUCHI_Mod* mod, TSUCHI_Actor actor) {
    return hidden.release(mod, actor.id, 0);
}

} // namespace tsuchi
