// A mod's own navigation put in the place of the game's, for the map that is loaded.
#pragma once
#include <stdint.h>

namespace tsuchi {

enum class NavigationResult : uint32_t {
    done,
    waiting,        // a named navmesh is not ready yet: ask again at the next step
    other_grid,     // the file was made for a navmesh that begins elsewhere or has tiles of another width
    too_many_tiles, // more than the game's navmesh has room for
    no_memory,
    tile_refused,   // the navmesh would not take one of the tiles
};
struct NavigationCounts {
    uint32_t navmeshes; // swapped whole
    uint32_t removed, added, refused; // tiles
    uint32_t picked, marked; // parts that mark polygons: those that picked theirs again, and those whose kind was marked again
};

struct NavigationState {
    void* parsed = nullptr;
};

// Parses and checks the whole file once, retaining the named map's navmeshes. The file's bytes must stay alive
// until release_navigation. False with well_formed says the map is absent, or (when parsed is null) no memory.
bool prepare_navigation(NavigationState& state, const uint8_t* file, uint64_t bytes, const char* map_path, bool& well_formed);
// Clear completed object lives when leaving a world, while keeping the parsed file for the next visit.
void reset_navigation(NavigationState& state);
void release_navigation(NavigationState& state);

// Finds all named actors in one walk and swaps only navmeshes not completed in this world. Waiting while any
// actor or its implementation is missing, even when others have finished. Counts are added to counts.
NavigationResult swap_navigation(NavigationState& state, uint64_t world, NavigationCounts& counts);

} // namespace tsuchi
