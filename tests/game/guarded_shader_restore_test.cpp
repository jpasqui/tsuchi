// Actual restoration-only shader resolver and native EngineCall boundaries.
#include "shader_materials.cpp"
#include "reflect_world.h"
#include <cstdio>
uint64_t fixture_shader_world=0;
// Unused manifest/frame/shader-application paths are explicit aborting stubs.
// Any accidental entry fails this focused resolver test.
namespace tsuchi {
const TSUCHI_Host host{};
bool add_manifest_section(const ManifestSection&) { std::abort(); }
bool each_frame(void (*)()) { std::abort(); }
bool on_actor_entered(void (*)(uint64_t,uint64_t)) { std::abort(); }
bool mod_file_path(Texts&,const TSUCHI_Mod*,const char*) { std::abort(); }
uint64_t shader_slot_source(uint64_t,uint32_t) { std::abort(); }
TSUCHI_Result restore_material(TSUCHI_Mod*,uint64_t,uint32_t) { std::abort(); }
TSUCHI_Result hold_material(TSUCHI_Mod*,uint64_t,uint32_t,uint64_t) { std::abort(); }
}
namespace tsuchi::game {
bool path_stays_within(const char*) { std::abort(); }
TSUCHI_Result file_size(const char*, uint64_t&) { return TSUCHI_NOT_FOUND; }
TSUCHI_Result read_whole_file(const char*,char*&,uint64_t&) { std::abort(); }
uint64_t loaded_world() { return fixture_shader_world; }
uint64_t table_entry(uint64_t,uint64_t) { std::abort(); }
}
using namespace tsuchi;
namespace {
int failures=0, phase=0, copies=0, creates=0;
bool owner_live=true;
uint64_t made=0;
#define CHECK(x) do { if (!(x)) { std::printf("line%d: %s\n",__LINE__,#x);++failures; } } while(0)
bool allowed(void*) { return owner_live; }
void callback(bool copying) {
    if ((!copying && phase==1) || (copying && phase==2)) owner_live=false;
    if ((!copying && phase==3) || (copying && phase==4)) sets.count=0;
    if (!copying && phase==5) {
        // Relocate provider arrays during creation without ending the provider.
        Material* previous=sets[0].materials;
        auto* replacement=static_cast<Material*>(allocate(sizeof(Material)));
        new (replacement) Material{};
        replacement->stage=previous->stage; replacement->copy=previous->copy; replacement->plain=previous->plain;
        sets[0].materials=replacement;
        previous->stage=Stage::failed;
    }
}
}
int main() {
    build_world();
    const uint64_t library=fake::engine_object(u"KismetMaterialLibrary",w.root);
    const uint64_t pattern=fake::engine_object(u"LibraryDefault",library);
    fake::put<uint64_t>(library+fake::t::class_type::default_object,pattern);
    const uint64_t material_class=fake::engine_object(u"MaterialInstanceConstant",w.root);
    fake::field(material_class,u"Parent",u"ObjectProperty",0xa0,8,material_class);
    made=fake::engine_object(u"Made",material_class);
    const uint64_t input=fake::t::constant::property_param;
    const uint64_t output=input|fake::t::constant::property_out|fake::t::constant::property_return;
    const uint64_t create=fake::function(library,w.function_class,u"CreateDynamicMaterialInstance",24,8,[](uint64_t,uint64_t args){
        ++creates;fake::put<uint64_t>(args+16,made);callback(false);
    });
    fake::flag(fake::field(create,u"WorldContextObject",u"ObjectProperty",0,8,w.actor),input);
    fake::flag(fake::field(create,u"Parent",u"ObjectProperty",8,8,material_class),input);
    fake::flag(fake::field(create,u"ReturnValue",u"ObjectProperty",16,8,material_class),output);
    const uint64_t copy=fake::function(material_class,w.function_class,u"CopyParameterOverrides",8,8,[](uint64_t,uint64_t){++copies;callback(true);});
    fake::flag(fake::field(copy,u"MaterialInstance",u"ObjectProperty",0,8,material_class),input);
    const uint64_t plain=game::to_handle(fake::engine_object(u"Plain",material_class));
    const uint64_t original_copy=game::to_handle(fake::engine_object(u"Copy",material_class));
    world_now=game::to_handle(fake::engine_object(u"ShaderWorld",w.actor));
    fixture_shader_world=game::from_handle(world_now);
    sets.add(Set{}); sets[0].id=19; sets[0].count=1;
    auto* row=static_cast<Material*>(allocate(sizeof(Material)));new (row) Material{};
    for (int step=0;step<=6;++step) {
        phase=step;owner_live=true;sets.count=1;sets[0].materials=row;
        row->stage=Stage::ready;row->copy=original_copy;row->plain=plain;row->stand_in=0;
        const int before=copies, before_create=creates, before_alloc=fake::engine().allocated,before_free=fake::engine().freed;
        uint64_t shown=0;
        if (step==6) { row->stand_in=game::to_handle(made); }
        const bool resolved=drawn_as_restoring(game::from_handle(plain),{allowed,nullptr},shown);
        CHECK(resolved==(step==0 || step==5 || step==6));
        CHECK(copies-before==(step==0 || step==2 || step==4 || step==5 ? 1 : 0));
        CHECK(creates-before_create==(step==6 ? 0 : 1));
        CHECK(fake::engine().allocated-before_alloc==fake::engine().freed-before_free);
        if (resolved) CHECK(shown==made);
        if (step==5) { CHECK(sets[0].materials!=row && game::from_handle(sets[0].materials->stand_in)==made); }
        if (step==1 || step==2) CHECK(row->stand_in==0);
    }
    return failures;
}
