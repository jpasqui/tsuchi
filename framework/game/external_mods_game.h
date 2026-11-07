// The external catalog and its saved requests for the prelaunch manager.
#pragma once
#include "external_mods.h"
namespace tsuchi {
ExternalMods& external_mods();
TSUCHI_Result load_external_mods();
TSUCHI_Result save_external_mods();
}
