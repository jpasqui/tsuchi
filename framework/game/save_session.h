// The player's save, opened the way the game opens it for as long as a session
// lives, and closed its way. It can be open once at a time, so whoever makes a
// session first asks whether the game is busy with a save itself.
#pragma once
#include "files.h"
#include "flow.h"
#include "game.h"

namespace tsuchi::game {

// Where mods keep what is their own in the save, beside the slots' folders.
inline constexpr const char* kSavedMods = "save:/tsuchi";

// True when the game's functions for opening and closing its save are known, and the file functions with them.
inline bool save_session_known() { return table::fn::save_begin_session && table::fn::save_end_session && files_known(); }

struct SaveSession {
    bool open;
    SaveSession() : open(table::fn::save_begin_session(0, false, false)) {}
    ~SaveSession() {
        if (open) table::fn::save_end_session();
    }
    SaveSession(const SaveSession&) = delete;
    SaveSession& operator=(const SaveSession&) = delete;
};

} // namespace tsuchi::game
