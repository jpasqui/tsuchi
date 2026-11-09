// Typed text through the system's software keyboard.
#pragma once
#include "tsuchi/core.h"

namespace tsuchi {
void ui_keyboard_start();
// The framework's own request, which no mod's text_entered tells: the text comes back to `done`, on the game thread
// at the start of a later frame, with TSUCHI_OK when the player accepted it. `most` is held to 1 to 256 characters,
// and a prompt longer than 256 bytes is cut. TSUCHI_BUSY while a keyboard is waiting or open; `done` is not called
// when the request is refused.
using UiKeyboardDone = void (*)(uint64_t request, TSUCHI_Result result, const char* text);
TSUCHI_Result ui_keyboard_ask(const char* prompt, const char* initial, uint32_t most, bool number, uint64_t request, UiKeyboardDone done);
}
