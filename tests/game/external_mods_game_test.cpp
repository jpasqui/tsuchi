#include "fake_game.h"
#include "external_mods_game.h"
#include <cassert>
#include <string>
using namespace tsuchi;
namespace {
namespace t = game::table;
const std::string revision(64, 'a');
std::string catalog = "{\"format\":1,\"revision\":\"" + revision +
    "\",\"backend\":\"companion\",\"mods\":[{\"id\":\"external.test\",\"name\":\"Test\",\"version\":\"1.0.0\","
    "\"kind\":\"exefs\",\"state\":\"enabled\",\"problem\":\"\",\"manageable\":true}]}";
std::string requests;
bool known = true, catalog_present = true, requests_present = false, can_open = true, opened = false;
uint64_t thread = 1, extra_size = 0, requests_extra_size = 0;
unsigned opens = 0, closes = 0, reads = 0, writes = 0;
TSUCHI_Result read_result = TSUCHI_OK, write_result = TSUCHI_OK, requests_read_result = TSUCHI_OK;
bool short_read = false, requests_short_read = false;
TSUCHI_Result requests_part_result = TSUCHI_OK;
bool begin(int32_t mode, bool a, bool b) {
    assert(!mode && !a && !b && !opened); ++opens; opened = can_open; return opened;
}
void end() { assert(opened); opened = false; ++closes; }
bool is_requests(const char* path) {
    if (!strcmp(path, "save:/tsuchi/external-requests.json")) { assert(opened); return true; }
    assert(!strcmp(path, "rom:/tsuchi/external-mods.json")); return false;
}
}
namespace tsuchi::game {
bool files_known() { return known; }
TSUCHI_Result file_size(const char* path, uint64_t& bytes) {
    const bool saved = is_requests(path);
    bytes = (saved ? requests.size() + requests_extra_size : catalog.size()) + extra_size;
    if (saved && requests_present && requests_read_result != TSUCHI_OK) return requests_read_result;
    return (saved ? requests_present : catalog_present) ? read_result : TSUCHI_NOT_FOUND;
}
TSUCHI_Result read_file_part(const char* path, uint64_t offset, void* out, uint64_t capacity, uint64_t& bytes) {
    assert(!offset); ++reads;
    const bool saved = is_requests(path);
    const auto& text = saved ? requests : catalog;
    assert(capacity == text.size());
    bytes = text.size() - ((short_read || (saved && requests_short_read)) && !text.empty());
    memcpy(out, text.data(), size_t(bytes)); return saved ? requests_part_result : read_result;
}
}
namespace tsuchi {
TSUCHI_Result write_save_file(const char* path, const void* text, uint64_t bytes) {
    assert(!opened && !strcmp(path, "save:/tsuchi/external-requests.json")); ++writes;
    if (write_result == TSUCHI_OK) { requests.assign(static_cast<const char*>(text), size_t(bytes)); requests_present = true; }
    return write_result;
}
}
int main() {
    set_thread_probe([] { return thread; }); note_game_thread();
    fake::main_image() = fake::memory(0x6000);
    t::fn::save_begin_session = begin; t::fn::save_end_session = end;
    known = false; assert(load_external_mods() == TSUCHI_NOT_READY && !reads); known = true;
    catalog_present = false; assert(load_external_mods() == TSUCHI_OK && !external_mods().count() && !opens);
    catalog_present = true; assert(load_external_mods() == TSUCHI_OK && external_mods().count() == 1);
    assert(!opened && opens == closes && external_mods().at(0)->requested);
    assert(external_mods().request("external.test", false) == TSUCHI_OK);
    write_result = TSUCHI_IO; assert(save_external_mods() == TSUCHI_IO && external_mods().changed());
    write_result = TSUCHI_OK; assert(save_external_mods() == TSUCHI_OK && !external_mods().changed() && external_mods().queued());
    assert(load_external_mods() == TSUCHI_OK && !external_mods().at(0)->requested);
    assert(external_mods().at(0)->observed == ExternalModState::enabled);
    requests[requests.find(revision)] = 'b';
    assert(load_external_mods() == TSUCHI_INVALID && external_mods().at(0)->requested);
    requests_present = false;
    for (const auto flag : {t::data::save_async_state, t::data::save_mounted, t::data::engine_save_mounted}) {
        fake::put<uint8_t>(fake::main_image() + flag, 1);
        assert(load_external_mods() == TSUCHI_NOT_READY && external_mods().count() == 1 && !opened);
        fake::put<uint8_t>(fake::main_image() + flag, 0);
    }
    can_open = false; assert(load_external_mods() == TSUCHI_NOT_READY && external_mods().count() == 1); can_open = true;
    const unsigned before = reads;
    extra_size = kExternalCatalogBytes; assert(load_external_mods() == TSUCHI_CAPACITY && !external_mods().count() && reads == before); extra_size = 0;
    short_read = true; assert(load_external_mods() == TSUCHI_IO && !external_mods().count()); short_read = false;
    read_result = TSUCHI_IO; assert(load_external_mods() == TSUCHI_IO); read_result = TSUCHI_OK;
    assert(load_external_mods() == TSUCHI_OK);
    thread = 2; assert(load_external_mods() == TSUCHI_INVALID && save_external_mods() == TSUCHI_INVALID); thread = 1;

    // Requests that cannot be read yet are never overwritten from the observed files: an earlier unapplied
    // request for one package survives a page that opened without it, whatever the player changes there.
    catalog = "{\"format\":1,\"revision\":\"" + revision + "\",\"backend\":\"companion\",\"mods\":["
        "{\"id\":\"external.a\",\"name\":\"A\",\"version\":\"1.0.0\",\"kind\":\"exefs\",\"state\":\"enabled\",\"problem\":\"\",\"manageable\":true},"
        "{\"id\":\"external.b\",\"name\":\"B\",\"version\":\"1.0.0\",\"kind\":\"exefs\",\"state\":\"enabled\",\"problem\":\"\",\"manageable\":true}]}";
    requests = "{\"format\":1,\"revision\":\"" + revision + "\",\"enabled\":{\"external.a\":false,\"external.b\":true}}";
    requests_present = true;
    const std::string earlier = requests;
    // Empty and damaged files are not a successful read of saved requests. In Eden an open denied by the
    // host can leave the VFS file visible with zero bytes, rather than returning TSUCHI_IO to this adapter.
    for (const std::string& damaged : {std::string(), std::string("{\"format\":1,")}) {
        requests = damaged;
        assert(load_external_mods() == TSUCHI_INVALID && external_mods().count() == 2);
        assert(external_mods().unread());
        const unsigned before_write = writes;
        assert(external_mods().request("external.b", false) == TSUCHI_NOT_READY);
        assert(save_external_mods() == TSUCHI_NOT_READY && writes == before_write && requests == damaged);
    }
    requests = earlier;
    for (unsigned failure = 0; failure < 4; ++failure) {
        requests_extra_size = failure == 0 ? kExternalRequestsBytes : 0;
        requests_short_read = failure == 1;
        requests_part_result = failure == 2 ? TSUCHI_IO : failure == 3 ? TSUCHI_NOT_FOUND : TSUCHI_OK;
        assert(load_external_mods() == (failure == 0 ? TSUCHI_CAPACITY : TSUCHI_IO));
        assert(external_mods().unread() && external_mods().count() == 2);
        const unsigned before_write = writes;
        assert(external_mods().request("external.b", false) == TSUCHI_NOT_READY);
        assert(save_external_mods() == TSUCHI_NOT_READY && writes == before_write && requests == earlier);
    }
    requests_extra_size = 0; requests_short_read = false; requests_part_result = TSUCHI_OK;
    for (const auto unreadable : {0, 1}) {
        if (unreadable) requests_read_result = TSUCHI_IO; else can_open = false;
        const TSUCHI_Result loaded = load_external_mods();
        assert(loaded == (unreadable ? TSUCHI_IO : TSUCHI_NOT_READY) && external_mods().count() == 2);
        const unsigned writes_before = writes;
        assert(external_mods().request("external.b", false) != TSUCHI_OK && !external_mods().changed());
        assert(save_external_mods() != TSUCHI_OK && writes == writes_before && requests == earlier);
        can_open = true; requests_read_result = TSUCHI_OK;
    }
    // Once read, the earlier request is the saved one and changes save as before.
    assert(load_external_mods() == TSUCHI_OK && !external_mods().find("external.a")->requested);
    assert(external_mods().request("external.b", false) == TSUCHI_OK && save_external_mods() == TSUCHI_OK);
    assert(requests.find("\"external.a\":false") != std::string::npos && requests.find("\"external.b\":false") != std::string::npos);
    external_mods().clear();
}
