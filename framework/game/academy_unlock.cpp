#include "academy_internal.h"
#include "game.h"
#include "objects.h"
#include "reflection_fields.h"

namespace tsuchi {
TSUCHI_Result academy_hold_unlocked_periods(TSUCHI_Mod* mod, TSUCHI_FieldHold* out) {
    if (!mod_usable(mod) || !out) return TSUCHI_INVALID;
    namespace t = game::table;
    uint64_t config = game::global<uint64_t>(t::data::pikmin_scatter_override);
    if (!config || !game::live(config)) config = game::global<uint64_t>(t::data::pikmin_scatter_config);
    const uint64_t handle = config ? game::to_handle(config) : 0;
    if (!handle) return TSUCHI_NOT_READY;
    constexpr const char* path = "DDKConfig.bCheckOpenedHeld";
    game::Place place;
    TSUCHI_Result result = game::find_place(game::type_of(config), config, path, place);
    if (result != TSUCHI_OK) return result;
    game::Value before;
    result = game::read_value(place, before);
    if (result != TSUCHI_OK) return result;
    if (before.kind != game::ValueKind::boolean) return TSUCHI_UNSUPPORTED;
    const bool blocked_before = academy_uploads_blocked();
    result = academy_block_uploads();
    if (result != TSUCHI_OK) return result;
    TSUCHI_Value wanted{};
    wanted.kind = TSUCHI_VALUE_BOOLEAN;
    wanted.as.boolean = false;
    result = field_holds::hold(mod, {handle}, path, &wanted, "", out);
    if (result != TSUCHI_OK && !blocked_before) academy_unblock_uploads(); // the unlock check was not changed
    return result;
}
}
