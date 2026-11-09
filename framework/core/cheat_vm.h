// Atmosphère-format cheats, read from their text and run by the framework itself, so that each named cheat can be
// turned on and off while the game runs (DESIGN.md, "Cheats while the game runs").
//
// A cheat file is blocks of 32-bit words in hexadecimal. Each block is under a name in [brackets]; one optional
// master block, in {braces}, comes first and runs before the others whenever any of them is on. The words are
// instructions for Atmosphère's cheat interpreter: memory writes, pointer reads, conditions, loops, register
// arithmetic, and button checks. This file reads the text with the same rules as tools/external_cheats.py, and
// runs the words. It knows nothing of the game: memory and buttons come in through Memory.
#pragma once
#include <stddef.h>
#include <stdint.h>

#include "tsuchi/core.h"

namespace tsuchi::cheat {

constexpr uint64_t kFileBytes = 64 * 1024; // a file is smaller than this
constexpr uint32_t kCheats = 127;          // named cheats in one file, besides the master
constexpr uint32_t kWords = 256;           // words in one block
constexpr uint32_t kProgramWords = 1024;   // words of the master and the cheats that are on, together
constexpr uint32_t kNameBytes = 64;        // a name's UTF-8 bytes and its terminator
constexpr uint32_t kRegisters = 16;
constexpr uint32_t kStaticRegisters = 256; // 0x00 to 0x7F are read by codes, 0x80 to 0xFF written by them
// Instructions one run may carry out, counting every pass through a loop. A run that reaches it stops there.
constexpr uint32_t kStepLimit = 1u << 20;
// The owner of a word of the master block.
constexpr uint16_t kMaster = 0xFFFF;

struct Block {
    char name[kNameBytes];
    uint32_t words[kWords];
    uint32_t count;
};

struct File {
    bool has_master;
    Block master;
    Block cheats[kCheats];
    uint32_t count; // named cheats, in the file's order
};

// Reads a file's text. TSUCHI_INVALID with `problem` saying why for text that breaks a rule: a size of 64 KiB or
// more, text that isn't UTF-8 or holds a control character or a byte order mark, a header that isn't alone on its
// line, an empty or duplicated name, a name over 63 bytes or with a bracket, brace or control character in it, a
// word that isn't eight hexadecimal digits, words before the first header, a block with no words or over 256, a
// second master, more than 127 named blocks, or no named block at all. After a failure `out` holds part of the
// file and isn't to be used.
TSUCHI_Result parse(const char* text, uint64_t size, File& out, char* problem, size_t capacity);

// Where a code's address is counted from.
enum class Region : uint8_t { main, heap, alias, aslr };

// The game, as a run sees it. `read` and `write` give false for memory that can't be read or written; a run counts
// those and goes on. `write` also hears which block the instruction came from: an index into File::cheats, or
// kMaster. `keys` are the buttons held now, in the system's bit order, and `previous_keys` those held at the run
// before.
struct Memory {
    uint64_t base[4]; // by Region
    void* user;
    bool (*read)(void* user, uint64_t address, void* out, uint32_t bytes);
    bool (*write)(void* user, uint64_t address, const void* value, uint32_t bytes, uint16_t owner);
    uint64_t keys, previous_keys;
};

// What lasts from one run to the next: the static registers. Everything else starts at zero for each run, as in
// Atmosphère.
struct State {
    uint64_t statics[kStaticRegisters];
};

struct Report {
    uint32_t steps;       // instructions carried out
    uint32_t faults;      // reads and writes `Memory` refused
    bool stopped;         // an instruction couldn't be read; the run stopped at it
    bool step_limit;      // the run reached kStepLimit
    uint32_t stopped_at;  // the word the run stopped at
    uint16_t stopped_owner;
};

// Runs a program once: the words of the master and of each cheat that is on, in the file's order. `owners` says
// for each word which block it came from.
Report run(const uint32_t* words, const uint16_t* owners, uint32_t count, const Memory& memory, State& state);

// Checks that every instruction of a block can be read, without running it. False with the word it stopped at.
bool decodable(const uint32_t* words, uint32_t count, uint32_t& stopped_at);

} // namespace tsuchi::cheat
