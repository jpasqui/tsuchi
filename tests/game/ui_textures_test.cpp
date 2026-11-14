// ui.create_texture and ui.release_texture against a made-up engine laid out as tests/fixtures/game_table.yaml
// says: the transient texture, its one mip's bulk data, the pixel order, UpdateResource and garbage-collector roots.
#include "fake_game.h"
#include "ui_textures.h"
#include "ui_screens.h"
#include "ui_service.h"
#include "objects.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <vector>

using namespace tsuchi;
namespace t = game::table;

namespace {
uint64_t texture_class = 0, vtable = 0;
std::vector<uint8_t> bulk_bytes;
int updates = 0, unlocks = 0, locks = 0, made = 0;
bool wrong_format = false, two_mips = false, null_texture = false;
uint64_t last_texture = 0;

void update_resource(void*) { ++updates; }
void* create_transient(int32_t width, int32_t height, uint32_t format, uint64_t) {
    ++made;
    if (null_texture) return nullptr;
    const uint64_t texture = fake::object(u"Texture2D", texture_class, 0x200);
    fake::put(texture, vtable);
    const uint64_t platform = fake::memory(0x40), mips = fake::memory(16), mip = fake::memory(0x40);
    fake::put(texture + t::texture_2d::platform_data, platform);
    fake::put<int32_t>(platform + t::texture_platform_data::width, width);
    fake::put<int32_t>(platform + t::texture_platform_data::height, height);
    fake::put<uint32_t>(platform + t::texture_platform_data::format, wrong_format ? 1 : format);
    fake::put(platform + t::texture_platform_data::mips, mips);
    fake::put<int32_t>(platform + t::texture_platform_data::mip_count, two_mips ? 2 : 1);
    fake::put(mips, mip);
    bulk_bytes.assign(size_t(width) * size_t(height) * 4, 0);
    last_texture = texture;
    return reinterpret_cast<void*>(texture);
}
uint64_t bulk_size(void*) { return bulk_bytes.size(); }
void* bulk_lock(void*, uint32_t how) { assert(how == uint32_t(t::constant::bulk_data_read_write)); ++locks; return bulk_bytes.data(); }
void bulk_unlock(void*) { ++unlocks; }
bool kept(uint64_t texture) { return game::object_kept(texture); }
// The engine's picture import: a texture of the size the test names, or none for bytes it does not read.
uint32_t decoded_width = 0, decoded_height = 0, decodes = 0;
std::vector<uint8_t> decoded_from;
void* from_image(void* array) {
    const auto* at = static_cast<const uint8_t*>(array);
    const uint8_t* data = nullptr; int32_t count = 0, max = 0;
    memcpy(&data, at + t::array::data, sizeof data);
    memcpy(&count, at + t::array::count, sizeof count);
    memcpy(&max, at + t::array::max, sizeof max);
    assert(count == max);
    decoded_from.assign(data, data + count);
    ++decodes;
    if (!decoded_width) return nullptr;
    return create_transient(int32_t(decoded_width), int32_t(decoded_height), uint32_t(t::constant::texture_format_bgra8), 0);
}
// Picture files as far as their headers: a PNG's signature and IHDR chunk, and a JPEG's start, an APP0 segment
// and a frame header of the kind given.
std::vector<uint8_t> png(uint32_t width, uint32_t height) {
    std::vector<uint8_t> file = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n', 0, 0, 0, 13, 'I', 'H', 'D', 'R'};
    for (uint32_t side : {width, height})
        for (int shift = 24; shift >= 0; shift -= 8) file.push_back(uint8_t(side >> shift));
    file.insert(file.end(), {8, 6, 0, 0, 0, 1, 2, 3, 4});   // depth, colour, methods, and the chunk's check
    return file;
}
std::vector<uint8_t> jpeg(uint32_t width, uint32_t height, uint8_t frame = 0xC0) {
    std::vector<uint8_t> file = {0xFF, 0xD8, 0xFF, 0xE0, 0, 16, 'J', 'F', 'I', 'F', 0, 1, 1, 0, 0, 1, 0, 1, 0, 0};
    file.insert(file.end(), {0xFF, frame, 0, 17, 8, uint8_t(height >> 8), uint8_t(height), uint8_t(width >> 8), uint8_t(width), 3,
                             1, 0x22, 0, 2, 0x11, 1, 3, 0x11, 1});
    file.insert(file.end(), {0xFF, 0xD9});
    return file;
}
}

namespace tsuchi {
ui::Backend& ui_screen_backend() { static ui::Backend backend{}; return backend; }
}

int main() {
    note_game_thread();
    fake::start();
    const uint64_t root = fake::object(u"Class", 0, 0x100);
    texture_class = fake::object(u"Texture2D", root, 0x100);
    vtable = fake::memory(0x100);
    fake::put(vtable + t::texture_2d::update_resource, &update_resource);
    fake::code().push_back(reinterpret_cast<uint64_t>(&update_resource));
    const TSUCHI_UiService& ui = ui::table;
    TSUCHI_Mod* mod = mod_create("pictures.mod");
    const uint8_t pixels[2 * 2 * 4] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
    TSUCHI_Object texture{};
    // Without the game's calls the family is unsupported.
    ui_textures_start();
    assert(ui.create_texture(mod, 2, 2, pixels, sizeof pixels, &texture) == TSUCHI_UNSUPPORTED);
    t::fn::texture_create_transient = create_transient; t::fn::bulk_data_size = bulk_size;
    t::fn::bulk_data_lock = bulk_lock; t::fn::bulk_data_unlock = bulk_unlock;
    // Sizes and byte counts are checked before the engine is asked.
    assert(ui.create_texture(mod, 0, 2, pixels, sizeof pixels, &texture) == TSUCHI_INVALID);
    assert(ui.create_texture(mod, 2049, 1, pixels, sizeof pixels, &texture) == TSUCHI_INVALID);
    assert(ui.create_texture(mod, 2, 2, pixels, sizeof pixels - 1, &texture) == TSUCHI_INVALID && made == 0);
    // Made: the pixels in the engine's order, the resource updated, the texture kept and owned.
    assert(ui.create_texture(mod, 2, 2, pixels, sizeof pixels, &texture) == TSUCHI_OK && texture.id);
    const uint64_t object = game::from_handle(texture.id);
    assert(object == last_texture && locks == 1 && unlocks == 1 && updates == 1);
    const std::vector<uint8_t> expected = {3, 2, 1, 4, 7, 6, 5, 8, 11, 10, 9, 12, 15, 14, 13, 16};
    assert(bulk_bytes == expected);
    assert(fake::get<uint8_t>(object + t::texture_2d::flags) & t::constant::texture_not_offline_processed);
    assert(kept(object) && owned_count(mod) == 1);
    // Released: no longer kept, and not the mod's.
    assert(ui.release_texture(mod, texture) == TSUCHI_OK && !kept(object) && owned_count(mod) == 0);
    assert(ui.release_texture(mod, texture) == TSUCHI_NOT_FOUND);
    // A texture that is not what was asked for is not written, and not kept.
    wrong_format = true;
    assert(ui.create_texture(mod, 2, 2, pixels, sizeof pixels, &texture) == TSUCHI_NOT_READY && locks == 1 && !kept(last_texture));
    wrong_format = false; two_mips = true;
    assert(ui.create_texture(mod, 2, 2, pixels, sizeof pixels, &texture) == TSUCHI_NOT_READY && locks == 1 && !kept(last_texture));
    two_mips = false; null_texture = true;
    assert(ui.create_texture(mod, 2, 2, pixels, sizeof pixels, &texture) == TSUCHI_NOT_READY);
    null_texture = false;
    // A picture file: the game's own import makes the texture from the bytes as they are.
    const std::vector<uint8_t> file = png(640, 360);
    assert(ui.decode_texture(mod, file.data(), file.size(), &texture) == TSUCHI_UNSUPPORTED);
    t::fn::texture_from_image = from_image;
    assert(ui.decode_texture(mod, file.data(), 0, &texture) == TSUCHI_INVALID && decodes == 0);
    assert(ui.decode_texture(mod, file.data(), (16ull << 20) + 1, &texture) == TSUCHI_INVALID && decodes == 0);
    assert(ui.decode_texture(mod, file.data(), file.size(), &texture) == TSUCHI_INVALID && decodes == 1);   // nothing reads it
    decoded_width = 640; decoded_height = 360;
    assert(ui.decode_texture(mod, file.data(), file.size(), &texture) == TSUCHI_OK && texture.id && decodes == 2);
    assert(decoded_from == file);
    const uint64_t picture = game::from_handle(texture.id);
    assert(picture == last_texture && kept(picture) && owned_count(mod) == 1);
    assert(ui.release_texture(mod, texture) == TSUCHI_OK && !kept(picture) && owned_count(mod) == 0);
    const std::vector<uint8_t> photo = jpeg(640, 360);
    assert(ui.decode_texture(mod, photo.data(), photo.size(), &texture) == TSUCHI_OK && decodes == 3 && decoded_from == photo);
    assert(ui.release_texture(mod, texture) == TSUCHI_OK && owned_count(mod) == 0);
    // The header is read first: a size over the limit, a header cut short or wrong, and any other kind of file
    // never reach the engine.
    const auto refused = [&](const std::vector<uint8_t>& bytes, size_t count, TSUCHI_Result expected) {
        const uint32_t before = decodes;
        return ui.decode_texture(mod, bytes.data(), count, &texture) == expected && decodes == before && owned_count(mod) == 0;
    };
    for (const auto& large : {png(4097, 16), png(16, 4097), png(0x7FFFFFFF, 0x7FFFFFFF), jpeg(4097, 16), jpeg(16, 65535), jpeg(65535, 65535, 0xC2),
                              jpeg(5000, 10, 0xC1)})
        assert(refused(large, large.size(), TSUCHI_CAPACITY));
    assert(refused(png(4096, 4096), 32, TSUCHI_INVALID));
    for (size_t count = 1; count < 33; ++count) assert(refused(png(4097, 16), count, TSUCHI_INVALID));
    for (size_t count = 1; count < 39; ++count) assert(refused(jpeg(4097, 16), count, TSUCHI_INVALID));
    std::vector<uint8_t> wrong = png(16, 16); wrong[11] = 12;   // IHDR's length
    assert(refused(wrong, wrong.size(), TSUCHI_INVALID));
    wrong = png(16, 16); wrong[12] = 'i';
    assert(refused(wrong, wrong.size(), TSUCHI_INVALID));
    assert(refused(png(0, 16), 33, TSUCHI_INVALID) && refused(jpeg(16, 0), 41, TSUCHI_INVALID));
    wrong = jpeg(16, 16); wrong[5] = 0xF0;   // APP0 says it runs past the end of the file
    assert(refused(wrong, wrong.size(), TSUCHI_INVALID));
    wrong = jpeg(16, 16); wrong[23] = 7;   // a frame header too short to hold the size
    assert(refused(wrong, wrong.size(), TSUCHI_INVALID));
    wrong = jpeg(16, 16); wrong[21] = 0xDA;   // the image data before any frame header
    assert(refused(wrong, wrong.size(), TSUCHI_INVALID));
    assert(refused(jpeg(16, 16, 0xC3), 41, TSUCHI_INVALID));   // a lossless frame
    const std::vector<uint8_t> gif = {'G', 'I', 'F', '8', '9', 'a', 0xFF, 0xFF, 0xFF, 0xFF, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    assert(refused(gif, gif.size(), TSUCHI_INVALID));
    // A decoded picture larger than the limit is let go of at once.
    decoded_width = 4097; decoded_height = 16;
    assert(ui.decode_texture(mod, file.data(), file.size(), &texture) == TSUCHI_CAPACITY && !kept(last_texture) && owned_count(mod) == 0);
    decoded_width = 0;
    // Another mod cannot release it, and the owner stopping lets go of it.
    TSUCHI_Mod* other = mod_create("other.mod");
    assert(ui.create_texture(mod, 2, 2, pixels, sizeof pixels, &texture) == TSUCHI_OK);
    const uint64_t second = game::from_handle(texture.id);
    assert(ui.release_texture(other, texture) == TSUCHI_NOT_FOUND && kept(second));
    mod_stop(mod);
    assert(!kept(second));
    std::puts("textures ok");
    return 0;
}
