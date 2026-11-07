#pragma once
#include "game.h"
#include "objects.h"
#include "ui_label_fit.h"

namespace tsuchi {
// Only generated labels use this adapter. The owner remains retained by its
// caller; each native setter may end that owner's host, so recheck between calls.
template<class Valid>
void ui_fit_owned_label(UiLabelFitStamp& stamp, uint64_t scroll, uint64_t label,
                        bool heading, Valid valid) {
    namespace t = game::table;
    if (!valid() || !scroll || !label || !t::bound(t::fn::ui_cached_geometry) ||
        !t::bound(t::fn::ui_widget_pivot) || !t::bound(t::fn::ui_widget_scale) || !t::bound(t::fn::ui_mod_list_transform)) return;
    const uint64_t scroll_handle = game::to_handle(scroll), label_handle = game::to_handle(label);
    const auto current = [&] {
        return valid() && game::from_handle(scroll_handle) == scroll && game::from_handle(label_handle) == label;
    };
    if (!scroll_handle || !label_handle || !current()) return;
    float clip[13]{}, geometry[13]{};
    const uint64_t clip_at = reinterpret_cast<uint64_t>(t::fn::ui_cached_geometry(reinterpret_cast<void*>(scroll)));
    if (!current() || !clip_at || !game::read(clip_at, clip)) return;
    const uint64_t label_at = reinterpret_cast<uint64_t>(t::fn::ui_cached_geometry(reinterpret_cast<void*>(label)));
    if (!current() || !label_at || !game::read(label_at, geometry) || ui_label_fit_seen(stamp, clip, geometry)) return;
    const float before = game::read_or<float>(label + t::ui_widget::render_scale_x);
    const float vertical = game::read_or<float>(label + t::ui_widget::render_scale_y);
    const float pivot = game::read_or<float>(label + t::ui_widget::render_pivot_x);
    const float pivot_y = game::read_or<float>(label + t::ui_widget::render_pivot_y);
    float x = game::read_or<float>(label + t::ui_widget::render_x), scale = before;
    if (!(vertical > 0 && vertical <= 2 && pivot >= 0 && pivot <= 1 && pivot_y >= 0 && pivot_y <= 1 &&
          x > -100000 && x < 100000)) return;
    if (heading) {
        if (!ui_heading_fit(clip, geometry, before, x, pivot, scale, x)) return;
    } else {
        if (!ui_label_fit(clip, geometry, before, scale)) return;
        // Adopt the right pivot without moving the preexisting right edge.
        x += (before - 1) * (1 - pivot) * geometry[0];
        if (!(x > -100000 && x < 100000)) return;
    }
    if (!current()) return;
    ui_label_fit_remember(stamp, clip, geometry);
    t::fn::ui_widget_pivot(heading ? 0 : 1, pivot_y, reinterpret_cast<void*>(label));
    if (!current()) return;
    t::fn::ui_widget_scale(scale, vertical, reinterpret_cast<void*>(label));
    if (!current() || !game::write<float>(label + t::ui_widget::render_x, x)) return;
    if (current()) t::fn::ui_mod_list_transform(reinterpret_cast<void*>(label));
}
} // namespace tsuchi
