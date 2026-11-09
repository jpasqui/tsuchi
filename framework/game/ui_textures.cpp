// Textures made from a mod's pixels (ui.create_texture), as UE 4.26's FImageUtils makes them: a transient
// UTexture2D of one mip, its bulk data filled in place, then UpdateResource. Each is kept from the garbage
// collector for its mod until released or the mod stops. Game thread only.
#include "ui_textures.h"
#include "ui_screens.h"
#include "ui_service.h"
#include "game.h"
#include "list.h"
#include "object_retention.h"
#include "objects.h"
#include "registry.h"
#include <string.h>

namespace tsuchi {
namespace {
namespace t = game::table;
constexpr uint32_t kMostSide = 2048;
const Kind kTexture{"ui.texture"};

void forget(void*, uint64_t handle) { release_game_object(handle); }

// The one mip's pixels, or null when the texture is not the one asked for.
uint8_t* lock_pixels(uint64_t texture, uint32_t width, uint32_t height, void*& bulk) {
    const uint64_t platform = game::read_or<uint64_t>(texture + t::texture_2d::platform_data);
    if (!platform || !game::readable(platform, t::texture_platform_data::mip_count + 4)) return nullptr;
    if (game::read_or<uint32_t>(platform + t::texture_platform_data::width) != width ||
        game::read_or<uint32_t>(platform + t::texture_platform_data::height) != height ||
        game::read_or<uint32_t>(platform + t::texture_platform_data::format) != uint32_t(t::constant::texture_format_bgra8) ||
        game::read_or<int32_t>(platform + t::texture_platform_data::mip_count) != 1) return nullptr;
    const uint64_t mips = game::read_or<uint64_t>(platform + t::texture_platform_data::mips);
    const uint64_t mip = mips ? game::read_or<uint64_t>(mips) : 0;
    if (!mip || !game::readable(mip, t::texture_mip::bulk_data + 8)) return nullptr;
    bulk = reinterpret_cast<void*>(mip + t::texture_mip::bulk_data);
    // The size the engine allocated, before any pixel is written.
    if (t::fn::bulk_data_size(bulk) != uint64_t(width) * height * 4) return nullptr;
    return static_cast<uint8_t*>(t::fn::bulk_data_lock(bulk, 2));
}

TSUCHI_Result create_texture(TSUCHI_Mod* mod, uint32_t width, uint32_t height, const void* pixels, uint64_t bytes, TSUCHI_Object* out) {
    if (!mod || !out || !width || !height || width > kMostSide || height > kMostSide || !pixels || bytes != uint64_t(width) * height * 4)
        return TSUCHI_INVALID;
    if (!on_game_thread()) return TSUCHI_NOT_READY;
    if (!t::bound(t::fn::texture_create_transient) || !t::bound(t::fn::bulk_data_size) || !t::bound(t::fn::bulk_data_lock) ||
        !t::bound(t::fn::bulk_data_unlock)) return TSUCHI_UNSUPPORTED;
    if (reserve_owned(mod, 1) != TSUCHI_OK) return TSUCHI_NO_MEMORY;
    const uint64_t texture = reinterpret_cast<uint64_t>(t::fn::texture_create_transient(int32_t(width), int32_t(height),
                                                                                       uint32_t(t::constant::texture_format_bgra8), 0));
    const uint64_t handle = game::live(texture) ? game::to_handle(texture) : 0;
    if (!handle) return TSUCHI_NOT_READY;
    // Kept before anything else runs: the engine can collect garbage in any call.
    if (!retain_game_object(handle)) return TSUCHI_NO_MEMORY;
    void* bulk = nullptr;
    uint8_t* to = lock_pixels(texture, width, height, bulk);
    if (!to) { release_game_object(handle); return TSUCHI_NOT_READY; }
    // The engine's order is blue, green, red, alpha.
    const auto* from = static_cast<const uint8_t*>(pixels);
    for (uint64_t i = 0; i < bytes; i += 4) { to[i] = from[i + 2]; to[i + 1] = from[i + 1]; to[i + 2] = from[i]; to[i + 3] = from[i + 3]; }
    t::fn::bulk_data_unlock(bulk);
    // Pixels made at run time are not the cooker's: the render resource is made from them as they are.
    game::write<uint8_t>(texture + t::texture_2d::flags, uint8_t(game::read_or<uint8_t>(texture + t::texture_2d::flags) | t::constant::texture_not_offline_processed));
    const auto update = game::method(texture, t::texture_2d::update_resource);
    if (!update) { release_game_object(handle); return TSUCHI_NOT_READY; }
    update(reinterpret_cast<void*>(texture));
    if (own(mod, kTexture, handle, forget, nullptr) != TSUCHI_OK) { release_game_object(handle); return TSUCHI_NO_MEMORY; }
    out->id = handle;
    return TSUCHI_OK;
}

TSUCHI_Result release_texture(TSUCHI_Mod* mod, TSUCHI_Object texture) {
    if (!mod || !texture.id) return TSUCHI_INVALID;
    if (disown(mod, kTexture, texture.id) != TSUCHI_OK) return TSUCHI_NOT_FOUND;
    release_game_object(texture.id);
    return TSUCHI_OK;
}
} // namespace

void ui_textures_start() {
    auto& backend = ui_screen_backend();
    backend.create_texture = create_texture;
    backend.release_texture = release_texture;
    ui::set_backend(&backend);
}
} // namespace tsuchi
