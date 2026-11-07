// Executes production shader cleanup and refresh against native setter/copy callbacks.
// The material hold endpoint is controlled: it removes the hold before calling
// the actual reflected setter, matching HoldTable.end's published contract.
#include "shader_materials.cpp"
#include "reflect_world.h"
#include <cstdio>
#include <cstdlib>
#include <map>
namespace fixture {
int failures=0, writes=0;
uint64_t world=0, source=0, renderer=0;
TSUCHI_Mod* stop_provider=nullptr;
void* retired=nullptr;
}

using namespace tsuchi;
#define CHECK(x) do { if (!(x)) { std::printf("line%d: %s\n",__LINE__,#x); std::fflush(stdout); ++fixture::failures; } } while(0)
namespace fixture {
std::map<uint64_t,uint64_t> holds;
std::map<uint64_t,int> copied;
std::map<void*,size_t> blocks;
uint64_t direct=0, child=0, shown=0;
Material* watched=nullptr;
bool stop_in_setter=false, swap_in_copy=false, released_rows=false;
TSUCHI_Mod* remove_provider=nullptr;
uint64_t swap_on=0, replacement_source=0;
constexpr uint32_t marker=0x51515151;
uint64_t material_type=0, dynamic_type=0;
void* allocate(size_t bytes) { void* p=std::malloc(bytes); blocks[p]=bytes; return p; }
void release(void* p) {
    if(!p) return;
    if(p==retired) {
        released_rows=true;
        std::memset(p,0,blocks[p]);
        static_cast<Material*>(p)->asked_at=marker; // detects any post-release record write
        return;
    }
    blocks.erase(p);std::free(p);
}
Set* find(uint64_t id) { for(uint32_t i=0;i<sets.count;++i) if(sets[i].id==id)return &sets[i]; return nullptr; }
uint64_t set(const char* name,uint32_t rows) {
    TSUCHI_Mod* mod=mod_create(name);CHECK(mod);
    const uint64_t id=next_set++;
    Set value{};value.id=id;value.mod=mod;value.count=rows;
    value.materials=static_cast<Material*>(tsuchi::allocate(sizeof(Material)*(rows+1)));
    for(uint32_t i=0;i<=rows;++i)new (value.materials+i)Material{};
    CHECK(sets.add(value));++counts.sets;counts.materials+=rows;
    CHECK(own(mod,kSet,id,end_set,nullptr)==TSUCHI_OK);
    return id;
}
void instance(Material& row,const std::u16string& name) {
    row.stage=Stage::ready;
    row.plain=game::to_handle(fake::engine_object(name,material_type));
    row.copy=game::to_handle(fake::engine_object(u"CopiedParent",material_type));
    const uint64_t child=fake::engine_object(u"LiveChild",dynamic_type);
    fake::put(child+0x80,game::from_handle(row.plain));
    const uint64_t mid=fake::engine_object(u"ShownChild",dynamic_type);
    const uint64_t child_handle=game::to_handle(child),mid_handle=game::to_handle(mid);
    CHECK(retain_game_object(child_handle) && retain_game_object(mid_handle));
    CHECK(row.derived.add(Derived{child_handle,mid_handle,0,true,0})); // no values signed yet: the next refresh copies
}
}
namespace tsuchi {
const TSUCHI_Host host{};
bool add_manifest_section(const ManifestSection&) {std::abort();}
bool each_frame(void (*)()) {std::abort();}
bool on_actor_entered(void (*)(uint64_t,uint64_t)) {std::abort();}
bool mod_file_path(Texts&,const TSUCHI_Mod*,const char*) {std::abort();}
TSUCHI_Result hold_material(TSUCHI_Mod*,uint64_t,uint32_t,uint64_t) {std::abort();}
uint64_t shader_slot_source(uint64_t object,uint32_t) {
    auto at=fixture::holds.find(object);return at==fixture::holds.end()?0:at->second;
}
TSUCHI_Result restore_material(TSUCHI_Mod*,uint64_t object,uint32_t slot) {
    auto at=fixture::holds.find(object);if(at==fixture::holds.end())return TSUCHI_NOT_FOUND;
    fixture::holds.erase(at); // the source hold is removed before its native setter
    return game::restore_slot_material(object,slot,game::from_handle(fixture::source),{})?TSUCHI_OK:TSUCHI_NOT_READY;
}
}
namespace tsuchi::game {
bool path_stays_within(const char*) {std::abort();}
TSUCHI_Result file_size(const char*, uint64_t&) { return TSUCHI_NOT_FOUND; }
TSUCHI_Result read_whole_file(const char*,char*&,uint64_t&) {std::abort();}
uint64_t loaded_world() {return fixture::world;}
uint64_t table_entry(uint64_t,uint64_t) {std::abort();}
}
int main() {
    build_world();tsuchi::set_allocator({fixture::allocate,fixture::release});
    fixture::world=fake::engine_object(u"OldWorld",w.actor);world_now=game::to_handle(fixture::world);
    fixture::material_type=fake::engine_object(u"MaterialInstanceConstant",w.root);
    fixture::dynamic_type=fake::engine_object(u"MaterialInstanceDynamic",w.root);
    fake::set_super(fixture::dynamic_type,fixture::material_type);
    fake::field(fixture::material_type,u"Parent",u"ObjectProperty",0x80,8,fixture::material_type);
    const uint64_t decal_type=fake::engine_object(u"DecalComponent",w.root);
    const uint64_t input=fake::t::constant::property_param;
    const uint64_t setter=fake::function(decal_type,w.function_class,u"SetDecalMaterial",8,8,[](uint64_t,uint64_t) {
        ++fixture::writes;
        if(!fixture::stop_in_setter)return;
        fixture::stop_in_setter=false;
        CHECK(fixture::watched->placed.count==0 && fixture::watched->derived.count==0 && fixture::watched->stand_in==0);
        mod_stop(fixture::stop_provider);
        CHECK(game::object_kept(game::from_handle(fixture::direct)));
        CHECK(game::object_kept(game::from_handle(fixture::child)));
        CHECK(game::object_kept(game::from_handle(fixture::shown)));
    });
    fake::flag(fake::field(setter,u"NewDecalMaterial",u"ObjectProperty",0,8),input);
    const uint64_t first=fixture::set("shader.cleanup",2);
    Set* selected=fixture::find(first);fixture::stop_provider=selected->mod;
    Material& row=selected->materials[0];fixture::watched=&row;fixture::retired=selected->materials;
    fixture::instance(row,u"CleanupPlain");row.stage=Stage::asked;row.asked_at=77;
    fixture::source=row.plain;fixture::child=row.derived[0].source;fixture::shown=row.derived[0].stand_in;
    fixture::direct=game::to_handle(fake::engine_object(u"DirectShown",fixture::dynamic_type));
    CHECK(retain_game_object(fixture::direct));row.stand_in=fixture::direct;
    for(int i=0;i<2;++i) {
        const uint64_t renderer=fake::engine_object(u"Decal",decal_type);
        fixture::holds[renderer]=row.plain;
        CHECK(row.placed.add(Placed{game::to_handle(renderer),0,row.plain}));
    }
    fixture::world=fake::engine_object(u"NewWorld",w.actor);fixture::stop_in_setter=true;
    step();
    CHECK(fixture::released_rows && static_cast<Material*>(fixture::retired)->asked_at==fixture::marker);
    CHECK(sets.count==0 && counts.sets==0 && counts.materials==0 && fixture::writes==2 && fixture::holds.empty());
    CHECK(!game::object_kept(game::from_handle(fixture::direct)) && !game::object_kept(game::from_handle(fixture::child)) && !game::object_kept(game::from_handle(fixture::shown)));
    fixture::blocks.erase(fixture::retired);std::free(fixture::retired);fixture::retired=nullptr;

    // Stopping another provider swaps the current provider, then publication
    // reuses its old slot with a smaller material array. All original rows refresh.
    const uint64_t a=fixture::set("shader.refresh-a",1),b=fixture::set("shader.refresh-b",3);
    fixture::remove_provider=fixture::find(a)->mod;
    for(uint32_t i=0;i<3;++i)fixture::instance(fixture::find(b)->materials[i],u"RefreshPlain");
    fixture::swap_on=game::from_handle(fixture::find(b)->materials[1].derived[0].stand_in);
    const uint64_t last_mid=game::from_handle(fixture::find(b)->materials[2].derived[0].stand_in);
    const uint64_t copier=fake::function(fixture::dynamic_type,w.function_class,u"K2_CopyMaterialInstanceParameters",16,8,[](uint64_t object,uint64_t) {
        ++fixture::copied[object];
        if(!fixture::swap_in_copy || object!=fixture::swap_on)return;
        fixture::swap_in_copy=false;mod_stop(fixture::remove_provider);
        const uint64_t c=fixture::set("shader.refresh-c",1);
        fixture::instance(fixture::find(c)->materials[0],u"NewPlain");
        fixture::replacement_source=game::from_handle(fixture::find(c)->materials[0].derived[0].stand_in);
    });
    fake::flag(fake::field(copier,u"Source",u"ObjectProperty",0,8,fixture::material_type),input);
    fake::flag(fake::bit_field(copier,u"bQuickParametersOnly",8,0,0xff),input);
    world_now=game::to_handle(fixture::world);world_steps=2;fixture::swap_in_copy=true;
    refresh_instances();
    CHECK(!fixture::swap_in_copy && fixture::copied[last_mid]==1);
    CHECK(fixture::replacement_source && fixture::copied[fixture::replacement_source]==0);
    CHECK(fixture::find(b) && fixture::find(b)->materials[2].derived[0].updated_at==world_steps);
    while(sets.count)mod_stop(sets[0].mod);
    CHECK(counts.sets==0 && counts.materials==0);
    if(!fixture::failures)std::puts("Shader cleanup setter-stop and refresh swap/reuse controls passed");
    return fixture::failures;
}
