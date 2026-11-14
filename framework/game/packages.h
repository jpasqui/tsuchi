// Packages on the SD card: downloaded, checked, extracted, published by a receipt, and removed.
// The packages service's calls, and what the Installed page needs.
//
// Game thread only, except for the download's file callbacks, which run on an HTTPS worker.
#pragma once
#include "package_receipt.h"
#include "text.h"
#include "tsuchi/core.h"

namespace tsuchi {

// Where packages live: one folder each, named by 32 hexadecimal digits.
inline constexpr char kPackagesFolder[] = "tsuchisd:/tsuchi/downloads";

// Before the mods are found: gives the loader the installed packages' mod folders. A package folder
// without a receipt is an install that never finished, and a removed or replaced one is no longer
// used; the first listing deletes both.
void packages_start();
// The clock receipts are dated by: seconds since 1970 UTC, or below 0 when it is unavailable.
void packages_set_clock(int64_t (*seconds)());

uint32_t installed_package_count();
const package_store::Receipt* installed_package(uint32_t index);
// The installed package a mod came from, and its index; null for a mod of the game's files.
const package_store::Receipt* package_holding(const char* mod_id, uint32_t* index = nullptr);
// Marks a package removed. Its files go at once when no loaded mod uses them, else at the next start.
TSUCHI_Result remove_installed_package(uint32_t index);
bool package_install_running();
// Reads the packages folder again. The loader's view is the one from the start.
TSUCHI_Result refresh_installed_packages();

} // namespace tsuchi
