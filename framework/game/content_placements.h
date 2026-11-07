// Placement values declared by added content. Game thread only.
#pragma once
#include "json.h"
#include "text.h"
#include "tsuchi/content.h"
namespace tsuchi {
bool content_placements_valid(const Json& content, Text& problem);
TSUCHI_Result content_placements_begin(TSUCHI_Mod* mod, const Json& content, Text& problem);
TSUCHI_Result content_placement_values(const char* kind, char* buffer, uint64_t capacity, uint64_t* out);
TSUCHI_Result content_placement_kind(const char* kind, const char* value, TSUCHI_PlacementKind* out);
TSUCHI_Result content_placement_toggle(const char* kind, const char* value, bool* out);
TSUCHI_Result content_placement_integer(const char* kind, const char* value, int64_t* out);
TSUCHI_Result content_placement_number(const char* kind, const char* value, float* out);
TSUCHI_Result content_placement_text(const char* kind, const char* value, char* buffer, uint64_t capacity, uint64_t* out);
}
