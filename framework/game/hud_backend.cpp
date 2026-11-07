// The game side of the hud service: the game's announcement banner.

#include <string.h>

#include "actors.h"
#include "frame.h"
#include "game.h"
#include "hud_service.h"
#include "objects.h"
#include "reflect.h"
#include "registry.h"
#include "services.h"
#include "shared.h"

namespace tsuchi {
namespace {

namespace t = game::table;

uint64_t live_at(uint64_t address) {
    const uint64_t object = game::read_or<uint64_t>(address);
    return object && game::live(object) ? object : 0;
}

// The banner's widget, made by the game if it is not there yet.
uint64_t banner_widget(bool make) {
    const uint64_t framework = game::game_framework();
    const uint64_t ui = framework ? live_at(framework + t::game_framework::ui) : 0;
    const uint64_t announcer = ui ? live_at(ui + t::ui_manager::announcer) : 0;
    if (!announcer) return 0;
    if (make && (!t::bound(t::fn::ensure_banner) || !t::fn::ensure_banner(reinterpret_cast<void*>(announcer)))) return 0;
    return live_at(announcer + t::announcer::banner);
}
// The part of it that counts the time.
uint64_t banner_clock(uint64_t widget) { return widget ? live_at(widget + t::banner_widget::clock) : 0; }

// The banner's clock keeps how long the game's own announcements stay. A mod's
// banner borrows it: the game's time is put back once the banner has taken ours.
struct Borrowed {
    uint64_t clock = 0; // a handle
    float wait = 0;
} borrowed;

void step() {
    if (!borrowed.clock) return;
    const uint64_t clock = banner_clock(banner_widget(false));
    if (!clock || game::to_handle(clock) != borrowed.clock) { // the banner went with its level
        borrowed = Borrowed{};
        return;
    }
    if (game::read_or<int32_t>(clock + t::announce_widget::state) == int32_t(t::constant::announce_sliding_in)) return;
    game::write(clock + t::announce_widget::wait_time, borrowed.wait);
    borrowed = Borrowed{};
}

TSUCHI_Result banner(const char* text, float seconds) {
    if (!(seconds > 0)) return TSUCHI_INVALID;
    size_t count = 0;
    if (!game::utf8_to_units(text, nullptr, 0, count)) return TSUCHI_INVALID;
    if (!t::bound(t::fn::text_from_string) || !t::bound(t::fn::banner_set_text) || !t::bound(t::fn::banner_place) || !t::bound(t::fn::banner_play)) return TSUCHI_NOT_READY;
    const uint64_t widget = banner_widget(true), clock = banner_clock(widget);
    const auto show = widget ? game::method(widget, t::widget_table::set_visibility) : nullptr;
    if (!clock || !show) return TSUCHI_NOT_READY;
    auto* units = static_cast<char16_t*>(allocate((count + 1) * 2));
    if (!units) return TSUCHI_NO_MEMORY;
    game::utf8_to_units(text, units, count, count);
    units[count] = 0;

    const uint64_t handle = game::to_handle(clock);
    if (borrowed.clock != handle) borrowed = Borrowed{handle, game::read_or<float>(clock + t::announce_widget::wait_time)};
    game::write(clock + t::announce_widget::wait_time, seconds);
    show(reinterpret_cast<void*>(widget), uint32_t(t::constant::widget_shown));
    // The engine makes its own kind of text from a string, and shares it; this lets go of its share afterward.
    alignas(8) uint8_t string[t::array::size] = {};
    const uint64_t data = reinterpret_cast<uint64_t>(units);
    const int32_t length = int32_t(count + 1);
    memcpy(string + t::array::data, &data, 8), memcpy(string + t::array::count, &length, 4), memcpy(string + t::array::max, &length, 4);
    game::EngineText made = t::fn::text_from_string(string);
    t::fn::banner_set_text(reinterpret_cast<void*>(widget), &made);
    game::drop_shared(made.counter());
    release(units);
    t::fn::banner_place(reinterpret_cast<void*>(widget));
    t::fn::banner_play(reinterpret_cast<void*>(widget));
    return TSUCHI_OK;
}

const hud::Backend service{banner};

} // namespace

void hud_start() {
    hud::set_backend(&service);
    each_frame(step);
}

} // namespace tsuchi
