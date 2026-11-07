// The SD card, mounted once as `tsuchisd:` for the log's file and for the mods' development folders.
//
// Game thread only.
#pragma once

namespace tsuchi {

// What the file functions call the card.
inline constexpr const char* kSdMount = "tsuchisd";

// Mounts the card the first time it is asked, and says whether it is mounted. False when the system's functions
// for it are missing or the mount was refused; it is not tried again then.
bool sd_card_mounted();

// From sd_enter until sd_leave is given what it gave, a missing path or a refused permission on the card comes back
// as an error instead of stopping the game. Only after sd_card_mounted() gave true. Every file opened in between is
// closed before sd_leave.
const void* sd_enter();
void sd_leave(const void* before);
// The same for as long as one lives.
struct SdScope {
    const void* before;
    SdScope() : before(sd_enter()) {}
    ~SdScope() { sd_leave(before); }
    SdScope(const SdScope&) = delete;
    SdScope& operator=(const SdScope&) = delete;
};

// Gives the mods their development folders, `tsuchisd:/tsuchi/dev/<id>/`, and holds restarts while the game
// changes scenes or saves. Called once when the framework starts. See DESIGN.md, "Restarting a mod".
void dev_folders_start();

} // namespace tsuchi
