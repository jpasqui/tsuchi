// Owned UMG behavior against host memory, not a claim about native UMG execution.
#include "fake_game.h"
#include "ui_widgets.h"
#include "ui_service.h"
#include "holds.h"
#include "hooks.h"
#include "actor_events.h"
#include "ordinary_lifecycle.h"
#include "objects.h"
#include "reflect.h"
#include "shared.h"
#include <cassert>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

using namespace tsuchi;
namespace t = game::table;
namespace {
struct NativeArray { uint64_t data; int32_t count, capacity; };
struct Slot { uint64_t parent, child; };
std::vector<void (*)()> steps;
std::map<uint64_t, std::vector<uint64_t>> panel_slots;
std::map<uint64_t, Slot> slot_info;
std::map<uint64_t, unsigned> removals;
std::map<uint64_t, int32_t> viewport;
std::map<uint64_t, uint64_t> context_worlds;
uint64_t thread = 1, loaded = 0, engine = 0;
uint64_t changing_context = 0, changed_world = 0;
uint64_t meta, widget_type, user_type, tree_type, panel_type, button_type, actor_type;
uint64_t slot_type, cooked_type, abstract_type, unrelated_type, methods;
uint64_t rich_type = 0;
uint64_t last_created = 0, last_tree = 0, last_constructed = 0;
unsigned factory_calls = 0, construct_calls = 0, attach_calls = 0, viewport_calls = 0;
unsigned original_calls = 0, hook_calls = 0, next_mod = 0;
bool fail_factory = false, fail_construct = false, fail_attach = false, corrupt_slot = false;
bool foreign_construct = false, patched = false, factory_outer_in_tree = false;
bool fail_viewport = false, refuse_viewport_remove = false;
TSUCHI_Result hook_result = TSUCHI_OK;
void (*factory_callback)() = nullptr;
void (*construct_callback)() = nullptr;
void (*attach_callback)() = nullptr;
void (*viewport_callback)() = nullptr;
void (*remove_callback)() = nullptr;
void (*original_callback)() = nullptr;
void (*from_string_callback)() = nullptr;
void (*set_text_callback)() = nullptr;
struct TextRecord { std::u16string value; unsigned references, temporary_drops; };
std::map<uint64_t, TextRecord> text_records;
std::map<uint64_t, game::EngineText> widget_text;
std::vector<uint64_t> text_native_pins;
uint64_t next_text = 1;
unsigned text_conversions = 0, text_sets = 0, text_drops = 0;
TSUCHI_Object text_callback_root{};
uint64_t text_callback_widget = 0;
TSUCHI_Mod* stopping = nullptr;
TSUCHI_Mod* framework = nullptr;
decltype(t::code_site::ui_widget_broadcast)::Function intercepted = nullptr;
void* original_list = nullptr;
void* original_payload = nullptr;

void invoke(void (*&callback)()) {
    const auto saved = callback; callback = nullptr;
    if (saved) saved();
}
void stop_owner() { mod_stop(stopping); }
void change_context_world() { context_worlds[changing_context] = changed_world; }
void step() { for (const auto work : steps) work(); }
TSUCHI_Mod* make_mod() {
    const std::string id = "widgets.test." + std::to_string(++next_mod);
    auto* result = mod_create(id.c_str()); assert(result); return result;
}
TSUCHI_Object handle(uint64_t object) { return {game::to_handle(object)}; }
uint64_t raw(TSUCHI_Object object) { const uint64_t result = game::from_handle(object.id); assert(result); return result; }
uint64_t make_type(const char* name, uint64_t super = 0) {
    std::u16string wide; while (*name) wide += char16_t(*name++);
    const uint64_t result = fake::object(wide, meta, 0x200);
    fake::set_super(result, super); return result;
}
uint64_t object(const char16_t* name, uint64_t type, uint64_t outer = 0) {
    const uint64_t result = fake::object(name, type, 0x600);
    fake::put<uint64_t>(result, methods);
    fake::put<uint64_t>(result + t::object::outer, outer);
    return result;
}
void publish_slots(uint64_t parent) {
    const auto& slots = panel_slots[parent];
    const uint64_t data = slots.empty() ? 0 : fake::memory(slots.size() * 8);
    for (size_t i = 0; i < slots.size(); ++i) fake::put<uint64_t>(data + i * 8, slots[i]);
    fake::put<NativeArray>(parent + t::ui_panel::slots, {data, int32_t(slots.size()), int32_t(slots.size())});
}
void remove_widget(void* pointer) {
    const uint64_t child = reinterpret_cast<uint64_t>(pointer);
    ++removals[child]; if (!refuse_viewport_remove) viewport.erase(child);
    const uint64_t slot = fake::get<uint64_t>(child + t::ui_mod_list_tab::slot);
    if (slot) {
        const uint64_t parent = fake::get<uint64_t>(slot + t::ui_mod_list_tab::parent);
        auto& slots = panel_slots[parent];
        for (auto at = slots.begin(); at != slots.end(); ++at)
            if (*at == slot) { slots.erase(at); break; }
        publish_slots(parent);
        fake::put<uint64_t>(child + t::ui_mod_list_tab::slot, 0);
        fake::put<uint64_t>(slot + t::ui_mod_list_tab::parent, 0);
    }
    invoke(remove_callback);
}
void* create_widget(void* context, void* type, void* name) {
    ++factory_calls; assert(name == nullptr);
    if (fail_factory) return nullptr;
    const uint64_t context_object = reinterpret_cast<uint64_t>(context);
    const uint64_t outer = factory_outer_in_tree ? fake::get<uint64_t>(context_object + t::ui_user_widget::tree) : context_object;
    last_created = object(u"RuntimeRoot", reinterpret_cast<uint64_t>(type), outer);
    last_tree = object(u"WidgetTree", tree_type, last_created);
    fake::put<uint64_t>(last_created + t::ui_user_widget::tree, last_tree);
    invoke(factory_callback);
    return reinterpret_cast<void*>(last_created);
}
void* construct_widget(const void* pointer) {
    ++construct_calls;
    if (fail_construct) return nullptr;
    const auto* parameters = static_cast<const uint64_t*>(pointer);
    assert(game::is_class(parameters[0]));
    last_constructed = object(u"Constructed", parameters[0], foreign_construct ? loaded : parameters[1]);
    invoke(construct_callback);
    return reinterpret_cast<void*>(last_constructed);
}
void* add_child(void* parent_pointer, void* child_pointer) {
    ++attach_calls;
    if (fail_attach) return nullptr;
    const uint64_t parent = reinterpret_cast<uint64_t>(parent_pointer), child = reinterpret_cast<uint64_t>(child_pointer);
    const uint64_t slot = object(u"Slot", slot_type, parent);
    slot_info[slot] = {parent, child};
    panel_slots[parent].push_back(slot); publish_slots(parent);
    fake::put<uint64_t>(slot + t::ui_mod_list_tab::parent, parent);
    fake::put<uint64_t>(slot + t::ui_panel_slot::content, corrupt_slot ? 0 : child);
    fake::put<uint64_t>(child + t::ui_mod_list_tab::slot, slot);
    invoke(attach_callback);
    return reinterpret_cast<void*>(slot);
}
void show_widget(void* pointer, int32_t z_order, bool local_player) {
    assert(!local_player); ++viewport_calls;
    invoke(viewport_callback);
    fake::put<uint64_t>(reinterpret_cast<uint64_t>(pointer) + t::ui_widget::cached_slate, 1);
    // Blueprint/native callbacks can stop the mod before AddToViewport finishes.
    if (!fail_viewport) viewport[reinterpret_cast<uint64_t>(pointer)] = z_order;
}
bool in_viewport(void* pointer) { return viewport.count(reinterpret_cast<uint64_t>(pointer)) != 0; }
void* context_world(void* supplied_engine, void* context, uint32_t mode) {
    assert(reinterpret_cast<uint64_t>(supplied_engine) == engine && mode == 0);
    const auto found = context_worlds.find(reinterpret_cast<uint64_t>(context));
    return reinterpret_cast<void*>(found == context_worlds.end() ? loaded : found->second);
}
void original_broadcast(void* list, void* payload) {
    ++original_calls; original_list = list; original_payload = payload;
    invoke(original_callback);
}
void check_text_native_pins() {
    for (const uint64_t object : text_native_pins) assert(game::object_kept(object));
}
game::EngineText text_from_string(const void* pointer) {
    ++text_conversions;
    const uint64_t at = reinterpret_cast<uint64_t>(pointer);
    const uint64_t data = fake::get<uint64_t>(at + t::array::data);
    const int32_t count = fake::get<int32_t>(at + t::array::count);
    assert(data && count > 0 && fake::get<int32_t>(at + t::array::max) == count);
    assert(fake::get<char16_t>(data + uint64_t(count - 1) * 2) == 0);
    const uint64_t token = next_text++;
    text_records[token] = {{reinterpret_cast<const char16_t*>(data), size_t(count - 1)}, 1, 0};
    game::EngineText result;
    memcpy(result.bytes + t::text_value::data, &token, 8);
    memcpy(result.bytes + t::text_value::counter, &token, 8);
    invoke(from_string_callback); check_text_native_pins(); return result;
}
void set_text(void* pointer, void* value) {
    ++text_sets; const uint64_t widget = reinterpret_cast<uint64_t>(pointer);
    const auto copied = *static_cast<game::EngineText*>(value);
    auto& record = text_records.at(copied.counter()); assert(record.references > 0);
    ++record.references; // Native FText assignment owns a share beyond this call.
    const auto before = widget_text.find(widget);
    if (before != widget_text.end()) { auto& old = text_records.at(before->second.counter()); assert(old.references); --old.references; }
    widget_text[widget] = copied;
    invoke(set_text_callback); check_text_native_pins();
}
std::u16string shown_text(uint64_t widget) {
    const auto& record = text_records.at(widget_text.at(widget).counter());
    assert(record.references > 0); return record.value;
}
void collect_widget_text() {
    // Simulated engine collection is separate from the framework dropping its
    // one temporary share; hiding and ending ownership do not restore a value.
    for (const auto& label : widget_text) { auto& record = text_records.at(label.second.counter()); assert(record.references); --record.references; }
    widget_text.clear();
    for (const auto& text : text_records) assert(text.second.references == 0 && text.second.temporary_drops == 1);
}
void destroy_text_root() { assert(ui_destroy_widget(stopping, text_callback_root) == TSUCHI_OK); }
void replace_text_world() { loaded = object(u"TextReplacementWorld", unrelated_type); step(); }
void move_text_to_private_tree() {
    const uint64_t private_tree = object(u"PrivateTextTree", tree_type, raw(text_callback_root));
    fake::put<uint64_t>(text_callback_widget + t::object::outer, private_tree);
}
uint32_t signal_offset(unsigned signal) {
    const uint64_t offsets[] = {t::ui_widget_button::clicked, t::ui_widget_button::pressed,
        t::ui_widget_button::released, t::ui_widget_button::hovered, t::ui_widget_button::unhovered};
    assert(signal < 5); return offsets[signal];
}
void broadcast(uint64_t button, unsigned signal = 0) {
    assert(intercepted);
    void* list = reinterpret_cast<void*>(button + signal_offset(signal));
    void* payload = reinterpret_cast<void*>(uintptr_t(0x1234));
    const unsigned before = original_calls;
    intercepted(list, payload);
    assert(original_calls == before + 1 && original_list == list && original_payload == payload);
}
TSUCHI_Object create(TSUCHI_Mod* mod, uint64_t context = 0) {
    TSUCHI_Object result{};
    assert(ui::table.create_widget(mod, handle(context ? context : loaded), handle(cooked_type), &result) == TSUCHI_OK);
    assert(result.id && game::object_kept(raw(result)) && game::object_kept(last_tree));
    return result;
}
TSUCHI_Object construct(TSUCHI_Mod* mod, TSUCHI_Object root, uint64_t type) {
    TSUCHI_Object result{};
    assert(ui_construct_widget(mod, root, handle(type), &result) == TSUCHI_OK);
    assert(game::object_kept(raw(result))); return result;
}
void no_pins() {
    const int32_t count = fake::get<int32_t>(fake::main_image() + t::data::object_count);
    for (int32_t i = 0; i < count; ++i) {
        const uint64_t item = fake::item_at(i);
        assert(!(fake::get<uint32_t>(item + t::object_item::flags) & t::constant::kept_flag));
    }
}
struct Events {
    std::vector<uint64_t> widgets;
    std::vector<uint32_t> signals;
    TSUCHI_Mod* owner = nullptr;
    bool stop = false;
};
void receive(void* pointer, const TSUCHI_UiButtonEvent* event) {
    auto& events = *static_cast<Events*>(pointer);
    events.widgets.push_back(event->widget.id); events.signals.push_back(event->signal);
    if (events.stop) { events.stop = false; mod_stop(events.owner); }
}
TSUCHI_Subscription subscribe(TSUCHI_Mod* mod, Events& events) {
    events.owner = mod; TSUCHI_Subscription subscription{};
    assert(ui::table.on_button_event(mod, receive, &events, &subscription) == TSUCHI_OK);
    return subscription;
}
void test_construction_and_ownership() {
    auto* owner = make_mod(); auto* other = make_mod();
    TSUCHI_Object result{777};
    assert(ui_create_widget(owner, {}, handle(cooked_type), &result) == TSUCHI_NOT_FOUND && result.id == 777);
    assert(ui_create_widget(owner, handle(loaded), handle(button_type), &result) == TSUCHI_INVALID && result.id == 777);
    assert(ui_create_widget(owner, handle(loaded), handle(abstract_type), &result) == TSUCHI_INVALID);
    assert(ui_create_widget(owner, handle(object(u"Unrelated", unrelated_type)), handle(cooked_type), &result) == TSUCHI_INVALID);
    fail_factory = true;
    assert(ui_create_widget(owner, handle(loaded), handle(cooked_type), &result) == TSUCHI_NOT_READY && result.id == 777);
    fail_factory = false; no_pins();
    const TSUCHI_Object root = create(owner); const uint64_t tree = last_tree;
    assert(ui_destroy_widget(other, root) == TSUCHI_DENIED);
    const TSUCHI_Object panel = construct(owner, root, panel_type);
    const TSUCHI_Object button = construct(owner, root, button_type);
    assert(game::outer_of(raw(panel)) == tree && game::outer_of(raw(button)) == tree);
    const unsigned before_self = attach_calls;
    assert(ui_set_root_widget(owner, root, root) == TSUCHI_INVALID);
    assert(ui_add_child(owner, panel, root, &result) == TSUCHI_INVALID && result.id == 777);
    assert(attach_calls == before_self && !fake::get<uint64_t>(tree + t::ui_widget_tree::root));
    assert(ui_construct_widget(owner, root, handle(cooked_type), &result) == TSUCHI_INVALID);
    assert(ui_construct_widget(owner, root, handle(unrelated_type), &result) == TSUCHI_INVALID);
    fail_construct = true;
    assert(ui_construct_widget(owner, root, handle(button_type), &result) == TSUCHI_NOT_READY && result.id == 777);
    fail_construct = false;
    foreign_construct = true;
    assert(ui_construct_widget(owner, root, handle(button_type), &result) == TSUCHI_NOT_READY);
    foreign_construct = false;
    assert(ui_set_root_widget(owner, root, panel) == TSUCHI_OK);
    assert(fake::get<uint64_t>(tree + t::ui_widget_tree::root) == raw(panel));
    assert(ui_set_root_widget(owner, root, button) == TSUCHI_BUSY);
    TSUCHI_Object slot{};
    assert(ui_add_child(owner, panel, button, &slot) == TSUCHI_OK);
    assert(game::object_kept(raw(slot)) && slot_info[raw(slot)].parent == raw(panel) && slot_info[raw(slot)].child == raw(button));
    assert(ui_add_child(owner, panel, button, &result) == TSUCHI_BUSY);
    assert(ui_add_child(owner, button, panel, &result) == TSUCHI_INVALID);
    assert(ui_add_child(owner, panel, panel, &result) == TSUCHI_INVALID);
    const TSUCHI_Object second_root = create(owner), second_panel = construct(owner, second_root, panel_type);
    assert(ui_set_root_widget(owner, second_root, panel) == TSUCHI_DENIED);
    assert(ui_add_child(owner, second_panel, button, &result) == TSUCHI_DENIED);
    const TSUCHI_Object nested_panel = construct(owner, second_root, panel_type);
    assert(ui_add_child(owner, second_panel, nested_panel, &result) == TSUCHI_OK);
    assert(ui_add_child(owner, nested_panel, second_panel, &result) == TSUCHI_INVALID);
    const TSUCHI_Object foreign_root = create(other), foreign_button = construct(other, foreign_root, button_type);
    assert(ui_add_child(owner, panel, foreign_button, &result) == TSUCHI_DENIED);
    fake::put(raw(button) + t::object::name, fake::name(u"Apply"));
    assert(ui_find_widget(owner, root, "Apply", &result) == TSUCHI_OK && result.id == button.id);
    assert(ui_find_widget(owner, root, "Missing", &result) == TSUCHI_OK && result.id == 0);
    assert(ui_find_widget(other, root, "Apply", &result) == TSUCHI_DENIED);
    assert(ui_find_widget(owner, root, "", &result) == TSUCHI_INVALID);
    const uint64_t nested_user = object(u"NestedMenu", cooked_type, tree);
    const uint64_t private_tree = object(u"PrivateTree", tree_type, nested_user);
    const uint64_t private_button = object(u"PrivateButton", button_type, private_tree);
    fake::put<uint64_t>(nested_user + t::ui_user_widget::tree, private_tree);
    fake::put<uint64_t>(private_tree + t::ui_widget_tree::root, private_button);
    add_child(reinterpret_cast<void*>(raw(panel)), reinterpret_cast<void*>(nested_user));
    assert(ui_find_widget(owner, root, "NestedMenu", &result) == TSUCHI_OK && result.id == handle(nested_user).id);
    assert(ui_find_widget(owner, root, "PrivateButton", &result) == TSUCHI_OK && result.id == 0);
    const unsigned before_private = attach_calls, before_private_hook = hook_calls;
    result.id = 777;
    assert(ui_add_child(owner, panel, handle(private_button), &result) == TSUCHI_NOT_FOUND && result.id == 777);
    assert(attach_calls == before_private);
    assert(ui_hold_button_events(owner, handle(private_button)) == TSUCHI_NOT_FOUND && hook_calls == before_private_hook);
    assert(fake::get<uint64_t>(private_tree + t::ui_widget_tree::root) == private_button);
    // A separately created UserWidget retains its own owner even when the
    // native factory places it under the context widget's tree.
    factory_outer_in_tree = true;
    const TSUCHI_Object contextual_root = create(other, raw(root));
    factory_outer_in_tree = false;
    assert(game::outer_of(raw(contextual_root)) == tree);
    const unsigned before_contextual = attach_calls;
    assert(ui_add_child(owner, panel, contextual_root, &result) == TSUCHI_DENIED && attach_calls == before_contextual);
    const TSUCHI_Object contextual_panel = construct(other, contextual_root, panel_type);
    assert(ui_set_root_widget(other, contextual_root, contextual_panel) == TSUCHI_OK);
    assert(ui_destroy_widget(owner, contextual_root) == TSUCHI_DENIED);
    const TSUCHI_Object loose = construct(owner, second_root, button_type);
    fail_attach = true; result.id = 777;
    assert(ui_add_child(owner, second_panel, loose, &result) == TSUCHI_NOT_READY && result.id == 777);
    fail_attach = false; corrupt_slot = true;
    assert(ui_add_child(owner, second_panel, loose, &result) == TSUCHI_NOT_READY && result.id == 777);
    corrupt_slot = false;
    assert(!fake::get<uint64_t>(raw(loose) + t::ui_mod_list_tab::slot));
    assert(ui_destroy_widget(owner, root) == TSUCHI_OK);
    assert(ui_destroy_widget(owner, root) == TSUCHI_NOT_FOUND);
    assert(ui_hold_button_events(owner, button) == TSUCHI_NOT_FOUND);
    mod_stop(owner); mod_stop(other); no_pins();
}
void test_viewport_and_context() {
    auto* owner = make_mod(); const TSUCHI_Object root = create(owner); const uint64_t widget = raw(root);
    assert(ui_release_widget(owner, root) == TSUCHI_NOT_FOUND);
    const unsigned before_external = viewport_calls;
    viewport[widget] = 99;
    assert(ui_hold_widget(owner, root, 12) == TSUCHI_BUSY && viewport_calls == before_external);
    viewport.erase(widget); fail_viewport = true;
    assert(ui_hold_widget(owner, root, 12) == TSUCHI_NOT_READY && viewport.count(widget) == 0);
    fail_viewport = false;
    assert(ui_hold_widget(owner, root, 12) == TSUCHI_OK && viewport[widget] == 12);
    const unsigned calls = viewport_calls;
    assert(ui_hold_widget(owner, root, 12) == TSUCHI_OK && viewport_calls == calls);
    assert(ui_hold_widget(owner, root, 13) == TSUCHI_BUSY);
    const unsigned removed = removals[widget];
    assert(ui_release_widget(owner, root) == TSUCHI_OK && viewport.count(widget) == 0 && removals[widget] == removed + 1);
    assert(game::object_kept(widget));
    assert(ui_hold_widget(owner, root, -2) == TSUCHI_OK);
    refuse_viewport_remove = true;
    assert(ui_release_widget(owner, root) == TSUCHI_NOT_READY && viewport.count(widget) == 1);
    refuse_viewport_remove = false;
    assert(ui_release_widget(owner, root) == TSUCHI_OK);
    assert(ui_hold_widget(owner, root, -2) == TSUCHI_OK);
    mod_stop(owner); assert(viewport.count(widget) == 0); no_pins();
    // An empty UserWidget must receive its root before TakeWidget caches a
    // spacer. Removing it from the viewport does not clear that Slate cache.
    owner = make_mod(); const TSUCHI_Object empty_root = create(owner);
    const TSUCHI_Object pending_root = construct(owner, empty_root, panel_type);
    assert(ui_hold_widget(owner, empty_root, 0) == TSUCHI_OK);
    assert(ui_set_root_widget(owner, empty_root, pending_root) == TSUCHI_BUSY);
    assert(ui_release_widget(owner, empty_root) == TSUCHI_OK);
    assert(ui_set_root_widget(owner, empty_root, pending_root) == TSUCHI_BUSY);
    mod_stop(owner); no_pins();
    owner = make_mod(); const TSUCHI_Object cached_root = create(owner);
    const TSUCHI_Object cached_panel = construct(owner, cached_root, panel_type);
    fake::put<uint64_t>(raw(cached_root) + t::ui_widget::cached_slate, 1);
    assert(ui_set_root_widget(owner, cached_root, cached_panel) == TSUCHI_BUSY);
    mod_stop(owner); no_pins();
    owner = make_mod(); const TSUCHI_Object parented_root = create(owner);
    const TSUCHI_Object parented_panel = construct(owner, parented_root, panel_type);
    const uint64_t external_panel = object(u"ExternalPanel", panel_type);
    add_child(reinterpret_cast<void*>(external_panel), reinterpret_cast<void*>(raw(parented_root)));
    assert(ui_set_root_widget(owner, parented_root, parented_panel) == TSUCHI_BUSY);
    mod_stop(owner); no_pins();
    // A live context is usable only when its native GetWorld agrees with the loaded world.
    owner = make_mod(); const uint64_t foreign_world = object(u"ForeignWorld", unrelated_type);
    const uint64_t foreign_actor = object(u"ForeignActor", actor_type);
    const uint64_t foreign_widget = object(u"ForeignWidget", cooked_type);
    context_worlds[foreign_actor] = foreign_world; context_worlds[foreign_widget] = foreign_world;
    TSUCHI_Object result{777}; const unsigned before_foreign = factory_calls;
    assert(ui_create_widget(owner, handle(foreign_actor), handle(cooked_type), &result) == TSUCHI_NOT_READY && result.id == 777);
    assert(ui_create_widget(owner, handle(foreign_widget), handle(cooked_type), &result) == TSUCHI_NOT_READY && result.id == 777);
    assert(factory_calls == before_foreign); mod_stop(owner); no_pins();
    owner = make_mod(); changing_context = object(u"ChangingContext", actor_type); changed_world = foreign_world;
    factory_callback = change_context_world;
    assert(ui_create_widget(owner, handle(changing_context), handle(cooked_type), &result) == TSUCHI_NOT_READY && result.id == 777);
    mod_stop(owner); no_pins();
    owner = make_mod(); changing_context = object(u"MovedContext", actor_type);
    const TSUCHI_Object moved_root = create(owner, changing_context); const uint64_t moved_widget = raw(moved_root);
    assert(ui_hold_widget(owner, moved_root, 0) == TSUCHI_OK);
    change_context_world(); step();
    assert(!game::object_kept(moved_widget) && viewport.count(moved_widget) == 0);
    assert(ui_destroy_widget(owner, moved_root) == TSUCHI_NOT_FOUND); mod_stop(owner); no_pins();
    owner = make_mod(); const uint64_t context = object(u"Captain", actor_type);
    const TSUCHI_Object actor_root = create(owner, context); const uint64_t actor_widget = raw(actor_root);
    assert(ui_hold_widget(owner, actor_root, 0) == TSUCHI_OK);
    fake::destroy(context); step();
    assert(!game::object_kept(actor_widget) && viewport.count(actor_widget) == 0);
    assert(ui_destroy_widget(owner, actor_root) == TSUCHI_NOT_FOUND); mod_stop(owner); no_pins();
    owner = make_mod(); const TSUCHI_Object world_root = create(owner); const uint64_t world_widget = raw(world_root);
    const uint64_t saved_world = loaded; loaded = 0; step(); loaded = saved_world;
    assert(!game::object_kept(world_widget));
    assert(ui_destroy_widget(owner, world_root) == TSUCHI_NOT_FOUND); mod_stop(owner); no_pins();
    // Retention supplied by an independent user must survive our final release.
    assert(game::keep_object(cooked_type, true));
    owner = make_mod(); create(owner); mod_stop(owner);
    assert(game::object_kept(cooked_type)); assert(game::keep_object(cooked_type, false)); no_pins();
}
void test_native_reentry() {
    auto* owner = make_mod(); stopping = owner; TSUCHI_Object result{777};
    factory_callback = stop_owner;
    assert(ui_create_widget(owner, handle(loaded), handle(cooked_type), &result) == TSUCHI_NOT_READY && result.id == 777);
    assert(!owned_count(owner)); no_pins();
    owner = make_mod(); stopping = owner; TSUCHI_Object root = create(owner);
    construct_callback = stop_owner;
    assert(ui_construct_widget(owner, root, handle(button_type), &result) == TSUCHI_NOT_READY && result.id == 777);
    assert(removals[last_constructed] == 1); no_pins();
    owner = make_mod(); stopping = owner; root = create(owner);
    const TSUCHI_Object panel = construct(owner, root, panel_type), button = construct(owner, root, button_type);
    attach_callback = stop_owner;
    assert(ui_add_child(owner, panel, button, &result) == TSUCHI_NOT_READY && result.id == 777);
    assert(!fake::get<uint64_t>(raw(button) + t::ui_mod_list_tab::slot)); no_pins();
    owner = make_mod(); stopping = owner; root = create(owner); const uint64_t viewport_widget = raw(root);
    viewport_callback = stop_owner;
    assert(ui_hold_widget(owner, root, 0) == TSUCHI_NOT_READY);
    assert(viewport.count(viewport_widget) == 0 && removals[viewport_widget] == 1); no_pins();
    owner = make_mod(); stopping = owner; root = create(owner); const uint64_t removed_widget = raw(root);
    assert(ui_hold_widget(owner, root, 0) == TSUCHI_OK); remove_callback = stop_owner;
    assert(ui_release_widget(owner, root) == TSUCHI_NOT_READY);
    assert(removals[removed_widget] == 1 && viewport.count(removed_widget) == 0); no_pins();
    owner = make_mod(); stopping = owner; root = create(owner); const uint64_t destroyed_widget = raw(root);
    assert(ui_hold_widget(owner, root, 0) == TSUCHI_OK);
    remove_callback = stop_owner;
    assert(ui_destroy_widget(owner, root) == TSUCHI_OK);
    assert(removals[destroyed_widget] == 1 && !owned_count(owner)); no_pins();
}
void test_button_delivery() {
    auto* owner = make_mod(); auto* other = make_mod(); Events events, foreign;
    const auto subscription = subscribe(owner, events); subscribe(other, foreign);
    const TSUCHI_Object root = create(owner), button = construct(owner, root, button_type);
    const uint64_t native_button = raw(button);
    assert(ui_hold_button_events(owner, root) == TSUCHI_INVALID);
    assert(ui_hold_button_events(other, button) == TSUCHI_DENIED);
    assert(ui_hold_button_events(owner, button) == TSUCHI_NOT_READY && hook_calls == 0);
    patched = true; hook_result = TSUCHI_NOT_READY;
    assert(ui_hold_button_events(owner, button) == TSUCHI_NOT_READY && hook_calls == 1);
    hook_result = TSUCHI_OK;
    assert(ui::table.hold_button_events(owner, button) == TSUCHI_OK && intercepted && hook_calls == 2);
    assert(ui_hold_button_events(owner, button) == TSUCHI_OK && hook_calls == 2);
    for (unsigned signal = 0; signal < 5; ++signal) broadcast(native_button, signal);
    assert(events.widgets.empty() && foreign.widgets.empty()); step();
    assert(events.widgets.size() == 5 && foreign.widgets.empty());
    for (unsigned signal = 0; signal < 5; ++signal) {
        assert(events.widgets[signal] == button.id && events.signals[signal] == signal);
    }
    broadcast(object(u"GameButton", button_type));
    thread = 2; broadcast(native_button); thread = 1; step(); assert(events.widgets.size() == 5);
    broadcast(native_button);
    assert(ui_release_button_events(owner, button) == TSUCHI_OK);
    assert(ui_hold_button_events(owner, button) == TSUCHI_OK); step(); assert(events.widgets.size() == 5);
    // Cancelling the subscription suppresses delivery without changing the native producer.
    broadcast(native_button); assert(cancel(owner, subscription) == TSUCHI_OK); step();
    assert(events.widgets.size() == 5); subscribe(owner, events);
    broadcast(native_button); assert(ui_destroy_widget(owner, root) == TSUCHI_OK); step();
    assert(events.widgets.size() == 5); mod_stop(owner); mod_stop(other); no_pins();
    // Native listeners may stop the owner before a queued signal is delivered.
    owner = make_mod(); stopping = owner; Events stopped; subscribe(owner, stopped);
    const TSUCHI_Object stopped_root = create(owner), stopped_button = construct(owner, stopped_root, button_type);
    assert(ui_hold_button_events(owner, stopped_button) == TSUCHI_OK);
    original_callback = stop_owner; broadcast(raw(stopped_button)); step();
    assert(stopped.widgets.empty()); no_pins();
    // Stop during delivery suppresses remaining subscriptions and queued signals.
    owner = make_mod(); Events first, later; first.stop = true;
    subscribe(owner, first); subscribe(owner, later);
    const TSUCHI_Object delivered_root = create(owner), delivered_button = construct(owner, delivered_root, button_type);
    assert(ui_hold_button_events(owner, delivered_button) == TSUCHI_OK);
    broadcast(raw(delivered_button)); broadcast(raw(delivered_button)); step();
    assert(first.widgets.size() == 1 && later.widgets.empty()); no_pins();
}
void test_owned_widget_text() {
    auto* owner = make_mod(); auto* other = make_mod();
    const TSUCHI_Object root = create(owner), label = construct(owner, root, rich_type);
    const uint64_t widget = raw(label), tree = last_tree;
    const unsigned conversions = text_conversions, drops = text_drops;
    assert(ui::table.set_widget_text(owner, label, "Ready") == TSUCHI_OK);
    assert(shown_text(widget) == u"Ready" && text_conversions == conversions + 1 && text_drops == drops + 1);
    assert(ui_hold_widget(owner, root, 0) == TSUCHI_OK);
    const unsigned before_hide = text_sets;
    assert(ui_release_widget(owner, root) == TSUCHI_OK && shown_text(widget) == u"Ready" && text_sets == before_hide);
    const char* tokens[] = {
        "[PictureFont:IconData key=\"A_Button\" scale=\"0.76\"/]", "[PictureFont:IconData key=\"B_Button\" scale=\"0.76\"/]",
        "[PictureFont:IconData key=\"X_Button\" scale=\"0.76\"/]", "[PictureFont:IconData key=\"Y_Button\" scale=\"0.76\"/]",
        "[PictureFont:IconData key=\"L_Button\" scale=\"0.76\"/]", "[PictureFont:IconData key=\"R_Button\" scale=\"0.76\"/]",
        "[PictureFont:IconData key=\"ZR_Button\" scale=\"0.76\"/]"};
    for (const auto token : tokens) assert(ui_set_widget_text(owner, label, token) == TSUCHI_OK);
    const std::string boundary = std::string(638, 'a') + "\xc3\xa9";
    assert(boundary.size() == 640 && ui_set_widget_text(owner, label, boundary.c_str()) == TSUCHI_OK);
    assert(shown_text(widget) == std::u16string(638, u'a') + u"\u00e9");
    const std::string astral = std::string(636, 'a') + "\xf0\x9f\x8c\xbf";
    assert(ui_set_widget_text(owner, label, astral.c_str()) == TSUCHI_OK);
    assert(shown_text(widget) == std::u16string(636, u'a') + u"\U0001f33f");
    assert(ui_set_widget_text(owner, label, "") == TSUCHI_OK && shown_text(widget).empty());
    const unsigned before_invalid = text_sets, before_invalid_conversion = text_conversions;
    const char* invalid[] = {"[", "]", "[Unknown/]", "Line\nTwo", "Tab\tTwo", "DEL\x7f",
        "[PictureFont:IconData key=\"A_Button\" scale=\"1.0\"/]", "[PictureFont:IconData key=\"ZL_Button\" scale=\"0.76\"/]",
        "\xc0\xaf", "\x80", "\xe2\x82", "\xed\xa0\x80", "\xf4\x90\x80\x80", "\xf8\x90\x80\x80"};
    for (const auto text : invalid) assert(ui_set_widget_text(owner, label, text) == TSUCHI_INVALID);
    const std::string oversized = std::string(639, 'a') + "\xc3\xa9";
    assert(ui_set_widget_text(owner, label, oversized.c_str()) == TSUCHI_INVALID);
    assert(ui_set_widget_text(owner, label, nullptr) == TSUCHI_INVALID);
    const TSUCHI_Object button = construct(owner, root, button_type);
    assert(ui_set_widget_text(owner, button, "Wrong type") == TSUCHI_INVALID);
    assert(ui_set_widget_text(other, label, "Foreign") == TSUCHI_DENIED);
    const uint64_t private_tree = object(u"PrivateTree", tree_type, raw(root));
    const TSUCHI_Object private_label = handle(object(u"PrivateLabel", rich_type, private_tree));
    assert(ui_set_widget_text(owner, private_label, "Private") == TSUCHI_NOT_FOUND);
    assert(ui_set_widget_text(owner, handle(object(u"GameLabel", rich_type)), "Game") == TSUCHI_NOT_FOUND);
    assert(text_sets == before_invalid && text_conversions == before_invalid_conversion);
    const auto from_saved = t::fn::text_from_string; t::fn::text_from_string = nullptr;
    assert(ui_set_widget_text(owner, label, "Unavailable") == TSUCHI_NOT_READY); t::fn::text_from_string = from_saved;
    const auto set_saved = t::fn::rich_text_set; t::fn::rich_text_set = nullptr;
    assert(ui_set_widget_text(owner, label, "Unavailable") == TSUCHI_NOT_READY); t::fn::rich_text_set = set_saved;
    assert(text_sets == before_invalid && text_conversions == before_invalid_conversion);
    fake::put(widget + t::object::name, fake::name(u"AuthoredLabel"));
    fake::put<uint64_t>(tree + t::ui_widget_tree::root, widget);
    TSUCHI_Object found{};
    assert(ui_find_widget(owner, root, "AuthoredLabel", &found) == TSUCHI_OK && found.id == label.id);
    assert(ui_set_widget_text(owner, found, "Before end") == TSUCHI_OK);
    const unsigned before_end = text_sets; const uint64_t saved_world = loaded;
    loaded = 0;
    assert(ui_set_widget_text(owner, label, "Ended world") == TSUCHI_NOT_READY && text_sets == before_end);
    step(); loaded = saved_world;
    assert(ui_set_widget_text(owner, label, "Ended root") == TSUCHI_NOT_FOUND && shown_text(widget) == u"Before end");
    fake::destroy(widget);
    assert(ui_set_widget_text(owner, label, "Stale") == TSUCHI_NOT_FOUND && text_sets == before_end);
    mod_stop(owner); mod_stop(other); no_pins(); collect_widget_text();
}
void test_widget_text_native_reentry() {
    // Conversion may stop/destroy the owner, replace the world, or move the
    // child into a private tree. None may publish a later native property write.
    for (unsigned scenario = 0; scenario < 6; ++scenario) {
        auto* owner = make_mod(); const TSUCHI_Object root = create(owner), label = construct(owner, root, rich_type);
        const uint64_t widget = raw(label), saved_world = loaded;
        assert(ui_set_widget_text(owner, label, "Ready") == TSUCHI_OK);
        stopping = owner; text_callback_root = root; text_callback_widget = widget;
        text_native_pins = {raw(root), last_tree, widget};
        const unsigned before_sets = text_sets, before_conversions = text_conversions, before_drops = text_drops;
        if (scenario == 0) from_string_callback = stop_owner;
        else if (scenario == 1) from_string_callback = destroy_text_root;
        else if (scenario == 2) from_string_callback = replace_text_world;
        else if (scenario == 3) set_text_callback = stop_owner;
        else if (scenario == 4) set_text_callback = replace_text_world;
        else from_string_callback = move_text_to_private_tree;
        assert(ui_set_widget_text(owner, label, "Attempt") == TSUCHI_NOT_READY);
        assert(text_sets == before_sets + (scenario == 3 || scenario == 4));
        assert(text_conversions == before_conversions + 1 && text_drops == before_drops + 1);
        assert(shown_text(widget) == ((scenario == 3 || scenario == 4) ? u"Attempt" : u"Ready"));
        text_native_pins.clear(); loaded = saved_world; mod_stop(owner); step();
        assert(text_sets == before_sets + (scenario == 3 || scenario == 4));
        no_pins(); collect_widget_text();
    }
}
} // namespace

namespace tsuchi {
ui::Backend& ui_screen_backend() { static ui::Backend backend{}; return backend; }
bool each_frame(void (*work)()) { steps.push_back(work); return true; }
bool on_actor_left(void (*)(uint64_t, uint64_t, ActorLeft)) { return true; }
void ordinary_lifecycle_listener(void (*)(uint64_t, uint64_t, bool)) {}
TSUCHI_Result ordinary_lifecycle_watch(uint64_t) { return TSUCHI_OK; }
uint64_t ordinary_lifecycle_restore_part(uint64_t part, uint64_t) { return game::from_handle(part); }
bool code_site_patched(uint32_t site) { assert(site == t::code_site::ui_widget_broadcast.number); return patched; }
TSUCHI_Result hook_code_site(TSUCHI_Mod* owner, uint32_t site, void* function, uint64_t* id, void** original) {
    assert(owner == framework && site == t::code_site::ui_widget_broadcast.number); ++hook_calls;
    if (hook_result != TSUCHI_OK) return hook_result;
    intercepted = reinterpret_cast<decltype(intercepted)>(function);
    *id = 1; *original = reinterpret_cast<void*>(original_broadcast); return TSUCHI_OK;
}
} // namespace tsuchi
namespace tsuchi::game {
void drop_shared(uint64_t counter) {
    if (!counter) return;
    auto& record = text_records.at(counter);
    assert(record.references > 0 && record.temporary_drops == 0);
    --record.references; ++record.temporary_drops; ++text_drops;
}
bool utf8_to_units(const char* text, char16_t* out, size_t capacity, size_t& count) {
    // This link seam admits ASCII and the two valid multibyte fixtures above;
    // unsupported/malformed byte sequences are refused. Production uses its
    // separately tested reflection converter, not this deliberately small stub.
    count = 0;
    const auto put = [&](char16_t unit) { if (count < capacity) out[count] = unit; ++count; };
    for (const auto* at = reinterpret_cast<const unsigned char*>(text); *at;) {
        if (*at < 128) put(char16_t(*at++));
        else if (at[0] == 0xc3 && at[1] == 0xa9) { put(u'\u00e9'); at += 2; }
        else if (at[0] == 0xf0 && at[1] == 0x9f && at[2] == 0x8c && at[3] == 0xbf) { put(0xd83c); put(0xdf3f); at += 4; }
        else return false;
    }
    return true;
}
uint64_t loaded_world() { return loaded; }
bool is_game_actor(uint64_t) { return false; }
uint64_t part_owner(uint64_t) { return 0; }
bool derives_from(uint64_t type, uint64_t wanted) {
    for (unsigned depth = 0; type && wanted && depth < 64; ++depth) {
        if (type == wanted) return true;
        type = read_or<uint64_t>(type + t::struct_type::super);
    }
    return false;
}
} // namespace tsuchi::game

int main() {
    fake::start(); set_thread_probe([] { return thread; }); note_game_thread();
    meta = fake::object(u"Class", 0, 0x200);
    methods = fake::memory(0x300);
    fake::put<uint64_t>(methods + t::ui_widget::remove_from_parent, reinterpret_cast<uint64_t>(remove_widget));
    fake::code().push_back(reinterpret_cast<uint64_t>(remove_widget));
    widget_type = make_type(t::name::ui_widget_class);
    user_type = make_type(t::name::ui_user_widget_class, widget_type);
    tree_type = make_type(t::name::ui_widget_tree_class);
    panel_type = make_type(t::name::ui_panel_base_class, widget_type);
    button_type = make_type(t::name::ui_native_button_class, widget_type);
    rich_type = make_type(t::name::ui_footer_text_class, widget_type);
    actor_type = make_type(t::name::engine_actor_class);
    slot_type = make_type("PanelSlot"); unrelated_type = make_type("Unrelated");
    cooked_type = make_type("CookedMenu_C", user_type);
    abstract_type = make_type("AbstractMenu_C", user_type);
    fake::put<uint32_t>(abstract_type + t::ui_widget_class_flags::flags, t::constant::ui_widget_abstract);
    loaded = object(u"World", unrelated_type);
    engine = object(u"Engine", unrelated_type);
    fake::put<uint64_t>(fake::main_image() + t::data::engine, engine);
    t::fn::ui_create_widget = create_widget; t::fn::ui_construct = construct_widget;
    t::fn::ui_add_child = add_child; t::fn::ui_widget_viewport = show_widget;
    t::fn::ui_widget_in_viewport = in_viewport; t::fn::ui_context_world = context_world;
    t::fn::text_from_string = text_from_string; t::fn::rich_text_set = set_text;
    framework = mod_create("widgets.framework"); assert(framework); ui_widgets_start(framework);
    test_construction_and_ownership();
    test_viewport_and_context();
    test_native_reentry();
    test_button_delivery();
    test_owned_widget_text();
    test_widget_text_native_reentry();
    mod_stop(framework); no_pins();
    std::puts("owned widgets: construction, ownership, viewport, queued native events and reentry passed");
}
