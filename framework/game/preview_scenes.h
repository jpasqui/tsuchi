// Owned temporary area reservations. Native scene references outlive their owner.
#pragma once
#include "tsuchi/game.h"

namespace tsuchi {
TSUCHI_Result reserve_game_preview(TSUCHI_Mod*, TSUCHI_PreviewScene*);
TSUCHI_Result game_preview_scene(TSUCHI_Mod*, TSUCHI_PreviewScene, TSUCHI_PreviewSceneInfo*);
TSUCHI_Result admit_game_preview(TSUCHI_Mod*, TSUCHI_PreviewScene, TSUCHI_AssetBundle, const char* world, const char* scene);
TSUCHI_Result prepare_game_preview(TSUCHI_Mod*, TSUCHI_PreviewScene);
TSUCHI_Result release_game_preview(TSUCHI_Mod*, TSUCHI_PreviewScene);
}
