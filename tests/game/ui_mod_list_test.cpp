#include "ui_mod_list.h"
#include "ui_mod_list_native.h"
#include "ui_settings.h"
#include "ui_screens.h"
#include "ui_service.h"
#include "dialog_service.h"
#include "mod_choices.h"
#include "external_mods_game.h"
#include "mods.h"
#include "actor_events.h"
#include "ordinary_lifecycle.h"
#include "cost_clock.h"
#include "game.h"
#include <cassert>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <string>
#include <fstream>
#include <iterator>
using namespace tsuchi;
namespace {
std::vector<void(*)()> steps;
ModRecord records[3]{};
UiModListNativeCallbacks callbacks{};
bool (*dispatcher)(uint64_t,uint32_t,bool&,bool&) = nullptr;
void (*listener)(uint64_t) = nullptr;
uint64_t active = 0;
uint32_t focus = 0, saves = 0, settings_calls = 0, destroyed_views = 0;
TSUCHI_Result save_result = TSUCHI_OK;
ExternalMods external;
uint32_t external_saves = 0, external_loads = 0;
TSUCHI_Result external_save_result = TSUCHI_OK, external_load_result = TSUCHI_OK;
std::string saved_external;
TSUCHI_Dialog asked{}; std::string prompt, message;
TSUCHI_Mod* framework = nullptr;
std::vector<std::string> tab_samples;
unsigned menu_calls = 0;
bool reject_menu_action = false, state_readable = false;
alignas(8) uint8_t diagnostic_host_memory[0x900]{};
const uint64_t diagnostic_host = reinterpret_cast<uint64_t>(diagnostic_host_memory);
void (*note_callback)() = nullptr;
unsigned expected_note_menu_calls = 0;
bool note_ended_session = false;
void reenter_held_sample() {listener(uint64_t(1)<<7);}
uint64_t number(const std::string& line, const char* field) {
 const std::string key=std::string(" ")+field+"=";const size_t at=line.find(key);assert(at!=std::string::npos);
 char* end=nullptr;const auto value=std::strtoull(line.c_str()+at+key.size(),&end,10);
 assert(end&&(*end==0||*end==' '));return value;
}
void end_session_from_note() {
 assert(menu_calls==expected_note_menu_calls&&active==diagnostic_host);
 callbacks.host_lost(diagnostic_host);note_ended_session=true;
}
TSUCHI_Result open_view(const UiScreen&,void** out) { *out = reinterpret_cast<void*>(1); return TSUCHI_OK; }
bool update_view(void*,const UiScreen&) { return true; }
bool live_view(void*) { return true; }
void close_view(void*) { ++destroyed_views; }
UiRenderer renderer{open_view,update_view,live_view,close_view};
TSUCHI_Result confirm(TSUCHI_Mod*,const char* text,TSUCHI_Dialog* out) { prompt=text; *out={++asked.id}; return TSUCHI_OK; }
TSUCHI_Result show(TSUCHI_Mod*,const char* text,TSUCHI_Dialog* out) { message=text; *out={999}; return TSUCHI_OK; }
const dialog::Backend dialog_backend{confirm,show};
void answer(bool yes) { dialog::emit_answered_to(framework,{asked,yes}); }
bool request(uint32_t index) { UiModListRow row{}; assert(callbacks.row(index,row)); return row.requested; }
void select(const char* control) { const auto* screen=ui_active_screen(); assert(screen); ui::emit_activated_to(framework,{{screen->id},control}); }
void step() { for(auto fn:steps) fn(); }
uint32_t find_row(const char* id, uint32_t first = 0) {
 for(uint32_t i=first;i<callbacks.count();++i) { UiModListRow row{};assert(callbacks.row(i,row));if(!strcmp(row.id,id))return i; }
 assert(false);return UINT32_MAX;
}
const UiControl* control(const char* id) {
 const auto* screen=ui_active_screen();assert(screen);
 for(uint32_t i=0;i<screen->controls.count;++i) if(!strcmp(screen->controls[i].id,id))return &screen->controls[i];
 return nullptr;
}
std::string large_catalog(uint32_t count,bool collision_last=false) {
 std::string source="{\"format\":1,\"revision\":\""+std::string(64,'b')+"\",\"backend\":\"companion\",\"mods\":[";
 for(uint32_t i=0;i<count;++i) {
  if(i)source+=",";
  const std::string id=collision_last&&i+1==count?"external.collision":"external.large"+std::to_string(i);
  source+="{\"id\":\""+id+"\",\"name\":\"Package\",\"version\":\"\",\"kind\":\"romfs\",\"state\":\"disabled\",\"problem\":\"\",\"manageable\":true}";
 }
 return source+"]}";
}
}
namespace tsuchi {
void mod_note(const char*,const char* text) {
 assert(std::string(text).find("Settings tab sample ")==0);tab_samples.emplace_back(text);
 if(note_callback) {const auto callback=note_callback;note_callback=nullptr;callback();}
}
bool ui_native_prepare_input() { return false; }
// This fixture keeps its existing world-step pipeline; engine routing is
// exercised separately by the real Showing scheduling regression.
bool each_engine_frame(void(*)()) { return false; }
bool each_frame(void(*fn)()) { steps.push_back(fn); return true; }
bool on_actor_left(void(*)(uint64_t,uint64_t,ActorLeft)) { return true; }
void ordinary_lifecycle_listener(void(*)(uint64_t,uint64_t,bool)) {}
TSUCHI_Result ordinary_lifecycle_watch(uint64_t) { return TSUCHI_OK; }
uint64_t ordinary_lifecycle_restore_part(uint64_t part,uint64_t) { return part; }
bool mod_file_path(Texts& out,const TSUCHI_Mod*,const char* path) { return out.add(path); }
uint32_t mod_count() { return 3; }
ModRecord* mod_at(uint32_t index) { return index<3? &records[index]:nullptr; }
ModRecord* find_mod(const char* id) { for(auto& record:records) if(!strcmp(record.id,id)) return &record; return nullptr; }
bool mod_requirement_satisfied(const ModRecord&,const char*) { return true; }
ExternalMods& external_mods() { return external; }
TSUCHI_Result load_external_mods() { ++external_loads; return external_load_result; }
TSUCHI_Result save_external_mods() {
 return external.save([](const char* text,uint64_t size) {
  ++external_saves;if(external_save_result!=TSUCHI_OK)return external_save_result;
  saved_external.assign(text,size);return TSUCHI_OK;
 });
}
TSUCHI_Result save_mod_choices(const ModChoices& choices) {
 ++saves; if(save_result!=TSUCHI_OK) return save_result;
 Text a,o; ModChoices next; assert(choices.activation_text(a)&&choices.order_text(o));
 assert(next.load(a.get(),o.get())==TSUCHI_OK); swap_mod_choices(next); next.clear(); a.clear();o.clear();return TSUCHI_OK;
}
TSUCHI_Result ui_mod_settings(ModRecord& record,TSUCHI_Screen* out) { ++settings_calls; auto result=ui::table.create(framework,record.id,out); if(result==TSUCHI_OK) result=ui::table.add_text(framework,*out,"test","Test","value");return result; }
void ui_mod_list_native_callbacks(const UiModListNativeCallbacks* value) { callbacks=*value; }
void ui_mod_list_native_start(TSUCHI_Mod*) {}
void ui_menu_dispatcher(bool(*fn)(uint64_t,uint32_t,bool&,bool&)) { dispatcher=fn; }
void ui_button_listener(void(*fn)(uint64_t)) { listener=fn; }
bool ui_menu_action(uint64_t host,uint32_t action) { ++menu_calls;if(reject_menu_action)return false;bool handled=false,result=false; return dispatcher(host,action,handled,result)&&handled&&result; }
TSUCHI_Result ui_mod_list_native_hold(uint64_t host) { active=host; return TSUCHI_OK; }
TSUCHI_Result ui_mod_list_native_release(uint64_t) { active=0;return TSUCHI_OK; }
TSUCHI_Result ui_mod_list_native_cycle(uint64_t host,bool previous) { return previous ? ui_mod_list_native_release(host) : ui_mod_list_native_hold(host); }
bool ui_mod_list_native_active(uint64_t host) { return active==host; }
TSUCHI_Result ui_mod_list_native_sync(uint64_t) { return TSUCHI_OK; }
TSUCHI_Result ui_mod_list_native_focus(uint64_t,uint32_t index) { focus=index; return TSUCHI_OK; }
bool ui_mod_list_native_dispatch(uint64_t host,uint32_t action,bool& handled,bool& result) { if(!active)return false;handled=true;result=callbacks.action(host,focus,action,request(focus));return true; }
}
namespace tsuchi::game {
bool readable(uint64_t address,uint64_t bytes) {return state_readable&&address==diagnostic_host+table::ui_host::state&&bytes==1;}
uint64_t from_handle(uint64_t id) { return id; }
uint64_t to_handle(uint64_t id) { return id; }
uint64_t part_owner(uint64_t id) { return id; }
bool is_game_actor(uint64_t) { return true; }
bool path_stays_within(const char*) { return true; }
TSUCHI_Result read_whole_file(const char*,char*&,uint64_t&) { return TSUCHI_NOT_FOUND; }
bool utf8_to_units(const char* text,char16_t* out,size_t capacity,size_t& count) {
    count = 0; const unsigned char* at = reinterpret_cast<const unsigned char*>(text);
    while (*at) {
        const uint32_t first = *at++; uint32_t code = first, extra = 0;
        if (first >= 0xf0) { code = first & 7; extra = 3; }
        else if (first >= 0xe0) { code = first & 15; extra = 2; }
        else if (first >= 0xc0) { code = first & 31; extra = 1; }
        else if (first >= 0x80) return false;
        for (uint32_t i = 0; i < extra; ++i) {
            if ((*at & 0xc0) != 0x80) return false;
            code = code * 64 + (*at++ & 63);
        }
        const size_t units = code > 0xffff ? 2 : 1;
        if (out && count + units > capacity) return false;
        if (out) {
            if (units == 1) out[count] = char16_t(code);
            else { out[count] = char16_t(0xd800 + ((code - 0x10000) >> 10)); out[count + 1] = char16_t(0xdc00 + ((code - 0x10000) & 1023)); }
        }
        count += units;
    }
    return true;
}
}
namespace {
void diagnostic_samples() {
 // Existing behavior ran without a clock. Its first host-ready sample is
 // still diagnostic evidence and must not disappear just because time is0.
 assert(!tab_samples.empty());
 assert(number(tab_samples.front(),"at_us")==0&&number(tab_samples.front(),"previous")==UINT64_MAX);
 assert(number(tab_samples.front(),"held")==0&&number(tab_samples.front(),"edges")==0);
 const size_t historical=tab_samples.size();assert(historical<48);
 framework=mod_create("tsuchi.tab-samples");assert(framework);ui_mod_list_start(framework);
 diagnostic_host_memory[game::table::ui_host::state]=1;state_readable=true;cost_clock=[] {return uint64_t(4242);};
 callbacks.host_ready(diagnostic_host);
 const uint64_t R=uint64_t(1)<<7;
 const auto sample=[&](uint64_t held) {const size_t before=tab_samples.size();listener(held);assert(tab_samples.size()==before+1);};
 sample(R);const uint64_t session=number(tab_samples.back(),"session");
 assert(number(tab_samples.back(),"at_us")==4242&&number(tab_samples.back(),"host")==diagnostic_host);
 assert(number(tab_samples.back(),"state")==1&&number(tab_samples.back(),"live")==1&&number(tab_samples.back(),"gates")==0);
 assert(number(tab_samples.back(),"previous")==UINT64_MAX&&number(tab_samples.back(),"held")==R&&number(tab_samples.back(),"edges")==0);
 assert(number(tab_samples.back(),"action")==0&&number(tab_samples.back(),"attempted")==0&&number(tab_samples.back(),"result")==0);
 sample(R);assert(number(tab_samples.back(),"previous")==R&&number(tab_samples.back(),"edges")==0);
 sample(0);assert(number(tab_samples.back(),"previous")==R&&number(tab_samples.back(),"held")==0&&number(tab_samples.back(),"edges")==0);
 const unsigned before_rejection=menu_calls;reject_menu_action=true;
 sample(R);assert(menu_calls==before_rejection+1);
 assert(number(tab_samples.back(),"edges")==R&&number(tab_samples.back(),"action")==100&&number(tab_samples.back(),"attempted")==1&&number(tab_samples.back(),"result")==0);
 sample(R);assert(menu_calls==before_rejection+1&&number(tab_samples.back(),"attempted")==0);reject_menu_action=false;
 sample(0);
 assert(callbacks.action(diagnostic_host,0,7,true));sample(R);
 assert(number(tab_samples.back(),"gates")==2&&number(tab_samples.back(),"action")==0&&number(tab_samples.back(),"attempted")==0);answer(false);
 sample(0);
 assert(callbacks.action(diagnostic_host,0,102,true)&&ui_active_screen());sample(R);
 assert(number(tab_samples.back(),"gates")==4&&number(tab_samples.back(),"attempted")==0);
 assert(ui_action(5));step();assert(!ui_active_screen());
 state_readable=false;sample(0);assert(number(tab_samples.back(),"state")==255);state_readable=true;
 uint32_t configured=0;UiModListRow row{};
 while(configured<callbacks.count()&&callbacks.row(configured,row)&&!row.has_settings)++configured;
 assert(configured<callbacks.count()&&callbacks.action(diagnostic_host,configured,4,true)&&ui_active_screen());sample(R);
 assert(number(tab_samples.back(),"gates")==8&&number(tab_samples.back(),"attempted")==0);
 assert(ui_action(5));step();assert(!ui_active_screen());sample(0);
 assert(tab_samples.size()==historical+12);
 for(size_t i=historical;i<tab_samples.size();++i) {
  assert(number(tab_samples[i],"session")==session);
  if(i>historical)assert(number(tab_samples[i],"sample")==number(tab_samples[i-1],"sample")+1);
 }
 // Exhausting diagnostics changes no input behavior.
 const size_t capped=tab_samples.size();const unsigned before_capped_action=menu_calls;
 listener(R);assert(tab_samples.size()==capped&&menu_calls==before_capped_action+1&&active==diagnostic_host);
 listener(R);listener(0);assert(tab_samples.size()==capped&&menu_calls==before_capped_action+1);
 callbacks.host_lost(diagnostic_host);ui_mod_list_native_release(diagnostic_host);
 callbacks.host_ready(diagnostic_host);sample(0);
 assert(number(tab_samples.back(),"session")>session&&number(tab_samples.back(),"previous")==UINT64_MAX);
 note_ended_session=false;expected_note_menu_calls=menu_calls+1;note_callback=end_session_from_note;sample(R);
 assert(note_ended_session&&menu_calls==expected_note_menu_calls&&!callbacks.row(0,row));
 ui_mod_list_native_release(diagnostic_host);
 callbacks.host_ready(diagnostic_host);const size_t before_nested=tab_samples.size();
 for(unsigned i=0;i<11;++i)sample(R);
 note_callback=reenter_held_sample;sample(R);
 assert(tab_samples.size()==before_nested+12); // reserve before a note callback can sample again
 listener(R);assert(tab_samples.size()==before_nested+12);
 while(tab_samples.size()<48) {
  callbacks.host_lost(diagnostic_host);callbacks.host_ready(diagnostic_host);
  const size_t before=tab_samples.size();
  for(unsigned i=0;i<16;++i)listener(R);
  const size_t expected=before+12<48?before+12:48;assert(tab_samples.size()==expected);
 }
 callbacks.host_lost(diagnostic_host);callbacks.host_ready(diagnostic_host);
 listener(0);const unsigned before_global_action=menu_calls;listener(R);
 assert(tab_samples.size()==48&&menu_calls==before_global_action+1&&active==diagnostic_host);
 mod_stop(framework);ui_mod_list_native_release(diagnostic_host);cost_clock=nullptr;state_readable=false;
}
}
int main(int argc,char** argv) {
 note_game_thread(); framework=mod_create("tsuchi.self"); assert(framework);
 const char* ids[]={"base","leaf","other"};
 for(uint32_t i=0;i<3;++i) { memcpy(records[i].id,ids[i],strlen(ids[i])+1); assert(records[i].name.set(ids[i])); records[i].manifest_valid=true; records[i].state=ModState::running;
 const char* text=i==1? "{\"requires\":{\"base\":\"*\"}}":"{}";
 records[i].manifest=read_json(text,strlen(text)); assert(records[i].manifest); }
 ui_start();ui_set_renderer(&renderer);dialog::set_backend(&dialog_backend);ui_mod_list_start(framework);
 // The Python companion test can drive the same menu controller against a
 // disposable catalog, then apply the emitted requests to its install tree.
 if(argc>=3&&argc<=5) {
  std::ifstream input(argv[1],std::ios::binary);
  if(!input)return 2;
  const std::string source{std::istreambuf_iterator<char>(input),std::istreambuf_iterator<char>()};
  if(external.load(source.c_str(),source.size())!=TSUCHI_OK)return 3;
  callbacks.host_ready(900);assert(ui_mod_list_native_hold(900)==TSUCHI_OK);
  uint32_t index=3;
  while(index<callbacks.count()&&(!external.at(index-3)->manageable||
        (argc==5&&strcmp(external.at(index-3)->id,argv[4]))))++index;
  if(index==callbacks.count())return 5;
  auto* entry=external.at(index-3);const auto before=entry->observed;
  const bool enabled=argc==3?!entry->requested:!strcmp(argv[3],"enable");
  if(argc>=4&&!enabled&&strcmp(argv[3],"disable"))return 4;
  callbacks.action(900,index,enabled?3:2,enabled);
  const bool changed=external.changed();callbacks.action(900,index,5,false);
  if(changed)answer(true);
  assert(!active&&entry->observed==before&&!external.changed());
  Text encoded;if(!external.requests_json(encoded))return 6;
  std::ofstream output(argv[2],std::ios::binary);if(!output)return 7;
  output<<encoded.get();encoded.clear();if(!output)return 8;
  mod_stop(framework);external.clear();
  for(auto& record:records) { record.name.clear();release_json(record.manifest); }
  return 0;
 }
 callbacks.host_ready(100); assert(callbacks.count()==3);
 listener(0);listener(uint64_t(1)<<7);assert(active==100);
 assert(callbacks.action(100,0,2,false));assert(prompt.find("Off: base")!=std::string::npos&&prompt.find("Off: leaf")!=std::string::npos);
 answer(false);assert(request(0)&&request(1));
 callbacks.action(100,0,2,false);answer(true);assert(!request(0)&&!request(1));
 // B with a draft asks to save it: no discards the draft and leaves.
 callbacks.action(100,0,5,false);assert(prompt=="Save changes for the next restart?");answer(false);assert(!active&&saves==0);
 assert(ui_mod_list_native_hold(100)==TSUCHI_OK);assert(request(0)&&request(1));
 callbacks.action(100,0,2,false);answer(true);assert(!request(0)&&!request(1));
 UiModListRow row{};assert(callbacks.row(0,row)&&!strcmp(row.status,"Restart"));
 // A failed save keeps the draft open; a saved one leaves.
 callbacks.action(100,0,5,false);save_result=TSUCHI_NOT_READY;answer(true);assert(saves==1&&mod_choices().enabled("base")&&active);
 callbacks.action(100,0,5,false);save_result=TSUCHI_OK;answer(true);assert(saves==2&&!mod_choices().enabled("base")&&!mod_choices().enabled("leaf")&&!active);
 assert(ui_mod_list_native_hold(100)==TSUCHI_OK);assert(callbacks.row(0,row)&&!strcmp(row.status,"Restart"));assert(!callbacks.row(3,row));
 // B without a draft just leaves.
 assert(callbacks.action(100,0,5,false)&&!active); assert(ui_mod_list_native_hold(100)==TSUCHI_OK);
 callbacks.action(100,2,102,true);assert(ui_active_screen());
 const auto* details=ui_active_screen();for(uint32_t i=0;i<details->controls.count;++i)assert(strcmp(details->controls[i].id,"settings")&&strcmp(details->controls[i].id,"back"));
 select("earlier");assert(!ui_active_screen()&&focus==1);
 callbacks.action(100,1,4,true);assert(!settings_calls&&!ui_active_screen());
 assert(records[2].settings.list.add(Setting{}));
 callbacks.action(100,1,4,true);assert(settings_calls==1&&ui_active_screen());
 ui_action(5);step(); assert(!ui_active_screen());
 callbacks.action(100,0,5,false);answer(false);assert(!active);assert(ui_mod_list_native_hold(100)==TSUCHI_OK);assert(callbacks.row(2,row)&&!strcmp(row.id,"other"));
 listener(0);listener(uint64_t(1)<<6);assert(!active);listener(0);listener(uint64_t(1)<<7);assert(active);
 callbacks.action(100,0,102,true);assert(ui_active_screen());callbacks.host_lost(100);assert(!ui_active_screen());
 callbacks.host_ready(101);assert(callbacks.count()==3&&!request(0));
 assert(ui_mod_list_native_hold(101)==TSUCHI_OK);
 callbacks.action(101,0,7,true);assert(prompt=="Restore default mod activation and order?"&&!ui_active_screen());
 answer(false);assert(!request(0));
 callbacks.action(101,0,7,true);answer(true);assert(request(0)&&request(1)&&request(2));
 callbacks.action(101,0,5,false);assert(prompt=="Save changes for the next restart?");answer(false);assert(!active&&!request(0));
 callbacks.host_lost(101);
 // External entries join the list without acquiring native contexts or changing files.
 memcpy(records[2].id,"external.collision",sizeof("external.collision"));
 const std::string catalog = "{\"format\":1,\"revision\":\""+std::string(64,'a')+"\",\"backend\":\"companion\",\"mods\":["
  "{\"id\":\"external.assets\",\"name\":\"[Assets]\",\"version\":\"1.0\",\"kind\":\"romfs\",\"state\":\"enabled\",\"problem\":\"\",\"manageable\":true},"
  "{\"id\":\"external.patch\",\"name\":\"Patch\",\"version\":\"1\",\"kind\":\"exefs\",\"state\":\"disabled\",\"problem\":\"\",\"manageable\":true},"
  "{\"id\":\"external.cheats\",\"name\":\"Cheats\",\"version\":\"\",\"kind\":\"cheats\",\"state\":\"enabled\",\"problem\":\"Cheat control is unavailable.\",\"manageable\":false},"
  "{\"id\":\"external.conflict\",\"name\":\"Conflict\",\"version\":\"\",\"kind\":\"mixed\",\"state\":\"conflict\",\"problem\":\"A foreign file occupies the destination.\",\"manageable\":true},"
  "{\"id\":\"external.collision\",\"name\":\"Duplicate ID\",\"version\":\"\",\"kind\":\"mixed\",\"state\":\"enabled\",\"problem\":\"\",\"manageable\":true},"
  "{\"id\":\"external.recovery\",\"name\":\"Interrupted\",\"version\":\"\",\"kind\":\"mixed\",\"state\":\"recovery\",\"problem\":\"Finish recovery before launch.\",\"manageable\":false}]}";
 assert(external.load(catalog.c_str(),catalog.size())==TSUCHI_OK);
 callbacks.host_ready(102);assert(external_loads==3&&callbacks.count()==9);
 assert(ui_mod_list_native_hold(102)==TSUCHI_OK);
 const uint32_t assets=find_row("external.assets",3),patch=find_row("external.patch",3);
 const uint32_t cheats=find_row("external.cheats",3),conflict=find_row("external.conflict",3);
 const uint32_t collision=find_row("external.collision",3),recovery=find_row("external.recovery",3);
 assert(callbacks.row(assets,row)&&!strcmp(row.name,"(Assets)")&&!strcmp(row.status,"Files enabled")&&row.requested);
 assert(callbacks.row(patch,row)&&!strcmp(row.status,"Files disabled")&&!row.requested);
 assert(callbacks.row(cheats,row)&&!strcmp(row.status,"Detected - unmanaged")&&!row.requested);
 assert(callbacks.row(collision,row)&&!strcmp(row.status,"ID conflict")&&!row.requested);
 assert(callbacks.row(conflict,row)&&!strcmp(row.status,"Conflict"));
 assert(callbacks.row(recovery,row)&&!strcmp(row.status,"Recovery required"));
 assert(!row.has_settings);
 // Y restores native defaults from every row and retains the external draft.
 callbacks.action(102,assets,2,false);assert(external.changed());
 callbacks.action(102,assets,7,false);assert(prompt=="Restore default mod activation and order?");
 answer(false);assert(!request(find_row("base"))&&!request(find_row("leaf")));
 assert(!external.find("external.assets")->requested&&external.find("external.assets")->saved);
 callbacks.action(102,assets,7,false);answer(true);
 assert(request(find_row("base"))&&request(find_row("leaf"))&&request(find_row("external.collision")));
 assert(!external.find("external.assets")->requested&&external.find("external.assets")->saved&&external.changed());
 callbacks.action(102,0,5,false);answer(false);
 assert(!active&&!request(find_row("base"))&&!request(find_row("leaf")));
 assert(external.find("external.assets")->requested&&!external.changed());
 assert(ui_mod_list_native_hold(102)==TSUCHI_OK);
 callbacks.action(102,assets,4,true);assert(!ui_active_screen());
 for(uint32_t index:{cheats,conflict,collision,recovery}) {
  callbacks.action(102,index,2,true);assert(!external.changed()&&!message.empty());
 }
 assert(!external.find("external.collision")->manageable);
 callbacks.action(102,assets,102,true);assert(ui_active_screen());
 assert(control("files")&&!strcmp(control("files")->text.get(),"Files enabled"));
 assert(!control("settings")&&!control("earlier")&&!control("later"));
 const uint32_t settings_before=settings_calls;
 select("settings");assert(ui_active_screen()&&settings_calls==settings_before);ui_action(5);step();assert(!ui_active_screen());
 // An external draft changes requests only. Discard restores the saved request.
 callbacks.action(102,assets,2,false);assert(external.changed()&&external.find("external.assets")->observed==ExternalModState::enabled);
 assert(callbacks.row(assets,row)&&!strcmp(row.status,"Apply with manager")&&!row.requested);
 callbacks.action(102,assets,102,false);assert(!strcmp(control("request")->text.get(),"Disable: unsaved draft"));ui_action(5);step();assert(!ui_active_screen());
 callbacks.action(102,0,5,false);assert(prompt.find("external requests")!=std::string::npos);answer(false);
 assert(!active&&!external.changed()&&external.find("external.assets")->requested);
 assert(ui_mod_list_native_hold(102)==TSUCHI_OK);
 // A failed external-only save keeps its draft. A successful save never claims files changed.
 const uint32_t native_saves=saves;
 callbacks.action(102,patch,3,true);callbacks.action(102,0,5,false);external_save_result=TSUCHI_NOT_READY;answer(true);
 assert(active&&external.changed()&&external_saves==1&&saves==native_saves);
 callbacks.action(102,0,5,false);external_save_result=TSUCHI_OK;answer(true);
 assert(!active&&!external.changed()&&external.queued()&&external_saves==2&&saves==native_saves);
 assert(saved_external.find("external.patch")!=std::string::npos&&saved_external.find("external.cheats")==std::string::npos&&saved_external.find("external.collision")==std::string::npos);
 assert(external.find("external.patch")->observed==ExternalModState::disabled);
 assert(message.find("Requests saved")!=std::string::npos&&message.find("manager")!=std::string::npos);
 assert(ui_mod_list_native_hold(102)==TSUCHI_OK);
 callbacks.action(102,patch,102,true);assert(!strcmp(control("request")->text.get(),"Enable: saved for manager"));ui_action(5);step();assert(!ui_active_screen());
 callbacks.action(102,0,5,false);assert(!active&&external_saves==2);
 assert(ui_mod_list_native_hold(102)==TSUCHI_OK);
 // Mixed saves report partial success and retry only the remaining draft.
 uint32_t base=find_row("base");callbacks.action(102,base,3,true);answer(true);
 callbacks.action(102,assets,2,false);callbacks.action(102,0,5,false);external_save_result=TSUCHI_NOT_READY;answer(true);
 assert(active&&mod_choices().enabled("base")&&external.changed()&&saves==native_saves+1);
 assert(message.find("Native saved")!=std::string::npos&&message.find("External failed")!=std::string::npos&&message.find("draft kept")!=std::string::npos);
 callbacks.action(102,0,5,false);external_save_result=TSUCHI_OK;answer(true);
 assert(!active&&!external.changed()&&saves==native_saves+1);
 assert(ui_mod_list_native_hold(102)==TSUCHI_OK);
 base=find_row("base");callbacks.action(102,base,2,false);answer(true);
 callbacks.action(102,assets,3,true);callbacks.action(102,0,5,false);save_result=TSUCHI_NOT_READY;answer(true);
 assert(active&&mod_choices().enabled("base")&&!external.changed());
 assert(message.find("External saved")!=std::string::npos&&message.find("Native failed")!=std::string::npos&&message.find("draft kept")!=std::string::npos);
 const uint32_t external_saved_count=external_saves;
 callbacks.action(102,0,5,false);answer(false);assert(!active&&mod_choices().enabled("base")&&external_saves==external_saved_count);
 // Managed cheat rows have independent saved choices and passive information.
 callbacks.host_lost(102);
 const std::string cheat_catalog="{\"format\":1,\"revision\":\""+std::string(64,'c')+"\",\"backend\":\"companion\",\"mods\":["
  "{\"id\":\"external.code-a\",\"name\":\"Code A\",\"version\":\"\",\"kind\":\"cheats\",\"state\":\"enabled\",\"problem\":\"Required shared code: Master\",\"manageable\":true},"
  "{\"id\":\"external.code-b\",\"name\":\"Code B\",\"version\":\"\",\"kind\":\"cheats\",\"state\":\"disabled\",\"problem\":\"Required shared code: Master\",\"manageable\":true}]}";
 assert(external.load(cheat_catalog.c_str(),cheat_catalog.size())==TSUCHI_OK);
 callbacks.host_ready(1000);assert(callbacks.count()==5&&ui_mod_list_native_hold(1000)==TSUCHI_OK);
 const uint32_t code_a=find_row("external.code-a",3),code_b=find_row("external.code-b",3);
 assert(callbacks.row(code_a,row)&&row.requested&&!row.has_settings&&!strcmp(row.status,"Files enabled"));
 callbacks.action(1000,code_a,102,true);assert(control("effects")&&control("effects")->kind==UiKind::text);
 assert(control("apply")&&control("apply_export")&&control("apply_next")&&control("apply_restart")&&control("saved_effects"));
 assert(strstr(control("apply")->text.get(),"close game")&&strstr(control("apply_export")->text.get(),"Export"));
 assert(strstr(control("apply_next")->text.get(),"computer manager")&&strstr(control("apply_restart")->text.get(),"Restart"));
 assert(strstr(control("effects")->text.get(),"undo earlier writes")&&strstr(control("saved_effects")->text.get(),"saved effects"));
 assert(control("reason")&&strstr(control("reason")->text.get(),"Master"));
 assert(!control("settings")&&!control("earlier")&&!control("later"));
 ui_action(5);step();assert(!ui_active_screen());
 callbacks.action(1000,code_a,2,false);
 assert(external.changed()&&!request(code_a)&&!request(code_b));
 assert(external.find("external.code-a")->observed==ExternalModState::enabled);
 const uint32_t saves_before_cheat=external_saves;
 callbacks.action(1000,code_a,5,false);answer(true);
 assert(!active&&!external.changed()&&external_saves==saves_before_cheat+1);
 assert(!external.find("external.code-a")->saved&&!external.find("external.code-b")->saved);
 assert(external.find("external.code-a")->observed==ExternalModState::enabled&&external.find("external.code-b")->observed==ExternalModState::disabled);
 assert(saved_external.find("\"external.code-a\":false")!=std::string::npos&&saved_external.find("\"external.code-b\":false")!=std::string::npos);
 // Unread requests show one recovery message on entry, toggle and Back; no save question or writer.
 callbacks.host_lost(1000);external.requests_unread();external_load_result=TSUCHI_INVALID;message.clear();
 callbacks.host_ready(1001);assert(message=="Saved requests unread; reopen Mods.");
 assert(ui_mod_list_native_hold(1001)==TSUCHI_OK);
 const uint32_t unread_code=find_row("external.code-a",3), unread_saves=external_saves;
 const bool unread_requested=request(unread_code);message.clear();
 callbacks.action(1001,unread_code,3,!unread_requested);
 assert(message=="Saved requests unread; reopen Mods."&&request(unread_code)==unread_requested&&!external.changed());
 message.clear();prompt.clear();callbacks.action(1001,unread_code,5,false);
 assert(message=="Saved requests unread; reopen Mods."&&prompt.empty()&&external_saves==unread_saves&&active);
 assert(external.load(cheat_catalog.c_str(),cheat_catalog.size())==TSUCHI_OK);
 // Loading errors leave the native menu available and warn about external data.
 callbacks.host_lost(1001);external_load_result=TSUCHI_INVALID;message.clear();
 callbacks.host_ready(103);assert(callbacks.count()==5&&message.find("could not be loaded")!=std::string::npos);
 assert(callbacks.row(find_row("base"),row)&&!strcmp(row.id,"base"));
 // The renderer accepts 128 combined rows. Additional external packages stay
 // in the request model without making the native Mods tab unavailable.
 callbacks.host_lost(103);external_load_result=TSUCHI_OK;message.clear();
 const uint32_t row_limit=game::table::constant::ui_mod_list_rows_limit;assert(row_limit==128);
 std::string large=large_catalog(row_limit-3);
 assert(external.load(large.c_str(),large.size())==TSUCHI_OK);
 callbacks.host_ready(104);assert(callbacks.count()==row_limit&&external.count()==125&&message.empty());
 assert(callbacks.row(127,row)&&!strcmp(row.id,"external.large124"));
 assert(callbacks.row(find_row("base"),row)&&!strcmp(row.id,"base"));
 callbacks.host_lost(104);message.clear();large=large_catalog(row_limit-2);
 const std::string hidden_request="{\"format\":1,\"revision\":\""+std::string(64,'b')+"\",\"enabled\":{\"external.large125\":true}}";
 assert(external.load(large.c_str(),large.size(),hidden_request.c_str(),hidden_request.size())==TSUCHI_OK);
 callbacks.host_ready(105);assert(callbacks.count()==row_limit&&external.count()==126);
 assert(message.find("Menu full")!=std::string::npos&&message.find("computer manager")!=std::string::npos);
 assert(callbacks.row(127,row)&&!strcmp(row.id,"external.large124")&&!callbacks.row(128,row));
 assert(external.find("external.large125")->requested&&external.find("external.large125")->saved);
 assert(ui_mod_list_native_hold(105)==TSUCHI_OK);
 callbacks.action(105,find_row("external.large0",3),3,true);callbacks.action(105,0,5,false);answer(true);
 assert(!active&&external.count()==126&&saved_external.find("\"external.large125\":true")!=std::string::npos);
 assert(external.find("external.large125")->saved&&external.find("external.large125")->observed==ExternalModState::disabled);
 // Collision admission also covers a package beyond the visible row budget.
 callbacks.host_lost(105);large=large_catalog(row_limit-2,true);
 assert(external.load(large.c_str(),large.size())==TSUCHI_OK);
 callbacks.host_ready(106);assert(callbacks.count()==row_limit&&external.count()==126);
 assert(!external.find("external.collision")->manageable&&external.find("external.collision")->problem.get()[0]);
 Text filtered;assert(external.requests_json(filtered));
 assert(!strstr(filtered.get(),"external.collision"));filtered.clear();
 // A load warning and an overflow notice both remain visible when combined.
 callbacks.host_lost(106);external_load_result=TSUCHI_INVALID;message.clear();
 callbacks.host_ready(107);assert(message.find("Load failed")!=std::string::npos&&message.find("menu full")!=std::string::npos&&message.find("manager")!=std::string::npos);
 mod_stop(framework);assert(!callbacks.row(0,row));external.clear();external_load_result=TSUCHI_OK;
 diagnostic_samples();
 for(auto& record:records) { record.name.clear();record.settings.list.clear();release_json(record.manifest); }
 return 0;
}
