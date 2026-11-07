// The game side of the assets service.

#include <string.h>

#include "asset_service.h"
#include "asset_bundles.h"
#include "game.h"
#include "holds.h"
#include "names.h"
#include "objects.h"
#include "object_retention.h"
#include "reflect.h"
#include "services.h"

namespace tsuchi {
namespace {

namespace t = game::table;

// The renderer may retain an asset too. Both use the same root ownership and
// release the engine flag only after the final internal hold ends.
struct Kept { bool retained; };
TSUCHI_Result keep_loaded(const Hold& hold, Kept& entry, const HoldNothing&, bool fresh) {
    if (fresh && !(entry.retained = retain_game_object(hold.object))) return TSUCHI_NO_MEMORY;
    return TSUCHI_OK;
}
void let_go(const Hold& hold, Kept& entry, HoldEnd) {
    if (entry.retained) release_game_object(hold.object);
}
Holds<Kept, HoldNothing, keep_loaded, nullptr, let_go> holds{"asset.hold", HoldOn::object, false};

TSUCHI_Result load(TSUCHI_Mod* mod, const char* path, TSUCHI_Object* out) {
    if (path[0] != '/') return TSUCHI_INVALID;
    game::Name name{};
    const TSUCHI_Result named = game::name_from_utf8(path, name);
    if (named != TSUCHI_OK) return named == TSUCHI_CAPACITY ? TSUCHI_INVALID : named;
    if (!t::bound(t::fn::load_soft_object)) return TSUCHI_NOT_READY;
    // The engine's reference to an asset by path: no object yet, and the path as one name.
    namespace s = t::soft_object;
    alignas(8) uint8_t reference[s::most] = {};
    const int32_t none = -1;
    memcpy(reference + s::index, &none, sizeof none), memcpy(reference + s::path, &name, sizeof name);
    const auto object = reinterpret_cast<uint64_t>(t::fn::load_soft_object(reference));
    const uint64_t asset = object ? game::to_handle(object) : 0;
    if (!asset) return TSUCHI_NOT_FOUND;
    const TSUCHI_Result held = holds.take(mod, asset, 0, HoldNothing{}); // the same hold when the mod has one already
    if (held == TSUCHI_OK) out->id = asset;
    return held;
}

TSUCHI_Result release(TSUCHI_Mod* mod, TSUCHI_Object asset) { return holds.release(mod, asset.id, 0); }

const asset::Backend service{load, release, begin_asset_bundle, write_asset_bundle,
    mount_asset_bundle, asset_bundle_status, release_asset_bundle};

} // namespace

void assets_start() {
    asset::set_backend(&service);
    holds.start();
}

} // namespace tsuchi
