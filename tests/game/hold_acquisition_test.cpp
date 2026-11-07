// Actual hold/material/visibility code with controlled synchronous native callbacks.
// These controls do not claim that native callbacks or shader GC ran in the game.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include "registry.h"
#include "holds.h"
#include "hidden.h"
#include "materials.h"
#include "object_retention.h"
#include "ordinary_lifecycle.h"
#include "actor_events.h"
#include "parts.h"
#include "objects.h"
#include "frame.h"
using namespace tsuchi;
namespace {
int failures=0,checks=0;
#define CHECK(x) do{++checks;if(!(x)){++failures;std::printf("line%d: %s\n",__LINE__,#x);std::fflush(stdout);}}while(0)
uint64_t fixture_owner=0x1000,part=0x2000,other=0x2100,original=0x5000,wanted=0x6000,other_original=0x5100,other_wanted=0x6100;
std::unordered_map<uint64_t,uint64_t> handles;
std::unordered_map<uint64_t,uint64_t> slots;
std::unordered_map<uint64_t,uint64_t> standins;
std::unordered_map<uint64_t,uint64_t> tracked;
std::unordered_map<uint64_t,bool> visible;
std::unordered_map<void*,size_t> allocations;
std::vector<void*> retired_allocations;
bool stop_on_transferred_list_free=false;
uint32_t part_lists=0;
bool dying=false;
std::unordered_set<uint64_t> roots;
uint64_t root_refusal=0;
bool setter_refusal=false;
uint32_t image_owner_queries=0;
bool stop_provider_in_restore=false;
uint64_t restore_mid=0xa000;
uint32_t restore_provider_stops=0;
bool request_growth=false,refuse_apply=false;
struct Captured { uint32_t value; };
TSUCHI_Result capture_apply(const Hold&,Captured&,const Captured&,bool);
Holds<Captured,Captured,capture_apply,nullptr,nullptr> captured_values{"test.capture",HoldOn::nothing,false};
Holds<Captured,Captured,capture_apply,nullptr,nullptr> legacy_actors{"test.legacy-actor",HoldOn::actor,false};
Holds<Captured,Captured,capture_apply,nullptr,nullptr> watched_actors{"test.watched-actor",HoldOn::watched_actor,false};
TSUCHI_Result actor_watch_result=TSUCHI_OK;
uint32_t actor_watch_calls=0;
void (*ordinary_left)(uint64_t,uint64_t,bool)=nullptr;
TSUCHI_Result capture_apply(const Hold&,Captured& kept,const Captured& request,bool) {
    if(refuse_apply)return TSUCHI_INVALID;
    if(request_growth){request_growth=false;CHECK(captured_values.reserve(100));}
    kept=request;return TSUCHI_OK;
}
uint64_t next_handle=1;
std::vector<void(*)()> steps;
TSUCHI_Mod* stopping=nullptr;
TSUCHI_Mod* survivor=nullptr;
enum class Callback{none,resolver,setter,create_other,release_retake,invalidate,grow,begin_destroying,hide,hide_second};
Callback callback=Callback::none;
uint32_t native_set=0,native_restore=0,visibility_set=0,visibility_restore=0,free_errors=0;
TSUCHI_Result nested_result=TSUCHI_OK;
void* allocated(size_t n){void* p=std::malloc(n);allocations[p]=n;if(n==8||n==16)++part_lists;return p;}
void released(void* p){
    if(!p)return;
    const auto found=allocations.find(p);
    if(found==allocations.end()){++free_errors;return;}
    const size_t bytes=found->second;
    allocations.erase(p);if(bytes==8||bytes==16)--part_lists;
    if(bytes==8&&stop_on_transferred_list_free){stop_on_transferred_list_free=false;mod_stop(stopping);}
    // A bounded host quarantine makes use of an ended table deterministic,
    // without making a claim about native free/GC or relying on heap reuse.
    std::memset(p,0xcc,bytes);retired_allocations.push_back(p);
}
void run_callback(Callback at){
    if(callback!=at)return;callback=Callback::none;
    if(at==Callback::create_other){CHECK(hold_material(survivor,other,0,other_wanted)==TSUCHI_OK);mod_stop(stopping);}
    else if(at==Callback::release_retake){CHECK(restore_material(stopping,part,0)==TSUCHI_OK);nested_result=hold_material(stopping,part,0,other_wanted);}
    else if(at==Callback::grow){
        for(uint64_t i=0;i<48;++i){const uint64_t p=0x3000+i;slots[p]=0x7000+i;CHECK(hold_material(survivor,p,0,0x8000+i)==TSUCHI_OK);}
        mod_stop(stopping);
    }
    else if(at==Callback::begin_destroying){dying=true;mod_stop(stopping);}
    else if(at==Callback::invalidate){const uint64_t h=game::to_handle(part);handles.erase(h);mod_stop(stopping);}
    else mod_stop(stopping);
}
void reset(){CHECK(roots.empty());callback=Callback::none;dying=false;native_set=native_restore=visibility_set=visibility_restore=0;
    slots[part]=original;slots[other]=other_original;visible[part]=visible[other]=true;}
void initialize_handles(){game::to_handle(fixture_owner);game::to_handle(part);game::to_handle(other);game::to_handle(original);game::to_handle(wanted);game::to_handle(other_original);game::to_handle(other_wanted);}
}
namespace tsuchi {
bool each_frame(void(*step)()){steps.push_back(step);return true;}
bool on_actor_left(void(*)(uint64_t,uint64_t,ActorLeft)){return true;}
void ordinary_lifecycle_listener(void(*listener)(uint64_t,uint64_t,bool)){ordinary_left=listener;}
TSUCHI_Result ordinary_lifecycle_watch(uint64_t){++actor_watch_calls;return actor_watch_result;}
uint64_t ordinary_lifecycle_restore_part(uint64_t h,uint64_t o){
    const uint64_t p=game::from_handle(h);return !dying&&p&&(!o||game::from_handle(o)==fixture_owner)?p:0;}
uint64_t shader_source(uint64_t m){for(const auto& item:standins)if(item.second==m)return item.first;return m;}
bool track_shader_slot(uint64_t source,uint64_t p,uint32_t){if(!standins.count(source))return false;tracked[p]=source;return true;}
uint64_t drawn_as(uint64_t m){run_callback(Callback::resolver);return m;}
// Never the same twice: every held slot is resolved in every step, as these checks expect.
uint32_t shader_generation() { static uint32_t calls = 0; return ++calls; }
bool drawn_as_restoring(uint64_t m,game::CallGuard guard,uint64_t& shown){
    if(!guard.permits())return false;run_callback(Callback::resolver);shown=standins.count(m)?standins[m]:m;return guard.permits();}
}
namespace tsuchi::game {
uint64_t from_handle(uint64_t h){auto at=handles.find(h);return at==handles.end()?0:at->second;}
uint64_t to_handle(uint64_t p){if(!p)return 0;for(const auto& h:handles)if(h.second==p)return h.first;handles[next_handle]=p;return next_handle++;}
bool is_game_actor(uint64_t){return false;}
uint64_t part_owner(uint64_t p){if(p==0x2900)++image_owner_queries;return fixture_owner;}
bool is_scene_part(uint64_t){return true;}
bool is_mesh_part(uint64_t p){return p!=0x2900;}
bool is_material_image(uint64_t p){return p==0x2900;}
bool is_shader_target(uint64_t){return true;}
bool object_kept(uint64_t p){return roots.count(p)!=0;}
bool keep_object(uint64_t p,bool keep){if(keep){if(p==root_refusal)return false;return roots.insert(p).second;}return roots.erase(p)!=0;}
bool is_material(uint64_t){return true;}
bool material_slots(uint64_t,uint32_t& count){count=1;return true;}
uint64_t slot_override(uint64_t p,uint32_t){return slots[p];}
bool native_material(uint64_t p,uint64_t m){
    if(setter_refusal)return false;
    if(m==wanted||m==other_wanted)++native_set;else ++native_restore;
    if(p==part){
        run_callback(Callback::setter);run_callback(Callback::create_other);run_callback(Callback::release_retake);
        run_callback(Callback::invalidate);run_callback(Callback::grow);run_callback(Callback::begin_destroying);
    }
    if(stop_provider_in_restore&&m==restore_mid){
        stop_provider_in_restore=false;++restore_provider_stops;
        // The provider relinquishes its independently held cache root before
        // the setter's remaining native work touches the resolved MID.
        release_game_object(to_handle(restore_mid));
        CHECK(object_kept(restore_mid));
    }
    slots[p]=m;return true;
}
bool set_slot_material(uint64_t p,uint32_t,uint64_t m){return native_material(p,m);}
bool restore_slot_material(uint64_t p,uint32_t,uint64_t m,CallGuard guard){
    if(!guard.permits())return false;const bool applied=native_material(p,m);return applied&&guard.permits();}
bool part_visible(uint64_t p,bool& value){value=visible[p];return true;}
bool set_part_visible(uint64_t p,bool value){
    if(value)++visibility_restore;else ++visibility_set;
    run_callback(Callback::hide);if(p==other)run_callback(Callback::hide_second);visible[p]=value;return true;}
bool restore_part_visible(uint64_t p,bool value,CallGuard guard){
    return guard.permits()&&set_part_visible(p,value)&&guard.permits();}
bool each_attached(uint64_t p,void(*visit)(void*,uint64_t),void* ctx){visit(ctx,p);visit(ctx,other);return true;}
}
int main(){
    set_allocator({allocated,released});initialize_handles();hidden_start();materials_start();
    auto* actor_mod=mod_create("acquisition.actor-watch");
    const uint64_t actor_handle=game::to_handle(fixture_owner);
    actor_watch_result=TSUCHI_UNSUPPORTED;
    CHECK(legacy_actors.take(actor_mod,actor_handle,0,Captured{1})==TSUCHI_OK&&actor_watch_calls==0);
    CHECK(watched_actors.take(actor_mod,actor_handle,0,Captured{1})==TSUCHI_UNSUPPORTED&&actor_watch_calls==1);
    actor_watch_result=TSUCHI_OK;
    CHECK(watched_actors.take(actor_mod,actor_handle,0,Captured{1})==TSUCHI_OK);
    CHECK(ordinary_left!=nullptr);ordinary_left(fixture_owner,actor_handle,true);
    CHECK(watched_actors.count()==0&&legacy_actors.count()==1);
    mod_stop(actor_mod);CHECK(legacy_actors.count()==0);
    reset();stopping=mod_create("acquisition.resolver");callback=Callback::resolver;
    CHECK(hold_material(stopping,part,0,wanted)==TSUCHI_NOT_READY);
    CHECK(!material_holds()&&slots[part]==original&&native_set==0&&native_restore==0&&owned_count(stopping)==0);
    reset();stopping=mod_create("acquisition.setter");callback=Callback::setter;
    CHECK(hold_material(stopping,part,0,wanted)==TSUCHI_NOT_READY);
    CHECK(!material_holds()&&slots[part]==original&&native_set==1&&native_restore==1&&owned_count(stopping)==0);
    reset();stopping=mod_create("acquisition.swap");survivor=mod_create("acquisition.survivor");callback=Callback::create_other;
    CHECK(hold_material(stopping,part,0,wanted)==TSUCHI_NOT_READY);
    CHECK(material_holds()==1&&slots[part]==original&&slots[other]==other_wanted);
    mod_stop(survivor);CHECK(!material_holds()&&slots[other]==other_original);
    reset();stopping=mod_create("acquisition.retake");callback=Callback::release_retake;
    CHECK(hold_material(stopping,part,0,wanted)==TSUCHI_NOT_READY);
    CHECK(nested_result==TSUCHI_BUSY&&!material_holds()&&slots[part]==original&&native_restore==1);
    mod_stop(stopping);
    reset();stopping=mod_create("acquisition.invalid");callback=Callback::invalidate;
    CHECK(hold_material(stopping,part,0,wanted)==TSUCHI_NOT_READY);
    CHECK(!material_holds()&&native_set==1&&native_restore==0);
    game::to_handle(part);
    reset();stopping=mod_create("acquisition.hidden");callback=Callback::hide;
    CHECK(hide_part(stopping,part,true)==TSUCHI_NOT_READY);
    CHECK(hidden_counts().parts==0&&hidden_counts().holds==0&&visible[part]&&visible[other]);
    CHECK(part_lists==0);
    reset();stopping=mod_create("acquisition.hidden-second");callback=Callback::hide_second;
    CHECK(hide_part(stopping,part,true)==TSUCHI_NOT_READY);
    CHECK(hidden_counts().parts==0&&hidden_counts().holds==0&&visible[part]&&visible[other]);
    CHECK(visibility_set==2&&visibility_restore==2&&part_lists==0);
    reset();stopping=mod_create("acquisition.hidden-update");
    CHECK(hide_part(stopping,part,false)==TSUCHI_OK);
    callback=Callback::hide_second;
    CHECK(hide_part(stopping,part,true)==TSUCHI_NOT_READY);
    CHECK(hidden_counts().parts==0&&hidden_counts().holds==0&&visible[part]&&visible[other]&&part_lists==0);
    reset();stopping=mod_create("acquisition.hidden-transfer");
    CHECK(hide_part(stopping,part,false)==TSUCHI_OK);
    stop_on_transferred_list_free=true;
    CHECK(hide_part(stopping,part,true)==TSUCHI_NOT_READY);
    CHECK(!stop_on_transferred_list_free&&hidden_counts().parts==0&&hidden_counts().holds==0&&visible[part]&&visible[other]);
    CHECK(part_lists==0&&free_errors==0);
    reset();stopping=mod_create("acquisition.update");
    CHECK(hold_material(stopping,part,0,wanted)==TSUCHI_OK);
    native_set=native_restore=0;callback=Callback::setter;
    CHECK(hold_material(stopping,part,0,other_wanted)==TSUCHI_NOT_READY);
    CHECK(!material_holds()&&slots[part]==original&&native_set==1&&native_restore==1);
    reset();stopping=mod_create("acquisition.keep");
    CHECK(hold_material(stopping,part,0,wanted)==TSUCHI_OK);
    native_set=native_restore=0;callback=Callback::resolver;
    for(auto step:steps)step();
    CHECK(!material_holds()&&slots[part]==original&&native_set==0&&native_restore==1);
    reset();stopping=mod_create("acquisition.grow");survivor=mod_create("acquisition.grow-survivor");callback=Callback::grow;
    CHECK(hold_material(stopping,part,0,wanted)==TSUCHI_NOT_READY);
    CHECK(material_holds()==48&&slots[part]==original);
    for(uint64_t i=0;i<48;++i)CHECK(slots[0x3000+i]==0x8000+i);
    mod_stop(survivor);
    CHECK(!material_holds());for(uint64_t i=0;i<48;++i)CHECK(slots[0x3000+i]==0x7000+i);
    reset();stopping=mod_create("acquisition.dying");callback=Callback::begin_destroying;
    CHECK(hold_material(stopping,part,0,wanted)==TSUCHI_NOT_READY);
    CHECK(!material_holds()&&native_set==1&&native_restore==0);
    dying=false;
    auto* captures=mod_create("acquisition.capture-values");
    CHECK(captured_values.take(captures,1,0,Captured{73})==TSUCHI_OK);
    const Captured& source=*captured_values.find(captures,1,0);
    request_growth=true;
    CHECK(captured_values.take(captures,2,0,source)==TSUCHI_OK);
    CHECK(captured_values.find(captures,2,0)->value==73);
    const uint32_t count_before=captured_values.count(),owned_before=owned_count(captures);
    refuse_apply=true;
    CHECK(captured_values.take(captures,3,0,Captured{81})==TSUCHI_INVALID);
    CHECK(captured_values.count()==count_before&&owned_count(captures)==owned_before&&!captured_values.find(captures,3,0));
    refuse_apply=false;mod_stop(captures);
    CHECK(captured_values.count()==0);
    reset();auto* restoration=mod_create("custody.restore-provider-stop");
    CHECK(hold_material(restoration,part,0,wanted)==TSUCHI_OK);
    standins[original]=restore_mid;game::to_handle(restore_mid);
    CHECK(retain_game_object(game::to_handle(restore_mid))); // shader cache
    stop_provider_in_restore=true;
    CHECK(restore_material(restoration,part,0)==TSUCHI_OK);
    CHECK(restore_provider_stops==1&&!stop_provider_in_restore&&slots[part]==restore_mid);
    CHECK(material_holds()==0&&roots.empty());standins.clear();mod_stop(restoration);
    reset();auto* restore_refused=mod_create("custody.restore-setter-refused");
    CHECK(hold_material(restore_refused,part,0,wanted)==TSUCHI_OK);
    standins[original]=restore_mid;setter_refusal=true;
    CHECK(restore_material(restore_refused,part,0)==TSUCHI_OK);
    CHECK(material_holds()==0&&roots.empty()&&slots[part]==wanted);
    setter_refusal=false;standins.clear();mod_stop(restore_refused);
    reset();auto* restore_root_refused=mod_create("custody.restore-root-refused");
    CHECK(hold_material(restore_root_refused,part,0,wanted)==TSUCHI_OK);
    standins[original]=restore_mid;root_refusal=restore_mid;
    native_restore=0;CHECK(restore_material(restore_root_refused,part,0)==TSUCHI_OK);
    CHECK(material_holds()==0&&roots.empty()&&slots[part]==wanted&&native_restore==0);
    root_refusal=0;standins.clear();mod_stop(restore_root_refused);
    reset();stopping=mod_create("custody.latest-before-stop");
    CHECK(hold_material(stopping,part,0,wanted)==TSUCHI_OK);
    slots[part]=other_original;callback=Callback::resolver;
    for(auto step:steps)step();
    CHECK(!material_holds()&&slots[part]==other_original&&roots.empty());
    reset();standins[wanted]=0x9000;game::to_handle(0x9000);root_refusal=0x9000;
    auto* shown_refusal=mod_create("custody.shown-root-refused");
    CHECK(hold_material(shown_refusal,part,0,wanted)==TSUCHI_NO_MEMORY);
    CHECK(!material_holds()&&roots.empty()&&slots[part]==original&&owned_count(shown_refusal)==0);
    root_refusal=0;standins.clear();mod_stop(shown_refusal);
    // A refusal at either distinct root leaves neither a hold nor a root.
    for(uint64_t denied : {original,wanted}){
        reset();root_refusal=denied;auto* mod=mod_create("custody.refused");
        CHECK(hold_material(mod,part,0,wanted)==TSUCHI_NO_MEMORY);
        CHECK(material_holds()==0&&roots.empty()&&slots[part]==original&&owned_count(mod)==0);
        root_refusal=0;mod_stop(mod);
    }
    reset();auto* refusal=mod_create("custody.setter-refused");setter_refusal=true;
    CHECK(hold_material(refusal,part,0,wanted)==TSUCHI_NOT_READY);
    CHECK(material_holds()==0&&roots.empty()&&slots[part]==original);
    setter_refusal=false;mod_stop(refusal);
    reset();auto* updated=mod_create("custody.update-refused");
    CHECK(hold_material(updated,part,0,wanted)==TSUCHI_OK);
    setter_refusal=true;CHECK(hold_material(updated,part,0,other_wanted)==TSUCHI_NOT_READY);
    CHECK(material_holds()==1&&slots[part]==wanted&&roots.count(original)&&roots.count(wanted)&&!roots.count(other_wanted));
    setter_refusal=false;mod_stop(updated);CHECK(roots.empty()&&slots[part]==original);
    // The asset and material services share counts. An engine-owned root is
    // never cleared merely because the framework's last lease ends.
    reset();roots.insert(original);CHECK(retain_game_object(game::to_handle(original)));
    auto* interleaved=mod_create("custody.interleaved");
    CHECK(hold_material(interleaved,part,0,wanted)==TSUCHI_OK);
    release_game_object(game::to_handle(original));CHECK(roots.count(original)&&roots.count(wanted));
    mod_stop(interleaved);CHECK(roots.size()==1&&roots.count(original));roots.clear();
    reset();CHECK(retain_game_object(game::to_handle(wanted)));
    auto* asset=mod_create("custody.asset-first");CHECK(hold_material(asset,part,0,wanted)==TSUCHI_OK);
    mod_stop(asset);CHECK(roots.size()==1&&roots.count(wanted));
    release_game_object(game::to_handle(wanted));CHECK(roots.empty());
    // Image is a UObject hold. It has no ActorComponent owner and public mods
    // still cannot take a brush through the mesh-only material service.
    reset();const uint64_t image=0x2900;slots[image]=original;game::to_handle(image);
    auto* image_mod=mod_create("custody.image-public");
    CHECK(hold_material(image_mod,image,0,wanted)==TSUCHI_UNSUPPORTED);mod_stop(image_mod);
    CHECK(hold_material(nullptr,image,0,wanted)==TSUCHI_OK);
    CHECK(image_owner_queries==0&&shader_slot_source(image,0)==game::to_handle(wanted));
    CHECK(roots.count(original)&&roots.count(wanted));
    CHECK(restore_material(nullptr,image,0)==TSUCHI_OK);
    CHECK(image_owner_queries==0&&slots[image]==original&&roots.empty()&&!shader_slot_source(image,0));
    CHECK(hold_material(nullptr,image,0,wanted)==TSUCHI_OK);
    handles.erase(game::to_handle(image));for(auto step:steps)step();
    CHECK(image_owner_queries==0&&material_holds()==0&&roots.empty());
    reset();game::to_handle(image);slots[image]=original;standins[wanted]=0x9000;standins[other_wanted]=0x9100;
    game::to_handle(0x9000);game::to_handle(0x9100);
    CHECK(hold_material(nullptr,image,0,wanted)==TSUCHI_OK&&slots[image]==0x9000);
    slots[image]=0x9100;for(auto step:steps)step();
    CHECK(slots[image]==0x9100&&tracked[image]==other_wanted);
    CHECK(shader_slot_source(image,0)==game::to_handle(other_wanted));
    CHECK(!roots.count(wanted)&&!roots.count(0x9000)&&roots.count(other_wanted)&&roots.count(0x9100));
    CHECK(restore_material(nullptr,image,0)==TSUCHI_OK&&slots[image]==other_wanted&&roots.empty());
    standins.clear();tracked.clear();
    reset();slots[image]=0;CHECK(hold_material(nullptr,image,0,wanted)==TSUCHI_OK);
    CHECK(restore_material(nullptr,image,0)==TSUCHI_OK&&slots[image]==0&&roots.empty());
    CHECK(free_errors==0);
    std::printf("acquisition: %d checks, %d failures, invalid frees=%u\n",checks,failures,free_errors);
    for(void* p:retired_allocations)std::free(p);
    return failures;
}
