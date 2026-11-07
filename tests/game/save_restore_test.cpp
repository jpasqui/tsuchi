// save.restore against the made-up game: the copy goes into the current save object of its kind
// through the object's own copy function, the object applies itself, and the game instance keeps its
// objects. Host evidence, not a game run.
#include "save_restore.cpp"
#include "reflect_world.h"
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace tsuchi;
namespace t = tsuchi::game::table;

namespace fixture {
int failures = 0;
uint64_t instance = 0, save_class = 0, story_class = 0, quest_class = 0, player_class = 0, other_class = 0;
uint64_t story = 0, quest = 0, functions = 0;
std::vector<std::string> calls;

std::string name_of(uint64_t object) {
    if (object == story) return "story";
    if (object == quest) return "quest";
    return "copy";
}
void copy_from(uint64_t save, uint64_t copy) { calls.push_back("copy " + name_of(save) + " " + name_of(copy)); }
void apply(uint64_t save) { calls.push_back("apply " + name_of(save)); }

// A class of the game's saves, and an object of it whose function table has the copy and apply entries.
uint64_t save_type(const std::u16string& name) {
    const uint64_t type = fake::object(name, w.root);
    fake::set_super(type, save_class);
    return type;
}
uint64_t save_object(const std::u16string& name, uint64_t type) {
    const uint64_t made = fake::object(name, type, 0x100);
    fake::put(made, functions);
    return made;
}
void setup() {
    build_world();
    save_class = fake::object(u"CarrotSaveGame", w.root);
    story_class = save_type(u"CarrotSaveGame_Story");
    quest_class = save_type(u"CarrotSaveGame_Quest");
    player_class = save_type(u"CarrotSaveGame_Player");
    other_class = fake::object(u"Thing", w.root);
    functions = fake::memory(0x400);
    const uint64_t copy_at = reinterpret_cast<uint64_t>(&copy_from), apply_at = reinterpret_cast<uint64_t>(&apply);
    fake::put(functions + t::save_table::copy_from, copy_at);
    fake::put(functions + t::save_table::apply, apply_at);
    fake::code().push_back(copy_at);
    fake::code().push_back(apply_at);
    instance = fake::object(u"Instance", w.component, 0x200);
    fake::put(fake::main_image() + t::data::game_instance, instance);
    story = save_object(u"Story", story_class);
    quest = save_object(u"Quest", quest_class);
    // The current saves, one of each kind the game has now; no player save in this made-up game.
    fake::put(instance + t::game_instance::saves + 8, story);
    fake::put(instance + t::game_instance::saves + 16, quest);
    calls.clear();
}
}
#define CHECK(x) do { if (!(x)) { std::printf("line %d: %s\n", __LINE__, #x); ++fixture::failures; } } while (0)

namespace tsuchi::game {
uint64_t game_framework() { return 0; } // flow.cpp asks for it elsewhere; restoring a save does not
}
namespace tsuchi {
TSUCHI_Result restore(uint64_t object) { return restore_save(game::to_handle(object)); }
}

int main(int argc, char** argv) {
    using namespace fixture;
    const std::string mode = argc > 1 ? argv[1] : "";
    setup();
    if (mode == "copy") {
        // The copy's data goes into the current object of its kind, which then applies itself; the game
        // instance keeps the same objects.
        const uint64_t copy = save_object(u"StoryCopy", story_class);
        CHECK(restore(copy) == TSUCHI_OK);
        CHECK((calls == std::vector<std::string>{"copy story copy", "apply story"}));
        CHECK(fake::get<uint64_t>(instance + t::game_instance::saves + 8) == story);
        CHECK(fake::get<uint64_t>(instance + t::game_instance::saves + 16) == quest);
        calls.clear();
        const uint64_t quest_copy = save_object(u"QuestCopy", quest_class);
        CHECK(restore(quest_copy) == TSUCHI_OK);
        CHECK((calls == std::vector<std::string>{"copy quest copy", "apply quest"}));
    } else if (mode == "refusals") {
        // Nothing of the game's is called for a refused copy.
        CHECK(restore(fake::object(u"NotASave", other_class)) == TSUCHI_INVALID);
        CHECK(restore(story) == TSUCHI_INVALID);
        CHECK(restore(save_object(u"PlayerCopy", player_class)) == TSUCHI_NOT_FOUND);
        CHECK(restore_save(0) == TSUCHI_INVALID);
        const uint64_t copy = save_object(u"StoryCopy", story_class);
        fake::put<uint8_t>(fake::main_image() + t::data::save_async_state, 1);
        CHECK(restore(copy) == TSUCHI_BUSY);
        fake::put<uint8_t>(fake::main_image() + t::data::save_async_state, 0);
        const uint64_t foreign = fake::memory(0x400); // a table whose entries are no code of the game's
        fake::put(foreign + t::save_table::copy_from, uint64_t(0x1234));
        fake::put(story, foreign);
        CHECK(restore(copy) == TSUCHI_UNSUPPORTED);
        fake::put(story, functions);
        fake::put(fake::main_image() + t::data::game_instance, uint64_t(0));
        CHECK(restore(copy) == TSUCHI_NOT_READY);
        CHECK(calls.empty());
    } else {
        std::printf("unknown mode\n");
        return 2;
    }
    if (failures) return 1;
    std::printf("Save restore checks passed\n");
    return 0;
}
