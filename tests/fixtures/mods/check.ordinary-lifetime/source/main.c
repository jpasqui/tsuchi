/* A native-only SDK observer. Configure one real donor and one lifetime route per run.
 * Startup installs the observer before the framework adds its later outer lifecycle hooks.
 * No native destruction is deferred, replayed, or called by its address. */
#include <stdatomic.h>
#include "tsuchi/tsuchi.h"
#include "tsuchi_check.h"

typedef void (*ProcessEvent)(void *, void *, void *);
typedef void (*EndPlay)(void *, uint8_t);
typedef void (*Unregister)(void *, bool);
typedef void (*Destroyed)(void *);
static const TSUCHI_Host *host;
static TSUCHI_Mod *self;
static TSUCHI_Hook locator, ended, unregistered, destroyed;
static TSUCHI_Subscription stepping, births, deaths;
static const char *const checks[] = {"installed","donor","held","before_native","bookkeeping","native_resources","added","gameplay_events"};
static bool finished[8], configured, destroy_asked, material_required, animation_required;
static atomic_bool held;
static atomic_flag sampling=ATOMIC_FLAG_INIT;
static unsigned steps, held_at;
static uint64_t table_offset, end_stock, unregister_stock, destroyed_stock, timeout;
static char scenario[16], world[96], class_name[160], actor_name[160], part_name[160];
static char material_path[240], clip_path[240];
static TSUCHI_Actor owner;
static TSUCHI_Component part, added;
static TSUCHI_Object original_material, replacement, clip, original_anim_class, original_instance_class;
static int64_t original_mode;
static bool original_visible;
static uint64_t raw_owner, raw_table, raw_index;
static atomic_bool locating;
static atomic_ullong located;
static atomic_uint birth_count, death_count, native_destroy_count;
static struct {
    atomic_bool ready;
    uint8_t kind, reason, state;
    bool identity, hidden, visible, has_clip, added_present, material_matches, mode_matches, instance_matches;
    TSUCHI_Result owner_result, hidden_result, visible_result, clip_result, list_result;
    uint64_t handle, raw, table, material, instance_class;
} observation;

static uint64_t pointer_at(uint64_t at) { return *(const volatile uint64_t *)(uintptr_t)at; }
static uint8_t byte_at(uint64_t at) { return *(const volatile uint8_t *)(uintptr_t)at; }
static uint32_t index_at(uint64_t at) { return *(const volatile uint32_t *)(uintptr_t)at; }
static bool copy_text(char *out, size_t capacity, const char *text) {
    const size_t size = strlen(text);
    if (size >= capacity) return false;
    memcpy(out,text,size+1); return true;
}
static bool hex(const char *at, uint64_t *out) {
    if (at[0]=='0' && at[1]=='x') at+=2;
    *out=0; unsigned digits=0;
    for (;*at;++at) {
        unsigned digit;
        if (*at>='0' && *at<='9') digit=(unsigned)(*at-'0');
        else if (*at>='a' && *at<='f') digit=(unsigned)(*at-'a'+10);
        else if (*at>='A' && *at<='F') digit=(unsigned)(*at-'A'+10);
        else return false;
        if (++digits>16) return false;
        *out=(*out<<4)|digit;
    }
    return digits!=0;
}
static bool config(void) {
    char buffer[1800]; const char *lines[12];
    if (tsuchi_check_lines("targets.txt",buffer,sizeof buffer,lines,12)!=12) return false;
    return copy_text(scenario,sizeof scenario,lines[0]) &&
        (!strcmp(scenario,"unload") || !strcmp(scenario,"destroy")) &&
        copy_text(world,sizeof world,lines[1]) && copy_text(class_name,sizeof class_name,lines[2]) &&
        copy_text(actor_name,sizeof actor_name,lines[3]) && copy_text(part_name,sizeof part_name,lines[4]) &&
        hex(lines[5],&table_offset) && hex(lines[6],&end_stock) && hex(lines[7],&unregister_stock) &&
        hex(lines[8],&destroyed_stock) && copy_text(material_path,sizeof material_path,lines[9]) &&
        copy_text(clip_path,sizeof clip_path,lines[10]) && hex(lines[11],&timeout) &&
        table_offset && end_stock && unregister_stock && destroyed_stock && timeout>=120 && timeout<=30000;
}
static TSUCHI_Result call(TSUCHI_Object target, const char *name, const char *argument,
                          const TSUCHI_Value *value, TSUCHI_Value *out) {
    TSUCHI_PreparedCall prepared={0};
    TSUCHI_Result result=host->reflection->begin_call(self,target,name,&prepared);
    if (result!=TSUCHI_OK) return result;
    if (argument) result=host->reflection->call_set(self,prepared,argument,value);
    if (result==TSUCHI_OK) result=host->reflection->invoke(self,prepared);
    if (result==TSUCHI_OK && out) result=host->reflection->call_get(self,prepared,"ReturnValue",out);
    const TSUCHI_Result ended_call=host->reflection->end_call(self,prepared);
    return result==TSUCHI_OK ? ended_call : result;
}
static bool integer(TSUCHI_Object object,const char *path,int64_t *out) {
    TSUCHI_Value value={0};
    if (host->reflection->get(self,object,path,&value)!=TSUCHI_OK || value.kind!=TSUCHI_VALUE_INTEGER) return false;
    *out=value.as.integer; return true;
}
static bool object_field(TSUCHI_Object object,const char *path,TSUCHI_Object *out) {
    TSUCHI_Value value={0};
    if (host->reflection->get(self,object,path,&value)!=TSUCHI_OK || value.kind!=TSUCHI_VALUE_OBJECT) return false;
    *out=value.as.object; return true;
}
static TSUCHI_Result visible(bool *out) {
    TSUCHI_Value value={0};
    TSUCHI_Result result=host->reflection->get(self,tsuchi_component_as_object(part),"bVisible",&value);
    if (result==TSUCHI_OK && value.kind!=TSUCHI_VALUE_BOOLEAN) return TSUCHI_INVALID;
    if (result==TSUCHI_OK) *out=value.as.boolean;
    return result;
}
static TSUCHI_Result engine_material(TSUCHI_Object *out) {
    TSUCHI_Value index={0}, value={0}; index.kind=TSUCHI_VALUE_INTEGER;
    TSUCHI_Result result=call(tsuchi_component_as_object(part),"GetMaterial","ElementIndex",&index,&value);
    if (result==TSUCHI_OK && value.kind!=TSUCHI_VALUE_OBJECT) return TSUCHI_INVALID;
    if (result==TSUCHI_OK) *out=value.as.object;
    return result;
}
static void capture_receiver(void *actor,void *function,void *parameters) {
    /* Disarm before continuing, so nested ProcessEvent calls cannot replace this receiver. */
    if (atomic_exchange_explicit(&locating,false,memory_order_acq_rel))
        atomic_store_explicit(&located,(uint64_t)(uintptr_t)actor,memory_order_release);
    ((ProcessEvent)locator.original)(actor,function,parameters);
}
static void observe(void *actor,uint8_t kind,uint8_t reason) {
    if (!held || atomic_load_explicit(&observation.ready,memory_order_acquire) ||
        (uint64_t)(uintptr_t)actor!=raw_owner) return;
    if (atomic_flag_test_and_set_explicit(&sampling,memory_order_acquire)) return;
    bool hidden_now=true, owner_alive=false;
    const TSUCHI_Result hidden_result=host->component->hidden(self,part,&hidden_now);
    /* Temporary unregister/register keeps a hold. It is not teardown evidence. */
    if ((kind!=2 && (hidden_result!=TSUCHI_OK || hidden_now)) || (kind==1 && reason!=3)) {
        atomic_flag_clear_explicit(&sampling,memory_order_release); return;
    }
    observation.kind=kind; observation.reason=reason; observation.state=byte_at(raw_owner+0x5c);
    observation.handle=owner.id; observation.raw=raw_owner; observation.table=pointer_at(raw_owner);
    observation.owner_result=host->actor->alive(self,owner,&owner_alive);
    TSUCHI_Actor current_owner={0};
    observation.identity=observation.owner_result==TSUCHI_OK && owner_alive &&
        host->component->owner(self,part,&current_owner)==TSUCHI_OK && current_owner.id==owner.id &&
        observation.table==raw_table && index_at(raw_owner+0xc)==raw_index;
    observation.hidden=hidden_now; observation.hidden_result=hidden_result;
    observation.visible_result=visible(&observation.visible);
    TSUCHI_Clip current={0};
    observation.clip_result=host->animation->clip(self,part,&current);
    observation.has_clip=current.clip.id!=0;
    if (material_required) {
        TSUCHI_Object value={0};
        observation.material_matches=engine_material(&value)==TSUCHI_OK && value.id==original_material.id;
        observation.material=value.id;
    }
    if (animation_required) {
        int64_t mode=-1; TSUCHI_Object cls={0},instance={0},instance_class={0};
        observation.mode_matches=integer(tsuchi_component_as_object(part),"AnimationMode",&mode) && mode==original_mode &&
            object_field(tsuchi_component_as_object(part),"AnimClass",&cls) && cls.id==original_anim_class.id;
        observation.instance_matches=object_field(tsuchi_component_as_object(part),"AnimScriptInstance",&instance) && instance.id &&
            host->reflection->class_of(self,instance,&instance_class)==TSUCHI_OK && instance_class.id==original_instance_class.id;
        observation.instance_class=instance_class.id;
    }
    TSUCHI_Component parts[128]; uint64_t count=0;
    observation.list_result=host->component->list(self,owner,parts,128,&count);
    if (observation.list_result==TSUCHI_OK)
        for (uint64_t i=0;i<count;++i) if (parts[i].id==added.id) observation.added_present=true;
    atomic_store_explicit(&observation.ready,true,memory_order_release);
}
static void before_end(void *actor,uint8_t reason) {
    observe(actor,1,reason); ((EndPlay)ended.original)(actor,reason);
}
static void before_unregister(void *actor,bool only_tick) {
    if (!only_tick) observe(actor,0,0);
    ((Unregister)unregistered.original)(actor,only_tick);
}
static void before_destroyed(void *actor) {
    if (held && (uint64_t)(uintptr_t)actor==raw_owner)
        atomic_fetch_add_explicit(&native_destroy_count,1,memory_order_relaxed);
    observe(actor,2,0); ((Destroyed)destroyed.original)(actor);
}
static void on_birth(void *user,const TSUCHI_EntitySpawned *event) {
    (void)user;
    if (owner.id && event->actor.id==owner.id) atomic_fetch_add_explicit(&birth_count,1,memory_order_relaxed);
}
static void on_death(void *user,const TSUCHI_EntityDestroyed *event) {
    (void)user;
    if (owner.id && event->actor.id==owner.id) atomic_fetch_add_explicit(&death_count,1,memory_order_relaxed);
}
static void begin(unsigned at) { tsuchi_check_begin(checks[at]); }
static void end(unsigned at) { tsuchi_check_end(); finished[at]=true; }
static void log_sample(const char *label) {
    char line[TSUCHI_CHECK_LINE]="ordinary-lifetime "; tsuchi_check_add(line,label);
    tsuchi_check_add(line," scenario="),tsuchi_check_add(line,scenario);
    tsuchi_check_add(line," actor="),tsuchi_check_add_number(line,(int64_t)owner.id);
    tsuchi_check_add(line," receiver="),tsuchi_check_add_number(line,(int64_t)raw_owner);
    tsuchi_check_add(line," table="),tsuchi_check_add_number(line,(int64_t)raw_table);
    tsuchi_check_add(line," index="),tsuchi_check_add_number(line,(int64_t)raw_index);
    tsuchi_check_add(line," component="),tsuchi_check_add_number(line,(int64_t)part.id);
    tsuchi_check_add(line," material_required="),tsuchi_check_add_number(line,material_required);
    tsuchi_check_add(line," animation_required="),tsuchi_check_add_number(line,animation_required);
    tsuchi_check_add(line," original_visible="),tsuchi_check_add_number(line,original_visible);
    host->mod->log(self,line);
}
static bool qualify(void) {
    char current[TSUCHI_ID_SIZE]; uint64_t size=0;
    if (host->game->world(self,current,sizeof current,&size)!=TSUCHI_OK || strcmp(current,world)) return false;
    bool control=false;
    if (host->game->has_control(self,&control)!=TSUCHI_OK || !control) return false;
    TSUCHI_Object kind={0},game_kind={0}; TSUCHI_Actor candidates[128]; uint64_t count=0;
    if (host->reflection->find_class(self,class_name,&kind)!=TSUCHI_OK ||
        host->reflection->find_class(self,"GActor",&game_kind)!=TSUCHI_OK ||
        host->actor->list(self,kind,candidates,128,&count)!=TSUCHI_OK) return false;
    for (uint64_t i=0;i<count;++i) {
        char name[160]; bool gameplay=true;
        if (host->reflection->name(self,tsuchi_actor_as_object(candidates[i]),name,sizeof name,&size)!=TSUCHI_OK ||
            (strcmp(actor_name,"-") && strcmp(name,actor_name)) ||
            host->reflection->is_a(self,tsuchi_actor_as_object(candidates[i]),game_kind,&gameplay)!=TSUCHI_OK || gameplay) continue;
        TSUCHI_Component candidate={0};
        if (host->component->find(self,candidates[i],part_name,&candidate)!=TSUCHI_OK) continue;
        owner=candidates[i]; part=candidate;
        if (visible(&original_visible)!=TSUCHI_OK || !original_visible) continue;
        atomic_store_explicit(&located,0,memory_order_relaxed);
        atomic_store_explicit(&locating,true,memory_order_release);
        const TSUCHI_Result getter=call(tsuchi_actor_as_object(owner),"K2_GetActorLocation",0,0,0);
        atomic_store_explicit(&locating,false,memory_order_release);
        const uint64_t captured=atomic_load_explicit(&located,memory_order_acquire);
        if (getter!=TSUCHI_OK || !captured) continue;
        raw_owner=captured; raw_table=pointer_at(raw_owner); raw_index=index_at(raw_owner+0xc);
        /* These callbacks supplied the native receiver. The vtable offset is independently
         * checked by the three SDK hook stock comparisons during installation. */
        if (!raw_table) continue;
        if (material_required && (engine_material(&original_material)!=TSUCHI_OK || !original_material.id ||
            (replacement.id==0 && host->asset->load(self,material_path,&replacement)!=TSUCHI_OK) || replacement.id==original_material.id)) continue;
        if (animation_required) {
            TSUCHI_Object mesh={0},mesh_skeleton={0},clip_skeleton={0},instance={0};
            if (!integer(tsuchi_component_as_object(part),"AnimationMode",&original_mode) || original_mode!=0 ||
                !object_field(tsuchi_component_as_object(part),"AnimClass",&original_anim_class) || !original_anim_class.id ||
                !object_field(tsuchi_component_as_object(part),"AnimScriptInstance",&instance) || !instance.id ||
                host->reflection->class_of(self,instance,&original_instance_class)!=TSUCHI_OK ||
                !object_field(tsuchi_component_as_object(part),"SkeletalMesh",&mesh) || !mesh.id ||
                !object_field(mesh,"Skeleton",&mesh_skeleton) || !mesh_skeleton.id ||
                (clip.id==0 && host->asset->load(self,clip_path,&clip)!=TSUCHI_OK) ||
                !object_field(clip,"Skeleton",&clip_skeleton) || clip_skeleton.id!=mesh_skeleton.id) continue;
        }
        begin(1);
        TSUCHI_Actor current_owner={0}; int32_t gameplay_team=0;
        tsuchi_expect_result(host->component->owner(self,part,&current_owner),TSUCHI_OK,"the selected part's owner");
        tsuchi_expect(current_owner.id==owner.id,"the same ordinary Actor generation owns the selected part");
        tsuchi_expect_result(host->entity->team(self,owner,&gameplay_team),TSUCHI_INVALID,"this owner is not a GActor");
        log_sample("qualified"); end(1); return true;
    }
    owner.id=part.id=raw_owner=raw_table=0; return false;
}
static bool take_holds(void) {
    begin(2);
    TSUCHI_Object scene={0};
    bool good=tsuchi_expect_result(host->reflection->find_class(self,"SceneComponent",&scene),TSUCHI_OK,"the added component class");
    if (good) good=tsuchi_expect_result(host->component->add(self,owner,scene,part,"",&added),TSUCHI_OK,"an observer-owned addition");
    if (good) good=tsuchi_expect_result(host->component->hold_hidden(self,part,false),TSUCHI_OK,"the ordinary visibility hold");
    if (good && material_required) good=tsuchi_expect_result(host->component->hold_material(self,part,0,replacement),TSUCHI_OK,"a distinct ordinary material hold");
    if (good && animation_required) good=tsuchi_expect_result(host->animation->hold_clip(self,part,clip,true,0),TSUCHI_OK,"the matching ordinary clip hold");
    bool native_visible=true, hidden_now=false;
    good=tsuchi_expect_result(visible(&native_visible),TSUCHI_OK,"native held visibility") && good;
    good=tsuchi_expect_result(host->component->hidden(self,part,&hidden_now),TSUCHI_OK,"the visibility hold") && good;
    good=tsuchi_expect(!native_visible && hidden_now,"visibility differs from the measured original") && good;
    if (material_required) {
        TSUCHI_Object value={0}; good=tsuchi_expect_result(engine_material(&value),TSUCHI_OK,"native held material") && good;
        good=tsuchi_expect(value.id==replacement.id && value.id!=original_material.id,"the native material changed") && good;
    }
    if (animation_required) {
        TSUCHI_Clip value={0}; int64_t mode=-1;
        good=tsuchi_expect_result(host->animation->clip(self,part,&value),TSUCHI_OK,"the held clip") && good;
        good=tsuchi_expect(value.clip.id==clip.id && integer(tsuchi_component_as_object(part),"AnimationMode",&mode) && mode==1,
                          "the native mesh changed from a running Blueprint to the requested single clip") && good;
    }
    end(2); held=good; held_at=steps;
    if (good) log_sample("held-ready");
    else {
        /* A failed fixture setup leaves no successful partial hold behind. */
        if (part.id) {
            if (animation_required) host->animation->release_clip(self,part);
            if (material_required) host->component->release_material(self,part,0);
            host->component->release_hidden(self,part);
        }
        if (added.id) host->component->remove(self,added);
    }
    return good;
}
static void report(void) {
    const bool destroying=!strcmp(scenario,"destroy");
    begin(3);
    tsuchi_expect(observation.identity,"the captured native receiver still owns the same live component generation");
    tsuchi_expect(observation.handle==owner.id && observation.raw==raw_owner && observation.table==raw_table,
                  "the pre-original observation has the qualified owner's identity");
    tsuchi_expect(destroying ? observation.kind==2 && (observation.state&8)!=0 : observation.kind<2,
                  "the observer saw the configured admitted lifetime route before its original");
    end(3);
    begin(4);
    tsuchi_expect_result(observation.hidden_result,TSUCHI_OK,"visibility bookkeeping before native teardown");
    tsuchi_expect(!observation.hidden,"no visibility hold survives admitted teardown");
    if (animation_required) {
        tsuchi_expect_result(observation.clip_result,TSUCHI_OK,"clip bookkeeping before native teardown");
        tsuchi_expect(!observation.has_clip,"no clip hold survives admitted teardown");
    }
    tsuchi_expect_result(host->component->release_hidden(self,part),TSUCHI_NOT_FOUND,"the visibility lease ended automatically");
    if (material_required) tsuchi_expect_result(host->component->release_material(self,part,0),TSUCHI_NOT_FOUND,"the material lease ended automatically");
    if (animation_required) tsuchi_expect_result(host->animation->release_clip(self,part),TSUCHI_NOT_FOUND,"the clip lease ended automatically");
    end(4);
    begin(5);
    if (!destroying) {
        tsuchi_expect_result(observation.visible_result,TSUCHI_OK,"surviving visibility before native teardown");
        tsuchi_expect(observation.visible==original_visible,"surviving native visibility was restored before teardown");
        if (material_required) tsuchi_expect(observation.material_matches,"the surviving native material was restored before teardown");
        if (animation_required) tsuchi_expect(observation.mode_matches && observation.instance_matches,
            "the same Blueprint class has resumed on the surviving mesh before teardown");
    } else {
        tsuchi_expect_number(atomic_load(&native_destroy_count),1,"one accepted native Destroyed callback for the selected owner");
        /* Root policy forgets parts that are being destroyed. Changed native resources
         * need not be restored on those parts, and this branch makes no such claim. */
        tsuchi_expect((observation.state&8)!=0,"the engine had accepted destruction before bookkeeping was forgotten");
    }
    end(5);
    begin(6);
    tsuchi_expect_result(observation.list_result,TSUCHI_OK,"the owner's parts before original teardown");
    if (!destroying) tsuchi_expect(!observation.added_present,"the owned addition left before surviving owner parts unregister");
    tsuchi_expect_result(host->component->remove(self,added),TSUCHI_NOT_FOUND,"the observer's addition has no remaining lease");
    /* This proves absence/bookkeeping only. It does not count DestroyComponent calls. */
    end(6);
    begin(7);
    tsuchi_expect_number(atomic_load(&birth_count),0,"ordinary-owner gameplay birth events");
    tsuchi_expect_number(atomic_load(&death_count),0,"ordinary-owner gameplay removal events");
    end(7);
    char line[TSUCHI_CHECK_LINE]="ordinary-lifetime before-original kind=";
    tsuchi_check_add_number(line,observation.kind);
    tsuchi_check_add(line," reason="),tsuchi_check_add_number(line,observation.reason);
    tsuchi_check_add(line," actor_state="),tsuchi_check_add_number(line,observation.state);
    tsuchi_check_add(line," visible="),tsuchi_check_add_number(line,observation.visible);
    tsuchi_check_add(line," material="),tsuchi_check_add_number(line,(int64_t)observation.material);
    tsuchi_check_add(line," blueprint_instance_class="),tsuchi_check_add_number(line,(int64_t)observation.instance_class);
    host->mod->log(self,line);
}
static void on_step(void *user,const TSUCHI_TimeStep *event) {
    (void)user; (void)event; ++steps;
    if (atomic_load_explicit(&observation.ready,memory_order_acquire)) {
        bool retired=!strcmp(scenario,"destroy");
        if (!retired) {
            TSUCHI_Object kind={0}; TSUCHI_Actor candidates[128]; uint64_t count=0;
            if (host->reflection->find_class(self,class_name,&kind)==TSUCHI_OK &&
                host->actor->list(self,kind,candidates,128,&count)==TSUCHI_OK) {
                retired=true;
                for (uint64_t i=0;i<count;++i) if (candidates[i].id==owner.id) retired=false;
            }
        }
        if (retired) { report(); host->cancel(self,stepping); return; }
        if (steps<timeout) {
            tsuchi_check_waiting("before_native","the same owner leaving the engine's active world list");
            return;
        }
    }
    if (!held && !finished[2]) {
        if (!owner.id && !qualify()) tsuchi_check_waiting("donor","the configured ordinary owner and measured native resources");
        else if (owner.id && !take_holds()) {
            for (unsigned i=3;i<8;++i) {
                begin(i); tsuchi_expect(false,"the ordinary resource setup did not pass"); end(i);
            }
            host->cancel(self,stepping); return;
        }
    }
    if (held && !strcmp(scenario,"destroy") && !destroy_asked && steps-held_at>=30) {
        destroy_asked=true;
        TSUCHI_Result result=call(tsuchi_actor_as_object(owner),"K2_DestroyActor",0,0,0);
        char line[TSUCHI_CHECK_LINE]="ordinary-lifetime K2_DestroyActor invocation=";
        tsuchi_check_add(line,tsuchi_check_result_name(result)); host->mod->log(self,line);
        /* Successful reflection invocation is not accepted-destruction evidence. */
    }
    if (steps>=timeout) {
        for (unsigned i=1;i<8;++i) if (!finished[i]) {
            begin(i); tsuchi_expect(false,"the configured donor or native teardown was not observed before the deadline"); end(i);
        }
        host->cancel(self,stepping);
    }
}
TSUCHI_Result tsuchi_mod_start(const TSUCHI_Host *given,TSUCHI_Mod *mod) {
    host=given; self=mod; tsuchi_check_start(host,self);
    for (unsigned i=0;i<8;++i) tsuchi_check_plan(checks[i]);
    configured=config(); material_required=strcmp(material_path,"-")!=0; animation_required=strcmp(clip_path,"-")!=0;
    begin(0); bool good=tsuchi_expect(configured,"twelve explicit qualified inputs in targets.txt");
    if (good) good=tsuchi_expect_result(host->hook->wrap_address(self,table_offset+0x228,0,(void*)capture_receiver,&locator),TSUCHI_OK,"the native receiver locator") && good;
    if (good) good=tsuchi_expect_result(host->hook->wrap_address(self,table_offset+0x348,end_stock,(void*)before_end,&ended),TSUCHI_OK,"EndPlay observer before any ordinary hold") && good;
    if (good) good=tsuchi_expect_result(host->hook->wrap_address(self,table_offset+0x4e0,unregister_stock,(void*)before_unregister,&unregistered),TSUCHI_OK,"unregister observer before any ordinary hold") && good;
    if (good) good=tsuchi_expect_result(host->hook->wrap_address(self,table_offset+0x548,destroyed_stock,(void*)before_destroyed,&destroyed),TSUCHI_OK,"admitted destruction observer before any ordinary hold") && good;
    if (good) good=tsuchi_expect_result(host->entity->on_spawned(self,on_birth,0,&births),TSUCHI_OK,"unchanged gameplay birth subscription") && good;
    if (good) good=tsuchi_expect_result(host->entity->on_destroyed(self,on_death,0,&deaths),TSUCHI_OK,"unchanged gameplay removal subscription") && good;
    if (good) good=tsuchi_expect_result(host->time->on_step(self,on_step,0,&stepping),TSUCHI_OK,"the bounded game-step observer") && good;
    end(0);
    if (!good) {
        for (unsigned i=1;i<8;++i) { begin(i); tsuchi_expect(false,"the ordinary observer did not install"); end(i); }
        return TSUCHI_NOT_READY;
    }
    return TSUCHI_OK;
}
