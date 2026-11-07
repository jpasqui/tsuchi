/* Shared SDK helpers. Script parameter names are captured reflection facts, not native prototypes. */
#ifndef ORDINARY_STARTUP_COMMON_H
#define ORDINARY_STARTUP_COMMON_H
#include "tsuchi/tsuchi.h"
#include "tsuchi_check.h"
static const char ol_actor_path[]="/Game/Carrot4/Placeables/Lights/BP_PointLight.BP_PointLight_C";
static const char ol_mesh_path[]="/Game/Carrot4/Characters/Teki/Chappy/SK_chr_Chappy_00.SK_chr_Chappy_00";
static const char ol_clip_path[]="/Game/Carrot4/Characters/Teki/Chappy/Animation/AS_Chappy_Wait.AS_Chappy_Wait";
static const char ol_material_path[]="/Engine/EngineMaterials/DefaultMaterial.DefaultMaterial";
static const TSUCHI_Vec3 ol_place={31000,32000,33000};
typedef struct OL_Context { const TSUCHI_Host *host; TSUCHI_Mod *self; } OL_Context;
typedef struct OL_Snapshot {
    TSUCHI_Actor owner;
    TSUCHI_Component component;
    TSUCHI_Object mesh,mesh_skeleton,clip_skeleton,material,anim_class,instance;
    int64_t mode;
    bool visible;
} OL_Snapshot;
static inline TSUCHI_Value ol_object(TSUCHI_Object object) { TSUCHI_Value v={0}; v.kind=TSUCHI_VALUE_OBJECT; v.as.object=object; return v; }
static inline TSUCHI_Value ol_number(double number) { TSUCHI_Value v={0}; v.kind=TSUCHI_VALUE_NUMBER; v.as.number=number; return v; }
static inline TSUCHI_Value ol_integer(int64_t integer) { TSUCHI_Value v={0}; v.kind=TSUCHI_VALUE_INTEGER; v.as.integer=integer; return v; }
static inline TSUCHI_Value ol_boolean(bool boolean) { TSUCHI_Value v={0}; v.kind=TSUCHI_VALUE_BOOLEAN; v.as.boolean=boolean; return v; }
static inline bool ol_get_object(OL_Context c,TSUCHI_Object object,const char *field,TSUCHI_Object *out) {
    TSUCHI_Value value={0};
    if (c.host->reflection->get(c.self,object,field,&value)!=TSUCHI_OK) return false;
    if (value.kind==TSUCHI_VALUE_NONE) { out->id=0; return true; }
    if (value.kind!=TSUCHI_VALUE_OBJECT) return false;
    *out=value.as.object; return true;
}
static inline bool ol_get_integer(OL_Context c,TSUCHI_Object object,const char *field,int64_t *out) {
    TSUCHI_Value value={0};
    if (c.host->reflection->get(c.self,object,field,&value)!=TSUCHI_OK || value.kind!=TSUCHI_VALUE_INTEGER) return false;
    *out=value.as.integer; return true;
}
static inline bool ol_get_visible(OL_Context c,TSUCHI_Component component,bool *out) {
    TSUCHI_Value value={0};
    if (c.host->reflection->get(c.self,tsuchi_component_as_object(component),"bVisible",&value)!=TSUCHI_OK || value.kind!=TSUCHI_VALUE_BOOLEAN) return false;
    *out=value.as.boolean; return true;
}
static inline TSUCHI_Result ol_call(OL_Context c,TSUCHI_Object target,const char *name,
                                    const char *argument,const TSUCHI_Value *value,TSUCHI_Value *out) {
    TSUCHI_PreparedCall call={0};
    TSUCHI_Result result=c.host->reflection->begin_call(c.self,target,name,&call);
    if (result!=TSUCHI_OK) return result;
    if (argument) result=c.host->reflection->call_set(c.self,call,argument,value);
    if (result==TSUCHI_OK) result=c.host->reflection->invoke(c.self,call);
    if (result==TSUCHI_OK && out) result=c.host->reflection->call_get(c.self,call,"ReturnValue",out);
    const TSUCHI_Result ended=c.host->reflection->end_call(c.self,call);
    return result==TSUCHI_OK ? ended : result;
}
static inline bool ol_material(OL_Context c,TSUCHI_Component component,TSUCHI_Object *out) {
    TSUCHI_Value zero=ol_integer(0),value={0};
    if (ol_call(c,tsuchi_component_as_object(component),"GetMaterial","ElementIndex",&zero,&value)!=TSUCHI_OK || value.kind!=TSUCHI_VALUE_OBJECT) return false;
    *out=value.as.object; return out->id!=0;
}
static inline bool ol_snapshot(OL_Context c,TSUCHI_Actor owner,TSUCHI_Component component,TSUCHI_Object clip,OL_Snapshot *out) {
    memset(out,0,sizeof *out); out->owner=owner; out->component=component;
    TSUCHI_Actor current_owner={0}; bool alive=false;
    const TSUCHI_Object object=tsuchi_component_as_object(component);
    return c.host->actor->alive(c.self,owner,&alive)==TSUCHI_OK && alive &&
        c.host->component->owner(c.self,component,&current_owner)==TSUCHI_OK && current_owner.id==owner.id &&
        ol_get_visible(c,component,&out->visible) && ol_material(c,component,&out->material) &&
        ol_get_integer(c,object,"AnimationMode",&out->mode) &&
        ol_get_object(c,object,"AnimClass",&out->anim_class) && ol_get_object(c,object,"AnimScriptInstance",&out->instance) &&
        ol_get_object(c,object,"SkeletalMesh",&out->mesh) && out->mesh.id &&
        ol_get_object(c,out->mesh,"Skeleton",&out->mesh_skeleton) && out->mesh_skeleton.id &&
        ol_get_object(c,clip,"Skeleton",&out->clip_skeleton) && out->mesh_skeleton.id==out->clip_skeleton.id;
}
static inline bool ol_at_place(const TSUCHI_Transform *pose) {
    const double dx=pose->location.x-ol_place.x,dy=pose->location.y-ol_place.y,dz=pose->location.z-ol_place.z;
    return dx>-0.1 && dx<0.1 && dy>-0.1 && dy<0.1 && dz>-0.1 && dz<0.1;
}
static inline void ol_log_id(OL_Context c,const char *name,uint64_t value) {
    char line[TSUCHI_CHECK_LINE]="ordinary-startup "; tsuchi_check_add(line,name); tsuchi_check_add(line,"=");
    tsuchi_check_add_number(line,(int64_t)value); c.host->mod->log(c.self,line);
}
static inline bool ol_actor_number(OL_Context c,TSUCHI_Actor actor,const char *name,const char *field,double *out) {
    TSUCHI_PreparedCall call={0}; TSUCHI_Value value={0};
    TSUCHI_Result result=c.host->reflection->begin_call(c.self,tsuchi_actor_as_object(actor),name,&call);
    if (result!=TSUCHI_OK) return false;
    result=c.host->reflection->invoke(c.self,call);
    if (result==TSUCHI_OK) result=c.host->reflection->call_get(c.self,call,field,&value);
    const TSUCHI_Result ended=c.host->reflection->end_call(c.self,call);
    if (result!=TSUCHI_OK || ended!=TSUCHI_OK || value.kind!=TSUCHI_VALUE_NUMBER) return false;
    *out=value.as.number; return true;
}
#endif
