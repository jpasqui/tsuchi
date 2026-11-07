/* Authored ordinary actor setup through public reflection. The observer keeps its component
 * alive while the normal loader stops a different mod that refused startup. */
#include <stdatomic.h>
#include "../../check.ordinary-lifetime-startup-producer/common.h"
typedef void (*OL_ProcessEvent)(void *,void *,void *);
typedef void (*OL_EndPlay)(void *,uint8_t);
typedef void (*OL_Unregister)(void *,bool);
typedef void (*OL_Destroyed)(void *);
static OL_Context c;
static TSUCHI_Actor owner;
static TSUCHI_Component mesh_part,root_part;
static TSUCHI_Object actor_class,mesh,clip,replacement;
static OL_Snapshot before;
static TSUCHI_Hook locator,ended,unregistered,destroyed;
static TSUCHI_Subscription stepping;
static atomic_bool locating,ready;
static atomic_ullong located;
static uint64_t raw_owner,raw_table;
static uint32_t raw_index,steps;
static atomic_uint accepted_destroyed;
static atomic_uchar accepted_state;
static const char *const names[]={"setup","producer_stopped","identity","visibility","material","empty_blueprint_mode","exclusive_reuse","cleanup"};
static uint64_t ol_pointer(uint64_t at) { return *(const volatile uint64_t*)(uintptr_t)at; }
static uint8_t ol_byte(uint64_t at) { return *(const volatile uint8_t*)(uintptr_t)at; }
static uint32_t ol_index(uint64_t at) { return *(const volatile uint32_t*)(uintptr_t)at; }
static void capture(void *actor,void *function,void *parameters) {
    if (atomic_exchange_explicit(&locating,false,memory_order_acq_rel)) atomic_store_explicit(&located,(uint64_t)(uintptr_t)actor,memory_order_release);
    ((OL_ProcessEvent)locator.original)(actor,function,parameters);
}
static void before_end(void *actor,uint8_t reason) { ((OL_EndPlay)ended.original)(actor,reason); }
static void before_unregister(void *actor,bool only_tick) { ((OL_Unregister)unregistered.original)(actor,only_tick); }
static void before_destroyed(void *actor) {
    if (atomic_load_explicit(&ready,memory_order_acquire) && (uint64_t)(uintptr_t)actor==raw_owner) {
        atomic_store_explicit(&accepted_state,ol_byte(raw_owner+0x5c),memory_order_relaxed);
        atomic_fetch_add_explicit(&accepted_destroyed,1,memory_order_relaxed);
    }
    ((OL_Destroyed)destroyed.original)(actor);
}
static TSUCHI_Result set_transform(TSUCHI_PreparedCall call) {
    const char *const fields[]={"SpawnTransform.Rotation.W","SpawnTransform.Scale3D.X","SpawnTransform.Scale3D.Y","SpawnTransform.Scale3D.Z",
                              "SpawnTransform.Translation.X","SpawnTransform.Translation.Y","SpawnTransform.Translation.Z"};
    const double numbers[]={1,1,1,1,31000,32000,33000};
    for (unsigned i=0;i<7;++i) {
        const TSUCHI_Value value=ol_number(numbers[i]);
        const TSUCHI_Result result=c.host->reflection->call_set(c.self,call,fields[i],&value);
        if (result!=TSUCHI_OK) return result;
    }
    return TSUCHI_OK;
}
static TSUCHI_Result authored_spawn(TSUCHI_Object statics,TSUCHI_Actor context) {
    TSUCHI_PreparedCall call={0}; TSUCHI_Value result_value={0};
    TSUCHI_Result result=c.host->reflection->begin_call(c.self,statics,"BeginDeferredActorSpawnFromClass",&call);
    if (result!=TSUCHI_OK) return result;
    TSUCHI_Value world_context=ol_object(tsuchi_actor_as_object(context)),klass=ol_object(actor_class),collision=ol_integer(1);
    result=c.host->reflection->call_set(c.self,call,"WorldContextObject",&world_context);
    if (result==TSUCHI_OK) result=c.host->reflection->call_set(c.self,call,"ActorClass",&klass);
    if (result==TSUCHI_OK) result=set_transform(call);
    if (result==TSUCHI_OK) result=c.host->reflection->call_set(c.self,call,"CollisionHandlingOverride",&collision);
    /* Owner is the captured nullable default. This authored actor belongs to no gameplay owner. */
    if (result==TSUCHI_OK) result=c.host->reflection->invoke(c.self,call);
    if (result==TSUCHI_OK) result=c.host->reflection->call_get(c.self,call,"ReturnValue",&result_value);
    const TSUCHI_Result ended_call=c.host->reflection->end_call(c.self,call);
    if (result!=TSUCHI_OK) return result;
    if (ended_call!=TSUCHI_OK) return ended_call;
    if (result_value.kind!=TSUCHI_VALUE_OBJECT || !result_value.as.object.id) return TSUCHI_NOT_READY;
    owner.id=result_value.as.object.id;
    result=c.host->reflection->begin_call(c.self,statics,"FinishSpawningActor",&call);
    if (result!=TSUCHI_OK) return result;
    TSUCHI_Value given=ol_object(tsuchi_actor_as_object(owner));
    result=c.host->reflection->call_set(c.self,call,"Actor",&given);
    if (result==TSUCHI_OK) result=set_transform(call);
    if (result==TSUCHI_OK) result=c.host->reflection->invoke(c.self,call);
    if (result==TSUCHI_OK) result=c.host->reflection->call_get(c.self,call,"ReturnValue",&result_value);
    const TSUCHI_Result finished_call=c.host->reflection->end_call(c.self,call);
    if (result!=TSUCHI_OK) return result;
    if (finished_call!=TSUCHI_OK) return finished_call;
    return result_value.kind==TSUCHI_VALUE_OBJECT && result_value.as.object.id==owner.id ? TSUCHI_OK : TSUCHI_NOT_READY;
}
static bool native_actor(void) {
    bool alive=false,gameplay=true; TSUCHI_Object got_class={0},gactor={0},root={0};
    TSUCHI_Transform pose={0};
    if (c.host->actor->alive(c.self,owner,&alive)!=TSUCHI_OK || !alive ||
        c.host->reflection->class_of(c.self,tsuchi_actor_as_object(owner),&got_class)!=TSUCHI_OK || got_class.id!=actor_class.id ||
        c.host->reflection->find_class(c.self,"GActor",&gactor)!=TSUCHI_OK ||
        c.host->reflection->is_a(c.self,tsuchi_actor_as_object(owner),gactor,&gameplay)!=TSUCHI_OK || gameplay ||
        c.host->actor->transform(c.self,owner,&pose)!=TSUCHI_OK || !ol_at_place(&pose) ||
        !ol_get_object(c,tsuchi_actor_as_object(owner),"RootComponent",&root) || !root.id) return false;
    root_part.id=root.id; TSUCHI_Actor root_owner={0};
    if (c.host->component->owner(c.self,root_part,&root_owner)!=TSUCHI_OK || root_owner.id!=owner.id) return false;
    atomic_store_explicit(&located,0,memory_order_relaxed); atomic_store_explicit(&locating,true,memory_order_release);
    const TSUCHI_Result getter=ol_call(c,tsuchi_actor_as_object(owner),"K2_GetActorLocation",0,0,0);
    atomic_store_explicit(&locating,false,memory_order_release);
    raw_owner=atomic_load_explicit(&located,memory_order_acquire);
    if (getter!=TSUCHI_OK || !raw_owner) return false;
    raw_table=ol_pointer(raw_owner); raw_index=ol_index(raw_owner+0xc);
    if (!raw_table || !ol_pointer(raw_owner+0x130) || (ol_byte(raw_owner+0x5c)&0x60)!=0x40 || (ol_byte(raw_owner+0x5c)&8)) return false;
    const char *const axes[]={"ReturnValue.X","ReturnValue.Y","ReturnValue.Z"};
    const double position[]={31000,32000,33000};
    for (unsigned i=0;i<3;++i) {
        double got=0,scale=0;
        if (!ol_actor_number(c,owner,"K2_GetActorLocation",axes[i],&got) || got-position[i]<-0.1 || got-position[i]>0.1 ||
            !ol_actor_number(c,owner,"GetActorScale3D",axes[i],&scale) || scale<0.999 || scale>1.001) return false;
    }
    ol_log_id(c,"authored_owner",owner.id); ol_log_id(c,"authored_receiver",raw_owner);
    ol_log_id(c,"authored_table",raw_table); ol_log_id(c,"authored_index",raw_index);
    atomic_store_explicit(&ready,true,memory_order_release); return true;
}
static TSUCHI_Result set_mesh(void) {
    TSUCHI_PreparedCall call={0};
    TSUCHI_Result result=c.host->reflection->begin_call(c.self,tsuchi_component_as_object(mesh_part),"SetSkeletalMesh",&call);
    if (result!=TSUCHI_OK) return result;
    TSUCHI_Value new_mesh=ol_object(mesh),reinitialize=ol_boolean(true);
    result=c.host->reflection->call_set(c.self,call,"NewMesh",&new_mesh);
    if (result==TSUCHI_OK) result=c.host->reflection->call_set(c.self,call,"bReinitPose",&reinitialize);
    if (result==TSUCHI_OK) result=c.host->reflection->invoke(c.self,call);
    const TSUCHI_Result ended_call=c.host->reflection->end_call(c.self,call);
    return result==TSUCHI_OK ? ended_call : result;
}
static void cleanup(bool assert_result) {
    if (assert_result) tsuchi_check_begin("cleanup");
    if (mesh_part.id) {
        const TSUCHI_Result removed=c.host->component->remove(c.self,mesh_part);
        if (assert_result) tsuchi_expect_result(removed,TSUCHI_OK,"remove the observer-owned surviving component once");
    }
    if (owner.id) {
        const TSUCHI_Result asked=ol_call(c,tsuchi_actor_as_object(owner),"K2_DestroyActor",0,0,0);
        if (assert_result) {
            tsuchi_expect_result(asked,TSUCHI_OK,"the reflected authored-owner cleanup request");
            tsuchi_expect_number(atomic_load(&accepted_destroyed),1,"one admitted native Destroyed callback for the authored owner");
            tsuchi_expect((atomic_load(&accepted_state)&8)!=0,"the native engine accepted destruction before its leaf callback");
        }
    }
    if (assert_result) tsuchi_check_end();
}
static void on_step(void *user,const TSUCHI_TimeStep *event) {
    (void)user; (void)event; if (++steps<3) return;
    char version[32]; uint64_t length=0;
    tsuchi_check_begin("producer_stopped");
    tsuchi_expect_result(c.host->mod->version_of(c.self,"check.ordinary-lifetime-startup-producer",version,sizeof version,&length),TSUCHI_NOT_FOUND,"the ordinary loader stopped the startup-refusing producer");
    /* The producer's separate applied receipt, same owner/component IDs, and loader
     * NOT_READY record are required inputs. Absence alone cannot prove it ran. */
    tsuchi_check_end();
    OL_Snapshot after={0}; const bool snapshot=ol_snapshot(c,owner,mesh_part,clip,&after);
    tsuchi_check_begin("identity");
    tsuchi_expect(snapshot,"the observer-owned mesh is still live on the same ordinary owner");
    tsuchi_expect(after.owner.id==before.owner.id && after.component.id==before.component.id && after.mesh.id==before.mesh.id &&
        after.mesh_skeleton.id==before.mesh_skeleton.id && after.clip_skeleton.id==before.clip_skeleton.id,"the surviving native objects and matching skeleton keep their identities");
    tsuchi_expect(raw_table==ol_pointer(raw_owner) && raw_index==ol_index(raw_owner+0xc),"the authored native owner still has the captured table and object index");
    tsuchi_check_end();
    tsuchi_check_begin("visibility"); bool hidden=true;
    tsuchi_expect(snapshot && after.visible==before.visible,"native visibility matches the observer's original");
    tsuchi_expect_result(c.host->component->hidden(c.self,mesh_part,&hidden),TSUCHI_OK,"visibility after producer stop");
    tsuchi_expect(!hidden,"no visibility hold survives producer stop"); tsuchi_check_end();
    tsuchi_check_begin("material");
    tsuchi_expect(snapshot && replacement.id!=before.material.id && after.material.id==before.material.id,
        "the distinct native material override ended on the still-live mesh"); tsuchi_check_end();
    tsuchi_check_begin("empty_blueprint_mode"); TSUCHI_Clip current={0};
    tsuchi_expect(snapshot && after.mode==before.mode && before.mode==0 && !before.anim_class.id && !before.instance.id &&
        after.anim_class.id==before.anim_class.id && after.instance.id==before.instance.id,
        "the native original empty Blueprint-mode state returned on the surviving mesh");
    tsuchi_expect_result(c.host->animation->clip(c.self,mesh_part,&current),TSUCHI_OK,"clip bookkeeping after producer stop");
    tsuchi_expect(!current.clip.id,"the stopped producer holds no clip");
    tsuchi_check_end();
    tsuchi_check_begin("exclusive_reuse");
    TSUCHI_Result material_hold=c.host->component->hold_material(c.self,mesh_part,0,replacement);
    tsuchi_expect_result(material_hold,TSUCHI_OK,"the observer can take the released material slot");
    if (material_hold==TSUCHI_OK) tsuchi_expect_result(c.host->component->release_material(c.self,mesh_part,0),TSUCHI_OK,"restore the observer's temporary slot hold");
    TSUCHI_Result clip_hold=c.host->animation->hold_clip(c.self,mesh_part,clip,true,0);
    tsuchi_expect_result(clip_hold,TSUCHI_OK,"the observer can take the released clip slot");
    if (clip_hold==TSUCHI_OK) tsuchi_expect_result(c.host->animation->release_clip(c.self,mesh_part),TSUCHI_OK,"restore the observer's temporary clip hold");
    tsuchi_check_end();
    ol_log_id(c,"surviving_component",mesh_part.id); ol_log_id(c,"restored_material",after.material.id);
    ol_log_id(c,"restored_mode",(uint64_t)after.mode); ol_log_id(c,"restored_anim_class",after.anim_class.id); ol_log_id(c,"restored_instance",after.instance.id);
    cleanup(true); c.host->cancel(c.self,stepping);
}
TSUCHI_Result tsuchi_mod_start(const TSUCHI_Host *given,TSUCHI_Mod *mod) {
    c=(OL_Context){given,mod}; tsuchi_check_start(given,mod);
    for (unsigned i=0;i<8;++i) tsuchi_check_plan(names[i]);
    tsuchi_check_begin("setup");
    bool good=tsuchi_expect_result(given->hook->wrap_address(mod,0x0905bea0+0x228,0,(void*)capture,&locator),TSUCHI_OK,"the captured ordinary PointLight receiver locator");
    if (good) good=tsuchi_expect_result(given->hook->wrap_address(mod,0x0905bea0+0x348,0x05086bb0,(void*)before_end,&ended),TSUCHI_OK,"the qualified ordinary EndPlay base leaf");
    if (good) good=tsuchi_expect_result(given->hook->wrap_address(mod,0x0905bea0+0x4e0,0x05094d50,(void*)before_unregister,&unregistered),TSUCHI_OK,"the qualified ordinary unregister base leaf");
    if (good) good=tsuchi_expect_result(given->hook->wrap_address(mod,0x0905bea0+0x548,0x05086fa0,(void*)before_destroyed,&destroyed),TSUCHI_OK,"the qualified ordinary destruction base leaf");
    TSUCHI_Object statics={0},actor_type={0},skeletal_type={0}; TSUCHI_Actor actors[128]; uint64_t count=0;
    if (good) good=tsuchi_expect_result(given->asset->load(mod,ol_actor_path,&actor_class),TSUCHI_OK,"the captured ordinary PointLight class asset");
    if (good) good=tsuchi_expect_result(given->reflection->find_class(mod,"GameplayStatics",&statics),TSUCHI_OK,"the native reflection spawning helper class");
    if (good) good=tsuchi_expect_result(given->reflection->find_class(mod,"Actor",&actor_type),TSUCHI_OK,"the general engine Actor class");
    if (good) good=tsuchi_expect_result(given->actor->list(mod,actor_type,actors,128,&count),TSUCHI_OK,"a real actor in the current startup world");
    if (good) good=tsuchi_expect(count>0,"a startup-world context actor exists without delaying the loader");
    if (good) good=tsuchi_expect_result(authored_spawn(statics,actors[0]),TSUCHI_OK,"native BeginDeferred/Finish through public prepared reflection calls");
    if (good) good=tsuchi_expect(native_actor(),"the authored native ordinary owner has the expected class, root, begun state, location, and scale");
    if (good) good=tsuchi_expect_result(given->reflection->find_class(mod,"SkeletalMeshComponent",&skeletal_type),TSUCHI_OK,"the added skeletal component class");
    if (good) good=tsuchi_expect_result(given->asset->load(mod,ol_mesh_path,&mesh),TSUCHI_OK,"the stock Chappy skeletal mesh");
    if (good) good=tsuchi_expect_result(given->asset->load(mod,ol_clip_path,&clip),TSUCHI_OK,"the stock matching stationary clip");
    if (good) good=tsuchi_expect_result(given->asset->load(mod,ol_material_path,&replacement),TSUCHI_OK,"a distinct replacement material");
    if (good) good=tsuchi_expect_result(given->component->add(mod,owner,skeletal_type,root_part,"",&mesh_part),TSUCHI_OK,"the observer-owned component on the authenticated ordinary owner");
    if (good) good=tsuchi_expect_result(set_mesh(),TSUCHI_OK,"set the skeletal mesh through its captured NewMesh and bReinitPose arguments");
    if (good) good=tsuchi_expect(ol_snapshot(c,owner,mesh_part,clip,&before) && before.visible && before.mesh.id==mesh.id &&
        before.material.id!=replacement.id && before.mode==0 && !before.anim_class.id && !before.instance.id,
        "the surviving fixture component has native visible/material/empty-Blueprint state and a matching skeleton");
    if (good) good=tsuchi_expect_result(given->time->on_step(mod,on_step,0,&stepping),TSUCHI_OK,"observe after the ordinary loader stops the producer");
    if (good) {
        ol_log_id(c,"original_component",mesh_part.id); ol_log_id(c,"original_material",before.material.id);
        ol_log_id(c,"original_mode",(uint64_t)before.mode); ol_log_id(c,"original_anim_class",before.anim_class.id);
        ol_log_id(c,"original_instance",before.instance.id); ol_log_id(c,"original_skeleton",before.mesh_skeleton.id);
    }
    tsuchi_check_end();
    if (!good) {
        cleanup(false);
        for (unsigned i=1;i<8;++i) { tsuchi_check_begin(names[i]); tsuchi_expect(false,"the authored ordinary startup prerequisite did not pass"); tsuchi_check_end(); }
        return TSUCHI_NOT_READY;
    }
    return TSUCHI_OK;
}
