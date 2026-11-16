// The cheats of one mod on a screen of the framework's own, turned on and off while the game runs.
// Entry texts from the cheat runtime are copied: none is kept past the frame it was read in.
#pragma once
#include "tsuchi/ui.h"
#include "cheat_runtime.h"
#include "cheat_vm.h"
namespace tsuchi {
bool mod_has_cheats(const char* mod_id);
class CheatMenu {
public:
    TSUCHI_Screen screen() const { return screen_; }
    // Whether the screen is shown, or waits to be shown again after a refresh the game refused.
    bool showing() const { return screen_.id != 0; }
    TSUCHI_Result open(TSUCHI_Mod* owner, const char* mod_id);
    void close();
    // Once a frame while open: shows what changed, and shows the screen again if showing it failed.
    void refresh();
    void activate(const TSUCHI_UiActivated&);
    void change(const TSUCHI_UiChoiceChanged&);
    // A refreshed screen that could not be shown is tried again for this many frames before the menu closes.
    static constexpr uint32_t kReopenFrames = 120;
private:
    static constexpr uint32_t page_size = 3;
    struct Row { char name[cheat::kNameBytes]{}, problem[kCheatProblemBytes]{}; bool on = false; };
    struct Snapshot { Row rows[page_size]; uint32_t count = 0, total = 0, page = 0; };
    TSUCHI_Mod* owner_ = nullptr;
    TSUCHI_Screen screen_{};
    char mod_id_[48]{}, message_[200]{};
    Snapshot shown_{};
    uint32_t reopen_frames_ = 0; // frames left to show screen_ again; 0 when it is shown
    Snapshot collect(uint32_t page) const;
    TSUCHI_Result rebuild(const Snapshot&, const char* focus = nullptr);
};
}
