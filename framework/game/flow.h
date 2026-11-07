// Where the game is: the rule it is playing, the stages it is in, the world that
// is loaded, and what is paused.
#pragma once
#include <stdint.h>

#include "game_table.h"
#include "names.h"
#include "tsuchi/core.h"

namespace tsuchi::game {

// What the game keeps from one level to the next, or 0 before it exists.
uint64_t game_context();
// The number of the rule being played. False before the game has a context.
bool game_rule(int32_t& rule);

// The name of the outermost stage. False when the game is in none.
bool top_stage(Name& name);
// The name of the innermost stage. False when the game is in none.
bool leaf_stage(Name& name);
// The game's own name for the outermost stage of its title screen.
inline constexpr const char* kTitleStage = table::name::stage_title;
// True while the game is at its title screen, where it holds no story.
bool at_title();
// A number that is the same while the game stays in the same stages, none included. False when they cannot be read through.
bool stage_signature(uint64_t& signature);
// The stages from the outermost in, joined by '/', as UTF-8 with a terminator;
// empty when the game is in none. `needed` gets the bytes it takes;
// TSUCHI_CAPACITY when that is more than `capacity`, and nothing is written;
// TSUCHI_NOT_READY when the stages cannot be read through.
TSUCHI_Result stage_path(char* buffer, uint64_t capacity, uint64_t* needed);

// True once the loaded world has had its first update.
bool world_begun();
// Null when world_begun() is true. Otherwise what stands in its way, as a line for the log.
const char* world_not_begun();
// The loaded world, or 0 while none has begun.
uint64_t loaded_world();
// The loaded world's name. False while none has begun.
bool world_name(Name& name);

// The pause manager, or 0 before the game has one.
uint64_t pause_manager();
// The kinds of thing paused now, one bit each. False without a pause manager.
bool paused_kinds(uint32_t& kinds);
// Adds one count to each kind given, or takes one away. False without a pause manager. The game's function for it has to be known.
bool change_pause(uint32_t kinds, bool add);

// True while the game is reading or writing a save, or moving to another scene.
bool busy_with_saves_or_scenes();

} // namespace tsuchi::game
