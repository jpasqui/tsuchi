// The memory service: blocks a mod reserves from the engine's allocator, and reads and writes at an address.
//
// Every access is checked against what the system says is mapped right now, so a wrong address fails instead of
// stopping the game. Nothing checks what the bytes mean. A block is owned by its mod and freed when the mod stops,
// unless it is lasting: the game may keep a pointer to a lasting block, so it stays until the game closes, and the
// bytes each mod has kept that way are counted for the whole run.

#include <string.h>

#include "game.h"
#include "names.h"
#include "memory_service.h"
#include "reflect.h"
#include "registry.h"
#include "memory_backend.h"

namespace tsuchi {

namespace {

namespace t = game::table;

constexpr uint64_t kMostBlock = 64ull << 20;   // one block
constexpr uint64_t kMostHeld = 64ull << 20;    // a mod's blocks at one time
constexpr uint64_t kMostLasting = 64ull << 20; // a mod's lasting blocks over the whole run
constexpr uint32_t kMostBlocks = 256;          // a mod's blocks at one time
constexpr uint64_t kMostAccess = 1ull << 20;   // one read or write
constexpr uint32_t kRecords = 4096;            // blocks held by every mod together
constexpr uint32_t kLastingMods = 256;

const Kind kBlock{"memory block"};

struct Block {
    uint64_t id;
    TSUCHI_Mod* mod;
    uint64_t address;
    uint64_t bytes;
    bool lasting;
};
Block blocks[kRecords] = {};
uint64_t next_id = 1;

// The lasting bytes each mod has kept, by its ID, as a mod that restarts gets a new context.
struct Kept {
    char id[kModIdBytes];
    uint64_t bytes;
};
Kept kept[kLastingMods] = {};

Block* find(uint64_t id) {
    for (Block& block : blocks)
        if (block.id && block.id == id) return &block;
    return nullptr;
}

Kept* kept_for(const TSUCHI_Mod* mod, bool add) {
    const char* id = mod_id(mod);
    for (Kept& entry : kept)
        if (entry.id[0] && !strcmp(entry.id, id)) return &entry;
    if (!add) return nullptr;
    for (Kept& entry : kept)
        if (!entry.id[0]) {
            memcpy(entry.id, id, strlen(id) + 1);
            return &entry;
        }
    return nullptr;
}

// The mod stopped while it held the block. Ordinary memory goes back to the engine; a lasting block stays where the
// game may still use it, and only its record goes.
void stopped(void*, uint64_t id) {
    Block* block = find(id);
    if (!block) return;
    if (!block->lasting) game::release_engine_memory(reinterpret_cast<void*>(block->address));
    *block = Block{};
}

bool power_of_two(uint32_t value) { return value && !(value & (value - 1)); }

// The game's code, which only hook.patch_instruction may change.
bool touches_code(uint64_t address, uint64_t bytes) {
    const uint64_t main = game::main_base();
    return main && address < main + t::constant::code_size && address + bytes > main;
}

TSUCHI_Result main(uint64_t* out) {
    *out = game::main_base();
    return *out ? TSUCHI_OK : TSUCHI_NOT_READY;
}

TSUCHI_Result allocate(TSUCHI_Mod* mod, uint64_t bytes, uint32_t alignment, bool lasting, TSUCHI_MemoryBlock* out) {
    if (!bytes || bytes > kMostBlock || (alignment && (!power_of_two(alignment) || alignment > 4096))) return TSUCHI_INVALID;
    uint32_t count = 0;
    uint64_t held = 0;
    Block* free_record = nullptr;
    for (Block& block : blocks) {
        if (!block.id) {
            if (!free_record) free_record = &block;
        } else if (block.mod == mod) {
            ++count;
            held += block.bytes;
        }
    }
    if (count >= kMostBlocks || held > kMostHeld - bytes || !free_record) return TSUCHI_CAPACITY;
    Kept* account = lasting ? kept_for(mod, true) : nullptr;
    if (lasting && (!account || account->bytes > kMostLasting - bytes)) return TSUCHI_CAPACITY;
    if (!game::engine_memory_available()) return TSUCHI_NOT_READY;
    void* memory = game::engine_memory_aligned(bytes, alignment);
    if (!memory) return TSUCHI_NO_MEMORY;
    memset(memory, 0, size_t(bytes));
    const uint64_t id = next_id++;
    const TSUCHI_Result owned = own(mod, kBlock, id, stopped, nullptr);
    if (owned != TSUCHI_OK) {
        game::release_engine_memory(memory);
        return owned;
    }
    *free_record = Block{id, mod, reinterpret_cast<uint64_t>(memory), bytes, lasting};
    if (account) account->bytes += bytes;
    out->id = id;
    return TSUCHI_OK;
}

TSUCHI_Result address(TSUCHI_MemoryBlock handle, uint64_t* out) {
    const Block* block = find(handle.id);
    if (!block) return TSUCHI_NOT_FOUND;
    *out = block->address;
    return TSUCHI_OK;
}

TSUCHI_Result release(TSUCHI_Mod* mod, TSUCHI_MemoryBlock handle) {
    Block* block = find(handle.id);
    if (!block || block->mod != mod) return TSUCHI_NOT_FOUND;
    disown(mod, kBlock, block->id);
    game::release_engine_memory(reinterpret_cast<void*>(block->address));
    if (block->lasting)
        if (Kept* account = kept_for(mod, false)) account->bytes -= block->bytes < account->bytes ? block->bytes : account->bytes;
    *block = Block{};
    return TSUCHI_OK;
}

TSUCHI_Result read(uint64_t address, void* buffer, uint64_t capacity, uint64_t* out) {
    *out = 0;
    if (capacity > kMostAccess || address > ~uint64_t(0) - capacity) return TSUCHI_INVALID;
    if (!capacity) return TSUCHI_OK;
    if (!game::readable(address, capacity)) return TSUCHI_NOT_FOUND;
    memcpy(buffer, reinterpret_cast<const void*>(address), size_t(capacity));
    *out = capacity;
    return TSUCHI_OK;
}

// A name the game keeps at an address. Its index is checked by the name table itself: one that names nothing,
// or memory that isn't a name at all, is not found.
TSUCHI_Result read_name(uint64_t address, char* buffer, uint64_t capacity, uint64_t* out) {
    *out = 0;
    if (address > ~uint64_t(0) - sizeof(game::Name) || !game::readable(address, sizeof(game::Name))) return TSUCHI_NOT_FOUND;
    game::Name name{};
    memcpy(&name, reinterpret_cast<const void*>(address), sizeof name);
    return game::name_into(name, buffer, capacity, out);
}

TSUCHI_Result write(uint64_t address, const void* data, uint64_t bytes) {
    if (bytes > kMostAccess || address > ~uint64_t(0) - bytes || touches_code(address, bytes)) return TSUCHI_INVALID;
    if (!bytes) return TSUCHI_OK;
    if (!game::writable(address, bytes)) return TSUCHI_NOT_FOUND;
    memcpy(reinterpret_cast<void*>(address), data, size_t(bytes));
    return TSUCHI_OK;
}

const memory::Backend service{main, allocate, address, release, read, read_name, write};

} // namespace

void memory_start() { memory::set_backend(&service); }

} // namespace tsuchi
