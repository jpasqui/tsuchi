// The game build's own pieces: finding the game in memory and asking the
// system what is mapped. It also defines the three functions game.h leaves open.
#pragma once
#include <stdint.h>

namespace tsuchi::target {

// Looks through memory for a build of the game the table describes, and selects it. False
// when there is none.
bool find_game(uint64_t own_base);
// Tells the player, when find_game found none, that the framework cannot run on this build.
void unknown_build_notice(uint64_t own_base);
// What the system says of the region of memory an address is in: where it starts, its size, and
// its permissions (1 read, 2 write, 4 execute). False when the system will not say.
bool query_region(uint64_t address, uint64_t& base, uint64_t& size, uint32_t& permission);
// Forgets what was learned about mapped memory, and takes the calling thread as the one whose answers are
// kept from here on. Called at start-up and at the start of every frame.
void new_frame();
// A number for the thread this is called on: where the system keeps what is that thread's own. Never 0, and
// no two threads have the same.
uint64_t current_thread();
// True when the address is in mapped code.
bool executable(uint64_t address);
// Writes text to the system's debug output. Nothing happens where nothing listens to it.
void debug_output(const char* text, uint64_t length);
// Installs lazy writable aliases for owned native hooks and code patches.
void code_memory_start();
// Where this process's heap region, alias region and ASLR region start, as the system says. False when it won't.
bool process_regions(uint64_t heap_alias_aslr[3]);

} // namespace tsuchi::target
