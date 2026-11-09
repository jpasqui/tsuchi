#pragma once
#include "registry.h"
namespace tsuchi {
// Called from early ui_start, before input_start registers its final commit.
// Returns false when the optional input service is absent or registration fails.
bool ui_native_prepare_input();
bool ui_native_start(TSUCHI_Mod* framework);
// Includes a form still inside open, before the core has captured its view.
void ui_native_host_detach(uint64_t host);
// The optional input service supplies post-latch buttons. Without one,
// the UI chains the game's own npad read slot after the presenter.
// Register before input_start commits edge history. Npad still chains stock,
// while this consumes the provider once after current-step mod injections.
bool ui_input_steps(bool (*register_step)(void (*)()));
void ui_buttons_provider(bool (*read)(uint32_t player, uint64_t& buttons));
// Internal Mods frontend. Return true after recognizing its host. handled
// says whether stock dispatch is suppressed, and result is its return value.
void ui_menu_dispatcher(bool (*dispatch)(uint64_t host, uint32_t action, bool& handled, bool& result));
bool ui_menu_action(uint64_t host, uint32_t action);
void ui_button_listener(void (*listener)(uint64_t buttons));
// For a mod's own page over the menus: hides every button of the first player's controller from the game, or
// shows them again. False when there is no input service or another mod holds that controller's input.
bool ui_native_hide_buttons(bool hide);
// The buttons that are physically down on it, hidden ones included. False when that is not known.
bool ui_native_buttons_down(uint64_t& held);
}
