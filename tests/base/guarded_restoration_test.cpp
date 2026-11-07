// Actual guarded reflection/EngineCall plumbing against the existing fake engine.
#include <cstdio>
#include "engine_call.h"
#include "objects.h"
#include "reflect_world.h"
using namespace tsuchi::game;
namespace {
int failures=0, phase=0, lookup_calls=0;
uint64_t target=0, target_handle=0;
bool veto=false;
#define CHECK(x) do { if (!(x)) { std::printf("line%d: %s\n",__LINE__,#x);++failures; } } while(0)
void destroy_at(int boundary) {
    if (phase!=boundary || veto) return;
    const int32_t index=fake::get<int32_t>(target+fake::t::object::index);
    fake::put<uint32_t>(fake::item_at(index)+fake::t::object_item::flags,uint32_t(fake::t::constant::unreachable_flags));
}
bool allowed(void*) { return from_handle(target_handle)==target; }
uint64_t name_lookup(void* text,uint64_t length,uint64_t mode) {
    const uint64_t result=fake::engine_name(text,length,mode);destroy_at(1);return result;
}
void* function_lookup(void* type,uint64_t name,uint64_t parents) {
    ++lookup_calls;
    void* result=fake::engine_find_function(type,name,parents);destroy_at(2);return result;
}
void* allocate_args(void* allocator,uint64_t bytes,uint32_t align) {
    void* result=fake::engine_allocate(allocator,bytes,align);destroy_at(3);return result;
}
void initialize_args(void* type,void* args,int32_t count) { fake::engine_initialize(type,args,count);destroy_at(4); }
void process_args(void* object,void* function,void* args) { fake::engine_process(object,function,args);destroy_at(5); }
}
int main() {
    build_world();
    fake::t::fn::name_from_text=name_lookup;
    fake::t::fn::class_find_function=function_lookup;
    for (auto ptr : {reinterpret_cast<uint64_t>(allocate_args),reinterpret_cast<uint64_t>(initialize_args),reinterpret_cast<uint64_t>(process_args)}) fake::code().push_back(ptr);
    const uint64_t allocator_table=fake::get<uint64_t>(fake::engine().allocator);
    fake::put(allocator_table+fake::t::allocator_vtable::malloc,allocate_args);
    fake::put(fake::engine().vtable+fake::t::struct_vtable::initialize,initialize_args);
    fake::put(fake::engine().vtable+fake::t::object_vtable::process_event,process_args);
    for (int step=1;step<=7;++step) {
        phase=0;veto=step==6;
        target=fake::engine_object(u"Guarded",w.creature,0x1B0);
        target_handle=to_handle(target);
        const int allocated=fake::engine().allocated,freed=fake::engine().freed,initialized=fake::engine().initialized,destroyed=fake::engine().destroyed,invoked=fake::engine().invoked;
        const int lookups=lookup_calls;
        phase=step==6 ? 1 : step;
        {
            EngineCall restore(target,"Heal",{allowed,nullptr});
            if (step<=4) CHECK(restore.ready()!=TSUCHI_OK);
            if (step==1) CHECK(lookup_calls==lookups); // no second native lookup after owner destruction
            if (step==3) CHECK(fake::engine().initialized==initialized); // allocated but never initialized
            if (step==4) CHECK(fake::engine().destroyed==destroyed+1); // initialized params cleaned despite dead owner
            if (step==5 || step==6 || step==7) {
                CHECK(restore.ready()==TSUCHI_OK);
                if (step==7) {
                    destroy_at(7);
                    const uint64_t block=fake::engine().last_block;
                    CHECK(restore.set("Amount",number_value(9))==TSUCHI_NOT_READY);
                    CHECK(fake::get<float>(block)==0); // no prepared property write after guard fails
                } else CHECK(restore.set("Amount",number_value(9))==TSUCHI_OK);
                const TSUCHI_Result result=restore.invoke();
                CHECK(result==(step==6 ? TSUCHI_OK : TSUCHI_NOT_READY));
            }
        }
        CHECK(fake::engine().allocated-allocated==fake::engine().freed-freed);
        if (step!=3) CHECK(fake::engine().initialized-initialized==fake::engine().destroyed-destroyed);
        CHECK(fake::engine().invoked-invoked==(step==5 || step==6 ? 1 : 0));
    }
    // An owner use generation ending at the same address also invalidates the retained permit.
    phase=0;veto=false;target=fake::engine_object(u"Reuse",w.creature,0x1B0);target_handle=to_handle(target);
    {
        EngineCall restore(target,"Heal",{allowed,nullptr});
        CHECK(restore.ready()==TSUCHI_OK);
        end_life(target);
        CHECK(from_handle(target_handle)==0 && live(target));
        CHECK(restore.set("Amount",number_value(9))==TSUCHI_NOT_READY);
        CHECK(restore.invoke()==TSUCHI_NOT_READY);
    }
    return failures;
}
