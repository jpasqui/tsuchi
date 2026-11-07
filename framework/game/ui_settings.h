// Declared settings on the shared screen renderer.
#pragma once
#include "mods.h"
#include "tsuchi/ui.h"
namespace tsuchi {
using UiSettingsTextDone = void (*)(uint64_t request, TSUCHI_Result result, const char* value);
// Copy the input strings before returning. Complete once on the game thread.
// A cancelled or failed edit completes with a result other than OK.
using UiSettingsTextProvider = TSUCHI_Result (*)(const char* title, const char* current,
    uint32_t max_length, uint64_t request, UiSettingsTextDone done);
void ui_settings_text_provider(UiSettingsTextProvider provider);
void ui_settings_start(TSUCHI_Mod* framework);
TSUCHI_Result ui_mod_settings(ModRecord& record, TSUCHI_Screen* out);
} // namespace tsuchi
