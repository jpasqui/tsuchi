// Cheats while the game runs: Atmosphère-format cheat files that come with mods, run by the framework's own
// interpreter (cheat_vm.h), with each named cheat turned on and off while the game runs (DESIGN.md, "Cheats while
// the game runs").
//
// A mod brings a cheat file as `cheats/<build>.txt` in its folder, where <build> is the first 16 hexadecimal digits
// of the running game's build ID, as Atmosphère names its files. Every named cheat starts off. The player's choices
// are kept in the save and come back at the next start.
//
// Each engine frame runs the master block and the cheats that are on, in the file's order, once. A write to memory
// the game can write goes there directly. A write to the game's code needs code access: it becomes an instruction
// patch the cheat owns, and turning the cheat off takes the patch away, which gives the game its own instruction
// back. Other writes stay as they are when a cheat is turned off: a value the cheat kept frozen is free again, but
// what it already gave stays.
#pragma once
#include <stdint.h>

#include "tsuchi/core.h"

namespace tsuchi {

constexpr uint32_t kCheatSets = 64;          // mods with a cheat file, at once
constexpr uint32_t kCheatPatches = 256;      // instruction patches the cheats of one mod hold at once
constexpr uint32_t kCheatProblemBytes = 96;

// What the runtime needs of the game. The game build fills it in from the framework; a test gives its own.
struct CheatHost {
    // The mods that are running, by index: the ID and context of each. False past the last.
    bool (*mod_at)(uint32_t index, const char*& id, TSUCHI_Mod*& context) = nullptr;
    // One of a mod's files, whole. The caller releases `data` with tsuchi::release.
    TSUCHI_Result (*read_file)(const char* mod_id, const char* path, char*& data, uint64_t& size) = nullptr;
    // The saved choices, and saving them. TSUCHI_NOT_READY while the save can't be used yet; a missing file is
    // TSUCHI_NOT_FOUND.
    TSUCHI_Result (*load_choices)(char*& data, uint64_t& size) = nullptr;
    TSUCHI_Result (*save_choices)(const char* data, uint64_t size) = nullptr;
    // Where the main code, the heap, the alias region and the ASLR region start, in that order. False while the
    // system won't say.
    bool (*regions)(uint64_t base[4]) = nullptr;
    // The buttons held on every controller together, in the system's bit order.
    uint64_t (*keys)() = nullptr;
    bool (*readable)(uint64_t address, uint64_t bytes) = nullptr;
    bool (*writable)(uint64_t address, uint64_t bytes) = nullptr;
    // The game's main code: where it starts and how long it is. A write there becomes an instruction patch.
    uint64_t code_start = 0, code_bytes = 0;
    // The file name's build part: 16 hexadecimal digits.
    const char* build = nullptr;
    // A line in the log, as a mod's.
    void (*note)(const char* mod_id, const char* text) = nullptr;
    // Microseconds, for running cheats twelve times a second as Atmosphère does. Without it, every frame.
    uint64_t (*now_us)() = nullptr;
};
void set_cheat_host(const CheatHost& host);
// Finds new cheat files, forgets the cheats of mods that stopped, and runs the cheats that are on. The engine frame
// calls this at its start; a test calls it itself.
void cheats_frame();
// Connects cheats_frame to the engine frame. Called once at start-up, after set_cheat_host.
void cheats_start();

// One named cheat, for the mod list.
struct CheatEntry {
    const char* mod_id;
    const char* name;
    bool on;
    const char* problem; // why it can't run, or ""
};
uint32_t cheat_count();
// False past the last. The texts last until the next frame.
bool cheat_at(uint32_t index, CheatEntry& out);
// Turns a cheat on or off, and saves the choice. TSUCHI_NOT_FOUND for a name the mod's file doesn't have,
// TSUCHI_UNSUPPORTED for a cheat whose words can't be read or whose writes need code access this install doesn't
// have, and TSUCHI_CAPACITY when it would take the master and the cheats that are on past 1,024 words. Game
// thread only. The cheat runs from the next frame; turning it off takes its instruction patches away at once.
TSUCHI_Result set_cheat(const char* mod_id, const char* name, bool on);

// For a test's log: frames that ran cheats, faults the interpreter met, and instruction patches held.
struct CheatStats {
    uint32_t runs, faults, patches;
};
CheatStats cheat_stats();
// Forgets every cheat file without saving, for a test.
void cheats_reset();

} // namespace tsuchi
