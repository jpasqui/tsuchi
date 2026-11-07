// Declared squad calls and native formation edits against a made-up game.
// These checks are host evidence. They are not a game run.
#include <array>
#include <cassert>
#include <cmath>
#include <cstring>
#include <vector>
#include "content_squad.h"
#include "actor_events.h"
#include "actors.h"
#include "asset_service.h"
#include "frame.h"
#include "game.h"
#include "hooks.h"
#include "ids.h"
#include "ordinary_lifecycle.h"
#include "objects.h"
#include "parts.h"
#include "pikmin.h"
#include "registry.h"

namespace t = tsuchi::game::table;
namespace {
struct Body {
    std::array<uint8_t, 0x2300> bytes{}, ai{}, methods{}, formation{};
    std::array<uint8_t, 0x48> stock{};
    bool leader = false, alive = true, ticking = true, launchable = true;
    uint64_t formation_serial = 0;
    float velocity[3]{3, 4, 5};
    int awake = 0;
    uint64_t at() { return reinterpret_cast<uint64_t>(bytes.data()); }
    uint64_t brain() { return reinterpret_cast<uint64_t>(ai.data()); }
    uint64_t fs() { return reinterpret_cast<uint64_t>(formation.data()); }
};
Body captain, rival, member;
std::array<uint8_t, 0x1000> framework{}, ui{}, hud{}, icon{}, material{}, atlas{};
std::array<uint8_t, 0x1000> wrong_texture{};
std::array<uint8_t, 0xe010> globals{};
uint64_t texture = reinterpret_cast<uint64_t>(atlas.data());
std::vector<void (*)()> steps;
struct Loaded { TSUCHI_Mod* mod; uint64_t id; };
std::vector<Loaded> loaded_assets;
const tsuchi::Kind kLoaded{"asset.test"};
void (*during_load)() = nullptr;
TSUCHI_Mod* callback_mod = nullptr;
TSUCHI_Mod* foreign_mod = nullptr;
const tsuchi::Json* foreign_content = nullptr;
uint32_t foreign_slot = 99;
void (*left)(uint64_t, uint64_t, tsuchi::ActorLeft) = nullptr;
TSUCHI_Mod* stop_on_tick = nullptr;
void (*hud_hook)(void*) = nullptr;
void (*cursor_hook)(void*, void*, float) = nullptr;
uint64_t (*grab_hook[2])(void*, uint64_t){};
uint64_t thread = 1;
unsigned stale_formation_accesses = 0;
uint8_t reserved_toggle = 0;
int joins = 0, leaves = 0, stock_grabs = 0, launches = 0, solutions = 0, freed_paths = 0;
std::array<uint8_t, 16> native_path{};
template <typename T> void put(uint64_t at, T value) { memcpy(reinterpret_cast<void*>(at), &value, sizeof value); }
template <typename T> T get(uint64_t at) { T value{}; memcpy(&value, reinterpret_cast<void*>(at), sizeof value); return value; }
Body* body(uint64_t at) {
    for (Body* object : {&captain, &rival, &member})
        if (object->alive && (at == object->at() || at == object->brain() || at == object->fs())) return object;
    return nullptr;
}
uint64_t ask_formation(void* part) { auto* object = body(reinterpret_cast<uint64_t>(part)); return object ? object->fs() : 0; }
void rotate(void* formation) {
    const uint64_t fs = reinterpret_cast<uint64_t>(formation), sentinel = fs + t::squad_formation::list;
    const uint64_t first = get<uint64_t>(sentinel + 8), last = get<uint64_t>(sentinel);
    if (!first || first == sentinel || first == last) return;
    const uint64_t next = get<uint64_t>(first + 8);
    put(next, sentinel), put(sentinel + 8, next);
    put(last + 8, first), put(first, last), put(first + 8, sentinel), put(sentinel, first);
}
void initialize(Body& object, bool leader) {
    object.leader = leader;
    const uint64_t methods = reinterpret_cast<uint64_t>(object.methods.data());
    put(object.brain(), methods);
    put(methods + t::avatar_table::squad, &ask_formation);
    put(object.brain() + t::squad_component::owner, object.at());
    const uint64_t sentinel = object.fs() + t::squad_formation::list, stock = reinterpret_cast<uint64_t>(object.stock.data());
    put(sentinel, stock), put(sentinel + 8, stock);
    put(stock, sentinel), put(stock + 8, sentinel), put(stock + t::squad_node::kind, int32_t(0));
    put(object.fs() + t::squad_formation::count, int32_t(1));
}
void stock_hud(void*) {}
void stock_cursor(void*, void*, float) {}
uint64_t stock_grab(void*, uint64_t) { ++stock_grabs; return 17; }
void start_throw(void*, void* actor, void*) { auto* object = body(reinterpret_cast<uint64_t>(actor)); assert(object); object->launchable = false; }
uint8_t toggle(void*, uint8_t kind) { return uint8_t(kind + 1); }
void image(void* target, void* source) { put(reinterpret_cast<uint64_t>(target), reinterpret_cast<uint64_t>(source)); }
void index(void* target, int32_t value) { put(reinterpret_cast<uint64_t>(target) + 8, value); }
void size(void* target, uint64_t name, float x, float y, float z, float w) {
    assert(name == 77 && x == y && z == 0 && w == 1);
    put(reinterpret_cast<uint64_t>(target) + 12, x);
}
void count(void* target, int32_t value) { put(reinterpret_cast<uint64_t>(target) + t::squad_icon::count, value); }
void cursor_count(void*, int32_t, int32_t) {}
uint32_t solve(void* block) {
    auto* bytes = static_cast<uint8_t*>(block);
    namespace s = t::squad_throw_solution;
    assert(get<float>(reinterpret_cast<uint64_t>(bytes + s::start) + 8) == 30.f);
    assert(get<float>(reinterpret_cast<uint64_t>(bytes + s::apex)) == 120.f);
    assert(get<float>(reinterpret_cast<uint64_t>(bytes + s::length_max)) == 1000.f);
    assert(get<float>(reinterpret_cast<uint64_t>(bytes + s::gravity)) == 980.f);
    const float velocity[3]{210, 110, 510};
    memcpy(bytes + s::velocity, velocity, sizeof velocity);
    put(reinterpret_cast<uint64_t>(bytes + s::path), reinterpret_cast<uint64_t>(native_path.data()));
    ++solutions;
    return 1;
}
void free_loaded(void* context, uint64_t id) {
    for (auto it = loaded_assets.begin(); it != loaded_assets.end(); ++it)
        if (it->mod == context && it->id == id) { loaded_assets.erase(it); return; }
}
TSUCHI_Result load(TSUCHI_Mod* mod, const char* path, TSUCHI_Object* out) {
    if (!strcmp(path, "/Game/Missing.Texture")) return TSUCHI_NOT_FOUND;
    if (during_load) { auto callback = during_load; during_load = nullptr; callback(); }
    out->id = !strcmp(path, "/Game/Wrong.Class") ? reinterpret_cast<uint64_t>(wrong_texture.data()) : texture;
    for (const auto& loaded : loaded_assets) if (loaded.mod == mod && loaded.id == out->id) return TSUCHI_OK;
    const auto result = tsuchi::own(mod, kLoaded, out->id, free_loaded, mod);
    if (result == TSUCHI_OK) loaded_assets.push_back({mod, out->id});
    return result;
}
TSUCHI_Result release(TSUCHI_Mod* mod, TSUCHI_Object object) {
    const auto result = tsuchi::disown(mod, kLoaded, object.id);
    if (result == TSUCHI_OK) free_loaded(mod, object.id);
    return result;
}
void notice(void*, const TSUCHI_SquadMembershipChanged* event) {
    assert(!strcmp(event->kind, "test.squad:groink") || !strcmp(event->kind, "test.squad:guard"));
    if (event->joined) ++joins; else ++leaves;
}
void step() { for (auto call : steps) call(); }
const tsuchi::Json* declaration(const char* source) { return tsuchi::read_json(source, strlen(source)); }
void foreign_admission() {
    char name[96]; uint64_t bytes = 0;
    const auto& api = tsuchi::squad::table;
    assert(api.type_name(callback_mod, "test.callback:first", name, sizeof name, &bytes) == TSUCHI_NOT_FOUND);
    assert(api.hold_member(foreign_mod, "test.callback:first", {captain.at()}, {member.at()}) == TSUCHI_NOT_FOUND);
    tsuchi::Text problem;
    const auto* overlap = declaration("{\"squad\":{\"first\":{\"display_name\":\"Overlap\"}}}");
    assert(tsuchi::content_squad_begin(callback_mod, *overlap, problem) == TSUCHI_BUSY);
    tsuchi::release_json(overlap);
    assert(tsuchi::content_squad_begin(foreign_mod, *foreign_content, problem) == TSUCHI_OK);
    for (uint32_t slot = 0; slot < 8; ++slot) {
        char id[tsuchi::kIdBytes]; tsuchi::id_placeholder(slot, id);
        if (api.type_name(foreign_mod, id, name, sizeof name, &bytes) == TSUCHI_OK && !strcmp(name, "Visitor")) foreign_slot = slot;
    }
    assert(foreign_slot < 8);
    problem.clear();
}
}
namespace tsuchi::game {
uint64_t main_base() { return reinterpret_cast<uint64_t>(globals.data()) - t::data::pikmin_scatter_config; }
bool readable(uint64_t at, uint64_t) {
    for (Body* entry : {&captain,&rival,&member})
        if (!entry->alive && at>=entry->fs() && at<entry->fs()+entry->formation.size()) { ++stale_formation_accesses;return false; }
    return at > 0xFFFFFFFFull;
}
bool writable(uint64_t at, uint64_t bytes) { return readable(at, bytes); }
bool executable(uint64_t at) { return at == reinterpret_cast<uint64_t>(&ask_formation); }
bool pikmin_hud_toggle_reserved(uint8_t toggle) { return toggle == reserved_toggle; }
bool pikmin_hud_icon_owned(uint64_t) { return false; }
void pikmin_hud_icon_guard(bool (*)(uint64_t)) {}
bool live(uint64_t at) {
    return body(at) || at == texture || at == reinterpret_cast<uint64_t>(wrong_texture.data()) ||
        at == reinterpret_cast<uint64_t>(icon.data()) || at == reinterpret_cast<uint64_t>(material.data());
}
bool is_a(uint64_t object, ClassRef& kind) { assert(!strcmp(kind.name, "Texture2D")); return object == texture; }
uint64_t to_handle(uint64_t at) {
    for (Body* entry : {&captain,&rival,&member}) if (at==entry->fs()) return entry->alive?at+entry->formation_serial:0;
    return live(at)?at:0;
}
uint64_t from_handle(uint64_t handle) {
    for (Body* entry : {&captain,&rival,&member})
        if (handle>=entry->fs() && handle<entry->fs()+32)
            return entry->alive && handle==entry->fs()+entry->formation_serial?entry->fs():0;
    return live(handle)?handle:0;
}
uint64_t game_framework() { return reinterpret_cast<uint64_t>(framework.data()); }
bool is_game_actor(uint64_t actor) { return body(actor) && actor == body(actor)->at(); }
bool is_leader(uint64_t actor) { return body(actor) && body(actor)->leader; }
bool is_pikmin(uint64_t) { return false; }
void each_pikmin(bool (*)(void*, uint64_t), void*) {}
void release_engine_memory(void* memory) { assert(memory == native_path.data()); ++freed_paths; }
uint64_t ai_component(uint64_t actor) { auto* object = body(actor); return object ? object->brain() : 0; }
uint64_t part_owner(uint64_t part) { auto* object = body(part); return object ? object->at() : 0; }
bool can_launch(uint64_t actor) { return body(actor) && body(actor)->launchable; }
bool component_ticking(uint64_t part, bool& on) { auto* object = body(part); if (!object) return false; on = object->ticking; return true; }
void set_component_ticking(uint64_t part, bool on) {
    if (auto* object = body(part)) object->ticking = on;
    if (!on && stop_on_tick) { auto* mod = stop_on_tick; stop_on_tick = nullptr; mod_stop(mod); }
}
bool get_velocity(uint64_t actor, float out[3]) { auto* object = body(actor); if (!object) return false; memcpy(out, object->velocity, 12); return true; }
void request_velocity(uint64_t actor, const float value[3]) { if (auto* object = body(actor)) memcpy(object->velocity, value, 12); }
void hold_awake(uint64_t actor, bool held) { if (auto* object = body(actor)) object->awake += held ? 1 : -1; }
void drop_velocity_requests(uint64_t) {}
bool get_transform(uint64_t actor, Transform& out) {
    if (!body(actor)) return false;
    out = Transform{};
    if (actor == member.at()) out.location[0] = 30;
    return true;
}
bool read_cursor(uint64_t actor, float out[3]) { if (!body(actor)) return false; out[0] = 200, out[1] = 100, out[2] = 0; return true; }
}
namespace tsuchi {
bool each_frame(void (*call)()) { steps.push_back(call); return true; }
bool on_actor_left(void (*call)(uint64_t, uint64_t, ActorLeft)) { left = call; return true; }
void ordinary_lifecycle_listener(void (*)(uint64_t, uint64_t, bool)) {}
TSUCHI_Result ordinary_lifecycle_watch(uint64_t) { return TSUCHI_OK; }
uint64_t ordinary_lifecycle_restore_part(uint64_t handle, uint64_t) { return game::from_handle(handle); }
void motion_begin(uint64_t, const float*) { ++launches; }
TSUCHI_Result hook_wrap(TSUCHI_Mod*, uint64_t entry, uint64_t, void* function, uint32_t flags, uint64_t* id, void** before) {
    assert(flags == 0); // a pre-existing foreign hook stays in the chain
    *id = entry;
    if (entry == game::main_base() + t::slot::throw_list_update.table + t::slot::throw_list_update.offset)
        hud_hook = reinterpret_cast<decltype(hud_hook)>(function), *before = reinterpret_cast<void*>(&stock_hud);
    else if (entry == game::main_base() + t::slot::cursor_toggle_tick.table + t::slot::cursor_toggle_tick.offset)
        cursor_hook = reinterpret_cast<decltype(cursor_hook)>(function), *before = reinterpret_cast<void*>(&stock_cursor);
    else {
        const unsigned which = entry == game::main_base() + t::slot::squad_grab_avatar.table + t::slot::squad_grab_avatar.offset ? 0 : 1;
        grab_hook[which] = reinterpret_cast<decltype(grab_hook[0])>(function), *before = reinterpret_cast<void*>(&stock_grab);
    }
    return TSUCHI_OK;
}
}
int main() {
    initialize(captain, true), initialize(rival, true), initialize(member, false);
    put(tsuchi::game::main_base() + t::data::content_icon_size, uint64_t(77));
    t::fn::squad_select_next = rotate, t::fn::start_throw = start_throw;
    t::fn::squad_throw_trajectory = solve;
    t::fn::pikmin_scatter_cos = std::cos, t::fn::pikmin_scatter_sin = std::sin;
    t::fn::throw_toggle_for_color = toggle, t::fn::icon_texture = image, t::fn::icon_index = index;
    t::fn::content_icon_vector = size, t::fn::squad_icon_count = count, t::fn::cursor_icon_count = cursor_count;
    tsuchi::asset::Backend assets{}; assets.load=load; assets.release=release; tsuchi::asset::set_backend(&assets);
    tsuchi::squad::Backend backend{}; tsuchi::squad_content_backend(backend); tsuchi::squad::set_backend(&backend);
    auto* self = tsuchi::mod_create("tsuchi"); tsuchi::content_squad_start(self);
    auto* mod = tsuchi::mod_create("test.squad"), *other = tsuchi::mod_create("test.other");
    const char* source = "{\"squad\":{\"groink\":{\"display_name\":\"Gr\\u00f6inks\",\"icon\":\"/Game/Test.Icon\"},\"guard\":{\"display_name\":\"Guards\"}}}";
    const auto* content = declaration(source);
    tsuchi::Text problem;
    assert(content && tsuchi::content_squad_valid(*content, problem));
    assert(tsuchi::content_squad_begin(mod, *content, problem) == TSUCHI_OK);
    tsuchi::release_json(content); content = nullptr;
    const auto* failed = declaration("{\"squad\":{\"first\":{\"display_name\":\"First\",\"icon\":\"/Game/Test.Icon\"},\"missing\":{\"display_name\":\"Missing\",\"icon\":\"/Game/Missing.Texture\"}}}");
    const uint32_t owned_before = tsuchi::owned_count(other);
    assert(tsuchi::content_squad_begin(other, *failed, problem) == TSUCHI_NOT_FOUND);
    assert(tsuchi::owned_count(other) == owned_before);
    assert(loaded_assets.size() == 1 && loaded_assets[0].mod == mod);
    const auto* wrong_icon = declaration("{\"squad\":{\"wrong\":{\"display_name\":\"Wrong\",\"icon\":\"/Game/Wrong.Class\"}}}");
    assert(tsuchi::content_squad_begin(other, *wrong_icon, problem) == TSUCHI_INVALID);
    assert(tsuchi::owned_count(other) == owned_before && loaded_assets.size() == 1);
    callback_mod = tsuchi::mod_create("test.callback"), foreign_mod = tsuchi::mod_create("test.foreign");
    foreign_content = declaration("{\"squad\":{\"visitor\":{\"display_name\":\"Visitor\"}}}");
    during_load = foreign_admission;
    assert(tsuchi::content_squad_begin(callback_mod, *failed, problem) == TSUCHI_NOT_FOUND);
    assert(tsuchi::owned_count(callback_mod) == 0 && loaded_assets.size() == 1);
    char foreign_id[tsuchi::kIdBytes]; tsuchi::id_placeholder(foreign_slot, foreign_id);
    char foreign_name[96]; uint64_t foreign_bytes = 0;
    assert(tsuchi::squad::table.type_name(foreign_mod, foreign_id, foreign_name, sizeof foreign_name, &foreign_bytes) == TSUCHI_OK);
    assert(!strcmp(foreign_name, "Visitor"));
    tsuchi::mod_stop(foreign_mod);
    auto* after_mod = tsuchi::mod_create("test.after");
    const auto* after_content = declaration("{\"squad\":{\"alpha\":{\"display_name\":\"Alpha\"},\"beta\":{\"display_name\":\"Beta\"}}}");
    assert(tsuchi::content_squad_begin(after_mod, *after_content, problem) == TSUCHI_OK);
    assert(tsuchi::squad::table.type_name(after_mod, foreign_id, foreign_name, sizeof foreign_name, &foreign_bytes) == TSUCHI_NOT_FOUND);
    foreign_mod = tsuchi::mod_create("test.foreign");
    assert(tsuchi::content_squad_begin(foreign_mod, *foreign_content, problem) == TSUCHI_OK);
    assert(tsuchi::squad::table.type_name(foreign_mod, foreign_id, foreign_name, sizeof foreign_name, &foreign_bytes) == TSUCHI_OK);
    assert(!strcmp(foreign_name, "Visitor"));
    TSUCHI_Subscription subscription{};
    assert(tsuchi::squad::table.on_membership_changed(other, notice, nullptr, &subscription) == TSUCHI_OK);
    const TSUCHI_Actor leader{captain.at()}, target{member.at()}, second{rival.at()};
    const auto& api = tsuchi::squad::table;
    assert(api.hold_member(mod, "missing:type", leader, target) == TSUCHI_NOT_FOUND);
    assert(api.hold_member(mod, "test.other:first", leader, target) == TSUCHI_NOT_FOUND);
    assert(api.hold_member(mod, "bad id", leader, target) == TSUCHI_INVALID);
    assert(api.hold_member(mod, "test.squad:groink", leader, leader) == TSUCHI_INVALID);
    assert(api.hold_member(mod, "test.squad:groink", leader, target) == TSUCHI_OK);
    assert(!member.ticking && member.awake == 1 && joins == 1);
    assert(api.hold_member(other, "test.squad:groink", leader, target) == TSUCHI_BUSY);
    TSUCHI_SquadMember info{};
    assert(api.member_type(mod, target, &info) == TSUCHI_OK && info.leader.id == leader.id && !strcmp(info.kind, "test.squad:groink"));
    uint64_t found = 0;
    assert(api.type_members(mod, "test.squad:groink", leader, nullptr, 0, &found) == TSUCHI_CAPACITY && found == 1);
    TSUCHI_Actor listed{};
    assert(api.type_members(mod, "test.squad:groink", leader, &listed, 1, &found) == TSUCHI_OK && listed.id == target.id);
    char picked[96]; uint64_t bytes = 0;
    memset(picked, '!', sizeof picked);
    assert(api.type_name(mod, "test.squad:groink", picked, 1, &bytes) == TSUCHI_CAPACITY && bytes == 9 && picked[0] == '!');
    assert(api.type_name(mod, "test.squad:groink", picked, sizeof picked, &bytes) == TSUCHI_OK && !strcmp(picked, "Gr\xC3\xB6inks"));
    assert(api.type_name(mod, "bad id", picked, sizeof picked, &bytes) == TSUCHI_INVALID);
    assert(api.type_name(mod, "missing:type", picked, sizeof picked, &bytes) == TSUCHI_NOT_FOUND);
    assert(api.type_name(mod, "test.other:first", picked, sizeof picked, &bytes) == TSUCHI_NOT_FOUND);
    assert(api.type_name(mod, "test.other:wrong", picked, sizeof picked, &bytes) == TSUCHI_NOT_FOUND);
    assert(api.selected(mod, leader, picked, sizeof picked, &bytes) == TSUCHI_OK && !*picked);
    step();
    assert(get<int32_t>(captain.fs() + t::squad_formation::count) == 2);
    const uint64_t hud_at = reinterpret_cast<uint64_t>(hud.data()), icon_at = reinterpret_cast<uint64_t>(icon.data());
    const uint64_t material_at = reinterpret_cast<uint64_t>(material.data());
    put(reinterpret_cast<uint64_t>(framework.data()) + t::game_framework::ui, reinterpret_cast<uint64_t>(ui.data()));
    put(hud_at + t::squad_hud::icons, hud_at + 0x700), put(hud_at + 0x700, icon_at);
    put(hud_at + t::squad_hud::count, int32_t(1)), put(hud_at + t::squad_hud::player, captain.at());
    put(icon_at + t::squad_icon::toggle, uint8_t(2));
    put(icon_at + t::squad_icon::material, material_at);
    put(icon_at + t::squad_icon::textures, icon_at + 0x700), put(icon_at + 0x700, texture);
    // An added Pikmin owns toggle 2; the squad must borrow a different cell.
    reserved_toggle = 2;
    hud_hook(hud.data());
    assert(get<uint64_t>(material_at) == 0);
    put(icon_at + t::squad_icon::toggle, uint8_t(3));
    hud_hook(hud.data());
    assert(get<uint64_t>(material_at) == texture && get<float>(material_at + 12) == 1.f);
    reserved_toggle = 0;
    put(icon_at + t::squad_icon::toggle, uint8_t(2));
    hud_hook(hud.data());
    assert(get<uint64_t>(material_at) == texture && get<int32_t>(material_at + 8) == 0);
    assert(get<float>(material_at + 12) == 1.f);
    assert(get<int32_t>(icon_at + t::squad_icon::count) == 1 && get<uint8_t>(icon_at + t::squad_icon::kind) == 16);
    assert(api.hold_selection(mod, leader, "test.squad:groink") == TSUCHI_OK);
    assert(api.hold_selection(other, leader, "test.squad:groink") == TSUCHI_BUSY);
    assert(api.selected(mod, leader, picked, sizeof picked, &bytes) == TSUCHI_OK && !strcmp(picked, "test.squad:groink"));
    assert(api.selected(mod, leader, picked, 1, &bytes) == TSUCHI_CAPACITY && bytes == strlen("test.squad:groink") + 1);
    assert(api.release_selection(mod, leader) == TSUCHI_OK);
    assert(api.selected(mod, leader, picked, sizeof picked, &bytes) == TSUCHI_OK && !*picked);
    assert(api.release_selection(mod, leader) == TSUCHI_NOT_FOUND);
    assert(api.hold_member(mod, "test.squad:guard", second, target) == TSUCHI_OK);
    step();
    assert(api.member_type(mod, target, &info) == TSUCHI_OK && info.leader.id == second.id && !strcmp(info.kind, "test.squad:guard"));
    assert(api.release_member(mod, target) == TSUCHI_OK && member.ticking && member.awake == 0 && member.velocity[0] == 3);
    step();
    assert(get<int32_t>(material_at + 8) == 1 && get<float>(material_at + 12) == 0.25f);
    assert(api.member_type(mod, target, &info) == TSUCHI_NOT_FOUND);
    assert(api.release_member(mod, target) == TSUCHI_NOT_FOUND);
    assert(api.hold_member(mod, "test.squad:groink", leader, target) == TSUCHI_OK);
    left(member.at(), member.at(), tsuchi::ActorLeft::put_away);
    assert(member.ticking && member.awake == 0 && api.member_type(mod, target, &info) == TSUCHI_NOT_FOUND);
    assert(api.hold_member(mod, "test.squad:groink", leader, target) == TSUCHI_OK);
    member.alive = false; left(member.at(), member.at(), tsuchi::ActorLeft::destroyed);
    assert(api.member_type(mod, target, &info) == TSUCHI_NOT_FOUND);
    member.alive = true; member.ticking = true; member.awake = 0;
    assert(api.hold_member(mod, "test.squad:groink", leader, target) == TSUCHI_OK);
    captain.alive = false;
    const unsigned stale_before=stale_formation_accesses;step();
    assert(stale_formation_accesses==stale_before);captain.alive = true;
    assert(member.ticking && member.awake == 0 && api.member_type(mod, target, &info) == TSUCHI_NOT_FOUND);
    assert(api.hold_member(mod, "test.squad:groink", leader, target) == TSUCHI_OK);
    step();
    // Reusing the same native address with a new generation must not unlink
    // the old node through the newly initialized sentinel or decrement it.
    ++captain.formation_serial;
    const uint64_t sentinel=captain.fs()+t::squad_formation::list, stock=reinterpret_cast<uint64_t>(captain.stock.data());
    put(sentinel,stock);put(sentinel+8,stock);put(stock,sentinel);put(stock+8,sentinel);
    put(captain.fs()+t::squad_formation::count,int32_t(1));step();
    assert(get<int32_t>(captain.fs()+t::squad_formation::count)==2);
    assert(api.hold_selection(mod, leader, "test.squad:groink") == TSUCHI_OK);
    assert(grab_hook[0](captain.ai.data(), 0) == 0 && !member.launchable);
    step(); assert(api.member_type(mod, target, &info) == TSUCHI_OK);
    member.launchable = true; step();
    assert(launches == 1 && solutions == 1 && freed_paths == 1 && api.member_type(mod, target, &info) == TSUCHI_NOT_FOUND);
    assert(member.velocity[0] == 210 && member.velocity[2] == 510);
    assert(api.hold_member(mod, "test.squad:groink", leader, target) == TSUCHI_OK);
    tsuchi::mod_stop(mod);
    assert(api.type_name(other, "test.squad:groink", picked, sizeof picked, &bytes) == TSUCHI_NOT_FOUND);
    assert(member.ticking && member.awake == 0 && api.member_type(other, target, &info) == TSUCHI_NOT_FOUND);
    assert(get<int32_t>(captain.fs() + t::squad_formation::count) == 1);
    mod = tsuchi::mod_create("test.squad");
    content = declaration(source);
    assert(tsuchi::content_squad_begin(mod, *content, problem) == TSUCHI_OK);
    assert(api.type_name(mod, "test.squad:groink", picked, sizeof picked, &bytes) == TSUCHI_OK && !strcmp(picked, "Gr\xC3\xB6inks"));
    stop_on_tick = mod;
    assert(api.hold_member(mod, "test.squad:groink", leader, target) == TSUCHI_NOT_READY);
    assert(member.ticking && member.awake == 0 && !tsuchi::mod_running(mod));
    const auto* bad = declaration("{\"squad\":{\"groink\":{\"display_name\":\"G\",\"icon\":\"../escape.png\"}}}");
    assert(!tsuchi::content_squad_valid(*bad, problem));
    tsuchi::set_thread_probe([]() { return thread; }); tsuchi::note_game_thread(); thread = 2;
    assert(api.type_members(other, "test.squad:groink", leader, nullptr, 0, &found) == TSUCHI_INVALID);
    assert(api.type_name(other, "test.squad:groink", picked, sizeof picked, &bytes) == TSUCHI_INVALID);
    thread = 1; tsuchi::mod_stop(other); tsuchi::mod_stop(callback_mod); tsuchi::mod_stop(foreign_mod); tsuchi::mod_stop(after_mod); tsuchi::mod_stop(self);
    tsuchi::release_json(content); tsuchi::release_json(bad); tsuchi::release_json(failed); tsuchi::release_json(wrong_icon); problem.clear();
    tsuchi::release_json(foreign_content); tsuchi::release_json(after_content);
    assert(loaded_assets.empty());
    assert(joins == 7 && leaves == 7);
}
