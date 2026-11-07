// Provisional class identity is restored on failure. Successful declarations
// retain the identity for the loaded class lifetime, as the content design requires.
#pragma once
#include "registry.h"
namespace tsuchi {
void content_identity_accept(TSUCHI_Mod* mod, uint64_t first, uint64_t last);
TSUCHI_Result content_identity_hold(TSUCHI_Mod* mod, uint64_t pattern, const char* identity);
}
