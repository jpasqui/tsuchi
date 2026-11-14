// The hud service's prompt calls and holds against host memory and the real handles and holds; the presenter's
// markers, which keep the game's prompts, are stood in for here (presenter_test.cpp checks them).
#include "fake_game.h"
#include "holds.h"
#include "hud_prompts.h"
#include "objects.h"
#include "ordinary_lifecycle.h"
#include <cassert>
#include <cstdio>
#include <map>
#include <vector>

using namespace tsuchi;
namespace t = game::table;
namespace {
std::vector<void (*)()> steps;
void (*actor_left_listener)(uint64_t, uint64_t, ActorLeft) = nullptr;
void step() { for (const auto work : steps) work(); }

// The markers stood in for: whether watching can start, what they report, and the hides they were given.
int watch_state = 0; // 0 cannot, 1 watching
bool watch_possible = true;
TSUCHI_Mod* watched_for = nullptr;
std::map<uint64_t, uint32_t> screens;
struct Hide { uint64_t actor; uint32_t players; };
std::map<uint64_t, Hide> hides;
bool hides_full = false;

uint64_t meta, actor_type;
TSUCHI_Actor actor_handle(uint64_t object) { return {game::to_handle(object)}; }
uint64_t make_actor(const char16_t* name) { return fake::object(name, actor_type, 0x400); }

void test_prompts() {
    TSUCHI_Mod* framework = mod_create("prompts.framework");
    TSUCHI_Mod* owner = mod_create("prompts.owner");
    TSUCHI_Mod* other = mod_create("prompts.other");
    assert(framework && owner && other);
    const uint64_t oatchi = make_actor(u"Oatchi"), captain = make_actor(u"Captain");
    uint32_t players = 77;
    // Before the service starts nothing can be watched.
    assert(hud_prompts_shown(actor_handle(oatchi), &players) == TSUCHI_NOT_READY && players == 77);
    hud_prompts_start(framework);
    assert(hud_prompts_shown(actor_handle(oatchi), nullptr) == TSUCHI_INVALID);
    assert(hud_prompts_shown(TSUCHI_Actor{}, &players) == TSUCHI_NOT_FOUND);
    // The first question starts watching, for the framework's own mod, and is answered from then on.
    watch_possible = false;
    assert(hud_prompts_shown(actor_handle(oatchi), &players) == TSUCHI_NOT_READY);
    watch_possible = true;
    assert(hud_prompts_shown(actor_handle(oatchi), &players) == TSUCHI_NOT_READY && watched_for == framework);
    screens[oatchi] = 1;
    assert(hud_prompts_shown(actor_handle(oatchi), &players) == TSUCHI_OK && players == 1);
    assert(hud_prompts_shown(actor_handle(captain), &players) == TSUCHI_OK && players == 0);
    // What is refused hides nothing.
    assert(hud_hold_hidden_prompts(owner, actor_handle(captain), 0) == TSUCHI_INVALID);
    assert(hud_hold_hidden_prompts(owner, actor_handle(captain), 16) == TSUCHI_INVALID);
    assert(hud_hold_hidden_prompts(owner, TSUCHI_Actor{}, 1) == TSUCHI_NOT_FOUND);
    hides_full = true;
    assert(hud_hold_hidden_prompts(owner, actor_handle(captain), 1) == TSUCHI_CAPACITY);
    hides_full = false;
    assert(hides.empty() && hud_release_hidden_prompts(owner, actor_handle(captain)) == TSUCHI_NOT_FOUND);
    // A hold hides the actor's prompts on its screens; held again, the screens change; released, they come back.
    assert(hud_hold_hidden_prompts(owner, actor_handle(captain), 1) == TSUCHI_OK);
    assert(hides.size() == 1 && hides.begin()->second.actor == captain && hides.begin()->second.players == 1);
    assert(hud_hold_hidden_prompts(owner, actor_handle(captain), 15) == TSUCHI_OK);
    assert(hides.size() == 1 && hides.begin()->second.players == 15);
    // Another mod's hold on the same actor is its own, and its release leaves the first.
    assert(hud_hold_hidden_prompts(other, actor_handle(captain), 2) == TSUCHI_OK && hides.size() == 2);
    assert(hud_release_hidden_prompts(other, actor_handle(captain)) == TSUCHI_OK && hides.size() == 1);
    assert(hud_release_hidden_prompts(other, actor_handle(captain)) == TSUCHI_NOT_FOUND);
    assert(hud_release_hidden_prompts(owner, actor_handle(captain)) == TSUCHI_OK && hides.empty());
    assert(hud_release_hidden_prompts(owner, actor_handle(captain)) == TSUCHI_NOT_FOUND);
    // The actor leaves play: the hold ends.
    assert(hud_hold_hidden_prompts(owner, actor_handle(oatchi), 1) == TSUCHI_OK && hides.size() == 1);
    assert(actor_left_listener);
    actor_left_listener(oatchi, game::to_handle(oatchi), ActorLeft{});
    step();
    assert(hides.empty() && hud_release_hidden_prompts(owner, actor_handle(oatchi)) == TSUCHI_NOT_FOUND);
    // The actor is gone: the hold ends.
    const uint64_t copy = make_actor(u"CaptainCopy");
    const TSUCHI_Actor copy_handle = actor_handle(copy);
    assert(hud_hold_hidden_prompts(owner, copy_handle, 1) == TSUCHI_OK && hides.size() == 1);
    fake::destroy(copy);
    step();
    assert(hides.empty());
    // The mod stops: the hold ends.
    assert(hud_hold_hidden_prompts(owner, actor_handle(captain), 1) == TSUCHI_OK && hides.size() == 1);
    mod_stop(owner);
    assert(hides.empty());
    // Watching that cannot start leaves no hold.
    TSUCHI_Mod* late = mod_create("prompts.late");
    watch_state = 0, watch_possible = false;
    assert(hud_hold_hidden_prompts(late, actor_handle(captain), 1) == TSUCHI_NOT_READY && hides.empty());
    mod_stop(late), mod_stop(other), mod_stop(framework);
}
} // namespace

namespace tsuchi {
bool each_frame(void (*work)()) { steps.push_back(work); return true; }
bool on_actor_left(void (*listener)(uint64_t, uint64_t, ActorLeft)) { actor_left_listener = listener; return true; }
void ordinary_lifecycle_listener(void (*)(uint64_t, uint64_t, bool)) {}
TSUCHI_Result ordinary_lifecycle_watch(uint64_t) { return TSUCHI_OK; }
uint64_t ordinary_lifecycle_restore_part(uint64_t part, uint64_t) { return game::from_handle(part); }
int markers_watch_prompts(TSUCHI_Mod* self) {
    if (!self || !watch_possible) return 0;
    watched_for = self;
    if (watch_state) return 1;
    watch_state = 1;
    return 2;
}
uint32_t markers_prompt_screens(uint64_t actor) {
    const auto found = screens.find(actor);
    return found == screens.end() ? 0 : found->second;
}
bool markers_hide_prompts(uint64_t key, uint64_t actor, uint32_t players) {
    if (hides_full && !hides.count(key)) return false;
    hides[key] = {actor, players};
    return true;
}
void markers_show_prompts(uint64_t key) { hides.erase(key); }
} // namespace tsuchi
namespace tsuchi::game {
bool is_game_actor(uint64_t) { return true; }
uint64_t part_owner(uint64_t) { return 0; }
} // namespace tsuchi::game

int main() {
    fake::start();
    set_thread_probe([] { return uint64_t(1); });
    note_game_thread();
    meta = fake::object(u"Class", 0, 0x200);
    actor_type = fake::object(u"Actor", meta, 0x200);
    test_prompts();
    std::puts("hud prompts: reads, holds, their changes and their ends passed");
}
