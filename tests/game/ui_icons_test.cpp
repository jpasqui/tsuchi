#include "ui_icons.h"
#include "ui_screens.h"
#include "ui_service.h"
#include "ui_feedback.h"
#include "holds.h"
#include "actor_events.h"
#include "objects.h"
#include "shared.h"
#include <cassert>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <vector>
#include <algorithm>
using namespace tsuchi;
namespace t=tsuchi::game::table;
namespace {
alignas(8) unsigned char data[96]{}, metadata[128]{};
alignas(8) unsigned char widget[0x600]{};
int scans=0, syncs=0, text_sets=0;
struct Row { game::Name key; alignas(8) unsigned char brush[136]{}; };
Row rows[3]{{{1,0},{}},{{2,0},{}},{{3,0},{}}};
std::map<uint64_t,int> pins;
std::vector<uint64_t> dropped;
std::vector<void(*)()> frame_work;
uint64_t fail_pin=0; bool asset_alive=true;
TSUCHI_Mod* stop_at_drop=nullptr;
ui::Backend backend{};
uint64_t ptr(const void* p){return reinterpret_cast<uint64_t>(p);}
template<class T> void put(void* p,const T& v){std::memcpy(p,&v,sizeof v);}
uint64_t resource(unsigned row){uint64_t v=0;std::memcpy(&v,rows[row].brush+72,8);return v;}
void resource(unsigned row,uint64_t value){put(rows[row].brush+72,value);}
void cache(unsigned row,uint64_t value){put(rows[row].brush+112,value);put(rows[row].brush+120,value);}
bool in(uint64_t p,uint64_t n,const void* base,size_t bytes){return p>=ptr(base)&&n<=bytes&&p-ptr(base)<=bytes-n;}
void sync_text(void*){++syncs;}
game::EngineText blank_text(const void*){game::EngineText value{};uint64_t data=300;std::memcpy(value.bytes,&data,8);return value;}
void set_text(void* object,void* text){assert(object==widget);std::memcpy(widget+t::ui_footer_label::text,text,20);std::memset(widget+t::ui_footer_label::text+20,0xA5,4);++text_sets;}
}
namespace tsuchi {
void ui_control_hints_start(TSUCHI_Mod*){}
void ui_control_hints_refreshing(bool){}
bool each_frame(void(*)()){return true;}
bool each_engine_frame(void(*f)()){frame_work.push_back(f);return true;}
bool on_actor_left(void(*)(uint64_t,uint64_t,ActorLeft)){return true;}
void ordinary_lifecycle_listener(void(*)(uint64_t,uint64_t,bool)){}
TSUCHI_Result ordinary_lifecycle_watch(uint64_t){return TSUCHI_OK;}
uint64_t ordinary_lifecycle_restore_part(uint64_t part,uint64_t){return part;}
bool retain_game_object(uint64_t h){if(h==fail_pin&&h)return false;if(h)++pins[h];return true;}
void release_game_object(uint64_t h){if(h){assert(pins[h]>0);if(!--pins[h])pins.erase(h);}}
bool ui_retain_text(const game::EngineText&){return true;}
ui::Backend& ui_screen_backend(){return backend;}
}
namespace tsuchi::game {
uint64_t main_base(){return 0;}
bool readable(uint64_t p,uint64_t n){return in(p,n,data,sizeof data)||in(p,n,metadata,sizeof metadata)||in(p,n,rows,sizeof rows)||in(p,n,widget,sizeof widget);}
bool writable(uint64_t p,uint64_t n){return readable(p,n)||p==900;}
bool executable(uint64_t){return false;}
uint64_t from_handle(uint64_t h){return h==400?ptr(widget):h==100?(asset_alive?ptr(data):0):(h>=200&&h<=299?h:0);}
uint64_t to_handle(uint64_t o){return o==ptr(widget)?400:o==ptr(data)?100:o;}
uint64_t part_owner(uint64_t h){return h;}
bool is_game_actor(uint64_t){return false;}
uint64_t type_of(uint64_t){return 10;}
bool is_a(uint64_t o,ClassRef& c){return !std::strcmp(c.name,t::name::ui_footer_text_class)?o==ptr(widget):!std::strcmp(c.name,"InTextIconData")?o==ptr(data):!std::strcmp(c.name,"Texture2D")&&o>=200&&o<=299;}
Name name_of(uint64_t){return {50,0};}
bool name_is(Name name,const char* text){return name.index==50&&!std::strcmp(text,"SlateBrush");}
TSUCHI_Result existing_name_from_utf8(const char* key,Name& out){
    if(!std::strcmp(key,"A_Button"))out={1,0};else if(!std::strcmp(key,"B_Button"))out={2,0};else return TSUCHI_NOT_FOUND;
    return TSUCHI_OK;
}
TSUCHI_Result find_place(uint64_t,uint64_t,const char* path,Place& out){
    if(!std::strcmp(path,"Text")){out={ptr(widget)+t::ui_footer_label::text,4,false};return TSUCHI_OK;}
    if(!std::strcmp(path,"Icons")){out={ptr(data),1,false};return TSUCHI_OK;}
    if(std::strncmp(path,"Icons[",6))return TSUCHI_NOT_FOUND;
    char* end=nullptr;const auto i=std::strtoul(path+6,&end,10);
    if(i>=3||!end||*end!=']')return TSUCHI_NOT_FOUND;
    const char* tail=end+1;
    if(!std::strcmp(tail,".Key"))out={ptr(&rows[i].key),2,false};
    else if(!std::strcmp(tail,".Icon"))out={ptr(rows[i].brush),ptr(metadata),false};
    else if(!std::strcmp(tail,".Icon.ResourceObject"))out={ptr(rows[i].brush+72),3,false};
    else return TSUCHI_NOT_FOUND;
    return TSUCHI_OK;
}
TSUCHI_Result struct_at(const Place& p,uint64_t& type,uint64_t& base){type=50;base=p.address;return TSUCHI_OK;}
TSUCHI_Result read_value(const Place& p,Value& out){
    out={};if(p.property==1){out.kind=ValueKind::array;out.count=3;return TSUCHI_OK;}
    if(p.property!=3)return TSUCHI_UNSUPPORTED;
    out.kind=ValueKind::object;std::memcpy(&out.object,reinterpret_cast<void*>(p.address),8);return TSUCHI_OK;
}
TSUCHI_Result write_value(const Place& p,const Value& v){assert(p.property==3);put(reinterpret_cast<void*>(p.address),v.object);return TSUCHI_OK;}
TSUCHI_Result read_engine_text(const Place&,char* out,size_t n,size_t*){const char text[]="<img id='PictureFont:IconData:A_Button'/>";assert(n>=sizeof text);std::memcpy(out,text,sizeof text);return TSUCHI_OK;}
bool each_object_from(int32_t&,bool(*visit)(void*,uint64_t),void* context){++scans;visit(context,ptr(widget));return true;}
void drop_shared(uint64_t counter){
    if(counter)dropped.push_back(counter);
    if(counter&&stop_at_drop){auto* owner=stop_at_drop;stop_at_drop=nullptr;mod_stop(owner);}
}
}
int main(){
    note_game_thread();put(metadata+t::property::element_bytes,int32_t(136));
    t::fn::ui_rich_text_sync=sync_text;t::fn::text_from_string=blank_text;t::fn::rich_text_set=set_text;
    put(widget+t::ui_rich_text::slate,uint64_t(1));
    game::EngineText original{};put(original.bytes,uint64_t(350));put(original.bytes+t::text_value::flags,uint32_t(7));
    put(widget+t::ui_footer_label::text,original);
    resource(0,211);resource(1,212);resource(2,213);
    ui_icons_start(nullptr);ui::set_backend(&backend);
    auto* owner=mod_create("icons.owner");auto* other=mod_create("icons.other");
    const auto& ui=ui::table;
    assert(ui.hold_icon_texture(owner,{100},"missing",{220})==TSUCHI_NOT_FOUND);
    assert(ui.hold_icon_texture(owner,{100},"",{220})==TSUCHI_INVALID);
    assert(ui.hold_icon_texture(owner,{100},"A_Button",{100})==TSUCHI_INVALID);
    cache(0,900);assert(ui.hold_icon_texture(owner,{100},"A_Button",{220})==TSUCHI_OK);
    assert(resource(0)==220&&dropped.back()==900&&pins[100]==1&&pins[211]==1&&pins[220]==1);
    for(auto work:frame_work)work();
    assert(scans==1&&syncs==1&&text_sets==2&&!std::memcmp(widget+t::ui_footer_label::text,original.bytes,20)&&!pins.count(400));
    for(auto work:frame_work)work();assert(scans==1); // no idle scans
    assert(ui.hold_icon_texture(other,{100},"A_Button",{221})==TSUCHI_BUSY);
    assert(ui.hold_icon_texture(owner,{100},"A_Button",{220})==TSUCHI_OK&&pins[220]==1);
    assert(ui.hold_icon_texture(owner,{100},"A_Button",{221})==TSUCHI_OK&&pins.count(220)==0);
    std::swap(rows[0],rows[1]);
    assert(ui.release_icon_texture(owner,{100},"A_Button")==TSUCHI_OK&&resource(1)==211&&pins.empty());
    assert(ui.release_icon_texture(owner,{100},"A_Button")==TSUCHI_NOT_FOUND);
    assert(ui.hold_icon_texture(owner,{100},"A_Button",{220})==TSUCHI_OK);
    resource(1,230);assert(ui.hold_icon_texture(owner,{100},"A_Button",{221})==TSUCHI_BUSY);
    assert(ui.release_icon_texture(owner,{100},"A_Button")==TSUCHI_OK&&resource(1)==230&&pins.empty());
    resource(1,211);fail_pin=220;
    assert(ui.hold_icon_texture(owner,{100},"A_Button",{220})==TSUCHI_NO_MEMORY&&resource(1)==211&&pins.empty());fail_pin=0;
    rows[2].key=rows[1].key;
    assert(ui.hold_icon_texture(owner,{100},"A_Button",{220})==TSUCHI_BUSY&&pins.empty());rows[2].key={3,0};
    put(metadata+t::property::element_bytes,int32_t(144));
    assert(ui.hold_icon_texture(owner,{100},"A_Button",{220})==TSUCHI_UNSUPPORTED&&pins.empty());
    put(metadata+t::property::element_bytes,int32_t(136));
    cache(1,900);stop_at_drop=owner;
    assert(ui.hold_icon_texture(owner,{100},"A_Button",{220})==TSUCHI_NOT_READY);
    assert(resource(1)==211&&pins.empty());
    owner=mod_create("icons.restarted");
    assert(ui.hold_icon_texture(owner,{100},"A_Button",{220})==TSUCHI_OK);
    assert(ui.hold_icon_texture(owner,{100},"B_Button",{221})==TSUCHI_OK);
    mod_stop(owner);assert(resource(1)==211&&resource(0)==212&&pins.empty());
    for(auto work:frame_work)work();
    mod_stop(other);std::puts("icon ownership, cache invalidation, row movement, foreign writes, pins and reentrant stop passed");
}
