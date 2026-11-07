// The game's side of restarting a mod: the development folders on the SD card, and when restarts may run.
// See DESIGN.md, "Restarting a mod".

#include "files.h"
#include "flow.h"
#include "frame.h"
#include "mods.h"
#include "sd_card.h"

namespace tsuchi {

namespace {

constexpr char kDevFolders[] = "tsuchisd:/tsuchi/dev";
// About once a second at 30 steps a second, and twice a second at 60.
constexpr uint32_t kWatchSteps = 30;

// Development files are read one call after another, never inside each other, so one context is kept at a time.
const void* before = nullptr;
bool entered = false;

bool begin() {
    if (entered || !game::files_known() || !sd_card_mounted()) return false;
    before = sd_enter(), entered = true;
    return true;
}

void end() {
    if (!entered) return;
    sd_leave(before), entered = false;
}

// Mods stop and start again only where the game would start them.
bool ready() { return game::world_begun() && !game::busy_with_saves_or_scenes(); }

} // namespace

void dev_folders_start() {
    set_dev_folder(kDevFolders, begin, end);
    set_restart_gate(ready);
    set_dev_watch(kWatchSteps);
    after_step(watch_dev_folders);
    after_step(run_restarts);
}

} // namespace tsuchi
