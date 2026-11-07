#include "actions.h"

#include <string.h>

#include "actors.h"
#include "game.h"
#include "names.h"
#include "objects.h"

namespace tsuchi::game {
namespace {

namespace t = table;

// No actor does this many things one within another: past it, the actions lead back into themselves.
constexpr uint32_t kDeepestAction = 64;

// Calls `visit` with the name of each running action from the top down. False
// for an actor without actions, or whose actions cannot be read through.
template <typename Visit>
bool each_running(uint64_t actor, Visit visit) {
    const uint64_t brain = ai_component(actor);
    const uint64_t tree = brain ? read_or<uint64_t>(brain + t::ai_component::actions) : 0;
    if (!tree) return false;
    uint64_t action = read_or<uint64_t>(tree + t::action::child);
    for (uint32_t depth = 0; action; ++depth) {
        Name name;
        if (depth == kDeepestAction || !read(action + t::action::name, name)) return false;
        visit(name);
        action = read_or<uint64_t>(action + t::action::child);
    }
    return true;
}

} // namespace

bool action_signature(uint64_t actor, uint64_t& signature) {
    uint64_t hash = 0xCBF29CE484222325ull;
    const auto mix = [&](uint64_t value) {
        for (int i = 0; i < 8; ++i) hash = (hash ^ ((value >> (8 * i)) & 0xFF)) * 0x100000001B3ull;
    };
    if (!each_running(actor, [&](Name name) { mix(uint64_t(name.index) | uint64_t(name.number) << 32); })) return false;
    signature = hash;
    return true;
}

TSUCHI_Result action_path(uint64_t actor, char* buffer, uint64_t capacity, uint64_t* needed) {
    // Measured first, so a buffer that is too small is left as it was.
    uint64_t bytes = 1;
    bool named = true;
    const auto measure = [&](Name name) {
        char text[kNameBytes];
        if (!name_text(name, text, sizeof text)) return void(named = false);
        bytes += (bytes > 1 ? 1 : 0) + strlen(text); // a '/' before every name but the first
    };
    if (!each_running(actor, measure) || !named) return TSUCHI_UNSUPPORTED;
    *needed = bytes;
    if (!buffer || bytes > capacity) return TSUCHI_CAPACITY;
    uint64_t at = 0;
    each_running(actor, [&](Name name) {
        if (at) buffer[at++] = '/';
        if (name_text(name, buffer + at, size_t(capacity - at))) at += strlen(buffer + at);
    });
    buffer[at] = 0;
    return TSUCHI_OK;
}

} // namespace tsuchi::game
