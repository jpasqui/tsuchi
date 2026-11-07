// Numeric console values are process resources, not UObject fields.
#include "reflection.h"
#include "game.h"
#include "holds.h"
#include "settings.h"
#include <float.h>
#include <string.h>
namespace tsuchi {
namespace {
namespace t = game::table;
struct Wanted { char name[128]; float value; };
struct State { char name[128]; uint64_t variable; float original, last, wanted; uint32_t flags; bool written; };
struct Kept { State* state; };
bool name_of(const char* name, char out[128], uint64_t& key) {
    key=14695981039346656037ull;
    uint32_t n=0;
    for (; name[n]; ++n) {
        const unsigned char c=static_cast<unsigned char>(name[n]);
        if (n==127 || c<=32 || c>126) return false;
        const char lower=c>='A' && c<='Z' ? char(c+('a'-'A')) : char(c);
        out[n]=lower; key=(key^static_cast<unsigned char>(lower))*1099511628211ull;
    }
    out[n]=0; return n!=0;
}
bool finite(float value) { return value==value && value<=FLT_MAX && value>=-FLT_MAX; }
TSUCHI_Result find(const char* name, uint64_t& variable) {
    const uint64_t manager=game::global<uint64_t>(t::data::console_manager);
    const auto lookup=game::method(manager,t::console_manager_table::find_variable);
    if (!lookup) return TSUCHI_NOT_READY;
    char16_t wide[128]; uint32_t i=0;
    do { wide[i]=static_cast<unsigned char>(name[i]); } while (name[i++]);
    variable=reinterpret_cast<uint64_t>(lookup(reinterpret_cast<void*>(manager),wide,true));
    return variable ? TSUCHI_OK : TSUCHI_NOT_FOUND;
}
bool read(uint64_t variable, float& value, uint32_t& flags) {
    const auto number=game::method(variable,t::console_variable_table::value);
    const auto bits=game::method(variable,t::console_variable_table::flags);
    if (!number || !bits) return false;
    flags=bits(reinterpret_cast<void*>(variable)); value=number(reinterpret_cast<void*>(variable));
    return finite(value);
}
bool same(float a,float b) { return a==b; }
bool write(uint64_t variable,float value,uint32_t flags) {
    const auto set=game::method(variable,t::console_variable_table::set);
    if (!set || (flags&t::constant::console_read_only)) return false;
    char text[48]; char16_t wide[48]; setting_number_text(value,text);
    uint32_t i=0; do { wide[i]=static_cast<unsigned char>(text[i]); } while (text[i++]);
    // Equal priority preserves the source bits and still accepts a user value.
    set(reinterpret_cast<void*>(variable),wide,flags&t::constant::console_priority_mask);
    return true;
}
bool current(const State& kept,float& value,uint32_t& flags) {
    uint64_t variable=0;
    return find(kept.name,variable)==TSUCHI_OK && variable==kept.variable && read(variable,value,flags);
}
void restore(const Hold&,Kept& slot,HoldEnd) {
    if (!slot.state) return;
    State& kept=*slot.state;
    float value=0; uint32_t flags=0;
    if (kept.written && current(kept,value,flags) && same(value,kept.last) &&
        (flags&t::constant::console_priority_mask)==(kept.flags&t::constant::console_priority_mask))
        write(kept.variable,kept.original,flags);
    tsuchi::release(slot.state); slot.state=nullptr;
}
TSUCHI_Result apply(const Hold& hold,Kept& slot,const Wanted& wanted,bool fresh) {
    const auto guard=hold_call_guard(hold);
    uint64_t variable=0; const auto found=find(wanted.name,variable);
    if (found!=TSUCHI_OK) return found;
    float before=0; uint32_t flags=0;
    if (!read(variable,before,flags)) return TSUCHI_NOT_READY;
    if (flags&t::constant::console_read_only) return TSUCHI_DENIED;
    if (fresh) {
        slot.state=static_cast<State*>(allocate(sizeof(State)));
        if (!slot.state) return TSUCHI_NO_MEMORY;
        *slot.state={};
    }
    State& kept=*slot.state;
    if (fresh) { memcpy(kept.name,wanted.name,sizeof kept.name); kept.variable=variable; kept.original=before; kept.flags=flags; }
    else if (strcmp(kept.name,wanted.name) || variable!=kept.variable) return TSUCHI_NOT_READY;
    else if (kept.written && (!same(before,kept.last) || (flags&t::constant::console_priority_mask)!=(kept.flags&t::constant::console_priority_mask))) {
        kept.original=before; kept.flags=flags;
    }
    const float previous=kept.last, previous_wanted=kept.wanted; const bool written=kept.written;
    kept.wanted=wanted.value;
    kept.last=wanted.value; kept.written=true; hold_apply_checkpoint(hold);
    const bool accepted=same(before,wanted.value) || write(variable,wanted.value,flags);
    if (!guard.permits()) return TSUCHI_NOT_READY;
    float actual=0; uint32_t after_flags=0;
    if (!accepted || !current(kept,actual,after_flags) || !same(actual,wanted.value)) {
        if (current(kept,actual,after_flags)) write(variable,before,after_flags);
        if (!guard.permits()) return TSUCHI_NOT_READY;
        kept.last=previous; kept.wanted=previous_wanted; kept.written=written;
        if (fresh) { tsuchi::release(slot.state); slot.state=nullptr; }
        return TSUCHI_NOT_READY;
    }
    return TSUCHI_OK;
}
bool keep(const Hold& hold,Kept& slot) {
    if (!slot.state) return false;
    State& kept=*slot.state;
    float value=0; uint32_t flags=0;
    if (!current(kept,value,flags) || (flags&t::constant::console_read_only)) return false;
    if (same(value,kept.last) && (flags&t::constant::console_priority_mask)==(kept.flags&t::constant::console_priority_mask)) return true;
    kept.original=value; kept.flags=flags; kept.last=kept.wanted; kept.written=true;
    const auto guard=hold_call_guard(hold);
    if (!write(kept.variable,kept.wanted,flags) || !guard.permits()) return false;
    return current(kept,value,flags) && same(value,kept.wanted);
}
Holds<Kept,Wanted,apply,keep,restore> holds{"reflection.console_variable",HoldOn::nothing,true};
TSUCHI_Result get(const char* name,float* out) {
    char checked[128]; uint64_t key=0,variable=0;
    if (!name_of(name,checked,key)) return TSUCHI_INVALID;
    const auto found=find(checked,variable); if (found!=TSUCHI_OK) return found;
    uint32_t flags=0; return read(variable,*out,flags) ? TSUCHI_OK : TSUCHI_NOT_READY;
}
TSUCHI_Result hold(TSUCHI_Mod* mod,const char* name,float value) {
    Wanted wanted{}; uint64_t key=0;
    if (!finite(value) || !name_of(name,wanted.name,key)) return TSUCHI_INVALID;
    if (!strcmp(wanted.name,"r.vsync") || !strcmp(wanted.name,"rhi.syncinterval") || !strcmp(wanted.name,"t.maxfps")) return TSUCHI_DENIED;
    wanted.value=value; return holds.take(mod,0,key,wanted);
}
TSUCHI_Result release_value(TSUCHI_Mod* mod,const char* name) {
    char checked[128]; uint64_t key=0;
    if (!name_of(name,checked,key)) return TSUCHI_INVALID;
    return holds.release(mod,0,key);
}
const ConsoleVariables backend{get,hold,release_value};
}
void console_variables_start() { holds.start(); set_console_variables(&backend); }
}
