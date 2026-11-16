// Action labels and button columns are separate FTexts in the game's HUD.
// Resolve each line against a localized label, never against its stock button.
#include "ui_control_hints.h"
#include "ui_icons.h"
#include "ui_screens.h"
#include "ui_service.h"
#include "ui_feedback.h"
#include "ui_rich_text.h"
#include "frame.h"
#include "hooks.h"
#include "holds.h"
#include "objects.h"
#include "reflect.h"
#include "shared.h"
#include <string.h>

namespace tsuchi {
namespace {
namespace t = game::table;
constexpr uint32_t kText = 8192;
struct Rule { char space[64]{}, key[128]{}, glyph[192]{}, label[512]{}, identity[33]{}; };
struct KeptRule { Rule* value=nullptr; };
struct TextState { game::EngineText source{},applied{}; bool retained=false,owned=false; };
struct View {
    uint64_t widget=0, label=0;
    TextState button{},caption{};
    bool dirty=true;
};
View* views=nullptr; uint32_t view_count=0,view_capacity=0;
bool scan=false, writing=false, started=false;
uint32_t refresh_depth=0;
TSUCHI_Mod* self=nullptr;
using SetText=decltype(t::fn::rich_text_set);
SetText next_set=nullptr;
game::ClassRef rich_class{t::name::ui_footer_text_class};
char* find(char* text,const char* needle) {
    const size_t n=strlen(needle),length=strlen(text);
    for (size_t i=0;i+n<=length;++i) if (!memcmp(text+i,needle,n)) return text+i;
    return nullptr;
}
void identity(const char* text,char out[33]) {
    constexpr char marker[]="[UE:Meta key=\"";const size_t length=strlen(text),n=sizeof marker-1;
    out[0]=0;
    for (size_t i=0;i+n+33<=length;++i) if (!memcmp(text+i,marker,n)&&text[i+n+32]=='"') {
        memcpy(out,text+i+n,32);out[32]=0;return;
    }
}

bool same(const game::EngineText& a,const game::EngineText& b) {
    return !memcmp(a.bytes+t::text_value::data,b.bytes+t::text_value::data,8) &&
        a.counter()==b.counter() && !memcmp(a.bytes+t::text_value::flags,b.bytes+t::text_value::flags,4);
}
void normalize(const char* in,char* out,uint32_t room) {
    uint32_t n=0; bool tag=false,space=false;
    for (;*in;++in) {
        if (*in=='[') {tag=true;continue;}
        if (tag) {if (*in==']') tag=false;continue;}
        if (*in==' '||*in=='\r'||*in=='\n'||*in=='\t') {space=n!=0;continue;}
        if (space&&n+1<room) out[n++]=' ';
        space=false;if (n+1<room) out[n++]=*in;
    }
    out[n]=0;
}
bool text(const game::EngineText& value,char* out,uint32_t room) {
    const uint64_t string=reinterpret_cast<uint64_t>(t::fn::text_display_string(const_cast<game::EngineText*>(&value)));
    if (!string) return false;
    size_t needed=0;
    return game::units_to_utf8(game::read_or<uint64_t>(string+t::array::data),game::read_or<int32_t>(string+t::array::count),out,room,&needed)==TSUCHI_OK;
}
bool localized(Rule& rule) {
    char16_t space[64]{},key[128]{};size_t ns=0,nk=0;
    if (!game::utf8_to_units(rule.space,space,63,ns)||!game::utf8_to_units(rule.key,key,127,nk)) return false;
    alignas(8) uint8_t a[t::array::size]{},b[t::array::size]{};
    const auto put=[](uint8_t* p,char16_t* s,int32_t n) {
        const uint64_t data=reinterpret_cast<uint64_t>(s);
        memcpy(p+t::array::data,&data,8);memcpy(p+t::array::count,&n,4);memcpy(p+t::array::max,&n,4);
    };
    put(a,space,int32_t(ns+1));put(b,key,int32_t(nk+1));
    auto value=t::fn::ui_footer_lookup(a,b,0,0,0);
    char shown[1024]{};const bool okay=text(value,shown,sizeof shown);
    game::drop_shared(value.counter());
    if (okay) {normalize(shown,rule.label,sizeof rule.label);identity(shown,rule.identity);}
    return okay&&rule.label[0];
}
TSUCHI_Result apply(const Hold& hold,KeptRule& kept,const Rule& wanted,bool) {
    Rule copy=wanted;if (!localized(copy)) return TSUCHI_NOT_FOUND;
    auto* next=static_cast<Rule*>(allocate(sizeof(Rule)));if (!next) return TSUCHI_NO_MEMORY;
    *next=copy;
    if (!hold_call_guard(hold).permits()) {tsuchi::release(next);return TSUCHI_NOT_READY;}
    tsuchi::release(kept.value);kept.value=next;
    scan=true;ui_icons_changed();return TSUCHI_OK;
}
void restore(const Hold&,KeptRule& kept,HoldEnd) {tsuchi::release(kept.value);kept={};scan=true;ui_icons_changed();}
Holds<KeptRule,Rule,apply,nullptr,restore> rules{"ui.control_hint",HoldOn::nothing,true};
uint64_t key_of(const char* space,const char* key) {
    uint64_t hash=14695981039346656037ull;
    for (const char* p=space;*p;++p) hash=(hash^uint8_t(*p))*1099511628211ull;
    hash=(hash^0)*1099511628211ull;
    for (const char* p=key;*p;++p) hash=(hash^uint8_t(*p))*1099511628211ull;
    return hash;
}
bool read(uint64_t handle,game::EngineText& out) {
    const auto object=game::from_handle(handle);
    return object&&game::read(object+t::ui_footer_label::text,out);
}
void drop(TextState& state) {
    if (state.retained) game::drop_shared(state.source.counter());
    if (state.owned) game::drop_shared(state.applied.counter());
    state={};
}
void drop(View& view) {drop(view.button);drop(view.caption);view={};}
bool remember(TextState& state,const game::EngineText& now) {
    if (!state.retained||(!same(now,state.source)&&(!state.owned||!same(now,state.applied)))) {
        drop(state);state.source=now;state.retained=ui_retain_text(now);
    }
    return state.retained;
}
uint64_t companion(uint64_t object) {
    char name[128]{};size_t needed=0;
    if (!game::name_text(game::name_of(object),name,sizeof name,&needed)) return 0;
    const char* suffix=nullptr;const char* prefix="F_Text";
    if (strlen(name)>=7&&!memcmp(name,"F_Guide",7)) suffix=name+7;
    else if (strlen(name)>=8&&!memcmp(name,"F_Button",8)) {suffix=name+8;prefix="F_Guide";}
    else return 0;
    char field[128]{};const size_t length=strlen(prefix);
    if (strlen(suffix)+length+1>=sizeof field) return 0;
    memcpy(field,prefix,length);memcpy(field+length,suffix,strlen(suffix)+1);
    const auto owner=game::outer_of(game::outer_of(object));
    auto label=owner?game::member_object(owner,field):0;
    // Floating actor prompts use Message rather than Guide, while HUD
    // columns use Text. All are reflected members of the same widget owner.
    if (!label&&owner) {
        constexpr const char* alternatives[]={"F_Message","F_Text"};
        for (const char* candidate:alternatives) {
            const size_t n=strlen(candidate);if (n+strlen(suffix)+1>=sizeof field) continue;
            memcpy(field,candidate,n);memcpy(field+n,suffix,strlen(suffix)+1);
            label=game::member_object(owner,field);if (label) break;
        }
    }
    return label&&game::is_a(label,rich_class)?game::to_handle(label):0;
}
bool visit(void*,uint64_t object) {
    if (!game::is_a(object,rich_class)) return true;
    const auto handle=game::to_handle(object);
    for (uint32_t i=0;i<view_count;++i) if (views[i].widget==handle) {views[i].dirty=true;return true;}
    const auto label=companion(object);
    if (!label) return true;
    // Settings screens can leave hundreds of hidden diagram widgets alive.
    // Grow with the discovered widgets so they cannot crowd out the live HUD.
    if (view_count==view_capacity) {
        const uint32_t next=view_capacity?view_capacity*2:64;
        if (next<=view_capacity) return true;
        auto* grown=static_cast<View*>(allocate(sizeof(View)*next));if (!grown) return true;
        if (view_count) memcpy(grown,views,sizeof(View)*view_count);
        tsuchi::release(views);views=grown;view_capacity=next;
    }
    views[view_count]={};views[view_count].widget=handle;views[view_count++].label=label;
    return true;
}
void update(View& view);
void set_text(void* object,void* incoming) {
    next_set(object,incoming);
    if (writing||refresh_depth||!on_game_thread()||!rules.count()) return;
    const auto handle=game::to_handle(reinterpret_cast<uint64_t>(object));
    for (uint32_t i=0;i<view_count;++i)
        if (views[i].widget==handle||views[i].label==handle) views[i].dirty=true;
    visit(nullptr,reinterpret_cast<uint64_t>(object));
    // Actor prompts set their icon again during every world tick, after the
    // engine-frame callback. Finish the rewrite here, before Slate paints it.
    for (uint32_t i=0;i<view_count;++i)
        if (views[i].widget==handle||views[i].label==handle) update(views[i]);
}
bool replace_line(char* line,uint32_t room,const char* glyph) {
    constexpr char prefix[]="[PictureFont:IconData key=\"";
    char* begin=find(line,prefix);
    if (!begin) return false;
    char* end=find(begin,"/]");if (!end) return false;end+=2;
    // A single controller glyph keeps this hint's original scale. Multiple
    // glyphs (a split control-diagram row), key names and Unbound replace the tag.
    char replacement[192]{};memcpy(replacement,glyph,strlen(glyph)+1);
    if (strlen(replacement)>=sizeof prefix-1&&!memcmp(replacement,prefix,sizeof prefix-1)&&!find(replacement+1,prefix)) {
        char* key=replacement+sizeof prefix-1;char* quote=find(key,"\"");
        char* old=begin+sizeof prefix-1;char* old_quote=find(old,"\"");
        if (quote&&old_quote) {
            const size_t n=size_t(quote-key),tail=strlen(old_quote),head=size_t(old-line);
            if (head+n+tail>=room) return false;
            memmove(old+n,old_quote,tail+1);memcpy(old,key,n);return true;
        }
    }
    const size_t head=size_t(begin-line),tail=strlen(end),n=strlen(glyph);
    if (head+n+tail>=room) return false;
    memmove(begin+n,end,tail+1);memcpy(begin,glyph,n);return true;
}
void apply_text(uint64_t handle,TextState& state,const game::EngineText& now,const char* wanted,bool changed) {
    if (!changed) {
        if (state.owned&&same(now,state.applied)) {
            writing=true;t::fn::rich_text_set(reinterpret_cast<void*>(game::from_handle(handle)),&state.source);writing=false;
            game::drop_shared(state.applied.counter());state.owned=false;
        }
        return;
    }
    if (state.owned) {
        char current[kText]{};
        if (text(state.applied,current,sizeof current)&&!strcmp(current,wanted)) {
            if (!same(now,state.applied)&&game::from_handle(handle)) {
                writing=true;t::fn::rich_text_set(reinterpret_cast<void*>(game::from_handle(handle)),&state.applied);writing=false;
            }
            return;
        }
    }
    char16_t units[kText]{};size_t count=0;
    if (!game::utf8_to_units(wanted,units,kText-1,count)) return;
    alignas(8) uint8_t string[t::array::size]{};
    const uint64_t data=reinterpret_cast<uint64_t>(units);const int32_t n=int32_t(count+1);
    memcpy(string+t::array::data,&data,8);memcpy(string+t::array::count,&n,4);memcpy(string+t::array::max,&n,4);
    auto result=t::fn::text_from_string(string);
    game::EngineText check{};
    const auto object=game::from_handle(handle);
    if (!object||!read(handle,check)||!same(check,now)) {game::drop_shared(result.counter());return;}
    writing=true;t::fn::rich_text_set(reinterpret_cast<void*>(object),&result);writing=false;
    if (state.owned) game::drop_shared(state.applied.counter());
    state.owned=read(handle,check)&&same(check,result)&&ui_retain_text(result);
    state.applied=state.owned?result:game::EngineText{};
    game::drop_shared(result.counter());
}
bool single_glyph(const char* glyph) {
    constexpr char prefix[]="[PictureFont:IconData key=\"";
    char copy[192]{};memcpy(copy,glyph,strlen(glyph)+1);
    if (strlen(copy)<sizeof prefix-1||memcmp(copy,prefix,sizeof prefix-1)) return false;
    char* end=find(copy,"/]");return end&&end[2]==0;
}
bool append_line(char* out,size_t& used,const char* line,bool newline) {
    const size_t n=strlen(line);if (used+n+2>=kText) return false;
    memcpy(out+used,line,n);used+=n;if (newline) out[used++]='\n';out[used]=0;return true;
}
void update(View& view) {
    if (!view.dirty) return;view.dirty=false;
    game::EngineText now{},caption_now{};
    if (!read(view.widget,now)||!read(view.label,caption_now)) {drop(view);return;}
    if (!remember(view.button,now)||!remember(view.caption,caption_now)) return;
    char original[kText]{},labels[kText]{},wanted[kText]{},wanted_labels[kText]{};
    if (!text(view.button.source,original,sizeof original)||!text(view.caption.source,labels,sizeof labels)) return;
    char* guide=original;char* caption=labels;size_t used=0,label_used=0;
    bool changed=false,caption_changed=false;
    while (guide&&caption) {
        char* gn=find(guide,"\n");char* ln=find(caption,"\n");
        if (gn) *gn++=0;if (ln) *ln++=0;
        char clean[512]{},id[33]{};normalize(caption,clean,sizeof clean);identity(caption,id);
        char line[kText]{},caption_line[kText]{};
        memcpy(line,guide,strlen(guide)+1);memcpy(caption_line,caption,strlen(caption)+1);
        for (uint32_t i=0;i<rules.count();++i) {
            const auto& rule=*rules.kept_at(i).value;
            // Metadata distinguishes identical captions such as Previous/Next.
            const bool match=rule.identity[0]||id[0] ? rule.identity[0]&&id[0]&&!strcmp(id,rule.identity) : !strcmp(clean,rule.label);
            if (!match) continue;
            if (single_glyph(rule.glyph)) changed|=replace_line(line,sizeof line,rule.glyph);
            else if (replace_line(line,sizeof line,"")) {
                // Fixed-width icon columns cannot fit key names or two bindings.
                // Put those runs in the caption's normal text flow instead.
                char hold[192]{};char* begin=find(guide,"[PictureFont:IconData key=\"Long_Press_Button\"");
                char* end=begin?find(begin,"/]"):nullptr;
                if (end&&size_t(end+2-begin)<sizeof hold) memcpy(hold,begin,size_t(end+2-begin));
                const size_t glyphs=strlen(rule.glyph),suffix=strlen(hold),words=strlen(caption);
                if (glyphs+suffix+words+2>=sizeof caption_line) return;
                memcpy(caption_line,rule.glyph,glyphs);memcpy(caption_line+glyphs,hold,suffix);
                caption_line[glyphs+suffix]=' ';memcpy(caption_line+glyphs+suffix+1,caption,words+1);
                line[0]=0;changed=true;caption_changed=true;
            }
            break;
        }
        if (!append_line(wanted,used,line,gn!=nullptr)||!append_line(wanted_labels,label_used,caption_line,ln!=nullptr)) return;
        guide=gn;caption=ln;
        if (bool(guide)!=bool(caption)) return;
    }
    apply_text(view.label,view.caption,caption_now,wanted_labels,caption_changed);
    apply_text(view.widget,view.button,now,wanted,changed);
}
void frame() {
    if (scan) {
        scan=false;for (uint32_t i=0;i<rules.count();++i) localized(*rules.kept_at(i).value);
        for (uint32_t i=0;i<view_count;++i) views[i].dirty=true;
        if (rules.count()) {int32_t cursor=0;game::each_object_from(cursor,visit,nullptr);}
    }
    for (uint32_t i=0;i<view_count;) {
        update(views[i]);
        if (!rules.count()||!views[i].widget) {drop(views[i]);views[i]=views[--view_count];} else ++i;
    }
    if (!rules.count()) {tsuchi::release(views);views=nullptr;view_capacity=0;}
}
TSUCHI_Result ensure() {
    if (next_set) return TSUCHI_OK;
    const auto* native=native_hooks_backend();if (!started||!native||!native->function) return TSUCHI_NOT_READY;
    void* next=nullptr;uint64_t id=0;
    const auto offset=reinterpret_cast<uint64_t>(t::fn::rich_text_set)-game::main_base();
    const auto result=native->function(self,offset,uint32_t(t::tables::ui_hint_text_words[0]),reinterpret_cast<void*>(set_text),&id,&next);
    if (result==TSUCHI_OK) next_set=reinterpret_cast<SetText>(next);
    return result;
}
TSUCHI_Result hold(TSUCHI_Mod* mod,const char* space,const char* key,const char* glyph) {
    if (!on_game_thread()) return TSUCHI_NOT_READY;
    if (!space||!*space||strlen(space)>=64||!key||!*key||strlen(key)>=128||!glyph||strlen(glyph)>=192||!ui_glyph_text_syntax(glyph)) return TSUCHI_INVALID;
    const auto ready=ensure();if (ready!=TSUCHI_OK) return ready;
    Rule rule{};memcpy(rule.space,space,strlen(space)+1);memcpy(rule.key,key,strlen(key)+1);memcpy(rule.glyph,glyph,strlen(glyph)+1);
    return rules.take(mod,0,key_of(space,key),rule);
}
TSUCHI_Result release(TSUCHI_Mod* mod,const char* space,const char* key) {
    if (!space||!*space||strlen(space)>=64||!key||!*key||strlen(key)>=128) return TSUCHI_INVALID;
    return rules.release(mod,0,key_of(space,key));
}
TSUCHI_Result refresh() {if (!on_game_thread()) return TSUCHI_NOT_READY;scan=true;ui_icons_changed();return TSUCHI_OK;}
}
void ui_control_hints_start(TSUCHI_Mod* owner) {
    self=owner;started=each_engine_frame(frame);
    auto& backend=ui_screen_backend();backend.hold_control_hint=hold;backend.release_control_hint=release;backend.refresh_control_hints=refresh;
}
void ui_control_hints_refreshing(bool begin) {if (begin) ++refresh_depth;else if (refresh_depth) --refresh_depth;}
}
