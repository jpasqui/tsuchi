// The answer to the game's Number of Players question, held for a mod (DESIGN.md, "The Number of
// Players question").
#pragma once
#include "tsuchi/core.h"
#include "game_service.h"

namespace tsuchi {
void player_choice_start(TSUCHI_Mod* framework);
// The game service's calls, which this file adds to.
game_service::Backend& game_service_backend();
}
