// Runtime cooked content, owned by the asset service. Public arguments never name disk paths.
#pragma once
#include "tsuchi/asset.h"

namespace tsuchi {
TSUCHI_Result begin_asset_bundle(TSUCHI_Mod*, const char* name, const char* package_root, TSUCHI_AssetBundle*);
TSUCHI_Result write_asset_bundle(TSUCHI_Mod*, TSUCHI_AssetBundle, TSUCHI_BundlePart, uint64_t offset, const void*, uint64_t size);
TSUCHI_Result mount_asset_bundle(TSUCHI_Mod*, TSUCHI_AssetBundle, const char* shader_metadata);
TSUCHI_Result asset_bundle_status(TSUCHI_Mod*, TSUCHI_AssetBundle, TSUCHI_BundleInfo*);
// Verifies an exact canonical package path in a live caller-owned mounted bundle's persisted
// TOC. Foreign handles are DENIED, stale handles NOT_FOUND, unmounted bundles NOT_READY,
// nonmembers or malformed paths INVALID, and unreadable or changed TOCs IO. Reentry is BUSY.
// Reads only: no mount, asset load, filesystem write, or bundle status change.
TSUCHI_Result asset_bundle_package(TSUCHI_Mod*, TSUCHI_AssetBundle, const char* canonical_package_path);
TSUCHI_Result release_asset_bundle(TSUCHI_Mod*, TSUCHI_AssetBundle);
}
