#include "content_squad.h"
#include <string.h>
#include "actors.h"
#include "asset_service.h"
#include "captain.h"
#include "frame.h"
#include "game.h"
#include "holds.h"
#include "hooks.h"
#include "ids.h"
#include "motion.h"
#include "objects.h"
#include "pikmin.h"
#include "pikmin_hud.h"
#include "registry.h"

namespace tsuchi {
namespace {
namespace g = game;
namespace t = game::table;
constexpr uint32_t kTypes = 8, kLeaders = 4;
// These are backing native list nodes, not records of holds. The members and
// their owner are only in Holds below. A type's number is never public.
// The game reads them by its own layout, so each field is at the selected build's offset.
struct Node {
    alignas(8) uint8_t bytes[t::squad_node::most];
    template <class T>
    T get(uint64_t offset) const {
        T value;
        memcpy(&value, bytes + offset, sizeof value);
        return value;
    }
    template <class T>
    void put(uint64_t offset, T value) { memcpy(bytes + offset, &value, sizeof value); }
    uint64_t previous() const { return get<uint64_t>(t::squad_node::previous); }
    uint64_t next() const { return get<uint64_t>(t::squad_node::next); }
    uint64_t self() const { return get<uint64_t>(t::squad_node::self); }
    uint64_t owner() const { return get<uint64_t>(t::squad_node::owner); }
    int32_t count() const { return get<int32_t>(t::squad_node::count); }
    void set_kind(int32_t kind) { put(t::squad_node::kind, kind); }
    void set_count(int32_t count) { put(t::squad_node::count, count); }
};
struct Type { TSUCHI_Mod* mod; char id[kIdBytes]; Text name; uint64_t texture; bool live, pending; };
Type types[kTypes]{};
struct Column { uint64_t leader, formation, formation_handle; Node nodes[kTypes]; uint8_t borrowed[kTypes]; };
Column columns[kLeaders]{};
Vocabulary vocabulary{"squad.SquadType", nullptr, 0, kTypes - 1};
const Kind kType{"content.squad"};
bool ready = false;
void (*hud_before)(void*) = nullptr;
void (*cursor_before)(void*, void*, float) = nullptr;
uint64_t (*grab_before[2])(void*, uint64_t){};
void* pointer(uint64_t at) { return reinterpret_cast<void*>(at); }

bool text(const Json* value) {
    return value && value->kind == Json::Kind::text && value->bytes && strlen(value->text) == value->bytes;
}
bool wrong(Text& out, const char* message) {
    if (out.reserve(strlen(message) + 1)) memcpy(out.data, message, strlen(message) + 1);
    return false;
}
TSUCHI_Result slot_of(const char* id, uint32_t& out) {
    const TSUCHI_Result result = id_value(vocabulary, id, &out);
    if (result != TSUCHI_OK) return result;
    return out < kTypes && types[out].live ? TSUCHI_OK : TSUCHI_NOT_FOUND;
}
uint64_t formation(uint64_t leader) {
    const uint64_t actor = g::from_handle(leader);
    const uint64_t part = actor ? g::ai_component(actor) : 0;
    const auto ask = part ? g::method(part, t::avatar_table::squad) : nullptr;
    const uint64_t result = ask ? ask(pointer(part)) : 0;
    return result && g::to_handle(result) &&
        g::readable(result + t::squad_formation::list, t::squad_node::owner) ? result : 0;
}
bool live_formation(const Column& col) {
    return col.formation && col.formation_handle && g::from_handle(col.formation_handle) == col.formation;
}
bool linked(uint64_t fs, const Node& node) {
    bool valid = false;
    for (const auto& col : columns) if (col.formation == fs && live_formation(col)) valid = true;
    if (!valid) return false;
    if (!fs || node.owner() != fs + t::squad_formation::list) return false;
    const uint64_t sentinel = fs + t::squad_formation::list;
    uint64_t at = g::read_or<uint64_t>(sentinel + t::squad_node::next);
    for (uint32_t step = 0; at && at != sentinel && step < 64; ++step) {
        if (at == reinterpret_cast<uint64_t>(&node)) return true;
        bool own = false;
        const uint64_t looking_at = at;
        for (const auto& col : columns) for (const auto& entry : col.nodes)
            if (looking_at == reinterpret_cast<uint64_t>(&entry)) { at = entry.next(); own = true; }
        if (!own) at = g::read_or<uint64_t>(at + t::squad_node::next);
    }
    return false;
}
void put_next(uint64_t at, uint64_t next) {
    for (auto& col : columns) for (auto& node : col.nodes)
        if (at == reinterpret_cast<uint64_t>(&node)) { node.put(t::squad_node::next, next); return; }
    g::write(at + t::squad_node::next, next);
}
void put_previous(uint64_t at, uint64_t previous) {
    for (auto& col : columns) for (auto& node : col.nodes)
        if (at == reinterpret_cast<uint64_t>(&node)) { node.put(t::squad_node::previous, previous); return; }
    g::write(at + t::squad_node::previous, previous);
}
void unlink(Column& col, Node& node) {
    if (live_formation(col) && linked(col.formation, node)) {
        const uint64_t sentinel = col.formation + t::squad_formation::list;
        if (g::read_or<uint64_t>(sentinel + t::squad_node::next) == reinterpret_cast<uint64_t>(&node)) {
            g::write(col.formation + t::squad_formation::throw_cache_a, UINT32_MAX);
            g::write(col.formation + t::squad_formation::throw_cache_b, UINT32_MAX);
        }
        put_next(node.previous(), node.next());
        put_previous(node.next(), node.previous());
        const int32_t count = g::read_or<int32_t>(col.formation + t::squad_formation::count);
        if (count > 0) g::write(col.formation + t::squad_formation::count, count - 1);
    }
    node = Node{};
}
void link(Column& col, Node& node, uint32_t count) {
    if (!live_formation(col)) return;
    const uint64_t sentinel = col.formation + t::squad_formation::list;
    const uint64_t last = g::read_or<uint64_t>(sentinel + t::squad_node::previous);
    if (!last) return;
    node = Node{};
    const uint64_t self = reinterpret_cast<uint64_t>(&node);
    node.put(t::squad_node::previous, last), node.put(t::squad_node::next, sentinel);
    node.put(t::squad_node::self, self), node.put(t::squad_node::owner, sentinel);
    node.set_kind(int32_t(t::constant::squad_other_kind)), node.set_count(int32_t(count));
    node.put(t::squad_node::members, self + t::squad_node::members), node.put(t::squad_node::members_last, self + t::squad_node::members);
    put_next(last, self);
    g::write(sentinel + t::squad_node::previous, self);
    g::write(col.formation + t::squad_formation::count, g::read_or<int32_t>(col.formation + t::squad_formation::count) + 1);
}

struct Member { uint64_t leader, ai; uint32_t slot; float velocity[3]; bool ticking, awake, held, thrown; };
struct Wanted { uint64_t leader; uint32_t slot; };
TSUCHI_Result apply_member(const Hold&, Member&, const Wanted&, bool);
bool keep_member(const Hold&, Member&);
void restore_member(const Hold&, Member&, HoldEnd);
Holds<Member, Wanted, apply_member, keep_member, restore_member> members{"squad.member", HoldOn::watched_actor, true};

bool throw_velocity(uint64_t leader, float out[3]) {
    if (!t::bound(t::fn::squad_throw_trajectory)) return false;
    g::Transform at;
    float target[3];
    if (!g::get_transform(leader, at) || !g::read_cursor(leader, target)) return false;
    float height = 150.f;
    g::each_pikmin([](void* context, uint64_t pikmin) {
        const uint64_t part = g::read_or<uint64_t>(pikmin + t::game_actor::pikmin);
        const float candidate = part ? g::read_or<float>(part + t::squad_pikmin::throw_height) : 0;
        if (candidate > 40.f && candidate < 1000.f) { *static_cast<float*>(context) = candidate; return false; }
        return true;
    }, &height);
    const uint64_t config = g::global<uint64_t>(t::data::pikmin_scatter_config);
    float length = config ? g::read_or<float>(config + t::squad_game_config::throw_length_max) : 0;
    if (!(length > 100.f && length < 10000.f)) length = 1000.f;
    namespace s = t::squad_throw_solution;
    alignas(8) uint8_t solution[s::most]{};
    float start[3]{at.location[0], at.location[1], at.location[2] + 30.f};
    const float apex = height > 30.f ? height - 30.f : 0;
    const float one = 1.f, gravity = 980.f;
    const uint8_t limited = 1;
    memcpy(solution + s::start, start, sizeof start), memcpy(solution + s::target, target, sizeof target);
    memcpy(solution + s::thrower, at.location, sizeof at.location), memcpy(solution + s::apex, &apex, sizeof apex);
    memcpy(solution + s::one, &one, sizeof one), memcpy(solution + s::limit, &limited, sizeof limited);
    memcpy(solution + s::length_max, &length, sizeof length), memcpy(solution + s::gravity, &gravity, sizeof gravity);
    const uint32_t result = t::fn::squad_throw_trajectory(solution);
    uint64_t path = 0;
    memcpy(&path, solution + s::path, sizeof path);
    if (path) g::release_engine_memory(pointer(path));
    memcpy(out, solution + s::velocity, sizeof(float) * 3);
    return result && finite(out[0]) && finite(out[1]) && finite(out[2]) && (out[0] != 0 || out[1] != 0 || out[2] != 0);
}

Column* column(uint64_t leader, bool make) {
    for (auto& col : columns) if (col.leader == leader) return &col;
    if (!make) return nullptr;
    for (auto& col : columns) {
        bool used = false;
        for (uint32_t i = 0; i < members.count(); ++i) if (members.kept_at(i).leader == col.leader) used = true;
        if (used && col.leader) continue;
        for (auto& node : col.nodes) unlink(col, node);
        col = Column{}; col.leader = leader;
        return &col;
    }
    return nullptr;
}
void refresh() {
    for (auto& col : columns) {
        if (!col.leader) continue;
        const uint64_t fs = formation(col.leader);
        const uint64_t identity = fs ? g::to_handle(fs) : 0;
        if (fs != col.formation || identity != col.formation_handle) {
            for (auto& node : col.nodes) unlink(col, node);
            col.formation = fs; col.formation_handle = identity;
        }
        for (uint32_t s = 0; s < kTypes; ++s) {
            uint32_t count = 0;
            for (uint32_t i = 0; i < members.count(); ++i)
                if (members.kept_at(i).leader == col.leader && members.kept_at(i).slot == s) ++count;
            Node& node = col.nodes[s];
            if (fs && count && types[s].live) {
                if (!linked(fs, node)) link(col, node, count);
                else node.set_count(int32_t(count));
            } else unlink(col, node);
        }
    }
}
void notice(uint64_t actor, const Member& member, bool joined) {
    squad::emit_membership_changed(TSUCHI_SquadMembershipChanged{{actor}, {member.leader}, types[member.slot].id, joined});
}
TSUCHI_Result apply_member(const Hold& hold, Member& kept, const Wanted& wanted, bool fresh) {
    if (!ready) return TSUCHI_NOT_READY;
    const uint64_t actor = g::from_handle(hold.object), leader = g::from_handle(wanted.leader);
    if (!actor || !g::is_game_actor(actor) || !leader || !g::is_leader(leader)) return TSUCHI_NOT_FOUND;
    if (g::is_leader(actor) || g::is_pikmin(actor)) return TSUCHI_INVALID;
    if (!column(wanted.leader, true)) return TSUCHI_CAPACITY;
    if (fresh) {
        const uint64_t ai = g::ai_component(actor);
        if (!g::can_launch(actor) || !ai || !g::component_ticking(ai, kept.ticking) || !g::get_velocity(actor, kept.velocity)) return TSUCHI_NOT_READY;
        kept.ai = g::to_handle(ai);
        kept.leader = wanted.leader, kept.slot = wanted.slot;
        hold_apply_checkpoint(hold);
        g::set_component_ticking(ai, false);
        if (!hold_call_guard(hold).permits()) return TSUCHI_NOT_READY;
        g::hold_awake(actor, true);
        kept.awake = true;
    }
    kept.leader = wanted.leader, kept.slot = wanted.slot;
    notice(hold.object, kept, true);
    return TSUCHI_OK;
}
bool keep_member(const Hold& hold, Member& kept) {
    const uint64_t actor = g::from_handle(hold.object), leader = g::from_handle(kept.leader), ai = g::from_handle(kept.ai);
    if (!actor || !leader || !ai || !types[kept.slot].live || !g::is_leader(leader)) return false;
    g::set_component_ticking(ai, false);
    if (!hold_call_guard(hold).permits()) return false;
    if (!g::can_launch(actor)) return true; // the captain's own hold owns it while it is in the hand
    if (kept.held) {
        float v[3];
        if (throw_velocity(leader, v)) {
            if (!hold_call_guard(hold).permits()) return false;
            g::request_velocity(actor, v);
            motion_begin(actor, v);
            kept.thrown = true;
        }
        return false; // a thrown member leaves the squad
    }
    g::Transform a, l;
    if (!g::get_transform(actor, a) || !g::get_transform(leader, l)) return false;
    // The native formation faces +X at zero yaw. Keep rows behind it, using
    // the game's sine and cosine functions already used for Pikmin scatter.
    uint32_t index = 0;
    for (uint32_t i = 0; i < members.count(); ++i) {
        if (members.hold_at(i).object == hold.object) break;
        if (members.kept_at(i).leader == kept.leader) ++index;
    }
    const float angle = l.angles[1] * 0.01745329252f;
    if (!t::bound(t::fn::pikmin_scatter_cos) || !t::bound(t::fn::pikmin_scatter_sin)) return false;
    const float c = t::fn::pikmin_scatter_cos(angle), s = t::fn::pikmin_scatter_sin(angle);
    const float back = 90.f + float(index / 5) * 55.f, side = (float(index % 5) - 2.f) * 50.f;
    const float dx = l.location[0] - c * back - s * side - a.location[0];
    const float dy = l.location[1] - s * back + c * side - a.location[1];
    const float distance = dx * dx + dy * dy;
    const float scale = distance > 360000.f ? 1.f : distance > 62500.f ? 0.8f : 0.6f;
    float v[3]{dx * scale, dy * scale, 0};
    if (distance < 400.f) v[0] = v[1] = 0;
    g::drop_velocity_requests(actor);
    g::request_velocity(actor, v);
    return true;
}
void restore_member(const Hold& hold, Member& kept, HoldEnd why) {
    if (why != HoldEnd::destroyed && why != HoldEnd::destroying) {
        if (const uint64_t ai = g::from_handle(kept.ai)) g::set_component_ticking(ai, kept.ticking);
        if (const uint64_t actor = g::from_handle(hold.object)) {
            if (kept.awake) g::hold_awake(actor, false);
            if (!kept.thrown) g::request_velocity(actor, kept.velocity);
        }
    }
    refresh();
    notice(hold.object, kept, false);
}

TSUCHI_Result pick(uint64_t leader, uint64_t node) {
    Column* col = column(leader, false);
    if (!col || !col->formation || !t::bound(t::fn::squad_select_next)) return TSUCHI_NOT_READY;
    bool found = false;
    for (const auto& candidate : col->nodes) if (candidate.self() == node && linked(col->formation, candidate)) found = true;
    if (!found && !g::readable(node, t::squad_node::count + sizeof(int32_t))) return TSUCHI_NOT_READY;
    const uint64_t sentinel = col->formation + t::squad_formation::list;
    const int32_t count = g::read_or<int32_t>(col->formation + t::squad_formation::count);
    if (count < 1 || count > 64) return TSUCHI_NOT_READY;
    for (int32_t i = 0; i < count; ++i) {
        if (g::read_or<uint64_t>(sentinel + t::squad_node::next) == node) return TSUCHI_OK;
        t::fn::squad_select_next(pointer(col->formation));
    }
    return TSUCHI_BUSY;
}
struct Selection { uint64_t before, formation; uint32_t slot; };
TSUCHI_Result apply_selection(const Hold& hold, Selection& kept, const uint32_t& slot, bool fresh) {
    refresh();
    Column* col = column(hold.object, false);
    if (!col || !linked(col->formation, col->nodes[slot])) return TSUCHI_NOT_READY;
    if (fresh) {
        kept.before = g::read_or<uint64_t>(col->formation + t::squad_formation::list + t::squad_node::next);
        kept.formation = col->formation_handle;
    }
    const TSUCHI_Result result = pick(hold.object, col->nodes[slot].self());
    if (result == TSUCHI_OK) kept.slot = slot;
    return result;
}
bool keep_selection(const Hold& hold, Selection& kept) {
    refresh();
    Column* col = column(hold.object, false);
    return col && types[kept.slot].live && linked(col->formation, col->nodes[kept.slot]) && pick(hold.object, col->nodes[kept.slot].self()) == TSUCHI_OK;
}
void restore_selection(const Hold& hold, Selection& kept, HoldEnd why) {
    if (why == HoldEnd::destroyed || why == HoldEnd::destroying) return;
    Column* col = column(hold.object, false);
    if (col && live_formation(*col) && col->formation_handle == kept.formation &&
        formation(hold.object) == col->formation) pick(hold.object, kept.before);
}
Holds<Selection, uint32_t, apply_selection, keep_selection, restore_selection> selections{"squad.selection", HoldOn::watched_actor, true};

void forget_type(void*, uint64_t key) {
    if (key >= kTypes) return;
    types[key].live = false;
    types[key].pending = false;
    // Other mods may hold members of this type. End them now, while the type's
    // ID still exists for its final membership event.
    for (uint32_t i = members.count(); i-- > 0;) if (members.kept_at(i).slot == key) {
        const Hold hold = members.hold_at(i);
        members.release(hold.mod, hold.object, 0);
    }
    for (uint32_t i = selections.count(); i-- > 0;) if (selections.kept_at(i).slot == key) {
        const Hold hold = selections.hold_at(i);
        selections.release(hold.mod, hold.object, 0);
    }
    refresh();
    types[key].name.clear();
    types[key].mod = nullptr;
}

void lend_colors() {
    static const uint8_t allowed[] = {0, 1, 2, 3, 4, 5, 6, 7, 9};
    for (auto& col : columns) {
        memset(col.borrowed, 0, sizeof col.borrowed);
        if (!live_formation(col)) continue;
        const uint64_t sentinel = col.formation + t::squad_formation::list;
        uint64_t at = g::read_or<uint64_t>(sentinel + t::squad_node::next);
        uint32_t used = 0;
        for (uint32_t step = 0; at && at != sentinel && step < 64; ++step) {
            Node* own = nullptr;
            for (auto& node : col.nodes) if (node.self() == at && node.owner()) own = &node;
            if (own) { at = own->next(); continue; }
            const int32_t kind = g::read_or<int32_t>(at + t::squad_node::kind, -1);
            if (kind >= 0 && kind < 16) used |= 1u << kind;
            at = g::read_or<uint64_t>(at + t::squad_node::next);
        }
        for (uint32_t slot = 0; slot < kTypes; ++slot) {
            if (!linked(col.formation, col.nodes[slot])) continue;
            for (uint8_t color : allowed) {
                if (used & (1u << color)) continue;
                const uint64_t framework = g::game_framework();
                const uint64_t ui = framework ? g::read_or<uint64_t>(framework + t::game_framework::ui) : 0;
                if (!ui || !t::bound(t::fn::throw_toggle_for_color) ||
                    g::pikmin_hud_toggle_reserved(t::fn::throw_toggle_for_color(pointer(ui), color))) continue;
                used |= 1u << color;
                col.borrowed[slot] = uint8_t(color + 1);
                col.nodes[slot].set_kind(color);
                break;
            }
        }
    }
}
int32_t shown_slot(uint8_t toggle, uint64_t fs, uint32_t& count) {
    const uint64_t framework = g::game_framework();
    const uint64_t ui = framework ? g::read_or<uint64_t>(framework + t::game_framework::ui) : 0;
    if (!ui || !t::bound(t::fn::throw_toggle_for_color)) return -1;
    for (const auto& col : columns) {
        if (!col.formation || (fs && fs != col.formation)) continue;
        for (uint32_t slot = 0; slot < kTypes; ++slot) {
            const uint8_t borrowed = col.borrowed[slot];
            if (!borrowed || !types[slot].live || !linked(col.formation, col.nodes[slot])) continue;
            if (t::fn::throw_toggle_for_color(pointer(ui), uint8_t(borrowed - 1)) != toggle) continue;
            count = uint32_t(col.nodes[slot].count());
            return int32_t(slot);
        }
    }
    return -1;
}
void paint(uint64_t material, uint64_t atlases, int32_t slot, const g::CallGuard& guard = {}) {
    if (!material || !t::bound(t::fn::icon_texture) || !t::bound(t::fn::icon_index) || !t::bound(t::fn::content_icon_vector)) return;
    const uint64_t texture = slot >= 0 ? g::from_handle(types[slot].texture) : 0;
    const uint64_t image = texture ? texture : g::read_or<uint64_t>(atlases);
    if (image) t::fn::icon_texture(pointer(material), pointer(image));
    if (!guard.permits()) return;
    t::fn::icon_index(pointer(material), texture ? 0 : 10);
    if (!guard.permits()) return;
    const uint64_t name = g::global<uint64_t>(t::data::content_icon_size);
    const float side = texture ? 1.f : 0.25f;
    if (name) t::fn::content_icon_vector(pointer(material), name, side, side, 0.f, 1.f);
}
// An icon can slide to a stock type while its widget stays alive. Its native
// material and size are handed back before another type takes the icon.
struct Painted {
    uint64_t material, atlases;
    uint32_t slot;
    uint8_t toggle, color;
    bool cursor;
};
TSUCHI_Result paint_icon(const Hold& hold, Painted& kept, const Painted& wanted, bool) {
    if (!types[wanted.slot].live) return TSUCHI_NOT_FOUND;
    // A native widget can move to another borrowed cell without changing the
    // squad type. Restore the cell it shows now, not the previous toggle.
    kept = wanted;
    hold_apply_checkpoint(hold);
    const uint64_t material = g::from_handle(kept.material);
    if (!material) return TSUCHI_NOT_FOUND;
    paint(material, kept.atlases, int32_t(kept.slot), hold_call_guard(hold));
    return hold_call_guard(hold).permits() ? TSUCHI_OK : TSUCHI_NOT_READY;
}
bool keep_icon(const Hold& hold, Painted& kept) {
    const uint64_t icon = g::from_handle(hold.object);
    const uint64_t field = kept.cursor ? t::cursor_toggle_icon::toggle : t::squad_icon::toggle;
    uint32_t count = 0;
    const uint64_t material_field = kept.cursor ? t::cursor_toggle_icon::material : t::squad_icon::material;
    return types[kept.slot].live && icon && g::from_handle(kept.material) == g::read_or<uint64_t>(icon + material_field) &&
        g::read_or<uint8_t>(icon + field) == kept.toggle &&
        shown_slot(kept.toggle, 0, count) == int32_t(kept.slot);
}
void unpaint_icon(const Hold& hold, Painted& kept, HoldEnd why) {
    if (why == HoldEnd::destroyed || why == HoldEnd::destroying || !g::from_handle(hold.object)) return;
    if (!t::bound(t::fn::icon_texture) || !t::bound(t::fn::icon_index) || !t::bound(t::fn::content_icon_vector)) return;
    const uint64_t material = g::from_handle(kept.material);
    const uint64_t icon = g::from_handle(hold.object);
    const uint64_t toggle = kept.cursor ? t::cursor_toggle_icon::toggle : t::squad_icon::toggle;
    const uint64_t slot = kept.cursor ? t::cursor_toggle_icon::material : t::squad_icon::material;
    // A different type has already repainted this recycled widget. Its owner
    // must keep both the new texture and its UV scale.
    if (g::pikmin_hud_icon_owned(hold.object) || !material || !icon || g::read_or<uint8_t>(icon + toggle) != kept.toggle ||
        g::read_or<uint64_t>(icon + slot) != material) return;
    const uint64_t image = g::read_or<uint64_t>(kept.atlases);
    if (image) t::fn::icon_texture(pointer(material), pointer(image));
    t::fn::icon_index(pointer(material), kept.color);
    const uint64_t name = g::global<uint64_t>(t::data::content_icon_size);
    if (name) t::fn::content_icon_vector(pointer(material), name, 0.25f, 0.25f, 0.f, 1.f);
    if (!kept.cursor) {
        const uint64_t icon = g::from_handle(hold.object);
        g::write(icon + t::squad_icon::kind, kept.color);
        g::write(icon + t::squad_icon::kind + 1, kept.color);
        g::write(icon + t::squad_icon::count, int32_t(-1));
    }
}
Holds<Painted, Painted, paint_icon, keep_icon, unpaint_icon> painted{"squad.icon", HoldOn::object, true};
bool owns_icon(uint64_t icon) {
    for (uint32_t i=0;i<painted.count();++i)
        if (painted.hold_at(i).object == icon) return keep_icon(painted.hold_at(i), painted.kept_at(i));
    return false;
}
void show_icon(uint64_t icon, uint64_t material, uint64_t atlases, uint8_t toggle, int32_t slot, bool cursor) {
    const uint64_t handle = g::to_handle(icon);
    const uint64_t material_handle = g::to_handle(material);
    if (!handle || !material_handle) return;
    const Painted* prior = painted.find(handle, 0);
    if (prior && (slot < 0 || prior->slot != uint32_t(slot))) {
        const Hold owned = [&]() {
            for (uint32_t i = 0; i < painted.count(); ++i) if (painted.hold_at(i).object == handle) return painted.hold_at(i);
            return Hold{};
        }();
        painted.release(owned.mod, handle, 0);
    }
    if (slot < 0) return;
    const uint8_t color = toggle > 0 ? uint8_t(toggle - 1) : 0;
    painted.take(types[slot].mod, handle, 0, Painted{material_handle, atlases, uint32_t(slot), toggle, color, cursor});
}
void hud_update(void* widget) {
    if (!on_game_thread() || !ready) { hud_before(widget); return; }
    refresh();
    lend_colors();
    hud_before(widget);
    for (auto& col : columns) for (auto& node : col.nodes) if (node.owner()) node.set_kind(int32_t(t::constant::squad_other_kind));
    const uint64_t hud = reinterpret_cast<uint64_t>(widget);
    const uint64_t player = g::read_or<uint64_t>(hud + t::squad_hud::player);
    const uint64_t fs = player ? formation(g::to_handle(player)) : 0;
    const uint64_t icons = g::read_or<uint64_t>(hud + t::squad_hud::icons);
    const int32_t total = g::read_or<int32_t>(hud + t::squad_hud::count);
    for (int32_t i = 0; icons && i < total && i < 16; ++i) {
        const uint64_t icon = g::read_or<uint64_t>(icons + uint64_t(i) * 8);
        uint32_t count = 0;
        const int32_t slot = icon ? shown_slot(g::read_or<uint8_t>(icon + t::squad_icon::toggle), fs, count) : -1;
        const uint64_t material = g::read_or<uint64_t>(icon + t::squad_icon::material);
        show_icon(icon, material, g::read_or<uint64_t>(icon + t::squad_icon::textures),
                  g::read_or<uint8_t>(icon + t::squad_icon::toggle), slot, false);
        if (slot < 0 || !types[slot].live) continue;
        g::write(icon + t::squad_icon::kind, uint8_t(t::constant::squad_other_kind));
        g::write(icon + t::squad_icon::kind + 1, uint8_t(t::constant::squad_other_kind));
        if (t::bound(t::fn::squad_icon_count)) t::fn::squad_icon_count(pointer(icon), int32_t(count));
    }
}
void cursor_update(void* widget, void* geometry, float seconds) {
    cursor_before(widget, geometry, seconds);
    if (!on_game_thread() || !ready) return;
    const uint64_t cursor = reinterpret_cast<uint64_t>(widget);
    const uint64_t icons = g::read_or<uint64_t>(cursor + t::cursor_toggle::icons);
    const int32_t total = g::read_or<int32_t>(cursor + t::cursor_toggle::icon_count);
    for (int32_t i = 0; icons && i < total && i < 8; ++i) {
        const uint64_t icon = g::read_or<uint64_t>(icons + uint64_t(i) * 8);
        uint32_t count = 0;
        const int32_t slot = icon ? shown_slot(g::read_or<uint8_t>(icon + t::cursor_toggle_icon::toggle), 0, count) : -1;
        show_icon(icon, g::read_or<uint64_t>(icon + t::cursor_toggle_icon::material), g::read_or<uint64_t>(icon + t::cursor_toggle_icon::textures),
                  g::read_or<uint8_t>(icon + t::cursor_toggle_icon::toggle), slot, true);
        if (slot < 0 || !types[slot].live) continue;
        if (t::bound(t::fn::cursor_icon_count)) t::fn::cursor_icon_count(pointer(icon), int32_t(count), -1);
    }
}
template <uint32_t Which> uint64_t grab(void* component, uint64_t argument) {
    if (!on_game_thread() || !ready) return grab_before[Which](component, argument);
    const uint64_t part = reinterpret_cast<uint64_t>(component);
    const uint64_t actor = g::read_or<uint64_t>(part + t::squad_component::owner);
    const uint64_t leader = actor ? g::to_handle(actor) : 0;
    Column* col = column(leader, false);
    if (!col || !live_formation(*col) || !t::bound(t::fn::start_throw)) return grab_before[Which](component, argument);
    const uint64_t selected = g::read_or<uint64_t>(col->formation + t::squad_formation::list + t::squad_node::next);
    int32_t slot = -1;
    for (uint32_t i = 0; i < kTypes; ++i) if (col->nodes[i].self() == selected && col->nodes[i].owner()) slot = int32_t(i);
    if (slot < 0) return grab_before[Which](component, argument);
    g::Transform at;
    if (!g::get_transform(actor, at)) return 0;
    Member* best = nullptr;
    uint64_t picked = 0;
    float nearest = 1.e30f;
    for (uint32_t i = 0; i < members.count(); ++i) {
        auto& kept = members.kept_at(i);
        if (kept.slot != uint32_t(slot) || kept.leader != leader) continue;
        const uint64_t member = g::from_handle(members.hold_at(i).object);
        g::Transform here;
        if (!member || !g::can_launch(member) || !g::get_transform(member, here)) continue;
        const float x = at.location[0] - here.location[0], y = at.location[1] - here.location[1];
        const float distance = x * x + y * y;
        if (distance < nearest) nearest = distance, best = &kept, picked = member;
    }
    if (best) {
        best->held = true;
        t::fn::start_throw(component, pointer(picked), nullptr);
    }
    return 0;
}

TSUCHI_Result held_member(TSUCHI_Mod* mod, const char* kind, TSUCHI_Actor leader, TSUCHI_Actor member) {
    uint32_t slot = 0;
    const TSUCHI_Result result = slot_of(kind, slot);
    return result == TSUCHI_OK ? members.take(mod, member.id, 0, Wanted{leader.id, slot}) : result;
}
TSUCHI_Result released_member(TSUCHI_Mod* mod, TSUCHI_Actor member) { return members.release(mod, member.id, 0); }
TSUCHI_Result type_members(const char* kind, TSUCHI_Actor leader, TSUCHI_Actor* buffer, uint64_t capacity, uint64_t* out) {
    uint32_t slot = 0;
    const TSUCHI_Result result = slot_of(kind, slot);
    if (result != TSUCHI_OK) return result;
    const uint64_t actor = g::from_handle(leader.id);
    if (!actor || !g::is_leader(actor)) return TSUCHI_NOT_FOUND;
    uint64_t count = 0;
    for (uint32_t i = 0; i < members.count(); ++i) if (members.kept_at(i).slot == slot && members.kept_at(i).leader == leader.id) ++count;
    *out = count;
    if (count > capacity) return TSUCHI_CAPACITY;
    count = 0;
    for (uint32_t i = 0; i < members.count(); ++i)
        if (members.kept_at(i).slot == slot && members.kept_at(i).leader == leader.id) buffer[count++].id = members.hold_at(i).object;
    return TSUCHI_OK;
}
TSUCHI_Result member_type(TSUCHI_Actor actor, TSUCHI_SquadMember* out) {
    const Member* kept = members.find(actor.id, 0);
    if (!kept) return TSUCHI_NOT_FOUND;
    *out = TSUCHI_SquadMember{};
    memcpy(out->kind, types[kept->slot].id, strlen(types[kept->slot].id) + 1);
    out->leader.id = kept->leader;
    return TSUCHI_OK;
}
TSUCHI_Result selected(TSUCHI_Actor leader, char* buffer, uint64_t capacity, uint64_t* out) {
    const uint64_t actor = g::from_handle(leader.id);
    if (!actor || !g::is_leader(actor)) return TSUCHI_NOT_FOUND;
    const uint64_t fs = formation(leader.id);
    if (!fs) return TSUCHI_NOT_READY;
    const uint64_t first = g::read_or<uint64_t>(fs + t::squad_formation::list + t::squad_node::next);
    const char* result = "";
    if (Column* col = column(leader.id, false)) for (uint32_t slot = 0; slot < kTypes; ++slot)
        if (types[slot].live && col->nodes[slot].self() == first && col->nodes[slot].owner()) result = types[slot].id;
    *out = strlen(result) + 1;
    if (capacity < *out) return TSUCHI_CAPACITY;
    memcpy(buffer, result, size_t(*out));
    return TSUCHI_OK;
}
TSUCHI_Result held_selection(TSUCHI_Mod* mod, TSUCHI_Actor leader, const char* kind) {
    uint32_t slot = 0;
    const TSUCHI_Result result = slot_of(kind, slot);
    if (result != TSUCHI_OK) return result;
    const uint64_t actor = g::from_handle(leader.id);
    if (!actor || !g::is_leader(actor)) return TSUCHI_NOT_FOUND;
    return selections.take(mod, leader.id, 0, slot);
}
TSUCHI_Result released_selection(TSUCHI_Mod* mod, TSUCHI_Actor leader) { return selections.release(mod, leader.id, 0); }
TSUCHI_Result type_name(const char* kind, char* buffer, uint64_t capacity, uint64_t* out) {
    uint32_t slot = 0;
    const TSUCHI_Result result = slot_of(kind, slot);
    if (result != TSUCHI_OK) return result;
    const char* name = types[slot].name.get();
    *out = strlen(name) + 1;
    if (capacity < *out) return TSUCHI_CAPACITY;
    memcpy(buffer, name, size_t(*out));
    return TSUCHI_OK;
}
} // namespace

bool content_squad_valid(const Json& content, Text& problem) {
    const Json* all = content.find("squad");
    if (!all) return true;
    if (all->kind != Json::Kind::object || all->count > kTypes) return wrong(problem, "content.squad must hold at most eight types");
    for (const Json* value = all->first; value; value = value->next) {
        if (!valid_mod_id(value->name) || value->kind != Json::Kind::object || !text(value->find("display_name")))
            return wrong(problem, "a squad type needs a short name and display_name");
        const Json* icon = value->find("icon");
        if (icon && (!text(icon) || icon->bytes < 7 || memcmp(icon->text, "/Game/", 6)))
            return wrong(problem, "a squad icon must name a cooked /Game/ texture");
        for (const Json* field = value->first; field; field = field->next)
            if (strcmp(field->name, "display_name") && strcmp(field->name, "icon")) return wrong(problem, "unknown squad type field");
    }
    return true;
}
TSUCHI_Result content_squad_begin(TSUCHI_Mod* mod, const Json& content, Text& problem) {
    if (!mod_usable(mod)) return TSUCHI_INVALID;
    if (!content_squad_valid(content, problem)) return TSUCHI_INVALID;
    const Json* all = content.find("squad");
    if (!all) return TSUCHI_OK;
    if (!ready) return TSUCHI_NOT_READY;
    // Admit a batch before native lists can see any of it. If loading an icon
    // or recording an owner fails, forget the declarations from this call.
    const uint64_t owned_before = ownership_mark(mod);
    uint32_t admitted[kTypes]{}, admitted_count = 0;
    bool fresh_number[kTypes]{};
    const Json* named[kTypes]{};
    auto fail = [&](TSUCHI_Result result) {
        for (uint32_t i = admitted_count; i-- > 0;) {
            const uint32_t slot = admitted[i];
            disown(mod, kType, slot);
            forget_type(nullptr, slot);
            id_remove(vocabulary, mod, named[i]->name);
            if (fresh_number[i]) {
                char id[kIdBytes];
                id_join(mod_id(mod), named[i]->name, id);
                for (uint32_t row = vocabulary.given.count; row-- > 0;)
                    if (vocabulary.given[row].value == slot && !strcmp(vocabulary.given[row].id, id)) {
                        vocabulary.given.remove(&vocabulary.given[row]);
                        break;
                    }
            }
        }
        // Pending types are invisible to live calls, so their new numbers have
        // no native nodes to retire. Foreign callback allocations stay intact.
        release_owned_between(mod, owned_before, ownership_mark(mod));
        return result;
    };
    const TSUCHI_Result room = reserve_owned(mod, all->count * 2);
    if (room != TSUCHI_OK) return room;
    for (const Json* value = all->first; value; value = value->next) {
        char id[kIdBytes];
        id_join(mod_id(mod), value->name, id);
        bool existing_number = false;
        for (uint32_t row = 0; row < vocabulary.given.count; ++row)
            if (!strcmp(vocabulary.given[row].id, id)) existing_number = true;
        uint32_t slot = 0;
        const TSUCHI_Result result = id_allocate(vocabulary, mod, value->name, 0, kTypes - 1, &slot);
        if (result != TSUCHI_OK) return fail(result);
        if (types[slot].live || types[slot].pending) return fail(TSUCHI_BUSY);
        admitted[admitted_count] = slot, fresh_number[admitted_count] = !existing_number;
        named[admitted_count++] = value;
        Type entry{}; entry.mod = mod; id_join(mod_id(mod), value->name, entry.id);
        if (!entry.name.set(value->find("display_name")->text)) return fail(TSUCHI_NO_MEMORY);
        entry.pending = true;
        types[slot].name.clear();
        types[slot] = entry;
        if (const Json* icon = value->find("icon")) {
            TSUCHI_Object texture{};
            const TSUCHI_Result loaded = asset::table.load(mod, icon->text, &texture);
            if (loaded != TSUCHI_OK) return fail(loaded);
            static g::ClassRef texture_class{t::name::content_texture_class};
            const uint64_t object = g::from_handle(texture.id);
            if (!object || !g::is_a(object, texture_class)) {
                return fail(object ? TSUCHI_INVALID : TSUCHI_NOT_FOUND);
            }
            types[slot].texture = texture.id;
        }
        const TSUCHI_Result owned = own(mod, kType, slot, forget_type, nullptr);
        if (owned != TSUCHI_OK) return fail(owned);
    }
    for (uint32_t i = 0; i < admitted_count; ++i) {
        types[admitted[i]].pending = false;
        types[admitted[i]].live = true;
    }
    return TSUCHI_OK;
}
void content_squad_start(TSUCHI_Mod* self) {
    members.start(), selections.start(), painted.start();
    g::pikmin_hud_icon_guard(owns_icon);
    each_frame(refresh);
    uint64_t id = 0;
    TSUCHI_Result result = TSUCHI_OK;
    if (!hud_before) result = hook_slot(self, t::slot::throw_list_update, &hud_update, &hud_before, 0, &id);
    if (result == TSUCHI_OK && !cursor_before) result = hook_slot(self, t::slot::cursor_toggle_tick, &cursor_update, &cursor_before, 0, &id);
    if (result == TSUCHI_OK && !grab_before[0]) result = hook_slot(self, t::slot::squad_grab_avatar, &grab<0>, &grab_before[0], 0, &id);
    if (result == TSUCHI_OK && !grab_before[1]) result = hook_slot(self, t::slot::squad_grab_orima, &grab<1>, &grab_before[1], 0, &id);
    ready = result == TSUCHI_OK && hud_before && cursor_before && grab_before[0] && grab_before[1];
}
void squad_content_backend(squad::Backend& backend) {
    backend.hold_member = held_member, backend.release_member = released_member;
    backend.type_members = type_members, backend.member_type = member_type;
    backend.selected = selected;
    backend.hold_selection = held_selection, backend.release_selection = released_selection;
    backend.type_name = type_name;
}
} // namespace tsuchi
