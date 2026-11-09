// Uses the real screen model with a runtime whose entries can change between frames.
#include "ui_cheats.h"
namespace {
struct FakeCheat {std::string mod,name,problem;bool on=false;};
std::vector<FakeCheat> fake_cheats;
TSUCHI_Result cheat_result=TSUCHI_OK;
uint32_t cheat_calls=0;
}
namespace tsuchi {
uint32_t cheat_count(){return uint32_t(fake_cheats.size());}
bool cheat_at(uint32_t index,CheatEntry& out){if(index>=fake_cheats.size())return false;const auto& c=fake_cheats[index];out={c.mod.c_str(),c.name.c_str(),c.on,c.problem.c_str()};return true;}
TSUCHI_Result set_cheat(const char* mod,const char* name,bool on){
 ++cheat_calls;for(auto& c:fake_cheats)if(c.mod==mod&&c.name==name){if(!c.problem.empty())return TSUCHI_UNSUPPORTED;if(cheat_result==TSUCHI_CAPACITY)return cheat_result;c.on=on;return cheat_result;}return TSUCHI_NOT_FOUND;
}
}
namespace {
void cheat_menu_checks(){
 ui_set_title_renderer(&renderer);
 fake_cheats={{"example","First","",false},{"unrelated","Foreign","",false},{"example","Blocked","no code access",false},{"example","Third","",false},{"example","Fourth","",false}};
 CheatMenu menu;assert(mod_has_cheats("example")&&!mod_has_cheats("missing"));assert(menu.open(framework,"example")==TSUCHI_OK);
 assert(control("cheat0")->kind==UiKind::choice&&control("cheat0")->selected==0);
 assert(control("cheat1")->kind==UiKind::text&&!strcmp(control("cheat1")->text.get(),"no code access"));assert(control("next"));
 const auto first=menu.screen();menu.activate({first,"next"});assert(menu.screen().id!=first.id&&!strcmp(control("cheat0")->label.get(),"Fourth"));
 menu.change({first,"cheat0","on","off"});assert(cheat_calls==0); // queued event from old page
 menu.activate({menu.screen(),"previous"});
 std::swap(fake_cheats[0],fake_cheats[4]); // old displayed name must win over new array index
 menu.change({menu.screen(),"cheat0","on","off"});assert(fake_cheats[4].on&&!fake_cheats[0].on&&!fake_cheats[1].on);
 assert(!strcmp(control("cheat0")->label.get(),"Fourth"));
 cheat_result=TSUCHI_CAPACITY;ui::table.set_choice(framework,menu.screen(),"cheat0","on");menu.change({menu.screen(),"cheat0","on","off"});
 assert(!fake_cheats[0].on&&control("cheat0")->selected==0&&strstr(control("status")->text.get(),"1,024"));
 cheat_result=TSUCHI_IO;menu.change({menu.screen(),"cheat0","on","off"});assert(fake_cheats[0].on&&control("cheat0")->selected==1&&strstr(control("status")->text.get(),"session"));
 const auto before_fault=menu.screen();fake_cheats[0].on=false;fake_cheats[0].problem="it ran too long without finishing";menu.refresh();
 assert(menu.screen().id!=before_fault.id&&control("cheat0")->kind==UiKind::text&&strstr(control("cheat0")->text.get(),"too long"));
 const auto calls=cheat_calls;menu.change({menu.screen(),"cheat0","on","off"});assert(cheat_calls==calls);
 fake_cheats={{"example","Code patch","it changes the game's code, and this install has no code access",false}};menu.refresh();
 assert(control("done")&&control("cheat0")->kind==UiKind::text);
 fake_cheats.clear();menu.refresh();assert(control("empty")&&!control("cheat0"));
 menu.change({before_fault,"cheat0","on","off"});assert(cheat_calls==calls);
 menu.activate({menu.screen(),"back"});assert(!menu.screen().id&&!ui_active_screen());cheat_result=TSUCHI_OK;
}
}
