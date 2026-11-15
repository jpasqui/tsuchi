// The hud service's action prompts (schema/hud.yaml: prompts_shown, hold_hidden_prompts, release_hidden_prompts).
#pragma once
#include "registry.h"
#include "tsuchi/hud.h"

namespace tsuchi {
// `self` is the framework's own mod, which the prompts' hooks are put on for.
void hud_prompts_start(TSUCHI_Mod* self);
TSUCHI_Result hud_prompts_shown(TSUCHI_Actor actor, uint32_t* out);
TSUCHI_Result hud_hold_hidden_prompts(TSUCHI_Mod* mod, TSUCHI_Actor actor, uint32_t players);
TSUCHI_Result hud_release_hidden_prompts(TSUCHI_Mod* mod, TSUCHI_Actor actor);
} // namespace tsuchi
