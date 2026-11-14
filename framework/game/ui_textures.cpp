// Textures made from a mod's pixels (ui.create_texture), as UE 4.26's FImageUtils makes them: a transient
// UTexture2D of one mip, its bulk data filled in place, then UpdateResource. And textures from a picture
// file's bytes (ui.decode_texture), which FImageUtils makes itself with the game's own decoders. Each is kept
// from the garbage collector for its mod until released or the mod stops. Game thread only.
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
constexpr uint32_t kMostSide = 2048, kMostDecodedSide = 4096;
constexpr uint64_t kMostImageBytes = 16ull << 20;
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
    return static_cast<uint8_t*>(t::fn::bulk_data_lock(bulk, uint32_t(t::constant::bulk_data_read_write)));
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

uint32_t big_endian16(const uint8_t* at) { return uint32_t(at[0]) << 8 | at[1]; }
uint32_t big_endian32(const uint8_t* at) { return big_endian16(at) << 16 | big_endian16(at + 2); }
// The size a PNG or JPEG file's header gives, read before the engine decodes anything, so that a small file
// cannot have it allocate for a huge picture. False for any other bytes, and for a header cut short or wrong.
bool picture_size(const uint8_t* file, uint64_t bytes, uint32_t& width, uint32_t& height) {
    static const uint8_t png[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    if (bytes >= sizeof png && !memcmp(file, png, sizeof png)) {
        // The first chunk is IHDR, 13 bytes and its check, which begin with the width and the height.
        if (bytes < 33 || big_endian32(file + 8) != 13 || memcmp(file + 12, "IHDR", 4)) return false;
        width = big_endian32(file + 16); height = big_endian32(file + 20);
        return width && height;
    }
    if (bytes < 4 || file[0] != 0xFF || file[1] != 0xD8) return false;
    // A JPEG's segments, up to the frame header of SOF0, SOF1 or SOF2: precision, height, width.
    uint64_t at = 2;
    for (;;) {
        if (at >= bytes || file[at] != 0xFF) return false;
        while (at < bytes && file[at] == 0xFF) ++at;   // fill bytes before a marker
        if (at >= bytes) return false;
        const uint8_t marker = file[at++];
        if (marker == 0x01 || (marker >= 0xD0 && marker <= 0xD7)) continue;   // markers without a length
        // Image data, the end, or another start before any frame header.
        if (marker == 0x00 || marker == 0xD8 || marker == 0xD9 || marker == 0xDA) return false;
        if (bytes - at < 2) return false;
        const uint64_t length = big_endian16(file + at);
        if (length < 2 || bytes - at < length) return false;
        if (marker == 0xC0 || marker == 0xC1 || marker == 0xC2) {
            if (length < 8) return false;
            height = big_endian16(file + at + 3); width = big_endian16(file + at + 5);
            return width && height;
        }
        // The other kinds of frame, lossless and arithmetic coded.
        if (marker >= 0xC3 && marker <= 0xCF && marker != 0xC4 && marker != 0xC8 && marker != 0xCC) return false;
        at += length;
    }
}

TSUCHI_Result decode_texture(TSUCHI_Mod* mod, const void* image, uint64_t bytes, TSUCHI_Object* out) {
    if (!mod || !out || !image || !bytes || bytes > kMostImageBytes) return TSUCHI_INVALID;
    uint32_t declared_width = 0, declared_height = 0;
    if (!picture_size(static_cast<const uint8_t*>(image), bytes, declared_width, declared_height)) return TSUCHI_INVALID;
    if (declared_width > kMostDecodedSide || declared_height > kMostDecodedSide) return TSUCHI_CAPACITY;
    if (!on_game_thread()) return TSUCHI_NOT_READY;
    if (!t::bound(t::fn::texture_from_image)) return TSUCHI_UNSUPPORTED;
    if (reserve_owned(mod, 1) != TSUCHI_OK) return TSUCHI_NO_MEMORY;
    // The engine's array of bytes, read and never changed: the file where the mod has it.
    alignas(8) uint8_t file[t::array::size]{};
    const void* data = image; const int32_t count = int32_t(bytes);
    memcpy(file + t::array::data, &data, sizeof data);
    memcpy(file + t::array::count, &count, sizeof count);
    memcpy(file + t::array::max, &count, sizeof count);
    const uint64_t texture = reinterpret_cast<uint64_t>(t::fn::texture_from_image(&file));
    const uint64_t handle = game::live(texture) ? game::to_handle(texture) : 0;
    if (!handle) return TSUCHI_INVALID;   // no decoder of the game's reads it
    if (!retain_game_object(handle)) return TSUCHI_NO_MEMORY;
    const uint64_t platform = game::read_or<uint64_t>(texture + t::texture_2d::platform_data);
    const uint32_t width = platform ? game::read_or<uint32_t>(platform + t::texture_platform_data::width) : 0;
    const uint32_t height = platform ? game::read_or<uint32_t>(platform + t::texture_platform_data::height) : 0;
    if (!width || !height || width > kMostDecodedSide || height > kMostDecodedSide) { release_game_object(handle); return TSUCHI_CAPACITY; }
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
    backend.decode_texture = decode_texture;
    ui::set_backend(&backend);
}
} // namespace tsuchi
