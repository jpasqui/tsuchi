// The Mods page on its own, over the title screen's menu (ui_mod_page.cpp).
#pragma once
#include "tsuchi/core.h"

namespace tsuchi {
void ui_mod_page_start(TSUCHI_Mod* framework);
// Opens the game's Settings page over the title menu. The list of mods shows in it alone, without the
// page's tabs, and Back closes it and returns to the title menu.
TSUCHI_Result ui_mod_page_open();
// From that call until the game's menu says the page has closed.
bool ui_mod_page_alone();
// Whether this build of the game has what the call needs, once it has started.
bool ui_mod_page_supported();
}
