// A second, made-up build of the made-up game: how the framework selects a build, what its numbers
// then are, and what an absent entry does. tests/test_multi_build.py makes the table. Exits with
// the number of failures.
#include <cstdio>
#include <cstring>
#include <vector>

#include "select_build.h"
#include "fake_game.h"
#include "hook_service.h"
#include "hooks.h"
#include "module_symbols.h"
#include "registry.h"
#include "world_service.h"

namespace {

namespace t = tsuchi::game::table;
int failures = 0;
#define CHECK(condition) \
    do { \
        if (!(condition)) { \
            std::printf("%s:%d: %s\n", __FILE__, __LINE__, #condition); \
            ++failures; \
        } \
    } while (0)

// The hash the framework reads a span with, over code that is one byte throughout.
uint64_t span_of(unsigned char byte) {
    uint64_t hash = 0xCBF29CE484222325ull;
    for (unsigned i = 0; i < 16; ++i) hash = (hash ^ byte) * 0x100000001B3ull;
    return hash;
}
int found_in(uint64_t bytes, unsigned char byte) { return t::find_build(bytes, [&](uint64_t) { return span_of(byte); }); }

void selection() {
    CHECK(t::build_count == 2);
    CHECK(found_in(0x4000, 0x11) == 0);
    CHECK(found_in(0x10000000, 0x22) == 1);
    CHECK(found_in(0x10000000, 0x33) == -1);
    CHECK(found_in(0x3FFF, 0x11) == -1); // spans match, and the region does not hold all of that build's code
    CHECK(found_in(0xFFFFFFF, 0x22) == -1);
    CHECK(std::strlen(t::builds[1].build_id) == 32);
}

void values() {
    const uint64_t color = t::game_actor::color;
    t::select_build(1);
    CHECK(std::strcmp(t::version, "9.9") == 0 && std::strcmp(t::build_key, "made-up") == 0);
    CHECK(uint64_t(t::game_actor::color) == color + 4);
    CHECK(uint64_t(t::constant::code_size) == 0x10000000);
    CHECK(uint64_t(t::constant::program_id) == 0x0423456789ABCDEFull);
    CHECK(std::strcmp(t::name::game_container, "StandIn-MainTwo") == 0);
    t::select_build(0);
    CHECK(std::strcmp(t::version, "0.0") == 0 && std::strcmp(t::build_key, "0.0") == 0);
    CHECK(uint64_t(t::game_actor::color) == color);
    CHECK(uint64_t(t::constant::code_size) == 0x4000);
    CHECK(std::strcmp(t::name::game_container, "StandIn-Main") == 0);
    t::select_build(1); // and from the first to the second a second time, as from anywhere
    t::select_build(1);
    CHECK(uint64_t(t::game_actor::color) == color + 4);
}

void hook(void*, uint64_t) {}

// What the framework's own checks see of a function the build does not have, and of a number cut short.
void stand_ins() {
    int (*missing)() = t::absent_function<int>;
    int (*mine)() = [] { return 1; };
    CHECK(!t::bound(missing) && t::bound(mine));
    int (*none)() = nullptr;
    CHECK(!t::bound(none));
    CHECK(uint32_t(t::absent_value) != 0);
}

void absent_entries(TSUCHI_Mod* mod) {
    t::select_build(1);
    // A read through what a build does not have fails.
    CHECK(!t::present(t::data::world));
    CHECK(t::present(t::data::object_chunks));
    CHECK(tsuchi::game::global<uint64_t>(t::data::world, 77) == 77);
    // Calling a function a build does not have does nothing, and returns zero.
    t::bind_functions(tsuchi::game::main_base());
    CHECK(t::fn::spawn_now != nullptr);
    CHECK(t::fn::spawn_now(nullptr, nullptr) == nullptr);
    CHECK(t::fn::kill_actor != nullptr);
    // A site of a build that has none is refused as unsupported, by number and by name, before the patch is asked.
    uint64_t id = 0;
    void* original = nullptr;
    CHECK(tsuchi::hook_code_site(mod, t::code_site::draw_call, reinterpret_cast<void*>(&hook), &id, &original) == TSUCHI_UNSUPPORTED);
    TSUCHI_Hook wrapped{};
    CHECK(tsuchi::hook::table.wrap_site(mod, "draw_call", reinterpret_cast<void*>(&hook), &wrapped) == TSUCHI_UNSUPPORTED);
    CHECK(tsuchi::hook_wrap(mod, t::absent_value + 8, 0, reinterpret_cast<void*>(&hook), 0, &id, &original) == TSUCHI_UNSUPPORTED);
}

// The second build's patch is in pieces, and the two values of its mark are not side by side.
void mark_in_two_places() {
    t::select_build(1);
    CHECK(uint64_t(t::stub::block_hash) != t::stub::block + 8);
    const uint64_t main = fake::main_image();
    fake::put<uint64_t>(main + t::stub::block, t::stub::patch_magic);
    fake::put<uint64_t>(main + t::stub::block_hash, t::stub::patch_hash);
    CHECK(tsuchi::find_patch() == tsuchi::Patch::right);
    fake::put<uint64_t>(main + t::stub::block_hash, t::stub::patch_hash + 1);
    CHECK(tsuchi::find_patch() == tsuchi::Patch::other);
    fake::put<uint64_t>(main + t::stub::block, 0);
    fake::put<uint64_t>(main + t::stub::block_hash, 0);
    tsuchi::find_patch();
}

TSUCHI_Result ground(const TSUCHI_Vec3*, const TSUCHI_Vec3*, TSUCHI_Vec3*) { return TSUCHI_OK; }

void families(TSUCHI_Mod* mod) {
    CHECK(t::family_count == 2 && std::strcmp(t::families[0], "world") == 0);
    CHECK(t::builds[0].absent_families == 0);
    CHECK(t::builds[1].absent_families == 1); // what the entries above take with them, and game_actor's color is still there
    tsuchi::world::Backend backend{};
    backend.ground = &ground;
    tsuchi::world::set_backend(&backend);
    const TSUCHI_Vec3 point{1, 2, 3}, extent{1, 1, 1};
    TSUCHI_Vec3 out{};
    tsuchi::select_build(0);
    CHECK(tsuchi::world::table.ground(mod, &point, &extent, &out) == TSUCHI_OK);
    tsuchi::select_build(1);
    CHECK(tsuchi::families_absent == t::builds[1].absent_families);
    CHECK(tsuchi::family_absent(t::family::world) && !tsuchi::family_absent(t::family::other));
    CHECK(tsuchi::world::table.ground(mod, &point, &extent, &out) == TSUCHI_UNSUPPORTED);
    tsuchi::select_build(0);
    CHECK(tsuchi::families_absent == 0);
    tsuchi::select_build(1);
    tsuchi::families_absent = 0;
}

// A loaded module as the system's loader leaves one, for the notice of a build the table does not describe.
void module_lists() {
    const uint64_t base = fake::memory(0x1000);
    const auto put = [&](uint64_t at, auto value) { std::memcpy(reinterpret_cast<void*>(base + at), &value, sizeof value); };
    put(4, uint32_t(0x100));
    std::memcpy(reinterpret_cast<void*>(base + 0x100), "MOD0", 4);
    put(0x104, int32_t(0x100));
    const uint64_t tags[][2] = {{4, 0x400}, {5, 0x500}, {6, 0x600}, {23, 0x700}, {2, 24}, {0, 0}};
    std::memcpy(reinterpret_cast<void*>(base + 0x200), tags, sizeof tags);
    put(0x404, uint32_t(3));
    std::memcpy(reinterpret_cast<void*>(base + 0x500), "\0defined\0imported\0", 18);
    put(0x618, uint32_t(1)), put(0x618 + 6, uint16_t(1)), put(0x618 + 8, uint64_t(0x234));
    put(0x630, uint32_t(9));
    put(0x700, uint64_t(0x900)), put(0x708, uint64_t(2) << 32 | 0x402);
    tsuchi::game::LoadedModule module;
    CHECK(tsuchi::game::read_module(base, module));
    CHECK(module.symbol_count == 3);
    CHECK(tsuchi::game::module_export(module, "defined") == base + 0x234);
    CHECK(tsuchi::game::module_export(module, "imported") == 0); // it names it, and defines nothing
    CHECK(tsuchi::game::module_import_slot(module, "imported") == base + 0x900);
    CHECK(tsuchi::game::module_import_slot(module, "defined") == 0);
    const uint64_t other = fake::memory(0x100);
    CHECK(!tsuchi::game::read_module(other, module));
}

} // namespace

namespace tsuchi {
uint32_t hook_thunk_count() { return 8; }
uint64_t hook_thunk_address(uint32_t thunk) { return 0x7000000000000000ull + 8ull * thunk; }
} // namespace tsuchi

int main() {
    fake::main_image() = fake::memory(0x6000);
    TSUCHI_Mod* mod = tsuchi::mod_create("check.builds");
    selection();
    values();
    stand_ins();
    absent_entries(mod);
    mark_in_two_places();
    families(mod);
    module_lists();
    if (!failures) std::printf("ok\n");
    return failures;
}
