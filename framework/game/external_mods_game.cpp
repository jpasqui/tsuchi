#include "external_mods_game.h"
#include "families.h"
#include "files.h"
#include "game.h"
#include "registry.h"
#include "save_session.h"
#include "services.h"

namespace tsuchi {
namespace {
ExternalMods catalog;
// The first build's file has no key in its name, as it did before there were several builds.
void catalog_path(char (&path)[96]) {
    namespace t = game::table;
    const char* const parts[] = {t::selected_build ? "rom:/tsuchi/external-mods-" : "rom:/tsuchi/external-mods", t::selected_build ? t::build_key : "", ".json"};
    uint32_t used = 0;
    for (const char* part : parts)
        for (; *part && used + 1 < sizeof path; ++part) path[used++] = *part;
    path[used] = 0;
}
constexpr const char* requests_path = "save:/tsuchi/external-requests.json";
struct Input {
    char* text = nullptr;
    uint64_t bytes = 0;
    ~Input() { release(text); }
};
TSUCHI_Result read_bounded(const char* path, uint64_t maximum, Input& out) {
    auto result = game::file_size(path, out.bytes);
    if (result != TSUCHI_OK) return result;
    if (out.bytes > maximum) return TSUCHI_CAPACITY;
    out.text = static_cast<char*>(allocate(size_t(out.bytes + 1)));
    if (!out.text) return TSUCHI_NO_MEMORY;
    uint64_t got = 0;
    result = game::read_file_part(path, 0, out.text, out.bytes, got);
    // A file that disappears after its size was read is a failed read, not an absent saved file.
    if (result == TSUCHI_NOT_FOUND) result = TSUCHI_IO;
    if (result == TSUCHI_OK && got != out.bytes) result = TSUCHI_IO;
    out.text[out.bytes] = 0;
    return result;
}
TSUCHI_Result write_requests(const char* text, uint64_t bytes) {
    return write_save_file(requests_path, text, bytes);
}
}

ExternalMods& external_mods() { return catalog; }

TSUCHI_Result load_external_mods() {
    if (family_absent(game::table::family::external_mods)) return TSUCHI_UNSUPPORTED;
    if (!on_game_thread()) return TSUCHI_INVALID;
    if (!game::files_known()) return TSUCHI_NOT_READY;
    Input installed;
    char path[96];
    catalog_path(path);
    auto result = read_bounded(path, kExternalCatalogBytes, installed);
    if (result == TSUCHI_NOT_FOUND) { catalog.clear(); return TSUCHI_OK; }
    if (result != TSUCHI_OK) { catalog.clear(); return result; }
    result = catalog.load(installed.text, installed.bytes);
    if (result != TSUCHI_OK) { catalog.clear(); return result; }

    namespace t = game::table;
    // Until the saved requests are read, the page shows the observed files and refuses external changes.
    if (!game::save_session_known() || game::global<uint8_t>(t::data::save_async_state) ||
        game::global<uint8_t>(t::data::save_mounted) || game::global<uint8_t>(t::data::engine_save_mounted)) {
        catalog.requests_unread(); return TSUCHI_NOT_READY;
    }
    game::SaveSession session;
    if (!session.open) { catalog.requests_unread(); return TSUCHI_NOT_READY; }
    Input requests;
    result = read_bounded(requests_path, kExternalRequestsBytes, requests);
    if (result == TSUCHI_NOT_FOUND) return TSUCHI_OK;
    if (result != TSUCHI_OK) { catalog.requests_unread(); return result; }
    return catalog.load(installed.text, installed.bytes, requests.text, requests.bytes);
}

TSUCHI_Result save_external_mods() {
    if (family_absent(game::table::family::external_mods)) return TSUCHI_UNSUPPORTED;
    if (!on_game_thread()) return TSUCHI_INVALID;
    return catalog.save(write_requests);
}
}
