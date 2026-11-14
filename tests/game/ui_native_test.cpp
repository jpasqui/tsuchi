#include "fake_game.h"
#include "ui_native.h"
#include "ui_native_classes.h"
#include "ui_screens.h"
#include "ui_mod_list_native.h"
#include "ui_feedback.h"
#include "ui_rich_text.h"
#include "hooks.h"
#include "native_routes.h"
#include "objects.h"
#include "shared.h"
#include "ui_form_layout_cases.h"
#ifdef TSUCHI_TEST_INPUT
#include "input_service.h"
#endif
#include <cassert>
#include <array>
#include <cmath>
#include <map>
#include <set>
#include <string>
#include <vector>

using namespace tsuchi;
namespace t = game::table;
namespace {
struct Array { uint64_t data; int32_t count, capacity; };
struct Margin { float left, top, width, height; };
struct Anchors { float left, top, right, bottom; };
struct Alignment { float x, y; };
const UiRenderer* renderer = nullptr;
const UiScreen* active_screen = nullptr;
void* active_view = nullptr;
uint64_t active_generation = 0;
bool model_actions = false, screen_sync_busy = false;
unsigned screen_sync_calls = 0, screen_sync_busy_skips = 0;
void (*screen_sync_callback)() = nullptr;
NativeHandler action_route = nullptr;
using Action = bool (*)(void*, uint32_t);
Action intercepted = nullptr;
void (*intercepted_npad)(void*) = nullptr;
uint64_t thread = 1, root_class, methods, host_type, category_type, row_type, label_type, panel_type;
uint64_t canvas_type, slot_type;
uint64_t host, host_handle, tree, root, scroll, scroll_panel, old_category, old_data;
uint64_t replacement_source = 0;
Array baseline{};
std::map<uint64_t, uint32_t> visibility;
std::map<uint64_t, std::u16string> labels;
std::map<uint64_t, uint32_t> justification;
std::map<uint64_t, bool> auto_size;
std::map<uint64_t, std::pair<float, float>> label_geometry;
std::map<uint64_t, std::array<float, 13>> arranged_geometry;
std::map<uint64_t, uint64_t> geometry_storage;
std::map<uint64_t, std::vector<uint64_t>> panel_children;
std::vector<uint64_t> categories, rows, selectors, native_rows;
std::vector<std::pair<int32_t, int32_t>> focus_history;
std::map<uint64_t, std::set<uint64_t>> pins;
std::map<uint64_t, uint64_t> borrowed_sources;
std::vector<uint64_t> released;
std::vector<std::string> reports;
float geometry_width = 0, geometry_height = 0;
uint64_t next_group = 1;
unsigned widget_calls = 0, construct_calls = 0, attach_calls = 0, label_calls = 0;
unsigned fail_widget_at = 0, fail_attach_at = 0, fail_pin_at = 0, pin_calls = 0;
unsigned refresh_calls = 0, focus_calls = 0, reveal_calls = 0, transform_calls = 0, scrollbar_calls = 0, redraw_calls = 0;
uint32_t last_reveal = 0;
unsigned host_queries = 0, footer_updates = 0, footer_releases = 0, focus_sounds = 0, arrow_calls = 0;
unsigned rich_text_syncs = 0;
unsigned feedback_cues = 0;
std::vector<uint64_t> feedback_targets;
unsigned borrow_calls = 0, return_source_calls = 0;
unsigned unpublished_detach_calls = 0;
bool host_cached = true, footer_ready = true, arrow_increment = false, action_changes = false;
bool zero_title_height = false;
bool physical_layout = false;
unsigned arrangements = 0;
UiFooterGuide last_guide{};
int32_t focused_row = -1;
void (*create_callback)() = nullptr;
void (*label_callback)() = nullptr;
void (*refresh_callback)() = nullptr;
void (*focus_callback)() = nullptr;
void (*redraw_callback)() = nullptr;
void (*reveal_callback)() = nullptr;
void (*footer_callback)() = nullptr;
void (*geometry_callback)() = nullptr;
void (*sync_callback)() = nullptr;
void (*engine_work)() = nullptr;
unsigned engine_registrations = 0;
void engine_frame() { assert(engine_work); engine_work(); }
void invoke(void (*&callback)()) { if (callback) { const auto saved = callback; callback = nullptr; saved(); } }
uint64_t field(uint64_t object, uint32_t offset) { return fake::get<uint64_t>(object + offset); }
std::array<float, 13> arranged(float width, float height, float x = 31, float y = 173) {
    return {width, height, .75f, x, y, 0, 0, .75f, 0, 0, .75f, x, y};
}
uint64_t object(uint64_t type_id = 0, const char* name = "widget", uint64_t outer = 0) {
    std::u16string wide; while (*name) wide.push_back(char16_t(*name++));
    const uint64_t made = fake::object(wide, type_id ? type_id : root_class, 0x900);
    fake::put<uint64_t>(made, methods); fake::put<uint64_t>(made + t::object::outer, outer);
    fake::put<float>(made + t::ui_widget::render_scale_x, 1);
    fake::put<float>(made + t::ui_widget::render_scale_y, 1);
    visibility[made] = t::constant::widget_shown; return made;
}
uint64_t type(const char* name) {
    std::u16string wide; while (*name) wide.push_back(char16_t(*name++));
    return fake::object(wide, root_class, 0x100);
}
void test_native_class_seed() {
    assert(ui_native_class_detail::native_metaclass() == root_class);
    const char* expected = "NativeSeedCanonical";
    const uint64_t canonical = type(expected), blueprint = type("NativeSeedCanonical_C");
    fake::set_super(blueprint, canonical);
    game::ClassRef seeded{expected};
    assert(ui_native_seed_class(seeded, blueprint));
    assert(seeded.found == canonical && seeded.handle == game::to_handle(canonical));
    assert(seeded.found != blueprint && game::is_a(object(blueprint), seeded));
    // A valid cache supplies the expected class; is_a still decides whether
    // each actual object belongs to that hierarchy.
    const uint64_t unrelated = type("NativeSeedUnrelated");
    assert(ui_native_seed_class(seeded, unrelated));
    assert(!game::is_a(object(unrelated), seeded));

    const auto empty = [](const game::ClassRef& ref) { assert(!ref.found && !ref.handle); };
    // Blueprint-generated classes satisfy is_class and may even reuse the
    // native short name. Only their exact native-metaclass ancestor is seeded.
    const char* collision_name = "NativeSeedCollisionBase";
    const uint64_t collision_native = type(collision_name), blueprint_meta = type("BlueprintGeneratedClass");
    fake::set_super(blueprint_meta, root_class);
    const uint64_t collision = fake::object(u"NativeSeedCollisionBase", blueprint_meta, 0x100);
    fake::set_super(collision, collision_native);
    assert(game::is_class(collision) && game::class_of(collision) == blueprint_meta);
    game::ClassRef collision_ref{collision_name};
    assert(ui_native_seed_class(collision_ref, collision) && collision_ref.found == collision_native);
    game::ClassRef cached_collision{collision_name, collision, game::to_handle(collision)};
    assert(!ui_native_seed_class(cached_collision, 0)); empty(cached_collision);
    cached_collision = {collision_name, collision, game::to_handle(collision)};
    assert(ui_native_seed_class(cached_collision, collision) && cached_collision.found == collision_native);
    const uint64_t orphan = fake::object(u"NativeSeedCollisionBase", blueprint_meta, 0x100);
    game::ClassRef orphan_ref{collision_name};
    assert(game::is_class(orphan) && !ui_native_seed_class(orphan_ref, orphan)); empty(orphan_ref);
    // The earlier native class wins ordinary lookup; a same-name orphan is
    // still outside that hierarchy when the existing is_a fallback runs.
    assert(!game::is_a(object(orphan), orphan_ref) && orphan_ref.found == collision_native);

    game::ClassRef wrong{expected};
    assert(!ui_native_seed_class(wrong, unrelated)); empty(wrong);
    const uint64_t misleading = type("NativeSeedCanonical_C_Only");
    assert(!ui_native_seed_class(wrong, misleading)); empty(wrong);

    const uint64_t ordinary = type("NativeSeedOrdinaryType");
    const uint64_t impostor = fake::object(u"NativeSeedCanonical", ordinary, 0x100);
    assert(!game::is_class(impostor));
    assert(!ui_native_seed_class(wrong, impostor)); empty(wrong);
    const uint64_t malformed = type("NativeSeedMalformed");
    fake::set_super(malformed, impostor); fake::set_super(impostor, canonical);
    assert(!ui_native_seed_class(wrong, malformed)); empty(wrong);
    fake::set_super(malformed, fake::memory(0x100));
    assert(!ui_native_seed_class(wrong, malformed)); empty(wrong);

    const uint64_t cycle_a = type("NativeSeedCycleA"), cycle_b = type("NativeSeedCycleB");
    fake::set_super(cycle_a, cycle_b); fake::set_super(cycle_b, cycle_a);
    fake::measured_read_address = cycle_a + t::struct_type::super; fake::measured_reads = 0;
    assert(!ui_native_seed_class(wrong, cycle_a)); empty(wrong);
    assert(fake::measured_reads > 0 && fake::measured_reads <= 64);
    std::array<uint64_t, 65> deep{};
    for (uint64_t& entry : deep) entry = type("NativeSeedDeep");
    for (size_t i = 0; i < deep.size(); ++i) fake::set_super(deep[i], i + 1 < deep.size() ? deep[i + 1] : canonical);
    fake::measured_read_address = canonical + t::object::name; fake::measured_reads = 0;
    assert(!ui_native_seed_class(wrong, deep[0])); empty(wrong);
    assert(!fake::measured_reads); // The canonical class lies beyond the bounded walk.
    assert(ui_native_seed_class(wrong, deep[2]) && wrong.found == canonical);
    assert(fake::measured_reads == 1); // The exact ancestor at position63 remains eligible.
    fake::measured_read_address = 0;

    // Validate cached kind and exact name, not only the cached object's life.
    game::ClassRef bad_name{expected, unrelated, game::to_handle(unrelated)};
    assert(!ui_native_seed_class(bad_name, 0)); empty(bad_name);
    game::ClassRef bad_kind{expected, impostor, game::to_handle(impostor)};
    assert(!ui_native_seed_class(bad_kind, 0)); empty(bad_kind);
    const uint64_t stale_handle = seeded.handle;
    game::end_life(canonical);
    assert(!game::from_handle(stale_handle) && game::live(canonical));
    assert(!ui_native_seed_class(seeded, 0)); empty(seeded);
    fake::destroy(canonical);
    const uint64_t replacement = type(expected), replacement_object = object(replacement);
    fake::measured_read_address = root_class + t::object::name; fake::measured_reads = 0;
    assert(game::is_a(replacement_object, seeded));
    assert(seeded.found == replacement && seeded.handle == game::to_handle(replacement));
    assert(fake::measured_reads > 0); // An unavailable seed leaves ordinary global lookup intact.
    fake::measured_read_address = 0; fake::measured_reads = 0;
}
void slots_for(uint64_t panel) {
    const auto& list = panel_children[panel]; const uint64_t data = list.empty() ? 0 : fake::memory(list.size() * 8);
    for (size_t i = 0; i < list.size(); ++i) {
        const uint64_t slot = object(); fake::put<uint64_t>(data + i * 8, slot);
        fake::put<uint64_t>(slot + t::ui_panel_slot::content, list[i]);
        fake::put<uint64_t>(slot + t::ui_mod_list_tab::parent, panel);
        fake::put<uint64_t>(list[i] + t::ui_mod_list_tab::slot, slot);
    }
    fake::put<Array>(panel + t::ui_panel::slots, {data, int32_t(list.size()), int32_t(list.size())});
}
void* add_child(void* pointer, void* child_pointer) {
    if (++attach_calls == fail_attach_at) return nullptr;
    const uint64_t parent = reinterpret_cast<uint64_t>(pointer), widget = reinterpret_cast<uint64_t>(child_pointer);
    // Forms attach real rows to their category, never an overlay to the host.
    assert(parent != root && parent != scroll_panel);
    panel_children[parent].push_back(widget); slots_for(parent);
    const Array slots = fake::get<Array>(parent + t::ui_panel::slots);
    return reinterpret_cast<void*>(fake::get<uint64_t>(slots.data + uint64_t(slots.count - 1) * 8));
}
void set_visible(void* pointer, uint32_t value) { visibility[reinterpret_cast<uint64_t>(pointer)] = value; }
uint32_t get_visibility(void* pointer) { return visibility[reinterpret_cast<uint64_t>(pointer)]; }
void* construct(const void*) { ++construct_calls; assert(false && "forms must use native Settings categories"); return nullptr; }
void* create_widget(void* owner, void* kind, void* name) {
    assert(reinterpret_cast<uint64_t>(owner) == host && !name);
    if (++widget_calls == fail_widget_at) return nullptr;
    const uint64_t type_id = reinterpret_cast<uint64_t>(kind), made = object(type_id);
    if (type_id == category_type) {
        const uint64_t own_tree = object(), panel = object(panel_type, t::name::ui_mod_list_panel_child, own_tree);
        const uint64_t title = object(label_type, t::name::ui_mod_list_title_child, own_tree);
        fake::put<uint64_t>(made + t::ui_host::tree, own_tree);
        fake::put<uint64_t>(own_tree + t::ui_tree::root, panel);
        label_geometry[title] = {1468, 85};
        panel_children[panel].push_back(title); slots_for(panel);
        fake::put<uint64_t>(made + t::ui_mod_list_category::row_class, row_type);
        // CreateWidget's native title/panel fields start unbound in the game.
        categories.push_back(made);
    } else { assert(type_id == row_type); rows.push_back(made); }
    invoke(create_callback); return reinterpret_cast<void*>(made);
}
void bind_row(void* pointer) {
    const uint64_t row = reinterpret_cast<uint64_t>(pointer), selector = object();
    const uint64_t title = object(label_type), canvas = object(canvas_type), title_slot = object(slot_type), selector_slot = object(slot_type);
    fake::put<uint64_t>(row + t::ui_mod_list_row::title, title);
    fake::put<uint64_t>(row + t::ui_mod_list_row::selector, selector);
    for (const auto& item : {std::pair<uint64_t, uint64_t>{title, title_slot}, {selector, selector_slot}}) {
        fake::put<uint64_t>(item.first + t::ui_mod_list_tab::slot, item.second);
        fake::put<uint64_t>(item.second + t::ui_mod_list_tab::parent, canvas);
        fake::put<uint64_t>(item.second + t::ui_panel_slot::content, item.first);
        fake::put<Anchors>(item.second + t::ui_mod_list_tab::anchors, {0, 0, 0, 0});
        fake::put<Alignment>(item.second + t::ui_mod_list_tab::alignment, {.5f, .5f});
        auto_size[item.second] = true;
    }
    fake::put<Anchors>(title_slot + t::ui_mod_list_tab::anchors, {.5f, .5f, .5f, .5f});
    fake::put<Margin>(title_slot + t::ui_mod_list_tab::offsets, {20, -10, 500, zero_title_height ? 0.f : 120.f});
    fake::put<float>(title + t::ui_widget::render_y, -2);
    // The captured broken renderer froze the authored box at120. Native
    // AutoSize later returns it to the paragraph's measured desired height.
    auto_size[title_slot] = false;
    label_geometry[title] = {500, zero_title_height ? 0.f : 120.f};
    arranged_geometry[row] = arranged(1468, 120);
    fake::put<uint8_t>(title + t::ui_rich_text::wrapping_policy, 0);
    fake::put<uint8_t>(title + t::ui_rich_text::auto_wrap, 0xA1);
    fake::put<float>(title + t::ui_rich_text::wrap_width, 0);
    fake::put<Margin>(selector_slot + t::ui_mod_list_tab::offsets, {560, 0, 870, 112});
    fake::put<uint64_t>(title + t::ui_rich_text::slate, fake::memory(0x100));
    fake::put<uint64_t>(selector + t::ui_mod_list_selector::inner, object());
    fake::put<uint64_t>(selector + t::ui_mod_list_selector::value, object(label_type));
    fake::put<uint64_t>(selector + t::ui_mod_list_selector::left, object());
    fake::put<uint64_t>(selector + t::ui_mod_list_selector::right, object());
    selectors.push_back(selector);
}
struct TextRecord { std::u16string value; int references; };
std::map<uint64_t, TextRecord> texts;
uint64_t next_text = 1;
game::EngineText make_text(const std::u16string& text) {
    const uint64_t token = next_text++;
    texts[token] = {text, 1}; game::EngineText result;
    memcpy(result.bytes, &token, 8); memcpy(result.bytes + t::text_value::counter, &token, 8); return result;
}
std::u16string text_value(const game::EngineText& text) {
    const auto found = texts.find(text.counter()); assert(found != texts.end() && found->second.references > 0);
    return found->second.value;
}
game::EngineText text_from_string(const void* pointer) {
    const uint64_t at = reinterpret_cast<uint64_t>(pointer), data = field(at, t::array::data);
    const int32_t count = fake::get<int32_t>(at + t::array::count);
    assert(count > 0 && fake::get<int32_t>(at + t::array::max) == count);
    return make_text({reinterpret_cast<const char16_t*>(data), size_t(count - 1)});
}
void set_text(void* pointer, void* value) {
    labels[reinterpret_cast<uint64_t>(pointer)] = text_value(*static_cast<game::EngineText*>(value));
    ++label_calls; invoke(label_callback);
}
void set_offsets(void* pointer, float left, float top, float width, float height) {
    fake::put<Margin>(reinterpret_cast<uint64_t>(pointer) + t::ui_mod_list_tab::offsets, {left, top, width, height});
}
void set_anchors(void* pointer, float left, float top, float right, float bottom) {
    fake::put<Anchors>(reinterpret_cast<uint64_t>(pointer) + t::ui_mod_list_tab::anchors, {left, top, right, bottom});
}
void set_alignment(void* pointer, float x, float y) {
    fake::put<Alignment>(reinterpret_cast<uint64_t>(pointer) + t::ui_mod_list_tab::alignment, {x, y});
}
void set_auto_size(void* pointer, bool value) { auto_size[reinterpret_cast<uint64_t>(pointer)] = value; }
void set_justification(void* pointer, uint32_t value) { justification[reinterpret_cast<uint64_t>(pointer)] = value; }
void rich_text_sync(void* pointer) { assert(field(reinterpret_cast<uint64_t>(pointer), t::ui_rich_text::slate)); ++rich_text_syncs; invoke(sync_callback); }
void append_choice(void* pointer, void* value) {
    const uint64_t selector = field(reinterpret_cast<uint64_t>(pointer), t::ui_mod_list_row::selector);
    const Array before = fake::get<Array>(selector + t::ui_mod_list_selector::choices);
    const uint64_t data = fake::memory(uint64_t(before.count + 1) * sizeof(game::EngineText));
    if (before.count) memcpy(reinterpret_cast<void*>(data), reinterpret_cast<void*>(before.data), size_t(before.count) * sizeof(game::EngineText));
    const auto& text = *static_cast<game::EngineText*>(value); ++texts[text.counter()].references;
    fake::put(data + uint64_t(before.count) * sizeof(game::EngineText), text);
    fake::put<Array>(selector + t::ui_mod_list_selector::choices, {data, before.count + 1, before.count + 1});
}
bool valid_choice(void* pointer, int32_t index) {
    const Array choices = fake::get<Array>(reinterpret_cast<uint64_t>(pointer) + t::ui_mod_list_selector::choices);
    return index >= 0 && index < choices.count;
}
void redraw(void* pointer) {
    const uint64_t selector = reinterpret_cast<uint64_t>(pointer);
    const uint32_t index = fake::get<uint32_t>(selector + t::ui_mod_list_selector::choice);
    assert(valid_choice(pointer, int32_t(index)));
    const Array choices = fake::get<Array>(selector + t::ui_mod_list_selector::choices);
    const auto text = fake::get<game::EngineText>(choices.data + uint64_t(index) * sizeof(game::EngineText));
    labels[field(selector, t::ui_mod_list_selector::value)] = text_value(text);
    ++redraw_calls; invoke(redraw_callback);
}
Array source() { return fake::get<Array>(host + t::ui_mod_list_menu::source); }
void arrange_native() {
    if (!physical_layout) return;
    const Array current = source();
    if (current.count != 1) return;
    const uint64_t category = fake::get<uint64_t>(current.data);
    const uint64_t heading = field(category, t::ui_mod_list_category::title);
    const uint64_t panel = field(category, t::ui_mod_list_category::panel);
    if (!heading || !panel) return;
    ++arrangements;
    const float translation = fake::get<float>(scroll_panel + t::ui_mod_list_scroll::y);
    const float origin = 173 + translation * .75f;
    arranged_geometry[heading] = arranged(1468, 85, 31, origin);
    float top = 85;
    for (uint64_t row : native_rows) {
        const uint64_t title = field(row, t::ui_mod_list_row::title);
        float desired_height = 56, desired_width = 500;
        if (!fake::get<uint8_t>(row + t::ui_mod_list_row::enabled)) {
            const float wrap = fake::get<float>(title + t::ui_rich_text::wrap_width);
            const float natural_width = float(labels[title].size()) * 14;
            const bool enabled = fake::get<uint8_t>(title + t::ui_rich_text::wrapping_policy) == 1 &&
                !(fake::get<uint8_t>(title + t::ui_rich_text::auto_wrap) & 1) && wrap > 0;
            const unsigned lines = enabled ? unsigned(std::ceil(natural_width / wrap)) : 1;
            desired_height = 36 * float(lines ? lines : 1);
            desired_width = enabled && natural_width > wrap ? wrap : natural_width;
        }
        const uint64_t slot = field(title, t::ui_mod_list_tab::slot);
        const Anchors anchors = fake::get<Anchors>(slot + t::ui_mod_list_tab::anchors);
        const Alignment alignment = fake::get<Alignment>(slot + t::ui_mod_list_tab::alignment);
        const Margin offsets = fake::get<Margin>(slot + t::ui_mod_list_tab::offsets);
        assert(anchors.top == anchors.bottom);
        const float height = auto_size[slot] ? desired_height : offsets.height;
        const float width = auto_size[slot] ? desired_width : offsets.width;
        // The native selector supplies112; an auto-sized paragraph contributes
        // its natural desired height. No invented message padding grows rows.
        const float row_height = height > 112 ? height : 112;
        const float local_top = row_height * anchors.top + offsets.top - height * alignment.y;
        const float local_left = 1468 * anchors.left + offsets.left - width * alignment.x;
        const float render_y = fake::get<float>(title + t::ui_widget::render_y);
        arranged_geometry[row] = arranged(1468, row_height, 31, origin + top * .75f);
        arranged_geometry[title] = arranged(width, height, 31 + local_left * .75f,
                                            origin + (top + local_top + render_y) * .75f);
        top += row_height;
    }
    arranged_geometry[category] = arranged(1468, top, 31, origin);
    arranged_geometry[panel] = arranged(1468, top, 31, origin);
    arranged_geometry[scroll_panel] = arranged(1468, top, 31, origin);
}
void refresh(void* pointer) {
    assert(reinterpret_cast<uint64_t>(pointer) == host); ++refresh_calls; native_rows.clear();
    const Array current = source(); assert(current.count >= 0 && current.count <= current.capacity);
    for (int32_t i = 0; i < current.count; ++i) {
        const uint64_t category = fake::get<uint64_t>(current.data + uint64_t(i) * 8);
        assert(game::live(category)); const Array category_rows = fake::get<Array>(category + t::ui_mod_list_category::rows);
        assert(category_rows.count >= 0 && category_rows.count <= category_rows.capacity);
        for (int32_t j = 0; j < category_rows.count; ++j) native_rows.push_back(fake::get<uint64_t>(category_rows.data + uint64_t(j) * 8));
    }
    const uint64_t active_data = current.count ? fake::memory(uint64_t(current.count) * 8) : 0;
    for (int32_t i = 0; i < current.count; ++i)
        fake::put<uint64_t>(active_data + uint64_t(i) * 8, fake::get<uint64_t>(current.data + uint64_t(i) * 8));
    fake::put<Array>(host + t::ui_mod_list_menu::active, {active_data, current.count, current.count});
    const uint64_t height_data = fake::memory((native_rows.size() + 1) * sizeof(float));
    fake::put<float>(height_data, 63);
    for (size_t i = 0; i < native_rows.size(); ++i) fake::put<float>(height_data + (i + 1) * sizeof(float), 112);
    fake::put<Array>(scroll + t::ui_mod_list_scroll::heights,
        {height_data, int32_t(native_rows.size() + 1), int32_t(native_rows.size() + 1)});
    fake::put<int32_t>(scroll + t::ui_mod_list_scroll::extent_count, int32_t(native_rows.size() + 1));
    fake::put<uint8_t>(scroll + t::ui_mod_list_scroll::variable_height, 1);
    fake::put<float>(scroll + t::ui_mod_list_scroll::spacing, 0);
    const uint64_t category = current.count == 1 ? fake::get<uint64_t>(current.data) : 0;
    const bool generated = category && field(category, t::ui_mod_list_category::title);
    fake::put<float>(scroll + t::ui_mod_list_scroll::total, generated ? 63 + 112 * float(native_rows.size()) : 3000);
    auto& scroll_children = panel_children[scroll_panel]; scroll_children.clear();
    for (int32_t i = 0; i < current.count; ++i) scroll_children.push_back(fake::get<uint64_t>(current.data + uint64_t(i) * 8));
    slots_for(scroll_panel);
    arrange_native();
    invoke(refresh_callback);
}
void replace_source() {
    assert(replacement_source);
    fake::put<Array>(host + t::ui_mod_list_menu::source, {replacement_source, 1, 1});
    fake::put<int32_t>(host + t::ui_mod_list_menu::selected, 2);
    fake::put<int32_t>(host + t::ui_mod_list_menu::previous, 1);
    refresh(reinterpret_cast<void*>(host));
}
void focus(void* pointer) {
    assert(reinterpret_cast<uint64_t>(pointer) == host); ++focus_calls;
    const int32_t selected = fake::get<int32_t>(host + t::ui_mod_list_menu::selected);
    focus_history.push_back({selected, fake::get<int32_t>(host + t::ui_mod_list_menu::previous)});
    // Stock focus's enabled-row scan must never see an all-message screen.
    assert(selected >= 0 && uint32_t(selected) < native_rows.size());
    assert(fake::get<uint8_t>(native_rows[uint32_t(selected)] + t::ui_mod_list_row::enabled));
    if (focused_row != selected) ++focus_sounds;
    focused_row = selected;
    fake::put<float>(scroll_panel + t::ui_mod_list_scroll::x, 0);
    fake::put<float>(scroll_panel + t::ui_mod_list_scroll::y, float(-selected * 100));
    invoke(focus_callback);
}
void reveal(void* pointer, uint32_t index) {
    assert(reinterpret_cast<uint64_t>(pointer) == scroll); last_reveal = index; ++reveal_calls;
    if (physical_layout) {
        const Array heights = fake::get<Array>(scroll + t::ui_mod_list_scroll::heights);
        assert(index < uint32_t(heights.count));
        const float spacing = fake::get<float>(scroll + t::ui_mod_list_scroll::spacing);
        float top = 0;
        for (uint32_t i = 0; i < index; ++i) top += fake::get<float>(heights.data + i * sizeof(float)) + spacing;
        const float height = fake::get<float>(heights.data + index * sizeof(float));
        const float viewport = fake::get<float>(scroll + t::ui_mod_list_scroll::viewport);
        const float total = fake::get<float>(scroll + t::ui_mod_list_scroll::total);
        const float before = -fake::get<float>(scroll_panel + t::ui_mod_list_scroll::y);
        float after = before;
        if (top < before) after = top;
        else if (top + height > before + viewport) after = top + height - viewport;
        const float maximum = total > viewport ? total - viewport : 0;
        if (after < 0) after = 0;
        if (after > maximum) after = maximum;
        fake::put<float>(scroll_panel + t::ui_mod_list_scroll::y, -after);
        arrange_native();
    }
    invoke(reveal_callback);
}
void transform(void* pointer) { assert(reinterpret_cast<uint64_t>(pointer) == scroll_panel); ++transform_calls; }
void scrollbar(void* pointer) { assert(reinterpret_cast<uint64_t>(pointer) == scroll); ++scrollbar_calls; }
void* geometry(void* pointer) {
    const uint64_t widget = reinterpret_cast<uint64_t>(pointer);
    std::array<float, 13> value = arranged(0, 0);
    if (widget == scroll) value = arranged(geometry_width, geometry_height);
    else if (const auto found = arranged_geometry.find(widget); found != arranged_geometry.end()) value = found->second;
    else if (const auto found = label_geometry.find(widget); found != label_geometry.end()) value = arranged(found->second.first, found->second.second);
    uint64_t& result = geometry_storage[widget];
    if (!result) result = fake::memory(0x100);
    fake::put(result, value);
    invoke(geometry_callback);
    return reinterpret_cast<void*>(result);
}
void close_active() { void* view = active_view; ++active_generation; active_view = nullptr; active_screen = nullptr; renderer->close(view); }
void detach_active_host() { assert(renderer->detach_host && active_view); renderer->detach_host(active_view, host_handle); }
void detach_unpublished_host() { assert(!active_view); ++unpublished_detach_calls; ui_native_host_detach(host_handle); }
void close_again() { renderer->close(active_view); }
void open_during_close() { void* attempted = nullptr; assert(renderer->open(*active_screen, &attempted) == TSUCHI_NOT_READY && !attempted); }
void stop_unpublished() { mod_stop(active_screen->mod); }
void stop_active() { mod_stop(active_screen->mod); close_active(); }
void clear_footer_from_callback() { assert(active_screen); const_cast<UiScreen*>(active_screen)->footer.clear(); }
void reset_baseline() {
    fake::put(host + t::ui_mod_list_menu::source, baseline);
    fake::put<int32_t>(host + t::ui_mod_list_menu::selected, 2);
    fake::put<int32_t>(host + t::ui_mod_list_menu::previous, 1);
    fake::put<float>(scroll_panel + t::ui_mod_list_scroll::x, 7);
    fake::put<float>(scroll_panel + t::ui_mod_list_scroll::y, -31);
}
void assert_baseline() {
    assert(source().data == baseline.data && source().count == baseline.count && source().capacity == baseline.capacity);
    assert(fake::get<int32_t>(host + t::ui_mod_list_menu::selected) == 2);
    assert(fake::get<int32_t>(host + t::ui_mod_list_menu::previous) == 1);
    assert(fake::get<float>(scroll_panel + t::ui_mod_list_scroll::x) == 7);
    assert(fake::get<float>(scroll_panel + t::ui_mod_list_scroll::y) == -31);
    assert(visibility[scroll] == 3 && !construct_calls);
    assert(borrowed_sources.empty() && borrow_calls == return_source_calls);
}
void make_screen(UiScreen& screen, TSUCHI_Mod* mod) {
    screen.id = 10; screen.mod = mod; screen.title.set("Garden"); screen.settings_tab = true; screen.resettable = true;
    for (unsigned i = 0; i < 7; ++i) {
        UiControl control{}; control.kind = i == 0 ? UiKind::choice : i == 1 ? UiKind::integer : i == 2 ? UiKind::text : UiKind::button;
        control.label.set(i == 0 ? "Difficulty" : i == 1 ? "Count" : i == 2 ? "Note" : "Go");
        control.text.set("Ready"); control.labels.add("Normal"); control.labels.add("Hard");
        control.selected = 1; control.minimum = INT64_MIN; control.maximum = INT64_MAX; control.step = 1; control.integer = INT64_MIN;
        assert(screen.controls.add(control));
    }
}
void clear_screen(UiScreen& screen) {
    for (uint32_t i = 0; i < screen.controls.count; ++i) {
        screen.controls[i].label.clear(); screen.controls[i].text.clear(); screen.controls[i].choices.clear(); screen.controls[i].labels.clear();
    }
    screen.controls.clear(); screen.title.clear(); screen.footer.clear();
}
void begin(UiScreen& screen) { active_screen = &screen; assert(renderer->open(screen, &active_view) == TSUCHI_OK && active_view); ++active_generation; }
UiScreen* successor_screen = nullptr;
void show_successor() { close_active(); assert(successor_screen); begin(*successor_screen); assert(renderer->update(active_view, *successor_screen)); }
void read_during_reveal() { assert(intercepted(reinterpret_cast<void*>(host), 1)); }
void sync_during_sync() {
    assert(active_screen && active_view);
    assert(!ui_sync_screen(active_screen->id, active_view));
}
uint64_t active_category() { const Array current = source(); assert(current.count == 1); return fake::get<uint64_t>(current.data); }
uint64_t active_row(uint32_t index) {
    const Array items = fake::get<Array>(active_category() + t::ui_mod_list_category::rows); assert(index < uint32_t(items.count));
    return fake::get<uint64_t>(items.data + uint64_t(index) * 8);
}
uint64_t active_selector(uint32_t index) { return field(active_row(index), t::ui_mod_list_row::selector); }
unsigned npad_calls = 0, provider_reads = 0, input_registrations = 0;
uint64_t provided_buttons = 0;
bool provider_ready = true, registrar_accepts = false, input_history_committed = false;
void (*registered_input_step)() = nullptr;
void (*provider_callback)() = nullptr;
void (*original_callback)() = nullptr;
TSUCHI_Result hook_result = TSUCHI_OK;
bool original_result = true;
bool back_closes = false;
uint32_t original_which = UINT32_MAX, effective_which = UINT32_MAX, accepted_which = UINT32_MAX;
unsigned original_calls = 0, accepted_calls = 0, listener_calls = 0, dispatcher_calls = 0;
uint64_t listener_buttons = 0;
bool dispatcher_handled = true;
void listener(uint64_t buttons) { ++listener_calls; listener_buttons = buttons; }
bool dispatcher(uint64_t handle, uint32_t, bool& handled, bool& result) {
    assert(handle == game::to_handle(host)); ++dispatcher_calls; handled = dispatcher_handled; result = true; return true;
}
void original_npad(void*) { ++npad_calls; }
bool register_input_step(void (*step)()) {
    assert(!input_history_committed); ++input_registrations;
    if (registrar_accepts) registered_input_step = step; return registrar_accepts;
}
bool read_buttons(uint32_t player, uint64_t& buttons) { ++provider_reads; assert(!player); buttons = provided_buttons; invoke(provider_callback); return provider_ready; }
bool original(void* pointer, uint32_t which) {
    ++original_calls; original_which = which; invoke(original_callback); if (!original_result) return false;
    NativeFrame frame{}; frame.x[19] = reinterpret_cast<uint64_t>(pointer); frame.x[20] = which;
    if (action_route) action_route(frame); effective_which = uint32_t(frame.x[20]);
    assert(frame.x[20] == which || frame.x[20] == 8); return true;
}
#ifdef TSUCHI_TEST_INPUT
TSUCHI_Buttons service_held = 0, service_hidden = 0;
bool hidden_ready = true;
TSUCHI_Result service_pad(uint32_t player, TSUCHI_PadState* out) { assert(!player); *out = {}; out->connected = true; out->held = service_held; return TSUCHI_OK; }
TSUCHI_Result service_hidden_mask(uint32_t player, TSUCHI_Buttons* out) { assert(!player); if (!hidden_ready) return TSUCHI_NOT_READY; *out = service_hidden; return TSUCHI_OK; }
const input::Backend input_backend{service_pad,nullptr,nullptr,nullptr,nullptr,nullptr,nullptr,service_hidden_mask};
#endif
}
namespace tsuchi {
bool each_engine_frame(void (*work)()) {
    assert(work && (!engine_work || engine_work == work));
    ++engine_registrations; engine_work = work; return true;
}
#ifdef TSUCHI_TEST_INPUT
bool after_step(void (*step)()) { registered_input_step = step; return true; }
#endif
void mod_note(const char*, const char* message) { reports.emplace_back(message); }
void ui_set_renderer(const UiRenderer* value) { renderer = value; }
const UiScreen* ui_active_screen() { return active_screen; }
void* ui_active_view() { return active_view; }
// The real Showing/HoldTable lifecycle is covered by ui_screens_test.cpp. This
// renderer fixture admits only the exact active pair and prevents nested sync,
// then keeps a local generation so callbacks cannot update/close a successor.
bool ui_sync_screen(uint64_t id, void* view) {
    ++screen_sync_calls;
    if (!active_screen || active_screen->id != id || active_view != view) return false;
    if (screen_sync_busy) { ++screen_sync_busy_skips; return false; }
    const UiScreen* screen = active_screen; const uint64_t generation = active_generation;
    const UiRenderer captured = *renderer;
    const auto current = [&] { return active_generation == generation && active_screen == screen && active_view == view; };
    screen_sync_busy = true;
    invoke(screen_sync_callback);
    const bool okay = current() && captured.live(view) && captured.update(view, *screen);
    if (!okay && current()) close_active();
    screen_sync_busy = false;
    return okay && current();
}
bool ui_action(uint32_t which) {
    ++accepted_calls; accepted_which = which;
    if (which == 5 && back_closes) close_active();
    if (model_actions && active_screen && active_screen->selected < active_screen->controls.count) {
        UiScreen& screen = *const_cast<UiScreen*>(active_screen);
        if (which == 0 || which == 1) {
            uint32_t next = screen.selected;
            for (uint32_t i = 0; i < screen.controls.count; ++i) {
                next = which == 0 ? (next ? next - 1 : screen.controls.count - 1) : (next + 1) % screen.controls.count;
                if (screen.controls[next].kind == UiKind::text) continue;
                if (next != screen.selected) { screen.selected = next; ++screen.revision; }
                break;
            }
        } else if (which >= 2 && which <= 4) {
            UiControl& control = screen.controls[screen.selected];
            if (control.kind == UiKind::choice) {
                const uint32_t before = control.selected;
                uint32_t count = 0;
                for (const char* label = control.labels.first(); label; label = control.labels.after(label)) ++count;
                if (which == 2 && control.selected) --control.selected;
                else if (which != 2 && control.selected + 1 < count) ++control.selected;
                if (control.selected != before) ++screen.revision;
            } else if (control.kind == UiKind::integer) {
                const uint64_t distance = which == 2 ? uint64_t(control.integer) - uint64_t(control.minimum) : uint64_t(control.maximum) - uint64_t(control.integer);
                if (distance >= uint64_t(control.step)) {
                    control.integer = which == 2 ? int64_t(uint64_t(control.integer) - uint64_t(control.step)) : int64_t(uint64_t(control.integer) + uint64_t(control.step));
                    ++screen.revision;
                }
            }
        }
    } else if (action_changes && active_screen && active_screen->selected < active_screen->controls.count) {
        UiScreen& screen = *const_cast<UiScreen*>(active_screen);
        ++screen.controls[screen.selected].integer; ++screen.revision;
    }
    return true;
}
bool ui_route_action(uint64_t host_value, uint32_t which) { const auto saved = *renderer; return active_view && active_screen && saved.action && saved.action(active_view, host_value, which); }
void ui_route_buttons(uint64_t held) { const auto saved = *renderer; if (active_view && active_screen && saved.buttons) saved.buttons(active_view, held); }
uint64_t ui_mod_list_native_host() { ++host_queries; return host_cached ? game::to_handle(host) : 0; }
bool ui_mod_list_native_content(uint64_t handle, UiContentRect& out) { assert(handle == game::to_handle(host)); out = {31, 173, 1468, 840}; return true; }
uint64_t ui_mod_list_native_group() { return next_group++; }
bool ui_mod_list_native_pin(uint64_t handle, uint64_t group, uint64_t pointer) {
    assert(handle == game::to_handle(host) && group && game::live(pointer));
    if (++pin_calls == fail_pin_at) return false; pins[group].insert(pointer); return true;
}
void ui_mod_list_native_unpin(uint64_t handle, uint64_t group) { assert(handle == host_handle); pins.erase(group); }
bool ui_mod_list_native_borrow(uint64_t handle, uint64_t before, uint64_t replacement) {
    assert(handle == host_handle && replacement && !borrowed_sources.count(replacement));
    assert(source().data == before); borrowed_sources[replacement] = before; ++borrow_calls; return true;
}
void ui_mod_list_native_return_source(uint64_t handle, uint64_t replacement) {
    assert(handle == host_handle && borrowed_sources.erase(replacement) == 1); ++return_source_calls;
}
void ui_mod_list_native_footer_release(uint64_t handle) { assert(handle == game::to_handle(host)); }
bool ui_footer_update(UiFooterLease&, uint64_t handle, const UiFooterGuide& guide) { assert(handle == game::to_handle(host)); ++footer_updates; invoke(footer_callback); last_guide = guide; return footer_ready; }
void ui_footer_release(UiFooterLease&) { ++footer_releases; }
bool ui_feedback_focus(uint64_t widget) { assert(game::from_handle(widget)); ++focus_sounds; ++feedback_cues; feedback_targets.push_back(widget); return true; }
bool ui_retain_text(const game::EngineText&) { return true; }
bool ui_feedback_arrow(uint64_t selector, bool increment) {
    assert(active_screen && active_screen->selected < active_screen->controls.count);
    assert(selector == game::to_handle(active_selector(active_screen->selected)));
    ++arrow_calls; arrow_increment = increment; return true;
}
TSUCHI_Result hook_wrap(TSUCHI_Mod*, uint64_t entry, uint64_t stock, void* function, uint32_t flags, uint64_t* id, void** next) {
    assert(entry == fake::main_image() + t::slot::npad_read.table + t::slot::npad_read.offset);
    assert(stock == fake::main_image() + t::slot::npad_read.stock && !flags);
    intercepted_npad = reinterpret_cast<void (*)(void*)>(function); *next = reinterpret_cast<void*>(original_npad); *id = 2; return TSUCHI_OK;
}
bool native_route(unsigned site, NativeHandler handler) {
    assert(site == t::native_route::ui_settings_dispatch); if (hook_result != TSUCHI_OK) return false;
    action_route = handler; intercepted = original; t::fn::ui_settings_dispatch = original; return true;
}
}
namespace tsuchi::game {
void* engine_memory(uint64_t bytes) { return reinterpret_cast<void*>(fake::memory(size_t(bytes))); }
void release_engine_memory(void* pointer) { const uint64_t at = reinterpret_cast<uint64_t>(pointer); assert(readable(at, 1)); released.push_back(at); fake::unmap(at); }
void drop_shared(uint64_t counter) { if (counter) { auto found = texts.find(counter); assert(found != texts.end() && found->second.references > 0); --found->second.references; } }
bool utf8_to_units(const char* text, char16_t* out, size_t capacity, size_t& count) {
    count = std::strlen(text); if (count > capacity) return false;
    for (size_t i = 0; i < count; ++i) { if (static_cast<unsigned char>(text[i]) > 127) return false; out[i] = text[i]; } return true;
}
}
int main() {
    test_ui_form_layout();
    fake::start(); set_thread_probe([] { return thread; }); note_game_thread();
    root_class = fake::object(u"Class", 0, 0x100); methods = fake::memory(0x800);
    const uint64_t inventory_sentinel = fake::object(u"NamedClassInventorySentinel", root_class, 0x100);
    fake::put<uint64_t>(methods + t::widget_table::set_visibility, reinterpret_cast<uint64_t>(set_visible));
    fake::put<uint64_t>(methods + t::ui_mod_list_selector::valid, reinterpret_cast<uint64_t>(valid_choice));
    fake::put<uint64_t>(methods + t::ui_mod_list_selector::redraw, reinterpret_cast<uint64_t>(redraw));
    for (auto address : {reinterpret_cast<uint64_t>(set_visible), reinterpret_cast<uint64_t>(valid_choice), reinterpret_cast<uint64_t>(redraw)}) fake::code().push_back(address);
    host_type = type(t::name::ui_host_class); category_type = type(t::name::ui_mod_list_category_class); row_type = type(t::name::ui_mod_list_row_class);
    label_type = type(t::name::ui_mod_list_label_class); panel_type = type(t::name::ui_mod_list_panel_class);
    canvas_type = type(t::name::ui_canvas_class); slot_type = type(t::name::ui_mod_list_slot_class);
    fake::set_super(panel_type, type(t::name::ui_panel_base_class));
    test_native_class_seed();
    host = object(host_type); host_handle = game::to_handle(host); tree = object(); root = object(); scroll = object(); scroll_panel = object(panel_type);
    fake::put<uint64_t>(host + t::ui_host::tree, tree); fake::put<uint64_t>(tree + t::ui_tree::root, root);
    fake::put<uint64_t>(host + t::ui_host::scroll, scroll); fake::put<uint8_t>(host + t::ui_host::state, 1);
    fake::put<uint8_t>(host + t::ui_mod_list_menu::new_mode, 1); fake::put<uint64_t>(host + t::ui_mod_list_menu::category_class, category_type);
    fake::put<uint64_t>(scroll + t::ui_mod_list_scroll::panel, scroll_panel);
    fake::put<float>(scroll + t::ui_mod_list_scroll::total, 3000); fake::put<float>(scroll + t::ui_mod_list_scroll::viewport, 840);
    fake::put<float>(scroll_panel + t::ui_mod_list_scroll::x, 7); fake::put<float>(scroll_panel + t::ui_mod_list_scroll::y, -31); visibility[scroll] = 3;
    old_category = object(category_type); old_data = fake::memory(8); fake::put<uint64_t>(old_data, old_category);
    const uint64_t old_rows = fake::memory(3 * 8);
    for (unsigned i = 0; i < 3; ++i) { const uint64_t row = object(row_type); fake::put<uint8_t>(row + t::ui_mod_list_row::enabled, 1); fake::put<uint64_t>(old_rows + i * 8, row); }
    fake::put<Array>(old_category + t::ui_mod_list_category::rows, {old_rows, 3, 3});
    baseline = {old_data, 1, 1}; fake::put(host + t::ui_mod_list_menu::source, baseline);
    fake::put<int32_t>(host + t::ui_mod_list_menu::selected, 2); fake::put<int32_t>(host + t::ui_mod_list_menu::previous, 1);
    t::fn::ui_construct = construct; t::fn::ui_create_widget = create_widget; t::fn::ui_add_child = add_child;
    t::fn::ui_get_visibility = get_visibility; t::fn::text_from_string = text_from_string; t::fn::rich_text_set = set_text;
    t::fn::ui_slot_offsets = set_offsets; t::fn::ui_text_justification = set_justification; t::fn::ui_rich_text_sync = rich_text_sync;
    t::fn::ui_slot_anchors = set_anchors; t::fn::ui_slot_alignment = set_alignment; t::fn::ui_slot_auto_size = set_auto_size;
    t::fn::ui_mod_list_bind_row = bind_row; t::fn::ui_mod_list_append_choice = append_choice;
    t::fn::ui_mod_list_refresh = refresh; t::fn::ui_mod_list_focus = focus; t::fn::ui_mod_list_reveal = reveal;
    t::fn::ui_cached_geometry = geometry; t::fn::ui_mod_list_transform = transform; t::fn::ui_mod_list_scrollbar = scrollbar;
    TSUCHI_Mod* mod = mod_create("native.menu"); assert(mod);
    const auto& route = t::native_routes[t::native_route::ui_settings_dispatch];
    const uint32_t jump = 0x14000000u | (uint32_t((route.entry - route.address) >> 2) & 0x03ffffffu);
    assert(!ui_native_start(mod)); fake::patch_mark(); fake::put<uint32_t>(fake::main_image() + route.address, jump);
    UiScreen screen{}; make_screen(screen, mod); void* missing = nullptr;
    hook_result = TSUCHI_NOT_READY; ui_native_start(mod); assert(renderer->open(screen, &missing) == TSUCHI_NOT_READY);
    hook_result = TSUCHI_OK; ui_native_start(mod); assert(intercepted);
    fake::put<uint8_t>(host + t::ui_host::state, 0); assert(renderer->open(screen, &missing) == TSUCHI_NOT_READY);
    fake::put<uint8_t>(host + t::ui_host::state, 1);
    // This sentinel precedes all target classes. Canonical Class lookup stops
    // at root_class before it; target-class inventory walks pass through it.
    // The separate UClass-root bootstrap is primed by the contracts above.
    // Arm before any failed cold construction can populate renderer caches.
    fake::measured_read_address = inventory_sentinel + t::object::name; fake::measured_reads = 0;
    fail_widget_at = widget_calls + 2; assert(renderer->open(screen, &missing) == TSUCHI_NOT_READY); fail_widget_at = 0; assert_baseline(); assert(pins.empty());
    assert(!fake::measured_reads);
    fail_attach_at = attach_calls + 1; assert(renderer->open(screen, &missing) == TSUCHI_NOT_READY); fail_attach_at = 0; assert_baseline(); assert(pins.empty());
    fail_pin_at = pin_calls + 1; assert(renderer->open(screen, &missing) == TSUCHI_NOT_READY); fail_pin_at = 0; assert_baseline(); assert(pins.empty());
    const auto create_saved = t::fn::ui_create_widget; t::fn::ui_create_widget = nullptr;
    assert(renderer->open(screen, &missing) == TSUCHI_NOT_READY); t::fn::ui_create_widget = create_saved; assert_baseline();
    host_cached = false; const unsigned widgets_before = widget_calls;
    assert(renderer->open(screen, &missing) == TSUCHI_NOT_READY && widget_calls == widgets_before); host_cached = true;
    const unsigned before_unpublished_borrow = borrow_calls, before_unpublished_return = return_source_calls;
    UiScreen unpublished{}; make_screen(unpublished, mod); unpublished.id = 17;
    refresh_callback = detach_unpublished_host; missing = nullptr;
    assert(renderer->open(unpublished, &missing) == TSUCHI_NOT_READY && !missing && !active_view && mod_usable(mod));
    clear_screen(unpublished);
    assert(unpublished_detach_calls == 1 && pins.empty() && borrow_calls == before_unpublished_borrow + 1 && return_source_calls == before_unpublished_return + 1);
    assert_baseline();
    ui_native_host_detach(host_handle); assert_baseline();
    assert(pins.empty() && return_source_calls == before_unpublished_return + 1);
    begin(screen); assert(source().data != old_data && native_rows.size() == 7 && !construct_calls && visibility[scroll] == 3);
    assert(!fake::measured_reads); fake::measured_read_address = 0;
    game::ClassRef native_category_class{t::name::ui_mod_list_category_class};
    assert(game::is_a(active_category(), native_category_class));
    assert(renderer->live(active_view) && renderer->update(active_view, screen));
    assert(renderer->uses_host && renderer->uses_host(active_view, host_handle) && !renderer->uses_host(active_view, game::to_handle(root)));
    assert(labels[field(active_category(), t::ui_mod_list_category::title)] == u"Garden");
    assert(labels[field(active_row(0), t::ui_mod_list_row::title)] == u"Difficulty");
    assert(labels[field(active_selector(0), t::ui_mod_list_selector::value)] == u"Hard");
    assert(labels[field(active_selector(1), t::ui_mod_list_selector::value)] == u"-9223372036854775808");
    assert(visibility[field(active_selector(1), t::ui_mod_list_selector::left)] == t::constant::widget_hidden);
    assert(visibility[field(active_selector(1), t::ui_mod_list_selector::right)] == t::constant::widget_shown);
    assert(!fake::get<uint8_t>(active_row(2) + t::ui_mod_list_row::enabled));
    assert(visibility[active_selector(2)] == t::constant::widget_hidden);
    assert(labels[field(active_row(2), t::ui_mod_list_row::title)].find(u"Ready") != std::u16string::npos);
    const auto passive_alignment = justification.find(field(active_row(2), t::ui_mod_list_row::title));
    assert(passive_alignment != justification.end() && passive_alignment->second == 0 && rich_text_syncs);
    const uint64_t passive_slot = field(field(active_row(2), t::ui_mod_list_row::title), t::ui_mod_list_tab::slot);
    const Anchors passive_anchors = fake::get<Anchors>(passive_slot + t::ui_mod_list_tab::anchors);
    const Alignment passive_origin = fake::get<Alignment>(passive_slot + t::ui_mod_list_tab::alignment);
    const Margin passive_offsets = fake::get<Margin>(passive_slot + t::ui_mod_list_tab::offsets);
    assert(passive_anchors.left == 0 && passive_anchors.right == 1 && passive_anchors.top == 0 && passive_anchors.bottom == 0);
    assert(passive_origin.x == 0 && passive_origin.y == 0 && auto_size[passive_slot]);
    assert(passive_offsets.left == 20 && passive_offsets.top == 0 && passive_offsets.width == 20 && passive_offsets.height == 120);
    const uint64_t passive_label = field(active_row(2), t::ui_mod_list_row::title);
    assert(fake::get<uint8_t>(passive_label + t::ui_rich_text::wrapping_policy) == 1);
    assert(fake::get<uint8_t>(passive_label + t::ui_rich_text::auto_wrap) == 0xA0);
    assert(fake::get<float>(passive_label + t::ui_rich_text::wrap_width) == 1428);
    assert(last_guide.edit && !last_guide.confirm && last_guide.reset && last_guide.tabs);
    assert(focus_calls && reveal_calls && focused_row == 0);
    assert(reports.empty());
    geometry_width = 1468; geometry_height = 840;
    assert(renderer->update(active_view, screen) && reports.size() == 1);
    assert(reports.back().find(" cold ") != std::string::npos && reports.back().find("geometry=1468x840") != std::string::npos);
    assert(reports.back().find("rows=7") != std::string::npos && reports.back().find("viewport=840") != std::string::npos);
    const unsigned written = label_calls, drawn = redraw_calls, focused = focus_calls;
    assert(renderer->update(active_view, screen) && label_calls == written && redraw_calls == drawn && focus_calls == focused);
    assert(reports.size() == 1);
    screen.selected = 3; ++screen.revision; assert(renderer->update(active_view, screen));
    assert(focused_row == 3 && last_guide.confirm && !last_guide.edit);
    assert(focus_history.back().first == 3 && focus_history.back().second == 0);
    assert(visibility[field(active_selector(3), t::ui_mod_list_selector::left)] == t::constant::widget_hidden);
    assert(visibility[field(active_selector(3), t::ui_mod_list_selector::right)] == t::constant::widget_hidden);
    screen.selected = 6; ++screen.revision; assert(renderer->update(active_view, screen) && focused_row == 6);
    // Integer edits replace the middle native choice, preserving all 64 bits.
    const Array ints = fake::get<Array>(active_selector(1) + t::ui_mod_list_selector::choices); assert(ints.count == 3);
    const uint64_t old_middle = fake::get<game::EngineText>(ints.data + sizeof(game::EngineText)).counter();
    screen.controls[1].integer = INT64_MAX; ++screen.revision; assert(renderer->update(active_view, screen));
    assert(labels[field(active_selector(1), t::ui_mod_list_selector::value)] == u"9223372036854775807" && texts[old_middle].references == 0);
    assert(visibility[field(active_selector(1), t::ui_mod_list_selector::left)] == t::constant::widget_shown);
    assert(visibility[field(active_selector(1), t::ui_mod_list_selector::right)] == t::constant::widget_hidden);
    // Decimal settings retain a native arrow selector and show their value.
    screen.controls[1].number.enabled=true;screen.controls[1].text.set("0.875");
    ++screen.revision;assert(renderer->update(active_view,screen));
    assert(labels[field(active_selector(1),t::ui_mod_list_selector::value)]==u"0.875");
    assert(fake::get<Array>(active_selector(1)+t::ui_mod_list_selector::choices).count==3);
    screen.controls[1].number.enabled=false;++screen.revision;
    assert(renderer->update(active_view,screen));
    screen.selected = 1; action_changes = true;
    thread = 2; assert(intercepted(reinterpret_cast<void*>(host), 2) && accepted_calls == 0); thread = 1;
    assert(intercepted(reinterpret_cast<void*>(root), 3) && accepted_calls == 0);
    original_result = false; assert(!intercepted(reinterpret_cast<void*>(host), 3) && accepted_calls == 0); original_result = true;
    screen.controls[1].integer = 8;
    assert(intercepted(reinterpret_cast<void*>(host), 3) && accepted_which == 3 && accepted_calls == 1 && arrow_calls == 1 && arrow_increment);
    action_changes = false; assert(intercepted(reinterpret_cast<void*>(host), 3) && arrow_calls == 1);
    // Native navigation publishes the model and focus before returning from
    // the intercepted action, independently of a later engine/world pass.
    model_actions = true; screen.selected = 0; ++screen.revision; assert(renderer->update(active_view, screen));
    const unsigned sync_before_navigation = screen_sync_calls, focus_before_navigation = focus_calls, sounds_before_navigation = focus_sounds;
    assert(intercepted(reinterpret_cast<void*>(host), 1) && effective_which == 8);
    assert(screen.selected == 1 && fake::get<int32_t>(host + t::ui_mod_list_menu::selected) == 1);
    assert(focus_history.back() == std::make_pair(1, 0));
    assert(screen_sync_calls == sync_before_navigation + 1 && focus_calls == focus_before_navigation + 1 && focus_sounds == sounds_before_navigation + 1);
    const unsigned focused_immediately = focus_calls, sounded_immediately = focus_sounds, arrows_immediately = arrow_calls;
    assert(renderer->update(active_view, screen)); engine_frame(); assert(renderer->update(active_view, screen));
    assert(focus_calls == focused_immediately && focus_sounds == sounded_immediately && arrow_calls == arrows_immediately);
    assert(intercepted(reinterpret_cast<void*>(host), 1) && screen.selected == 3); // skip passive row2
    assert(fake::get<int32_t>(host + t::ui_mod_list_menu::selected) == 3 && focus_history.back() == std::make_pair(3, 1));
    assert(intercepted(reinterpret_cast<void*>(host), 0) && screen.selected == 1);
    assert(fake::get<int32_t>(host + t::ui_mod_list_menu::selected) == 1 && focus_history.back() == std::make_pair(1, 3));
    screen.selected = 0; ++screen.revision; assert(renderer->update(active_view, screen));
    const unsigned boundary_arrows = arrow_calls, boundary_focus = focus_calls;
    assert(screen.controls[0].selected == 1 && intercepted(reinterpret_cast<void*>(host), 3));
    assert(screen.controls[0].selected == 1 && arrow_calls == boundary_arrows && focus_calls == boundary_focus);
    assert(intercepted(reinterpret_cast<void*>(host), 2) && screen.controls[0].selected == 0);
    assert(fake::get<uint32_t>(active_selector(0) + t::ui_mod_list_selector::choice) == 0 && arrow_calls == boundary_arrows + 1 && !arrow_increment);
    const unsigned synchronized_redraws = redraw_calls, synchronized_arrows = arrow_calls;
    assert(renderer->update(active_view, screen) && redraw_calls == synchronized_redraws && arrow_calls == synchronized_arrows);
    const unsigned before_nested_sync = screen_sync_calls, before_busy_skip = screen_sync_busy_skips;
    screen_sync_callback = sync_during_sync;
    assert(intercepted(reinterpret_cast<void*>(host), 3) && active_view && renderer->live(active_view));
    assert(screen_sync_calls == before_nested_sync + 2 && screen_sync_busy_skips == before_busy_skip + 1);
    const unsigned before_sync_close_arrows = arrow_calls;
    label_callback = close_active;
    assert(intercepted(reinterpret_cast<void*>(host), 2) && effective_which == 8 && !active_view);
    assert(arrow_calls == before_sync_close_arrows); assert_baseline();
    begin(screen); screen.selected = 1; ++screen.revision; assert(renderer->update(active_view, screen));
    void* const action_reused_view = active_view;
    const unsigned before_action_reopen_arrows = arrow_calls;
    successor_screen = &screen; label_callback = show_successor;
    assert(intercepted(reinterpret_cast<void*>(host), 3) && active_view == action_reused_view && active_screen == &screen);
    assert(renderer->live(active_view) && arrow_calls == before_action_reopen_arrows); // stale action cannot cue the reopened generation
    successor_screen = nullptr;
    assert(intercepted(reinterpret_cast<void*>(host), 3) && arrow_calls == before_action_reopen_arrows + 1);
    assert(labels[field(active_selector(1), t::ui_mod_list_selector::value)] == u"11");
    model_actions = false;
    const uint64_t device = fake::memory(0x100); fake::put<uint32_t>(device + t::npad_device::id, 0);
    fake::put<uint64_t>(device + t::npad_device::buttons, t::constant::ui_confirm_button);
    const unsigned before_input = accepted_calls;
    intercepted_npad(reinterpret_cast<void*>(device)); assert(accepted_calls == before_input && npad_calls == 1);
    fake::put<uint64_t>(device + t::npad_device::buttons, 0); intercepted_npad(reinterpret_cast<void*>(device));
    fake::put<uint64_t>(device + t::npad_device::buttons, t::constant::ui_confirm_button);
    intercepted_npad(reinterpret_cast<void*>(device)); assert(accepted_calls == before_input + 1 && accepted_which == 4 && original_which == 4);
    intercepted_npad(reinterpret_cast<void*>(device)); assert(accepted_calls == before_input + 1);
    ui_buttons_provider(read_buttons); provided_buttons = 0; intercepted_npad(reinterpret_cast<void*>(device));
    provided_buttons = t::constant::ui_confirm_button; original_result = false; intercepted_npad(reinterpret_cast<void*>(device)); assert(accepted_calls == before_input + 1); original_result = true;
    provider_ready = false; provided_buttons = 0; intercepted_npad(reinterpret_cast<void*>(device)); provider_ready = true;
    thread = 2; intercepted_npad(reinterpret_cast<void*>(device)); thread = 1;
    fake::put<uint32_t>(device + t::npad_device::id, 1); intercepted_npad(reinterpret_cast<void*>(device)); fake::put<uint32_t>(device + t::npad_device::id, 0);
    assert(accepted_calls == before_input + 1); ui_buttons_provider(nullptr);
    refresh_callback = close_again; renderer->close(active_view); active_view = nullptr; active_screen = nullptr; assert_baseline();
    const unsigned warm_widgets = widget_calls; begin(screen); assert(widget_calls == warm_widgets && reports.size() == 2 && reports.back().find(" warm ") != std::string::npos); close_active(); assert_baseline();
    const auto permanent_pins = pins;
    const Array original_baseline = baseline;
    for (unsigned i = 0; i < 4; ++i) {
        const uint64_t category = object(category_type), data = fake::memory(8);
        fake::put(category + t::ui_mod_list_category::rows, fake::get<Array>(old_category + t::ui_mod_list_category::rows));
        fake::put<uint64_t>(data, category); baseline = {data, 1, 1}; reset_baseline();
        begin(screen); assert(widget_calls == warm_widgets && pins.size() == permanent_pins.size() + 1);
        close_active(); assert_baseline(); assert(pins == permanent_pins && reports.size() == 2);
    }
    baseline = original_baseline; reset_baseline();
    begin(screen); const uint64_t installed_source = source().data;
    assert(renderer->detach_host); renderer->detach_host(active_view, game::to_handle(root));
    assert(renderer->live(active_view) && source().data == installed_source);
    detach_active_host(); assert(active_view && !renderer->live(active_view)); assert_baseline();
    close_active(); assert_baseline();
    begin(screen); ++screen.revision; label_callback = detach_active_host;
    assert(!renderer->update(active_view, screen) && active_view); assert_baseline();
    close_active(); assert_baseline();
    ui_menu_dispatcher(dispatcher); original_result = false;
    assert(!intercepted(reinterpret_cast<void*>(host), 0) && dispatcher_calls == 0); original_result = true;
    assert(intercepted(reinterpret_cast<void*>(host), 4) && dispatcher_calls == 1 && original_which == 4);
    dispatcher_handled = false; assert(intercepted(reinterpret_cast<void*>(host), 0) && dispatcher_calls == 2); ui_menu_dispatcher(nullptr);
    ui_button_listener(listener); fake::put<uint64_t>(device + t::npad_device::buttons, 99);
    intercepted_npad(reinterpret_cast<void*>(device)); assert(listener_calls == 1 && listener_buttons == 99); ui_button_listener(nullptr);
    begin(screen); refresh_callback = open_during_close; renderer->close(active_view); active_view = nullptr; active_screen = nullptr; assert_baseline();
    begin(screen); ui_buttons_provider(read_buttons); provided_buttons = 0; intercepted_npad(reinterpret_cast<void*>(device));
    provided_buttons = t::constant::ui_confirm_button; provider_callback = close_active;
    const unsigned before_provider_close = accepted_calls; intercepted_npad(reinterpret_cast<void*>(device)); assert(!active_view && accepted_calls == before_provider_close); assert_baseline();
    begin(screen); provided_buttons = 0; intercepted_npad(reinterpret_cast<void*>(device));
    provided_buttons = t::constant::ui_confirm_button; original_callback = close_active;
    intercepted_npad(reinterpret_cast<void*>(device)); assert(!active_view && accepted_calls == before_provider_close); assert_baseline(); ui_buttons_provider(nullptr);
    begin(screen); fake::put<uint8_t>(host + t::ui_host::state, 0); assert(!renderer->live(active_view)); close_active(); fake::put<uint8_t>(host + t::ui_host::state, 1);
    // A replacement native source belongs to its new owner, even during close.
    begin(screen); const uint64_t external_data = fake::memory(8); fake::put<uint64_t>(external_data, old_category);
    fake::put<Array>(host + t::ui_mod_list_menu::source, {external_data, 1, 1}); const unsigned refresh_before_external = refresh_calls;
    assert(!renderer->live(active_view)); close_active(); assert(source().data == external_data && refresh_calls == refresh_before_external);
    fake::put(host + t::ui_mod_list_menu::source, baseline); fake::put<int32_t>(host + t::ui_mod_list_menu::selected, 2); fake::put<int32_t>(host + t::ui_mod_list_menu::previous, 1);
    fake::put<float>(scroll_panel + t::ui_mod_list_scroll::x, 7); fake::put<float>(scroll_panel + t::ui_mod_list_scroll::y, -31);
    for (unsigned i = 0; i < 2; ++i) {
        begin(screen); screen.selected = 6; ++screen.revision;
        replacement_source = fake::memory(8); fake::put<uint64_t>(replacement_source, old_category);
        const unsigned before_foreign_focus = focus_calls, before_foreign_reveal = reveal_calls;
        if (i == 0) label_callback = replace_source;
        else { screen.controls[0].selected = 1 - screen.controls[0].selected; redraw_callback = replace_source; }
        assert(!renderer->update(active_view, screen) && !renderer->live(active_view));
        assert(source().data == replacement_source && focus_calls == before_foreign_focus && reveal_calls == before_foreign_reveal);
        close_active(); assert(source().data == replacement_source && borrowed_sources.empty()); reset_baseline(); assert_baseline();
    }
    begin(screen); screen.selected = 4; ++screen.revision; focus_callback = close_active;
    assert(!renderer->update(active_view, screen) && !active_view); assert_baseline();
    begin(screen); screen.controls[0].selected = 1 - screen.controls[0].selected; ++screen.revision; redraw_callback = close_active;
    assert(!renderer->update(active_view, screen) && !active_view); assert_baseline();
    begin(screen); ++screen.revision; label_callback = close_active; assert(!renderer->update(active_view, screen) && !active_view); assert_baseline();
    begin(screen); ++screen.revision; label_callback = stop_active; assert(!renderer->update(active_view, screen) && !active_view && !mod_usable(mod)); assert_baseline(); assert(pins.empty()); clear_screen(screen);
    TSUCHI_Mod* second_mod = mod_create("native.stopping"); UiScreen stopped{}; make_screen(stopped, second_mod);
    active_screen = &stopped; create_callback = stop_unpublished; missing = nullptr;
    assert(renderer->open(stopped, &missing) == TSUCHI_NOT_READY && !missing && !mod_usable(second_mod)); assert_baseline(); assert(pins.empty()); active_screen = nullptr; clear_screen(stopped);
    TSUCHI_Mod* deferred_mod = mod_create("native.deferred-close"); UiScreen deferred{}; make_screen(deferred, deferred_mod); deferred.id = 15;
    begin(deferred); ++deferred.revision; label_callback = stop_unpublished;
    assert(!renderer->update(active_view, deferred) && active_view && !mod_usable(deferred_mod));
    // The core owns the delivered pointer until its deferred close callback.
    close_active(); clear_screen(deferred); assert_baseline(); assert(pins.empty());
    TSUCHI_Mod* third_mod = mod_create("native.host-loss"); UiScreen lost{}; make_screen(lost, third_mod); begin(lost);
    // The UI observer runs before input commits its edge history.
    assert(!ui_input_steps(nullptr) && !ui_input_steps(register_input_step) && input_registrations == 0); ui_buttons_provider(read_buttons);
    thread = 2; assert(!ui_input_steps(register_input_step)); thread = 1;
    assert(!ui_input_steps(register_input_step) && input_registrations == 1 && !registered_input_step);
    registrar_accepts = true; assert(ui_input_steps(register_input_step) && registered_input_step && input_registrations == 2); input_history_committed = true;
    assert(!ui_input_steps(register_input_step) && input_registrations == 2);
    const unsigned actions_before_steps = accepted_calls, reads_before_steps = provider_reads, npad_before_steps = npad_calls;
    provided_buttons = 0; fake::put<uint64_t>(device + t::npad_device::buttons, t::constant::ui_confirm_button); intercepted_npad(reinterpret_cast<void*>(device));
    assert(npad_calls == npad_before_steps + 1 && provider_reads == reads_before_steps && accepted_calls == actions_before_steps);
    registered_input_step(); registered_input_step(); assert(provider_reads == reads_before_steps + 2 && accepted_calls == actions_before_steps);
    input_history_committed = false; provided_buttons = t::constant::ui_confirm_button; fake::put<uint64_t>(device + t::npad_device::buttons, 0); intercepted_npad(reinterpret_cast<void*>(device));
    assert(provider_reads == reads_before_steps + 2 && accepted_calls == actions_before_steps); registered_input_step(); input_history_committed = true;
    assert(provider_reads == reads_before_steps + 3 && accepted_calls == actions_before_steps + 1 && accepted_which == 4);
    registered_input_step(); assert(provider_reads == reads_before_steps + 4 && accepted_calls == actions_before_steps + 1);
    provider_ready = false; provided_buttons = 0; registered_input_step(); provider_ready = true; registered_input_step(); provided_buttons = t::constant::ui_confirm_button; registered_input_step();
    assert(provider_reads == reads_before_steps + 7 && accepted_calls == actions_before_steps + 2);
    thread = 2; registered_input_step(); thread = 1; ui_buttons_provider(nullptr); registered_input_step();
    assert(provider_reads == reads_before_steps + 7 && accepted_calls == actions_before_steps + 2);
#ifdef TSUCHI_TEST_INPUT
    ui_native_start(third_mod); input::set_backend(&input_backend); assert(ui_native_prepare_input());
    const unsigned actions_before_hidden = accepted_calls;
    service_held = TSUCHI_BUTTONS_A; service_hidden = TSUCHI_BUTTONS_A; registered_input_step(); registered_input_step(); assert(accepted_calls == actions_before_hidden);
    hidden_ready = false; service_hidden = 0; registered_input_step(); assert(accepted_calls == actions_before_hidden);
    hidden_ready = true; registered_input_step(); assert(accepted_calls == actions_before_hidden);
    service_held = 0; registered_input_step(); service_held = TSUCHI_BUTTONS_A; registered_input_step(); assert(accepted_calls == actions_before_hidden + 1);
    registered_input_step(); service_hidden = TSUCHI_BUTTONS_A; registered_input_step(); service_hidden = 0; registered_input_step(); assert(accepted_calls == actions_before_hidden + 1);
    service_held = 0; registered_input_step(); service_held = TSUCHI_BUTTONS_A; registered_input_step(); assert(accepted_calls == actions_before_hidden + 2);
#endif
    close_active(); assert_baseline();
    // All-text pages keep messages, but never enter stock's enabled-row loop.
    UiScreen messages{}; messages.id = 11; messages.mod = third_mod; messages.title.set("Information"); messages.selected = kUiNoSelection;
    UiControl message{}; message.kind = UiKind::text; message.label.set("Status"); message.text.set("Ready"); assert(messages.controls.add(message));
    for (unsigned i = 0; i < 11; ++i) {
        UiControl another{}; another.kind = UiKind::text; another.label.set("More information"); another.text.set("Ready"); assert(messages.controls.add(another));
    }
    const std::string message_footer = std::string(ui_glyph_token(1)) + " Back";
    assert(messages.footer.set(message_footer.c_str()));
    const unsigned focused_before_messages = focus_calls, revealed_before_messages = reveal_calls;
    begin(messages); footer_callback = clear_footer_from_callback; assert(renderer->update(active_view, messages));
    assert(messages.controls.count == 12 && !strcmp(last_guide.custom, message_footer.c_str()));
    assert(!messages.footer.get()[0]);
    const uint32_t footer_revision = messages.revision;
    assert(renderer->update(active_view, messages) && messages.revision == footer_revision && !last_guide.custom[0]);
    const std::string changed_footer = std::string(ui_glyph_token(6)) + " Read more";
    assert(messages.footer.set(changed_footer.c_str())); ++messages.revision;
    assert(renderer->update(active_view, messages) && !strcmp(last_guide.custom, changed_footer.c_str()));
    assert(messages.footer.set(message_footer.c_str())); ++messages.revision;
    assert(renderer->update(active_view, messages) && !strcmp(last_guide.custom, message_footer.c_str()));
    for (uint32_t i = 0; i < messages.controls.count; ++i) assert(!fake::get<uint8_t>(active_row(i) + t::ui_mod_list_row::enabled));
    assert(fake::get<int32_t>(host + t::ui_mod_list_menu::selected) == -1 && fake::get<int32_t>(host + t::ui_mod_list_menu::previous) == -1);
    assert(focus_calls == focused_before_messages && reveal_calls == revealed_before_messages && !last_guide.edit && !last_guide.confirm);
    const unsigned actions_before_reading = accepted_calls;
    const unsigned sync_before_reading = screen_sync_calls;
    const unsigned cues_before_reading = feedback_cues;
    const uint32_t passive_revision = messages.revision;
    assert(intercepted(reinterpret_cast<void*>(host), 0) && effective_which == 8 && last_reveal == 1 && feedback_cues == cues_before_reading);
    for (unsigned i = 1; i < 12; ++i) {
        const uint64_t expected = game::to_handle(active_row(i));
        assert(intercepted(reinterpret_cast<void*>(host), 1) && effective_which == 8 && last_reveal == i + 1);
        assert(feedback_cues == cues_before_reading + i && feedback_targets.back() == expected);
    }
    assert(intercepted(reinterpret_cast<void*>(host), 1) && effective_which == 8 && last_reveal == 12 && feedback_cues == cues_before_reading + 11);
    for (int i = 10; i >= 0; --i) {
        const uint64_t expected = game::to_handle(active_row(uint32_t(i)));
        assert(intercepted(reinterpret_cast<void*>(host), 0) && effective_which == 8 && last_reveal == uint32_t(i + 1));
        assert(feedback_cues == cues_before_reading + 22u - uint32_t(i) && feedback_targets.back() == expected);
    }
    assert(intercepted(reinterpret_cast<void*>(host), 0) && effective_which == 8 && last_reveal == 1 && feedback_cues == cues_before_reading + 22);
    assert(focus_calls == focused_before_messages && accepted_calls == actions_before_reading && messages.selected == kUiNoSelection);
    assert(screen_sync_calls == sync_before_reading); // passive Reveal does not manufacture a model update
    for (uint32_t which : {2u, 3u, 4u}) assert(intercepted(reinterpret_cast<void*>(host), which) && effective_which == 8);
    assert(feedback_cues == cues_before_reading + 22 && focus_calls == focused_before_messages && messages.selected == kUiNoSelection);
    assert(messages.revision == passive_revision && messages.controls.count == 12);
    for (uint32_t i = 0; i < messages.controls.count; ++i)
        assert(messages.controls[i].kind == UiKind::text && !strcmp(messages.controls[i].text.get(), "Ready"));
    back_closes = true; assert(intercepted(reinterpret_cast<void*>(host), 5) && effective_which == 8 && !active_view); back_closes = false; assert_baseline();
    begin(messages); assert(renderer->update(active_view, messages)); const unsigned before_close_cues = feedback_cues;
    reveal_callback = close_active; assert(intercepted(reinterpret_cast<void*>(host), 1) && effective_which == 8 && !active_view);
    assert(feedback_cues == before_close_cues); assert_baseline();
    begin(messages); assert(renderer->update(active_view, messages));
    replacement_source = fake::memory(8); fake::put<uint64_t>(replacement_source, old_category);
    reveal_callback = replace_source; assert(intercepted(reinterpret_cast<void*>(host), 1) && effective_which == 8 && !renderer->live(active_view));
    assert(feedback_cues == before_close_cues); close_active(); assert(source().data == replacement_source); reset_baseline(); assert_baseline();
    UiScreen successor{}; successor.id = 19; successor.mod = third_mod; successor.title.set("Successor"); successor.selected = kUiNoSelection;
    UiControl successor_message{}; successor_message.kind = UiKind::text; successor_message.label.set("Status"); successor_message.text.set("Ready"); assert(successor.controls.add(successor_message));
    begin(messages); assert(renderer->update(active_view, messages)); successor_screen = &successor;
    reveal_callback = show_successor; assert(intercepted(reinterpret_cast<void*>(host), 1) && effective_which == 8 && active_screen == &successor);
    assert(feedback_cues == before_close_cues); close_active(); clear_screen(successor); successor_screen = nullptr; assert_baseline();
    begin(messages); assert(renderer->update(active_view, messages));
    const unsigned before_reopen_cues = feedback_cues;
    assert(intercepted(reinterpret_cast<void*>(host), 1) && effective_which == 8 && last_reveal == 2);
    assert(feedback_cues == before_reopen_cues + 1);
    void* const reused_view = active_view;
    successor_screen = &messages; reveal_callback = show_successor;
    assert(intercepted(reinterpret_cast<void*>(host), 0) && effective_which == 8 && active_screen == &messages && active_view == reused_view);
    assert(last_reveal == 1 && feedback_cues == before_reopen_cues + 1); // old Up cannot cue the reopened view
    assert(intercepted(reinterpret_cast<void*>(host), 1) && effective_which == 8 && last_reveal == 2);
    assert(feedback_cues == before_reopen_cues + 2); // the reopened view's own Down remains audible
    close_active(); successor_screen = nullptr; assert_baseline();
    begin(messages); assert(renderer->update(active_view, messages));
    const unsigned before_nested_cues = feedback_cues;
    const uint64_t nested_target = game::to_handle(active_row(2));
    reveal_callback = read_during_reveal; assert(intercepted(reinterpret_cast<void*>(host), 1) && effective_which == 8 && last_reveal == 3);
    assert(feedback_cues == before_nested_cues + 1 && feedback_targets.back() == nested_target);
    close_active(); assert_baseline(); clear_screen(messages);
    // A button and then more messages than the view holds: Down reads on through the messages and focus stays on
    // the button, clamped at the last one. Up reads back to the button, and then focus moves as it does.
    UiScreen mixed{}; mixed.id = 12; mixed.mod = third_mod; mixed.title.set("Information"); mixed.selected = 0;
    UiControl go{}; go.kind = UiKind::button; go.label.set("Go"); go.text.set("Ready"); assert(mixed.controls.add(go));
    for (unsigned i = 0; i < 11; ++i) {
        UiControl another{}; another.kind = UiKind::text; another.label.set("More information"); another.text.set("Ready"); assert(mixed.controls.add(another));
    }
    begin(mixed); assert(renderer->update(active_view, mixed) && mixed.selected == 0);
    for (unsigned i = 1; i < 12; ++i) assert(intercepted(reinterpret_cast<void*>(host), 1) && last_reveal == i + 1 && mixed.selected == 0);
    assert(intercepted(reinterpret_cast<void*>(host), 1) && last_reveal == 12 && mixed.selected == 0);
    assert(renderer->update(active_view, mixed) && last_reveal == 12);
    for (int i = 10; i >= 0; --i) assert(intercepted(reinterpret_cast<void*>(host), 0) && last_reveal == uint32_t(i + 1) && mixed.selected == 0);
    assert(intercepted(reinterpret_cast<void*>(host), 0) && mixed.selected == 0);
    close_active(); assert_baseline(); clear_screen(mixed);
    UiScreen long_message{}; long_message.id = 14; long_message.mod = third_mod; long_message.title.set("Information"); long_message.selected = kUiNoSelection;
    UiControl long_text{}; long_text.kind = UiKind::text;
    const std::string long_caption(640, 'a'), long_value(640, 'b');
    assert(long_text.label.set(long_caption.c_str()) && long_text.text.set(long_value.c_str()) && long_message.controls.add(long_text));
    zero_title_height = true; begin(long_message); zero_title_height = false;
    assert(renderer->update(active_view, long_message));
    assert(labels[field(active_row(0), t::ui_mod_list_row::title)] == std::u16string(640, u'a') + u": " + std::u16string(640, u'b'));
    const uint64_t measured_label = field(active_row(0), t::ui_mod_list_row::title), measured_slot = field(measured_label, t::ui_mod_list_tab::slot);
    assert(auto_size[measured_slot]);
    const uint32_t unchanged_revision = long_message.revision;
    label_geometry[measured_label] = {500, 64}; assert(renderer->update(active_view, long_message));
    const Anchors measured_anchors = fake::get<Anchors>(measured_slot + t::ui_mod_list_tab::anchors);
    const Alignment measured_alignment = fake::get<Alignment>(measured_slot + t::ui_mod_list_tab::alignment);
    const Margin measured_offsets = fake::get<Margin>(measured_slot + t::ui_mod_list_tab::offsets);
    assert(long_message.revision == unchanged_revision && auto_size[measured_slot]);
    assert(measured_anchors.left == 0 && measured_anchors.right == 1 && measured_anchors.top == 0 && measured_anchors.bottom == 0);
    assert(measured_alignment.x == 0 && measured_alignment.y == 0);
    assert(measured_offsets.left == 20 && measured_offsets.top == 0 && measured_offsets.width == 20 && measured_offsets.height == 0);
    assert(fake::get<float>(measured_label + t::ui_rich_text::wrap_width) == 1428);
    close_active(); clear_screen(long_message); assert_baseline();
    // The consumer's passive page was physically 1525 units tall while native
    // Refresh advertised 1407. Reveal must use reconciled arranged boundaries.
    UiScreen wrapped{}; wrapped.id = 21; wrapped.mod = third_mod; wrapped.title.set("Notes"); wrapped.selected = kUiNoSelection;
    for (unsigned i = 0; i < 12; ++i) {
        UiControl control{}; control.kind = UiKind::text;
        assert(control.label.set(i == 11 ? "Reason" : "Note") && control.text.set("Ready") && wrapped.controls.add(control));
    }
    physical_layout = true;
    begin(wrapped);
    const uint64_t reason_row = active_row(11), reason_label = field(reason_row, t::ui_mod_list_row::title);
    const auto bottom = [&](uint64_t widget) { const auto& value = arranged_geometry.at(widget); return value[12] + value[1] * value[10]; };
    const float clip_bottom = 173 + geometry_height * .75f;
    assert(arranged_geometry.at(active_category())[1] == 1525);
    t::fn::ui_mod_list_reveal(reinterpret_cast<void*>(scroll), 12);
    assert(bottom(reason_label) > clip_bottom); // captured fixed120 layout reproduces the crop
    const unsigned arrangements_before_style = arrangements, focus_before_wrapped = focus_calls;
    const Array wrapped_heights = fake::get<Array>(scroll + t::ui_mod_list_scroll::heights);
    assert(wrapped_heights.count == 13 && fake::get<float>(scroll + t::ui_mod_list_scroll::total) == 1407);
    assert(renderer->update(active_view, wrapped));
    assert(arrangements == arrangements_before_style && fake::get<float>(scroll + t::ui_mod_list_scroll::total) == 1407);
    // Repeated and reentrant-style updates in one engine epoch cannot consume
    // the pre-setter cached arrangement, even though it is internally coherent.
    assert(renderer->update(active_view, wrapped));
    assert(fake::get<float>(wrapped_heights.data) == 63 && fake::get<float>(scroll + t::ui_mod_list_scroll::total) == 1407);
    for (unsigned i = 1; i < 12; ++i) assert(intercepted(reinterpret_cast<void*>(host), 1) && last_reveal == i + 1);
    engine_frame(); arrange_native(); assert(renderer->update(active_view, wrapped));
    assert(fake::get<float>(scroll + t::ui_mod_list_scroll::total) == 1429 && fake::get<float>(wrapped_heights.data) == 85);
    for (unsigned i = 1; i <= 12; ++i) assert(fake::get<float>(wrapped_heights.data + i * sizeof(float)) == 112);
    assert(last_reveal == 12 && bottom(reason_label) <= clip_bottom + .01f);
    assert(focus_calls == focus_before_wrapped && wrapped.selected == kUiNoSelection);
    const unsigned stable_reveal = reveal_calls, stable_transform = transform_calls, stable_scrollbar = scrollbar_calls;
    engine_frame(); arrange_native(); assert(renderer->update(active_view, wrapped));
    assert(reveal_calls == stable_reveal && transform_calls == stable_transform && scrollbar_calls == stable_scrollbar);
    const uint64_t reason_slot = field(reason_label, t::ui_mod_list_tab::slot);
    assert(auto_size[reason_slot] && fake::get<float>(reason_label + t::ui_rich_text::wrap_width) == 1428);
    assert(wrapped.controls[11].text.set(std::string(400, 'p').c_str())); ++wrapped.revision;
    const float before_wrap = fake::get<float>(scroll + t::ui_mod_list_scroll::total);
    assert(renderer->update(active_view, wrapped) && fake::get<float>(scroll + t::ui_mod_list_scroll::total) == before_wrap);
    assert(renderer->update(active_view, wrapped) && fake::get<float>(scroll + t::ui_mod_list_scroll::total) == before_wrap);
    engine_frame(); arrange_native(); assert(renderer->update(active_view, wrapped));
    const float grown = arranged_geometry.at(reason_row)[1];
    assert(grown == 144 && arranged_geometry.at(reason_label)[1] == grown);
    assert(fake::get<float>(wrapped_heights.data + 12 * sizeof(float)) == grown);
    assert(fake::get<float>(scroll + t::ui_mod_list_scroll::total) == 1317 + grown);
    assert(bottom(reason_label) <= clip_bottom + .01f);
    const auto& reason_bounds = arranged_geometry.at(reason_label);
    const auto& reason_row_bounds = arranged_geometry.at(reason_row);
    const float native_render_y = fake::get<float>(reason_label + t::ui_widget::render_y);
    assert(native_render_y == -2);
    assert(std::fabs(reason_bounds[12] - native_render_y * reason_row_bounds[10] - reason_row_bounds[12]) < .01f);
    assert(reason_bounds[12] >= bottom(field(active_row(10), t::ui_mod_list_row::title)));
    assert(reason_bounds[11] >= 31 && reason_bounds[11] + reason_bounds[0] * reason_bounds[7] <= 31 + geometry_width * .75f);
    // Invalid native shapes and foreign active-category ownership must leave
    // the old array and total untouched, rather than partially fixing them.
    const auto reset_extents = [&] {
        fake::put<float>(wrapped_heights.data, 63);
        for (unsigned i = 1; i <= 12; ++i) fake::put<float>(wrapped_heights.data + i * sizeof(float), 112);
        fake::put<float>(scroll + t::ui_mod_list_scroll::total, 1407);
    };
    reset_extents(); fake::put<Array>(scroll + t::ui_mod_list_scroll::heights, {wrapped_heights.data, 12, 13});
    engine_frame(); assert(renderer->update(active_view, wrapped));
    assert(fake::get<float>(wrapped_heights.data) == 63 && fake::get<float>(scroll + t::ui_mod_list_scroll::total) == 1407);
    fake::put(scroll + t::ui_mod_list_scroll::heights, wrapped_heights);
    const Array wrapped_active = fake::get<Array>(host + t::ui_mod_list_menu::active);
    fake::put<uint64_t>(wrapped_active.data, old_category);
    engine_frame(); assert(renderer->update(active_view, wrapped));
    assert(fake::get<float>(wrapped_heights.data) == 63 && fake::get<float>(scroll + t::ui_mod_list_scroll::total) == 1407);
    fake::put<uint64_t>(wrapped_active.data, active_category());
    arranged_geometry[reason_row][8] = .1f;
    engine_frame(); assert(renderer->update(active_view, wrapped));
    assert(fake::get<float>(wrapped_heights.data) == 63 && fake::get<float>(scroll + t::ui_mod_list_scroll::total) == 1407);
    arranged_geometry[reason_row][8] = 0;
    engine_frame(); assert(renderer->update(active_view, wrapped));
    assert(fake::get<float>(scroll + t::ui_mod_list_scroll::total) == 1317 + grown);
    back_closes = true; assert(intercepted(reinterpret_cast<void*>(host), 5) && !active_view); back_closes = false;
    assert_baseline();
    begin(wrapped); assert(renderer->update(active_view, wrapped)); engine_frame(); arrange_native();
    const Array abandoned_heights = fake::get<Array>(scroll + t::ui_mod_list_scroll::heights);
    replacement_source = fake::memory(8); fake::put<uint64_t>(replacement_source, old_category);
    const unsigned before_geometry_foreign_focus = focus_calls, before_geometry_foreign_reveal = reveal_calls;
    geometry_callback = replace_source;
    assert(!renderer->update(active_view, wrapped) && !renderer->live(active_view));
    assert(fake::get<float>(abandoned_heights.data) == 63 && fake::get<float>(scroll + t::ui_mod_list_scroll::total) == 3000);
    assert(focus_calls == before_geometry_foreign_focus && reveal_calls == before_geometry_foreign_reveal);
    close_active(); assert(source().data == replacement_source); reset_baseline(); assert_baseline();
    begin(wrapped); assert(renderer->update(active_view, wrapped)); engine_frame(); arrange_native();
    void* const geometry_reused_view = active_view;
    successor_screen = &wrapped; geometry_callback = show_successor;
    (void)renderer->update(active_view, wrapped);
    assert(active_view == geometry_reused_view && active_screen == &wrapped && renderer->live(active_view));
    const Array reopened_heights = fake::get<Array>(scroll + t::ui_mod_list_scroll::heights);
    assert(fake::get<float>(reopened_heights.data) == 63 && fake::get<float>(scroll + t::ui_mod_list_scroll::total) == 1407);
    engine_frame(); arrange_native(); assert(renderer->update(active_view, wrapped));
    assert(fake::get<float>(reopened_heights.data) == 85 && fake::get<float>(scroll + t::ui_mod_list_scroll::total) == 1461);
    close_active(); successor_screen = nullptr; assert_baseline();
    begin(wrapped); assert(renderer->update(active_view, wrapped));
    assert(wrapped.controls[0].text.set("Changed during synchronization")); ++wrapped.revision;
    sync_callback = close_active;
    assert(!renderer->update(active_view, wrapped) && !active_view); assert_baseline();
    physical_layout = false; clear_screen(wrapped);
    assert(fake::get<float>(scroll + t::ui_mod_list_scroll::total) == 3000 && engine_registrations == 1);
    TSUCHI_Mod* sync_mod = mod_create("native.immediate-sync"); UiScreen sync_edges{};
    make_screen(sync_edges, sync_mod); sync_edges.id = 22; sync_edges.selected = 1;
    model_actions = true; begin(sync_edges); assert(renderer->update(active_view, sync_edges));
    const unsigned before_sync_loss_arrows = arrow_calls;
    label_callback = detach_active_host;
    assert(intercepted(reinterpret_cast<void*>(host), 3) && effective_which == 8 && !active_view);
    assert(arrow_calls == before_sync_loss_arrows && mod_usable(sync_mod)); assert_baseline();
    begin(sync_edges); assert(renderer->update(active_view, sync_edges));
    label_callback = stop_active;
    assert(intercepted(reinterpret_cast<void*>(host), 3) && effective_which == 8 && !active_view && !mod_usable(sync_mod));
    assert(arrow_calls == before_sync_loss_arrows); assert_baseline();
    model_actions = false; clear_screen(sync_edges);
    // Long choice labels are native FTexts, including values beyond old Off/On limits.
    UiScreen long_choices{}; make_screen(long_choices, third_mod); long_choices.id = 12;
    long_choices.controls[0].labels.clear(); const std::string long_label(640, 'x');
    assert(long_choices.controls[0].labels.add(long_label.c_str()) && long_choices.controls[0].labels.add("Short")); long_choices.controls[0].selected = 0;
    begin(long_choices); assert(renderer->update(active_view, long_choices));
    assert(labels[field(active_selector(0), t::ui_mod_list_selector::value)] == std::u16string(640, u'x'));
    const unsigned choice_widgets = widget_calls;
    long_choices.controls[0].selected = 1; ++long_choices.revision; assert(renderer->update(active_view, long_choices));
    assert(labels[field(active_selector(0), t::ui_mod_list_selector::value)] == u"Short" && widget_calls == choice_widgets);
    close_active(); clear_screen(long_choices); assert_baseline();
    TSUCHI_Mod* installing_mod = mod_create("native.install-stop"); UiScreen installing{}; make_screen(installing, installing_mod); installing.id = 13;
    active_screen = &installing; refresh_callback = stop_unpublished; missing = nullptr;
    assert(renderer->open(installing, &missing) == TSUCHI_NOT_READY && !missing && !mod_usable(installing_mod));
    active_screen = nullptr; clear_screen(installing); assert_baseline();
    begin(lost); const uint64_t previous_scroll = scroll; scroll = object(); const uint64_t previous_panel = scroll_panel; scroll_panel = object(panel_type);
    visibility[scroll] = 3; fake::put<uint64_t>(host + t::ui_host::scroll, scroll); fake::put<uint64_t>(scroll + t::ui_mod_list_scroll::panel, scroll_panel);
    fake::put<float>(scroll + t::ui_mod_list_scroll::total, 3000); fake::put<float>(scroll + t::ui_mod_list_scroll::viewport, 840);
    const unsigned before_changed_refresh = refresh_calls, before_changed_focus = focus_calls, before_changed_reveal = reveal_calls;
    assert(!renderer->live(active_view)); close_active(); assert(visibility[previous_scroll] == 3 && visibility[scroll] == 3);
    assert(source().data == baseline.data && refresh_calls == before_changed_refresh && focus_calls == before_changed_focus && reveal_calls == before_changed_reveal);
    assert(fake::get<int32_t>(host + t::ui_mod_list_menu::selected) == 2 && fake::get<int32_t>(host + t::ui_mod_list_menu::previous) == 1);
    assert(borrowed_sources.empty());
    // The new scroller owns its focus/translation; source restoration cannot touch it.
    reset_baseline();
    const unsigned widgets_before_replacement = widget_calls; begin(lost); assert(widget_calls > widgets_before_replacement); close_active(); assert_baseline();
    begin(lost); fake::destroy(host); assert(!renderer->live(active_view)); close_active(); assert(visibility[scroll] == 3);
    mod_stop(third_mod); clear_screen(lost); assert(pins.empty() && !construct_calls && !released.empty());
    assert(borrowed_sources.empty() && borrow_calls == return_source_calls);
    (void)previous_panel; return 0;
}
