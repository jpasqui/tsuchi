// The memory service against the made-up game: blocks, their owners and limits, and checked reads and writes.
#include <cstdio>
#include <cstring>
#include <string>

#include "fake_engine.h"
#include "memory_backend.h"
#include "memory_service.h"
#include "mod_service.h"
#include "mods.h"
#include "registry.h"

namespace {

int failures = 0;
#define CHECK(condition) \
    do { if (!(condition)) { std::printf("%s:%d: %s\n", __FILE__, __LINE__, #condition); ++failures; } } while (0)

const TSUCHI_MemoryService& api = tsuchi::memory::table;

uint64_t address_of(TSUCHI_Mod* mod, TSUCHI_MemoryBlock block) {
    uint64_t at = 0;
    return api.address(mod, block, &at) == TSUCHI_OK ? at : 0;
}

std::string read(TSUCHI_Mod* mod, uint64_t address, uint64_t bytes, TSUCHI_Result* result = nullptr) {
    std::string out(size_t(bytes), '\0');
    uint64_t got = 0;
    const TSUCHI_Result r = api.read(mod, address, out.data(), bytes, &got);
    if (result) *result = r;
    return r == TSUCHI_OK && got == bytes ? out : "<failed>";
}

} // namespace

int main() {
    fake::start();
    fake::start_engine();
    tsuchi::memory_start();
    TSUCHI_Mod* mod = tsuchi::mod_create("memory.test");
    TSUCHI_Mod* other = tsuchi::mod_create("memory.other");

    uint64_t main = 0;
    CHECK(api.main(mod, &main) == TSUCHI_OK && main == fake::main_image());

    // The running build, by the name a manifest's `game` list uses, so a mod can pick its own offsets.
    tsuchi::mods_start();
    char build[32] = {};
    uint64_t needed = 0;
    CHECK(tsuchi::mod::table.game_build(mod, build, sizeof build, &needed) == TSUCHI_OK &&
          !std::strcmp(build, tsuchi::game::table::build_key) && build[0] && needed == std::strlen(build) + 1);

    // A block starts zeroed, though the game's allocator doesn't clear it.
    TSUCHI_MemoryBlock block{};
    CHECK(api.allocate(mod, 64, 16, false, &block) == TSUCHI_OK && block.id);
    const uint64_t at = address_of(mod, block);
    CHECK(at && read(mod, at, 64) == std::string(64, '\0'));
    CHECK(api.write(mod, at + 8, "pikmin", 6) == TSUCHI_OK && read(mod, at + 8, 6) == "pikmin");
    CHECK(api.write(mod, at, "", 0) == TSUCHI_OK);

    // Sizes and alignments outside their ranges.
    TSUCHI_MemoryBlock refused{};
    CHECK(api.allocate(mod, 0, 0, false, &refused) == TSUCHI_INVALID);
    CHECK(api.allocate(mod, (64ull << 20) + 1, 0, false, &refused) == TSUCHI_INVALID);
    CHECK(api.allocate(mod, 8, 3, false, &refused) == TSUCHI_INVALID);
    CHECK(api.allocate(mod, 8, 8192, false, &refused) == TSUCHI_INVALID);

    // Reads and writes are checked against what is mapped, and the game's code isn't written here.
    TSUCHI_Result result = TSUCHI_OK;
    CHECK(read(mod, 0x10, 4, &result) == "<failed>" && result == TSUCHI_NOT_FOUND);
    CHECK(read(mod, ~uint64_t(0) - 3, 8, &result) == "<failed>" && result == TSUCHI_INVALID);
    CHECK(read(mod, at, (1ull << 20) + 1, &result) == "<failed>" && result == TSUCHI_INVALID);
    CHECK(read(mod, at + 60, 8, &result) == "<failed>" && result == TSUCHI_NOT_FOUND); // runs past the block
    CHECK(api.write(mod, main, "\0\0\0\0", 4) == TSUCHI_INVALID);
    const uint64_t locked = fake::memory(16, false);
    CHECK(api.write(mod, locked, "x", 1) == TSUCHI_NOT_FOUND && read(mod, locked, 1) == std::string(1, '\0'));

    // Only the owner frees a block, once.
    CHECK(api.free(other, block) == TSUCHI_NOT_FOUND && address_of(mod, block) == at);
    const int freed = fake::engine().freed;
    CHECK(api.free(mod, block) == TSUCHI_OK && fake::engine().freed == freed + 1);
    CHECK(api.free(mod, block) == TSUCHI_NOT_FOUND && address_of(mod, block) == 0);

    // A mod holds 256 blocks at most.
    TSUCHI_MemoryBlock many[256] = {};
    for (TSUCHI_MemoryBlock& each : many) CHECK(api.allocate(mod, 8, 0, false, &each) == TSUCHI_OK);
    CHECK(api.allocate(mod, 8, 0, false, &refused) == TSUCHI_CAPACITY);
    CHECK(api.allocate(other, 8, 0, false, &refused) == TSUCHI_OK); // another mod has its own
    for (TSUCHI_MemoryBlock& each : many) CHECK(api.free(mod, each) == TSUCHI_OK);

    // Inside a hook, reads and writes work and reservations are refused: they would reach the engine's allocator.
    TSUCHI_MemoryBlock kept{};
    CHECK(api.allocate(mod, 32, 0, false, &kept) == TSUCHI_OK);
    tsuchi::hook_client_enter();
    CHECK(api.write(mod, address_of(mod, kept), "hook", 4) == TSUCHI_OK && read(mod, address_of(mod, kept), 4) == "hook");
    CHECK(api.allocate(mod, 8, 0, false, &refused) == TSUCHI_INVALID && api.free(mod, kept) == TSUCHI_INVALID);
    tsuchi::hook_client_leave();

    // A stopping mod's blocks are freed, except lasting ones, which the game may still use.
    TSUCHI_MemoryBlock lasting{};
    CHECK(api.allocate(mod, 40ull << 20, 0, true, &lasting) == TSUCHI_OK);
    const uint64_t lasting_at = address_of(mod, lasting);
    CHECK(api.write(mod, lasting_at, "stay", 4) == TSUCHI_OK);
    const int before_stop = fake::engine().freed;
    tsuchi::mod_stop(mod);
    CHECK(fake::engine().freed == before_stop + 1); // only `kept`
    CHECK(address_of(other, lasting) == 0 && read(other, lasting_at, 4) == "stay");

    // Lasting bytes count for the mod's ID over the whole run, so a restart doesn't reset them.
    TSUCHI_Mod* again = tsuchi::mod_create("memory.test");
    CHECK(api.allocate(again, 30ull << 20, 0, true, &refused) == TSUCHI_CAPACITY);
    CHECK(api.allocate(again, 20ull << 20, 0, true, &refused) == TSUCHI_OK);
    CHECK(api.allocate(again, 30ull << 20, 0, false, &refused) == TSUCHI_OK); // ordinary blocks aren't counted

    // Without the engine's allocator nothing is reserved; with no memory left, it says so.
    fake::engine().out_of_memory = true;
    CHECK(api.allocate(again, 8, 0, false, &refused) == TSUCHI_NO_MEMORY);
    fake::engine().out_of_memory = false;

    // A stopped mod can't call at all.
    CHECK(api.main(mod, &main) == TSUCHI_INVALID);
    tsuchi::mod_stop(again);
    tsuchi::mod_stop(other);
    if (failures) std::printf("%d failures\n", failures);
    else std::puts("memory passed");
    return failures;
}
