// Shared game-thread retention for internal renderer and asset holds. Handles.
#pragma once
#include <stdint.h>
namespace tsuchi {
// Zero needs no retention. Only roots added by the first holder are released.
bool retain_game_object(uint64_t handle);
void release_game_object(uint64_t handle);
}
