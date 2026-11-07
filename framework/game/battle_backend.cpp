// Dandori Battle. Random-outcome state belongs to the shared hold tables.
#include <string.h>
#include "actors.h"
#include "battle_service.h"
#include "content.h"
#include "flow.h"
#include "frame.h"
#include "game.h"
#include "holds.h"
#include "hooks.h"
#include "list.h"
#include "objects.h"
#include "reflect.h"

namespace tsuchi {
namespace {
namespace t=game::table;
namespace m=t::battle_manager;
namespace k=t::battle_clock;
namespace p=t::battle_pop;
namespace f=t::battle_flow;
namespace u=t::battle_tug;
constexpr float held_time=1.0e9f;
uint64_t manager() {
    const uint64_t gf=game::game_framework();
    const uint64_t value=gf?game::read_or<uint64_t>(gf+t::battle_framework::manager):0;
    return value && game::readable(value,m::size) && game::read_or<uint64_t>(value)==game::main_base()+t::data::battle_manager_table?value:0;
}
uint64_t clock_object() {
    if (!manager()) return 0;
    const uint64_t value=game::read_or<uint64_t>(game::game_framework()+t::battle_framework::clock);
    return value && game::readable(value,k::size) && game::read_or<uint64_t>(value)==game::main_base()+t::data::clock_table?value:0;
}
TSUCHI_Result phase(TSUCHI_BattlePhase* out) {
    const uint64_t value=manager(); *out=TSUCHI_BATTLE_PHASE_NONE;
    if (value) *out=!game::read_or<uint8_t>(value+m::running)?TSUCHI_BATTLE_PHASE_PREPARED:
        game::read_or<uint8_t>(clock_object()+k::time_up)?TSUCHI_BATTLE_PHASE_TIME_UP:TSUCHI_BATTLE_PHASE_RUNNING;
    return TSUCHI_OK;
}
TSUCHI_Result score(uint32_t player,int32_t* out) {
    if (player>1) return TSUCHI_INVALID;
    const auto value=manager(); if (!value || !t::bound(t::fn::battle_score)) return TSUCHI_NOT_READY;
    *out=t::fn::battle_score(reinterpret_cast<void*>(value),int32_t(player)); return TSUCHI_OK;
}
TSUCHI_Result add_score(uint32_t player,int32_t points) {
    if (player>1) return TSUCHI_INVALID;
    const auto value=manager(); if (!value || !t::bound(t::fn::battle_add_score) || !game::read_or<uint8_t>(value+m::running)) return TSUCHI_NOT_READY;
    t::fn::battle_add_score(reinterpret_cast<void*>(value),int32_t(player),points); return TSUCHI_OK;
}
TSUCHI_Result clock(TSUCHI_BattleClock* out) {
    const auto value=clock_object(); if (!value) return TSUCHI_NOT_READY;
    *out={game::read_or<float>(value+k::elapsed),game::read_or<float>(value+k::minutes)*60,game::read_or<uint8_t>(value+k::time_up)!=0}; return TSUCHI_OK;
}
TSUCHI_Result set_clock(float seconds) {
    const auto value=clock_object(); if (!value) return TSUCHI_NOT_READY;
    if (!(seconds>=0) || seconds>game::read_or<float>(value+k::minutes)*60) return TSUCHI_INVALID;
    return game::write(value+k::elapsed,seconds)?TSUCHI_OK:TSUCHI_NOT_READY;
}
uint64_t flow_root() {
    const uint64_t context=game::game_context(); if (!context) return 0;
    const uint64_t root=game::read_or<uint64_t>(context+t::battle_context::flow);
    const uint64_t value=root?game::read_or<uint64_t>(root+f::child):0;
    return value && game::readable(value,f::size) && game::read_or<uint64_t>(value)==game::main_base()+t::data::battle_flow_table?value:0;
}
TSUCHI_Result flow(TSUCHI_BattleStatus* out) {
    *out={TSUCHI_BATTLE_FLOW_NONE,-1,false,false};
    const auto root=flow_root(); if (!root) return TSUCHI_OK;
    out->aborted=game::read_or<uint8_t>(root+f::aborted)!=0; out->play_again=game::read_or<uint8_t>(root+f::retry)!=0;
    const auto child=game::read_or<uint64_t>(root+f::child); if (!child || !game::readable(child,f::size)) return TSUCHI_NOT_READY;
    const auto table=game::read_or<uint64_t>(child)-game::main_base();
    out->stage=table==t::data::battle_load_table || table==t::data::battle_demo_table?TSUCHI_BATTLE_FLOW_LOADING:
        table==t::data::battle_start_table?TSUCHI_BATTLE_FLOW_COUNTDOWN:table==t::data::battle_play_table?TSUCHI_BATTLE_FLOW_PLAYING:
        table==t::data::battle_results_table?TSUCHI_BATTLE_FLOW_RESULTS:table==t::data::battle_end_table?TSUCHI_BATTLE_FLOW_CLOSING:TSUCHI_BATTLE_FLOW_OTHER;
    if (out->stage==TSUCHI_BATTLE_FLOW_RESULTS || out->stage==TSUCHI_BATTLE_FLOW_CLOSING) out->step=game::read_or<int32_t>(child+f::step);
    return TSUCHI_OK;
}
TSUCHI_Result end_match() {
    TSUCHI_BattlePhase current{}; phase(&current); if (current!=TSUCHI_BATTLE_PHASE_RUNNING) return TSUCHI_NOT_READY;
    const auto value=clock_object(); if (!value) return TSUCHI_NOT_READY;
    if (game::read_or<uint8_t>(value+k::unlimited)) {
        if (!game::writable(value+k::time_up,1) || !game::writable(value+k::stopped,1)) return TSUCHI_NOT_READY;
        game::write(value+k::time_up,uint8_t(1)); game::write(value+k::stopped,uint8_t(1)); return TSUCHI_OK;
    }
    return set_clock(game::read_or<float>(value+k::minutes)*60);
}
TSUCHI_Result abort_match() {
    TSUCHI_BattleStatus state{}; flow(&state); if (state.stage!=TSUCHI_BATTLE_FLOW_PLAYING) return TSUCHI_NOT_READY;
    return game::write(flow_root()+f::aborted,uint8_t(1))?TSUCHI_OK:TSUCHI_NOT_READY;
}
TSUCHI_Result choose(bool again) {
    const auto root=flow_root(), child=root?game::read_or<uint64_t>(root+f::child):0;
    if (!child || game::read_or<uint64_t>(child)!=game::main_base()+t::data::battle_results_table || game::read_or<int32_t>(child+f::step)!=int32_t(t::constant::battle_results_ready_step)) return TSUCHI_NOT_READY;
    if (!game::writable(child+f::choice,1) || !game::writable(child+f::step,4)) return TSUCHI_NOT_READY;
    game::write(child+f::choice,uint8_t(again?0:1)); game::write(child+f::step,int32_t(t::constant::battle_results_resume_step)); return TSUCHI_OK;
}
TSUCHI_Result play_again() { return choose(true); }
TSUCHI_Result exit_match() { return choose(false); }
struct Array { uint64_t data; int32_t count; };
Array array(uint64_t at,uint64_t stride,uint32_t most) {
    const auto data=game::read_or<uint64_t>(at); const auto count=game::read_or<int32_t>(at+8,-1);
    if (count<0 || uint32_t(count)>most || (count && (!data || !game::readable(data,stride*uint32_t(count))))) return {};
    return {data,count};
}
template<class Visit> bool groups(uint64_t owner,Visit visit) {
    const uint64_t pop=game::read_or<uint64_t>(owner+m::pops); if (!pop) return false;
    const uint64_t base=pop+p::groups, data=game::read_or<uint64_t>(base), heap=game::read_or<uint64_t>(base+p::bit_heap);
    const int32_t count=game::read_or<int32_t>(base+p::bit_count,-1); const uint64_t words=heap?heap:base+p::bit_inline;
    if (count<0 || count>32 || (count && (!data || !game::readable(data,p::stride*uint32_t(count)) || !game::readable(words,4*((uint32_t(count)+31)/32))))) return false;
    for (int32_t i=0;i<count;++i) if ((game::read_or<uint32_t>(words+4*(uint32_t(i)/32))>>(i%32))&1u) visit(uint32_t(i),data+p::stride*uint32_t(i)+p::group);
    return true;
}
uint64_t place(uint64_t owner,uint32_t group,uint32_t index) {
    uint64_t found=0; groups(owner,[&](uint32_t which,uint64_t data) {
        if (which!=group) return;
        const Array entries=array(data+p::places,p::place_stride,128);
        if (index<uint32_t(entries.count)) found=entries.data+p::place_stride*index;
    }); return found;
}
struct Saved { uint64_t row; float countdown,repop; };
struct TugTimer { uint64_t state,actor; float timer; int32_t decision; };
struct State {
    List<Saved> rows;
    List<TugTimer> tugs;
    uint32_t bonus_phase, paused_phase;
    bool bonus_paused;
    uint32_t queued[2][8]; uint8_t queue_head[2],queue_count[2];
};
struct Kept { uint64_t manager,world; State* state; };
bool same_battle(const Kept& kept) { return manager()==kept.manager && game::from_handle(kept.world)==game::loaded_world(); }
uint32_t (*next_tug)(void*,float)=nullptr;
uint32_t (*next_roulette)(void*,void*)=nullptr;
bool row_present(uint64_t owner,uint64_t wanted) {
    bool found=false; groups(owner,[&](uint32_t,uint64_t group) {
        const Array rows=array(group+p::rows,p::row_stride,256);
        if (wanted>=rows.data && (wanted-rows.data)%p::row_stride==0 && (wanted-rows.data)/p::row_stride<uint32_t(rows.count)) found=true;
    }); return found;
}
void restore(const Hold& hold,Kept& kept,HoldEnd) {
    State* state=kept.state; if (!state) return;
    if (same_battle(kept)) {
        if (hold.what==TSUCHI_BATTLE_OUTCOME_POP) for (uint32_t i=0;i<state->rows.count;++i) { const auto& row=state->rows[i]; if (row_present(kept.manager,row.row)) {
            game::write(row.row+p::countdown,row.countdown); game::write(row.row+p::repop,row.repop);
        } }
        if (hold.what==TSUCHI_BATTLE_OUTCOME_BONUS && state->bonus_paused &&
            game::read_or<uint32_t>(kept.manager+m::bonus_phase)==state->paused_phase)
            game::write(kept.manager+m::bonus_phase,state->bonus_phase);
        if (hold.what==TSUCHI_BATTLE_OUTCOME_TUG) for (uint32_t i=0;i<state->tugs.count;++i) { const auto& entry=state->tugs[i];
            const auto actor=game::from_handle(entry.actor);
            if (actor && game::read_or<uint64_t>(entry.state+u::actor)==actor && game::read_or<uint8_t>(entry.state+u::phase)==0) game::write(entry.state+u::timer,entry.timer);
        }
    }
    state->rows.clear(); state->tugs.clear(); release(state); kept.state=nullptr;
}
bool keep(const Hold& hold,Kept& kept) {
    if (!kept.state || !same_battle(kept)) return false;
    if (hold.what==TSUCHI_BATTLE_OUTCOME_BONUS) {
        auto& state=*kept.state;
        const auto current=game::read_or<uint32_t>(kept.manager+m::bonus_phase);
        if (!state.bonus_paused || current!=state.paused_phase) state.bonus_phase=current;
        state.paused_phase=game::read_or<uint32_t>(kept.manager+m::bonus_phases);
        state.bonus_paused=game::write(kept.manager+m::bonus_phase,state.paused_phase);
        return state.bonus_paused;
    }
    if (hold.what!=TSUCHI_BATTLE_OUTCOME_POP) return true;
    bool valid=true;
    const bool walked=groups(kept.manager,[&](uint32_t,uint64_t group) {
        const Array rows=array(group+p::rows,p::row_stride,256);
        for (int32_t i=0;i<rows.count;++i) {
            const auto row=rows.data+p::row_stride*uint32_t(i); bool known=false;
            for (uint32_t n=0;n<kept.state->rows.count;++n) if (kept.state->rows[n].row==row) known=true;
            if (!game::writable(row+p::countdown,4) || !game::writable(row+p::repop,4)) { valid=false; continue; }
            if (!known && !kept.state->rows.add({row,game::read_or<float>(row+p::countdown),game::read_or<float>(row+p::repop)})) { valid=false; continue; }
            game::write(row+p::countdown,held_time); game::write(row+p::repop,held_time);
        }
    }); return walked && valid;
}
TSUCHI_Result apply(const Hold& hold,Kept& kept,const HoldNothing&,bool fresh) {
    if (!fresh) return TSUCHI_OK;
    kept.manager=manager(); if (!kept.manager) return TSUCHI_NOT_READY;
    kept.world=game::to_handle(game::loaded_world()); if (!kept.world) return TSUCHI_NOT_READY;
    if ((hold.what==TSUCHI_BATTLE_OUTCOME_TUG && !next_tug) || (hold.what==TSUCHI_BATTLE_OUTCOME_ROULETTE && !next_roulette)) return TSUCHI_UNSUPPORTED;
    kept.state=static_cast<State*>(allocate(sizeof(State))); if (!kept.state) return TSUCHI_NO_MEMORY;
    memset(kept.state,0,sizeof(State)); kept.state->bonus_phase=game::read_or<uint32_t>(kept.manager+m::bonus_phase);
    if (!keep(hold,kept)) { restore(hold,kept,HoldEnd::lost); return TSUCHI_NOT_READY; }
    return TSUCHI_OK;
}
Holds<Kept,HoldNothing,apply,keep,restore> outcomes{"battle.outcome",HoldOn::nothing,true};
TSUCHI_Result hold_outcome(TSUCHI_Mod* mod,TSUCHI_BattleOutcome outcome) { return outcomes.take(mod,0,outcome,{}); }
TSUCHI_Result release_outcome(TSUCHI_Mod* mod,TSUCHI_BattleOutcome outcome) { return outcomes.release(mod,0,outcome); }
TSUCHI_Result outcome(TSUCHI_BattleOutcome value,bool* out) { *out=outcomes.find(0,value)!=nullptr; return TSUCHI_OK; }
Kept* active(TSUCHI_BattleOutcome value) { auto* kept=outcomes.find(0,value); return kept && same_battle(*kept)?kept:nullptr; }
struct ClassSearch { const char* id; game::Name added; bool is_added; uint64_t found; bool ambiguous; };
bool find_kind_class(void* context,uint64_t candidate) {
    auto& search=*static_cast<ClassSearch*>(context);
    if (!game::is_class(candidate)) return true;
    const uint64_t pattern=game::default_object(candidate);
    if (!pattern || !game::is_game_actor(pattern)) return true;
    const game::Name kind=game::actor_kind(pattern); if (!kind.index) return true;
    bool matches=false;
    if (search.is_added) matches=kind.index==search.added.index && kind.number==search.added.number;
    else { char id[kIdBytes]; uint64_t needed=0; matches=game::kind_into(kind,id,sizeof id,&needed)==TSUCHI_OK && id_same(id,search.id); }
    if (!matches) return true;
    if (search.found && search.found!=candidate) { search.ambiguous=true; return false; }
    search.found=candidate; return true;
}
TSUCHI_Result class_for_kind(const char* id,uint64_t& out) {
    ClassSearch search{id,{},false,0,false};
    const auto form=id_form(id); if (form==IdForm::malformed) return TSUCHI_INVALID;
    if (form==IdForm::placeholder) return TSUCHI_NOT_FOUND;
    if (form==IdForm::added) {
        if (!game::added_name(id,search.added)) return TSUCHI_NOT_FOUND;
        search.is_added=true;
    }
    game::each_object(find_kind_class,&search);
    if (search.ambiguous) return TSUCHI_UNSUPPORTED;
    if (!search.found) return TSUCHI_NOT_FOUND;
    out=search.found; return TSUCHI_OK;
}
TSUCHI_Result replay_pop(uint32_t group,uint32_t index,TSUCHI_PopKind kind,const char* of_class,bool alternate) {
    const auto owner=manager(); if (!owner) return TSUCHI_NOT_READY;
    if (kind==TSUCHI_POP_KIND_UNDEF || kind==TSUCHI_POP_KIND_NONE) return TSUCHI_UNSUPPORTED;
    const uint32_t number=uint32_t(kind);
    uint64_t cls=0; const auto resolved=class_for_kind(of_class,cls); if (resolved!=TSUCHI_OK) return resolved;
    const auto at=place(owner,group,index); if (!at) return TSUCHI_NOT_FOUND;
    if (game::read_or<uint8_t>(at+p::queued) || game::from_weak_reference(at+p::actor)) return TSUCHI_BUSY;
    const auto place_owner=game::read_or<uint64_t>(at+p::owner); if (!place_owner || !t::bound(t::fn::battle_pop_class) || !t::bound(t::fn::battle_pop_name)) return TSUCHI_NOT_READY;
    const auto type=game::read_or<uint8_t>(place_owner+p::owner_type,255);
    if (type<t::constant::battle_pop_owner_types && ((t::constant::battle_pop_class_owner_mask>>type)&1u)) t::fn::battle_pop_class(reinterpret_cast<void*>(at),number,reinterpret_cast<void*>(cls),alternate?1:0);
    else {
        char name[game::kNameBytes]; if (!game::name_text(game::name_of(cls),name,sizeof name)) return TSUCHI_NOT_FOUND;
        size_t size=strlen(name); if (size>=2 && name[size-2]=='_' && name[size-1]=='C') size-=2;
        if (!size || size>=96) return TSUCHI_UNSUPPORTED;
        char16_t wide[96]; for (size_t i=0;i<size;++i) { if (uint8_t(name[i])>127) return TSUCHI_UNSUPPORTED; wide[i]=char16_t(name[i]); } wide[size]=0;
        struct String { const char16_t* data; int32_t size,capacity; } text{wide,int32_t(size+1),int32_t(size+1)};
        t::fn::battle_pop_name(reinterpret_cast<void*>(at),number,&text,alternate?1:0);
    }
    return TSUCHI_OK;
}
TSUCHI_Result replay_tug(TSUCHI_Mod* mod,TSUCHI_Actor actor,int32_t winner) {
    if (winner<-1 || winner>1) return TSUCHI_INVALID;
    if (!game::from_handle(actor.id)) return TSUCHI_NOT_FOUND;
    auto* held=active(TSUCHI_BATTLE_OUTCOME_TUG); if (!held) return TSUCHI_NOT_READY;
    if (outcomes.holder(0,TSUCHI_BATTLE_OUTCOME_TUG)!=mod) return TSUCHI_DENIED;
    for (uint32_t i=0;i<held->state->tugs.count;++i) { auto& tug=held->state->tugs[i]; if (tug.actor==actor.id) { tug.decision=winner; return TSUCHI_OK; } }
    return TSUCHI_NOT_READY;
}
struct Bonus { uint8_t theme[4]; uint32_t cid[2]; uint8_t addition[4]; int32_t multiplier[2]; };
static_assert(sizeof(Bonus)==0x18,"native Bonus Find layout");
TSUCHI_Result replay_bonus(uint32_t index,TSUCHI_BonusTheme theme,uint32_t theme_data,bool addition,uint32_t addition_data,const char* kind,int32_t first,int32_t second) {
    if (index>=16 || first<0 || first>100 || second<0 || second>100 || theme_data>0xFFFFFF || addition_data>0xFFFFFF) return TSUCHI_INVALID;
    const uint64_t owner=manager(); if (!owner) return TSUCHI_NOT_READY;
    game::Name target{};
    if (!id_same(kind,"none") && !game::added_name(kind,target)) {
        if (id_form(kind)!=IdForm::plain) return TSUCHI_NOT_FOUND;
        // Find-only FName lookup. A typo must not manufacture a new target name.
        if (!t::bound(t::fn::name_from_text)) return TSUCHI_NOT_READY;
        char16_t wide[kIdBytes]; size_t units=0;
        if (!game::utf8_to_units(kind,wide,kIdBytes,units)) return TSUCHI_INVALID;
        const uint64_t found=t::fn::name_from_text(wide,int32_t(units),0);
        if (!found) return TSUCHI_NOT_FOUND;
        target={uint32_t(found),uint32_t(found>>32)};
    }
    const int32_t count=game::read_or<int32_t>(owner+m::bonus_count,-1);
    if (count<0 || count>16 || index>uint32_t(count)) return TSUCHI_INVALID;
    // Appending through the old grow routine changes Num before allocation and cannot report failure.
    // Refuse when the native list has no existing capacity so a failed call changes nothing.
    if (index==uint32_t(count) && count>=game::read_or<int32_t>(owner+m::bonus_max)) return TSUCHI_CAPACITY;
    const auto data=game::read_or<uint64_t>(owner+m::bonus);
    if (!data || !game::writable(data+sizeof(Bonus)*index,sizeof(Bonus)) || !game::writable(owner+m::bonus_refresh,1) || !game::writable(owner+m::bonus_count,4)) return TSUCHI_NOT_READY;
    Bonus record{}; record.theme[0]=uint8_t(theme); record.addition[0]=addition?1:0;
    for (unsigned i=1;i<4;++i) { record.theme[i]=uint8_t(theme_data>>(8*(i-1))); record.addition[i]=uint8_t(addition_data>>(8*(i-1))); }
    record.cid[0]=target.index; record.cid[1]=target.number; record.multiplier[0]=first; record.multiplier[1]=second;
    game::write(data+sizeof(Bonus)*index,record); if (index==uint32_t(count)) game::write(owner+m::bonus_count,count+1);
    game::write(owner+m::bonus_refresh,uint8_t(1)); return TSUCHI_OK;
}
TSUCHI_Result get_pop(uint32_t group,uint32_t index,TSUCHI_BattlePop* out) {
    const auto owner=manager(); if (!owner) return TSUCHI_NOT_READY;
    const auto at=place(owner,group,index); if (!at) return TSUCHI_NOT_FOUND;
    *out={}; out->group=group; out->place=index;
    out->kind=TSUCHI_PopKind(game::read_or<uint8_t>(at+p::kind));
    const auto actor=game::from_weak_reference(at+p::actor);
    out->actor={actor?game::to_handle(actor):0};
    if (actor) { uint64_t needed=0; const auto result=game::kind_into(game::actor_kind(actor),out->of_class,sizeof out->of_class,&needed); if (result!=TSUCHI_OK) return result; }
    out->alternate=(game::read_or<uint8_t>(at+p::flag)&1)!=0;
    return TSUCHI_OK;
}
TSUCHI_Result get_bonus(uint32_t index,TSUCHI_BattleBonus* out) {
    if (index>=16) return TSUCHI_INVALID;
    const auto owner=manager(); if (!owner) return TSUCHI_NOT_READY;
    const auto entries=array(owner+m::bonus,sizeof(Bonus),16); if (index>=uint32_t(entries.count)) return TSUCHI_NOT_FOUND;
    Bonus record{}; if (!game::read(entries.data+sizeof(Bonus)*index,record)) return TSUCHI_NOT_READY;
    *out={}; out->index=index;
    out->theme=TSUCHI_BonusTheme(record.theme[0]); out->addition=record.addition[0]!=0;
    for (unsigned i=1;i<4;++i) { out->theme_data|=uint32_t(record.theme[i])<<(8*(i-1)); out->addition_data|=uint32_t(record.addition[i])<<(8*(i-1)); }
    if (record.cid[0] || record.cid[1]) { uint64_t needed=0; const auto result=game::kind_into({record.cid[0],record.cid[1]},out->kind,sizeof out->kind,&needed); if (result!=TSUCHI_OK) return result; }
    else memcpy(out->kind,"none",5);
    out->first=record.multiplier[0]; out->second=record.multiplier[1]; return TSUCHI_OK;
}
uint64_t roulette_slot(uint32_t player) {
    const auto system=game::read_or<uint64_t>(game::game_framework()+t::battle_framework::roulette);
    if (!system) return 0;
    const auto slots=array(system+t::battle_roulette::slots,8,4); if (!slots.count) return 0;
    const auto index=player<uint32_t(slots.count)?player:uint32_t(slots.count-1);
    const auto value=game::read_or<uint64_t>(slots.data+8*index);
    return value && game::readable(value,t::constant::battle_roulette_slot_bytes)?value:0;
}
TSUCHI_Result replay_roulette(TSUCHI_Mod* mod,uint32_t player,TSUCHI_BattleItem item) {
    if (player>1) return TSUCHI_INVALID;
    const uint32_t number=uint32_t(item);
    auto* held=active(TSUCHI_BATTLE_OUTCOME_ROULETTE); if (!held) return TSUCHI_NOT_READY;
    if (outcomes.holder(0,TSUCHI_BATTLE_OUTCOME_ROULETTE)!=mod) return TSUCHI_DENIED;
    const auto slot=roulette_slot(player); if (!slot || !t::bound(t::fn::battle_spin)) return TSUCHI_NOT_READY;
    auto& state=*held->state; if (state.queue_count[player]>=8) return TSUCHI_CAPACITY;
    state.queued[player][(state.queue_head[player]+state.queue_count[player])%8]=number; ++state.queue_count[player];
    t::fn::battle_spin(reinterpret_cast<void*>(slot)); return TSUCHI_OK;
}
struct Draw { uint32_t player,item; };
Draw draws[16]; uint32_t draw_count=0;
struct Decision { uint64_t actor; int32_t winner; };
Decision decisions[16]; uint32_t decision_count=0;
uint32_t roulette_draw(void* instance,void* slot) {
    if (!on_game_thread()) return next_roulette(instance,slot);
    const uint32_t player=game::read_or<uint32_t>(reinterpret_cast<uint64_t>(slot)+t::battle_roulette::team)?1u:0u;
    auto* held=active(TSUCHI_BATTLE_OUTCOME_ROULETTE); uint32_t item=0;
    if (held && held->state->queue_count[player]) {
        auto& state=*held->state; item=state.queued[player][state.queue_head[player]];
        state.queue_head[player]=(state.queue_head[player]+1)%8; --state.queue_count[player];
    } else item=next_roulette(instance,slot);
    if (draw_count<16) draws[draw_count++]={player,item}; return item;
}
uint32_t tug_update(void* object,float seconds) {
    if (!on_game_thread()) return next_tug(object,seconds);
    const auto state=reinterpret_cast<uint64_t>(object), actor=game::read_or<uint64_t>(state+u::actor), handle=game::to_handle(actor);
    const uint8_t before=game::read_or<uint8_t>(state+u::phase,255);
    auto* held=active(TSUCHI_BATTLE_OUTCOME_TUG); int32_t applied=-2;
    if (held && handle) {
        TugTimer* entry=nullptr; for (uint32_t i=0;i<held->state->tugs.count;++i) { auto& timer=held->state->tugs[i]; if (timer.state==state && timer.actor==handle) entry=&timer; }
        if (!entry) {
            // A hold must not fall through to a random draw when tracking memory runs out.
            if (!held->state->tugs.add({state,handle,game::read_or<float>(state+u::timer),-2})) return 0;
            entry=&held->state->tugs[held->state->tugs.count-1];
        }
        if (before==0) game::write(state+u::timer,held_time);
        if (before==2) {
            if (entry->decision==-2) return 0;
            applied=entry->decision; entry->decision=-2;
            if (applied>=0 && !t::bound(t::fn::battle_tug_flick)) return 0;
            const auto context=game::game_context();
            game::write(state+u::timer,applied>=0?game::read_or<float>(context+t::battle_context::tug_time):0.0f);
            if (applied>=0) t::fn::battle_tug_flick(object,uint64_t(applied));
            game::write(state+u::phase,uint8_t(3));
        }
    }
    const auto result=next_tug(object,seconds);
    if (game::from_handle(handle)!=actor) return result;
    const auto after=game::read_or<uint8_t>(state+u::phase,255);
    if (applied!=-2 || (before==2 && after==3)) {
        int32_t winner=applied;
        if (winner==-2) {
            const auto holder=game::read_or<uint64_t>(state+u::holder), carry=game::read_or<uint64_t>(holder), info=game::read_or<uint64_t>(carry+u::info);
            const auto first=game::read_or<int32_t>(info+u::strength), second=game::read_or<int32_t>(info+u::strength+4), required=game::read_or<int32_t>(carry+u::required);
            winner=first!=second && first>=required && second>=required?(second>first?1:0):-1;
        }
        if (decision_count<16) decisions[decision_count++]={handle,winner};
    }
    // A chained native update can stop the holder. Resolve the table again after it returns.
    auto* remaining=active(TSUCHI_BATTLE_OUTCOME_TUG);
    if (remaining && (result==1 || result==2)) for (uint32_t i=remaining->state->tugs.count;i--;) if (remaining->state->tugs[i].state==state) remaining->state->tugs.remove(&remaining->state->tugs[i]);
    return result;
}
uint64_t seen_manager=0,seen_places[32][128]{};
Bonus seen_bonus[16]{}; uint32_t seen_bonus_count=0;
TSUCHI_BattlePhase seen_phase=TSUCHI_BATTLE_PHASE_NONE; int32_t seen_score[2]{};
void step() {
    const auto owner=manager(); const bool fresh=owner!=seen_manager;
    if (fresh) { seen_manager=owner; memset(seen_places,0,sizeof seen_places); seen_bonus_count=0; }
    TSUCHI_BattlePhase current{}; phase(&current);
    if (current!=seen_phase) { const auto old=seen_phase; seen_phase=current; battle::emit_phase_changed({current,old}); }
    for (uint32_t player=0;player<2;++player) { int32_t now=0; if (score(player,&now)!=TSUCHI_OK) continue;
        if (now!=seen_score[player]) { const auto old=seen_score[player]; seen_score[player]=now; if (!fresh) battle::emit_score_changed({player,now,old}); }
    }
    if (owner) groups(owner,[&](uint32_t group,uint64_t data) {
        const auto places=array(data+p::places,p::place_stride,128);
        for (int32_t i=0;i<places.count;++i) {
            const auto at=places.data+p::place_stride*uint32_t(i), actor=game::from_weak_reference(at+p::actor), handle=actor?game::to_handle(actor):0;
            if (handle==seen_places[group][i]) continue;
            seen_places[group][i]=handle;
            if (handle && !fresh) { char of_class[kIdBytes]; const auto kind=TSUCHI_PopKind(game::read_or<uint8_t>(at+p::kind));
                uint64_t needed=0; if (game::kind_into(game::actor_kind(actor),of_class,sizeof of_class,&needed)==TSUCHI_OK)
                    battle::emit_popped({group,uint32_t(i),kind,of_class,(game::read_or<uint8_t>(at+p::flag)&1)!=0,{handle}}); }
        }
    });
    if (owner) {
        const auto records=array(owner+m::bonus,sizeof(Bonus),16);
        for (int32_t i=0;i<records.count;++i) {
            Bonus now{}; if (!game::read(records.data+sizeof(Bonus)*uint32_t(i),now)) continue;
            if (uint32_t(i)<seen_bonus_count && !memcmp(&now,&seen_bonus[i],sizeof now)) continue;
            seen_bonus[i]=now; char kind[kIdBytes]="none";
            uint64_t needed=0; if (now.cid[0] || now.cid[1]) game::kind_into({now.cid[0],now.cid[1]},kind,sizeof kind,&needed);
            uint32_t theme_data=0,addition_data=0;
            for (unsigned part=1;part<4;++part) { theme_data|=uint32_t(now.theme[part])<<(8*(part-1)); addition_data|=uint32_t(now.addition[part])<<(8*(part-1)); }
            if (!fresh) battle::emit_bonus_changed({uint32_t(i),TSUCHI_BonusTheme(now.theme[0]),theme_data,now.addition[0]!=0,addition_data,kind,now.multiplier[0],now.multiplier[1]});
        } seen_bonus_count=uint32_t(records.count);
    }
    const uint32_t draws_to_emit=draw_count; draw_count=0;
    for (uint32_t i=0;i<draws_to_emit;++i) battle::emit_roulette({draws[i].player,TSUCHI_BattleItem(draws[i].item)});
    const uint32_t tugs_to_emit=decision_count; decision_count=0;
    for (uint32_t i=0;i<tugs_to_emit;++i) battle::emit_tug({{decisions[i].actor},decisions[i].winner});
}
const battle::Backend service{phase,score,add_score,clock,set_clock,flow,end_match,abort_match,play_again,exit_match,
    hold_outcome,release_outcome,outcome,replay_pop,replay_tug,replay_bonus,replay_roulette,get_pop,get_bonus};
} // namespace
void battle_start(TSUCHI_Mod* self) {
    battle::set_backend(&service); outcomes.start(); each_frame(step);
    uint64_t id=0; hook_slot(self,t::slot::battle_tug,tug_update,&next_tug,0,&id);
    hook_slot(self,t::slot::battle_roulette,roulette_draw,&next_roulette,0,&id);
}
} // namespace tsuchi
