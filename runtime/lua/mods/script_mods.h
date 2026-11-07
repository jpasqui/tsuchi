// Mods written in Lua. A mod whose folder holds a `main.lua` is one: the file is run in a state of its own when
// the mod starts, with every service as a table, and the state is closed when the mod stops.
#pragma once

namespace tsuchi_lua {

// Tells the framework about this kind of mod. Called once when the framework starts.
void register_script_mods();

} // namespace tsuchi_lua
