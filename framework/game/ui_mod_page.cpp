// The Mods page on its own, from the title screen.
//
// The title's Options menu opens Controls and the Gameplay Guide as pages of the pause menu: it starts the
// pause menu's presenter and hands it the page and what to call once the page has closed. Its Settings entry,
// which the game hides, opens the Settings page the same way. This opens that page from the title menu itself.
// The list of mods then shows in it alone (ui_mod_list.cpp), and the title menu takes no presses until the
// presenter has called back.
#include "ui_mod_page.h"
#include "ui_mod_list.h"
#include "ui_screens.h"
#include "ui_title.h"
#include "frame.h"
#include "game.h"
#include "mods.h"
#include "actors.h"
#include "objects.h"
#include "reflect.h"
#include "registry.h"
#include "shared.h"
#include <string.h>

namespace tsuchi {
namespace {
namespace t = game::table;

TSUCHI_Mod* framework_mod = nullptr;
bool alone = false;
uint64_t presenter = 0;   // the presenter showing the page, as a handle
game::ClassRef presenter_class{t::name::game_menu_presenter_class};

// What the presenter calls once its page has closed, as the game's own code passes one: an object that starts
// with a table of functions. The presenter keeps a copy that `copy` makes, and frees it with `drop`.
struct Closed { const void* table; uint64_t unused; };
extern const void* const closed_table[7];
void closed_end(Closed*) {}
void closed_drop(Closed* self) { game::release_engine_memory(self); }
Closed* closed_copy(const Closed*) {
    auto* made = static_cast<Closed*>(game::engine_memory(sizeof(Closed)));
    if (made) *made = {closed_table, 0};
    return made;
}
void closed_copy_to(const Closed*, Closed* out) { *out = {closed_table, 0}; }
void closed_call(Closed*) { alone = false; presenter = 0; }
// In the game's order: end, end and free, copy, copy to a place, end, end and free, call.
const void* const closed_table[7] = {
    reinterpret_cast<const void*>(&closed_end), reinterpret_cast<const void*>(&closed_drop),
    reinterpret_cast<const void*>(&closed_copy), reinterpret_cast<const void*>(&closed_copy_to),
    reinterpret_cast<const void*>(&closed_end), reinterpret_cast<const void*>(&closed_drop),
    reinterpret_cast<const void*>(&closed_call)};
Closed closed_source{closed_table, 0};
// The game's holder of such an object: room for a small one, which this does not use, and the object.
struct Holder { uint64_t room[4]; Closed* object; uint64_t unused; };

// The page's title: the pause menu's header says Settings, and on its own the page is Mods. The game writes
// its own title as the page moves in, so this is written again over the page's first frames.
constexpr uint32_t kTitleFrames = 45;
uint32_t titled = 0;
void title() {
    const uint64_t object = game::from_handle(presenter);
    const uint64_t view = object ? game::read_or<uint64_t>(object + t::game_menu_presenter::view) : 0;
    const uint64_t header = game::live(view) ? game::member_object(view, t::name::host_page_header) : 0;
    const uint64_t block = game::live(header) ? game::member_object(header, t::name::host_header_text) : 0;
    if (!game::live(block) || !t::bound(t::fn::text_from_string) || !t::bound(t::fn::rich_text_set)) return;
    char16_t units[8]; size_t count = 0;
    if (!game::utf8_to_units("Mods", units, 7, count)) return;
    units[count] = 0;
    alignas(8) uint8_t string[t::array::size]{};
    const uint64_t data = reinterpret_cast<uint64_t>(units); const int32_t length = int32_t(count + 1);
    memcpy(string + t::array::data, &data, 8); memcpy(string + t::array::count, &length, 4); memcpy(string + t::array::max, &length, 4);
    game::EngineText made = t::fn::text_from_string(string);
    t::fn::rich_text_set(reinterpret_cast<void*>(block), &made);
    game::drop_shared(made.counter());
}
// A presenter that went away without calling back, as when the title screen itself ends.
void step() {
    if (alone && (!game::from_handle(presenter) || !mod_usable(framework_mod))) { alone = false; presenter = 0; }
    if (alone && titled < kTitleFrames) { ++titled; title(); }
}
bool covered() { return alone || ui_menu_input_held(); }
} // namespace

bool ui_mod_page_alone() { return alone; }
namespace {
// Why the page did not open, in the log.
TSUCHI_Result refused(const char* why) {
    char line[160] = "Mods page not opened: ";
    const size_t used = strlen(line), size = strlen(why);
    if (used + size < sizeof line) memcpy(line + used, why, size + 1);
    mod_note(kFrameworkLogId, line);
    return TSUCHI_NOT_READY;
}
} // namespace
bool ui_mod_page_supported() {
    return framework_mod && t::bound(t::fn::ui_presenter_find) && t::bound(t::fn::presenter_begin) && t::bound(t::fn::game_menu_open_page);
}

TSUCHI_Result ui_mod_page_open() {
    if (!ui_mod_page_supported()) return TSUCHI_UNSUPPORTED;
    if (!mod_usable(framework_mod) || !on_game_thread()) return TSUCHI_NOT_READY;
    if (alone) return TSUCHI_OK;
    if (!ui_title_menu_ready()) return TSUCHI_NOT_READY;   // the usual answer away from the title menu: no note
    if (ui_active_screen()) return TSUCHI_BUSY;
    const uint64_t framework = game::game_framework();
    const uint64_t screens = framework ? game::read_or<uint64_t>(framework + t::game_framework::ui) : 0;
    if (!game::live(screens)) return refused("the game's screens are not there");
    const uint64_t object = reinterpret_cast<uint64_t>(t::fn::ui_presenter_find(reinterpret_cast<void*>(screens),
        uint32_t(t::constant::game_menu_presenter)));
    if (!game::live(object)) return refused("the pause menu has no presenter");
    if (!game::is_a(object, presenter_class)) return refused("the presenter is not the pause menu's");
    const auto reset = game::method(object, t::presenter_table::reset);
    if (!reset) return refused("the presenter's function table is not the expected one");
    // The three calls of the title's Options menu, in its order: the presenter is reset, then begun, which is
    // where it finds its widget, then handed the page.
    reset(reinterpret_cast<void*>(object), 0);
    t::fn::presenter_begin(reinterpret_cast<void*>(object), false);
    // Without the widget the game's last function returns at once, and nothing would say the page had closed.
    if (!game::read_or<uint64_t>(object + t::game_menu_presenter::view)) return refused("the pause menu has no widget");
    alone = true; presenter = game::to_handle(object);
    Holder holder{{}, &closed_source, 0};
    t::fn::game_menu_open_page(reinterpret_cast<void*>(object), &holder, uint32_t(t::constant::game_menu_settings_page));
    titled = 0; title();
    return TSUCHI_OK;
}

void ui_mod_page_start(TSUCHI_Mod* framework) {
    framework_mod = framework;
    static bool listening = false;
    if (!listening) listening = each_engine_frame(step);
    ui_title_cover(covered);
}
} // namespace tsuchi
