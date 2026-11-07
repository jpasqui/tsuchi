// Checks of the base layer against a made-up game. Exits with the number of failures.
#include <cstdio>
#include <string>

#include "fake_game.h"
#include "names.h"
#include "objects.h"

namespace {

using namespace tsuchi::game;

int failures = 0;
#define CHECK(condition) \
    do { \
        if (!(condition)) { \
            std::printf("%s:%d: %s\n", __FILE__, __LINE__, #condition); \
            ++failures; \
        } \
    } while (0)

std::string text_of(Name name) {
    char buffer[64];
    return name_text(name, buffer, sizeof buffer) ? buffer : "<unreadable>";
}

void memory() {
    fake::start();
    const uint64_t block = fake::memory(16);
    uint32_t value = 7;
    CHECK(write(block, uint32_t(42)) && read(block, value) && value == 42);
    CHECK(!read(block + 14, value) && value == 42);       // runs off the end: refused, value untouched
    CHECK(!read(uint64_t(0), value) && !read(uint64_t(8), value));
    CHECK(read_or<uint32_t>(0, 9) == 9 && read_or<uint32_t>(block) == 42);
    fake::protect(block, false);
    CHECK(!write(block, uint32_t(1)) && read_or<uint32_t>(block) == 42); // readable, not writable
    fake::unmap(block);
    CHECK(!read(block, value));
    CHECK(global<int32_t>(fake::t::data::object_serial_counter) == 1000);
}

void names() {
    fake::start();
    const Name plain = fake::name(u"Bulborb");
    const Name numbered = fake::name(u"Onion", 4);
    const Name wide = fake::name(u"Pikminé日");          // two-byte and three-byte characters
    const Name astral = fake::name(u"A\U0001F331");               // a surrogate pair
    CHECK(text_of(plain) == "Bulborb");
    CHECK(text_of(numbered) == "Onion_3");
    CHECK(text_of(wide) == "Pikmin\xC3\xA9\xE6\x97\xA5");
    CHECK(text_of(astral) == "A\xF0\x9F\x8C\xB1");
    CHECK(name_is(plain, "Bulborb") && !name_is(plain, "Bulbor") && !name_is(plain, "Bulborbs") && !name_is(plain, nullptr));
    CHECK(name_is(numbered, "Onion_3") && !name_is(numbered, "Onion"));

    // A buffer that is too small is refused, with the size it takes. Nothing is cut short.
    char small[4] = "zzz";
    size_t needed = 0;
    CHECK(!name_text(plain, small, sizeof small, &needed) && needed == 8 && std::string(small) == "zzz");
    char exact[8];
    CHECK(name_text(plain, exact, sizeof exact, &needed) && std::string(exact) == "Bulborb");
    CHECK(!name_text(plain, nullptr, 0, &needed) && needed == 8);

    // Names the game does not have.
    CHECK(!name_text({0x00FFFFFF, 0}, exact, sizeof exact, &needed) && needed == 0); // block out of range
    CHECK(!name_text({200, 0}, exact, sizeof exact));                                // an empty place

    // The normal objects route exceeds the old 32-block fixture pool.
    const uint64_t marker = 0x123456789abcdef0;
    fake::put<uint64_t>(fake::main_image() + fake::t::data::allocator, marker);
    fake::next_name() = 32u << fake::t::constant::names_per_block_shift;
    const Name beyond_old_pool = fake::name(u"BeyondOldPool");
    CHECK(beyond_old_pool.index && name_is(beyond_old_pool, "BeyondOldPool"));
    CHECK(fake::get<uint64_t>(fake::main_image() + fake::t::data::allocator) == marker);

    const uint32_t capacity = fake::t::constant::name_block_count << fake::t::constant::names_per_block_shift;
    fake::next_name() = capacity - 1;
    const Name last = fake::name(u"Z");
    CHECK(last.index == capacity - 1 && name_is(last, "Z") && fake::next_name() == capacity);
    const size_t blocks_before = fake::blocks().size();
    CHECK(fake::name(u"Full").index == 0 && fake::next_name() == capacity);
    CHECK(fake::blocks().size() == blocks_before && name_is(last, "Z"));
    CHECK(fake::get<uint64_t>(fake::main_image() + fake::t::data::allocator) == marker);
    fake::next_name() = capacity - 1;
    CHECK(fake::name(u"TooLongForLastPlace").index == 0 && fake::next_name() == capacity - 1);
    CHECK(name_is(last, "Z"));
    fake::next_name() = 1;
    CHECK(fake::name(std::u16string(4000, u'Z')).index == 0 && fake::next_name() == 1);
}

void objects_and_handles() {
    fake::start();
    const uint64_t root = fake::object(u"Class", 0);
    const uint64_t thing = fake::object(u"Thing", root);
    const uint64_t a = fake::object(u"a", thing), b = fake::object(u"b", thing);
    CHECK(live(root) && live(a) && live(b));
    CHECK(!live(0) && !live(a + 8) && !live(fake::memory(0x80))); // not objects
    CHECK(class_of(a) == thing && name_is(name_of(a), "a"));

    int seen = 0;
    each_object([](void* count, uint64_t) { return ++*static_cast<int*>(count) < 3; }, &seen);
    CHECK(seen == 3); // stops when asked
    seen = 0;
    each_object([](void* count, uint64_t) { return ++*static_cast<int*>(count), true; }, &seen);
    CHECK(seen == 4);

    // A handle resolves to its object and to nothing else.
    const uint64_t ha = to_handle(a), hb = to_handle(b);
    CHECK(ha && hb && ha != hb && to_handle(a) == ha);
    CHECK(from_handle(ha) == a && from_handle(hb) == b);
    CHECK(!from_handle(0) && !from_handle(ha ^ 1) && !from_handle(~uint64_t(0)));
    CHECK(!to_handle(0) && !to_handle(a + 8));
    // Original DestroyActor sets this before the later GU PendingKill update.
    fake::put<uint8_t>(a + fake::t::engine_actor::play_state, uint8_t(fake::t::constant::actor_destroying_mask));
    CHECK(live(a) && from_handle(ha) == a);
    CHECK(read_or<uint8_t>(a + fake::t::engine_actor::play_state) & fake::t::constant::actor_destroying_mask);
    fake::put<uint8_t>(a + fake::t::engine_actor::play_state, 0);
    CHECK(live(a) && from_handle(ha) == a);

    // Making a handle numbers the object, as the game does for its own weak references.
    CHECK(fake::get<int32_t>(fake::item_at(2) + fake::t::object_item::serial) == 1001);
    CHECK(fake::get<int32_t>(fake::main_image() + fake::t::data::object_serial_counter) == 1002);

    // Being destroyed ends it.
    fake::put<uint32_t>(fake::item_at(3) + fake::t::object_item::flags, uint32_t(fake::t::constant::unreachable_flags));
    CHECK(!live(b) && !from_handle(hb));
    seen = 0;
    each_object([](void* count, uint64_t) { return ++*static_cast<int*>(count), true; }, &seen);
    CHECK(seen == 3);

    // Another object in the same table slot is not the old one, even at the same address.
    fake::destroy(a);
    CHECK(!live(a) && !from_handle(ha));
    fake::place(a, 2);
    CHECK(live(a) && !from_handle(ha));
    const uint64_t ha2 = to_handle(a);
    CHECK(ha2 && ha2 != ha && from_handle(ha2) == a);

    // The game reusing a live object for something else ends its handles too.
    end_life(a);
    CHECK(live(a) && !from_handle(ha2));
    const uint64_t ha3 = to_handle(a);
    CHECK(ha3 && ha3 != ha2 && from_handle(ha3) == a);
    end_life(a);
    end_life(a);
    CHECK(!from_handle(ha3) && from_handle(to_handle(a)) == a);
    CHECK(from_handle(to_handle(root)) == root); // other objects are untouched

    // An object whose number cannot be written gets no handle.
    const uint64_t c = fake::object(u"c", thing);
    fake::protect(fake::main_image(), false);
    CHECK(!to_handle(c));
    fake::protect(fake::main_image(), true);
    CHECK(from_handle(to_handle(c)) == c);
}

void classes() {
    fake::start();
    const uint64_t root = fake::object(u"Class", 0);
    const uint64_t blueprint_class = fake::object(u"BlueprintClass", root); // a kind of class
    fake::set_super(blueprint_class, root);
    const uint64_t actor = fake::object(u"Actor", root);
    const uint64_t decoy = fake::object(u"Creature", actor); // an object that only shares the name, and comes first
    const uint64_t creature = fake::object(u"Creature", root);
    fake::set_super(creature, actor);
    const uint64_t bulborb = fake::object(u"Bulborb", blueprint_class);     // a class made by a Blueprint
    fake::set_super(bulborb, creature);
    const uint64_t treasure = fake::object(u"Treasure", root);
    fake::set_super(treasure, actor);
    const uint64_t red = fake::object(u"red", bulborb), nugget = fake::object(u"nugget", treasure);

    static ClassRef actors{"Actor"}, creatures{"Creature"}, bulborbs{"Bulborb"}, treasures{"Treasure"}, missing{"Onion"};
    actors = {"Actor"}, creatures = {"Creature"}, bulborbs = {"Bulborb"}, treasures = {"Treasure"}, missing = {"Onion"};
    CHECK(find_class(creatures) == creature && find_class(creatures) != decoy);
    CHECK(find_class(bulborbs) == bulborb && find_class(missing) == 0);
    CHECK(is_a(red, bulborbs) && is_a(red, creatures) && is_a(red, actors) && !is_a(red, treasures));
    CHECK(is_a(nugget, treasures) && is_a(nugget, actors) && !is_a(nugget, creatures));
    CHECK(is_a(decoy, actors) && !is_a(decoy, creatures));
    CHECK(!is_a(0, actors) && !is_a(red, missing));

    // The answer is remembered: the name is not looked up again.
    fake::put(creature + fake::t::object::name, fake::name(u"Renamed"));
    CHECK(find_class(creatures) == creature && is_a(red, creatures));

    // A class that goes away is looked for again, and found when it comes back.
    fake::destroy(bulborb);
    CHECK(find_class(bulborbs) == 0 && !is_a(red, bulborbs));
    const uint64_t reloaded = fake::object(u"Bulborb", blueprint_class);
    fake::set_super(reloaded, actor);
    const uint64_t blue = fake::object(u"blue", reloaded);
    CHECK(find_class(bulborbs) == reloaded && is_a(blue, bulborbs));

    // A class chain that loops does not hang.
    const uint64_t loop_a = fake::object(u"LoopA", root), loop_b = fake::object(u"LoopB", root);
    fake::set_super(loop_a, loop_b);
    fake::set_super(loop_b, loop_a);
    CHECK(!is_a(fake::object(u"looper", loop_a), treasures));
}

float doubled(float value) { return value * 2; }

void functions() {
    // The table gives each of the game's functions its real signature, so a decimal is passed
    // and returned as one. A test points them at its own.
    CHECK(table::fn::double_it == nullptr);
    table::bind_functions(0x10000);
    CHECK(reinterpret_cast<uint64_t>(table::fn::double_it) == 0x11000);
    CHECK(reinterpret_cast<uint64_t>(table::fn::add) == 0x12000);
    table::fn::double_it = doubled;
    CHECK(table::fn::double_it(1.25f) == 2.5f);
    CHECK(table::slot::actor_end_play.table == 0x3000 && table::slot::actor_end_play.stock == 0x1234);
    CHECK(std::string(table::version) == "0.0");
}

} // namespace

int main() {
    memory();
    names();
    objects_and_handles();
    classes();
    functions();
    if (failures) std::printf("%d failed\n", failures);
    return failures;
}
