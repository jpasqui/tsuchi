// Declared squad types and their live memberships. All memberships and
// selections use the shared hold tables.
#pragma once
#include "json.h"
#include "text.h"
#include "squad_service.h"
namespace tsuchi {
bool content_squad_valid(const Json& content, Text& wrong);
TSUCHI_Result content_squad_begin(TSUCHI_Mod* mod, const Json& content, Text& problem);
void content_squad_start(TSUCHI_Mod* self);
void squad_content_backend(squad::Backend& backend);
}
