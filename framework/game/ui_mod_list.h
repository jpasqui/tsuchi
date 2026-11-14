// The player frontend over the original Settings scrolling rows.
#pragma once
#include "registry.h"
namespace tsuchi {
void ui_mod_list_start(TSUCHI_Mod* framework);
// A mod holds the menus' input for a page of its own, or has just let go of it with a button still down.
bool ui_menu_input_held();
}
