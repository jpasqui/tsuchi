// Real host callable thunks, including mixed float/integer args and provenance.
#include "fake_game.h"
#include "callable_thunks.h"
#include "registry.h"
#include <cstdio>
namespace {
int failures=0, calls=0, integer_calls=0;
uint64_t id1=0,id2=0;
void* next1=nullptr;void* next2=nullptr;
#define CHECK(x) do { if (!(x)) { std::printf("line%d: %s\n",__LINE__,#x);++failures; } } while(0)
uint64_t thread() { return 731; }
void original(void* object,float seconds,bool idle) {
    CHECK(object==reinterpret_cast<void*>(0x123456) && seconds==0.03125f && !idle);++calls;
}
void first(void* object,float seconds,bool idle) {
    CHECK(tsuchi::hook_invocation(id1).thread==731);
    reinterpret_cast<thunks::EngineTick>(next1)(object,seconds,idle);
}
void second(void* object,float seconds,bool idle) {
    CHECK(tsuchi::hook_invocation(id2).caller==1);
    reinterpret_cast<thunks::EngineTick>(next2)(object,seconds,idle);
}
uint64_t integer(uint64_t a,uint64_t b,uint64_t c,uint64_t d,uint64_t e,uint64_t f) {
    CHECK(a==11 && b==22 && c==33 && d==44 && e==55 && f==66);++integer_calls;return 731;
}
void* integer_next=nullptr;
uint64_t integer_wrap(uint64_t a,uint64_t b,uint64_t c,uint64_t d,uint64_t e,uint64_t f) {
    return thunks::call(reinterpret_cast<uint64_t>(integer_next),a,b,c,d,e,f);
}
}
int main() {
    fake::start();tsuchi::set_thread_probe(thread);
    for (auto pointer : {reinterpret_cast<uint64_t>(original),reinterpret_cast<uint64_t>(first),reinterpret_cast<uint64_t>(second),reinterpret_cast<uint64_t>(integer),reinterpret_cast<uint64_t>(integer_wrap)}) fake::code().push_back(pointer);
    for (auto pointer : thunks::all) fake::code().push_back(reinterpret_cast<uint64_t>(pointer));
    for (auto pointer : thunks::all_engine_ticks) fake::code().push_back(reinterpret_cast<uint64_t>(pointer));
    auto* mod=tsuchi::mod_create("abi");
    const uint64_t int_entry=fake::memory(8),float_entry=fake::memory(8);
    fake::put(int_entry,integer);fake::put(float_entry,original);
    uint64_t int_id=0;
    CHECK(tsuchi::hook_wrap(mod,int_entry,0,reinterpret_cast<void*>(integer_wrap),0,&int_id,&integer_next)==TSUCHI_OK);
    thunks::engine_ticks=true;
    CHECK(tsuchi::hook_wrap(mod,float_entry,0,reinterpret_cast<void*>(first),0,&id1,&next1)==TSUCHI_OK);
    CHECK(tsuchi::hook_wrap(mod,float_entry,0,reinterpret_cast<void*>(second),0,&id2,&next2)==TSUCHI_OK);
    thunks::engine_ticks=false;
    for (int step=0;step<10;++step) {
        CHECK(thunks::call(fake::get<uint64_t>(int_entry),11,22,33,44,55,66)==731);
        reinterpret_cast<thunks::EngineTick>(fake::get<uint64_t>(float_entry))(reinterpret_cast<void*>(0x123456),0.03125f,false);
        CHECK(!tsuchi::hook_invocation(id1).token && !tsuchi::hook_invocation(id2).token);
    }
    CHECK(calls==10 && integer_calls==10);
    tsuchi::mod_stop(mod);tsuchi::set_thread_probe(nullptr);
    reinterpret_cast<thunks::EngineTick>(fake::get<uint64_t>(float_entry))(reinterpret_cast<void*>(0x123456),0.03125f,false);
    CHECK(calls==11);
    return failures;
}
