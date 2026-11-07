#include "ui_feedback.h"
#include "ui_rich_text.h"
#include "objects.h"
#include "reflect.h"
#include <limits.h>
#include <string.h>

namespace tsuchi {
namespace {
namespace t = game::table;
constexpr uint32_t kGuideUnits = 2048;
game::ClassRef host_class{t::name::ui_host_class};
game::ClassRef owner_class{t::name::ui_option_owner_class};
game::ClassRef footer_class{t::name::ui_footer_class};
game::ClassRef ingame_class{t::name::ui_footer_ingame_class};
game::ClassRef text_class{t::name::ui_footer_text_class};
game::ClassRef arrow_class{t::name::ui_arrow_class};

uint64_t child(uint64_t object, uint64_t offset) {
    const uint64_t value = object ? game::read_or<uint64_t>(object + offset) : 0;
    return value && game::live(value) ? value : 0;
}
uint64_t member(uint64_t object, const char* name) {
    const uint64_t value = game::member_object(object, name);
    return value && game::live(value) ? value : 0;
}
uint64_t label_for(uint64_t handle) {
    const uint64_t host = game::from_handle(handle);
    if (!host || !game::is_a(host, host_class)) return 0;
    uint64_t owner = host;
    for (uint32_t i = 0; owner && i < 8; ++i) {
        if (game::is_a(owner, owner_class) && member(owner, t::name::ui_option_settings_member) == host) {
            const uint64_t footer = member(owner, t::name::ui_option_footer_member);
            if (!footer) return 0;
            const uint64_t label = game::is_a(footer, footer_class) ? member(footer, t::name::ui_footer_text_member) :
                game::is_a(footer, ingame_class) ? member(footer, t::name::ui_footer_ingame_text_member) : 0;
            return label && game::is_a(label, text_class) ? game::to_handle(label) : 0;
        }
        owner = child(owner, t::object::outer);
    }
    return 0;
}
bool read_text(uint64_t handle, game::EngineText& out) {
    const uint64_t label = game::from_handle(handle);
    return label && game::read(label + t::ui_footer_label::text, out);
}
bool same(const game::EngineText& a, const game::EngineText& b) {
    // Native FText assignment copies data/control and flags, not its padding.
    return !memcmp(a.bytes + t::text_value::data, b.bytes + t::text_value::data, 8) &&
        a.counter() == b.counter() && !memcmp(a.bytes + t::text_value::flags, b.bytes + t::text_value::flags, 4);
}
bool same(const UiFooterGuide& a, const UiFooterGuide& b) {
    if (a.custom[0] || b.custom[0]) {
        for (size_t i = 0; i < sizeof a.custom; ++i) {
            if (a.custom[i] != b.custom[i]) return false;
            if (!a.custom[i]) return true;
        }
        return false;
    }
    return a.tabs == b.tabs && a.edit == b.edit && a.confirm == b.confirm &&
        a.back == b.back && a.reset == b.reset && a.details == b.details &&
        a.settings == b.settings && a.preserve_stock == b.preserve_stock;
}
bool retain_text(const game::EngineText& text) {
    const uint64_t counter = text.counter();
    if (!counter) return true;
    if (!game::writable(counter, t::shared_count::size)) return false;
    auto* holders = reinterpret_cast<int32_t*>(counter + t::shared_count::holders);
    int32_t before = __atomic_load_n(holders, __ATOMIC_RELAXED);
    while (before > 0 && before < INT32_MAX) {
        if (__atomic_compare_exchange_n(holders, &before, before + 1, false, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) return true;
    }
    return false;
}
void string_value(uint8_t* out, const char16_t* text, uint32_t count) {
    const uint64_t data = reinterpret_cast<uint64_t>(text);
    const int32_t units = int32_t(count);
    memcpy(out + t::array::data, &data, sizeof data);
    memcpy(out + t::array::count, &units, sizeof units);
    memcpy(out + t::array::max, &units, sizeof units);
}
bool append(char16_t* out, uint32_t& used, const char16_t* text, uint32_t length) {
    const uint32_t separator = used ? 4 : 0;
    if (!length || separator + length >= kGuideUnits - used) return false;
    for (uint32_t i = 0; i < separator; ++i) out[used++] = u' ';
    for (uint32_t i = 0; i < length; ++i) {
        if (!text[i]) return false;
        out[used++] = text[i];
    }
    out[used] = 0;
    return true;
}
bool copy_text(char16_t* out, uint32_t& used, game::EngineText& text, bool allow_empty) {
    const uint64_t string = reinterpret_cast<uint64_t>(t::fn::text_display_string(&text));
    const uint64_t data = string ? game::read_or<uint64_t>(string + t::array::data) : 0;
    const int32_t count = string ? game::read_or<int32_t>(string + t::array::count) : 0;
    const int32_t capacity = string ? game::read_or<int32_t>(string + t::array::max) : 0;
    const bool okay = data && count >= 1 && count <= int32_t(kGuideUnits) && capacity >= count &&
        game::readable(data, uint64_t(count) * sizeof(char16_t)) &&
        game::read_or<char16_t>(data + uint64_t(count - 1) * sizeof(char16_t)) == 0;
    if (!okay) return false;
    return count == 1 ? allow_empty : append(out, used, reinterpret_cast<const char16_t*>(data), uint32_t(count - 1));
}
bool entry(char16_t* out, uint32_t& used, const char16_t* key) {
    const char16_t space[] = u"FooterGuide";
    uint32_t length = 0; while (key[length]) ++length;
    alignas(8) uint8_t ns[t::array::size]{}, name[t::array::size]{};
    string_value(ns, space, sizeof space / sizeof space[0]);
    string_value(name, key, length + 1);
    game::EngineText text = t::fn::ui_footer_lookup(ns, name, 0, 0, 0);
    const bool okay = copy_text(out, used, text, false);
    game::drop_shared(text.counter());
    return okay;
}
bool action_entry(char16_t* out, uint32_t& used, uint32_t button, const char* caption) {
    const char* token = ui_glyph_token(button);
    if (!token) return false;
    char text[96];
    const size_t token_size = strlen(token), caption_size = strlen(caption);
    if (token_size + caption_size >= sizeof text) return false;
    memcpy(text, token, token_size); memcpy(text + token_size, caption, caption_size + 1);
    char16_t units[96]; size_t count = 0;
    return game::utf8_to_units(text, units, 95, count) && append(out, used, units, uint32_t(count));
}
bool compose(char16_t* out, uint32_t& used, const UiFooterGuide& guide, game::EngineText* stock) {
    used = 0;
    if (guide.custom[0]) {
        size_t length = 0; while (length < sizeof guide.custom && guide.custom[length]) ++length;
        size_t count = 0;
        if (length == sizeof guide.custom || !ui_glyph_text_syntax(guide.custom) ||
            !game::utf8_to_units(guide.custom, out, kGuideUnits - 1, count)) return false;
        used = uint32_t(count); out[used] = 0;
        return used != 0;
    }
    if (guide.preserve_stock) {
        if (!stock || !copy_text(out, used, *stock, true)) return false;
        return guide.tabs ? entry(out, used, u"ZLZR_MovePages") : used != 0;
    }
    if (guide.tabs && !entry(out, used, u"ZLZR_MovePages")) return false;
    if (guide.edit && !entry(out, used, u"LstLR_Edit")) return false;
    if (guide.settings) {
        if (!action_entry(out, used, 0, " Settings")) return false;
    } else if (guide.confirm && !entry(out, used, u"A_Decide")) return false;
    if (guide.details && !action_entry(out, used, 2, " Details")) return false;
    if (guide.reset && !entry(out, used, u"X_Return_Init")) return false; // stock key uses the Y glyph
    return entry(out, used, guide.back ? u"B_Back" : u"B_SaveClose");
}
}

void ui_footer_release(UiFooterLease& lease) {
    const uint64_t host = lease.host, label = lease.label;
    const bool installed = lease.installed;
    game::EngineText saved = lease.saved;
    const game::EngineText applied = lease.applied;
    ++lease.revision;
    lease.host = lease.label = 0;
    lease.saved = {}; lease.applied = {}; lease.guide = {}; lease.installed = false;
    // Forget ownership before calling the engine: reentrant cleanup cannot
    // restore or drop the same retained FText twice.
    game::EngineText current;
    if (installed && t::bound(t::fn::rich_text_set) && label_for(host) == label && read_text(label, current) && same(current, applied))
        t::fn::rich_text_set(reinterpret_cast<void*>(game::from_handle(label)), &saved);
    if (installed) { game::drop_shared(saved.counter()); game::drop_shared(applied.counter()); }
}

bool ui_footer_update(UiFooterLease& lease, uint64_t host, const UiFooterGuide& input) {
    // Native text/lookup callbacks may replace the caller's screen or guide.
    const UiFooterGuide guide = input;
    const bool preserve_stock = guide.preserve_stock && !guide.custom[0];
    const uint64_t label = label_for(host);
    game::EngineText current;
    if (lease.installed && lease.host == host && lease.label == label && same(lease.guide, guide) &&
        read_text(label, current) && same(current, lease.applied)) return true;
    const uint64_t revision = lease.revision + 1;
    ui_footer_release(lease);
    // Restoring the old guide can synchronously install a successor lease.
    if (lease.revision != revision || lease.installed) return false;
    if (!label || !t::bound(t::fn::text_from_string) || !t::bound(t::fn::rich_text_set) ||
        (!guide.custom[0] && (!t::bound(t::fn::ui_footer_lookup) || !t::bound(t::fn::text_display_string)))) return false;
    char16_t units[kGuideUnits]; uint32_t count = 0;
    game::EngineText saved;
    bool retained = false;
    if (preserve_stock) {
        if (!read_text(label, saved) || !retain_text(saved)) return false;
        retained = true;
    }
    if (!compose(units, count, guide, preserve_stock ? &saved : nullptr) ||
        lease.revision != revision || label_for(host) != label) {
        if (retained) game::drop_shared(saved.counter());
        return false;
    }
    alignas(8) uint8_t string[t::array::size]{};
    string_value(string, units, count + 1);
    game::EngineText wanted = t::fn::text_from_string(string), baseline;
    bool ready = lease.revision == revision && label_for(host) == label && read_text(label, baseline);
    if (preserve_stock) ready = ready && same(baseline, saved);
    else if (ready) { saved = baseline; ready = retain_text(saved); retained = ready; }
    if (!ready) {
        if (retained) game::drop_shared(saved.counter());
        game::drop_shared(wanted.counter()); return false;
    }
    t::fn::rich_text_set(reinterpret_cast<void*>(game::from_handle(label)), &wanted);
    // Own the applied value as well as the baseline. A native replacement can
    // release the label's last share before this lease is next inspected.
    const bool applied = lease.revision == revision && label_for(host) == label && read_text(label, current) &&
        same(current, wanted) && retain_text(current);
    if (applied) {
        lease.host = host; lease.label = label; lease.saved = saved; lease.applied = current;
        lease.guide = guide; lease.installed = true;
    } else {
        // A native callback may close this view while SetText is running. Put
        // back only text demonstrably written by this call, never a successor.
        if (label_for(host) == label && read_text(label, current) && same(current, wanted))
            t::fn::rich_text_set(reinterpret_cast<void*>(game::from_handle(label)), &saved);
        game::drop_shared(saved.counter());
    }
    game::drop_shared(wanted.counter());
    return applied;
}
bool ui_retain_text(const game::EngineText& text) { return retain_text(text); }
bool ui_feedback_arrow(uint64_t selector, bool increment) {
    const uint64_t object = game::from_handle(selector);
    if (!object || !game::is_a(object, arrow_class)) return false;
    const auto play = increment ? t::fn::ui_arrow_right : t::fn::ui_arrow_left;
    if (!play) return false;
    play(reinterpret_cast<void*>(object));
    return game::from_handle(selector) == object;
}
bool ui_feedback_focus(uint64_t widget) {
    const uint64_t object = game::from_handle(widget);
    const uint64_t sound = game::global<uint64_t>(t::data::sound_system);
    const uint64_t key = game::global<uint64_t>(t::data::ui_select_sound);
    if (!object || !sound || !key || !t::bound(t::fn::ui_post_sound) ||
        !game::read_or<uint8_t>(sound + t::sound_system::started) ||
        !game::read_or<uint8_t>(sound + t::sound_system::ready)) return false;
    t::fn::ui_post_sound(reinterpret_cast<void*>(object), &key, 0, 4, false, false);
    return game::from_handle(widget) == object;
}
}
