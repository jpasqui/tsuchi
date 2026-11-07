#pragma once
#include "tsuchi/core.h"
namespace tsuchi::game {
// Extend the native restoration batch through its own per-Pikmin step. Stock
// colors and native batch limits stay in the game's original implementation.
TSUCHI_Result pikmin_birth_start(TSUCHI_Mod* owner);
}
