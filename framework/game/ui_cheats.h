// Framework-owned cheat controls. Runtime entry texts never survive a frame here.
#pragma once
#include "tsuchi/ui.h"
#include "cheat_runtime.h"
#include "cheat_vm.h"
namespace tsuchi {
bool mod_has_cheats(const char* mod_id);
class CheatMenu {
public:
    TSUCHI_Screen screen() const { return screen_; }
    TSUCHI_Result open(TSUCHI_Mod* owner,const char* mod_id);
    void close();
    void refresh();
    void activate(const TSUCHI_UiActivated&);
    void change(const TSUCHI_UiChoiceChanged&);
private:
    static constexpr uint32_t page_size=3;
    struct Row {char name[cheat::kNameBytes]{},problem[kCheatProblemBytes]{};bool on=false;};
    struct Snapshot {Row rows[page_size];uint32_t count=0,total=0,page=0;};
    TSUCHI_Mod* owner_=nullptr;
    TSUCHI_Screen screen_{};
    char mod_id_[48]{},message_[200]{};
    Snapshot shown_{};
    Snapshot collect(uint32_t page) const;
    TSUCHI_Result rebuild(const Snapshot&,const char* focus=nullptr);
};
}
