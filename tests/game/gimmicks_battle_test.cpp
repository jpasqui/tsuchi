// The real backends and central Holds, with native finishers supplied by a made-up game.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>
#include "actors.h"
#include "actor_events.h"
#include "battle_service.h"
#include "content.h"
#include "flow.h"
#include "frame.h"
#include "game.h"
#include "gimmicks_service.h"
#include "holds.h"
#include "life.h"
#include "objects.h"
#include "ordinary_lifecycle.h"
#include "registry.h"
#include "hooks.h"
using namespace tsuchi;
namespace t=tsuchi::game::table;
void tsuchi_gimmicks_start();
namespace tsuchi { void gimmicks_start(); void battle_start(TSUCHI_Mod*); }
namespace {
int checks=0;
#define CHECK(x) do { ++checks; if (!(x)) { std::fprintf(stderr,"line %d: %s\n",__LINE__,#x); std::exit(1); } } while(0)
struct Memory { uint64_t address,size; };
std::vector<Memory> ranges;
std::vector<void(*)()> steps;
std::vector<void(*)(uint64_t,uint64_t,ActorLeft)> left_listeners;
struct Object { uint64_t handle,address,ai; std::string type; float life=100; bool in_play=true; };
std::vector<Object> objects;
std::map<uint64_t,uint64_t> defaults;
std::map<uint64_t,game::Name> kinds;
bool content_live=true;
uint64_t queued_class=0;
uint64_t framework=0,context=0,base=0x100000000ull;
uint64_t thread=1;
uint64_t thread_probe() { return thread; }
uint32_t (*tug_hook)(void*,float)=nullptr;
uint32_t (*roulette_hook)(void*,void*)=nullptr;
int32_t scores[2]{};
unsigned changed=0,popped=0,bonuses=0,score_events=0,phases=0,roulette_events=0,tug_events=0;
template<class T> void put(uint64_t address,T value) { std::memcpy(reinterpret_cast<void*>(address),&value,sizeof value); }
uint64_t memory(size_t bytes) { auto* block=std::calloc(1,bytes); CHECK(block); const auto address=reinterpret_cast<uint64_t>(block); ranges.push_back({address,bytes}); return address; }
Object* object(uint64_t address) { for (auto& entry:objects) if (entry.address==address) return &entry; return nullptr; }
Object* owner_ai(uint64_t ai) { for (auto& entry:objects) if (entry.ai==ai) return &entry; return nullptr; }
uint64_t actor(const char* type) {
    const auto address=memory(0x900),ai=memory(0xC00),state=memory(0xC0),board=memory(0xC0),table=memory(0x900),fsm_table=memory(0x100);
    objects.push_back({uint64_t(objects.size()+100),address,ai,type});
    put(address+t::gimmick_actor::ai,ai); put(ai+t::gimmick_ai::fsm,state); put(ai,table); put(state,fsm_table);
    put(state+t::gimmick_fsm::owner,address); put(state+t::gimmick_fsm::blackboard,board);
    return address;
}
TSUCHI_Actor handle(uint64_t actor) { return {game::to_handle(actor)}; }
void step() { note_game_thread(); for (const auto function:steps) function(); }
uint64_t state_of(uint64_t ai) { return game::read_or<uint64_t>(ai+t::gimmick_ai::fsm); }
uint64_t board_of(uint64_t ai) { return game::read_or<uint64_t>(state_of(ai)+t::gimmick_fsm::blackboard); }
void done(void* ai,bool value) { put(reinterpret_cast<uint64_t>(ai)+t::gimmick_ai::done,uint8_t(value)); }
void removed(void* ai,bool) { put(owner_ai(reinterpret_cast<uint64_t>(ai))->address+t::gimmick_actor::removed,uint8_t(1)); }
void gate_break(void* state,bool) { const auto actor=game::read_or<uint64_t>(reinterpret_cast<uint64_t>(state)+t::gimmick_fsm::owner); done(reinterpret_cast<void*>(object(actor)->ai),true); }
void apply_pieces(void*) {}
void built(void* ai) { done(ai,true); }
void complete_ui(void*,void*) {}
void nav_rebuild(void*,bool,bool) {}
void press(void* state) { const auto board=game::read_or<uint64_t>(reinterpret_cast<uint64_t>(state)+t::gimmick_fsm::blackboard); put(board+t::gimmick_board::pressed,uint8_t(1)); }
void dig(void* ai) { put(reinterpret_cast<uint64_t>(ai)+t::gimmick_ai::dig_ratio,1.0f); }
void pot(void* ai,float,float) { put(reinterpret_cast<uint64_t>(ai)+t::gimmick_ai::pot_broken,uint8_t(1)); }
void drain(void* ai,bool) { done(ai,true); }
void down(void* actor,bool) { done(reinterpret_cast<void*>(object(reinterpret_cast<uint64_t>(actor))->ai),true); }
void door(void* state,uint64_t,void*) { const auto actor=game::read_or<uint64_t>(reinterpret_cast<uint64_t>(state)+t::gimmick_fsm::owner); put(object(actor)->ai+t::gimmick_ai::door_open,uint8_t(1)); }
void warp_refresh(void* state,bool) { const auto board=game::read_or<uint64_t>(reinterpret_cast<uint64_t>(state)+t::gimmick_fsm::blackboard); put(board+t::gimmick_board::warp_stage,uint32_t(2)); }
void warp_open(void* state,bool) { const auto actor=game::read_or<uint64_t>(reinterpret_cast<uint64_t>(state)+t::gimmick_fsm::owner); done(reinterpret_cast<void*>(object(actor)->ai),true); }
int32_t read_score(void*,int32_t player) { return scores[player]; }
void add_score(void*,int32_t player,int32_t delta) { scores[player]+=delta; if (scores[player]<0) scores[player]=0; }
void pop_class(void* place,uint64_t kind,void* cls,uint64_t flag) {
    queued_class=reinterpret_cast<uint64_t>(cls);
    const auto at=reinterpret_cast<uint64_t>(place); put(at+t::battle_pop::queued,uint8_t(1)); put(at+t::battle_pop::kind,uint8_t(kind)); put(at+t::battle_pop::flag,uint8_t(flag));
}
void spin(void*) {}
void flick(void*,uint64_t) {}
uint32_t original_tug(void* state,float) { if (game::read_or<uint8_t>(reinterpret_cast<uint64_t>(state)+t::battle_tug::phase)==2) put(reinterpret_cast<uint64_t>(state)+t::battle_tug::phase,uint8_t(3)); return 0; }
uint32_t original_roulette(void*,void*) { return 9; }
void record_gimmick(void*,const TSUCHI_GimmicksChanged*) { ++changed; }
void record_phase(void*,const TSUCHI_BattlePhaseChanged*) { ++phases; }
void record_score(void*,const TSUCHI_BattleScoreChanged*) { ++score_events; }
void record_pop(void*,const TSUCHI_BattlePopped*) { ++popped; }
void record_bonus(void*,const TSUCHI_BattleBonusChanged*) { ++bonuses; }
void record_roulette(void*,const TSUCHI_BattleRoulette*) { ++roulette_events; }
void record_tug(void*,const TSUCHI_BattleTug*) { ++tug_events; }
}
namespace tsuchi {
bool each_frame(void(*function)()) { steps.push_back(function); return true; }
bool on_actor_left(void(*listener)(uint64_t,uint64_t,ActorLeft)) { left_listeners.push_back(listener); return true; }
void ordinary_lifecycle_listener(void(*)(uint64_t,uint64_t,bool)) {}
TSUCHI_Result ordinary_lifecycle_watch(uint64_t) { return TSUCHI_OK; }
uint64_t ordinary_lifecycle_restore_part(uint64_t handle,uint64_t) { return game::from_handle(handle); }
TSUCHI_Result hook_wrap(TSUCHI_Mod*,uint64_t entry,uint64_t,void* replacement,uint32_t,uint64_t* id,void** original) {
    *id=1;
    if (entry==base+t::slot::battle_tug.table+t::slot::battle_tug.offset) { tug_hook=reinterpret_cast<uint32_t(*)(void*,float)>(replacement); *original=reinterpret_cast<void*>(original_tug); }
    else { roulette_hook=reinterpret_cast<uint32_t(*)(void*,void*)>(replacement); *original=reinterpret_cast<void*>(original_roulette); }
    return TSUCHI_OK;
}
}
namespace tsuchi::game {
uint64_t main_base() { return base; }
bool readable(uint64_t address,uint64_t bytes) { for (auto range:ranges) if (address>=range.address && address-range.address<=range.size && bytes<=range.size-(address-range.address)) return true; return false; }
bool writable(uint64_t address,uint64_t bytes) { return readable(address,bytes); }
bool executable(uint64_t address) { return address!=0; }
bool live(uint64_t address) { return object(address)!=nullptr || owner_ai(address)!=nullptr; }
uint64_t from_handle(uint64_t handle) { for (auto& entry:objects) if (entry.handle==handle) return entry.address; return 0; }
uint64_t to_handle(uint64_t address) { auto* entry=object(address); return entry?entry->handle:0; }
bool is_game_actor(uint64_t address) { auto* entry=object(address); return entry && entry->type!="Class" && entry->type!="World"; }
bool is_class(uint64_t address) { auto* entry=object(address); return entry && entry->type=="Class"; }
uint64_t class_of(uint64_t) { for (const auto& entry:objects) if (entry.type=="Class") return entry.address; return 0; }
bool is_a(uint64_t ai,ClassRef& type) {
    auto* owner=owner_ai(ai); if (!owner) return false;
    if (!std::strcmp(type.name,"ObjectAIComponent")) return true;
    if (owner->type==type.name) return true;
    return owner->type=="TwinSwitchAIComponent" && !std::strcmp(type.name,"SwitchBaseAIComponent");
}
Name actor_kind(uint64_t actor) { auto found=kinds.find(actor); return found==kinds.end()?Name{uint32_t(to_handle(actor)),0}:found->second; }
Name name_of(uint64_t actor) { return actor_kind(actor); }
bool name_text(Name name,char* buffer,size_t capacity,size_t*) { const char* text=name.index?"GCrackPot_C":"none"; if (std::strlen(text)+1>capacity) return false; std::memcpy(buffer,text,std::strlen(text)+1); return true; }
TSUCHI_Result kind_into(Name name,char* buffer,uint64_t capacity,uint64_t* needed) { const char* text=name.index==77?"CRACKPOT":name.index==78?(content_live?"example.creature:test":"NATIVE_MOD_CID"):name.index?"WORKOBJECT":"none"; *needed=std::strlen(text)+1; if (capacity<*needed) return TSUCHI_CAPACITY; std::memcpy(buffer,text,*needed); return TSUCHI_OK; }
bool added_name(const char* id,Name& out) { if (content_live && !std::strcmp(id,"example.creature:test")) { out={78,0}; return true; } return false; }
uint64_t default_object(uint64_t of_class) { auto found=defaults.find(of_class); return found==defaults.end()?0:found->second; }
void each_object(bool(*visit)(void*,uint64_t),void* context) { for (auto& entry:objects) if (!visit(context,entry.address)) break; }
TSUCHI_Result name_from_utf8(const char*,Name& out) { out={1,0}; return TSUCHI_OK; }
bool utf8_to_units(const char* text,char16_t* buffer,size_t capacity,size_t& count) { count=std::strlen(text); if (count>capacity) return false; for (size_t i=0;i<count;++i) buffer[i]=char16_t(text[i]); return true; }
uint64_t game_framework() { return framework; }
uint64_t game_context() { return context; }
uint64_t loaded_world() { return framework; }
uint64_t part_owner(uint64_t) { return 0; }
void each_actor(bool(*visit)(void*,uint64_t),void* context) { for (auto& entry:objects) if (entry.in_play && is_game_actor(entry.address) && !visit(context,entry.address)) break; }
bool read_life(uint64_t actor,Life& out) { auto* entry=object(actor); if (!entry) return false; out={entry->life,100,0,0}; return true; }
uint64_t life_component(uint64_t actor) { return object(actor)?actor:0; }
bool set_life(uint64_t actor,float life,bool) { auto* entry=object(actor); if (!entry) return false; entry->life=life; return true; }
bool get_transform(uint64_t,Transform& out) { out={}; return true; }
uint64_t from_weak_reference(uint64_t address) { return from_handle(read_or<uint64_t>(address)); }
}
int main() {
    set_thread_probe(thread_probe); note_game_thread();
    auto* mod=mod_create("battle-tools"); auto* other=mod_create("other-tools"); auto* self=mod_create("tsuchi.self"); CHECK(mod&&other&&self);
    framework=memory(0xF00); context=memory(0xB00);
    objects.push_back({99,framework,0,"World"});
    gimmicks_start(); battle_start(self);
    const auto& g=gimmicks::table; const auto& b=battle::table; TSUCHI_Subscription sub{};
    CHECK(g.on_changed(mod,record_gimmick,nullptr,&sub)==TSUCHI_OK);
    CHECK(b.on_phase_changed(mod,record_phase,nullptr,&sub)==TSUCHI_OK); CHECK(b.on_score_changed(mod,record_score,nullptr,&sub)==TSUCHI_OK);
    CHECK(b.on_popped(mod,record_pop,nullptr,&sub)==TSUCHI_OK); CHECK(b.on_bonus_changed(mod,record_bonus,nullptr,&sub)==TSUCHI_OK);
    CHECK(b.on_roulette(mod,record_roulette,nullptr,&sub)==TSUCHI_OK); CHECK(b.on_tug(mod,record_tug,nullptr,&sub)==TSUCHI_OK);
    t::fn::gimmick_work_done=done; t::fn::gimmick_removed=removed; t::fn::gimmick_complete_ui=complete_ui;
    t::fn::gimmick_nav_rebuild=nav_rebuild;
    t::fn::gimmick_dig_finish=dig; t::fn::gimmick_pot_break=pot; t::fn::gimmick_down_remove=down; t::fn::gimmick_door_open=door;
    const auto gate=actor("GateAIComponent"), build=actor("BuildObjectAIComponent"), switch_actor=actor("SwitchBaseAIComponent"), door_actor=actor("TriggerDoorAIComponent"), pot_actor=actor("CrackPotAIComponent"), dig_actor=actor("ExcavationAIComponent"), drain_actor=actor("MizunukiAIComponent"), down_actor=actor("DownFloorAIComponent"), vent_actor=actor("HibaBaseAIComponent");
    auto* gate_obj=object(gate); put(gate_obj->ai+t::gimmick_ai::gate_total,int32_t(3)); put(gate_obj->ai+t::gimmick_ai::gate_left,int32_t(3));
    put(game::read_or<uint64_t>(state_of(gate_obj->ai))+t::gimmick_methods::gate_break.offset,reinterpret_cast<uint64_t>(gate_break));
    auto* build_obj=object(build); put(build_obj->ai+t::gimmick_ai::total_pieces,int32_t(5));
    put(game::read_or<uint64_t>(build_obj->ai)+t::gimmick_methods::apply_pieces.offset,reinterpret_cast<uint64_t>(apply_pieces)); put(game::read_or<uint64_t>(build_obj->ai)+t::gimmick_methods::built.offset,reinterpret_cast<uint64_t>(built));
    auto* switch_obj=object(switch_actor); const auto switch_board=board_of(switch_obj->ai); put(switch_board+t::gimmick_board::enabled,uint8_t(1)); put(switch_board+t::gimmick_board::armed,uint8_t(1));
    put(game::read_or<uint64_t>(state_of(switch_obj->ai))+t::gimmick_methods::switch_press.offset,reinterpret_cast<uint64_t>(press));
    const auto door_ai=object(door_actor)->ai,door_child=memory(0x90); put(state_of(door_ai)+t::gimmick_fsm::child,door_child); put(door_child,base+t::data::gimmick_door_closed_table); put(door_ai+t::gimmick_ai::door_type,uint8_t(1));
    put(object(dig_actor)->ai+t::gimmick_ai::dig_stage,int32_t(2));
    put(game::read_or<uint64_t>(object(drain_actor)->ai)+t::gimmick_methods::drain.offset,reinterpret_cast<uint64_t>(drain));
    uint64_t total=0; TSUCHI_Actor list[10]{}; CHECK(g.list(mod,list,1,&total)==TSUCHI_CAPACITY && total==9 && !list[0].id);
    CHECK(g.list(mod,list,10,&total)==TSUCHI_OK && total==9);
    TSUCHI_Gimmick description{}; CHECK(g.describe(mod,handle(gate),&description)==TSUCHI_OK && description.total==3 && !description.done);
    step(); CHECK(changed==0);
    put(state_of(object(gate)->ai)+t::gimmick_fsm::locked,uint8_t(1)); CHECK(g.break_gate(mod,handle(gate),false)==TSUCHI_BUSY); put(state_of(object(gate)->ai)+t::gimmick_fsm::locked,uint8_t(0));
    CHECK(g.break_gate(mod,handle(gate),false)==TSUCHI_OK); CHECK(g.build(mod,handle(build),true)==TSUCHI_OK); CHECK(g.press(mod,handle(switch_actor))==TSUCHI_OK);
    CHECK(g.open(mod,handle(door_actor),false)==TSUCHI_OK); put(door_ai+t::gimmick_ai::door_open,uint8_t(1));
    const auto wait_name=memory(8); put(wait_name,uint64_t(44)); base=wait_name-t::data::gimmick_door_wait_name; put(door_child+t::ai_state::name,uint64_t(44));
    CHECK(g.close(mod,handle(door_actor))==TSUCHI_OK); base=0x100000000ull;
    CHECK(g.finish(mod,handle(pot_actor))==TSUCHI_OK); CHECK(g.finish(mod,handle(dig_actor))==TSUCHI_OK); CHECK(g.finish(mod,handle(drain_actor))==TSUCHI_OK); CHECK(g.finish(mod,handle(down_actor))==TSUCHI_OK); CHECK(g.finish(mod,handle(vent_actor))==TSUCHI_OK);
    CHECK(g.set_progress(mod,handle(build),2)==TSUCHI_INVALID); step(); CHECK(changed>=6);
    const auto build2=actor("BuildObjectAIComponent"); put(object(build2)->ai+t::gimmick_ai::total_pieces,int32_t(5)); put(game::read_or<uint64_t>(object(build2)->ai)+t::gimmick_methods::apply_pieces.offset,reinterpret_cast<uint64_t>(apply_pieces)); put(game::read_or<uint64_t>(object(build2)->ai)+t::gimmick_methods::built.offset,reinterpret_cast<uint64_t>(built));
    CHECK(g.set_progress(mod,handle(build2),2)==TSUCHI_OK);
    const auto warp_actor=actor("WarpCarryAIComponent"),warp_ai=object(warp_actor)->ai,warp_state=state_of(warp_ai),warp_board=board_of(warp_ai);
    put(warp_state,base+t::data::gimmick_warp_table); put(warp_board+t::gimmick_board::warp_ai,warp_ai); put(warp_board+t::gimmick_board::warp_parameter,warp_ai+t::gimmick_ai::warp_parameter);
    put(game::read_or<uint64_t>(warp_ai)+t::gimmick_methods::apply_pieces.offset,base+t::data::gimmick_warp_stage_function);
    t::fn::gimmick_warp_refresh=warp_refresh; t::fn::gimmick_warp_open=warp_open;
    put(warp_ai+t::gimmick_ai::warp_parameter+t::gimmick_switch_parameter::warp_first,-1.0f); CHECK(g.finish(mod,handle(warp_actor))==TSUCHI_INVALID);
    put(warp_ai+t::gimmick_ai::warp_parameter+t::gimmick_switch_parameter::warp_first,0.0f); CHECK(g.finish(mod,handle(warp_actor))==TSUCHI_OK); CHECK(g.describe(mod,handle(warp_actor),&description)==TSUCHI_OK && description.done && description.progress==2);
    const auto departing=actor("CrackPotAIComponent"); step(); const auto before_departure=changed;
    put(object(departing)->ai+t::gimmick_ai::pot_broken,uint8_t(1));
    for (const auto listener:left_listeners) listener(departing,game::to_handle(departing),ActorLeft::destroyed);
    object(departing)->in_play=false; step(); CHECK(changed==before_departure+1);
    TSUCHI_BattlePhase phase{}; CHECK(b.phase(mod,&phase)==TSUCHI_OK && phase==TSUCHI_BATTLE_PHASE_NONE); int32_t points=0; CHECK(b.score(mod,0,&points)==TSUCHI_NOT_READY);
    const auto manager=memory(0x2D0),clock=memory(0xB0),root=memory(0xE0),vs=memory(0xE0),flow=memory(0xE0);
    put(framework+t::battle_framework::manager,manager); put(manager,base+t::data::battle_manager_table); put(manager+t::battle_manager::running,uint8_t(1));
    put(framework+t::battle_framework::clock,clock); put(clock,base+t::data::clock_table); put(clock+t::battle_clock::minutes,3.0f);
    put(context+t::battle_context::flow,root); put(root+t::battle_flow::child,vs); put(vs,base+t::data::battle_flow_table); put(vs+t::battle_flow::child,flow); put(flow,base+t::data::battle_play_table);
    t::fn::battle_score=read_score; t::fn::battle_add_score=add_score; t::fn::battle_pop_class=pop_class; t::fn::battle_pop_name=pop_class; t::fn::battle_spin=spin; t::fn::battle_tug_flick=flick;
    CHECK(b.phase(mod,&phase)==TSUCHI_OK && phase==TSUCHI_BATTLE_PHASE_RUNNING); step(); CHECK(phases>=1);
    CHECK(b.score(mod,2,&points)==TSUCHI_INVALID); CHECK(b.add_score(mod,0,50)==TSUCHI_OK); CHECK(b.score(mod,0,&points)==TSUCHI_OK && points==50); step(); CHECK(score_events==1);
    TSUCHI_BattleClock time{}; CHECK(b.clock(mod,&time)==TSUCHI_OK && time.limit==180); CHECK(b.set_clock(mod,181)==TSUCHI_INVALID); CHECK(b.set_clock(mod,40)==TSUCHI_OK);
    TSUCHI_BattleStatus status{}; CHECK(b.flow(mod,&status)==TSUCHI_OK && status.stage==TSUCHI_BATTLE_FLOW_PLAYING); CHECK(b.abort(mod)==TSUCHI_OK); CHECK(b.play_again(mod)==TSUCHI_NOT_READY);
    put(flow,base+t::data::battle_results_table); put(flow+t::battle_flow::step,int32_t(3)); CHECK(b.play_again(mod)==TSUCHI_OK && game::read_or<int32_t>(flow+t::battle_flow::step)==10); put(flow+t::battle_flow::step,int32_t(3)); CHECK(b.exit(mod)==TSUCHI_OK);
    CHECK(b.end_match(mod)==TSUCHI_OK && game::read_or<float>(clock+t::battle_clock::elapsed)==180);
    const auto pop=memory(0x70),groups=memory(0x58),rows=memory(0xD0),places=memory(0x650),place_owner=memory(0x10);
    put(manager+t::battle_manager::pops,pop); put(pop+t::battle_pop::groups,groups); put(pop+t::battle_pop::groups+0x10,uint32_t(1)); put(pop+t::battle_pop::groups+0x28,int32_t(1));
    put(groups+t::battle_pop::group+t::battle_pop::rows,rows); put(groups+t::battle_pop::group+t::battle_pop::rows+8,int32_t(1));
    put(groups+t::battle_pop::group+t::battle_pop::places,places); put(groups+t::battle_pop::group+t::battle_pop::places+8,int32_t(1)); put(places+t::battle_pop::owner,place_owner); put(place_owner+t::battle_pop::owner_type,uint8_t(1));
    put(rows+t::battle_pop::countdown,12.0f); put(rows+t::battle_pop::repop,5.0f);
    CHECK(b.hold_outcome(mod,TSUCHI_BATTLE_OUTCOME_POP)==TSUCHI_OK); CHECK(game::read_or<float>(rows+t::battle_pop::countdown)==1.0e9f);
    CHECK(b.hold_outcome(other,TSUCHI_BATTLE_OUTCOME_POP)==TSUCHI_BUSY); bool held=false; CHECK(b.outcome(mod,TSUCHI_BATTLE_OUTCOME_POP,&held)==TSUCHI_OK && held);
    const auto cls=actor("Class"); defaults[cls]=pot_actor; kinds[pot_actor]={77,0};
    CHECK(b.replay_pop(mod,0,0,256,"CRACKPOT",false)==TSUCHI_INVALID);
    CHECK(b.replay_pop(mod,0,0,TSUCHI_POP_KIND_TREASURE,"bad/id",false)==TSUCHI_INVALID);
    CHECK(b.replay_pop(mod,0,0,TSUCHI_POP_KIND_TREASURE,"UNKNOWN_KIND",false)==TSUCHI_NOT_FOUND);
    CHECK(b.replay_pop(mod,0,0,TSUCHI_POP_KIND_TREASURE,"example.missing:kind",false)==TSUCHI_NOT_FOUND);
    CHECK(b.replay_pop(mod,0,0,TSUCHI_POP_KIND_TREASURE,"crackpot",false)==TSUCHI_OK && queued_class==cls); CHECK(b.replay_pop(mod,0,0,TSUCHI_POP_KIND_TREASURE,"CRACKPOT",false)==TSUCHI_BUSY);
    put(places+t::battle_pop::queued,uint8_t(0));
    const auto variant=actor("Class"); defaults[variant]=pot_actor;
    CHECK(b.replay_pop(mod,0,0,TSUCHI_POP_KIND_TREASURE,"CRACKPOT",false)==TSUCHI_UNSUPPORTED && game::read_or<uint8_t>(places+t::battle_pop::queued)==0);
    defaults.erase(variant);
    const auto added=actor("Class"),added_pattern=actor("CrackPotAIComponent"); defaults[added]=added_pattern; kinds[added_pattern]={78,0};
    CHECK(b.replay_pop(mod,0,0,TSUCHI_POP_KIND_TREASURE,"example.creature:test",false)==TSUCHI_OK && queued_class==added); put(places+t::battle_pop::queued,uint8_t(0));
    content_live=false; CHECK(b.replay_pop(mod,0,0,TSUCHI_POP_KIND_TREASURE,"example.creature:test",false)==TSUCHI_NOT_FOUND); content_live=true;
    step(); put(places+t::battle_pop::actor,game::to_handle(pot_actor)); step(); CHECK(popped==1);
    CHECK(b.release_outcome(mod,TSUCHI_BATTLE_OUTCOME_POP)==TSUCHI_OK); CHECK(game::read_or<float>(rows+t::battle_pop::countdown)==12 && game::read_or<float>(rows+t::battle_pop::repop)==5);
    put(manager+t::battle_manager::bonus_phase,uint32_t(1)); put(manager+t::battle_manager::bonus_phases,uint32_t(3));
    CHECK(b.hold_outcome(mod,TSUCHI_BATTLE_OUTCOME_BONUS)==TSUCHI_OK && game::read_or<uint32_t>(manager+t::battle_manager::bonus_phase)==3);
    CHECK(b.release_outcome(mod,TSUCHI_BATTLE_OUTCOME_BONUS)==TSUCHI_OK && game::read_or<uint32_t>(manager+t::battle_manager::bonus_phase)==1);
    // Replaying or advancing a bonus while held must survive another keep and release.
    CHECK(b.hold_outcome(mod,TSUCHI_BATTLE_OUTCOME_BONUS)==TSUCHI_OK);
    put(manager+t::battle_manager::bonus_phase,uint32_t(2));step();
    CHECK(b.release_outcome(mod,TSUCHI_BATTLE_OUTCOME_BONUS)==TSUCHI_OK && game::read_or<uint32_t>(manager+t::battle_manager::bonus_phase)==2);
    CHECK(b.hold_outcome(mod,TSUCHI_BATTLE_OUTCOME_BONUS)==TSUCHI_OK);
    put(manager+t::battle_manager::bonus_phase,uint32_t(1));
    CHECK(b.release_outcome(mod,TSUCHI_BATTLE_OUTCOME_BONUS)==TSUCHI_OK && game::read_or<uint32_t>(manager+t::battle_manager::bonus_phase)==1);
    const auto bonus=memory(0x30); put(manager+t::battle_manager::bonus,bonus); put(manager+t::battle_manager::bonus_max,int32_t(2));
    CHECK(b.replay_bonus(mod,0,TSUCHI_BONUS_THEME_GOLD,0x123456,true,0x789ABC,"none",2,3)==TSUCHI_OK); step(); CHECK(bonuses==1); CHECK(b.replay_bonus(mod,3,TSUCHI_BONUS_THEME_GOLD,0,false,0,"none",2,3)==TSUCHI_INVALID);
    CHECK(b.replay_bonus(mod,0,7,0,false,0,"none",2,3)==TSUCHI_INVALID);
    TSUCHI_BattleBonus bonus_read{}; CHECK(b.bonus(mod,0,&bonus_read)==TSUCHI_OK && bonus_read.theme_data==0x123456 && bonus_read.addition_data==0x789ABC);
    TSUCHI_BattlePop pop_read{}; CHECK(b.pop(mod,0,0,&pop_read)==TSUCHI_OK && !std::strcmp(pop_read.of_class,"CRACKPOT") && pop_read.actor.id==game::to_handle(pot_actor)); CHECK(b.pop(mod,32,0,&pop_read)==TSUCHI_NOT_FOUND);
    const auto roulette=memory(0xB0),slots=memory(8),slot=memory(0x40); put(framework+t::battle_framework::roulette,roulette); put(roulette+t::battle_roulette::slots,slots); put(roulette+t::battle_roulette::count,int32_t(1)); put(slots,slot);
    CHECK(b.hold_outcome(mod,TSUCHI_BATTLE_OUTCOME_ROULETTE)==TSUCHI_OK); CHECK(b.replay_roulette(other,0,TSUCHI_BATTLE_ITEM_BOMB)==TSUCHI_DENIED); CHECK(b.replay_roulette(mod,0,TSUCHI_BATTLE_ITEM_BOMB)==TSUCHI_OK);
    CHECK(roulette_hook(nullptr,reinterpret_cast<void*>(slot))==7); step(); CHECK(roulette_events==1); CHECK(b.release_outcome(mod,TSUCHI_BATTLE_OUTCOME_ROULETTE)==TSUCHI_OK); CHECK(roulette_hook(nullptr,reinterpret_cast<void*>(slot))==9);
    const auto tug=memory(0xE0); put(tug+t::battle_tug::actor,pot_actor); put(tug+t::battle_tug::timer,6.0f);
    CHECK(b.hold_outcome(mod,TSUCHI_BATTLE_OUTCOME_TUG)==TSUCHI_OK); tug_hook(reinterpret_cast<void*>(tug),0.1f); CHECK(game::read_or<float>(tug+t::battle_tug::timer)==1.0e9f);
    CHECK(b.release_outcome(mod,TSUCHI_BATTLE_OUTCOME_TUG)==TSUCHI_OK && game::read_or<float>(tug+t::battle_tug::timer)==6);
    put(tug+t::battle_tug::phase,uint8_t(2)); CHECK(b.hold_outcome(mod,TSUCHI_BATTLE_OUTCOME_TUG)==TSUCHI_OK); CHECK(tug_hook(reinterpret_cast<void*>(tug),0.1f)==0 && game::read_or<uint8_t>(tug+t::battle_tug::phase)==2);
    CHECK(b.replay_tug(mod,handle(pot_actor),1)==TSUCHI_OK); tug_hook(reinterpret_cast<void*>(tug),0.1f); step(); CHECK(tug_events==1); CHECK(b.release_outcome(mod,TSUCHI_BATTLE_OUTCOME_TUG)==TSUCHI_OK);
    CHECK(b.hold_outcome(other,TSUCHI_BATTLE_OUTCOME_POP)==TSUCHI_OK); CHECK(b.hold_outcome(other,TSUCHI_BATTLE_OUTCOME_BONUS)==TSUCHI_OK); CHECK(b.hold_outcome(other,TSUCHI_BATTLE_OUTCOME_ROULETTE)==TSUCHI_OK); CHECK(b.replay_roulette(other,0,TSUCHI_BATTLE_ITEM_BOMB)==TSUCHI_OK);
    put(tug+t::battle_tug::phase,uint8_t(0)); put(tug+t::battle_tug::timer,7.0f); CHECK(b.hold_outcome(other,TSUCHI_BATTLE_OUTCOME_TUG)==TSUCHI_OK); tug_hook(reinterpret_cast<void*>(tug),0.1f);
    mod_stop(other); CHECK(game::read_or<float>(rows+t::battle_pop::countdown)==12 && game::read_or<uint32_t>(manager+t::battle_manager::bonus_phase)==1); CHECK(roulette_hook(nullptr,reinterpret_cast<void*>(slot))==9);
    CHECK(game::read_or<float>(tug+t::battle_tug::timer)==7);
    CHECK(b.hold_outcome(mod,TSUCHI_BATTLE_OUTCOME_POP)==TSUCHI_OK); CHECK(b.hold_outcome(mod,TSUCHI_BATTLE_OUTCOME_BONUS)==TSUCHI_OK); CHECK(b.hold_outcome(mod,TSUCHI_BATTLE_OUTCOME_ROULETTE)==TSUCHI_OK); CHECK(b.hold_outcome(mod,TSUCHI_BATTLE_OUTCOME_TUG)==TSUCHI_OK);
    put(framework+t::battle_framework::manager,uint64_t(0)); step();
    for (uint32_t outcome=0;outcome<4;++outcome) { CHECK(b.outcome(mod,outcome,&held)==TSUCHI_OK && !held); CHECK(b.release_outcome(mod,outcome)==TSUCHI_NOT_FOUND); }
    // Lost battle memory is untouched. A later battle receives its own countdowns.
    CHECK(game::read_or<float>(rows+t::battle_pop::countdown)==1.0e9f && game::read_or<uint32_t>(manager+t::battle_manager::bonus_phase)==3);
    const auto prior_bonuses=bonuses;
    put(framework+t::battle_framework::manager,manager);step();
    CHECK(bonuses==prior_bonuses);
    put(framework+t::battle_framework::manager,uint64_t(0));step();
    thread=2; CHECK(g.describe(mod,handle(gate),&description)==TSUCHI_INVALID); CHECK(b.add_score(mod,0,100)==TSUCHI_INVALID); thread=1;
    const auto original_progress=game::read_or<int32_t>(object(build2)->ai+t::gimmick_ai::pieces); mod_stop(mod); CHECK(game::read_or<int32_t>(object(build2)->ai+t::gimmick_ai::pieces)==original_progress);
    std::printf("gimmicks and battle: %d host assertions passed\n",checks);
}
