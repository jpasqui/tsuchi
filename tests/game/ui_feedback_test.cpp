// Scoped footer ownership and localized guide composition against a made-up game.
#include "fake_game.h"
#include "objects.h"
#include "ui_feedback.h"
#include "ui_rich_text.h"
#include "ui_members.h"
#include <cassert>
#include <climits>
#include <map>
#include <string>
#include <vector>

using namespace tsuchi;
namespace t = game::table;
namespace {
uint64_t root_type, host_type, owner_type, footer_type, ingame_type, label_type, arrow_type;
uint64_t host, owner, footer, label, host_handle;
unsigned lookups = 0, sets = 0, left = 0, right = 0;
unsigned focus_calls = 0;
constexpr uint64_t select_key = 37;
bool other_locale = false;
std::u16string missing, oversized;
std::vector<uint64_t> counters;
struct TextAllocation { uint64_t data, string; size_t bytes; };
std::map<uint64_t, TextAllocation> allocations;
UiFooterLease* cancel_during_set = nullptr;
UiFooterLease* replace_during_restore = nullptr;
UiFooterGuide restore_successor_guide;
bool restore_successor_applied = false;
UiFooterGuide* mutate_guide_during_set = nullptr;
std::string guide_mutation;

uint64_t type(const char* name) {
    std::u16string units; while (*name) units.push_back(char16_t(*name++));
    return fake::object(units, root_type, 0x100);
}
uint64_t widget(uint64_t kind) {
    const uint64_t made = fake::object(u"widget", kind, 0x900);
    if (kind == label_type) {
        game::EngineText empty;
        memset(empty.bytes, 0xD3, sizeof empty.bytes);
        memset(empty.bytes + t::text_value::data, 0, sizeof(uint64_t));
        memset(empty.bytes + t::text_value::counter, 0, sizeof(uint64_t));
        memset(empty.bytes + t::text_value::flags, 0, sizeof(uint32_t));
        fake::put(made + t::ui_footer_label::text, empty);
    }
    return made;
}
std::u16string string_at(const void* pointer) {
    const uint64_t at = reinterpret_cast<uint64_t>(pointer);
    const uint64_t data = fake::get<uint64_t>(at + t::array::data);
    const int32_t count = fake::get<int32_t>(at + t::array::count);
    assert(data && count > 0);
    return {reinterpret_cast<const char16_t*>(data), size_t(count - 1)};
}
game::EngineText text(const std::u16string& value) {
    const uint64_t data = fake::memory((value.size() + 1) * 2), string = fake::memory(t::array::size);
    memcpy(reinterpret_cast<void*>(data), value.c_str(), (value.size() + 1) * 2);
    fake::put<uint64_t>(string + t::array::data, data);
    fake::put<int32_t>(string + t::array::count, int32_t(value.size() + 1));
    fake::put<int32_t>(string + t::array::max, int32_t(value.size() + 1));
    const uint64_t counter = fake::memory(t::shared_count::size);
    fake::put<int32_t>(counter + t::shared_count::holders, 1);
    counters.push_back(counter);
    allocations[counter] = {data, string, (value.size() + 1) * 2};
    game::EngineText out;
    // Native FText assignment does not copy its gaps or trailing padding.
    // Each returned value deliberately differs from the label's poisoned gaps.
    memset(out.bytes, int(0x40 | (counters.size() & 0x3F)), sizeof out.bytes);
    const uint32_t flags = 0;
    memcpy(out.bytes + t::text_value::data, &string, 8);
    memcpy(out.bytes + t::text_value::counter, &counter, 8);
    memcpy(out.bytes + t::text_value::flags, &flags, 4);
    return out;
}
void* display_string(void* pointer) {
    const auto& value = *static_cast<game::EngineText*>(pointer);
    const uint64_t counter = value.counter();
    assert(counter && game::readable(counter, t::shared_count::size));
    assert(fake::get<int32_t>(counter + t::shared_count::holders) > 0);
    uint64_t string = 0; memcpy(&string, value.bytes + t::text_value::data, 8);
    assert(game::readable(string, t::array::size));
    const uint64_t data = fake::get<uint64_t>(string + t::array::data);
    const int32_t count = fake::get<int32_t>(string + t::array::count);
    assert(count > 0 && game::readable(data, uint64_t(count) * 2));
    return reinterpret_cast<void*>(string);
}
game::EngineText lookup(const void* space, const void* key, uint32_t flags, uint32_t fallback, uint32_t) {
    assert(string_at(space) == u"FooterGuide" && !flags && !fallback);
    ++lookups;
    const auto name = string_at(key);
    if (name == missing) return text(u"");
    if (name == oversized) return text(std::u16string(2048, u'a'));
    // Stand-in translations retain their glyph markup, including the key whose
    // name says X while the actual game entry advertises Y.
    const std::map<std::u16string, std::u16string> values{
        {u"ZLZR_MovePages", u"[PictureFont:IconData key=\"Variable_L_Button\"/]/[PictureFont:IconData key=\"Variable_R_Button\"/] Tabs"},
        {u"LstLR_Edit", u"[PictureFont:IconData key=\"Variable_L_Stick_LR\"/] Edit"},
        {u"A_Decide", u"[PictureFont:IconData key=\"Variable_A_Button\"/] Confirm"},
        {u"X_Return_Init", u"[PictureFont:IconData key=\"Variable_Y_Button\"/] Reset"},
        {u"B_Back", u"[PictureFont:IconData key=\"Variable_B_Button\"/] Back"},
        {u"B_SaveClose", u"[PictureFont:IconData key=\"Variable_B_Button\"/] Save and close"}
    };
    assert(values.count(name));
    return text((other_locale ? u"locale-two " : u"") + values.at(name));
}
game::EngineText from_string(const void* pointer) { return text(string_at(pointer)); }
void retain(uint64_t counter) {
    if (counter) {
        assert(game::readable(counter, t::shared_count::size));
        const int32_t before = fake::get<int32_t>(counter + t::shared_count::holders);
        assert(before > 0 && before < INT32_MAX);
        fake::put<int32_t>(counter + t::shared_count::holders, before + 1);
    }
}
void set_text(void* pointer, void* value) {
    ++sets;
    const uint64_t address = reinterpret_cast<uint64_t>(pointer) + t::ui_footer_label::text;
    const auto before = fake::get<game::EngineText>(address);
    const auto wanted = *static_cast<game::EngineText*>(value);
    retain(wanted.counter());
    game::drop_shared(before.counter());
    uint64_t data = 0; uint32_t flags = 0;
    memcpy(&data, wanted.bytes + t::text_value::data, sizeof data);
    memcpy(&flags, wanted.bytes + t::text_value::flags, sizeof flags);
    fake::put<uint64_t>(address + t::text_value::data, data);
    fake::put<uint64_t>(address + t::text_value::counter, wanted.counter());
    fake::put<uint32_t>(address + t::text_value::flags, flags);
    const auto after = fake::get<game::EngineText>(address);
    for (size_t i = 0; i < sizeof after.bytes; ++i) {
        const bool semantic = i - size_t(t::text_value::data) < 8 ||
            i - size_t(t::text_value::counter) < 8 || i - size_t(t::text_value::flags) < 4;
        assert(semantic || after.bytes[i] == before.bytes[i]);
    }
    if (cancel_during_set) {
        auto* lease = cancel_during_set; cancel_during_set = nullptr;
        ui_footer_release(*lease);
    }
    if (replace_during_restore) {
        auto* lease = replace_during_restore; replace_during_restore = nullptr;
        restore_successor_applied = ui_footer_update(*lease, host_handle, restore_successor_guide);
    }
    if (mutate_guide_during_set) {
        auto* guide = mutate_guide_during_set; mutate_guide_during_set = nullptr;
        assert(guide_mutation.size() < sizeof guide->custom);
        memcpy(guide->custom, guide_mutation.c_str(), guide_mutation.size() + 1);
    }
}
std::u16string shown(uint64_t target) {
    auto value = fake::get<game::EngineText>(target + t::ui_footer_label::text);
    return string_at(display_string(&value));
}
void original(uint64_t target, const char16_t* caption) {
    auto value = text(caption);
    set_text(reinterpret_cast<void*>(target), &value);
    game::drop_shared(value.counter());
}
void play_left(void*) { ++left; }
void play_right(void*) { ++right; }
uint64_t play_focus(void* widget, const void* key, uint32_t flags, uint32_t group, bool first, bool second) {
    assert(game::live(reinterpret_cast<uint64_t>(widget)));
    uint64_t value = 0; memcpy(&value, key, sizeof value);
    assert(value == select_key && flags == 0 && group == 4 && !first && !second);
    ++focus_calls; return 1;
}
bool has(const std::u16string& value, const char16_t* part) { return value.find(part) != std::u16string::npos; }
std::u16string ascii_units(const std::string& value) {
    std::u16string result;
    for (unsigned char byte : value) { assert(byte < 128); result.push_back(char16_t(byte)); }
    return result;
}
bool has_settings(const std::u16string& value) { return value.find(ascii_units(std::string(ui_glyph_token(0)) + " Settings")) != std::u16string::npos; }
bool has_details(const std::u16string& value) { return value.find(ascii_units(std::string(ui_glyph_token(2)) + " Details")) != std::u16string::npos; }
unsigned occurrences(const std::u16string& value, const char16_t* part) {
    unsigned count = 0; size_t at = 0;
    while ((at = value.find(part, at)) != std::u16string::npos) { ++count; ++at; }
    return count;
}
int32_t holders(uint64_t counter) { return fake::get<int32_t>(counter + t::shared_count::holders); }
}
namespace tsuchi::game {
bool utf8_to_units(const char* text, char16_t* out, size_t capacity, size_t& count) {
    count = 0;
    const auto* at = reinterpret_cast<const unsigned char*>(text);
    while (*at) {
        const uint32_t first = *at++;
        uint32_t scalar = 0, extra = 0, minimum = 0;
        if (first < 0x80) scalar = first;
        else if (first >= 0xC2 && first <= 0xDF) { scalar = first & 0x1F; extra = 1; minimum = 0x80; }
        else if (first >= 0xE0 && first <= 0xEF) { scalar = first & 0x0F; extra = 2; minimum = 0x800; }
        else if (first >= 0xF0 && first <= 0xF4) { scalar = first & 7; extra = 3; minimum = 0x10000; }
        else return false;
        for (uint32_t i = 0; i < extra; ++i) {
            if ((*at & 0xC0) != 0x80) return false;
            scalar = scalar << 6 | (*at++ & 0x3F);
        }
        if (scalar < minimum || scalar > 0x10FFFF || (scalar >= 0xD800 && scalar <= 0xDFFF)) return false;
        const size_t units = scalar > 0xFFFF ? 2 : 1;
        if (units > capacity - count) return false;
        if (units == 1) out[count++] = char16_t(scalar);
        else { scalar -= 0x10000; out[count++] = char16_t(0xD800 + (scalar >> 10)); out[count++] = char16_t(0xDC00 + (scalar & 0x3FF)); }
    }
    return true;
}
void drop_shared(uint64_t counter) {
    if (!counter) return;
    assert(game::readable(counter, t::shared_count::size));
    const int32_t before = fake::get<int32_t>(counter + t::shared_count::holders);
    assert(before > 0);
    fake::put<int32_t>(counter + t::shared_count::holders, before - 1);
    if (before == 1) {
        const auto allocation = allocations.at(counter);
        memset(reinterpret_cast<void*>(allocation.data), 0xDE, allocation.bytes);
        memset(reinterpret_cast<void*>(allocation.string), 0xDE, t::array::size);
        fake::unmap(allocation.data); fake::unmap(allocation.string); fake::unmap(counter);
    }
}
}
int main() {
    fake::start();
    root_type = fake::object(u"Class", 0, 0x100);
    host_type = type(t::name::ui_host_class); owner_type = type(t::name::ui_option_owner_class);
    footer_type = type(t::name::ui_footer_class); ingame_type = type(t::name::ui_footer_ingame_class);
    label_type = type(t::name::ui_footer_text_class); arrow_type = type(t::name::ui_arrow_class);
    ui_members::option_owner(owner_type); ui_members::footers(footer_type, ingame_type);
    host = widget(host_type); owner = widget(owner_type); footer = widget(footer_type); label = widget(label_type);
    const uint64_t tree = widget(root_type);
    fake::put<uint64_t>(host + t::object::outer, tree); fake::put<uint64_t>(tree + t::object::outer, owner);
    fake::put<uint64_t>(owner + ui_members::settings, host);
    fake::put<uint64_t>(owner + ui_members::footer, footer);
    fake::put<uint64_t>(footer + ui_members::footer_text, label);
    host_handle = game::to_handle(host);
    t::fn::ui_footer_lookup = lookup; t::fn::text_display_string = display_string;
    t::fn::text_from_string = from_string; t::fn::rich_text_set = set_text;
    t::fn::ui_arrow_left = play_left; t::fn::ui_arrow_right = play_right;
    t::fn::ui_post_sound = play_focus;
    original(label, u"original guide");
    const auto baseline = fake::get<game::EngineText>(label + t::ui_footer_label::text);
    UiFooterLease lease;
    UiFooterGuide guide{true, true, true, false, true, true};
    assert(ui_footer_update(lease, host_handle, guide));
    assert(lease.installed && holders(lease.applied.counter()) == 2); // label and lease each own a share
    auto content = shown(label);
    assert(has(content, u"Variable_L_Button") && has(content, u"Variable_R_Button"));
    assert(has(content, u"Variable_Y_Button") && !has(content, u"Variable_X_Button"));
    assert(has_details(content) && !has(content, u"X: Details") && has(content, u"Confirm") && has(content, u"Save and close"));
    assert(fake::get<int32_t>(baseline.counter() + t::shared_count::holders) == 1);
    const unsigned initial_lookups = lookups, initial_sets = sets;
    for (int i = 0; i < 100; ++i) assert(ui_footer_update(lease, host_handle, guide));
    assert(lookups == initial_lookups && sets == initial_sets);

    UiFooterGuide passive{true, false, false, true, false};
    other_locale = true;
    assert(ui_footer_update(lease, host_handle, passive));
    content = shown(label);
    assert(has(content, u"locale-two") && has(content, u"Back"));
    assert(!has(content, u"Confirm") && !has(content, u"Reset") && !has(content, u"Details") && !has(content, u" Edit"));
    ui_footer_release(lease);
    assert(shown(label) == u"original guide");
    const unsigned released_sets = sets;
    ui_footer_release(lease); assert(sets == released_sets);
    assert(fake::get<int32_t>(baseline.counter() + t::shared_count::holders) == 1);

    // Releasing A restores its baseline through SetText. That callback can
    // install C on the same lease before the outer request for B resumes.
    assert(ui_footer_update(lease, host_handle, guide));
    const uint64_t before_restore_applied = lease.applied.counter();
    restore_successor_guide = {false, true, true, true, false};
    replace_during_restore = &lease;
    assert(!ui_footer_update(lease, host_handle, passive));
    assert(restore_successor_applied && lease.installed);
    assert(!lease.guide.tabs && lease.guide.edit && lease.guide.confirm && lease.guide.back && !lease.guide.reset);
    content = shown(label);
    assert(has(content, u"Confirm") && has(content, u" Edit") && has(content, u"Back"));
    assert(!has(content, u"Variable_L_Button") && !has(content, u"Reset") && !has(content, u"Details"));
    const auto successor_value = fake::get<game::EngineText>(label + t::ui_footer_label::text);
    assert(successor_value.counter() == lease.applied.counter() && lease.saved.counter() == baseline.counter());
    assert(holders(before_restore_applied) == 0 && holders(lease.applied.counter()) == 2 && holders(baseline.counter()) == 1);
    const unsigned successor_stable_sets = sets, successor_stable_lookups = lookups;
    for (unsigned i = 0; i < 40; ++i) assert(ui_footer_update(lease, host_handle, restore_successor_guide));
    assert(sets == successor_stable_sets && lookups == successor_stable_lookups);
    const uint64_t restored_successor_counter = lease.applied.counter();
    ui_footer_release(lease);
    assert(shown(label) == u"original guide" && holders(restored_successor_counter) == 0 && holders(baseline.counter()) == 1);

    UiFooterGuide stock_guide;
    stock_guide.tabs = true;
    stock_guide.preserve_stock = true;
    stock_guide.confirm = stock_guide.reset = true; // ignored in preserve mode
    assert(ui_footer_update(lease, host_handle, stock_guide));
    content = shown(label);
    assert(content.find(u"original guide") == 0 && has(content, u"Variable_L_Button"));
    assert(!has(content, u"Confirm") && !has(content, u"Reset") && !has(content, u"Back"));
    const unsigned stock_lookups = lookups, stock_sets = sets;
    for (int i = 0; i < 100; ++i) assert(ui_footer_update(lease, host_handle, stock_guide));
    assert(lookups == stock_lookups && sets == stock_sets);
    ui_footer_release(lease);
    assert(shown(label) == u"original guide");

    // Failed localization is whole-or-nothing, including an oversized entry.
    missing = u"X_Return_Init";
    assert(!ui_footer_update(lease, host_handle, guide)); assert(shown(label) == u"original guide");
    missing.clear(); oversized = u"ZLZR_MovePages";
    assert(!ui_footer_update(lease, host_handle, guide)); assert(shown(label) == u"original guide");
    oversized.clear();
    fake::protect(baseline.counter(), false);
    assert(!ui_footer_update(lease, host_handle, guide) && shown(label) == u"original guide");
    fake::protect(baseline.counter(), true);
    fake::put<int32_t>(baseline.counter() + t::shared_count::holders, INT32_MAX);
    assert(!ui_footer_update(lease, host_handle, guide) && shown(label) == u"original guide");
    assert(fake::get<int32_t>(baseline.counter() + t::shared_count::holders) == INT32_MAX);
    fake::put<int32_t>(baseline.counter() + t::shared_count::holders, 1);
    fake::put<uint64_t>(owner + ui_members::settings, 0);
    assert(!ui_footer_update(lease, host_handle, guide));
    fake::put<uint64_t>(owner + ui_members::settings, host);

    // A guide written by the game wins over our outstanding lease.
    assert(ui_footer_update(lease, host_handle, guide));
    const auto overwritten_applied = lease.applied;
    original(label, u"successor game guide");
    assert(holders(overwritten_applied.counter()) == 1);
    auto retained_applied = overwritten_applied;
    assert(has(string_at(display_string(&retained_applied)), u"Confirm"));
    const unsigned successor_sets = sets;
    ui_footer_release(lease);
    assert(sets == successor_sets && shown(label) == u"successor game guide");
    assert(holders(overwritten_applied.counter()) == 0 && !game::readable(allocations.at(overwritten_applied.counter()).data, 1));

    // A lost/reused label never receives restoration through an old handle.
    assert(ui_footer_update(lease, host_handle, guide));
    const int32_t index = fake::get<int32_t>(label + t::object::index);
    game::drop_shared(fake::get<game::EngineText>(label + t::ui_footer_label::text).counter());
    fake::destroy(label); fake::place(label, index);
    auto replacement = text(u"new generation guide"); fake::put(label + t::ui_footer_label::text, replacement);
    assert(game::to_handle(label) != lease.label);
    const unsigned replacement_sets = sets;
    ui_footer_release(lease);
    assert(sets == replacement_sets && shown(label) == u"new generation guide");

    // In-game footer uses its distinct reflected text field.
    const uint64_t in_game = widget(ingame_type), in_game_label = widget(label_type);
    fake::put<uint64_t>(owner + ui_members::footer, in_game);
    fake::put<uint64_t>(in_game + ui_members::ingame_text, in_game_label);
    original(in_game_label, u"in-game original");
    assert(ui_footer_update(lease, host_handle, passive)); ui_footer_release(lease);
    assert(shown(in_game_label) == u"in-game original");

    // Cleanup during SetText cancels publication and releases the saved share.
    cancel_during_set = &lease;
    assert(!ui_footer_update(lease, host_handle, guide));
    assert(!lease.installed && shown(in_game_label) == u"in-game original");

    assert(ui_footer_update(lease, host_handle, stock_guide));
    original(in_game_label, u"stock context changed");
    assert(ui_footer_update(lease, host_handle, stock_guide));
    content = shown(in_game_label);
    assert(content.find(u"stock context changed") == 0);
    const auto token = content.find(u"Variable_L_Button");
    assert(token != std::u16string::npos && content.find(u"Variable_L_Button", token + 1) == std::u16string::npos);
    ui_footer_release(lease);
    assert(shown(in_game_label) == u"stock context changed");

    // The source-array loan does not transfer a footer lease. The Mods guide
    // returns to the native baseline before a custom view acquires its guide.
    UiFooterLease game_lease, mods_lease, custom_lease;
    UiFooterGuide mods_guide;
    mods_guide.tabs = mods_guide.edit = mods_guide.settings = mods_guide.details = mods_guide.reset = true;
    UiFooterGuide custom_guide;
    custom_guide.tabs = custom_guide.edit = custom_guide.back = custom_guide.reset = true;
    const char16_t stock_context[] = u"[PictureFont:IconData key=\"Variable_L_Stick_LR\"/] Edit    [PictureFont:IconData key=\"Variable_Y_Button\"/] Revert    [PictureFont:IconData key=\"Variable_B_Button\"/] Save and close";
    const char16_t revert_context[] = u"[PictureFont:IconData key=\"Variable_L_Stick_LR\"/] Edit after revert    [PictureFont:IconData key=\"Variable_Y_Button\"/] Revert    [PictureFont:IconData key=\"Variable_B_Button\"/] Save and close";
    original(in_game_label, stock_context);
    assert(ui_footer_update(game_lease, host_handle, stock_guide));
    content = shown(in_game_label);
    assert(has(content, u"Variable_L_Stick_LR") && occurrences(content, u"Variable_L_Button") == 1 && occurrences(content, u"Variable_R_Button") == 1);
    const unsigned game_stable_sets = sets;
    for (unsigned i = 0; i < 40; ++i) assert(ui_footer_update(game_lease, host_handle, stock_guide));
    assert(sets == game_stable_sets);
    ui_footer_release(game_lease); assert(shown(in_game_label) == stock_context);
    assert(ui_footer_update(mods_lease, host_handle, mods_guide));
    assert(has_settings(shown(in_game_label)) && has_details(shown(in_game_label)));
    assert(!has(shown(in_game_label), u"A: Settings") && !has(shown(in_game_label), u"X: Details"));
    ui_footer_release(mods_lease); assert(shown(in_game_label) == stock_context);
    assert(ui_footer_update(custom_lease, host_handle, custom_guide));
    content = shown(in_game_label);
    assert(has(content, u"Variable_L_Stick_LR") && has(content, u"Back") && !has_settings(content) && !has_details(content));
    const unsigned custom_stable_sets = sets;
    for (unsigned i = 0; i < 40; ++i) assert(ui_footer_update(custom_lease, host_handle, custom_guide));
    assert(sets == custom_stable_sets);
    ui_footer_release(custom_lease); assert(shown(in_game_label) == stock_context);
    assert(ui_footer_update(mods_lease, host_handle, mods_guide));
    ui_footer_release(mods_lease); assert(shown(in_game_label) == stock_context);
    assert(ui_footer_update(game_lease, host_handle, stock_guide));
    content = shown(in_game_label);
    assert(has(content, u"Variable_L_Stick_LR") && !has_settings(content) && !has_details(content));
    assert(occurrences(content, u"Variable_L_Button") == 1 && occurrences(content, u"Variable_R_Button") == 1);
    const auto before_revert_applied = game_lease.applied;
    original(in_game_label, revert_context); // the stock Revert continuation replaces its guide
    assert(holders(before_revert_applied.counter()) == 1);
    assert(ui_footer_update(game_lease, host_handle, stock_guide));
    assert(holders(before_revert_applied.counter()) == 0 && !game::readable(allocations.at(before_revert_applied.counter()).string, t::array::size));
    content = shown(in_game_label);
    assert(content.find(revert_context) == 0 && !has_settings(content) && !has_details(content));
    assert(occurrences(content, u"Variable_L_Stick_LR") == 1 && occurrences(content, u"Variable_L_Button") == 1 && occurrences(content, u"Variable_R_Button") == 1);
    const unsigned after_revert_sets = sets, after_revert_lookups = lookups;
    for (unsigned i = 0; i < 80; ++i) assert(ui_footer_update(game_lease, host_handle, stock_guide));
    assert(sets == after_revert_sets && lookups == after_revert_lookups);
    ui_footer_release(game_lease); ui_footer_release(mods_lease); ui_footer_release(custom_lease);
    assert(shown(in_game_label) == revert_context);

    // A native successor can preserve the shared text while changing its flags.
    // Those flags are part of identity; shifted fixture storage must be read.
    assert(ui_footer_update(game_lease, host_handle, stock_guide));
    const uint64_t flagged_applied = game_lease.applied.counter(), flagged_saved = game_lease.saved.counter();
    const uint64_t flags_address = in_game_label + t::ui_footer_label::text + t::text_value::flags;
    const uint32_t native_flags = fake::get<uint32_t>(flags_address) ^ 0x80u;
    fake::put<uint32_t>(flags_address, native_flags);
    const unsigned before_flags_release = sets;
    ui_footer_release(game_lease);
    const auto flagged_current = fake::get<game::EngineText>(in_game_label + t::ui_footer_label::text);
    assert(sets == before_flags_release && flagged_current.counter() == flagged_applied && fake::get<uint32_t>(flags_address) == native_flags);
    assert(holders(flagged_applied) == 1 && holders(flagged_saved) == 0); // only the label's successor share remains
    original(in_game_label, revert_context);
    assert(holders(flagged_applied) == 0 && shown(in_game_label) == revert_context);

    // An authored footer replaces every automatic command and owns its bytes.
    // Exercise every canonical glyph and a non-ASCII caption through UTF-8.
    const std::string custom_ascii = std::string(ui_glyph_token(0)) + " Inspect    " + ui_glyph_token(1) + " Back    " +
        ui_glyph_token(2) + " Details    " + ui_glyph_token(3) + " Reset    " + ui_glyph_token(4) + "/" + ui_glyph_token(5) +
        " Tabs    " + ui_glyph_token(6) + " More Caf";
    const std::string custom_first = custom_ascii + "\xC3\xA9";
    const std::u16string custom_first_units = ascii_units(custom_ascii) + u"\u00E9";
    const std::string custom_second = std::string(ui_glyph_token(1)) + " Exit";
    UiFooterGuide custom_input;
    custom_input.tabs = custom_input.edit = custom_input.confirm = custom_input.back = custom_input.reset = true;
    custom_input.details = custom_input.settings = custom_input.preserve_stock = true;
    assert(custom_first.size() < sizeof custom_input.custom);
    memcpy(custom_input.custom, custom_first.c_str(), custom_first.size() + 1);
    assert(ui_footer_update(lease, host_handle, guide));
    const auto lookup_saved = t::fn::ui_footer_lookup;
    const auto display_saved = t::fn::text_display_string;
    t::fn::ui_footer_lookup = nullptr; t::fn::text_display_string = nullptr;
    guide_mutation = custom_second; mutate_guide_during_set = &custom_input;
    assert(ui_footer_update(lease, host_handle, custom_input));
    assert(shown(in_game_label) == custom_first_units && !strcmp(lease.guide.custom, custom_first.c_str()));
    assert(!strcmp(custom_input.custom, custom_second.c_str())); // the restore callback changed the caller's storage
    UiFooterGuide same_custom = lease.guide;
    same_custom.tabs = same_custom.edit = same_custom.confirm = same_custom.back = same_custom.reset = false;
    same_custom.details = same_custom.settings = same_custom.preserve_stock = false;
    const size_t custom_length = strlen(same_custom.custom);
    memset(same_custom.custom + custom_length + 1, 0xAB, sizeof same_custom.custom - custom_length - 1);
    const unsigned custom_sets = sets, custom_lookups = lookups;
    for (unsigned i = 0; i < 60; ++i) assert(ui_footer_update(lease, host_handle, same_custom));
    assert(sets == custom_sets && lookups == custom_lookups && shown(in_game_label) == custom_first_units);
    const uint64_t custom_before_reentry = lease.applied.counter();
    const std::string custom_third = std::string(ui_glyph_token(2)) + " Review";
    restore_successor_guide = {};
    memcpy(restore_successor_guide.custom, custom_third.c_str(), custom_third.size() + 1);
    restore_successor_applied = false; replace_during_restore = &lease;
    assert(!ui_footer_update(lease, host_handle, custom_input));
    assert(restore_successor_applied && lease.installed && !strcmp(lease.guide.custom, custom_third.c_str()));
    assert(shown(in_game_label) == ascii_units(custom_third) && holders(custom_before_reentry) == 0);
    assert(holders(lease.applied.counter()) == 2 && holders(lease.saved.counter()) == 1);
    const unsigned custom_successor_sets = sets;
    for (unsigned i = 0; i < 20; ++i) assert(ui_footer_update(lease, host_handle, restore_successor_guide));
    assert(sets == custom_successor_sets && lookups == custom_lookups);
    const uint64_t custom_successor_counter = lease.applied.counter();
    assert(ui_footer_update(lease, host_handle, custom_input));
    assert(shown(in_game_label) == ascii_units(custom_second) && !strcmp(lease.guide.custom, custom_second.c_str()));
    assert(holders(custom_successor_counter) == 0);
    t::fn::ui_footer_lookup = lookup_saved; t::fn::text_display_string = display_saved;
    custom_input = {}; custom_input.tabs = custom_input.edit = custom_input.back = true;
    assert(ui_footer_update(lease, host_handle, custom_input));
    content = shown(in_game_label);
    assert(has(content, u"Variable_L_Stick_LR") && has(content, u"Variable_L_Button") && has(content, u"Back"));
    assert(!has(content, u"Exit") && !has(content, u"Inspect") && !has(content, u"ZR_Button"));
    ui_footer_release(lease); assert(shown(in_game_label) == revert_context);

    const uint64_t derived = type("DerivedArrow"); fake::set_super(derived, arrow_type);
    const uint64_t arrow = widget(derived), unrelated = widget(label_type);
    assert(!ui_feedback_focus(game::to_handle(arrow)));
    const uint64_t sound = fake::memory(0x100);
    fake::put<uint64_t>(game::main_base() + t::data::sound_system, sound);
    fake::put<uint64_t>(game::main_base() + t::data::ui_select_sound, select_key);
    fake::put<uint8_t>(sound + t::sound_system::started, 1);
    assert(!ui_feedback_focus(game::to_handle(arrow)));
    fake::put<uint8_t>(sound + t::sound_system::ready, 1);
    assert(ui_feedback_focus(game::to_handle(arrow)) && focus_calls == 1);
    assert(ui_feedback_arrow(game::to_handle(arrow), false));
    assert(ui_feedback_arrow(game::to_handle(arrow), true));
    assert(left == 1 && right == 1);
    assert(!ui_feedback_arrow(game::to_handle(unrelated), true));
    const auto old_arrow = game::to_handle(arrow); fake::destroy(arrow);
    assert(!ui_feedback_focus(old_arrow) && focus_calls == 1);
    assert(!ui_feedback_arrow(old_arrow, false) && left == 1 && right == 1);
    // Each generated form invokes this only for a changed accepted value;
    // stock Settings adjustment is exercised separately and never calls here.
    unsigned live_texts = 0;
    for (const uint64_t counter : counters) {
        const auto holders = fake::get<int32_t>(counter + t::shared_count::holders);
        assert(holders == 0 || holders == 1);
        if (holders) ++live_texts;
    }
    assert(live_texts == 2); // only the two surviving labels retain any FText
    puts("Scoped localized footer leases and native arrow feedback: stand-in checks passed");
}
