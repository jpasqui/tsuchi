// Compiled mods. A mod whose folder holds `mod.nro` has code of its own, built
// with the SDK. The framework hands the file to the loader of the system the game
// runs on when the mod first starts, and calls its `tsuchi_mod_start`.
//
// The code is loaded once and stays loaded for as long as the game runs. The
// system takes a module back only by going through every other module that is
// loaded, and it stops the game at one that was built with nothing to bind when
// it loads, which is how the framework and its mods are built. So a mod that
// stops is undone like any other, and its code stays where it is, with nothing
// left that calls it. If the mod starts again, the same code is started again,
// with its global variables as they were left.
#pragma once
#include <stdint.h>

namespace tsuchi {

// The file a compiled mod starts from, and the function of it the framework calls.
extern const char* const kCompiledEntry;
extern const char* const kCompiledStart;
// The name of the mark the SDK gives a compiled mod: the version of the headers it was built against.
extern const char* const kCompiledBuiltFor;

// Adds compiled mods as a kind of mod. False when there is no memory.
bool register_compiled_mods();

struct CompiledCounts {
    uint32_t loaded;  // how many mods' code has been loaded
    uint32_t started; // how many times a mod's start has been called
    uint32_t refused; // what the system's loader last answered when it refused something, or 0
};
const CompiledCounts& compiled_counts();

} // namespace tsuchi
