#define _SILENCE_CXX17_CODECVT_HEADER_DEPRECATION_WARNING
#include "ui_screens.h"
#include "ui_service.h"
#include "holds.h"
#include "frame.h"
#include "actor_events.h"
#include "ordinary_lifecycle.h"
#include "mods.h"
#include <cassert>
#include <cstring>
#include <cstdlib>
#include <string>
#include <vector>
#include <codecvt>
#include <locale>
#include <stdexcept>
using namespace tsuchi;
namespace {
std::vector<void(*)()> steps, engine_steps;
bool fail_engine_registration = true;
unsigned engine_registration_attempts = 0;
unsigned opens=0,closes=0,updates=0,changed=0,activated=0,closed=0,foreign_events=0;
bool host_live=true,fail_open=false,fail_update=false;
std::string file;
std::string shown;
uint64_t thread=1;
bool fail_allocation = false;
void* test_allocate(size_t bytes) { return fail_allocation ? nullptr : std::malloc(bytes); }
void* watched_release = nullptr;
unsigned watched_releases = 0;
void test_release(void* block) { if (block && block == watched_release) ++watched_releases; std::free(block); }
TSUCHI_Result open_view(const UiScreen& screen,void** view) { if(fail_open)return TSUCHI_NOT_READY; ++opens;shown=screen.title.get();*view=reinterpret_cast<void*>(1);return TSUCHI_OK; }
bool update_view(void*,const UiScreen& screen) { ++updates;shown=screen.title.get();for(uint32_t i=0;i<screen.controls.count;++i)shown+='|'+std::string(screen.controls[i].label.get());return !fail_update; }
bool live_view(void*) {return host_live;}
void close_view(void*) {++closes;}
UiRenderer renderer{open_view,update_view,live_view,close_view};
struct Events { unsigned resets = 0, activated = 0; uint64_t reset_screen = 0; std::string control; };
struct OpaqueView { uint64_t marker; bool live = false, attached = false; };
OpaqueView first_view{0x12345678}, second_view{0x87654321};
unsigned first_opens = 0, first_updates = 0, first_closes = 0, first_actions = 0, first_buttons = 0;
unsigned second_opens = 0, second_updates = 0, second_closes = 0, second_actions = 0, second_buttons = 0;
unsigned first_host_queries = 0, second_host_queries = 0, first_detaches = 0, second_detaches = 0;
constexpr uint64_t first_host = 111, second_host = 222;
uint64_t first_bound_host = first_host;
uint64_t seen_host = 0, seen_buttons = 0;
uint32_t seen_action = 0;
TSUCHI_Mod* routing_owner = nullptr;
TSUCHI_Screen routing_screen{};
bool swap_on_open = false, close_on_action = false, close_on_buttons = false, close_on_live = false, stop_on_update = false;
bool lose_host_on_update = false, in_first_update = false, replace_on_host_query = false, replace_on_detach = false;
bool reuse_view_on_host_query = false;
UiRenderer first_renderer, second_renderer;
TSUCHI_Result open_first(const UiScreen&, void** out) {
    assert(!first_view.live); first_view.live = true; first_view.attached = true; ++first_opens; *out = &first_view;
    if (swap_on_open) { swap_on_open = false; ui_set_renderer(&second_renderer); }
    return TSUCHI_OK;
}
TSUCHI_Result open_second(const UiScreen&, void** out) { assert(!second_view.live); second_view.live = true; second_view.attached = true; ++second_opens; *out = &second_view; return TSUCHI_OK; }
bool live_first(void* view) {
    assert(view == &first_view && first_view.live);
    if (close_on_live) { close_on_live = false; assert(ui::table.release_open(routing_owner, routing_screen) == TSUCHI_OK); }
    return true;
}
bool live_second(void* view) { assert(view == &second_view && second_view.live); return true; }
bool update_first(void* view, const UiScreen&) {
    assert(view == &first_view && first_view.live); ++first_updates;
    if (stop_on_update) { stop_on_update = false; mod_stop(routing_owner); }
    if (lose_host_on_update) {
        lose_host_on_update = false; in_first_update = true;
        const unsigned before_close = first_closes, before_detach = first_detaches;
        ui_host_lost(first_host);
        // Native source arrays must be detached immediately, while the view
        // remains alive until the real Showing keep/update invocation returns.
        assert(first_detaches == before_detach + 1 && !first_view.attached);
        assert(first_closes == before_close && first_view.live);
        in_first_update = false;
    }
    return true;
}
bool update_second(void* view, const UiScreen&) { assert(view == &second_view && second_view.live); ++second_updates; return true; }
void close_first(void* view) {
    assert(view == &first_view && first_view.live && !in_first_update); first_view.live = false; ++first_closes;
    const TSUCHI_Result result = ui::table.release_open(routing_owner, routing_screen);
    assert(result == TSUCHI_NOT_FOUND || (!mod_usable(routing_owner) && result == TSUCHI_INVALID));
}
void close_second(void* view) { assert(view == &second_view && second_view.live); second_view.live = false; ++second_closes; }
bool action_first(void* view, uint64_t host, uint32_t action) {
    assert(view == &first_view && first_view.live); ++first_actions; seen_host = host; seen_action = action;
    if (close_on_action) { close_on_action = false; assert(ui::table.release_open(routing_owner, routing_screen) == TSUCHI_OK); ui_set_renderer(&second_renderer); }
    return true;
}
bool action_second(void* view, uint64_t host, uint32_t action) {
    assert(view == &second_view && second_view.live); ++second_actions; seen_host = host; seen_action = action;
    return host == 77;
}
void buttons_first(void* view, uint64_t held) { assert(view == &first_view && first_view.live); ++first_buttons; seen_buttons = held; }
void buttons_second(void* view, uint64_t held) {
    assert(view == &second_view && second_view.live); ++second_buttons; seen_buttons = held;
    if (close_on_buttons) { close_on_buttons = false; assert(ui::table.release_open(routing_owner, routing_screen) == TSUCHI_OK); ui_set_renderer(&first_renderer); }
}
void replace_showing() {
    assert(ui::table.release_open(routing_owner, routing_screen) == TSUCHI_OK);
    ui_set_renderer(&second_renderer);
    assert(ui::table.hold_open(routing_owner, routing_screen) == TSUCHI_OK);
    assert(ui_active_view() == &second_view && second_view.live);
}
bool uses_first_host(void* view, uint64_t host) {
    assert(view == &first_view && first_view.live); ++first_host_queries;
    if (host != first_bound_host) return false;
    if (replace_on_host_query) { replace_on_host_query = false; replace_showing(); }
    if (reuse_view_on_host_query) {
        reuse_view_on_host_query = false;
        assert(ui::table.release_open(routing_owner, routing_screen) == TSUCHI_OK);
        first_bound_host = second_host;
        ui_set_renderer(&first_renderer);
        assert(ui::table.hold_open(routing_owner, routing_screen) == TSUCHI_OK);
        assert(ui_active_view() == &first_view && first_view.live && first_view.attached);
    }
    return true;
}
bool uses_second_host(void* view, uint64_t host) {
    assert(view == &second_view && second_view.live); ++second_host_queries;
    return host == second_host;
}
void detach_first_host(void* view, uint64_t host) {
    assert(view == &first_view && first_view.live && host == first_bound_host && first_view.attached);
    ++first_detaches; first_view.attached = false;
    if (replace_on_detach) { replace_on_detach = false; replace_showing(); }
}
void detach_second_host(void* view, uint64_t host) {
    assert(view == &second_view && second_view.live && host == second_host && second_view.attached);
    ++second_detaches; second_view.attached = false;
}
void install_temporary_renderer() {
    UiRenderer temporary{open_first, update_first, live_first, close_first, action_first, buttons_first, uses_first_host, detach_first_host};
    ui_set_renderer(&temporary);
    temporary = second_renderer; // both the default and its open view must own a copy
}
void world_frame() { note_game_thread(); for (auto step : steps) step(); }
void engine_frame() { note_game_thread(); for (auto work : engine_steps) work(); }
void frame() { engine_frame(); world_frame(); }
}
namespace tsuchi {
bool ui_native_prepare_input() { return false; }
bool each_frame(void(*step)()) {steps.push_back(step);return true;}
bool each_engine_frame(void(*work)()) {
    ++engine_registration_attempts;
    if (fail_engine_registration) return false;
    engine_steps.push_back(work); return true;
}
bool on_actor_left(void(*)(uint64_t,uint64_t,ActorLeft)) {return true;}
void ordinary_lifecycle_listener(void(*)(uint64_t,uint64_t,bool)) {}
TSUCHI_Result ordinary_lifecycle_watch(uint64_t) {return TSUCHI_OK;}
uint64_t ordinary_lifecycle_restore_part(uint64_t part,uint64_t) {return part;}
bool mod_file_path(Texts& out,const TSUCHI_Mod*,const char* path) {return out.add(path);}
}
namespace tsuchi::game {
uint64_t from_handle(uint64_t id) {return id;}
uint64_t to_handle(uint64_t id) {return id;}
uint64_t part_owner(uint64_t id) {return id;}
bool is_game_actor(uint64_t) {return true;}
bool path_stays_within(const char* path) {return path&&*path&&path[0]!='/'&&!std::strchr(path,':');}
TSUCHI_Result read_whole_file(const char* path,char*& out,uint64_t& bytes) {
    if(std::strcmp(path,"screen.json"))return TSUCHI_NOT_FOUND;
    out=static_cast<char*>(allocate(file.size()+1));if(!out)return TSUCHI_NO_MEMORY;
    std::memcpy(out,file.c_str(),file.size()+1);bytes=file.size();return TSUCHI_OK;
}
bool utf8_to_units(const char* text,char16_t* out,size_t capacity,size_t& count) {
    // An independent standard decoder keeps this host model from accepting only
    // ASCII, without copying the game's converter into the expected results.
    std::wstring_convert<std::codecvt_utf8<char32_t>, char32_t> decoder;
    std::u32string points;
    try {
        points = decoder.from_bytes(text);
        if (decoder.converted() != std::strlen(text)) return false;
        // MSVC's codecvt accepts overlong sequences (for example C0 AF).
        // Canonical re-encoding must reproduce the exact original UTF-8 bytes.
        if (decoder.to_bytes(points) != text) return false;
    } catch (const std::range_error&) { return false; }
    count = 0;
    for (char32_t point : points) {
        if (point > 0x10FFFF || (point >= 0xD800 && point <= 0xDFFF)) return false;
        count += point > 0xFFFF ? 2 : 1;
    }
    if (out && capacity < count) return false;
    size_t at = 0;
    if (out) for (char32_t point : points) {
        if (point <= 0xFFFF) out[at++] = char16_t(point);
        else { point -= 0x10000; out[at++] = char16_t(0xD800 + (point >> 10)); out[at++] = char16_t(0xDC00 + (point & 0x3FF)); }
    }
    return true;
}
}
namespace {
std::string json_text(const std::string& value) {
    std::string result = "\"";
    for (char byte : value) { if (byte == '"' || byte == '\\') result += '\\'; result += byte; }
    return result + '"';
}
void glyph_and_footer_tests() {
    const auto& ui = ui::table;
    TSUCHI_Mod* owner = mod_create("garden.glyphs"), *other = mod_create("garden.glyph-watcher"); assert(owner && other);
    unsigned events = 0; TSUCHI_Subscription subscription{};
    assert(ui.on_changed(owner, [](void* p, const TSUCHI_UiChanged*) { ++*static_cast<unsigned*>(p); }, &events, &subscription) == TSUCHI_OK);
    assert(ui.on_activated(owner, [](void* p, const TSUCHI_UiActivated*) { ++*static_cast<unsigned*>(p); }, &events, &subscription) == TSUCHI_OK);
    assert(ui.on_reset_requested(owner, [](void* p, const TSUCHI_UiResetRequested*) { ++*static_cast<unsigned*>(p); }, &events, &subscription) == TSUCHI_OK);
    const char* const tokens[] = {
        "[PictureFont:IconData key=\"A_Button\" scale=\"0.76\"/]",
        "[PictureFont:IconData key=\"B_Button\" scale=\"0.76\"/]",
        "[PictureFont:IconData key=\"X_Button\" scale=\"0.76\"/]",
        "[PictureFont:IconData key=\"Y_Button\" scale=\"0.76\"/]",
        "[PictureFont:IconData key=\"L_Button\" scale=\"0.76\"/]",
        "[PictureFont:IconData key=\"R_Button\" scale=\"0.76\"/]",
        "[PictureFont:IconData key=\"ZR_Button\" scale=\"0.76\"/]"
    };
    const TSUCHI_GlyphButton buttons[] = {TSUCHI_GLYPH_BUTTON_A, TSUCHI_GLYPH_BUTTON_B, TSUCHI_GLYPH_BUTTON_X,
        TSUCHI_GLYPH_BUTTON_Y, TSUCHI_GLYPH_BUTTON_L, TSUCHI_GLYPH_BUTTON_R, TSUCHI_GLYPH_BUTTON_ZR};
    char buffer[641]; uint64_t needed = 0;
    for (size_t i = 0; i < sizeof buttons / sizeof *buttons; ++i) {
        std::memcpy(buffer, "untouched", sizeof "untouched");
        assert(ui.glyph(owner, buttons[i], buffer, 1, &needed) == TSUCHI_CAPACITY);
        assert(needed == std::strlen(tokens[i]) + 1 && !std::strcmp(buffer, "untouched"));
        assert(ui.glyph(owner, buttons[i], buffer, sizeof buffer, &needed) == TSUCHI_OK && !std::strcmp(buffer, tokens[i]));
        buffer[0] = '!'; // Returned buffers do not expose the shared token storage.
        assert(ui.glyph(owner, buttons[i], buffer, sizeof buffer, &needed) == TSUCHI_OK && !std::strcmp(buffer, tokens[i]));
    }
    std::memcpy(buffer, "untouched", sizeof "untouched"); needed = 1234;
    assert(ui.glyph(owner, TSUCHI_GlyphButton(99), buffer, sizeof buffer, &needed) == TSUCHI_INVALID);
    assert(ui_screen_backend().glyph(TSUCHI_GlyphButton(UINT32_MAX), buffer, sizeof buffer, &needed) == TSUCHI_INVALID);
    assert(needed == 1234 && !std::strcmp(buffer, "untouched"));

    TSUCHI_Screen screen{}, ignored{0x1234};
    assert(ui.create(owner, tokens[0], &ignored) == TSUCHI_INVALID && ignored.id == 0x1234);
    assert(ui.create(owner, u8"Résumé 日本語", &screen) == TSUCHI_OK);
    const std::string label = std::string(tokens[0]) + tokens[1] + u8" 日本語";
    const std::string message = std::string(u8"Prêt 😀 ") + tokens[6];
    const std::string choices = "[{\"id\":\"first\",\"label\":" + json_text(label) + "},{\"id\":\"second\",\"label\":\"Second\"}]";
    std::vector<char> copied_label(label.begin(), label.end()); copied_label.push_back(0);
    std::vector<char> copied_message(message.begin(), message.end()); copied_message.push_back(0);
    assert(ui.add_button(owner, screen, "go", copied_label.data()) == TSUCHI_OK);
    assert(ui.add_choice(owner, screen, "choice", label.c_str(), choices.c_str(), "first") == TSUCHI_OK);
    assert(ui.add_integer(owner, screen, "count", label.c_str(), 0, 5, 1, 0) == TSUCHI_OK);
    assert(ui.add_text(owner, screen, "message", label.c_str(), copied_message.data()) == TSUCHI_OK);
    copied_label.assign(copied_label.size(), '!'); copied_message.assign(copied_message.size(), '!');
    assert(ui.hold_open(owner, screen) == TSUCHI_OK);
    const UiScreen* model = ui_active_screen(); assert(model && !*model->footer.get());
    assert(!std::strcmp(model->controls[0].label.get(), label.c_str()));
    assert(!std::strcmp(model->controls[1].labels.first(), label.c_str()));
    assert(!std::strcmp(model->controls[3].text.get(), message.c_str()));
    const void* view = ui_active_view(); const unsigned before_opens = opens, before_closes = closes;
    const uint32_t selected = model->selected, initial_revision = model->revision;
    std::string footer = std::string(tokens[0]) + u8" Choisir " + tokens[1] + " Back";
    std::vector<char> copied_footer(footer.begin(), footer.end()); copied_footer.push_back(0);
    assert(ui.set_footer(owner, screen, copied_footer.data()) == TSUCHI_OK);
    copied_footer.assign(copied_footer.size(), '!'); frame();
    assert(!std::strcmp(model->footer.get(), footer.c_str()) && model->revision == initial_revision + 1);
    assert(ui_active_view() == view && model->selected == selected && opens == before_opens && closes == before_closes && events == 0);
    uint32_t revision = model->revision;
    assert(ui.set_footer(other, screen, "Foreign") == TSUCHI_DENIED);
    assert(model->revision == revision && !std::strcmp(model->footer.get(), footer.c_str()));
    const char* const invalid[] = {"[unsafe]", "[PictureFont:IconData key=\"A_Button\" scale=\"1\"/]",
        "[PictureFont:IconData key=\"Unknown_Button\" scale=\"0.76\"/]",
        "[PictureFont:IconData key=\"A_Button\" scale=\"0.76\" extra=\"x\"/]",
        "[PictureFont:IconData key=\"A_Button\" scale=\"0.76\"/", "stray ]", "line\nbreak", "tab\there", "delete\177",
        "\xC0\xAF", "\xE3\x81", "\xED\xA0\x80", "\xF4\x90\x80\x80"};
    for (const char* bad : invalid) {
        assert(ui.set_footer(owner, screen, bad) == TSUCHI_INVALID);
        assert(ui.set_text(owner, screen, "message", bad) == TSUCHI_INVALID);
        assert(model->revision == revision && !std::strcmp(model->footer.get(), footer.c_str()));
        assert(!std::strcmp(model->controls[3].text.get(), message.c_str()));
    }
    const std::string maximum = std::string(tokens[0]) + std::string(640 - std::strlen(tokens[0]), 'x');
    assert(ui.set_footer(owner, screen, maximum.c_str()) == TSUCHI_OK && std::strlen(model->footer.get()) == 640);
    revision = model->revision;
    assert(ui.set_footer(owner, screen, (maximum + 'x').c_str()) == TSUCHI_INVALID && model->revision == revision);
    std::string multibyte; for (unsigned i = 0; i < 320; ++i) multibyte += u8"é";
    assert(ui.set_text(owner, screen, "message", multibyte.c_str()) == TSUCHI_OK);
    revision = model->revision;
    assert(ui.set_text(owner, screen, "message", (multibyte + u8"é").c_str()) == TSUCHI_INVALID && model->revision == revision);
    assert(ui.set_text(owner, screen, "message", message.c_str()) == TSUCHI_OK);
    assert(ui.set_footer(owner, screen, "") == TSUCHI_OK && !*model->footer.get());
    // Use a fresh screen so this longer replacement must allocate, rather than
    // reusing capacity left by the exact-limit test above.
    TSUCHI_Screen allocation{};
    assert(ui.create(owner, "Allocation", &allocation) == TSUCHI_OK);
    assert(ui.add_button(owner, allocation, "go", "Go") == TSUCHI_OK);
    assert(ui.set_footer(owner, allocation, "Old") == TSUCHI_OK);
    assert(ui.hold_settings_tab(owner, allocation) == TSUCHI_OK);
    const UiScreen* allocating = ui_settings_tab(ui_settings_tab_count() - 1); assert(allocating && allocating->id == allocation.id);
    revision = allocating->revision; fail_allocation = true;
    const TSUCHI_Result failed = ui.set_footer(owner, allocation, footer.c_str());
    fail_allocation = false;
    assert(failed == TSUCHI_NO_MEMORY && allocating->revision == revision && !std::strcmp(allocating->footer.get(), "Old"));
    watched_release = allocating->footer.data; watched_releases = 0;
    assert(ui.destroy(owner, allocation) == TSUCHI_OK && watched_releases == 1); watched_release = nullptr;
    frame(); assert(events == 0 && ui_active_view() == view && opens == before_opens && closes == before_closes);
    assert(ui.release_open(owner, screen) == TSUCHI_OK);
    revision = model->revision; const uint32_t controls = model->controls.count;
    for (const char* bad : invalid) {
        assert(ui.add_button(owner, screen, "bad", bad) == TSUCHI_INVALID);
        assert(ui.add_integer(owner, screen, "bad", bad, 0, 1, 1, 0) == TSUCHI_INVALID);
        assert(ui.add_text(owner, screen, "bad", "Message", bad) == TSUCHI_INVALID);
        const std::string bad_options = "[{\"id\":\"a\",\"label\":" + json_text(bad) + "},{\"id\":\"b\",\"label\":\"B\"}]";
        assert(ui.add_choice(owner, screen, "bad", "Choice", bad_options.c_str(), "a") == TSUCHI_INVALID);
        assert(model->revision == revision && model->controls.count == controls);
    }

    // JSON uses the same copied model and strict text rules as code builders.
    const std::string rows = "\"controls\":[{\"id\":\"go\",\"kind\":\"button\",\"label\":" + json_text(label) +
        "},{\"id\":\"choice\",\"kind\":\"choice\",\"label\":" + json_text(label) + ",\"options\":" + choices +
        ",\"initial\":\"first\"},{\"id\":\"message\",\"kind\":\"text\",\"label\":" + json_text(label) + ",\"initial\":" + json_text(message) + "}]";
    file = "{\"title\":\"JSON\",\"footer\":" + json_text(footer) + ',' + rows + '}';
    TSUCHI_Screen data{}; assert(ui.load(owner, "screen.json", &data) == TSUCHI_OK);
    file.assign(file.size(), '!'); assert(ui.hold_open(owner, data) == TSUCHI_OK);
    model = ui_active_screen(); assert(model && model->controls.count == 3);
    assert(!std::strcmp(model->footer.get(), footer.c_str()) && !std::strcmp(model->controls[0].label.get(), label.c_str()));
    assert(!std::strcmp(model->controls[1].labels.first(), label.c_str()) && !std::strcmp(model->controls[2].text.get(), message.c_str()));
    assert(ui.destroy(owner, data) == TSUCHI_OK);
    const auto reject_file = [&] {
        TSUCHI_Screen untouched{0xABCDEF};
        assert(ui.load(owner, "screen.json", &untouched) == TSUCHI_INVALID && untouched.id == 0xABCDEF);
    };
    for (const char* bad_footer : {"null", "12", "{}", "[]", "\"ok\\u0000[unsafe]\"", "\"[unsafe]\""}) {
        file = std::string("{\"title\":\"Bad\",\"footer\":") + bad_footer + ',' + rows + '}'; reject_file();
    }
    for (const char* bad : invalid) {
        file = "{\"title\":\"Bad\",\"footer\":" + json_text(bad) + ',' + rows + '}'; reject_file();
    }
    file = "{\"title\":\"Bad\",\"footers\":\"Typo\"," + rows + '}'; reject_file();
    file = "{\"title\":\"Bad\",\"footer\":\"A\",\"footer\":\"B\"," + rows + '}'; reject_file();
    file = "{\"title\":\"Bad\",\"footer\":" + json_text(maximum + 'x') + ',' + rows + '}'; reject_file();
    file = "{\"title\":\"Bad\",\"footer\":\"Copied before invalid row\",\"controls\":[{\"id\":\"go\",\"kind\":\"button\",\"label\":\"Go\"},{\"id\":\"bad\",\"kind\":\"text\",\"label\":\"Bad\",\"initial\":\"[unsafe]\"}]}"; reject_file();
    file = "{\"title\":\"Bad\",\"controls\":[{\"id\":\"go\",\"kind\":\"button\",\"label\":\"Go\",\"unknown\":true}]}"; reject_file();
    file = "{\"title\":\"Bad\",\"controls\":[{\"id\":\"choice\",\"kind\":\"choice\",\"label\":\"Choice\",\"initial\":\"a\",\"options\":[{\"id\":\"a\",\"label\":\"A\",\"unknown\":true},{\"id\":\"b\",\"label\":\"B\"}]}]}"; reject_file();
    for (bool explicit_empty : {false, true}) {
        file = std::string("{\"title\":\"Automatic\",") + (explicit_empty ? "\"footer\":\"\"," : "") + rows + '}';
        assert(ui.load(owner, "screen.json", &data) == TSUCHI_OK && ui.hold_open(owner, data) == TSUCHI_OK);
        assert(!*ui_active_screen()->footer.get()); assert(ui.destroy(owner, data) == TSUCHI_OK);
    }
    frame(); assert(events == 0);
    assert(ui.destroy(owner, screen) == TSUCHI_OK);
    assert(ui.set_footer(owner, screen, "Ended") == TSUCHI_NOT_FOUND);
    assert(ui.set_footer(other, screen, "Ended") == TSUCHI_NOT_FOUND);
    assert(ui.create(owner, "Stop cleanup", &screen) == TSUCHI_OK && ui.add_button(owner, screen, "go", "Go") == TSUCHI_OK);
    assert(ui.set_footer(owner, screen, footer.c_str()) == TSUCHI_OK && ui.hold_settings_tab(owner, screen) == TSUCHI_OK);
    watched_release = ui_settings_tab(ui_settings_tab_count() - 1)->footer.data; watched_releases = 0;
    mod_stop(owner); assert(watched_releases == 1); watched_release = nullptr;
    mod_stop(other); frame();
}
}

// The real Showing/HoldTable operation is used here; no mock immediate close
// or permissive recursive open can conceal a lifetime/admission failure.
namespace {
enum class ScheduleMode { normal, fail, recursive, destroy, stop, host_lost, release_reopen };
struct ScheduleView { bool live = false, attached = false, updating = false; } scheduled_view;
TSUCHI_Mod* schedule_owner = nullptr;
TSUCHI_Screen schedule_screen{};
ScheduleMode schedule_mode = ScheduleMode::normal;
unsigned schedule_opens = 0, schedule_updates = 0, schedule_closes = 0, schedule_detaches = 0, schedule_closed = 0;
unsigned schedule_epoch = 0, observed_epoch = 0;
std::string scheduled_message, scheduled_footer;
std::vector<std::string> schedule_events;
uint32_t scheduled_selection = 0, scheduled_choice = 0;
int64_t scheduled_integer = 0;
constexpr uint64_t schedule_host = 333;
void advance_schedule_epoch() { ++schedule_epoch; }
TSUCHI_Result schedule_open(const UiScreen&, void** view) {
    assert(!scheduled_view.live); scheduled_view.live = true; scheduled_view.attached = true;
    ++schedule_opens; *view = &scheduled_view; return TSUCHI_OK;
}
bool schedule_live(void* view) { assert(view == &scheduled_view && scheduled_view.live); return true; }
void schedule_close(void* view) {
    assert(view == &scheduled_view && scheduled_view.live && !scheduled_view.updating);
    scheduled_view.live = false; ++schedule_closes;
}
bool schedule_uses_host(void* view, uint64_t host) { assert(view == &scheduled_view && scheduled_view.live); return host == schedule_host; }
void schedule_detach(void* view, uint64_t host) {
    assert(view == &scheduled_view && scheduled_view.live && scheduled_view.attached && host == schedule_host);
    scheduled_view.attached = false; ++schedule_detaches;
}
bool schedule_update(void* view, const UiScreen& screen) {
    assert(view == &scheduled_view && scheduled_view.live && !scheduled_view.updating);
    assert(ui_active_screen() == &screen && ui_active_view() == view && screen.id == schedule_screen.id);
    scheduled_view.updating = true; ++schedule_updates; observed_epoch = schedule_epoch;
    scheduled_message = screen.controls[0].text.get(); scheduled_footer = screen.footer.get();
    scheduled_selection = screen.selected; scheduled_choice = screen.controls[2].selected; scheduled_integer = screen.controls[3].integer;
    const ScheduleMode mode = schedule_mode; schedule_mode = ScheduleMode::normal;
    const unsigned before_close = schedule_closes, before_update = schedule_updates;
    if (mode == ScheduleMode::recursive) {
        assert(!ui_sync_screen(schedule_screen.id, view));
        assert(ui::table.hold_open(schedule_owner, schedule_screen) == TSUCHI_BUSY);
        assert(schedule_updates == before_update && schedule_closes == before_close && ui_active_view() == view);
    } else if (mode == ScheduleMode::destroy) {
        assert(ui::table.destroy(schedule_owner, schedule_screen) == TSUCHI_OK);
    } else if (mode == ScheduleMode::stop) mod_stop(schedule_owner);
    else if (mode == ScheduleMode::host_lost) {
        const unsigned before_detach = schedule_detaches;
        ui_host_lost(schedule_host);
        assert(schedule_detaches == before_detach + 1 && !scheduled_view.attached);
    } else if (mode == ScheduleMode::release_reopen) {
        assert(ui::table.release_open(schedule_owner, schedule_screen) == TSUCHI_OK);
        // Even after record removal the active operation owns this view until
        // return. A same-screen reopen must not reuse it in this callback.
        assert(ui::table.hold_open(schedule_owner, schedule_screen) == TSUCHI_BUSY);
    }
    assert(scheduled_view.live && schedule_closes == before_close);
    scheduled_view.updating = false;
    return mode != ScheduleMode::fail;
}
const UiRenderer schedule_renderer{schedule_open, schedule_update, schedule_live, schedule_close,
    nullptr, nullptr, schedule_uses_host, schedule_detach};
void make_schedule_screen(const char* owner_id) {
    schedule_owner = mod_create(owner_id); assert(schedule_owner);
    assert(ui::table.create(schedule_owner, "Scheduled", &schedule_screen) == TSUCHI_OK);
    assert(ui::table.add_text(schedule_owner, schedule_screen, "status", "Status", "Ready") == TSUCHI_OK);
    assert(ui::table.add_button(schedule_owner, schedule_screen, "go", "Go") == TSUCHI_OK);
    assert(ui::table.add_choice(schedule_owner, schedule_screen, "choice", "Choice",
        R"([{"id":"off","label":"Off"},{"id":"on","label":"On"}])", "off") == TSUCHI_OK);
    assert(ui::table.add_integer(schedule_owner, schedule_screen, "number", "Number", 0, 10, 1, 0) == TSUCHI_OK);
    assert(ui::table.set_footer(schedule_owner, schedule_screen, "A Select B Back") == TSUCHI_OK);
    TSUCHI_Subscription subscription{};
    assert(ui::table.on_closed(schedule_owner, [](void*, const TSUCHI_UiClosed*) { ++schedule_closed; }, nullptr, &subscription) == TSUCHI_OK);
    assert(ui::table.on_activated(schedule_owner, [](void*, const TSUCHI_UiActivated* event) {
        assert(event->screen.id == schedule_screen.id && !std::strcmp(event->control, "go")); schedule_events.push_back("activated");
    }, nullptr, &subscription) == TSUCHI_OK);
    assert(ui::table.on_choice_changed(schedule_owner, [](void*, const TSUCHI_UiChoiceChanged* event) {
        assert(event->screen.id == schedule_screen.id && !std::strcmp(event->control, "choice") &&
            !std::strcmp(event->previous, "off") && !std::strcmp(event->choice, "on")); schedule_events.push_back("choice");
    }, nullptr, &subscription) == TSUCHI_OK);
    assert(ui::table.on_integer_changed(schedule_owner, [](void*, const TSUCHI_UiIntegerChanged* event) {
        assert(event->screen.id == schedule_screen.id && !std::strcmp(event->control, "number") && event->previous == 0 && event->integer == 1);
        schedule_events.push_back("integer");
    }, nullptr, &subscription) == TSUCHI_OK);
    assert(ui::table.on_changed(schedule_owner, [](void*, const TSUCHI_UiChanged* event) {
        assert(event->screen.id == schedule_screen.id);
        assert((event->kind == 1 && !std::strcmp(event->control, "choice")) || (event->kind == 2 && !std::strcmp(event->control, "number")));
        schedule_events.push_back(event->kind == 1 ? "changed-choice" : "changed-integer");
    }, nullptr, &subscription) == TSUCHI_OK);
    assert(ui::table.on_reset_requested(schedule_owner, [](void*, const TSUCHI_UiResetRequested* event) {
        assert(event->screen.id == schedule_screen.id); schedule_events.push_back("reset");
    }, nullptr, &subscription) == TSUCHI_OK);
}
void scheduling_tests() {
    assert(!ui_active_view() && fail_engine_registration && engine_steps.empty());
    make_schedule_screen("garden.schedule"); ui_set_renderer(&schedule_renderer);
    assert(engine_registration_attempts && engine_steps.empty());

    // A rejected initial update owns cleanup but never announces an accepted
    // open through the existing closed channel (and invents no opened event).
    schedule_mode = ScheduleMode::fail;
    const unsigned rejected_closed = schedule_closed, rejected_close = schedule_closes;
    assert(ui::table.hold_open(schedule_owner, schedule_screen) == TSUCHI_NOT_READY);
    assert(!ui_active_screen() && !scheduled_view.live && schedule_closes == rejected_close + 1);
    frame(); assert(schedule_closed == rejected_closed);

    const unsigned initial_updates = schedule_updates;
    schedule_mode = ScheduleMode::recursive;
    assert(ui::table.hold_open(schedule_owner, schedule_screen) == TSUCHI_OK);
    assert(schedule_updates == initial_updates + 1 && ui_active_view() == &scheduled_view);
    assert(scheduled_message == "Ready" && scheduled_footer == "A Select B Back" && ui_active_screen()->selected == 1);
    // Engine registration failure preserves programmatic world-step updates.
    assert(ui::table.set_text(schedule_owner, schedule_screen, "status", "World fallback") == TSUCHI_OK);
    const unsigned before_fallback = schedule_updates;
    engine_frame(); assert(schedule_updates == before_fallback);
    world_frame(); assert(schedule_updates == before_fallback + 1 && scheduled_message == "World fallback");
    schedule_mode = ScheduleMode::recursive;
    assert(ui_sync_screen(schedule_screen.id, &scheduled_view));
    assert(scheduled_view.live && ui_active_view() == &scheduled_view);

    // A rejected update of an accepted view ends exactly its generation and
    // reports one close, rather than leaving a failed fresh=false hold alive.
    schedule_mode = ScheduleMode::fail;
    const unsigned accepted_close = schedule_closes, accepted_closed = schedule_closed;
    assert(!ui_sync_screen(schedule_screen.id, &scheduled_view));
    assert(!ui_active_view() && !scheduled_view.live && schedule_closes == accepted_close + 1);
    frame(); frame(); assert(schedule_closed == accepted_closed + 1 && schedule_closes == accepted_close + 1);

    // Release during the retained update defers close; once it unwinds the
    // same opaque address can be reused and pending work cannot close it again.
    assert(ui::table.hold_open(schedule_owner, schedule_screen) == TSUCHI_OK);
    schedule_mode = ScheduleMode::release_reopen;
    const unsigned reused_close = schedule_closes;
    assert(!ui_sync_screen(schedule_screen.id, &scheduled_view));
    assert(!ui_active_view() && schedule_closes == reused_close + 1);
    assert(ui::table.hold_open(schedule_owner, schedule_screen) == TSUCHI_OK && ui_active_view() == &scheduled_view);
    frame(); assert(schedule_closes == reused_close + 1 && scheduled_view.live);
    assert(ui::table.release_open(schedule_owner, schedule_screen) == TSUCHI_OK); frame();

    // An initial host loss detaches arrays immediately but leaves the view
    // alive until the active Showing update returns, and emits no closed notice.
    schedule_mode = ScheduleMode::host_lost;
    const unsigned lost_closed = schedule_closed, lost_close = schedule_closes;
    assert(ui::table.hold_open(schedule_owner, schedule_screen) == TSUCHI_NOT_READY);
    assert(!ui_active_view() && !scheduled_view.live && schedule_closes == lost_close + 1);
    frame(); assert(schedule_closed == lost_closed);
    schedule_mode = ScheduleMode::destroy;
    assert(ui::table.hold_open(schedule_owner, schedule_screen) == TSUCHI_NOT_READY);
    frame(); assert(schedule_closed == lost_closed && !ui_active_view()); mod_stop(schedule_owner);

    make_schedule_screen("garden.schedule-stop");
    schedule_mode = ScheduleMode::stop;
    const unsigned stopped_close = schedule_closes;
    assert(ui::table.hold_open(schedule_owner, schedule_screen) == TSUCHI_NOT_READY);
    assert(!scheduled_view.live && schedule_closes == stopped_close + 1 && !mod_usable(schedule_owner));
    frame(); assert(schedule_closed == lost_closed);

    make_schedule_screen("garden.schedule-engine");
    // Match production order: native epoch first, then the UI listener installed
    // by renderer publication. A successful retry transfers maintenance once.
    fail_engine_registration = false;
    assert(each_engine_frame(advance_schedule_epoch));
    const unsigned before_registration = engine_registration_attempts;
    ui_set_renderer(&schedule_renderer);
    assert(engine_registration_attempts == before_registration + 1 && engine_steps.size() == 2);
    ui_set_renderer(&schedule_renderer);
    assert(engine_registration_attempts == before_registration + 1 && engine_steps.size() == 2);
    assert(ui::table.hold_open(schedule_owner, schedule_screen) == TSUCHI_OK);
    assert(ui::table.set_text(schedule_owner, schedule_screen, "status", "Engine only") == TSUCHI_OK);
    const unsigned engine_update = schedule_updates;
    world_frame(); assert(schedule_updates == engine_update && scheduled_message == "Ready");
    engine_frame(); assert(schedule_updates == engine_update + 1 && scheduled_message == "Engine only" && observed_epoch == schedule_epoch && schedule_epoch == 1);
    world_frame(); assert(schedule_updates == engine_update + 1);
    assert(!ui_sync_screen(schedule_screen.id + 1, &scheduled_view));
    assert(!ui_sync_screen(schedule_screen.id, &first_view));
    assert(schedule_updates == engine_update + 1 && scheduled_view.live);
    // Visual sync is independent of world steps, but the public UI event
    // contract is not changed: notices remain queued until the next world step.
    assert(schedule_events.empty() && ui_active_screen()->selected == 1);
    assert(ui_action(4) && ui_action(1) && ui_action(3) && ui_action(1) && ui_action(3) && ui_action(7));
    assert(ui_sync_screen(schedule_screen.id, &scheduled_view));
    assert(scheduled_selection == 3 && scheduled_choice == 1 && scheduled_integer == 1 && schedule_events.empty());
    const unsigned before_notice_engine = schedule_updates;
    engine_frame(); assert(schedule_updates == before_notice_engine + 1 && schedule_events.empty());
    world_frame();
    const std::vector<std::string> expected_events{"activated", "choice", "changed-choice", "integer", "changed-integer", "reset"};
    assert(schedule_events == expected_events && schedule_updates == before_notice_engine + 1);
    engine_frame(); world_frame(); assert(schedule_events == expected_events);
    schedule_mode = ScheduleMode::fail;
    const unsigned engine_close = schedule_closes, engine_closed = schedule_closed;
    engine_frame(); assert(!ui_active_view() && schedule_closes == engine_close + 1);
    engine_frame(); world_frame(); assert(schedule_closes == engine_close + 1 && schedule_closed == engine_closed + 1);
    assert(ui::table.destroy(schedule_owner, schedule_screen) == TSUCHI_OK); mod_stop(schedule_owner);
    assert(schedule_opens == schedule_closes);
}
} // namespace

int main() {
    set_allocator({test_allocate, test_release});
    set_thread_probe([]{return thread;});note_game_thread();ui_start();ui_set_renderer(&renderer);
    TSUCHI_Mod* owner=mod_create("garden.menu"),*other=mod_create("garden.other");assert(owner&&other);
    const auto& ui=ui::table;TSUCHI_Screen screen{},second{};TSUCHI_Subscription a{},b{},c{};
    assert(ui.on_activated(owner,[](void*,const TSUCHI_UiActivated* event){assert(!std::strcmp(event->control,"go"));++activated;},nullptr,&a)==TSUCHI_OK);
    assert(ui.on_changed(owner,[](void*,const TSUCHI_UiChanged*){++changed;},nullptr,&b)==TSUCHI_OK);
    assert(ui.on_closed(owner,[](void*,const TSUCHI_UiClosed*){++closed;},nullptr,&c)==TSUCHI_OK);
    TSUCHI_Subscription foreign{};
    assert(ui.on_activated(other,[](void*,const TSUCHI_UiActivated*){++foreign_events;},nullptr,&foreign)==TSUCHI_OK);
    assert(ui.on_changed(other,[](void*,const TSUCHI_UiChanged*){++foreign_events;},nullptr,&foreign)==TSUCHI_OK);
    assert(ui.on_closed(other,[](void*,const TSUCHI_UiClosed*){++foreign_events;},nullptr,&foreign)==TSUCHI_OK);
    assert(ui.create(owner,"Garden",&screen)==TSUCHI_OK);
    assert(ui.create(owner,"[markup]",&second)==TSUCHI_INVALID);
    assert(ui.add_button(owner,screen,"go","Go")==TSUCHI_OK);
    const char* options=R"([{"id":"normal","label":"Normal"},{"id":"hard","label":"Hard"}])";
    assert(ui.add_choice(owner,screen,"difficulty","Difficulty",options,"normal")==TSUCHI_OK);
    assert(ui.add_integer(owner,screen,"count","Count",-10,10,2,0)==TSUCHI_OK);
    assert(ui.add_text(owner,screen,"note","Note","Ready")==TSUCHI_OK);
    assert(ui.add_button(owner,screen,"go","Duplicate")==TSUCHI_INVALID);
    assert(ui.add_integer(owner,screen,"bad","Bad",0,10,0,0)==TSUCHI_INVALID);
    assert(ui.add_choice(owner,screen,"bad","Bad",options,"missing")==TSUCHI_NOT_FOUND);
    assert(ui.add_choice(owner,screen,"bad","Bad",R"([{"id":"same","label":"A"},{"id":"same","label":"B"}])","same")==TSUCHI_INVALID);
    assert(ui.add_button(other,screen,"foreign","Foreign")==TSUCHI_DENIED);
    int64_t number=0;char buffer[128];uint64_t needed=0;bool opened=false;
    assert(ui.choice(owner,screen,"difficulty",buffer,sizeof buffer,&needed)==TSUCHI_OK&&!std::strcmp(buffer,"normal"));
    assert(ui.choice(owner,screen,"difficulty",buffer,1,&needed)==TSUCHI_CAPACITY&&needed==7);
    assert(ui.set_choice(owner,screen,"difficulty","hard")==TSUCHI_OK);
    assert(ui.set_choice(owner,screen,"difficulty","missing")==TSUCHI_NOT_FOUND);
    assert(ui.integer(owner,screen,"count",&number)==TSUCHI_OK&&number==0);
    assert(ui.set_integer(owner,screen,"count",2)==TSUCHI_OK&&ui.set_integer(owner,screen,"count",3)==TSUCHI_INVALID);
    assert(ui.set_text(owner,screen,"note","Done")==TSUCHI_OK);
    assert(ui.text(owner,screen,"note",buffer,sizeof buffer,&needed)==TSUCHI_OK&&!std::strcmp(buffer,"Done"));
    assert(ui.set_text(owner,screen,"note","[unsafe]")==TSUCHI_INVALID);
    assert(ui.open(owner,screen,&opened)==TSUCHI_OK&&!opened);
    fail_open=true;assert(ui.hold_open(owner,screen)==TSUCHI_NOT_READY&&closes==0);fail_open=false;
    assert(ui.hold_open(owner,screen)==TSUCHI_OK&&ui.open(owner,screen,&opened)==TSUCHI_OK&&opened);
    assert(ui.add_button(owner,screen,"late","Late")==TSUCHI_BUSY);
    assert(ui.create(other,"Other",&second)==TSUCHI_OK&&ui.add_button(other,second,"go","Go")==TSUCHI_OK);
    assert(ui.hold_open(other,second)==TSUCHI_BUSY);
    assert(ui_action(4)&&activated==0);frame();assert(activated==1);
    assert(ui_action(1)&&ui_action(2));frame();assert(changed==1);
    assert(ui_action(1)&&ui_action(3));frame();assert(changed==2&&ui.integer(owner,screen,"count",&number)==TSUCHI_OK&&number==4);
    assert(ui.release_open(other,screen)==TSUCHI_DENIED);
    assert(ui.release_open(owner,screen)==TSUCHI_OK&&closes==1&&closed==0);frame();assert(closed==1);
    assert(ui.release_open(owner,screen)==TSUCHI_NOT_FOUND);
    assert(ui.hold_open(owner,screen)==TSUCHI_OK);host_live=false;frame();assert(closes==2&&closed==2);host_live=true;
    assert(ui.hold_open(owner,screen)==TSUCHI_OK);fail_update=true;frame();assert(closes==3&&closed==3);fail_update=false;
    // File and code forms publish the same model to the same renderer.
    file=R"({"title":"Garden","controls":[{"id":"go","kind":"button","label":"Go"},{"id":"difficulty","kind":"choice","label":"Difficulty","options":[{"id":"normal","label":"Normal"},{"id":"hard","label":"Hard"}],"initial":"normal"},{"id":"count","kind":"integer","label":"Count","minimum":-10,"maximum":10,"step":2,"initial":0},{"id":"note","kind":"text","label":"Note","initial":"Ready"}]})";
    TSUCHI_Screen data{};assert(ui.load(owner,"screen.json",&data)==TSUCHI_OK);
    assert(ui.hold_open(owner,data)==TSUCHI_OK);frame();assert(shown=="Garden|Go|Difficulty|Count|Note");
    assert(ui.destroy(owner,data)==TSUCHI_OK&&closes==4);frame();assert(closed==4);
    assert(ui.load(owner,"missing.json",&data)==TSUCHI_NOT_FOUND);
    file=R"({"title":"Wrong","controls":[{"id":"bad","kind":"integer","label":"Bad","minimum":0,"maximum":1,"step":0,"initial":0}]})";
    assert(ui.load(owner,"screen.json",&data)==TSUCHI_INVALID);
    thread=2;assert(ui.set_integer(owner,screen,"count",4)==TSUCHI_INVALID&&ui.hold_open(owner,screen)==TSUCHI_INVALID);thread=1;
    const uint32_t before_tabs = ui_settings_tab_revision();
    assert(ui.hold_settings_tab(other,screen)==TSUCHI_DENIED);
    assert(ui.hold_settings_tab(owner,screen)==TSUCHI_OK && ui_settings_tab_count()==1);
    assert(ui_settings_tab_revision()!=before_tabs && ui_settings_tab(0)->id==screen.id);
    assert(ui.release_settings_tab(owner,screen)==TSUCHI_OK && ui_settings_tab_count()==0);
    assert(ui.hold_settings_tab(owner,screen)==TSUCHI_OK);
    assert(ui.hold_open(owner,screen)==TSUCHI_OK);mod_stop(owner);
    assert(ui_settings_tab_count()==0 && !ui_settings_tab(0));frame();assert(closes==5&&closed==4);
    assert(ui.integer(other,screen,"count",&number)==TSUCHI_NOT_FOUND);
    assert(foreign_events==0);
    assert(ui.destroy(other,second)==TSUCHI_OK);mod_stop(other);

    // Read-only text is never selected. Selection is an absolute model index,
    // including controls beyond the old six-row boundary.
    TSUCHI_Mod* focus_owner = mod_create("garden.focus"), *watcher = mod_create("garden.watcher"); assert(focus_owner && watcher);
    TSUCHI_Screen focus_screen{}, passive{}; Events own_events{}, other_events{}; TSUCHI_Subscription subscribed{};
    const auto reset_event = [](void* context, const TSUCHI_UiResetRequested* event) {
        auto& events = *static_cast<Events*>(context); ++events.resets; events.reset_screen = event->screen.id;
    };
    assert(ui.on_reset_requested(focus_owner, reset_event, &own_events, &subscribed) == TSUCHI_OK);
    assert(ui.on_reset_requested(watcher, reset_event, &other_events, &subscribed) == TSUCHI_OK);
    assert(ui.on_activated(focus_owner, [](void* context, const TSUCHI_UiActivated* event) {
        auto& events = *static_cast<Events*>(context); ++events.activated; events.control = event->control;
    }, &own_events, &subscribed) == TSUCHI_OK);
    assert(ui.create(focus_owner, "Focus", &focus_screen) == TSUCHI_OK);
    assert(ui.add_text(focus_owner, focus_screen, "heading", "Heading", "Read only") == TSUCHI_OK);
    assert(ui.add_button(focus_owner, focus_screen, "first", "First") == TSUCHI_OK);
    assert(ui.add_text(focus_owner, focus_screen, "middle", "Middle", "Read only") == TSUCHI_OK);
    assert(ui.add_choice(focus_owner, focus_screen, "choice", "Choice", options, "hard") == TSUCHI_OK);
    assert(ui.add_text(focus_owner, focus_screen, "description", "Description", "Read only") == TSUCHI_OK);
    assert(ui.add_integer(focus_owner, focus_screen, "integer", "Integer", 0, 10, 1, 5) == TSUCHI_OK);
    assert(ui.add_text(focus_owner, focus_screen, "more", "More", "Read only") == TSUCHI_OK);
    assert(ui.add_button(focus_owner, focus_screen, "last", "Last") == TSUCHI_OK);
    assert(ui.add_text(focus_owner, focus_screen, "tail", "Tail", "Read only") == TSUCHI_OK);
    assert(ui.hold_open(focus_owner, focus_screen) == TSUCHI_OK);
    assert(ui_active_screen()->selected == 1 && ui_active_screen()->controls.count == 9);
    for (uint32_t selected : {3u, 5u, 7u, 1u}) { assert(ui_action(1)); assert(ui_active_screen()->selected == selected); }
    for (uint32_t selected : {7u, 5u, 3u, 1u}) { assert(ui_action(0)); assert(ui_active_screen()->selected == selected); }
    assert(ui_route_action(99, 4)); assert(own_events.activated == 0); frame();
    assert(own_events.activated == 1 && own_events.control == "first");
    assert(ui_action(7) && own_events.resets == 0); frame();
    assert(own_events.resets == 1 && own_events.reset_screen == focus_screen.id && other_events.resets == 0);
    assert(ui.choice(focus_owner, focus_screen, "choice", buffer, sizeof buffer, &needed) == TSUCHI_OK && !std::strcmp(buffer, "hard"));
    assert(ui.integer(focus_owner, focus_screen, "integer", &number) == TSUCHI_OK && number == 5);
    assert(ui_action(5) && !ui_active_screen()); frame();
    assert(ui.create(focus_owner, "Information", &passive) == TSUCHI_OK);
    assert(ui.add_text(focus_owner, passive, "one", "One", "Read only") == TSUCHI_OK);
    assert(ui.add_text(focus_owner, passive, "two", "Two", "Read only") == TSUCHI_OK);
    assert(ui.hold_open(focus_owner, passive) == TSUCHI_OK && ui_active_screen()->selected == kUiNoSelection);
    const uint32_t passive_revision = ui_active_screen()->revision;
    for (uint32_t action : {0u, 1u, 2u, 3u, 4u}) assert(ui_action(action));
    assert(ui_active_screen()->selected == kUiNoSelection && ui_active_screen()->revision == passive_revision); frame();
    assert(own_events.activated == 1);
    assert(ui_action(7) && own_events.resets == 1); frame();
    assert(own_events.resets == 2 && own_events.reset_screen == passive.id && other_events.resets == 0);
    assert(ui_action(7)); mod_stop(focus_owner); frame();
    assert(!ui_active_screen() && own_events.resets == 2 && other_events.resets == 0); mod_stop(watcher);

    // Each view owns the renderer callbacks it was opened with, independently
    // of a later default and of the lifetime of that default's source struct.
    first_renderer = {open_first, update_first, live_first, close_first, action_first, buttons_first, uses_first_host, detach_first_host};
    second_renderer = {open_second, update_second, live_second, close_second, action_second, buttons_second, uses_second_host, detach_second_host};
    routing_owner = mod_create("garden.routing"); assert(routing_owner);
    assert(ui.create(routing_owner, "Routing", &routing_screen) == TSUCHI_OK);
    assert(ui.add_button(routing_owner, routing_screen, "go", "Go") == TSUCHI_OK);
    install_temporary_renderer(); swap_on_open = true;
    assert(ui.hold_open(routing_owner, routing_screen) == TSUCHI_OK && ui_active_view() == &first_view);
    assert(first_opens == 1 && second_opens == 0); frame();
    assert(first_updates == 2 && second_updates == 0);
    assert(ui_route_action(77, 7) && first_actions == 1 && second_actions == 0 && seen_host == 77 && seen_action == 7);
    ui_route_buttons(123); assert(first_buttons == 1 && second_buttons == 0 && seen_buttons == 123);
    thread = 2; assert(!ui_route_action(77, 4) && !ui_action(4)); ui_route_buttons(321); thread = 1;
    assert(first_actions == 1 && first_buttons == 1);
    close_on_action = true; assert(ui_route_action(77, 5));
    assert(!ui_active_view() && first_closes == 1 && !first_view.live); ui_route_buttons(456);
    assert(first_buttons == 1 && !ui_route_action(77, 4)); frame();
    assert(ui.hold_open(routing_owner, routing_screen) == TSUCHI_OK && ui_active_view() == &second_view); frame();
    assert(second_opens == 1 && second_updates == 2);
    assert(!ui_route_action(76, 3) && ui_route_action(77, 3) && second_actions == 2 && first_actions == 2);
    close_on_buttons = true; ui_route_buttons(456);
    assert(second_buttons == 1 && second_closes == 1 && seen_buttons == 456 && !ui_active_view()); frame();
    assert(ui.hold_open(routing_owner, routing_screen) == TSUCHI_OK && ui_active_view() == &first_view);
    close_on_live = true; const unsigned before_live_close = first_updates; frame();
    assert(!ui_active_view() && first_closes == 2 && first_updates == before_live_close);
    assert(ui.hold_open(routing_owner, routing_screen) == TSUCHI_OK && ui_active_view() == &first_view);
    stop_on_update = true; frame();
    assert(!ui_active_view() && !mod_usable(routing_owner) && first_closes == 3);
    assert(first_opens == first_closes && second_opens == second_closes);

    // Host loss belongs to the renderer captured by this opaque view. Changing
    // the default, including the temporary source struct, cannot reinterpret it.
    routing_owner = mod_create("garden.hostloss"); assert(routing_owner);
    assert(ui.create(routing_owner, "Host loss", &routing_screen) == TSUCHI_OK);
    assert(ui.add_button(routing_owner, routing_screen, "go", "Go") == TSUCHI_OK);
    install_temporary_renderer();
    assert(ui.hold_open(routing_owner, routing_screen) == TSUCHI_OK && ui_active_view() == &first_view);
    ui_set_renderer(&second_renderer);
    const unsigned original_first_closes = first_closes, original_second_closes = second_closes;
    ui_host_lost(second_host);
    assert(ui_active_view() == &first_view && first_view.attached && first_closes == original_first_closes);
    assert(first_host_queries == 1 && second_host_queries == 0 && first_detaches == 0 && second_detaches == 0);
    thread = 2; ui_host_lost(first_host); thread = 1;
    assert(first_host_queries == 1 && first_closes == original_first_closes);
    ui_host_lost(first_host);
    assert(!ui_active_view() && !first_view.live && !first_view.attached);
    assert(first_closes == original_first_closes + 1 && first_detaches == 1 && second_closes == original_second_closes && second_detaches == 0);
    ui_host_lost(first_host); assert(first_closes == original_first_closes + 1);

    // A renderer with no host adapter also keeps its arbitrary view untouched.
    UiRenderer unknown_host = first_renderer; unknown_host.uses_host = nullptr;
    ui_set_renderer(&unknown_host); assert(ui.hold_open(routing_owner, routing_screen) == TSUCHI_OK);
    ui_set_renderer(&second_renderer);
    const unsigned before_unknown_query = first_host_queries;
    ui_host_lost(first_host);
    assert(ui_active_view() == &first_view && first_view.attached && first_host_queries == before_unknown_query);
    assert(ui.release_open(routing_owner, routing_screen) == TSUCHI_OK);

    ui_set_renderer(&first_renderer); assert(ui.hold_open(routing_owner, routing_screen) == TSUCHI_OK);
    ui_set_renderer(&second_renderer);
    const unsigned before_update_close = first_closes, before_update_detach = first_detaches;
    lose_host_on_update = true; frame();
    assert(!lose_host_on_update && !in_first_update && !ui_active_view());
    assert(first_closes == before_update_close + 1 && first_detaches == before_update_detach + 1 && !first_view.live);
    assert(second_closes == original_second_closes && second_detaches == 0);

    // A callback may release its old Showing and open another screen instance
    // with the same screen ID. Neither stale query nor stale detach may release
    // the replacement record or call its renderer with the old opaque pointer.
    ui_set_renderer(&first_renderer); assert(ui.hold_open(routing_owner, routing_screen) == TSUCHI_OK);
    const unsigned before_reentrant_detach = first_detaches;
    replace_on_host_query = true; ui_host_lost(first_host);
    assert(!replace_on_host_query && ui_active_view() == &second_view && second_view.live && second_view.attached);
    assert(first_detaches == before_reentrant_detach && second_detaches == 0 && second_closes == original_second_closes);
    ui_host_lost(first_host); assert(ui_active_view() == &second_view && second_view.attached);
    ui_host_lost(second_host); assert(!ui_active_view() && second_detaches == 1 && second_closes == original_second_closes + 1);

    ui_set_renderer(&first_renderer); assert(ui.hold_open(routing_owner, routing_screen) == TSUCHI_OK);
    replace_on_detach = true; ui_host_lost(first_host);
    assert(!replace_on_detach && ui_active_view() == &second_view && second_view.live && second_view.attached);
    assert(first_detaches == before_reentrant_detach + 1 && second_detaches == 1 && second_closes == original_second_closes + 1);
    ui_host_lost(second_host); assert(!ui_active_view() && second_detaches == 2 && second_closes == original_second_closes + 2);

    // Reopening can reuse the same screen ID and opaque view address. Only the
    // exact Showing record distinguishes this fresh host from the lost one.
    ui_set_renderer(&first_renderer); assert(ui.hold_open(routing_owner, routing_screen) == TSUCHI_OK);
    const unsigned before_reuse_close = first_closes, before_reuse_detach = first_detaches;
    reuse_view_on_host_query = true; ui_host_lost(first_host);
    assert(!reuse_view_on_host_query && ui_active_view() == &first_view && first_view.live && first_view.attached);
    assert(first_bound_host == second_host && first_closes == before_reuse_close + 1 && first_detaches == before_reuse_detach);
    ui_host_lost(first_host); assert(ui_active_view() == &first_view && first_view.attached);
    ui_host_lost(second_host); assert(!ui_active_view() && first_closes == before_reuse_close + 2 && first_detaches == before_reuse_detach + 1);
    first_bound_host = first_host;
    assert(first_opens == first_closes && second_opens == second_closes);
    mod_stop(routing_owner);
    ui_set_renderer(&renderer);
    frame(); glyph_and_footer_tests();
    scheduling_tests();
}
