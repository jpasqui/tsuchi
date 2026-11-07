#include "flow.h"

#include <string.h>

#include "actors.h"
#include "game.h"
#include "objects.h"

namespace tsuchi::game {
namespace {

namespace t = table;

// The game is never in this many stages one within another: past it, they lead back into themselves.
constexpr uint32_t kDeepestStage = 64;

// Calls `visit` with the name of each stage from the outermost in. False when the stages cannot be read
// through. Before the game has set itself up it is in no stages, which is not a failure.
template <typename Visit>
bool each_stage(Visit visit) {
    const uint64_t context = game_context();
    const uint64_t root = context ? read_or<uint64_t>(context + t::game_context::flow) : 0;
    if (!root) return true;
    uint64_t stage = read_or<uint64_t>(root + t::flow_stage::child);
    for (uint32_t depth = 0; stage; ++depth) {
        Name name;
        if (depth == kDeepestStage || !readable(stage, t::flow_stage::size) || !read(stage + t::flow_stage::name, name)) return false;
        visit(name);
        stage = read_or<uint64_t>(stage + t::flow_stage::child);
    }
    return true;
}

} // namespace

uint64_t game_context() { return global<uint64_t>(t::data::game_context); }

bool game_rule(int32_t& rule) {
    const uint64_t context = game_context();
    uint8_t number = 0;
    if (!read(context + t::game_context::rule, number)) return false; // with no context there is nothing there to read
    rule = number;
    return true;
}

bool top_stage(Name& name) {
    bool found = false;
    return each_stage([&](Name each) {
        if (!found) name = each;
        found = true;
    }) && found;
}

bool leaf_stage(Name& name) {
    bool found = false;
    return each_stage([&](Name each) { name = each, found = true; }) && found;
}

bool at_title() {
    Name top;
    char text[kNameBytes];
    return top_stage(top) && name_text(top, text, sizeof text) && !strcmp(text, kTitleStage);
}

bool stage_signature(uint64_t& signature) {
    uint64_t hash = 0xCBF29CE484222325ull;
    const auto mix = [&](uint64_t value) {
        for (int i = 0; i < 8; ++i) hash = (hash ^ ((value >> (8 * i)) & 0xFF)) * 0x100000001B3ull;
    };
    if (!each_stage([&](Name name) { mix(uint64_t(name.index) | uint64_t(name.number) << 32); })) return false;
    signature = hash;
    return true;
}

TSUCHI_Result stage_path(char* buffer, uint64_t capacity, uint64_t* needed) {
    // Measured first, so a buffer that is too small is left as it was. Before the game has stages, it is in none.
    uint64_t bytes = 1;
    bool named = true;
    const bool read_through = each_stage([&](Name name) {
        char text[kNameBytes];
        if (!name_text(name, text, sizeof text)) return void(named = false);
        bytes += (bytes > 1 ? 1 : 0) + strlen(text);
    });
    if (!named || !read_through) return TSUCHI_NOT_READY;
    *needed = bytes;
    if (!buffer || bytes > capacity) return TSUCHI_CAPACITY;
    uint64_t at = 0;
    each_stage([&](Name name) {
        if (at) buffer[at++] = '/';
        if (name_text(name, buffer + at, size_t(capacity - at))) at += strlen(buffer + at);
    });
    buffer[at] = 0;
    return TSUCHI_OK;
}

bool world_begun() {
    const uint64_t framework = game_framework();
    return framework && read_or<uint8_t>(framework + t::game_framework::world_begun) != 0;
}

const char* world_not_begun() {
    if (world_begun()) return nullptr;
    if (!global<uint64_t>(t::data::game_framework)) return "no game framework exists yet";
    if (!game_framework()) return "the game framework cannot be read through this build's numbers";
    return "no world has begun";
}

uint64_t loaded_world() { return world_begun() ? global<uint64_t>(t::data::world) : 0; }

bool world_name(Name& name) {
    const uint64_t world = global<uint64_t>(t::data::world);
    return world_begun() && world && read(world + t::world::level_name, name);
}

uint64_t pause_manager() {
    const uint64_t instance = global<uint64_t>(t::data::game_instance);
    const uint64_t manager = instance ? read_or<uint64_t>(instance + t::game_instance::pause_manager) : 0;
    // Only the manager this knows: its function table says what it is.
    return manager && readable(manager, t::pause_manager::size) && read_or<uint64_t>(manager) == main_base() + t::data::pause_manager_table ? manager : 0;
}

bool paused_kinds(uint32_t& kinds) {
    const uint64_t manager = pause_manager();
    return manager && read(manager + t::pause_manager::active, kinds);
}

bool change_pause(uint32_t kinds, bool add) {
    const uint64_t manager = pause_manager();
    if (!manager) return false;
    t::fn::pause_change(reinterpret_cast<void*>(manager), kinds, add);
    return true;
}

bool busy_with_saves_or_scenes() {
    if (global<uint8_t>(t::data::save_async_state) || global<uint8_t>(t::data::save_mounted) || global<uint8_t>(t::data::engine_save_mounted)) return true;
    const uint64_t instance = global<uint64_t>(t::data::game_instance);
    const uint64_t scenes = instance ? read_or<uint64_t>(instance + t::game_instance::scene_manager) : 0;
    return scenes && read_or<uint64_t>(scenes + t::scene_manager::changing) != 0;
}

} // namespace tsuchi::game
