// Work objects use their own finishers. Their saved changes are permanent.
#include <string.h>
#include "actors.h"
#include "actor_events.h"
#include "content.h"
#include "frame.h"
#include "flow.h"
#include "game.h"
#include "gimmicks_service.h"
#include "life.h"
#include "objects.h"
#include "registry.h"

namespace tsuchi {
namespace {
namespace t = game::table;
namespace a = t::gimmick_actor;
namespace c = t::gimmick_ai;
namespace f = t::gimmick_fsm;
namespace bb = t::gimmick_board;
game::ClassRef classes[] = {{t::name::gimmick_ai_class}, {t::name::gimmick_gate_class},
    {t::name::gimmick_build_class}, {t::name::gimmick_station_class},
    {t::name::gimmick_switch_class}, {t::name::gimmick_twin_class}, {t::name::gimmick_door_class},
    {t::name::gimmick_drain_class}, {t::name::gimmick_down_class}, {t::name::gimmick_pot_class},
    {t::name::gimmick_dig_class}, {t::name::gimmick_jelly_class}, {t::name::gimmick_vent_class},
    {t::name::gimmick_warp_class}};
enum Kind { other, gate, build_kind, station, switch_kind, twin, door, drain, down, pot, dig, jelly, vent, warp };
game::ClassRef scene_class{t::name::scene_component_class};
bool is(uint64_t ai, unsigned kind) { return game::is_a(ai, classes[kind]); }
uint64_t ai_of(uint64_t actor) {
    if (!game::is_game_actor(actor)) return 0;
    uint64_t ai = game::read_or<uint64_t>(actor + a::ai);
    return ai && game::live(ai) && is(ai, other) && game::readable(ai + c::done, 1) ? ai : 0;
}
TSUCHI_Result resolve(TSUCHI_Actor handle, uint64_t& actor, uint64_t& ai) {
    actor = game::from_handle(handle.id);
    if (!actor) return TSUCHI_NOT_FOUND;
    ai = ai_of(actor);
    return ai ? TSUCHI_OK : TSUCHI_INVALID;
}
bool on(uint64_t at) { return game::read_or<uint8_t>(at) != 0; }
uint64_t fsm(uint64_t ai) { return game::read_or<uint64_t>(ai + c::fsm); }
uint64_t board(uint64_t state) { return game::read_or<uint64_t>(state + f::blackboard); }
bool locked(uint64_t ai) { const uint64_t state = fsm(ai); return state && on(state + f::locked); }
bool awake(uint64_t actor, uint32_t frames = 90) { return game::write(actor + a::awake, frames); }
TSUCHI_Result state(uint64_t actor, uint64_t ai, TSUCHI_Gimmick& out) {
    out = {}; out.life = out.max_life = -1;
    uint64_t bytes = 0;
    TSUCHI_Result named = game::kind_into(game::actor_kind(actor), out.kind, sizeof out.kind, &bytes);
    if (named != TSUCHI_OK) return named;
    out.done = on(ai+c::done); out.removed = on(actor+a::removed);
    out.asleep = game::read_or<uint32_t>(actor+a::asleep) != 0;
    game::Life life{};
    if (game::read_life(actor, life)) { out.life=life.life; out.max_life=life.max_life; }
    if (is(ai,gate)) {
        int32_t total=game::read_or<int32_t>(ai+c::gate_total), left=game::read_or<int32_t>(ai+c::gate_left);
        if (total>0 && total<=16 && left>=0 && left<=total) { out.total=uint32_t(total); out.progress=uint32_t(total-left); }
    } else if (is(ai,build_kind) && !is(ai,station)) {
        int32_t total=game::read_or<int32_t>(ai+c::total_pieces), put=game::read_or<int32_t>(ai+c::pieces);
        if (total>0 && total<=100000 && put>=0 && put<=total) { out.total=uint32_t(total); out.progress=uint32_t(put); }
    } else if (is(ai,switch_kind)) out.on=on(board(fsm(ai))+bb::pressed);
    else if (is(ai,door)) out.on=on(ai+c::door_open);
    else if (is(ai,drain)) out.done |= on(ai+c::drain_done);
    else if (is(ai,down)) out.done |= on(ai+c::down_done);
    else if (is(ai,pot)) out.done |= on(ai+c::pot_broken);
    else if (is(ai,dig)) {
        const float ratio=game::read_or<float>(ai+c::dig_ratio);
        out.total=100; out.progress=ratio<=0?0u:ratio>=1?100u:uint32_t(ratio*100);
        out.done |= ratio>=1;
    } else if (is(ai,warp)) {
        const uint32_t phase=game::read_or<uint32_t>(board(fsm(ai))+bb::warp_stage,3);
        if (phase<=2) { out.progress=phase; out.total=2; out.done |= phase==2; }
    }
    return TSUCHI_OK;
}
TSUCHI_Result describe(TSUCHI_Actor handle, TSUCHI_Gimmick* out) {
    uint64_t actor=0,ai=0;
    const auto result=resolve(handle,actor,ai);
    return result==TSUCHI_OK?state(actor,ai,*out):result;
}
struct Collect { TSUCHI_Actor* buffer; uint64_t capacity,count; };
bool collect(void* context,uint64_t actor) {
    auto& list=*static_cast<Collect*>(context);
    if (!ai_of(actor)) return true;
    const auto handle=game::to_handle(actor); if (!handle) return true;
    if (list.count<list.capacity) list.buffer[list.count]={handle};
    ++list.count; return true;
}
TSUCHI_Result list(TSUCHI_Actor* buffer,uint64_t capacity,uint64_t* out) {
    if (!game::game_framework()) return TSUCHI_NOT_READY;
    Collect counted{nullptr,0,0}; game::each_actor(collect,&counted); *out=counted.count;
    if (counted.count>capacity) return TSUCHI_CAPACITY;
    Collect written{buffer,capacity,0}; game::each_actor(collect,&written); return TSUCHI_OK;
}
TSUCHI_Result break_gate(TSUCHI_Actor handle,bool quiet) {
    uint64_t actor=0,ai=0; auto r=resolve(handle,actor,ai); if (r!=TSUCHI_OK) return r;
    if (!is(ai,gate)) return TSUCHI_UNSUPPORTED;
    if (on(ai+c::done)) return TSUCHI_OK;
    const uint64_t s=fsm(ai); const auto request=game::method(s,t::gimmick_methods::gate_break);
    if (!request || !t::bound(t::fn::gimmick_work_done) || !t::bound(t::fn::gimmick_removed) || !game::writable(ai+c::gate_left,4)) return TSUCHI_NOT_READY;
    if (locked(ai)) return TSUCHI_BUSY;
    if (!game::set_life(actor,0,false)) return TSUCHI_NOT_READY;
    game::write(ai+c::gate_left,int32_t(0)); awake(actor); request(reinterpret_cast<void*>(s),quiet);
    if (quiet) { t::fn::gimmick_removed(reinterpret_cast<void*>(ai),false); t::fn::gimmick_work_done(reinterpret_cast<void*>(ai),true); }
    return TSUCHI_OK;
}
TSUCHI_Result pieces(TSUCHI_Actor handle,uint32_t wanted,bool quiet) {
    uint64_t actor=0,ai=0; auto r=resolve(handle,actor,ai); if (r!=TSUCHI_OK) return r;
    if (!is(ai,build_kind) || is(ai,station)) return TSUCHI_UNSUPPORTED;
    const int32_t current=game::read_or<int32_t>(ai+c::pieces,-1), total=game::read_or<int32_t>(ai+c::total_pieces,-1);
    if (total<=0 || current<0 || current>total) return TSUCHI_NOT_READY;
    if (wanted<uint32_t(current) || wanted>uint32_t(total)) return TSUCHI_INVALID;
    if (wanted==uint32_t(current)) return TSUCHI_OK;
    if (locked(ai)) return TSUCHI_BUSY;
    const auto apply=game::method(ai,t::gimmick_methods::apply_pieces), built=game::method(ai,t::gimmick_methods::built);
    const uint64_t context=game::game_context();
    if (!apply || !built || !t::bound(t::fn::gimmick_work_done) || !t::bound(t::fn::gimmick_complete_ui) || !t::bound(t::fn::gimmick_nav_rebuild) || !context || !game::writable(ai+c::pieces,4)) return TSUCHI_NOT_READY;
    game::write(ai+c::pieces,int32_t(wanted)); apply(reinterpret_cast<void*>(ai));
    if (wanted==uint32_t(total)) { t::fn::gimmick_work_done(reinterpret_cast<void*>(ai),true); built(reinterpret_cast<void*>(ai)); if (!quiet) { uint32_t effect[6]={uint32_t(t::constant::gimmick_complete_ui_kind),0,0,0,0,0}; t::fn::gimmick_complete_ui(reinterpret_cast<void*>(ai),effect); } }
    t::fn::gimmick_nav_rebuild(reinterpret_cast<void*>(context),true,true);
    return TSUCHI_OK;
}
TSUCHI_Result build(TSUCHI_Actor handle,bool quiet) {
    uint64_t actor=0,ai=0; auto r=resolve(handle,actor,ai); if (r!=TSUCHI_OK) return r;
    if (!is(ai,build_kind) || is(ai,station)) return TSUCHI_UNSUPPORTED;
    if (on(ai+c::done)) return TSUCHI_OK;
    return pieces(handle,game::read_or<uint32_t>(ai+c::total_pieces),quiet);
}
TSUCHI_Result set_progress(TSUCHI_Actor actor,uint32_t wanted) { return pieces(actor,wanted,false); }
TSUCHI_Result press(TSUCHI_Actor handle) {
    uint64_t actor=0,ai=0; auto r=resolve(handle,actor,ai); if (r!=TSUCHI_OK) return r;
    if (!is(ai,switch_kind)) return TSUCHI_UNSUPPORTED;
    const uint64_t s=fsm(ai), b=board(s); const auto request=game::method(s,t::gimmick_methods::switch_press);
    if (!s || !b || !request) return TSUCHI_NOT_READY;
    if (locked(ai) || !on(b+bb::enabled) || !on(b+bb::armed)) return TSUCHI_BUSY;
    const uint64_t param=game::read_or<uint64_t>(b+bb::parameter);
    if (!is(ai,twin) && !on(param+t::gimmick_switch_parameter::repeatable) && on(b+bb::pressed)) return TSUCHI_INVALID;
    if (!awake(actor)) return TSUCHI_NOT_READY;
    request(reinterpret_cast<void*>(s)); return TSUCHI_OK;
}
TSUCHI_Result open(TSUCHI_Actor handle,bool quiet) {
    uint64_t actor=0,ai=0; auto r=resolve(handle,actor,ai); if (r!=TSUCHI_OK) return r;
    if (!is(ai,door)) return TSUCHI_UNSUPPORTED;
    if (on(ai+c::door_open)) return TSUCHI_OK;
    const uint64_t s=fsm(ai), b=board(s), child=game::read_or<uint64_t>(s+f::child);
    if (!s || !b || !child) return TSUCHI_NOT_READY;
    if (locked(ai) || game::read_or<uint64_t>(child)!=game::main_base()+t::data::gimmick_door_closed_table) return TSUCHI_BUSY;
    if (quiet && (!t::bound(t::fn::gimmick_door_open) || !t::bound(t::fn::gimmick_work_done) || !game::writable(actor+a::wake_count,4))) return TSUCHI_NOT_READY;
    if (!game::writable(b+bb::want_open,2) || !awake(actor)) return TSUCHI_NOT_READY;
    if (quiet) {
        game::write(actor+a::wake_count,game::read_or<uint32_t>(actor+a::wake_count)+1);
        game::write(actor+a::flag_8c8,uint8_t(0)); game::write(actor+a::flag_8b0,uint8_t(0)); uint8_t immediate=1;
        t::fn::gimmick_door_open(reinterpret_cast<void*>(s),game::global<uint64_t>(t::data::gimmick_door_open_name),&immediate);
        t::fn::gimmick_work_done(reinterpret_cast<void*>(ai),true);
    } else { game::write(b+bb::want_close,uint8_t(0)); game::write(b+bb::want_open,uint8_t(1)); }
    return TSUCHI_OK;
}
TSUCHI_Result close(TSUCHI_Actor handle) {
    uint64_t actor=0,ai=0; auto r=resolve(handle,actor,ai); if (r!=TSUCHI_OK) return r;
    if (!is(ai,door) || !on(ai+c::door_type)) return TSUCHI_UNSUPPORTED;
    if (!on(ai+c::door_open)) return TSUCHI_OK;
    const uint64_t s=fsm(ai), b=board(s), child=game::read_or<uint64_t>(s+f::child);
    if (!s || !b || !child) return TSUCHI_NOT_READY;
    if (locked(ai) || game::read_or<uint64_t>(child+t::ai_state::name)!=game::global<uint64_t>(t::data::gimmick_door_wait_name)) return TSUCHI_BUSY;
    if (!game::writable(b+bb::want_open,2) || !awake(actor)) return TSUCHI_NOT_READY;
    game::write(b+bb::want_open,uint8_t(0)); game::write(b+bb::want_close,uint8_t(1)); return TSUCHI_OK;
}
TSUCHI_Result finish(TSUCHI_Actor handle) {
    uint64_t actor=0,ai=0; auto r=resolve(handle,actor,ai); if (r!=TSUCHI_OK) return r;
    TSUCHI_Gimmick described{}; r=state(actor,ai,described); if (r!=TSUCHI_OK) return r;
    if (described.done) return TSUCHI_OK;
    if (locked(ai)) return TSUCHI_BUSY;
    if (is(ai,dig)) {
        if (on(actor+a::removed) || on(actor+a::leaving) || on(ai+c::dig_filling) || game::read_or<int32_t>(ai+c::dig_stage)<2) return TSUCHI_BUSY;
        if (!t::bound(t::fn::gimmick_dig_finish) || !awake(actor,5)) return TSUCHI_NOT_READY;
        t::fn::gimmick_dig_finish(reinterpret_cast<void*>(ai));
    } else if (is(ai,pot)) {
        if (on(actor+a::removed) || !fsm(ai)) return TSUCHI_BUSY;
        game::Transform location{};
        if (!t::bound(t::fn::gimmick_pot_break) || !game::get_transform(actor,location) || !awake(actor)) return TSUCHI_NOT_READY;
        t::fn::gimmick_pot_break(reinterpret_cast<void*>(ai),location.location[0]-float(t::constant::gimmick_pot_hit_offset),location.location[1]);
    } else if (is(ai,jelly) || is(ai,vent)) {
        if (!game::life_component(actor) || !awake(actor)) return TSUCHI_NOT_READY;
        return game::set_life(actor,0,false)?TSUCHI_OK:TSUCHI_NOT_READY;
    } else if (is(ai,drain)) {
        const auto request=game::method(ai,t::gimmick_methods::drain);
        if (!request || !game::writable(ai+c::drain_done,1) || !awake(actor)) return TSUCHI_NOT_READY;
        request(reinterpret_cast<void*>(ai),false); game::write(ai+c::drain_done,uint8_t(1));
    } else if (is(ai,down)) {
        if (game::read_or<int32_t>(ai+c::down_phase)!=0) return TSUCHI_BUSY;
        if (!t::bound(t::fn::gimmick_down_remove) || !awake(actor)) return TSUCHI_NOT_READY;
        t::fn::gimmick_down_remove(reinterpret_cast<void*>(actor),true);
    } else if (is(ai,warp)) {
        const uint64_t s=fsm(ai), b=board(s);
        if (!s || game::read_or<uint64_t>(s)!=game::main_base()+t::data::gimmick_warp_table ||
            game::read_or<uint64_t>(s+f::owner)!=actor || !game::readable(s,t::constant::gimmick_warp_state_bytes) || !game::readable(b,t::constant::gimmick_warp_board_bytes) ||
            game::read_or<uint64_t>(b+bb::warp_ai)!=ai || game::read_or<uint64_t>(b+bb::warp_parameter)!=ai+c::warp_parameter ||
            game::read_or<uint32_t>(b+bb::warp_stage)>2 || on(actor+a::leaving)) return TSUCHI_NOT_READY;
        const auto parameter=ai+c::warp_parameter;
        if (!(game::read_or<float>(parameter+t::gimmick_switch_parameter::warp_first,-1)>=0 &&
            game::read_or<float>(parameter+t::gimmick_switch_parameter::warp_second,-1)>=0)) return TSUCHI_INVALID;
        const auto table=game::read_or<uint64_t>(ai);
        if (game::read_or<uint64_t>(table+t::gimmick_methods::apply_pieces.offset)!=game::main_base()+t::data::gimmick_warp_stage_function) return TSUCHI_NOT_READY;
        const int32_t shapes=game::read_or<int32_t>(b+bb::warp_count,-1);
        const uint64_t data=game::read_or<uint64_t>(b+bb::warp_shapes);
        if (shapes<0 || shapes>128 || (shapes && (!data || !game::readable(data,8*uint32_t(shapes))))) return TSUCHI_NOT_READY;
        for (int32_t i=0;i<shapes;++i) { const auto shape=game::read_or<uint64_t>(data+8*uint32_t(i)); if (shape && (!game::live(shape) || !game::is_a(shape,scene_class))) return TSUCHI_NOT_READY; }
        if (!t::bound(t::fn::gimmick_warp_refresh) || !t::bound(t::fn::gimmick_warp_open) || !game::life_component(actor)) return TSUCHI_NOT_READY;
        if (!awake(actor) || !game::set_life(actor,0,false)) return TSUCHI_NOT_READY;
        t::fn::gimmick_warp_refresh(reinterpret_cast<void*>(s),true); t::fn::gimmick_warp_open(reinterpret_cast<void*>(s),true);
    } else return TSUCHI_UNSUPPORTED;
    return TSUCHI_OK;
}
struct Seen { uint64_t actor; TSUCHI_Gimmick state,previous; uint32_t round; bool changed,left; };
Seen* seen=nullptr; uint32_t count=0,capacity=0,round=0;
uint32_t seen_index(uint64_t handle) {
    uint32_t low=0,high=count;
    while (low<high) { const uint32_t mid=low+(high-low)/2;
        if (seen[mid].actor<handle) low=mid+1; else high=mid; }
    return low;
}
bool poll_actor(void*,uint64_t actor) {
    const uint64_t ai=ai_of(actor), handle=game::to_handle(actor); if (!ai || !handle) return true;
    TSUCHI_Gimmick now{}; if (state(actor,ai,now)!=TSUCHI_OK) return true;
    const uint32_t i=seen_index(handle);
    if (i<count && seen[i].actor==handle) {
        const TSUCHI_Gimmick previous=seen[i].state; seen[i].round=round; seen[i].state=now;
        seen[i].previous=previous;
        seen[i].changed=now.progress!=previous.progress || now.done!=previous.done || now.on!=previous.on;
        return true;
    }
    if (count==capacity) {
        const uint32_t next=capacity?capacity*2:64; auto* grown=static_cast<Seen*>(allocate(sizeof(Seen)*next));
        if (!grown) return true;
        if (seen) memcpy(grown,seen,sizeof(Seen)*count); release(seen); seen=grown; capacity=next;
    }
    if (i<count) memmove(seen+i+1,seen+i,sizeof(Seen)*(count-i));
    seen[i]={handle,now,{},round,false,false}; ++count; return true;
}
void left(uint64_t actor,uint64_t handle,ActorLeft) {
    const auto ai=ai_of(actor); if (!ai) return;
    TSUCHI_Gimmick now{}; if (state(actor,ai,now)!=TSUCHI_OK) return;
    const uint32_t i=seen_index(handle);
    if (i<count && seen[i].actor==handle) {
        seen[i].previous=seen[i].state; seen[i].state=now; seen[i].left=true;
        seen[i].changed=now.progress!=seen[i].previous.progress || now.done!=seen[i].previous.done || now.on!=seen[i].previous.on;
    }
}
void step() {
    if (!gimmicks::changed_wanted()) { count=0; return; }
    ++round; game::each_actor(poll_actor,nullptr);
    for (uint32_t i=0;i<count;++i) if (seen[i].changed) {
        const Seen event=seen[i]; seen[i].changed=false;
        gimmicks::emit_changed({{event.actor},event.state.kind,event.state.progress,event.previous.progress,event.state.done,event.previous.done,event.state.on,event.previous.on});
    }
    for (uint32_t i=count;i--;) if (seen[i].round!=round || seen[i].left) {
        --count; if (i<count) memmove(seen+i,seen+i+1,sizeof(Seen)*(count-i));
    }
}
const gimmicks::Backend service{list,describe,break_gate,build,press,open,close,finish,set_progress};
} // namespace
void gimmicks_start() { gimmicks::set_backend(&service); on_actor_left(left); each_frame(step); }
} // namespace tsuchi
