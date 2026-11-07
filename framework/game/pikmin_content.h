// Added Pikmin: manifest admission and permanent saved native colors.
#pragma once
#include <stdint.h>
#include "json.h"
#include "text.h"
#include "tsuchi/core.h"
namespace tsuchi {
bool pikmin_content_valid(const Json& content, Text& wrong);
TSUCHI_Result pikmin_content_begin(TSUCHI_Mod* mod, const Json& content, Text& problem);
void pikmin_content_start(TSUCHI_Mod* self);
TSUCHI_Result added_pikmin_ready(uint8_t color);
TSUCHI_Result added_pikmin_native_ready(uint8_t color);
}
