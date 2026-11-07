// Controlled ordinary teardown admission. This tests real hold/restoration code,
// not a claim that the modeled native call sites have run in the game.
#include <array>
#include <cstdio>
#include <cstdlib>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include "ordinary_lifecycle.cpp"
#include "actor_events.h"
#include "added.h"
#include "clips.h"
#include "hidden.h"
#include "holds.h"
#include "materials.h"
#include "parts.h"
#include "frame.h"
#include "reflect.h"
using namespace tsuchi;
namespace {
int failed = 0;
std::vector<void (*)()> frame_steps;
void advance_frame() { for (auto step : frame_steps) step(); }
#define CHECK(x) do { if (!(x)) { std::printf("line%d: %s\n",__LINE__,#x); ++failed; } } while(0)
std::array<uint64_t, 0x560/8> vtable{};
std::array<uint64_t, 12> actor{}, other_actor{}, level{}, fixture_world{};
uint64_t fixture_owner = reinterpret_cast<uint64_t>(actor.data());
uint64_t fixture_part_owner = fixture_owner;
std::array<uint64_t,24> part_memory{}, second_memory{}, added_memory{};
uint64_t part = reinterpret_cast<uint64_t>(part_memory.data()), second_part = reinterpret_cast<uint64_t>(second_memory.data()),
    added_part = reinterpret_cast<uint64_t>(added_memory.data()), material = 0x3000, clip = 0x4000;
bool second_visible=true, trigger_destroy=false, destroy_veto=false, allow_native_reentry=true;
uint32_t restoration_calls=0, creations=0;
bool destroy_result=false;
void native_destroy();
std::unordered_map<uint64_t, uint64_t> handles;
uint64_t next_handle = 1;
std::unordered_set<uint64_t> retained_roots;
bool visible = true, gameplay = false, registered = true, existing_chain = false, chain_owned = false;
uint64_t fixture_slot = 0x5000;
uint32_t mode = 0;
uint32_t unregister_calls = 0, destroy_calls = 0, added_destroys = 0, end_calls = 0, fail_wrap = 0, wrap_attempts = 0, removed_hooks = 0;
HookInvocation invocation{};
void (*game_left)(uint64_t,uint64_t,ActorLeft) = nullptr;
TSUCHI_Mod* mod = nullptr;
uint64_t removal_caller = 0;
bool nested = false, native_reentry = false, expect_cleanup = true, mismatch_thread = false, lose_level = false, only_subset = false;
void stock_unregister(uint64_t, bool) {
    ++unregister_calls;
    registered = false;
    if (native_reentry) {
        CHECK(hold_material(mod, part, 0, material) == TSUCHI_BUSY);
        CHECK(hide_part(mod, part, false) == TSUCHI_BUSY);
        CHECK(hold_clip(mod, part, clip, true, 1) == TSUCHI_BUSY);
    }
}
struct EndBoundary {
    bool active = false, visible = false, second_visible = true;
    uint64_t material = 0;
    uint32_t mode = 0, addition_count = 0;
} end_boundary;
void stock_end(uint64_t, uint8_t) {
    if (end_boundary.active) {
        // Assert native resources at the original boundary, not only afterward
        // or through already-cleared public lease bookkeeping.
        CHECK(visible == end_boundary.visible);
        CHECK(second_visible == end_boundary.second_visible);
        CHECK(fixture_slot == end_boundary.material);
        CHECK(mode == end_boundary.mode);
        CHECK(added_destroys == end_boundary.addition_count);
        CHECK(!hidden_counts().holds && !hidden_counts().parts);
        CHECK(!material_holds() && !clip_holds() && !added_count());
    }
    ++end_calls;
}
void stock_destroyed(uint64_t) {
    ++destroy_calls;
    if (!(reinterpret_cast<uint8_t*>(actor.data())[game::table::engine_actor::play_state] & game::table::constant::actor_destroying_mask)) return;
    CHECK(!hidden_counts().holds && !hidden_counts().parts && !material_holds() && !clip_holds());
    CHECK(added_count() == 0 && added_destroys == 0); // native owns component teardown
    native_reentry = false;
    stock_unregister(fixture_owner, false);
    on_end(fixture_owner, 0); // nested native EndPlay consumes no second lease
    ++added_destroys; // one modeled engine teardown, not DestroyComponent twice
}
void native_destroy() {
    if (destroy_veto) { destroy_result=false;return; }
    auto& state=reinterpret_cast<uint8_t*>(actor.data())[game::table::engine_actor::play_state];
    if (state & game::table::constant::actor_destroying_mask) { destroy_result=true;return; }
    state |= uint8_t(game::table::constant::actor_destroying_mask);
    on_destroyed(fixture_owner);
    reinterpret_cast<uint32_t*>(part_memory.data())[game::table::actor_component::lifecycle_flags/4] |= uint32_t(game::table::constant::component_destroying_mask);
    reinterpret_cast<uint32_t*>(second_memory.data())[game::table::actor_component::lifecycle_flags/4] |= uint32_t(game::table::constant::component_destroying_mask);
    destroy_result=true;
}
void stock_remove(uint64_t, uint64_t, uint32_t flags) {
    if (!flags) { on_end(fixture_owner, game::table::constant::end_play_removed); return; }
    const HookInvocation before = invocation;
    if (nested) {
        invocation.caller = 0x1234; // temporary re-registration inside removal
        on_unregister(fixture_owner, false);
        CHECK(!visible && material_holds() == 1 && clip_holds() == 1);
        registered = true;
    }
    invocation.caller = removal_caller;
    if (mismatch_thread) invocation.thread=2;
    const uint64_t level_handle=game::to_handle(reinterpret_cast<uint64_t>(level.data()));
    const uint64_t level_value=game::from_handle(level_handle);
    if (lose_level) handles.erase(level_handle);
    native_reentry = allow_native_reentry && expect_cleanup && removal_caller == game::table::constant::removal_unregister_return;
    on_unregister(fixture_owner, only_subset);
    if (lose_level) handles[level_handle]=level_value;
    native_reentry = false;
    invocation = before;
}
void take_all() {
    registered = true;
    reinterpret_cast<uint8_t*>(actor.data())[game::table::engine_actor::play_state]=uint8_t(game::table::constant::actor_play_state_begun);
    CHECK(hide_part(mod, part, false) == TSUCHI_OK);
    CHECK(hold_material(mod, part, 0, material) == TSUCHI_OK);
    CHECK(hold_clip(mod, part, clip, true, 1) == TSUCHI_OK);
    CHECK(!visible && fixture_slot == material && mode == 1);
}
void restored() {
    CHECK(visible && fixture_slot == 0x5000 && mode == 0);
    CHECK(!hidden_counts().holds && !material_holds() && !clip_holds());
}
}
namespace tsuchi {
bool each_frame(void (*step)()) { frame_steps.push_back(step);return true; }
bool on_actor_left(void (*fn)(uint64_t,uint64_t,ActorLeft)) { game_left = fn; return true; }
uint64_t shader_source(uint64_t value) { return value; }
bool track_shader_slot(uint64_t,uint64_t,uint32_t) { return false; }
uint64_t drawn_as(uint64_t value) { return value; }
// Never the same twice: every held slot is resolved in every step, as these checks expect.
uint32_t shader_generation() { static uint32_t calls = 0; return ++calls; }
bool drawn_as_restoring(uint64_t value, game::CallGuard guard, uint64_t& shown) { shown=value; return guard.permits(); }
HookInvocation hook_invocation(uint64_t) { return invocation; }
uint32_t hook_count(uint64_t) { return existing_chain ? 1 : 0; }
bool hook_entry_owned(uint64_t) { return chain_owned; }
uint64_t hook_entry_base(uint64_t address) {
    const uint64_t offset=address-reinterpret_cast<uint64_t>(vtable.data());
    if (existing_chain && offset==game::table::ordinary_actor_vtable::end_play.offset) return game::table::constant::ordinary_end_play;
    return game::read_or<uint64_t>(address);
}
TSUCHI_Result hook_remove(TSUCHI_Mod*,uint64_t) { ++removed_hooks;return TSUCHI_OK; }
TSUCHI_Result hook_wrap(TSUCHI_Mod*,uint64_t entry,uint64_t,void*,uint32_t,uint64_t* id,void** next) {
    static uint64_t ids = 10;
    if (++wrap_attempts==fail_wrap) return TSUCHI_NO_MEMORY;
    *id = ids++;
    const uint64_t offset = entry - reinterpret_cast<uint64_t>(vtable.data());
    if (offset == game::table::ordinary_actor_vtable::end_play.offset) *next = reinterpret_cast<void*>(stock_end);
    else if (offset == game::table::ordinary_actor_vtable::unregister.offset) *next = reinterpret_cast<void*>(stock_unregister);
    else *next = reinterpret_cast<void*>(stock_destroyed);
    return TSUCHI_OK;
}
TSUCHI_Result hook_code_site(TSUCHI_Mod*,uint32_t,void*,uint64_t* id,void** next) { *id = 1; *next = reinterpret_cast<void*>(stock_remove); return TSUCHI_OK; }
}
namespace tsuchi::game {
uint64_t main_base() { return 0; }
bool readable(uint64_t at,uint64_t bytes) {
    for (const auto& range : {std::pair<uint64_t,uint64_t>{reinterpret_cast<uint64_t>(actor.data()),sizeof actor},
         {reinterpret_cast<uint64_t>(vtable.data()),sizeof vtable}, {reinterpret_cast<uint64_t>(other_actor.data()),sizeof other_actor}, {part,sizeof part_memory}, {second_part,sizeof second_memory}, {added_part,sizeof added_memory}})
        if (at >= range.first && at + bytes <= range.first + range.second) return true;
    return false;
}
bool writable(uint64_t at,uint64_t bytes) { return readable(at,bytes); }
bool executable(uint64_t) { return true; }
bool live(uint64_t value) { return value != 0; }
uint64_t to_handle(uint64_t value) {
    if (!value) return 0;
    for (const auto& h : handles) if (h.second == value) return h.first;
    handles[next_handle] = value; return next_handle++;
}
uint64_t from_handle(uint64_t handle) { const auto i = handles.find(handle); return i == handles.end() ? 0 : i->second; }
bool is_game_actor(uint64_t value) { return gameplay && value == fixture_owner; }
uint64_t part_owner(uint64_t value) { return value == part || value == second_part || value == added_part ? fixture_part_owner : 0; }
bool is_scene_part(uint64_t value) { return value == part || value == second_part || value == added_part; }
bool is_mesh_part(uint64_t value) { return value == part; }
bool is_material_image(uint64_t) { return false; }
bool is_shader_target(uint64_t value) { return value==part; }
bool keep_object(uint64_t value,bool keep) { return keep ? retained_roots.insert(value).second : retained_roots.erase(value)!=0; }
bool object_kept(uint64_t value) { return retained_roots.count(value)!=0; }
bool is_material(uint64_t value) { return value == material; }
bool material_slots(uint64_t,uint32_t& count) { count=1; return true; }
uint64_t slot_override(uint64_t,uint32_t) { return fixture_slot; }
bool set_slot_material(uint64_t,uint32_t,uint64_t value) { fixture_slot=value; return true; }
bool part_visible(uint64_t p,bool& value) { value=p==second_part ? second_visible : visible; return true; }
bool set_part_visible(uint64_t p,bool value) { (p==second_part ? second_visible : visible)=value; return true; }
bool restore_part_visible(uint64_t p,bool value,CallGuard guard) {
    CHECK(guard.permits());
    ++restoration_calls;
    set_part_visible(p,value);
    if (trigger_destroy) {
        trigger_destroy=false;
        CHECK(hidden_counts().parts==1); // the current hold ended before its callback
        native_destroy();
    }
    return true;
}
bool restore_slot_material(uint64_t p,uint32_t s,uint64_t value,CallGuard guard) {
    CHECK(guard.permits());++restoration_calls;return set_slot_material(p,s,value);
}
bool restore_animation_mode(uint64_t p,uint32_t value,CallGuard guard) {
    CHECK(guard.permits());++restoration_calls;return set_animation_mode(p,value);
}
bool restore_destroy_part(uint64_t p,CallGuard guard) { CHECK(guard.permits());++restoration_calls;return destroy_part(p); }
bool each_attached(uint64_t value,void (*fn)(void*,uint64_t),void* context) { fn(context,value); return true; }
bool is_skeletal_part(uint64_t value) { return value == part; }
bool is_clip(uint64_t value) { return value == clip; }
bool animation_mode(uint64_t,uint32_t& value) { value=mode; return true; }
bool set_animation_mode(uint64_t,uint32_t value) { mode=value; return true; }
bool play_clip_on(uint64_t,uint64_t,bool,float) { mode=1; return true; }
bool set_clip_rate_on(uint64_t,float) { return true; }
bool seek_clip_on(uint64_t,float) { return true; }
bool clip_state(uint64_t,float& position,float& rate,bool& playing) { position=0;rate=1;playing=true;return true; }
TSUCHI_Result add_part(uint64_t,uint64_t,uint64_t,const char*,uint64_t& out) { ++creations;out=added_part;return TSUCHI_OK; }
bool destroy_part(uint64_t) { ++added_destroys; return true; }
}
uint32_t second_listener_calls = 0;
void second_listener(uint64_t, uint64_t, bool) { ++second_listener_calls; }
int main() {
    using namespace tsuchi::game::table;
    actor[0] = reinterpret_cast<uint64_t>(vtable.data());
    actor[object::outer/8] = reinterpret_cast<uint64_t>(level.data());
    vtable[ordinary_actor_vtable::end_play.offset/8] = constant::ordinary_end_play;
    vtable[ordinary_actor_vtable::unregister.offset/8] = constant::ordinary_unregister;
    vtable[ordinary_actor_vtable::destroyed.offset/8] = constant::ordinary_destroyed;
    hidden_start(); materials_start(); clips_start(); added_start();
    // The camera listens after the hold tables do. Both must keep hearing teardown.
    ordinary_lifecycle_listener(second_listener);
    mod = mod_create("ordinary.test");
    CHECK(ordinary_lifecycle_watch(fixture_owner) == TSUCHI_NOT_READY);
    ordinary_lifecycle_start(mod_create("ordinary.framework"));
    // Unknown override is refused before any visible/material/animation change.
    vtable[ordinary_actor_vtable::destroyed.offset/8] = 123;
    CHECK(ordinary_lifecycle_watch(fixture_owner) == TSUCHI_UNSUPPORTED);
    uint64_t refused_part=999;
    CHECK(add_part_for(mod,fixture_owner,1,0,nullptr,refused_part)==TSUCHI_UNSUPPORTED && !refused_part && creations==0);
    CHECK(owned_count(mod)==0);
    restored();
    vtable[ordinary_actor_vtable::destroyed.offset/8] = constant::ordinary_destroyed;
    existing_chain=true;
    vtable[ordinary_actor_vtable::end_play.offset/8]=9999;
    CHECK(ordinary_lifecycle_watch(fixture_owner)==TSUCHI_BUSY);
    restored();
    CHECK(owned_count(mod)==0); // refusal allocated no mod hold
    chain_owned=true;
    take_all(); // authenticated core-owned chain is supported
    invocation = {0x1234,1,1};
    on_unregister(fixture_owner,false); // no removal scope: valid leases remain
    CHECK(!visible && material_holds()==1 && clip_holds()==1);
    reinterpret_cast<uint8_t*>(actor.data())[engine_actor::play_state]=0;
    on_end(fixture_owner,constant::end_play_removed); // original native EndPlay is a no-op before BeginPlay
    CHECK(!visible && material_holds()==1 && clip_holds()==1);
    reinterpret_cast<uint8_t*>(actor.data())[engine_actor::play_state]=uint8_t(constant::actor_play_state_begun);
    // Same actor outer and exact caller without a tracked removal scope refuse.
    invocation.caller=constant::removal_unregister_return;
    on_unregister(fixture_owner,false);
    CHECK(!visible && material_holds()==1);
    // Base548 called without native BeingDestroyed does not consume leases.
    const uint32_t before_direct_destroy=destroy_calls;
    on_destroyed(fixture_owner);
    CHECK(destroy_calls==before_direct_destroy+1); // original forwarded exactly once
    // Refused DestroyActor is modeled by no admitted virtual dispatch.
    CHECK(!visible && material_holds()==1 && clip_holds()==1);
    removal_caller = 0x1234;
    on_remove(reinterpret_cast<uint64_t>(fixture_world.data()),reinterpret_cast<uint64_t>(level.data()),1);
    CHECK(!visible && material_holds()==1);
    removal_caller = constant::removal_unregister_return;
    expect_cleanup=false;
    invocation.token=0; // untracked scope is not teardown provenance
    on_remove(reinterpret_cast<uint64_t>(fixture_world.data()),reinterpret_cast<uint64_t>(level.data()),1);
    CHECK(!visible && material_holds()==1);
    invocation.token=1;mismatch_thread=true;
    on_remove(reinterpret_cast<uint64_t>(fixture_world.data()),reinterpret_cast<uint64_t>(level.data()),1);
    CHECK(!visible && material_holds()==1);
    mismatch_thread=false;lose_level=true;
    on_remove(reinterpret_cast<uint64_t>(fixture_world.data()),reinterpret_cast<uint64_t>(level.data()),1);
    CHECK(!visible && material_holds()==1);
    lose_level=false;only_subset=true;
    on_remove(reinterpret_cast<uint64_t>(fixture_world.data()),reinterpret_cast<uint64_t>(level.data()),1);
    CHECK(!visible && material_holds()==1);
    only_subset=false;expect_cleanup=true;nested=true;
    on_remove(reinterpret_cast<uint64_t>(fixture_world.data()),reinterpret_cast<uint64_t>(level.data()),1);
    restored(); nested=false;
    // Same retained UObject can enter play and acquire new leases again.
    take_all();
    on_remove(reinterpret_cast<uint64_t>(fixture_world.data()),reinterpret_cast<uint64_t>(level.data()),0);
    restored();
    take_all();
    uint64_t made=0;
    CHECK(add_part_for(mod,fixture_owner,1,0,nullptr,made)==TSUCHI_OK && made==added_part);
    native_destroy();
    CHECK(!visible && fixture_slot==material && mode==1); // accepted destruction forgets, never restores destroyed parts
    CHECK(destroy_calls==2 && added_destroys==1 && end_calls==3);
    reinterpret_cast<uint8_t*>(actor.data())[engine_actor::play_state]=uint8_t(constant::actor_play_state_begun);
    part_memory.fill(0);second_memory.fill(0);added_memory.fill(0);
    visible=true;second_visible=true;fixture_slot=0x5000;mode=0;added_destroys=0;
    // First restoration destroys owner synchronously. Later hidden/material/clip/added holds end before native teardown.
    take_all();CHECK(hide_part(mod,second_part,false)==TSUCHI_OK);
    CHECK(add_part_for(mod,fixture_owner,1,0,nullptr,made)==TSUCHI_OK);
    allow_native_reentry=false;trigger_destroy=true;
    const uint32_t before_restore=restoration_calls;
    on_remove(reinterpret_cast<uint64_t>(fixture_world.data()),reinterpret_cast<uint64_t>(level.data()),1);
    CHECK(destroy_result && restoration_calls==before_restore+1 && second_visible && !visible);
    CHECK(fixture_slot==material && mode==1 && added_destroys==1);
    CHECK(!hidden_counts().holds && !hidden_counts().parts && !material_holds() && !clip_holds() && !added_count());
    // Veto keeps native flags and synchronous false result; surviving later holds all restore.
    reinterpret_cast<uint8_t*>(actor.data())[engine_actor::play_state]=uint8_t(constant::actor_play_state_begun);
    part_memory.fill(0);second_memory.fill(0);added_memory.fill(0);
    visible=true;second_visible=true;fixture_slot=0x5000;mode=0;added_destroys=0;
    take_all();CHECK(hide_part(mod,second_part,false)==TSUCHI_OK);
    CHECK(add_part_for(mod,fixture_owner,1,0,nullptr,made)==TSUCHI_OK);
    destroy_veto=true;trigger_destroy=true;
    on_remove(reinterpret_cast<uint64_t>(fixture_world.data()),reinterpret_cast<uint64_t>(level.data()),1);
    CHECK(!destroy_result && second_visible && added_destroys==1);
    restored();destroy_veto=false;allow_native_reentry=true;
    // Existing gameplay destroyed policy stays unchanged for hidden/material.
    gameplay=true; take_all();
    game_left(fixture_owner,game::to_handle(fixture_owner),ActorLeft::destroyed);
    CHECK(!visible && fixture_slot==material && mode==0);
    visible=true;fixture_slot=0x5000;
    take_all(); game_left(fixture_owner,game::to_handle(fixture_owner),ActorLeft::put_away); restored();
    gameplay=false; take_all(); mod_stop(mod); restored();
    // Same raw owner address with a new serial/use generation must not receive
    // restoration from holds that captured its old generation.
    mod=mod_create("ordinary.owner-reuse");take_all();
    const uint64_t old_owner=game::to_handle(fixture_owner);
    handles.erase(old_owner);
    CHECK(game::to_handle(fixture_owner)!=old_owner);
    const uint32_t before_owner_reuse=restoration_calls;
    mod_stop(mod);
    CHECK(restoration_calls==before_owner_reuse && !visible && fixture_slot==material && mode==1);
    CHECK(!hidden_counts().holds && !hidden_counts().parts && !material_holds() && !clip_holds());
    visible=true;fixture_slot=0x5000;mode=0;
    // Existing ordinary take/update rejects a new owner generation before apply.
    mod=mod_create("ordinary.update-owner-reuse");take_all();
    const uint64_t update_owner=game::to_handle(fixture_owner);
    handles.erase(update_owner);
    CHECK(game::to_handle(fixture_owner)!=update_owner);
    const uint32_t before_update_reuse=restoration_calls;
    CHECK(hide_part(mod,part,false)==TSUCHI_NOT_READY && !visible); // identical children shortcut
    CHECK(hide_part(mod,part,true)==TSUCHI_NOT_READY && !visible);
    CHECK(hold_material(mod,part,0,material)==TSUCHI_NOT_READY);
    CHECK(hold_clip(mod,part,clip,true,1)==TSUCHI_NOT_READY);
    // Keep must forget all stale associations before reapplying any state.
    visible=true;fixture_slot=0x9000;mode=0;
    advance_frame();
    CHECK(visible && fixture_slot==0x9000 && mode==0 && restoration_calls==before_update_reuse);
    CHECK(!hidden_counts().holds && !hidden_counts().parts && !material_holds() && !clip_holds());
    mod_stop(mod);fixture_slot=0x5000;
    // Transfer of a live component to another live Actor is independent of reuse.
    mod=mod_create("ordinary.reparent");take_all();
    other_actor=actor;
    fixture_part_owner=reinterpret_cast<uint64_t>(other_actor.data());
    CHECK(hide_part(mod,part,false)==TSUCHI_NOT_READY && !visible); // identical children shortcut
    CHECK(hide_part(mod,part,true)==TSUCHI_NOT_READY && !visible);
    CHECK(hold_material(mod,part,0,material)==TSUCHI_NOT_READY);
    visible=true;fixture_slot=0x9000;mode=0;
    const uint32_t before_reparent=restoration_calls;
    advance_frame();
    CHECK(visible && fixture_slot==0x9000 && mode==0 && restoration_calls==before_reparent);
    CHECK(!hidden_counts().holds && !material_holds() && !clip_holds());
    mod_stop(mod);fixture_part_owner=fixture_owner;fixture_slot=0x5000;
    // A matching ordinary association remains usable for updates and keep.
    mod=mod_create("ordinary.same-owner");take_all();
    CHECK(hide_part(mod,part,false)==TSUCHI_OK && !visible);
    CHECK(hide_part(mod,part,true)==TSUCHI_OK && !visible);
    CHECK(hide_part(mod,part,false)==TSUCHI_OK && !visible);
    fixture_slot=0x9000;
    advance_frame();
    CHECK(!visible && fixture_slot==material && material_holds()==1);
    mod_stop(mod);CHECK(visible && fixture_slot==0x9000 && mode==0);
    fixture_slot=0x5000;
    // Gameplay marker0 keeps its preexisting owner-independent update policy.
    gameplay=true;mod=mod_create("gameplay.marker-zero");take_all();
    const uint64_t game_owner=game::to_handle(fixture_owner);
    handles.erase(game_owner);CHECK(game::to_handle(fixture_owner)!=game_owner);
    CHECK(hide_part(mod,part,false)==TSUCHI_OK && !visible);
    CHECK(hide_part(mod,part,true)==TSUCHI_OK && !visible);
    fixture_slot=0x9000;advance_frame();
    CHECK(fixture_slot==material && material_holds()==1);
    mod_stop(mod);CHECK(visible && fixture_slot==0x9000 && mode==0);
    gameplay=false;fixture_slot=0x5000;
    // The part generation is independently checked even when its owner survives.
    mod=mod_create("ordinary.part-reuse");take_all();
    const uint64_t old_part=game::to_handle(part);
    handles.erase(old_part);
    CHECK(game::to_handle(part)!=old_part);
    const uint32_t before_part_reuse=restoration_calls;
    mod_stop(mod);
    CHECK(restoration_calls==before_part_reuse && !visible && fixture_slot==material && mode==1);
    CHECK(!hidden_counts().holds && !hidden_counts().parts && !material_holds() && !clip_holds());
    visible=true;fixture_slot=0x5000;mode=0;
    CHECK(unregister_calls>=4);
    // Native7's first actual boundary was LevelTransition(reason1), owner
    // state0x44 and part flags0x0d42060b, both lacking their destroying bits.
    // EndPlay's reason never proves accepted native destruction. All engine
    // reasons must restore this live owner before forwarding its original.
    for (uint8_t reason = 0; reason != 5; ++reason) {
        mod = mod_create("ordinary.live-end-boundary");
        part_memory.fill(0); second_memory.fill(0); added_memory.fill(0);
        visible = second_visible = true; fixture_slot = 0x5000; mode = 0;
        take_all();
        reinterpret_cast<uint8_t*>(actor.data())[engine_actor::play_state] = 0x44;
        reinterpret_cast<uint32_t*>(part_memory.data())[actor_component::lifecycle_flags/4] = 0x0d42060b;
        uint64_t addition = 0;
        CHECK(add_part_for(mod,fixture_owner,1,0,nullptr,addition) == TSUCHI_OK);
        const uint32_t before_end = end_calls;
        end_boundary = {true, true, true, 0x5000, 0, added_destroys + 1};
        on_end(fixture_owner,reason);
        end_boundary.active = false;
        CHECK(end_calls == before_end + 1); // native original exactly once
        CHECK(owned_count(mod) == 0);
        restored(); mod_stop(mod);
    }
    // Even RemovedFromWorld(reason3) must not restore an owner that native
    // DestroyActor has accepted. Current part bits may not be set yet.
    for (uint8_t reason : {uint8_t(1), uint8_t(3)}) {
        mod = mod_create("ordinary.accepted-end-boundary");
        part_memory.fill(0); second_memory.fill(0); added_memory.fill(0);
        visible = second_visible = true; fixture_slot = 0x5000; mode = 0;
        take_all(); uint64_t addition = 0;
        CHECK(add_part_for(mod,fixture_owner,1,0,nullptr,addition) == TSUCHI_OK);
        reinterpret_cast<uint8_t*>(actor.data())[engine_actor::play_state] |= uint8_t(constant::actor_destroying_mask);
        const uint32_t before_end = end_calls, before_restore = restoration_calls;
        end_boundary = {true, false, true, material, 1, added_destroys};
        on_end(fixture_owner,reason);
        end_boundary.active = false;
        CHECK(end_calls == before_end + 1 && restoration_calls == before_restore);
        CHECK(owned_count(mod) == 0);
        mod_stop(mod);
    }
    // A live owner can have one component already being destroyed. Skip that
    // part but restore its surviving sibling and destroy the owned addition.
    mod = mod_create("ordinary.mixed-end-boundary");
    part_memory.fill(0); second_memory.fill(0); added_memory.fill(0);
    visible = second_visible = true; fixture_slot = 0x5000; mode = 0;
    take_all(); CHECK(hide_part(mod,second_part,false) == TSUCHI_OK);
    uint64_t addition = 0;
    CHECK(add_part_for(mod,fixture_owner,1,0,nullptr,addition) == TSUCHI_OK);
    reinterpret_cast<uint32_t*>(part_memory.data())[actor_component::lifecycle_flags/4] |= uint32_t(constant::component_destroying_mask);
    const uint32_t before_mixed_end = end_calls;
    end_boundary = {true, false, true, material, 1, added_destroys + 1};
    on_end(fixture_owner,1);
    end_boundary.active = false;
    CHECK(end_calls == before_mixed_end + 1 && owned_count(mod) == 0);
    mod_stop(mod);
    part_memory.fill(0); second_memory.fill(0); added_memory.fill(0);
    visible = second_visible = true; fixture_slot = 0x5000; mode = 0;
    // First live LevelTransition restoration can synchronously accept native
    // destruction. The remaining holds end without another native restore.
    mod = mod_create("ordinary.reentrant-transition");
    part_memory.fill(0); second_memory.fill(0); added_memory.fill(0);
    visible = second_visible = true; fixture_slot = 0x5000; mode = 0; added_destroys = 0;
    take_all(); CHECK(hide_part(mod,second_part,false) == TSUCHI_OK);
    CHECK(add_part_for(mod,fixture_owner,1,0,nullptr,addition) == TSUCHI_OK);
    allow_native_reentry = false; trigger_destroy = true;
    const uint32_t before_reentrant_transition = restoration_calls, before_reentrant_end = end_calls;
    on_end(fixture_owner,1);
    CHECK(destroy_result && restoration_calls == before_reentrant_transition + 1);
    CHECK(second_visible && !visible && fixture_slot == material && mode == 1);
    CHECK(added_destroys == 1 && end_calls == before_reentrant_end + 2);
    CHECK(!hidden_counts().holds && !hidden_counts().parts && !material_holds() && !clip_holds() && !added_count());
    CHECK(owned_count(mod) == 0); mod_stop(mod);
    // A native veto during the same boundary leaves resources restorable.
    mod = mod_create("ordinary.veto-transition");
    part_memory.fill(0); second_memory.fill(0); added_memory.fill(0);
    visible = second_visible = true; fixture_slot = 0x5000; mode = 0; added_destroys = 0;
    take_all(); CHECK(hide_part(mod,second_part,false) == TSUCHI_OK);
    CHECK(add_part_for(mod,fixture_owner,1,0,nullptr,addition) == TSUCHI_OK);
    destroy_veto = true; trigger_destroy = true;
    const uint32_t before_veto_end = end_calls;
    end_boundary = {true, true, true, 0x5000, 0, added_destroys + 1};
    on_end(fixture_owner,1);
    end_boundary.active = false;
    CHECK(!destroy_result && end_calls == before_veto_end + 1 && owned_count(mod) == 0);
    CHECK(second_visible); restored(); mod_stop(mod);
    destroy_veto = false; allow_native_reentry = true;
    part_memory.fill(0); second_memory.fill(0); added_memory.fill(0);
    visible = second_visible = true; fixture_slot = 0x5000; mode = 0;
    // Failure after one slot was installed rolls back that hook, allocates no
    // mod hold, and makes no visibility/material/animation change.
    tsuchi::tables=nullptr; // isolated fake hook backend starts a fresh installation
    fail_wrap=wrap_attempts+2;
    mod=mod_create("ordinary.install-failure");
    CHECK(hide_part(mod,part,false)==TSUCHI_NO_MEMORY);
    CHECK(removed_hooks==1 && owned_count(mod)==0);
    restored();
    CHECK(ordinary_lifecycle_watch(fixture_owner)==TSUCHI_UNSUPPORTED);
    mod_stop(mod);
    CHECK(second_listener_calls > 0);
    return failed;
}
