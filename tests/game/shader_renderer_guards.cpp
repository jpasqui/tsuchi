// Real reflected renderer setters against the shared host engine fixture.
// This qualifies dispatch and call admission, not native pixels or UI ownership.
#include <cstdio>
#include <vector>
#include "parts.h"
#include "reflect.h"
#include "reflect_world.h"
namespace {
using namespace tsuchi::game;
namespace t = table;
int failures = 0, writes = 0;
bool permitted = true;
uint64_t last_object = 0, last_resource = 0;
int32_t last_slot = -1;
#define CHECK(test) do { if (!(test)) { std::printf("%s:%d: %s\n", __FILE__, __LINE__, #test); ++failures; } } while (0)
void* lookup_and_veto(void* type, uint64_t name, uint64_t parents) {
    void* result = fake::engine_find_function(type, name, parents);
    permitted = false;
    return result;
}
}
int main() {
    build_world();
    const uint64_t in = t::constant::property_param;
    const uint64_t component = fake::engine_object(u"ActorComponent", w.root);
    const uint64_t primitive = fake::engine_object(u"PrimitiveComponent", w.root);
    fake::set_super(primitive, component);
    const uint64_t material_type = fake::engine_object(u"MaterialInterface", w.root);
    const uint64_t material = fake::engine_object(u"OriginalMaterial", material_type);
    const uint64_t texture = fake::engine_object(u"OriginalBrushTexture", w.component);
    const uint64_t mesh_type = fake::engine_object(u"MeshComponent", w.root);
    fake::set_super(mesh_type, primitive);
    const uint64_t particle_type = fake::engine_object(u"ParticleSystemComponent", w.root);
    fake::set_super(particle_type, primitive);
    const uint64_t decal_type = fake::engine_object(u"DecalComponent", w.root);
    fake::set_super(decal_type, component);
    const uint64_t image_type = fake::engine_object(u"Image", w.root);
    const uint64_t set_material = fake::function(primitive, w.function_class, u"SetMaterial", 16, 8,
        [](uint64_t object, uint64_t args) {
            ++writes; last_object = object; last_slot = fake::get<int32_t>(args);
            last_resource = fake::get<uint64_t>(args + 8);
        });
    fake::flag(fake::field(set_material,u"ElementIndex",u"IntProperty",0,4),in);
    fake::flag(fake::field(set_material,u"Material",u"ObjectProperty",8,8,material_type),in);
    for (const auto& entry : std::vector<std::pair<uint64_t,std::u16string>>{
        {decal_type,u"SetDecalMaterial"},{image_type,u"SetBrushResourceObject"}}) {
        const uint64_t setter = fake::function(entry.first,w.function_class,entry.second,8,8,
            [](uint64_t object,uint64_t args) { ++writes; last_object=object;last_slot=0;last_resource=fake::get<uint64_t>(args); });
        fake::flag(fake::field(setter,entry.first==image_type?u"ResourceObject":u"NewDecalMaterial",u"ObjectProperty",0,8),in);
    }
    const CallGuard guard{[](void*) { return permitted; },nullptr};
    uint64_t particle = 0;
    for (uint64_t type : {mesh_type,particle_type,decal_type,image_type}) {
        const uint64_t renderer = fake::engine_object(u"Renderer",type);
        if(type==particle_type) particle=renderer;
        for(uint64_t resource : {material,uint64_t(0)}) {
            permitted=true;
            CHECK(restore_slot_material(renderer,0,resource,guard));
            CHECK(last_object==renderer && last_resource==resource && last_slot==0);
            const int before=writes;
            permitted=false;
            CHECK(!restore_slot_material(renderer,0,resource,guard) && writes==before);
        }
        if(type==image_type) {
            permitted=true;
            CHECK(restore_slot_material(renderer,0,texture,guard) && last_resource==texture);
        }
        if(type==image_type || type==decal_type) {
            permitted=true;const int before=writes;
            CHECK(!restore_slot_material(renderer,1,material,guard) && writes==before);
        }
    }
    permitted=true;
    const int before=writes;
    const auto lookup=t::fn::class_find_function;
    t::fn::class_find_function=lookup_and_veto;
    CHECK(!restore_slot_material(particle,0,material,guard) && writes==before);
    t::fn::class_find_function=lookup;
    if(!failures) std::puts("Guarded mesh/PSC/decal/Image setter controls passed");
    return failures;
}
