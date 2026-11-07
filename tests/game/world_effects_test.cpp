// Service contracts against a made-up engine, including native reentry and holds.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include "effects_native.h"
#include "effects_service.h"
#include "world_service.h"
#include "engine_call.h"
#include "flow.h"
#include "game.h"
#include "holds.h"
#include "objects.h"
#include "object_retention.h"
#include "actor_events.h"
#include "effects_lua.cpp"
namespace tsuchi { void world_start(); }
using namespace tsuchi;
namespace {
int failures=0,checks=0;
#define CHECK(x) do { ++checks; if (!(x)) { ++failures; std::printf("line %d: %s\n",__LINE__,#x); } } while(0)
std::unordered_set<uint64_t> live_handles;
std::unordered_map<uint64_t,int> roots;
std::vector<void(*)()> steps;
void (*left_listener)(uint64_t,uint64_t,ActorLeft)=nullptr;
void (*ordinary_listener)(uint64_t,uint64_t,bool)=nullptr;
TSUCHI_Result watch_result=TSUCHI_OK;
bool teardown_during_create=false;
bool teardown_during_parameter=false, recurse_parameter=false;
TSUCHI_Mod* recursive_mod=nullptr;
TSUCHI_Effect recursive_effect{};
bool ready=true,begun=true,mesh_loaded=true,projection=true,valid=true,partial=false,break_coordinate=false;
uint64_t world_handle=100,thread=1,next_emitter=1000;
unsigned scans=0,creates=0,stops=0,parameter_calls=0;
uint32_t catalog_entries=3;
uint64_t catalog_revision=1;
unsigned object_scans=0;
TSUCHI_Host lua_host{};
TSUCHI_Mod* lua_mod=nullptr;
TSUCHI_Mod* stop_during_create=nullptr;
TSUCHI_Mod* stop_during_parameter=nullptr;
bool finish_during_create=false,finish_during_parameter=false;
TSUCHI_Result create_result=TSUCHI_OK,parameter_result=TSUCHI_OK;
std::unordered_map<uint64_t,uint64_t> emitters;
std::vector<TSUCHI_EffectsFinished> events;
std::vector<TSUCHI_Vec3> path_points{{1,2,3},{4,5,6},{7,8,9}};
alignas(8) unsigned char nav_actor[4096]{},implementation[1024]{};
uint64_t world_address=200,nav_address=uint64_t(nav_actor);
void tick(){for(auto step:steps)step();}
void arrived(void*,const TSUCHI_EffectsFinished* event){events.push_back(*event);}
void map(uint64_t object){live_handles.insert(object);}
const TSUCHI_Vec3 point{10,20,30},extent{40,50,60};
void configure_mesh(){
    namespace t=game::table;
    uint64_t p=uint64_t(implementation),mesh=mesh_loaded?300:0;
    std::memcpy(nav_actor+t::navmesh_actor::implementation,&p,8);
    std::memcpy(implementation+t::navmesh_implementation::mesh,&mesh,8);
}
}
namespace tsuchi {
bool each_frame(void(*step)()){steps.push_back(step);return true;}
bool on_actor_left(void(*listener)(uint64_t,uint64_t,ActorLeft)){left_listener=listener;return true;}
void ordinary_lifecycle_listener(void(*listener)(uint64_t,uint64_t,bool)){ordinary_listener=listener;}
TSUCHI_Result ordinary_lifecycle_watch(uint64_t){return watch_result;}
uint64_t ordinary_lifecycle_restore_part(uint64_t handle,uint64_t){return game::from_handle(handle);}
bool retain_game_object(uint64_t handle){if(!game::from_handle(handle))return false;++roots[handle];return true;}
void release_game_object(uint64_t handle){if(roots[handle]>0)--roots[handle];}
bool effects_native_ready(){return ready;}
uint64_t effects_native_world(){return begun&&ready?world_handle:0;}
uint64_t effects_native_revision(){return catalog_revision;}
TSUCHI_Result effects_native_catalog(EffectVisit visit,void* context){
    ++scans;
    for(uint32_t i=0;i<catalog_entries;++i){
        EffectAsset asset{i?501u:500u,{}};
        std::memcpy(asset.id,i?"Dust":"Spark",i?5:6);
        if(!visit(context,asset))return TSUCHI_NO_MEMORY;
    }
    return TSUCHI_OK;
}
TSUCHI_Result effects_native_create(uint64_t asset,uint64_t parent,const TSUCHI_Vec3&,uint64_t& emitter,uint64_t& root){
    if(!game::from_handle(asset))return TSUCHI_NOT_FOUND;
    if(parent&&!game::from_handle(parent))return TSUCHI_NOT_FOUND;
    if(parent==999)return TSUCHI_INVALID;
    ++creates;
    if(create_result!=TSUCHI_OK)return create_result;
    emitter=next_emitter++;root=parent?parent+10000:0;map(emitter);emitters[emitter]=parent;
    if(teardown_during_create){teardown_during_create=false;ordinary_listener(parent,parent,true);}
    if(stop_during_create){auto* mod=stop_during_create;stop_during_create=nullptr;mod_stop(mod);}
    if(finish_during_create){finish_during_create=false;effects_finished_native(emitter);}
    return TSUCHI_OK;
}
bool effects_native_alive(uint64_t emitter,uint64_t parent,uint64_t root){
    return game::from_handle(emitter)&&(!parent||(game::from_handle(parent)&&root==parent+10000));
}
void effects_native_stop(uint64_t emitter){if(game::from_handle(emitter)){++stops;live_handles.erase(emitter);}emitters.erase(emitter);}
TSUCHI_Result effects_native_parameter(uint64_t emitter,const char*,TSUCHI_ParameterKind,const TSUCHI_ParticleValue&){
    ++parameter_calls;
    if(stop_during_parameter){
        auto* mod=stop_during_parameter;stop_during_parameter=nullptr;
        const unsigned before=stops;mod_stop(mod);
        CHECK(stops==before&&game::from_handle(emitter));
    }
    if(teardown_during_parameter){
        teardown_during_parameter=false;
        const unsigned before=stops;ordinary_listener(801,801,true);
        CHECK(stops==before&&game::from_handle(emitter));
    }
    if(recurse_parameter){
        recurse_parameter=false;
        const TSUCHI_ParticleValue nested{9,0,0,0};
        CHECK(effects::table.set_parameter(recursive_mod,recursive_effect,"Size",0,&nested)==TSUCHI_BUSY);
    }
    if(finish_during_parameter){finish_during_parameter=false;effects_finished_native(emitter);}
    return parameter_result;
}
void effects_native_start(TSUCHI_Mod*){}
}
namespace tsuchi::game {
uint64_t main_base(){return uint64_t(&world_address)-table::data::world;}
bool readable(uint64_t address,uint64_t){return address!=0;}
bool writable(uint64_t address,uint64_t){return address!=0;}
bool executable(uint64_t){return true;}
uint64_t to_handle(uint64_t object){return live_handles.count(object)?object:0;}
uint64_t from_handle(uint64_t handle){return live_handles.count(handle)?handle:0;}
bool is_game_actor(uint64_t){return true;}
uint64_t part_owner(uint64_t part){return part;}
bool world_begun(){return begun;}
void each_object(bool(*visit)(void*,uint64_t),void* context){++object_scans;visit(context,nav_address);}
bool is_a(uint64_t object,ClassRef&){return object==nav_address;}
uint64_t outer_of(uint64_t object){return object==nav_address?400:object==400?world_address:0;}
uint64_t find_class(ClassRef&){return 600;}
uint64_t type_of(uint64_t){return 600;}
EngineCall::EngineCall(uint64_t object,const char* name):EngineCall(object,name,{}){}
EngineCall::EngineCall(uint64_t object,const char* name,CallGuard guard):object_(object),state_(TSUCHI_OK),guard_(guard){
    function_=std::strcmp(name,"K2_ProjectPointToNavigation")==0?1:
        std::strcmp(name,"FindPathToLocationSynchronously")==0?2:std::strcmp(name,"IsValid")==0?3:4;
}
EngineCall::~EngineCall(){}
TSUCHI_Result EngineCall::set(const char*,const Value&){return guard_.permits()?TSUCHI_OK:TSUCHI_NOT_READY;}
TSUCHI_Result EngineCall::invoke(){return guard_.permits()?TSUCHI_OK:TSUCHI_NOT_READY;}
TSUCHI_Result EngineCall::get(const char* field,Value& out){
    if(!guard_.permits())return TSUCHI_NOT_READY;
    if(std::strcmp(field,"ReturnValue")==0){
        if(function_==2){out=object_value(700);return TSUCHI_OK;}
        out=boolean_value(function_==1?projection:function_==3?valid:partial);return TSUCHI_OK;
    }
    out=number_value(field[std::strlen(field)-1]=='X'?10:field[std::strlen(field)-1]=='Y'?20:25);return TSUCHI_OK;
}
TSUCHI_Result find_place(uint64_t,uint64_t,const char* path,Place& out){
    if(std::strcmp(path,"PathPoints")==0){out.address=1;return TSUCHI_OK;}
    unsigned index=0;const char* at=path+11;
    if(*at<'0'||*at>'9')return TSUCHI_NOT_FOUND;
    while(*at>='0'&&*at<='9')index=index*10+unsigned(*at++-'0');
    if(at[0]!=']'||at[1]!='.'||!at[2]||index>=path_points.size())return TSUCHI_NOT_FOUND;
    const char lane=at[2];
    out.address=100+index*3+(lane=='X'?0:lane=='Y'?1:2);return TSUCHI_OK;
}
TSUCHI_Result read_value(const Place& place,Value& out){
    if(place.address==1){out.kind=ValueKind::array;out.count=uint32_t(path_points.size());return TSUCHI_OK;}
    if(break_coordinate&&place.address==104)return TSUCHI_NOT_READY;
    const uint64_t index=(place.address-100)/3,lane=(place.address-100)%3;
    out=number_value(lane==0?path_points[index].x:lane==1?path_points[index].y:path_points[index].z);return TSUCHI_OK;
}
}

// Only list is called through this binding fixture. Any other helper aborts,
// so an unexpected generated dependency cannot quietly pass the regression.
namespace tsuchi_lua {
TSUCHI_Mod* mod(lua_State*){return lua_mod;}
const TSUCHI_Host* host(lua_State*){return &lua_host;}
int fail(lua_State* L,TSUCHI_Result){lua_pushnil(L);return 1;}
uint64_t check_handle(lua_State*,int,const char*,bool){std::abort();}
TSUCHI_Vec3 check_vec3(lua_State*,int){std::abort();}
void push_handle(lua_State*,const char*,uint64_t){std::abort();}
void push_unnamed(lua_State*,uint32_t){std::abort();}
Listener* listen(lua_State*,int){std::abort();}
int subscribed(lua_State*,Listener*,TSUCHI_Result,TSUCHI_Subscription){std::abort();}
void deliver(Listener*,lua_CFunction,const void*){std::abort();}
}
int main(){
    set_thread_probe([](){return thread;});note_game_thread();
    const uint64_t handles[]={100,200,500,501,502,700,800,999,nav_address};
    for(uint64_t h:handles)map(h);
    configure_mesh();effects_start(nullptr);world_start();tick();
    for(unsigned i=0;i<60;++i)tick();CHECK(scans==0);
    const auto& effects=effects::table;const auto& world=world::table;
    auto* mod=mod_create("effects.owner");auto* other=mod_create("effects.observer");
    TSUCHI_Subscription subscription{};
    CHECK(effects.on_finished(other,arrived,nullptr,&subscription)==TSUCHI_OK);
    char names[100];std::memset(names,0x55,sizeof names);uint64_t n=0;
    CHECK(effects.list(mod,names,16,&n)==TSUCHI_CAPACITY&&n==17&&names[0]==0x55);
    CHECK(effects.list(mod,names,17,&n)==TSUCHI_OK&&n==17&&std::memcmp(names,"Spark\0Dust\0Dust\0\0",17)==0&&names[17]==0x55);
    CHECK(scans==1);TSUCHI_Effect effect{},second{};
    CHECK(effects.play(mod,"bad/id",&point,&effect)==TSUCHI_INVALID);
    CHECK(effects.play(mod,"missing",&point,&effect)==TSUCHI_NOT_FOUND);
    CHECK(effects.play(mod,"Dust",&point,&effect)==TSUCHI_BUSY);
    CHECK(effects.play(mod,"spark",&point,&effect)==TSUCHI_OK&&effect.id);
    CHECK(effects.play_on(mod,"Spark",{800},&point,&second)==TSUCHI_OK);
    CHECK(scans==1&&creates==2&&owned_count(mod)==2);
    TSUCHI_ParticleValue value{2,3,4,5},got{};
    CHECK(effects.parameter(mod,effect,"Size",TSUCHI_PARAMETER_KIND_SCALAR,&got)==TSUCHI_NOT_FOUND);
    CHECK(effects.set_parameter(other,effect,"Size",TSUCHI_PARAMETER_KIND_SCALAR,&value)==TSUCHI_DENIED);
    CHECK(effects.set_parameter(mod,effect,"Size",TSUCHI_PARAMETER_KIND_SCALAR,&value)==TSUCHI_OK);
    CHECK(effects.parameter(mod,effect,"size",TSUCHI_PARAMETER_KIND_SCALAR,&got)==TSUCHI_OK&&got.x==2&&got.y==0&&got.w==0);
    CHECK(effects.set_parameter(mod,effect,"Tint",TSUCHI_PARAMETER_KIND_COLOR,&value)==TSUCHI_OK);
    CHECK(effects.parameter(mod,effect,"Tint",TSUCHI_PARAMETER_KIND_COLOR,&got)==TSUCHI_OK&&got.w==5);
    CHECK(effects.set_parameter(mod,effect,"Vector",TSUCHI_PARAMETER_KIND_VECTOR,&value)==TSUCHI_OK);
    CHECK(effects.parameter(mod,effect,"Vector",TSUCHI_PARAMETER_KIND_VECTOR,&got)==TSUCHI_OK&&got.z==4&&got.w==0);
    thread=2;
    CHECK(effects.stop(mod,effect)==TSUCHI_INVALID);
    CHECK(effects.list(mod,names,sizeof names,&n)==TSUCHI_INVALID);
    CHECK(effects.play(mod,"Spark",&point,&effect)==TSUCHI_INVALID);
    CHECK(effects.play_on(mod,"Spark",{800},&point,&second)==TSUCHI_INVALID);
    CHECK(effects.parameter(mod,effect,"Size",0,&got)==TSUCHI_INVALID);
    CHECK(effects.set_parameter(mod,effect,"Size",0,&value)==TSUCHI_INVALID);thread=1;
    CHECK(effects.stop(other,effect)==TSUCHI_DENIED);
    CHECK(effects.stop(mod,effect)==TSUCHI_OK);tick();
    CHECK(events.size()==1&&events.back().reason==TSUCHI_EFFECT_END_STOPPED);
    CHECK(effects.stop(mod,effect)==TSUCHI_NOT_FOUND);
    left_listener(800,800,ActorLeft::put_away);live_handles.erase(800);tick();
    CHECK(events.size()==2&&events.back().reason==TSUCHI_EFFECT_END_PARENT_LOST);
    map(800);CHECK(effects.play_on(mod,"Spark",{800},&point,&effect)==TSUCHI_OK);
    const uint64_t natural=next_emitter-1;effects_finished_native(natural);effects_finished_native(natural);tick();
    CHECK(events.back().reason==TSUCHI_EFFECT_END_NATURAL&&roots[natural]==0);
    const size_t once=events.size();effects_finished_native(natural);tick();CHECK(events.size()==once);
    CHECK(effects.stop(mod,effect)==TSUCHI_NOT_FOUND);
    CHECK(effects.play(mod,"Spark",&point,&effect)==TSUCHI_OK);
    const uint64_t lost=next_emitter-1;live_handles.erase(lost);tick();CHECK(events.back().reason==TSUCHI_EFFECT_END_LOST);
    CHECK(effects.play_on(mod,"Spark",{800},&point,&effect)==TSUCHI_OK);
    left_listener(800,800,ActorLeft::destroyed);live_handles.erase(800);tick();CHECK(events.back().reason==TSUCHI_EFFECT_END_PARENT_LOST);
    CHECK(effects.play(mod,"Spark",&point,&effect)==TSUCHI_OK);world_handle=101;map(101);tick();
    CHECK(events.back().reason==TSUCHI_EFFECT_END_WORLD_UNLOADED&&scans==1);
    CHECK(effects.play(mod,"Spark",&point,&effect)==TSUCHI_OK);mod_stop(mod);tick();
    CHECK(owned_count(mod)==0&&events.back().reason==TSUCHI_EFFECT_END_STOPPED);
    auto* reentrant=mod_create("effects.reentrant");stop_during_create=reentrant;
    CHECK(effects.play(reentrant,"Spark",&point,&effect)==TSUCHI_NOT_READY);tick();CHECK(owned_count(reentrant)==0);
    auto* parameter_mod=mod_create("effects.parameter");CHECK(effects.play(parameter_mod,"Spark",&point,&effect)==TSUCHI_OK);
    stop_during_parameter=parameter_mod;CHECK(effects.set_parameter(parameter_mod,effect,"Size",0,&value)==TSUCHI_NOT_FOUND);tick();
    auto* final_mod=mod_create("effects.final");CHECK(effects.play(final_mod,"Spark",&point,&effect)==TSUCHI_OK);
    finish_during_parameter=true;CHECK(effects.set_parameter(final_mod,effect,"Size",0,&value)==TSUCHI_NOT_FOUND);tick();
    create_result=TSUCHI_UNSUPPORTED;CHECK(effects.play(final_mod,"Spark",&point,&effect)==TSUCHI_UNSUPPORTED);create_result=TSUCHI_OK;
    map(801);watch_result=TSUCHI_UNSUPPORTED;
    const unsigned unchanged_creates=creates;
    CHECK(effects.play_on(final_mod,"Spark",{801},&point,&effect)==TSUCHI_UNSUPPORTED&&creates==unchanged_creates);
    watch_result=TSUCHI_OK;
    CHECK(effects.play_on(final_mod,"Spark",{801},&point,&effect)==TSUCHI_OK);
    ordinary_listener(801,801,false);tick();CHECK(events.back().reason==TSUCHI_EFFECT_END_PARENT_LOST);
    CHECK(effects.play_on(final_mod,"Spark",{801},&point,&effect)==TSUCHI_OK);
    ordinary_listener(801,801,true);tick();CHECK(events.back().reason==TSUCHI_EFFECT_END_PARENT_LOST);
    teardown_during_create=true;
    const unsigned before_teardown_stops=stops;
    const size_t before_teardown_events=events.size();
    CHECK(effects.play_on(final_mod,"Spark",{801},&point,&effect)==TSUCHI_NOT_READY);
    tick();CHECK(stops==before_teardown_stops+1&&owned_count(final_mod)==0&&events.size()==before_teardown_events);
    CHECK(effects.play_on(final_mod,"Spark",{801},&point,&effect)==TSUCHI_OK);
    teardown_during_parameter=true;
    CHECK(effects.set_parameter(final_mod,effect,"Size",0,&value)==TSUCHI_NOT_FOUND);
    tick();CHECK(events.back().reason==TSUCHI_EFFECT_END_PARENT_LOST&&owned_count(final_mod)==0);
    CHECK(effects.play(final_mod,"Spark",&point,&effect)==TSUCHI_OK);
    recursive_mod=final_mod;recursive_effect=effect;recurse_parameter=true;
    CHECK(effects.set_parameter(final_mod,effect,"Size",0,&value)==TSUCHI_OK);
    CHECK(effects.parameter(final_mod,effect,"Size",0,&got)==TSUCHI_OK&&got.x==2);
    begun=false;tick();CHECK(effects.play(final_mod,"Spark",&point,&effect)==TSUCHI_NOT_READY);
    TSUCHI_Vec3 answer{99,99,99},points[3]{{99,99,99},{99,99,99},{99,99,99}};
    CHECK(world.ground(final_mod,&point,&extent,&answer)==TSUCHI_NOT_READY);
    CHECK(world.path(final_mod,&point,&point,points,3,&n)==TSUCHI_NOT_READY);
    begun=true;mesh_loaded=false;configure_mesh();CHECK(world.ground(final_mod,&point,&extent,&answer)==TSUCHI_NOT_READY);
    const unsigned absent_scans=object_scans;
    for(unsigned i=0;i<100;++i) CHECK(world.ground(final_mod,&point,&extent,&answer)==TSUCHI_NOT_READY);
    CHECK(object_scans==absent_scans);
    mesh_loaded=true;configure_mesh();for(unsigned i=0;i<30;++i)tick();
    TSUCHI_Vec3 zero{};CHECK(world.ground(final_mod,&point,&zero,&answer)==TSUCHI_INVALID);
    CHECK(world.ground(final_mod,&point,&extent,&answer)==TSUCHI_OK&&answer.z==25);
    projection=false;answer={99,99,99};CHECK(world.ground(final_mod,&point,&extent,&answer)==TSUCHI_NOT_FOUND&&answer.z==99);projection=true;
    CHECK(world.path(final_mod,&point,&point,points,1,&n)==TSUCHI_CAPACITY&&n==3&&points[0].x==99);
    CHECK(world.path(final_mod,&point,&point,nullptr,0,&n)==TSUCHI_CAPACITY&&n==3);
    CHECK(world.path(final_mod,&point,&point,points,3,&n)==TSUCHI_OK&&n==3&&points[2].z==9);
    partial=true;CHECK(world.path(final_mod,&point,&point,points,3,&n)==TSUCHI_NOT_FOUND);partial=false;
    valid=false;CHECK(world.path(final_mod,&point,&point,points,3,&n)==TSUCHI_NOT_FOUND);valid=true;
    break_coordinate=true;points[0]={99,99,99};CHECK(world.path(final_mod,&point,&point,points,3,&n)==TSUCHI_NOT_READY&&points[0].x==99);break_coordinate=false;
    thread=2;CHECK(world.path(final_mod,&point,&point,points,3,&n)==TSUCHI_INVALID);
    CHECK(world.ground(final_mod,&point,&extent,&answer)==TSUCHI_INVALID);thread=1;
    TSUCHI_Vec3 nan{std::numeric_limits<float>::quiet_NaN(),0,0};
    CHECK(world.path(final_mod,&nan,&point,points,3,&n)==TSUCHI_INVALID);
    CHECK(world.ground(final_mod,&nan,&extent,&answer)==TSUCHI_INVALID);
    mod_stop(final_mod);mod_stop(other);
    for(const auto& root:roots)CHECK(root.second==0);
    CHECK(scans==2&&stops==12&&parameter_calls==7);
    auto* stream_mod=mod_create("effects.stream");
    // A catalog requested after streaming sees new assets without a world change.
    catalog_entries=1;++catalog_revision;
    char refreshed[32];uint64_t refreshed_size=0;
    CHECK(effects.list(stream_mod,refreshed,sizeof refreshed,&refreshed_size)==TSUCHI_OK);
    CHECK(scans==3&&refreshed_size==7&&std::strcmp(refreshed,"Spark")==0);
    for(unsigned i=0;i<60;++i)tick();CHECK(scans==3);
    // Playback allocations change the inventory too. Reusing a live asset
    // must not rescan every UObject for each particle that a mod creates.
    for(unsigned i=0;i<3;++i){
        ++catalog_revision;
        CHECK(effects.play(stream_mod,"Spark",&point,&effect)==TSUCHI_OK);
        CHECK(effects.stop(stream_mod,effect)==TSUCHI_OK);tick();
    }
    CHECK(scans==3);
    catalog_entries=2;++catalog_revision;
    CHECK(effects.play(stream_mod,"Dust",&point,&effect)==TSUCHI_OK&&scans==4);
    CHECK(effects.stop(stream_mod,effect)==TSUCHI_OK);tick();mod_stop(stream_mod);
    // Run the real generated Lua parser against empty, single, small, and heap
    // packed lists. The heap case previously read past the missing empty ID.
    lua_State* L=luaL_newstate();CHECK(L!=nullptr);
    lua_host.effects=&effects;lua_mod=mod_create("effects.list-binding");
    const uint32_t counts[]={0,1,3,512};
    for(uint32_t count:counts){
        catalog_entries=count;world_handle=world_handle==100?200:100;tick();
        const uint64_t bytes=count?1+6+5*(count-1):1;
        std::vector<char> packed(size_t(bytes+1),char(0x55));
        CHECK(effects.list(lua_mod,packed.data(),bytes-1,&n)==TSUCHI_CAPACITY&&n==bytes&&packed[0]==0x55);
        CHECK(effects.list(lua_mod,packed.data(),bytes,&n)==TSUCHI_OK&&packed[bytes-1]==0&&packed[bytes]==0x55);
        lua_pushcfunction(L,l_list);
        CHECK(lua_pcall(L,0,1,0)==LUA_OK&&lua_istable(L,-1)&&lua_rawlen(L,-1)==count);
        for(uint32_t i=0;i<count;++i){
            lua_rawgeti(L,-1,i+1);
            CHECK(lua_isstring(L,-1)&&std::strcmp(lua_tostring(L,-1),i?"Dust":"Spark")==0);
            lua_pop(L,1);
        }
        lua_pop(L,1);
    }
    lua_close(L);mod_stop(lua_mod);
    std::printf("world and effects: %d checks, %d failures\n",checks,failures);return failures?1:0;
}
