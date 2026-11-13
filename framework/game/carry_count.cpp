// A carry counter's number held by a mod. The game's counter manager (UInstancedCarryNumMgrComponent, at the game
// framework) updates every registered counter each frame, whatever its object is doing: the counter's update asks the
// carry component for its numbers (0xD4BA10), which takes the first from the component's shown count when that is 0 or
// more, and otherwise from the carriers' strength. Only the component's constructor writes the shown count, as -1. The
// counter shows while its number is above 0 and moves to a new one as it does for the game's own numbers.
#include "actors.h"
#include "carry_count.h"
#include "families.h"
#include "game.h"
#include "holds.h"
#include "objects.h"

namespace tsuchi {
namespace {
namespace t = game::table;
game::ClassRef carry_class{t::name::carry_component_class};

uint64_t counter_part(uint64_t actor) {
    const uint64_t part = actor ? game::read_or<uint64_t>(actor + t::carry_actor::component) : 0;
    return part && game::live(part) && game::is_a(part, carry_class) ? part : 0;
}

struct Shown { int32_t count; };

TSUCHI_Result show(const Hold& hold, Shown& kept, const int32_t& wanted, bool) {
    const uint64_t part = counter_part(game::from_handle(hold.object));
    if (!part) return TSUCHI_UNSUPPORTED;
    kept.count = wanted;
    game::write(part + t::carry_component::shown_count, wanted);
    return TSUCHI_OK;
}

// Once a frame the number is put back, should anything have changed it. Without its carry part the hold is over.
bool keep_shown(const Hold& hold, Shown& kept) {
    const uint64_t part = counter_part(game::from_handle(hold.object));
    if (!part) return false;
    if (game::read_or<int32_t>(part + t::carry_component::shown_count) != kept.count) game::write(part + t::carry_component::shown_count, kept.count);
    return true;
}

// The counter goes back to the carriers' number, as the game made it, for an object that is still there.
void give_back(const Hold& hold, Shown&, HoldEnd why) {
    if (why == HoldEnd::destroyed || why == HoldEnd::destroying) return;
    if (const uint64_t part = counter_part(game::from_handle(hold.object))) game::write(part + t::carry_component::shown_count, int32_t(-1));
}

Holds<Shown, int32_t, show, keep_shown, give_back> counts{"carry.count", HoldOn::actor, true};
} // namespace

TSUCHI_Result hold_count(TSUCHI_Mod* mod, TSUCHI_Actor actor, int32_t count) {
    if (family_absent(t::family::carry_count)) return TSUCHI_UNSUPPORTED; // a build without the shown count
    if (count < 0) return TSUCHI_INVALID;
    const uint64_t object = game::from_handle(actor.id);
    if (!object) return TSUCHI_NOT_FOUND;
    if (!game::is_game_actor(object)) return TSUCHI_INVALID;
    if (!counter_part(object)) return TSUCHI_UNSUPPORTED;
    return counts.take(mod, actor.id, 0, count);
}

TSUCHI_Result release_count(TSUCHI_Mod* mod, TSUCHI_Actor actor) { return counts.release(mod, actor.id, 0); }

} // namespace tsuchi
