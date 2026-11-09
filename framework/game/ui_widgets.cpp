#include "ui_widgets.h"
#include "ui_service.h"
#include "ui_screens.h"
#include "ui_rich_text.h"
#include "flow.h"
#include "frame.h"
#include "game.h"
#include "holds.h"
#include "hooks.h"
#include "list.h"
#include "names.h"
#include "object_retention.h"
#include "objects.h"
#include "reflect.h"
#include "shared.h"
#include <string.h>

namespace tsuchi { namespace {
namespace t = game::table;
game::ClassRef widget_class{t::name::ui_widget_class};
game::ClassRef user_class{t::name::ui_user_widget_class};
game::ClassRef tree_class{t::name::ui_widget_tree_class};
game::ClassRef panel_class{t::name::ui_panel_base_class};
game::ClassRef button_class{t::name::ui_native_button_class};
game::ClassRef actor_class{t::name::engine_actor_class};
game::ClassRef text_class{t::name::ui_footer_text_class};
struct Array { uint64_t data; int32_t count, capacity; };
struct Observation { uint64_t widget, token; };
struct Root {
    TSUCHI_Mod* mod;
    uint64_t context, world, widget, tree, key;
    List<uint64_t> pins, made;
    List<uint64_t> nested;   // other UserWidgets of the same mod that were put in this one's panels
    List<Observation> observations;
    uint32_t calls;
    bool retired, viewport, removing, adding_viewport;
    int32_t z_order;
};
struct Want { Root* root; uint64_t type; };
struct Notice { TSUCHI_Mod* mod; uint64_t root, widget, token; uint32_t signal; };
TSUCHI_Mod* framework = nullptr;
uint64_t next_root = 1, next_observation = 1;
List<Notice> notices;
bool steps_registered = false, hooked = false;
decltype(t::code_site::ui_widget_broadcast)::Function next_broadcast = nullptr;

uint64_t resolve(uint64_t handle) { return game::from_handle(handle); }
bool available() { return on_game_thread() && framework && mod_usable(framework); }
void finish(Root* root);
void cleanup(Root& root);
bool active(Root& root) {
    const bool valid = !root.retired && root.mod && mod_usable(root.mod) && available() &&
        resolve(root.context) && resolve(root.world) &&
        game::to_handle(game::loaded_world()) == root.world &&
        (!root.widget || (resolve(root.widget) && (!root.tree || game::to_handle(game::read_or<uint64_t>(
            resolve(root.widget) + t::ui_user_widget::tree)) == root.tree)));
    const uint64_t engine = game::global<uint64_t>(t::data::engine);
    if (!valid || !engine || !t::bound(t::fn::ui_context_world)) return false;
    // Context.GetWorld is native virtual code. Keep state and its pins alive if
    // that call stops the owner, then recheck captured generations afterwards.
    ++root.calls;
    const uint64_t context_world = reinterpret_cast<uint64_t>(t::fn::ui_context_world(
        reinterpret_cast<void*>(engine),
        reinterpret_cast<void*>(resolve(root.context)), 0));
    const bool same = !root.retired && root.mod && mod_usable(root.mod) && available() && resolve(root.context) &&
        resolve(root.world) && game::to_handle(context_world) == root.world &&
        game::to_handle(game::loaded_world()) == root.world &&
        (!root.widget || (resolve(root.widget) && (!root.tree || game::to_handle(game::read_or<uint64_t>(
            resolve(root.widget) + t::ui_user_widget::tree)) == root.tree)));
    finish(&root); return same;
}
bool allowed(void* pointer) { return active(*static_cast<Root*>(pointer)); }
game::CallGuard guard(Root& root) { return {allowed, &root}; }
void finish(Root* root) {
    if (--root->calls || !root->retired) return;
    cleanup(*root);
    root->pins.clear(); root->made.clear(); root->nested.clear(); root->observations.clear(); release(root);
}
struct Use {
    Root* root;
    explicit Use(Root* value) : root(value) { ++root->calls; }
    ~Use() { finish(root); }
};
struct TempEngineText {
    game::EngineText value;
    ~TempEngineText() { game::drop_shared(value.counter()); }
};
bool pin(Root& root, uint64_t handle) {
    if (!handle) return false;
    for (uint32_t i = 0; i < root.pins.count; ++i) if (root.pins[i] == handle) return true;
    if (!root.pins.reserve(root.pins.count + 1) || !retain_game_object(handle)) return false;
    root.pins.add(handle); return true;
}
void detach(uint64_t handle) {
    const uint64_t widget = resolve(handle);
    const auto remove = widget ? game::method(widget, t::ui_widget::remove_from_parent) : nullptr;
    if (remove) remove(reinterpret_cast<void*>(widget));
}
void detach_attached(uint64_t handle) {
    const uint64_t object = resolve(handle);
    if (!object) return;
    const bool parented = game::read_or<uint64_t>(object + t::ui_mod_list_tab::slot) != 0;
    const bool viewport = game::is_a(object, user_class) && t::fn::ui_widget_in_viewport &&
        t::fn::ui_widget_in_viewport(reinterpret_cast<void*>(object));
    if (parented || viewport) detach(handle);
}
void cleanup(Root& root) {
    // All native calls using this state have returned before the final reference
    // releases its objects. Native Construct/Destruct may collect garbage.
    // The mod's other UserWidgets in this one's panels come out first: they are their own roots, and stay.
    for (uint32_t i = root.nested.count; i--;) detach_attached(root.nested[i]);
    detach_attached(root.widget);
    for (uint32_t i = root.made.count; i--;) detach_attached(root.made[i]);
    for (uint32_t i = root.pins.count; i--;) release_game_object(root.pins[i]);
}
void retire(Root& root) {
    if (root.retired) return;
    root.retired = true;
    root.observations.clear();
    // Forget ownership before native teardown: Blueprint Destruct can reenter.
    root.viewport = false;
}
TSUCHI_Result create(const Hold& hold, Root*& kept, const Want& wanted, bool fresh) {
    if (!fresh) return TSUCHI_BUSY;
    Root* root = wanted.root; ++root->calls; kept = root;
    hold_apply_checkpoint(hold);
    auto permit = hold_call_guard(hold);
    TSUCHI_Result result = TSUCHI_NOT_READY;
    const uint64_t context = resolve(root->context), type = resolve(wanted.type);
    if (permit.permits() && context && type && t::bound(t::fn::ui_create_widget) && pin(*root, wanted.type)) {
        const uint64_t made = reinterpret_cast<uint64_t>(t::fn::ui_create_widget(
            reinterpret_cast<void*>(context), reinterpret_cast<void*>(type), nullptr));
        root->widget = game::to_handle(made);
        if (root->widget && game::is_a(made, user_class)) {
            const uint64_t tree = game::read_or<uint64_t>(made + t::ui_user_widget::tree);
            root->tree = game::to_handle(tree);
            if (!permit.permits() || !active(*root)) result = TSUCHI_NOT_READY;
            else if (!root->tree || !game::is_a(tree, tree_class)) result = TSUCHI_NOT_READY;
            else if (!pin(*root, root->widget) || !pin(*root, root->tree)) result = TSUCHI_NO_MEMORY;
            else result = TSUCHI_OK;
        }
    } else if (permit.permits() && context && type && t::fn::ui_create_widget) result = TSUCHI_NO_MEMORY;
    // A stopped operation is restored by Holds after the native call returns.
    if (result != TSUCHI_OK && permit.permits()) { retire(*root); finish(root); kept = nullptr; }
    return result;
}
bool keep(const Hold&, Root*& root) { return root && active(*root); }
void restore(const Hold&, Root*& root, HoldEnd) {
    if (!root) return;
    retire(*root); finish(root); root = nullptr;
}
Holds<Root*, Want, create, keep, restore> roots{"ui.widget", HoldOn::object, false};
Root* owner_of(uint64_t object) {
    for (uint32_t n = 0; object && n < 128; ++n) {
        // The nearest independently created UserWidget owns its entire authored
        // subtree. Never cross that boundary to claim another mod's children.
        const uint64_t handle = game::to_handle(object);
        for (uint32_t i = 0; i < roots.count(); ++i) {
            Root* root = roots.kept_at(i);
            if (root && root->widget == handle) return root;
        }
        if (game::is_a(object, tree_class)) {
            const uint64_t user = game::outer_of(object);
            if (!game::is_a(user, user_class) ||
                game::read_or<uint64_t>(user + t::ui_user_widget::tree) != object) return nullptr;
        }
        object = game::outer_of(object);
    }
    return nullptr;
}
bool belongs(const Root& root, uint64_t object) { return owner_of(object) == &root; }
TSUCHI_Result locate(TSUCHI_Mod* mod, uint64_t handle, Root*& found, bool root_only = false) {
    found = nullptr;
    if (!available() || !mod || !mod_usable(mod)) return TSUCHI_INVALID;
    const uint64_t object = resolve(handle);
    if (!object) return TSUCHI_NOT_FOUND;
    if (!game::is_a(object, widget_class)) return TSUCHI_INVALID;
    // Independently created roots may have another widget's tree as their Outer.
    // Their explicit ownership wins over that earlier root's ancestry.
    for (uint32_t i = 0; i < roots.count(); ++i) {
        Root* root = roots.kept_at(i);
        if (!root || root->retired || root->widget != handle) continue;
        if (root->mod != mod) return TSUCHI_DENIED;
        if (!active(*root)) return TSUCHI_NOT_READY;
        found = root; return TSUCHI_OK;
    }
    if (root_only) return TSUCHI_NOT_FOUND;
    for (uint32_t i = 0; i < roots.count(); ++i) {
        Root* root = roots.kept_at(i);
        if (!root || root->retired) continue;
        if (!belongs(*root, object)) continue;
        if (root->mod != mod) return TSUCHI_DENIED;
        if (!active(*root)) return TSUCHI_NOT_READY;
        found = root; return TSUCHI_OK;
    }
    return TSUCHI_NOT_FOUND;
}
TSUCHI_Result concrete(uint64_t handle, game::ClassRef& base, bool reject_user, uint64_t& type) {
    type = resolve(handle);
    if (!type) return TSUCHI_NOT_FOUND;
    const uint64_t wanted = game::find_class(base), user = game::find_class(user_class);
    if (!wanted || (reject_user && !user)) return TSUCHI_NOT_READY;
    if (!game::is_class(type) || !game::derives_from(type, wanted) ||
        (reject_user && game::derives_from(type, user)))
        return TSUCHI_INVALID;
    uint32_t flags = 0;
    if (!game::read(type + t::ui_widget_class_flags::flags, flags)) return TSUCHI_NOT_READY;
    if (flags & t::constant::ui_widget_abstract) return TSUCHI_INVALID;
    return TSUCHI_OK;
}
bool tree_children(uint64_t object, List<uint64_t>& pending) {
    if (!game::is_a(object, panel_class)) return true;
    Array slots{};
    if (!game::read(object + t::ui_panel::slots, slots) || slots.count < 0 || slots.count > slots.capacity ||
        (slots.count && !game::readable(slots.data, uint64_t(slots.count) * 8))) return false;
    for (int32_t i = 0; i < slots.count; ++i) {
        const uint64_t slot = game::read_or<uint64_t>(slots.data + uint64_t(i) * 8);
        const uint64_t child = game::read_or<uint64_t>(slot + t::ui_panel_slot::content);
        if (!game::live(child)) return false;
        bool seen = false;
        for (uint32_t j = 0; j < pending.count; ++j) if (pending[j] == child) { seen = true; break; }
        if (!seen && !pending.add(child)) return false;
    }
    return true;
}
Observation* observation(Root& root, uint64_t widget) {
    for (uint32_t i = 0; i < root.observations.count; ++i)
        if (root.observations[i].widget == widget) return &root.observations[i];
    return nullptr;
}
void broadcast(void* list, void* payload) {
    if (on_game_thread()) {
        const uint64_t at = reinterpret_cast<uint64_t>(list);
        const uint64_t offsets[] = {t::ui_widget_button::clicked, t::ui_widget_button::pressed,
            t::ui_widget_button::released, t::ui_widget_button::hovered, t::ui_widget_button::unhovered};
        for (uint32_t i = 0; i < roots.count(); ++i) {
            Root* root = roots.kept_at(i);
            if (!root || !active(*root)) continue;
            for (uint32_t j = 0; j < root->observations.count; ++j) {
                const Observation observed = root->observations[j];
                const uint64_t widget = resolve(observed.widget);
                for (uint32_t signal = 0; widget && signal < 5; ++signal)
                    if (at == widget + offsets[signal])
                        notices.add({root->mod, root->widget, observed.widget, observed.token, signal});
            }
        }
    }
    // This is the actual producer's continuation, even when no delegate is bound.
    next_broadcast(list, payload);
}
void deliver() {
    List<Notice> sending = notices; notices = {};
    for (uint32_t i = 0; i < sending.count; ++i) {
        const Notice event = sending[i];
        Root* root = nullptr;
        if (locate(event.mod, event.root, root, true) != TSUCHI_OK) continue;
        Use use(root);
        const Observation* current = observation(*root, event.widget);
        if (!current || current->token != event.token || !resolve(event.widget)) continue;
        ui::emit_button_event_to(event.mod, {{event.widget}, event.signal});
    }
    sending.clear();
}
} // namespace

void ui_widgets_start(TSUCHI_Mod* owner) {
    framework = owner; roots.start();
    if (!steps_registered) steps_registered = each_frame(deliver);
    auto& backend = ui_screen_backend();
    backend.create_widget = ui_create_widget;
    backend.destroy_widget = ui_destroy_widget;
    backend.construct_widget = ui_construct_widget;
    backend.find_widget = ui_find_widget;
    backend.set_widget_text = ui_set_widget_text;
    backend.set_root_widget = ui_set_root_widget;
    backend.add_child = ui_add_child;
    backend.hold_widget = ui_hold_widget;
    backend.release_widget = ui_release_widget;
    backend.hold_button_events = ui_hold_button_events;
    backend.release_button_events = ui_release_button_events;
    ui::set_backend(&backend);
}
TSUCHI_Result ui_create_widget(TSUCHI_Mod* mod, TSUCHI_Object context, TSUCHI_Object of_class, TSUCHI_Object* out) {
    if (!out || !available() || !mod || !mod_usable(mod)) return TSUCHI_INVALID;
    const uint64_t from = resolve(context.id);
    if (!from) return TSUCHI_NOT_FOUND;
    const uint64_t world = game::to_handle(game::loaded_world());
    if (!world) return TSUCHI_NOT_READY;
    if (from != resolve(world) && !game::is_a(from, widget_class) && !game::is_a(from, actor_class)) return TSUCHI_INVALID;
    const uint64_t engine = game::global<uint64_t>(t::data::engine);
    if (!engine || !t::bound(t::fn::ui_context_world)) return TSUCHI_NOT_READY;
    const uint64_t context_world = reinterpret_cast<uint64_t>(t::fn::ui_context_world(
        reinterpret_cast<void*>(engine), reinterpret_cast<void*>(from), 0));
    if (!mod_usable(mod) || !resolve(context.id) || game::to_handle(context_world) != world ||
        game::to_handle(game::loaded_world()) != world) return TSUCHI_NOT_READY;
    uint64_t type = 0;
    TSUCHI_Result result = concrete(of_class.id, user_class, false, type);
    if (result != TSUCHI_OK) return result;
    auto* root = static_cast<Root*>(allocate(sizeof(Root)));
    if (!root) return TSUCHI_NO_MEMORY;
    *root = {}; root->calls = 1; root->mod = mod; root->context = context.id; root->world = world; root->key = next_root++;
    result = roots.take(mod, context.id, root->key, {root, of_class.id});
    if (result == TSUCHI_OK && active(*root)) *out = {root->widget};
    else {
        result = result == TSUCHI_OK ? TSUCHI_NOT_READY : result;
        if (!root->retired) {
            roots.release(mod, context.id, root->key);
            if (!root->retired) retire(*root);
        }
    }
    finish(root); return result;
}
TSUCHI_Result ui_destroy_widget(TSUCHI_Mod* mod, TSUCHI_Object widget) {
    Root* root = nullptr; const auto result = locate(mod, widget.id, root, true);
    if (result != TSUCHI_OK) return result;
    return roots.release(mod, root->context, root->key);
}
TSUCHI_Result ui_construct_widget(TSUCHI_Mod* mod, TSUCHI_Object widget, TSUCHI_Object of_class, TSUCHI_Object* out) {
    if (!out) return TSUCHI_INVALID;
    Root* root = nullptr; auto result = locate(mod, widget.id, root, true);
    if (result != TSUCHI_OK) return result;
    Use use(root); uint64_t type = 0;
    result = concrete(of_class.id, widget_class, true, type);
    if (result != TSUCHI_OK) return result;
    if (!t::bound(t::fn::ui_construct) || !pin(*root, of_class.id)) return t::bound(t::fn::ui_construct) ? TSUCHI_NO_MEMORY : TSUCHI_NOT_READY;
    const uint64_t tree = resolve(root->tree);
    if (!tree || !root->made.reserve(root->made.count + 1)) return tree ? TSUCHI_NO_MEMORY : TSUCHI_NOT_READY;
    uint64_t parameters[8]{}; parameters[0] = type; parameters[1] = tree;
    const auto permit = guard(*root);
    const uint64_t made = reinterpret_cast<uint64_t>(t::fn::ui_construct(parameters));
    const uint64_t handle = game::to_handle(made);
    if (!permit.permits()) { if (handle) detach(handle); return TSUCHI_NOT_READY; }
    if (!handle || !game::is_a(made, widget_class) || !belongs(*root, made)) return TSUCHI_NOT_READY;
    if (!pin(*root, handle)) return TSUCHI_NO_MEMORY;
    root->made.add(handle); *out = {handle}; return TSUCHI_OK;
}
TSUCHI_Result ui_find_widget(TSUCHI_Mod* mod, TSUCHI_Object widget, const char* name, TSUCHI_Object* out) {
    if (!out || !name || !*name) return TSUCHI_INVALID;
    Root* root = nullptr; const auto result = locate(mod, widget.id, root);
    if (result != TSUCHI_OK) return result;
    Use use(root); List<uint64_t> pending;
    const uint64_t user = resolve(widget.id);
    if (!game::is_a(user, user_class)) return TSUCHI_INVALID;
    const uint64_t tree = game::read_or<uint64_t>(user + t::ui_user_widget::tree);
    if (!game::is_a(tree, tree_class) || game::outer_of(tree) != user) return TSUCHI_NOT_READY;
    const uint64_t first = tree ? game::read_or<uint64_t>(tree + t::ui_widget_tree::root) : 0;
    if (!tree) return TSUCHI_NOT_READY;
    if (first && (!game::live(first) || !pending.add(first))) return TSUCHI_NO_MEMORY;
    uint64_t found = 0;
    for (uint32_t i = 0; i < pending.count; ++i) {
        const uint64_t object = pending[i];
        if (game::name_is(game::name_of(object), name)) { found = game::to_handle(object); break; }
        if (!tree_children(object, pending)) { pending.clear(); return TSUCHI_NOT_READY; }
    }
    pending.clear();
    if (!active(*root)) return TSUCHI_NOT_READY;
    if (found && !belongs(*root, resolve(found))) return TSUCHI_DENIED;
    if (found && !pin(*root, found)) return TSUCHI_NO_MEMORY;
    *out = {found}; return TSUCHI_OK;
}
TSUCHI_Result ui_set_widget_text(TSUCHI_Mod* mod, TSUCHI_Object widget, const char* text) {
    char16_t units[kUiTextBytes + 1]; size_t count = 0;
    if (!ui_glyph_text_syntax(text, true) || !game::utf8_to_units(text, units, kUiTextBytes, count))
        return TSUCHI_INVALID;
    units[count] = 0;
    Root* root = nullptr; const auto result = locate(mod, widget.id, root);
    if (result != TSUCHI_OK) return result;
    Use use(root);
    const uint64_t object = resolve(widget.id);
    if (!game::is_a(object, text_class)) return TSUCHI_INVALID;
    if (!t::bound(t::fn::text_from_string) || !t::bound(t::fn::rich_text_set)) return TSUCHI_NOT_READY;
    if (!pin(*root, widget.id)) return TSUCHI_NO_MEMORY;
    const auto permit = guard(*root);
    if (!permit.permits()) return TSUCHI_NOT_READY;
    alignas(8) uint8_t string[t::array::size]{};
    const uint64_t data = reinterpret_cast<uint64_t>(units);
    const int32_t length = int32_t(count + 1);
    memcpy(string + t::array::data, &data, sizeof data);
    memcpy(string + t::array::count, &length, sizeof length);
    memcpy(string + t::array::max, &length, sizeof length);
    // Both native calls can stop the owner or replace its world. Keep the root,
    // label and temporary text alive through those callbacks, and never write
    // after conversion if that captured ownership has ended.
    TempEngineText made{t::fn::text_from_string(string)};
    if (!permit.permits() || resolve(widget.id) != object || !belongs(*root, object)) return TSUCHI_NOT_READY;
    t::fn::rich_text_set(reinterpret_cast<void*>(object), &made.value);
    return permit.permits() && resolve(widget.id) == object && belongs(*root, object) ? TSUCHI_OK : TSUCHI_NOT_READY;
}
TSUCHI_Result ui_set_root_widget(TSUCHI_Mod* mod, TSUCHI_Object widget, TSUCHI_Object child) {
    Root* root = nullptr; auto result = locate(mod, widget.id, root, true);
    if (result != TSUCHI_OK) return result;
    Use use(root); Root* other = nullptr; result = locate(mod, child.id, other);
    if (result != TSUCHI_OK) return result;
    if (other != root) return TSUCHI_DENIED;
    if (child.id == root->widget) return TSUCHI_INVALID;
    const uint64_t user_widget = resolve(root->widget);
    // TakeWidget caches the Slate tree, including an empty spacer. Replacing
    // WidgetTree.RootWidget after that does not rebuild the displayed content.
    if (root->viewport || root->adding_viewport || root->removing ||
        game::read_or<uint64_t>(user_widget + t::ui_mod_list_tab::slot) ||
        game::read_or<uint64_t>(user_widget + t::ui_widget::cached_slate) ||
        (t::bound(t::fn::ui_widget_in_viewport) && t::fn::ui_widget_in_viewport(reinterpret_cast<void*>(user_widget))))
        return TSUCHI_BUSY;
    bool made = false; for (uint32_t i = 0; i < root->made.count; ++i) if (root->made[i] == child.id) made = true;
    const uint64_t object = resolve(child.id), tree = resolve(root->tree);
    if (!made || !tree || game::read_or<uint64_t>(object + t::ui_mod_list_tab::slot)) return TSUCHI_INVALID;
    if (game::read_or<uint64_t>(tree + t::ui_widget_tree::root)) return TSUCHI_BUSY;
    return game::write<uint64_t>(tree + t::ui_widget_tree::root, object) ? TSUCHI_OK : TSUCHI_NOT_READY;
}
TSUCHI_Result ui_add_child(TSUCHI_Mod* mod, TSUCHI_Object panel, TSUCHI_Object child, TSUCHI_Object* out) {
    if (!out) return TSUCHI_INVALID;
    Root* root = nullptr; auto result = locate(mod, panel.id, root);
    if (result != TSUCHI_OK) return result;
    Use use(root); Root* other = nullptr; result = locate(mod, child.id, other);
    if (result != TSUCHI_OK) return result;
    // A widget of this tree, or another UserWidget the mod created, whole: never a part of that one's tree.
    const bool nested = other != root;
    if (nested && child.id != other->widget) return TSUCHI_DENIED;
    if (!nested && child.id == root->widget) return TSUCHI_INVALID;
    if (nested && (other->viewport || other->adding_viewport || other->removing ||
        (t::bound(t::fn::ui_widget_in_viewport) && t::fn::ui_widget_in_viewport(reinterpret_cast<void*>(resolve(child.id)))))) return TSUCHI_BUSY;
    if (nested && !root->nested.reserve(root->nested.count + 1)) return TSUCHI_NO_MEMORY;
    const uint64_t parent = resolve(panel.id), object = resolve(child.id);
    if (!game::is_a(parent, panel_class) || parent == object) return TSUCHI_INVALID;
    const uint64_t child_tree = game::outer_of(object);
    if (game::read_or<uint64_t>(object + t::ui_mod_list_tab::slot) ||
        (game::is_a(child_tree, tree_class) && game::read_or<uint64_t>(child_tree + t::ui_widget_tree::root) == object)) return TSUCHI_BUSY;
    uint64_t ancestor = parent;
    for (uint32_t n = 0; ancestor && n < 128; ++n) {
        if (ancestor == object) return TSUCHI_INVALID;
        const uint64_t slot = game::read_or<uint64_t>(ancestor + t::ui_mod_list_tab::slot);
        if (slot) ancestor = game::read_or<uint64_t>(slot + t::ui_mod_list_tab::parent);
        else {
            // The top of a tree: on to the UserWidget whose tree it is, which may sit in a panel itself.
            uint64_t outer = game::outer_of(ancestor);
            for (uint32_t hops = 0; outer && !game::is_a(outer, tree_class) && hops < 8; ++hops) outer = game::outer_of(outer);
            const uint64_t user = outer && game::is_a(outer, tree_class) ? game::outer_of(outer) : 0;
            ancestor = user && user != ancestor && game::is_a(user, user_class) ? user : 0;
        }
        if (n == 127 && ancestor) return TSUCHI_INVALID;
    }
    if (!t::bound(t::fn::ui_add_child)) return TSUCHI_NOT_READY;
    const uint64_t slot = reinterpret_cast<uint64_t>(t::fn::ui_add_child(reinterpret_cast<void*>(parent), reinterpret_cast<void*>(object)));
    const uint64_t handle = game::to_handle(slot);
    if (!active(*root)) { detach(child.id); return TSUCHI_NOT_READY; }
    if (!handle || game::read_or<uint64_t>(object + t::ui_mod_list_tab::slot) != slot ||
        game::read_or<uint64_t>(slot + t::ui_mod_list_tab::parent) != parent ||
        game::read_or<uint64_t>(slot + t::ui_panel_slot::content) != object) {
        detach(child.id); return TSUCHI_NOT_READY;
    }
    if (!pin(*root, handle)) { detach(child.id); return TSUCHI_NO_MEMORY; }
    if (nested) {
        bool listed = false;
        for (uint32_t i = 0; i < root->nested.count; ++i) listed = listed || root->nested[i] == child.id;
        if (!listed) root->nested.add(child.id);
    }
    *out = {handle}; return TSUCHI_OK;
}
TSUCHI_Result ui_hold_widget(TSUCHI_Mod* mod, TSUCHI_Object widget, int32_t z_order) {
    Root* root = nullptr; const auto result = locate(mod, widget.id, root, true);
    if (result != TSUCHI_OK) return result;
    Use use(root);
    if (root->removing || root->adding_viewport) return TSUCHI_BUSY;
    const uint64_t object = resolve(widget.id);
    if (!t::bound(t::fn::ui_widget_viewport) || !t::bound(t::fn::ui_widget_in_viewport)) return TSUCHI_NOT_READY;
    const bool shown = t::fn::ui_widget_in_viewport(reinterpret_cast<void*>(object));
    if (root->viewport && shown) return root->z_order == z_order ? TSUCHI_OK : TSUCHI_BUSY;
    if (!root->viewport && shown) return TSUCHI_BUSY;
    root->viewport = false;
    if (game::read_or<uint64_t>(object + t::ui_mod_list_tab::slot)) return TSUCHI_BUSY;
    root->viewport = true; root->z_order = z_order; root->adding_viewport = true;
    t::fn::ui_widget_viewport(reinterpret_cast<void*>(object), z_order, false);
    root->adding_viewport = false;
    if (!active(*root)) { detach(widget.id); return TSUCHI_NOT_READY; }
    if (!root->viewport || root->z_order != z_order || !t::fn::ui_widget_in_viewport(reinterpret_cast<void*>(resolve(widget.id)))) {
        root->viewport = false; return TSUCHI_NOT_READY;
    }
    return TSUCHI_OK;
}
TSUCHI_Result ui_release_widget(TSUCHI_Mod* mod, TSUCHI_Object widget) {
    Root* root = nullptr; const auto result = locate(mod, widget.id, root, true);
    if (result != TSUCHI_OK) return result;
    Use use(root);
    if (root->removing || root->adding_viewport) return TSUCHI_BUSY;
    if (!root->viewport) return TSUCHI_NOT_FOUND;
    if (!t::bound(t::fn::ui_widget_in_viewport) || !game::method(resolve(widget.id), t::ui_widget::remove_from_parent)) return TSUCHI_NOT_READY;
    root->viewport = false; root->removing = true;
    detach(widget.id); root->removing = false;
    if (!active(*root)) return TSUCHI_NOT_READY;
    if (t::fn::ui_widget_in_viewport(reinterpret_cast<void*>(resolve(widget.id)))) {
        root->viewport = true; return TSUCHI_NOT_READY;
    }
    return TSUCHI_OK;
}
TSUCHI_Result ui_hold_button_events(TSUCHI_Mod* mod, TSUCHI_Object widget) {
    Root* root = nullptr; const auto result = locate(mod, widget.id, root);
    if (result != TSUCHI_OK) return result;
    Use use(root);
    if (!game::is_a(resolve(widget.id), button_class)) return TSUCHI_INVALID;
    if (observation(*root, widget.id)) return TSUCHI_OK;
    if (!hooked) {
        if (!steps_registered || !code_site_patched(t::code_site::ui_widget_broadcast.number)) return TSUCHI_NOT_READY;
        uint64_t id = 0;
        if (hook_site(framework, t::code_site::ui_widget_broadcast, broadcast, &next_broadcast, &id) != TSUCHI_OK) return TSUCHI_NOT_READY;
        hooked = true;
    }
    if (!active(*root)) return TSUCHI_NOT_READY;
    if (!pin(*root, widget.id) || !root->observations.add({widget.id, next_observation++})) return TSUCHI_NO_MEMORY;
    return TSUCHI_OK;
}
TSUCHI_Result ui_release_button_events(TSUCHI_Mod* mod, TSUCHI_Object widget) {
    Root* root = nullptr; const auto result = locate(mod, widget.id, root);
    if (result != TSUCHI_OK) return result;
    Observation* found = observation(*root, widget.id);
    if (!found) return TSUCHI_NOT_FOUND;
    root->observations.remove(found); return TSUCHI_OK;
}
} // namespace tsuchi
