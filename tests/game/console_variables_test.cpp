#include "fake_game.h"
#include "reflection.h"
#include "holds.h"
#include "actor_events.h"
#include "ordinary_lifecycle.h"
#include <cassert>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <map>
#include <string>
#include <vector>
using namespace tsuchi;
namespace t=game::table;
namespace {
struct Variable { float value; uint32_t flags; };
std::map<std::string,uint64_t> names;
std::map<uint64_t,Variable> variables;
std::vector<void(*)()> steps;
const ConsoleVariables* api=nullptr;
uint64_t table=0,thread=1;
unsigned writes=0;
bool clamp_value=false;
bool clamp_small=false;
TSUCHI_Mod* stop_in_set=nullptr;
void* lookup(void*,const void* pointer,bool) {
    const auto* wide=static_cast<const char16_t*>(pointer);std::string name;
    while (*wide) { char c=char(*wide++);name+=c>='A'&&c<='Z'?char(c+32):c; }
    auto found=names.find(name);return found==names.end()?nullptr:reinterpret_cast<void*>(found->second);
}
uint32_t flags(void* variable) { return variables.at(reinterpret_cast<uint64_t>(variable)).flags; }
float number(void* variable) { return variables.at(reinterpret_cast<uint64_t>(variable)).value; }
void set(void* pointer,const void* raw,uint32_t priority) {
    auto& variable=variables.at(reinterpret_cast<uint64_t>(pointer));
    assert(priority==(variable.flags&t::constant::console_priority_mask));
    assert(!(variable.flags&t::constant::console_read_only));
    const auto* wide=static_cast<const char16_t*>(raw);char value[48]{};unsigned n=0;
    while (*wide) value[n++]=char(*wide++);
    variable.value=std::strtof(value,nullptr);
    if (clamp_value && variable.value==999) variable.value=998;
    if (clamp_small && variable.value>0 && variable.value<0.002f) variable.value=0;
    ++writes;
    if (stop_in_set) { auto* stopping=stop_in_set;stop_in_set=nullptr;mod_stop(stopping); }
}
uint64_t make(float value,uint32_t source) {
    const auto address=fake::memory(0x20);fake::put<uint64_t>(address,table);variables[address]={value,source};return address;
}
void step() { for(auto fn:steps) fn(); }
}
namespace tsuchi {
void set_console_variables(const ConsoleVariables* backend) { api=backend; }
void setting_number_text(float value,char out[48]) { std::snprintf(out,48,"%.9g",double(value)); }
bool each_frame(void(*fn)()) { steps.push_back(fn);return true; }
bool on_actor_left(void(*)(uint64_t,uint64_t,ActorLeft)) { return true; }
void ordinary_lifecycle_listener(void(*)(uint64_t,uint64_t,bool)) {}
TSUCHI_Result ordinary_lifecycle_watch(uint64_t) { return TSUCHI_OK; }
uint64_t ordinary_lifecycle_restore_part(uint64_t part,uint64_t) { return part; }
}
namespace tsuchi::game {
uint64_t from_handle(uint64_t handle) { return handle; }
uint64_t to_handle(uint64_t handle) { return handle; }
uint64_t part_owner(uint64_t handle) { return handle; }
bool is_game_actor(uint64_t) { return true; }
}
int main() {
    fake::start();set_thread_probe([] { return thread; });note_game_thread();
    table=fake::memory(0x200);
    fake::put<uint64_t>(table+t::console_variable_table::flags,reinterpret_cast<uint64_t>(flags));
    fake::put<uint64_t>(table+t::console_variable_table::value,reinterpret_cast<uint64_t>(number));
    fake::put<uint64_t>(table+t::console_variable_table::set,reinterpret_cast<uint64_t>(set));
    const uint64_t manager=fake::memory(0x20),manager_table=fake::memory(0x200);
    fake::put<uint64_t>(manager,manager_table);
    fake::put<uint64_t>(manager_table+t::console_manager_table::find_variable,reinterpret_cast<uint64_t>(lookup));
    fake::put<uint64_t>(fake::main_image()+t::data::console_manager,manager);
    for(auto pointer:{reinterpret_cast<uint64_t>(lookup),reinterpret_cast<uint64_t>(flags),reinterpret_cast<uint64_t>(number),reinterpret_cast<uint64_t>(set)}) fake::code().push_back(pointer);
    const uint64_t shadow=make(1024,0x01000000);
    names["r.shadow.maxresolution"]=shadow;
    names["r.readonly"]=make(1,4);
    const auto small=make(0,0x01000000);
    names["r.small.value"]=small;
    auto* a=mod_create("video.settings");auto* b=mod_create("other.settings");
    console_variables_start();assert(api);
    float value=0;
    assert(api->read("r.Shadow.MaxResolution",&value)==TSUCHI_OK&&value==1024);
    assert(api->read("missing",&value)==TSUCHI_NOT_FOUND);
    assert(api->hold(a,"bad name",1)==TSUCHI_INVALID);
    assert(api->hold(a,"r.readonly",0)==TSUCHI_DENIED);
    assert(api->hold(a,"R.VSYNC",0)==TSUCHI_DENIED);
    assert(api->hold(a,"r.Shadow.MaxResolution",2048)==TSUCHI_OK&&owned_count(a)==1);
    assert(variables[shadow].value==2048&&variables[shadow].flags==0x01000000);
    assert(api->hold(b,"r.shadow.maxresolution",4096)==TSUCHI_BUSY);
    assert(api->hold(a,"R.SHADOW.MAXRESOLUTION",4096)==TSUCHI_OK&&owned_count(a)==1);
    variables[shadow]={1536,0x02000000};step();
    assert(variables[shadow].value==4096&&variables[shadow].flags==0x02000000);
    assert(api->release(a,"r.Shadow.MaxResolution")==TSUCHI_OK&&variables[shadow].value==1536);
    assert(api->release(a,"r.Shadow.MaxResolution")==TSUCHI_NOT_FOUND);
    clamp_value=true;
    assert(api->hold(a,"r.Shadow.MaxResolution",999)==TSUCHI_NOT_READY&&!owned_count(a)&&variables[shadow].value==1536);
    clamp_value=false;
    // A rejected change must not replace the previous hold's desired value.
    // Video can reselect its cached successful value without taking it again;
    // a later engine update must still restore 2048, never the refused 999.
    assert(api->hold(a,"r.Shadow.MaxResolution",2048)==TSUCHI_OK);
    clamp_value=true;
    assert(api->hold(a,"r.Shadow.MaxResolution",999)==TSUCHI_NOT_READY&&owned_count(a)==1&&variables[shadow].value==2048);
    clamp_value=false;
    variables[shadow]={1664,0x03000000};step();
    assert(variables[shadow].value==2048&&variables[shadow].flags==0x03000000&&owned_count(a)==1);
    assert(api->release(a,"r.Shadow.MaxResolution")==TSUCHI_OK&&variables[shadow].value==1664);
    assert(api->hold(a,"r.Shadow.MaxResolution",2048)==TSUCHI_OK);
    variables[shadow].value=888;assert(api->release(a,"r.Shadow.MaxResolution")==TSUCHI_OK&&variables[shadow].value==888);
    assert(api->hold(a,"r.Shadow.MaxResolution",2048)==TSUCHI_OK);
    names["r.shadow.maxresolution"]=make(777,0);const unsigned before=writes;step();
    assert(!owned_count(a)&&writes==before&&variables[shadow].value==2048);
    const auto replacement=names["r.shadow.maxresolution"];
    assert(api->hold(a,"r.Shadow.MaxResolution",4096)==TSUCHI_OK);
    mod_stop(a);assert(variables[replacement].value==777&&!owned_count(a));
    auto* reentrant=mod_create("video.reentrant");stop_in_set=reentrant;
    assert(api->hold(reentrant,"r.Shadow.MaxResolution",1024)==TSUCHI_NOT_READY);
    assert(variables[replacement].value==777&&!owned_count(reentrant));
    auto* precise=mod_create("video.precise");
    // Small requests must actually be written, not accepted as close to zero.
    const unsigned small_before=writes;
    assert(api->hold(precise,"r.small.value",0.001f)==TSUCHI_OK);
    assert(variables[small].value==0.001f&&writes==small_before+1);
    assert(api->hold(precise,"r.small.value",0.0011f)==TSUCHI_OK);
    assert(variables[small].value==0.0011f&&writes==small_before+2);
    const unsigned held=writes;step();assert(writes==held);
    // A small game update becomes the new restoration baseline even when it
    // keeps the same console priority as our value.
    variables[small].value=0.00115f;step();
    assert(variables[small].value==0.0011f&&writes==held+1);
    assert(api->release(precise,"r.small.value")==TSUCHI_OK&&variables[small].value==0.00115f);
    assert(api->hold(precise,"r.small.value",0)==TSUCHI_OK&&variables[small].value==0);
    variables[small].value=0.0002f;
    assert(api->release(precise,"r.small.value")==TSUCHI_OK&&variables[small].value==0.0002f);
    // A native clamp to zero is a refusal, not a successful small hold.
    variables[small].value=0;clamp_small=true;
    assert(api->hold(precise,"r.small.value",0.001f)==TSUCHI_NOT_READY);
    assert(variables[small].value==0&&!owned_count(precise));
    clamp_small=false;mod_stop(precise);
    mod_stop(b);
}
