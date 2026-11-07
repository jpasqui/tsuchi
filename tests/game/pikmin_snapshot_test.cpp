// Snapshot projection against an opaque native-classifier stand-in. Native
// classification, route timing and serialized saves still require game proof.
#include <cstdio>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>
#include "game.h"
#include "objects.h"
#include "reflect.h"
#include "pikmin_snapshot.h"
namespace {
namespace t = tsuchi::game::table;
int failures = 0, calls = 0;
#define EXPECT(x) do { if (!(x)) { std::printf("%d: %s\n", __LINE__, #x); ++failures; } } while (0)
struct Region { std::unique_ptr<uint8_t[]> data; uint64_t at, size; };
std::deque<Region> regions;
std::set<uint64_t> lives;
std::map<uint64_t,uint64_t> classes;
uint64_t memory(uint64_t size) {
    Region r{std::make_unique<uint8_t[]>(size),0,size};
    r.at = reinterpret_cast<uint64_t>(r.data.get()); memset(r.data.get(),0,size);
    regions.push_back(std::move(r)); return regions.back().at;
}
template<class T> void put(uint64_t at,T v) { memcpy(reinterpret_cast<void*>(at),&v,sizeof v); }
template<class T> T get(uint64_t at) { T v{}; memcpy(&v,reinterpret_cast<void*>(at),sizeof v); return v; }
bool mapped(uint64_t at,uint64_t size) {
    for (const auto& r:regions) if (at>=r.at && at-r.at<=r.size && size<=r.size-(at-r.at)) return true;
    return false;
}
struct Node {
    enum Kind { structure,array,integer } kind=structure;
    std::map<std::string,uint64_t> fields;
    std::vector<uint64_t> rows;
    int64_t value=37;
    bool writable=true;
};
std::map<uint64_t,Node> nodes;
uint64_t next_id=1;
uint64_t make(Node::Kind kind=Node::structure) { auto id=next_id++; nodes[id].kind=kind; return id; }
uint64_t main_address,context,framework,pause,current,partner,ai_class,part_class;
struct Cell { unsigned leaf,mode,player; int32_t count; };
std::map<uint64_t,std::vector<Cell>> classified;
void classify(void* raw,void* object) {
    ++calls;
    const auto actor=reinterpret_cast<uint64_t>(object), scratch=reinterpret_cast<uint64_t>(raw);
    EXPECT(get<int32_t>(scratch+t::pikmin_counter::bank)==0);
    for (unsigned i=0;i<t::pikmin_counter::size;++i) EXPECT(get<uint8_t>(scratch+i)==0);
    const auto color=get<uint8_t>(actor+t::game_actor::color);
    EXPECT(color==19 || color==21);
    for (const auto& c:classified.at(actor)) put(scratch+(c.leaf+1)*t::pikmin_counter::leaf_stride+c.mode*t::pikmin_counter::mode_stride+c.player*t::pikmin_counter::player_stride,c.count);
    // Unused leaf/category/bank cells deliberately contain noise. Category zero
    // is not native Query(0), which aggregates the named stock categories.
    put(scratch+t::pikmin_counter::bank_stride+4,int32_t(999));
    put(scratch+4,int32_t(999));
}
void* select_current(void* players) { EXPECT(reinterpret_cast<uint64_t>(players)==context+t::game_context::players); return reinterpret_cast<void*>(current); }
void* select_partner(void* players) { EXPECT(reinterpret_cast<uint64_t>(players)==context+t::game_context::players); return reinterpret_cast<void*>(partner); }
uint64_t leader(int32_t id) {
    auto actor=memory(t::game_actor::snapshot_playerinfo+8), info=memory(t::snapshot_playerinfo::id+4);
    put(actor+t::game_actor::snapshot_playerinfo,info); put(info+t::snapshot_playerinfo::id,id); lives.insert(actor); return actor;
}
uint64_t actor(unsigned color) {
    auto a=memory(t::game_actor::snapshot_excluded+1), ai=memory(t::ai_component::size), part=memory(t::pikmin_component::size);
    auto action=memory(t::ai_action_root::state+8), state=memory(t::ai_state::name+8);
    lives.insert(a); lives.insert(ai); lives.insert(part); classes[ai]=ai_class; classes[part]=part_class;
    put(a+t::game_actor::color,uint8_t(color)); put(a+t::game_actor::ai,ai); put(a+t::game_actor::pikmin,part);
    put(ai+t::ai_component::actions,action); put(action+t::ai_action_root::state,state);
    put(part+t::pikmin_component::classify_decoration,uint8_t(t::constant::spawn_decoration_none)); put(part+t::pikmin_component::classify_ready,uint64_t(1));
    classified[a]={}; return a;
}
struct Fixture {
    uint64_t story,sum,first,second;
    Fixture() {
        calls=0; classified.clear(); story=make(); sum=make(); lives.insert(story);
        nodes[story].fields["StorySummary"]=sum;
        for (const auto* field:{"FollowPikminNum0","FollowPikminNum1","OnyonPikminNum"}) {
            auto outer=make(Node::array); nodes[sum].fields[field]=outer;
            for (unsigned c=0;c<30;++c) {
                auto row=make(), inner=make(Node::array); nodes[outer].rows.push_back(row); nodes[row].fields["Num"]=inner;
                for(unsigned l=0;l<6;++l) nodes[inner].rows.push_back(make(Node::integer));
            }
        }
        context=memory(t::game_context::players+8); framework=memory(t::game_framework::pikmin_list+t::actor_node::size);
        put(context+t::game_context::snapshot_owner,story); put(context+t::game_context::rule,uint8_t(t::constant::rule_story));
        pause=memory(t::pause_manager::size); auto buffer=memory(32); put(pause+t::pause_manager::counters,buffer);
        put(buffer+18,int8_t(1)); put(buffer+20,int8_t(1));
        current=leader(0); partner=leader(1);
        first=actor(19); second=actor(21);
        ring({actor(0),first,second});
    }
    void ring(std::vector<uint64_t> actors) {
        auto head=framework+t::game_framework::pikmin_list, tail=head;
        for(auto a:actors) {auto node=memory(t::actor_node::size); put(tail+t::actor_node::next,node); put(node+t::actor_node::actor,a); tail=node;}
        put(tail+t::actor_node::next,head);
    }
    uint64_t cell(unsigned side,unsigned color,unsigned leaf=0) {
        auto outer=nodes[sum].fields.at(side?"FollowPikminNum1":"FollowPikminNum0"); auto row=nodes[outer].rows.at(color);
        return nodes[nodes[row].fields.at("Num")].rows.at(leaf);
    }
    int64_t count(unsigned side,unsigned color,unsigned leaf=0) {return nodes[cell(side,color,leaf)].value;}
    TSUCHI_Result run(uint64_t loose=1,uint32_t photon=1) {return tsuchi::pikmin_snapshot_follow(story,loose,photon);}
};
bool same_nodes(const std::map<uint64_t,Node>& before) {
    for(const auto& p:before) if(nodes[p.first].value!=p.second.value || nodes[p.first].fields!=p.second.fields || nodes[p.first].rows!=p.second.rows) return false;
    return true;
}
}
namespace tsuchi::game {
uint64_t main_base() {return main_address;}
bool readable(uint64_t at,uint64_t size) {return mapped(at,size);}
bool writable(uint64_t at,uint64_t size) {return nodes.count(at)?nodes[at].writable:mapped(at,size);}
bool live(uint64_t at) {return lives.count(at)!=0;}
uint64_t game_context() {return context;}
uint64_t game_framework() {return framework;}
uint64_t pause_manager() {return pause;}
uint64_t from_weak_reference(uint64_t at) {auto object=read_or<uint64_t>(at); return live(object)?object:0;}
uint64_t class_of(uint64_t at) {return classes[at];}
bool derives_from(uint64_t type,uint64_t wanted) {return type==wanted;}
uint64_t type_of(uint64_t object) {return nodes.count(object)?object:0;}
TSUCHI_Result find_place(uint64_t,uint64_t base,const char* path,Place& out) {
    std::string name(path); auto bracket=name.find('['); auto field=name.substr(0,bracket);
    if(!nodes.count(base) || !nodes[base].fields.count(field)) return TSUCHI_NOT_FOUND;
    auto id=nodes[base].fields.at(field);
    if(bracket!=std::string::npos) {auto i=unsigned(std::stoul(name.substr(bracket+1))); if(i>=nodes[id].rows.size()) return TSUCHI_NOT_FOUND; id=nodes[id].rows[i];}
    out=Place{id,id,false}; return TSUCHI_OK;
}
TSUCHI_Result array_of(const Place& p,ArrayMemory& out) {
    if(nodes[p.address].kind!=Node::array) return TSUCHI_INVALID;
    out=ArrayMemory{p.address,int32_t(nodes[p.address].rows.size()),int32_t(nodes[p.address].rows.size())}; return TSUCHI_OK;
}
TSUCHI_Result struct_at(const Place& p,uint64_t& type,uint64_t& base) {
    if(nodes[p.address].kind!=Node::structure) return TSUCHI_INVALID; type=base=p.address; return TSUCHI_OK;
}
TSUCHI_Result read_value(const Place& p,Value& value) {
    value.kind=nodes[p.address].kind==Node::integer?ValueKind::integer:ValueKind::none; value.integer=nodes[p.address].value; return TSUCHI_OK;
}
TSUCHI_Result write_value(const Place& p,const Value& value) {
    if(!nodes[p.address].writable || nodes[p.address].kind!=Node::integer) return TSUCHI_INVALID;
    nodes[p.address].value=value.integer; return TSUCHI_OK;
}
}
int main() {
    auto globals=memory(t::data::pikmin_component_class-t::data::pikmin_ai_class+8); main_address=globals-t::data::pikmin_ai_class;
    ai_class=memory(8); part_class=memory(8); lives.insert(ai_class); lives.insert(part_class);
    put(main_address+t::data::pikmin_ai_class,ai_class); put(main_address+t::data::pikmin_component_class,part_class);
    t::fn::pikmin_counter_classify=classify; t::fn::pikmin_snapshot_current=select_current; t::fn::pikmin_snapshot_partner=select_partner;
    {Fixture f;
     classified[f.first]={{0,3,2,2},{0,3,3,3},{0,7,2,9},{0,8,4,1},{0,10,5,1},{0,11,2,1},{1,3,2,1}};
     classified[f.second]={{2,3,3,4}};
     const auto before=nodes;
     std::vector<std::vector<uint8_t>> memory_before; for(const auto& r:regions) memory_before.emplace_back(r.data.get(),r.data.get()+r.size);
     EXPECT(!tsuchi::game::live(context)); EXPECT(f.run()==TSUCHI_OK && calls==2);
     EXPECT(f.count(0,19)==3 && f.count(1,19)==3 && f.count(0,19,1)==1 && f.count(1,21,2)==4);
     for(const auto& p:before) {
        bool allowed=false; for(unsigned side=0;side<2;++side) for(unsigned color:{19u,21u}) for(unsigned leaf=0;leaf<4;++leaf) if(p.first==f.cell(side,color,leaf)) allowed=true;
        if(!allowed) EXPECT(nodes[p.first].value==p.second.value);
        EXPECT(nodes[p.first].rows==p.second.rows && nodes[p.first].fields==p.second.fields);
     }
     unsigned i=0; for(const auto& r:regions) {EXPECT(memcmp(r.data.get(),memory_before[i].data(),r.size)==0); ++i;}
     EXPECT(f.count(0,19,3)==0 && f.count(1,21,3)==0 && f.count(0,19,4)==37);
     current=leader(2); partner=leader(1); EXPECT(f.run(0)==TSUCHI_OK && f.count(1,19)==5 && f.count(0,19)==0);
     current=0; partner=0; EXPECT(f.run(0)==TSUCHI_OK && f.count(0,19)==0 && f.count(1,19)==0);
    }
    {Fixture f; classified[f.first]={{0,7,5,2},{0,11,1,1}}; auto buffer=get<uint64_t>(pause+t::pause_manager::counters);
     put(buffer+18,int8_t(-1)); EXPECT(f.run()==TSUCHI_OK && f.count(0,19)==2);
     put(buffer+18,int8_t(1)); EXPECT(f.run()==TSUCHI_OK && f.count(0,19)==1);
     put(context+t::game_context::rule,uint8_t(7)); EXPECT(f.run(2,0)==TSUCHI_OK && f.count(0,19)==0);
    }
    {Fixture f; classified[f.first]={{0,3,2,100000}}; EXPECT(f.run(0)==TSUCHI_OK && f.count(0,19)==99999);}
    {Fixture f; auto before=nodes; auto ai=get<uint64_t>(f.second+t::game_actor::ai), child=actor(19);
     put(ai+t::ai_component::classify_child,child); EXPECT(f.run()==TSUCHI_NOT_READY && calls==0 && same_nodes(before));
     put(child+t::game_actor::removed,uint8_t(1)); EXPECT(f.run()==TSUCHI_OK && calls==2);
    }
    {Fixture f; auto before=nodes; put(main_address+t::data::pikmin_ai_class,uint64_t(0)); EXPECT(f.run()==TSUCHI_NOT_READY && calls==0 && same_nodes(before)); put(main_address+t::data::pikmin_ai_class,ai_class);}
    {Fixture f; auto before=nodes; put(context+t::game_context::snapshot_owner,uint64_t(0)); EXPECT(f.run()==TSUCHI_NOT_READY && calls==0 && same_nodes(before));}
    {Fixture f; auto before=nodes; put(context+t::game_context::rule,uint8_t(3)); EXPECT(f.run()==TSUCHI_NOT_READY && calls==0 && same_nodes(before));}
    {Fixture f; nodes[f.cell(1,21,3)].writable=false; auto before=nodes; EXPECT(f.run()==TSUCHI_NOT_READY && calls==0 && same_nodes(before));}
    {Fixture f; auto outer=nodes[f.sum].fields.at("FollowPikminNum1"); nodes[outer].rows.resize(21); auto before=nodes; EXPECT(f.run()!=TSUCHI_OK && calls==0 && same_nodes(before));}
    {Fixture f; nodes[f.cell(1,21,3)].kind=Node::structure; auto before=nodes; EXPECT(f.run()==TSUCHI_INVALID && calls==0 && same_nodes(before));}
    {Fixture f; auto outer=nodes[f.sum].fields.at("FollowPikminNum1"), row=nodes[outer].rows[21], inner=nodes[row].fields.at("Num"); nodes[inner].rows.resize(3); auto before=nodes; EXPECT(f.run()==TSUCHI_NOT_READY && calls==0 && same_nodes(before));}
    {Fixture f; put(f.first+t::game_actor::kill_queued,uint8_t(1)); EXPECT(f.run()==TSUCHI_OK && calls==1 && f.count(0,19)==0);}
    {Fixture f; put(f.first+t::game_actor::rest,int32_t(1)); EXPECT(f.run()==TSUCHI_OK && calls==1 && f.count(0,19)==0);}
    {Fixture f; put(f.first+t::game_actor::rest,int32_t(t::constant::pikmin_snapshot_rest_second)); EXPECT(f.run()==TSUCHI_OK && calls==2);}
    {Fixture f; auto part=get<uint64_t>(f.first+t::game_actor::pikmin); put(part+t::pikmin_component::classify_decoration,uint8_t(2)); put(part+t::pikmin_component::classify_skip,uint8_t(1)); EXPECT(f.run()==TSUCHI_OK && calls==1 && f.count(0,19)==0);}
    {Fixture f; auto node=memory(t::actor_node::size); put(node+t::actor_node::actor,f.second); put(node+t::actor_node::next,uint64_t(0));
     auto head=framework+t::game_framework::pikmin_list; put(head+t::actor_node::next,node); auto before=nodes; EXPECT(f.run()==TSUCHI_NOT_READY && calls==0 && same_nodes(before));}
    std::printf("Pikmin native snapshot projection: %d failures\n",failures); return failures?1:0;
}
