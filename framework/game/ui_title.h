// Mod entries in the game's title menu (DESIGN.md, "Menu entries and typed text").
#pragma once
#include "tsuchi/core.h"

namespace tsuchi {
void ui_title_start(TSUCHI_Mod* framework);
// The title widget the menu is in, while its menu takes input; 0 otherwise.
uint64_t ui_title_view();
bool ui_title_menu_ready();
// While `covered` says so, the title menu acts on no press: a page of the game's own is over it.
void ui_title_cover(bool (*covered)());
void ui_title_host_start();
// The title's Options menu (ui_title_options.cpp).
void ui_title_options_start(TSUCHI_Mod* framework);
TSUCHI_Result ui_options_hold_entry(TSUCHI_Mod* mod, const char* id, const char* text);
TSUCHI_Result ui_options_release_entry(TSUCHI_Mod* mod, const char* id);
// The note beside the title menu (ui_title_host.cpp), made again whenever the title widget is.
void ui_title_note_tick(uint64_t title_widget, bool menu_shown);
TSUCHI_Result ui_title_hold_note(TSUCHI_Mod* mod, const char* text);
TSUCHI_Result ui_title_release_note(TSUCHI_Mod* mod);
}
