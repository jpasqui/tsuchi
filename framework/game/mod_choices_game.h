// The mod list's activation and order in its own committed save file.
#pragma once
#include "mod_choices.h"
namespace tsuchi {
void mod_choices_start();
// Accepts the draft only after the existing storage writer commits it.
TSUCHI_Result save_mod_choices(const ModChoices& choices);
} // namespace tsuchi
