/* The normal loader, rather than an SDK/private stop call, cleans this producer up. */
#include "../common.h"
TSUCHI_Result tsuchi_mod_start(const TSUCHI_Host *host,TSUCHI_Mod *mod) {
    OL_Context c={host,mod};
    char version[32]; uint64_t length=0;
    /* This non-check dependency is staged for other selections as well. It must
     * not plan or report a check when its explicit observer is absent/refused. */
    if (host->mod->version_of(mod,"check.ordinary-lifetime-startup",version,sizeof version,&length)!=TSUCHI_OK)
        return TSUCHI_NOT_READY;
    tsuchi_check_start(host,mod); tsuchi_check_plan("applied");
    tsuchi_check_begin("applied");
    bool good=true;
    TSUCHI_Object actor_class={0},mesh={0},clip={0},replacement={0},skeletal={0};
    if (good) good=tsuchi_expect_result(host->asset->load(mod,ol_actor_path,&actor_class),TSUCHI_OK,"the same ordinary owner class");
    if (good) good=tsuchi_expect_result(host->asset->load(mod,ol_mesh_path,&mesh),TSUCHI_OK,"the same skeletal mesh");
    if (good) good=tsuchi_expect_result(host->asset->load(mod,ol_clip_path,&clip),TSUCHI_OK,"the compatible clip asset");
    if (good) good=tsuchi_expect_result(host->asset->load(mod,ol_material_path,&replacement),TSUCHI_OK,"the replacement material");
    if (good) good=tsuchi_expect_result(host->reflection->find_class(mod,"SkeletalMeshComponent",&skeletal),TSUCHI_OK,"the component class");
    TSUCHI_Actor candidates[128],owner={0}; uint64_t count=0;
    if (good) good=tsuchi_expect_result(host->actor->list(mod,actor_class,candidates,128,&count),TSUCHI_OK,"the current native ordinary owners");
    unsigned matches=0;
    if (good) for (uint64_t i=0;i<count;++i) {
        TSUCHI_Transform pose={0};
        if (host->actor->transform(mod,candidates[i],&pose)==TSUCHI_OK && ol_at_place(&pose)) { owner=candidates[i]; ++matches; }
    }
    if (good) good=tsuchi_expect(matches==1,"exactly one native ordinary owner occupies the observer's authored position");
    TSUCHI_Component component={0},parts[128]; count=0; matches=0;
    if (good) good=tsuchi_expect_result(host->component->list(mod,owner,parts,128,&count),TSUCHI_OK,"the authored owner's live components");
    if (good) for (uint64_t i=0;i<count;++i) {
        bool is_skeletal=false; TSUCHI_Object got_mesh={0};
        if (host->reflection->is_a(mod,tsuchi_component_as_object(parts[i]),skeletal,&is_skeletal)==TSUCHI_OK && is_skeletal &&
            ol_get_object(c,tsuchi_component_as_object(parts[i]),"SkeletalMesh",&got_mesh) && got_mesh.id==mesh.id) { component=parts[i]; ++matches; }
    }
    if (good) good=tsuchi_expect(matches==1,"exactly one observer-owned matching skeletal component survives on that owner");
    OL_Snapshot before={0};
    if (good) good=tsuchi_expect(ol_snapshot(c,owner,component,clip,&before) && before.visible && before.mode==0 &&
        !before.anim_class.id && !before.instance.id && before.material.id!=replacement.id,
        "the independent pre-hold native snapshot matches the authored empty-Blueprint fixture");
    if (good) good=tsuchi_expect_result(host->component->hold_hidden(mod,component,false),TSUCHI_OK,"hide the observer-owned ordinary component");
    if (good) good=tsuchi_expect_result(host->component->hold_material(mod,component,0,replacement),TSUCHI_OK,"hold the distinct material on the ordinary component");
    if (good) good=tsuchi_expect_result(host->animation->hold_clip(mod,component,clip,true,0),TSUCHI_OK,"hold the matching clip on the ordinary component");
    if (good) {
        bool visible=true,hidden=false; TSUCHI_Object shown={0}; TSUCHI_Clip held={0}; int64_t mode=-1;
        good=tsuchi_expect(ol_get_visible(c,component,&visible) && !visible &&
            host->component->hidden(mod,component,&hidden)==TSUCHI_OK && hidden &&
            ol_material(c,component,&shown) && shown.id==replacement.id && shown.id!=before.material.id &&
            host->animation->clip(mod,component,&held)==TSUCHI_OK && held.clip.id==clip.id && held.rate==0 && held.looping &&
            ol_get_integer(c,tsuchi_component_as_object(component),"AnimationMode",&mode) && mode==1,
            "native visibility, material, and animation mode changed before ordinary startup refusal");
    }
    ol_log_id(c,"producer_owner",owner.id); ol_log_id(c,"producer_component",component.id);
    ol_log_id(c,"producer_original_material",before.material.id); ol_log_id(c,"producer_original_skeleton",before.mesh_skeleton.id);
    tsuchi_check_end();
    /* Even failed partial setup follows the loader's normal startup cleanup path.
     * No release/remove/stop call hides whether the framework cleans these holds up. */
    return TSUCHI_NOT_READY;
}
