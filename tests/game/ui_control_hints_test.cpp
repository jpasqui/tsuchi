#include "ui_control_hints.h"
#include "ui_screens.h"
#include "ui_service.h"
#include "ui_feedback.h"
#include "holds.h"
#include "actor_events.h"
#include "objects.h"
#include "hooks.h"
#include "shared.h"
#include <cassert>
#include <cstring>
#include <cstdio>
#include <memory>
#include <vector>
#include <string>
using namespace tsuchi;
namespace t=game::table;
namespace {
alignas(8) unsigned char widget[0x600]{},label[0x600]{};
alignas(8) unsigned char hidden[600][0x600]{};
struct String {char16_t* data;int32_t count,max;};
struct Text {String string;std::u16string value;};
std::vector<std::unique_ptr<Text>> texts;
std::vector<void(*)()> work;
ui::Backend backend{};
bool alive=true;int diagram=0,scans=0,sets=0,dirty=0;
uint64_t ptr(const void* p){return reinterpret_cast<uint64_t>(p);}
template<class T> void put(void* p,const T& v){std::memcpy(p,&v,sizeof v);}
game::EngineText make(const std::u16string& value){
    auto item=std::make_unique<Text>();item->value=value;item->value.push_back(0);
    item->string={item->value.data(),int32_t(item->value.size()),int32_t(item->value.size())};
    game::EngineText text{};put(text.bytes,ptr(item.get()));texts.push_back(std::move(item));return text;
}
void* display(void* p){uint64_t data=0;std::memcpy(&data,p,8);return reinterpret_cast<void*>(data);}
std::u16string wide(const void* p){String s{};std::memcpy(&s,p,sizeof s);return {s.data,size_t(s.count-1)};}
game::EngineText lookup(const void*,const void* key,uint32_t,uint32_t,uint32_t){
    const auto k=wide(key);return make(k==u"ThrowPikmin"?u"[UE:Meta key=\"11111111111111111111111111111111\"/]Throw":k==u"Attack"?u"Chomp":k==u"Jump"?u"Jump":k==u"Fullcon_1P_Abutton"?u"Throw/Go to Work":k==u"Fullcon_1P_Lbutton"?u"[UE:Meta key=\"22222222222222222222222222222222\"/]Select Pikmin":k==u"NPC_Talk"?u"Talk":u"");
}
game::EngineText from_string(const void* p){return make(wide(p));}
void stock_set(void* object,void* value){put(static_cast<unsigned char*>(object)+t::ui_footer_label::text,*static_cast<game::EngineText*>(value));++sets;}
std::string shown(const unsigned char* object=widget){game::EngineText value{};std::memcpy(&value,object+t::ui_footer_label::text,sizeof value);auto* s=static_cast<String*>(display(&value));return std::string(s->data,s->data+s->count-1);}
std::string visible(){return shown()+"\n"+shown(label);}
TSUCHI_Result wrap(TSUCHI_Mod*,uint64_t,uint32_t,void* function,uint64_t* id,void** next){*next=reinterpret_cast<void*>(stock_set);*id=1;t::fn::rich_text_set=reinterpret_cast<decltype(t::fn::rich_text_set)>(function);return TSUCHI_OK;}
NativeHooks native{wrap,nullptr,nullptr,nullptr,nullptr};
void tick(){for(auto f:work)f();}
void game_set(const std::u16string& source){auto value=make(source);t::fn::rich_text_set(widget,&value);}
}
namespace tsuchi {
bool each_frame(void(*)()){return true;}
bool each_engine_frame(void(*f)()){work.push_back(f);return true;}
bool on_actor_left(void(*)(uint64_t,uint64_t,ActorLeft)){return true;}
void ordinary_lifecycle_listener(void(*)(uint64_t,uint64_t,bool)){}
TSUCHI_Result ordinary_lifecycle_watch(uint64_t){return TSUCHI_OK;}
uint64_t ordinary_lifecycle_restore_part(uint64_t p,uint64_t){return p;}
bool ui_retain_text(const game::EngineText&){return true;}
void ui_icons_changed(){++dirty;}
ui::Backend& ui_screen_backend(){return backend;}
const NativeHooks* native_hooks_backend(){return &native;}
}
namespace tsuchi::game {
uint64_t main_base(){return 0;}
bool readable(uint64_t p,uint64_t n){return p>4096&&n<100000;}
bool writable(uint64_t p,uint64_t n){return readable(p,n);}
bool executable(uint64_t){return false;}
bool is_hidden(uint64_t p){return p>=ptr(hidden)&&p<ptr(hidden)+sizeof hidden&&(p-ptr(hidden))%sizeof hidden[0]==0;}
uint64_t from_handle(uint64_t h){return alive&&(h==ptr(widget)||h==ptr(label)||is_hidden(h))?h:0;}
uint64_t to_handle(uint64_t o){return from_handle(o);}
uint64_t part_owner(uint64_t h){return h;}
bool is_game_actor(uint64_t){return false;}
uint64_t type_of(uint64_t){return 10;}
bool is_a(uint64_t o,ClassRef&){return o==ptr(widget)||o==ptr(label)||is_hidden(o);}
Name name_of(uint64_t o){return {o==ptr(widget)||is_hidden(o)?1u:2u,0};}
bool name_text(Name n,char* out,size_t room,size_t*){const char* value=n.index==1?(diagram==2?"F_Button_Npc":diagram?"F_Button_Right_000":"F_Guide_000"):"F_Text_000";assert(std::strlen(value)<room);std::memcpy(out,value,std::strlen(value)+1);return true;}
uint64_t outer_of(uint64_t o){return o==ptr(widget)||is_hidden(o)?100:o==100?200:0;}
uint64_t member_object(uint64_t o,const char* name){return o==200&&!std::strcmp(name,diagram==2?"F_Message_Npc":diagram?"F_Guide_Right_000":"F_Text_000")?ptr(label):0;}
bool each_object_from(int32_t&,bool(*visit)(void*,uint64_t),void* context){++scans;for(auto& item:hidden)visit(context,ptr(item));visit(context,ptr(widget));visit(context,ptr(label));return true;}
void drop_shared(uint64_t){}
bool utf8_to_units(const char* s,char16_t* out,size_t room,size_t& count){count=std::strlen(s);if(count>room)return false;for(size_t i=0;i<count;++i)out[i]=s[i];return true;}
TSUCHI_Result units_to_utf8(uint64_t data,int32_t count,char* out,size_t room,size_t* needed){
    if(count<1||size_t(count)>room)return TSUCHI_CAPACITY;*needed=size_t(count);auto* s=reinterpret_cast<const char16_t*>(data);for(int32_t i=0;i<count;++i)out[i]=char(s[i]);return TSUCHI_OK;
}
}
int main(){
    note_game_thread();t::fn::rich_text_set=stock_set;t::fn::ui_footer_lookup=lookup;t::fn::text_display_string=display;t::fn::text_from_string=from_string;
    auto captions=make(u"[UE:Meta key=\"11111111111111111111111111111111\"/]Throw\nChomp\nJump");stock_set(label,&captions);
    const std::u16string original=u"[PictureFont:IconData key=\"A_Button\" scale=\"0.64\"/]\n[PictureFont:IconData key=\"A_Button\" scale=\"0.64\"/]\n[PictureFont:IconData key=\"X_Button\" scale=\"0.64\"/][PictureFont:IconData key=\"Long_Press_Button\"/]";
    game_set(original);ui_control_hints_start(nullptr);ui::set_backend(&backend);
    auto* owner=mod_create("hints.owner");auto* other=mod_create("hints.other");const auto& ui=ui::table;
    const char* r="[PictureFont:IconData key=\"R_Button\" scale=\"0.76\"/]";
    assert(ui.hold_control_hint(owner,"ActionMarker","ThrowPikmin",r)==TSUCHI_OK);
    assert(ui.hold_control_hint(other,"ActionMarker","ThrowPikmin","B")==TSUCHI_BUSY);
    assert(ui.hold_control_hint(owner,"ActionMarker","Attack","Space key")==TSUCHI_OK);
    assert(ui.hold_control_hint(owner,"ActionMarker","Jump","Unbound")==TSUCHI_OK);
    assert(ui.hold_control_hint(owner,"ActionMarker","missing","A")==TSUCHI_NOT_FOUND);
    assert(ui.hold_control_hint(owner,"ActionMarker","Jump","[bad]")==TSUCHI_INVALID);
    tick();assert(shown().find("R_Button")!=std::string::npos&&visible().find("Space key")!=std::string::npos&&visible().find("Unbound")!=std::string::npos&&visible().find("Long_Press_Button")!=std::string::npos);
    const int writes=sets,walks=scans;tick();assert(sets==writes&&scans==walks);
    assert(ui.hold_control_hint(owner,"ActionMarker","Attack","X key")==TSUCHI_OK);tick();assert(visible().find("X key")!=std::string::npos);
    auto native_value=make(original);t::fn::rich_text_set(widget,&native_value);
    assert(visible().find("X key")!=std::string::npos); // native per-tick writes are fixed before rendering
    const auto cached_count=texts.size();t::fn::rich_text_set(widget,&native_value);
    assert(texts.size()==cached_count); // reuse the generated FText for unchanged native sources
    game::EngineText shown_value{};std::memcpy(&shown_value,widget+t::ui_footer_label::text,sizeof shown_value);
    ui_control_hints_refreshing(true);game_set(u"");t::fn::rich_text_set(widget,&shown_value);ui_control_hints_refreshing(false);tick();
    assert(visible().find("X key")!=std::string::npos); // icon-style refresh preserves the original baseline
    assert(ui.release_control_hint(owner,"ActionMarker","ThrowPikmin")==TSUCHI_OK);tick();assert(shown().find("R_Button")==std::string::npos);
    mod_stop(owner);tick();assert(shown()==std::string(original.begin(),original.end()));
    owner=mod_create("hints.restart");assert(ui.hold_control_hint(owner,"ActionMarker","Jump","L key")==TSUCHI_OK);tick();game_set(u"foreign text");mod_stop(owner);tick();assert(shown()=="foreign text");
    diagram=true;owner=mod_create("hints.diagram");captions=make(u"Throw/Go to Work");stock_set(label,&captions);
    const std::u16string a=u"[PictureFont:IconData key=\"A_Button\" scale=\"0.66\"/]";game_set(a);
    assert(ui.hold_control_hint(owner,"ControlGuide","Fullcon_1P_Abutton",r)==TSUCHI_OK);tick();assert(shown().find("R_Button")!=std::string::npos&&shown().find("0.66")!=std::string::npos);
    const char* two="[PictureFont:IconData key=\"R_Button\" scale=\"0.76\"/] / [PictureFont:IconData key=\"ZL_Button\" scale=\"0.76\"/]";
    assert(ui.hold_control_hint(owner,"ControlGuide","Fullcon_1P_Abutton",two)==TSUCHI_OK);tick();
    assert(shown().empty()&&shown(label).find("ZL_Button")!=std::string::npos&&shown(label).find("Throw/Go to Work")!=std::string::npos);
    mod_stop(owner);tick();assert(shown(label)=="Throw/Go to Work"&&shown()==std::string(a.begin(),a.end()));owner=mod_create("hints.identities");
    captions=make(u"[UE:Meta key=\"33333333333333333333333333333333\"/]Select Pikmin");stock_set(label,&captions);game_set(a);
    assert(ui.hold_control_hint(owner,"ControlGuide","Fullcon_1P_Lbutton",r)==TSUCHI_OK);tick();assert(shown().find("R_Button")==std::string::npos); // identical words, different action identity
    captions=make(u"[UE:Meta key=\"22222222222222222222222222222222\"/]Select Pikmin");t::fn::rich_text_set(label,&captions);tick();assert(shown().find("R_Button")!=std::string::npos);
    mod_stop(owner);tick();diagram=2;owner=mod_create("hints.npc");captions=make(u"Talk");stock_set(label,&captions);game_set(a);
    assert(ui.hold_control_hint(owner,"ActionMarker","NPC_Talk","ZL key")==TSUCHI_OK);tick();assert(shown().empty()&&shown(label)=="ZL key Talk");
    auto foreign_caption=make(u"Someone else");t::fn::rich_text_set(label,&foreign_caption);
    mod_stop(owner);tick();assert(shown()==std::string(a.begin(),a.end())&&shown(label)=="Someone else");
    alive=false;tick();mod_stop(other);std::puts("control hint ownership, separate rows, native updates, identities, diagrams, NPC prompts and restoration passed");
}
