// Checks of IDs: how one is spelled, which two are the same, and a vocabulary the game keeps
// as numbers. Exits with the number of failures.
#include <cstdio>
#include <cstring>
#include <string>

#include "ids.h"

namespace {

int failures = 0;
#define CHECK(condition) \
    do { \
        if (!(condition)) { \
            std::printf("%s:%d: %s\n", __FILE__, __LINE__, #condition); \
            ++failures; \
        } \
    } while (0)

using tsuchi::IdForm;

const std::string longest_part(47, 'a');

void spelling() {
    // A mod's ID, which is also how the name after the colon is spelled.
    CHECK(tsuchi::valid_mod_id("example.gems") && tsuchi::valid_mod_id("a") && tsuchi::valid_mod_id("7") && tsuchi::valid_mod_id("a-b_c.d"));
    CHECK(tsuchi::valid_mod_id(longest_part.c_str()) && !tsuchi::valid_mod_id((longest_part + "a").c_str()));
    CHECK(!tsuchi::valid_mod_id(nullptr) && !tsuchi::valid_mod_id("") && !tsuchi::valid_mod_id("Example") && !tsuchi::valid_mod_id("a b"));
    CHECK(!tsuchi::valid_mod_id(".a") && !tsuchi::valid_mod_id("a.") && !tsuchi::valid_mod_id("-") && !tsuchi::valid_mod_id("a:b") && !tsuchi::valid_mod_id("caf\xC3\xA9"));

    // The game's own: a plain name, in the game's spelling.
    CHECK(tsuchi::id_form("red") == IdForm::plain && tsuchi::id_form("OTADARTS") == IdForm::plain && tsuchi::id_form("Area001_P") == IdForm::plain);
    CHECK(tsuchi::id_form("_x") == IdForm::plain && tsuchi::id_form("a.b-c") == IdForm::plain && tsuchi::id_form(std::string(95, 'Z').c_str()) == IdForm::plain);
    CHECK(tsuchi::id_form(std::string(96, 'Z').c_str()) == IdForm::malformed && tsuchi::id_form(std::string(5000, 'Z').c_str()) == IdForm::malformed);
    CHECK(tsuchi::id_form(nullptr) == IdForm::malformed && tsuchi::id_form("") == IdForm::malformed && tsuchi::id_form("two words") == IdForm::malformed);
    CHECK(tsuchi::id_form("a/b") == IdForm::malformed && tsuchi::id_form("\xff") == IdForm::malformed && tsuchi::id_form("caf\xC3\xA9") == IdForm::malformed);
    // Text that never ends is read no further than an ID can go.
    char endless[tsuchi::kIdBytes];
    std::memset(endless, 'a', sizeof endless);
    CHECK(tsuchi::id_form(endless) == IdForm::malformed);

    // A mod's: its ID, a colon, and a name, both in small letters.
    size_t colon = 0;
    CHECK(tsuchi::id_form("example.gems:ruby", &colon) == IdForm::added && colon == 12);
    CHECK(tsuchi::id_form((longest_part + ":" + longest_part).c_str(), &colon) == IdForm::added && colon == 47);
    CHECK(tsuchi::id_form((longest_part + "a:x").c_str()) == IdForm::malformed && tsuchi::id_form(("x:" + longest_part + "a").c_str()) == IdForm::malformed);
    CHECK(tsuchi::id_form("example.gems:Ruby") == IdForm::malformed && tsuchi::id_form("Example.gems:ruby") == IdForm::malformed);
    CHECK(tsuchi::id_form(":ruby") == IdForm::malformed && tsuchi::id_form("example.gems:") == IdForm::malformed && tsuchi::id_form(":") == IdForm::malformed);
    CHECK(tsuchi::id_form("a:b:c") == IdForm::malformed && tsuchi::id_form("a:_b") == IdForm::malformed && tsuchi::id_form("a.:b") == IdForm::malformed);
    CHECK(tsuchi::id_valid("example.gems:ruby") && tsuchi::id_valid("red") && tsuchi::id_valid("#8") && !tsuchi::id_valid("no good"));

    // A number nobody has named.
    uint32_t number = 77;
    CHECK(tsuchi::id_form("#0", nullptr, &number) == IdForm::placeholder && number == 0);
    CHECK(tsuchi::id_form("#8", nullptr, &number) == IdForm::placeholder && number == 8);
    CHECK(tsuchi::id_form("#4294967295", nullptr, &number) == IdForm::placeholder && number == 0xFFFFFFFFu);
    CHECK(tsuchi::id_form("#4294967296") == IdForm::malformed && tsuchi::id_form("#99999999999") == IdForm::malformed);
    CHECK(tsuchi::id_form("#") == IdForm::malformed && tsuchi::id_form("#08") == IdForm::malformed && tsuchi::id_form("#-1") == IdForm::malformed);
    CHECK(tsuchi::id_form("#8a") == IdForm::malformed && tsuchi::id_form("a#8") == IdForm::malformed && tsuchi::id_form("example.gems:#8") == IdForm::malformed);
    char text[tsuchi::kIdBytes];
    tsuchi::id_placeholder(0, text);
    CHECK(std::string(text) == "#0");
    tsuchi::id_placeholder(4294967295u, text);
    CHECK(std::string(text) == "#4294967295");

    // The same thing: a plain name whatever its letters' case, as the engine matches names; anything else letter for letter.
    CHECK(tsuchi::id_same("OTADARTS", "otadarts") && tsuchi::id_same("Area001_P", "AREA001_p") && tsuchi::id_same("red", "red"));
    CHECK(!tsuchi::id_same("red", "reds") && !tsuchi::id_same("red", "re") && !tsuchi::id_same("red", "#0") && !tsuchi::id_same("a:b", "a"));
    CHECK(tsuchi::id_same("a:b", "a:b") && !tsuchi::id_same("a:b", "a:c") && tsuchi::id_same("#8", "#8") && !tsuchi::id_same("#8", "#9"));
    CHECK(!tsuchi::id_same("no good", "no good") && !tsuchi::id_same(nullptr, "red") && !tsuchi::id_same("red", nullptr));

    // Put together and taken apart.
    CHECK(tsuchi::id_join("example.gems", "ruby", text) && std::string(text) == "example.gems:ruby");
    CHECK(tsuchi::id_join(longest_part.c_str(), longest_part.c_str(), text) && std::strlen(text) == tsuchi::kIdLength);
    CHECK(!tsuchi::id_join("Example", "ruby", text) && !tsuchi::id_join("example", "", text) && !tsuchi::id_join("example", "a:b", text));
    char mod[tsuchi::kModIdBytes];
    const char* name = nullptr;
    CHECK(tsuchi::id_split("example.gems:ruby", mod, &name) && std::string(mod) == "example.gems" && std::string(name) == "ruby");
    CHECK(!tsuchi::id_split("ruby", mod, &name) && !tsuchi::id_split("#8", mod, &name) && !tsuchi::id_split("a:b:c", mod, &name));
}

constexpr tsuchi::IdName kColors[] = {{"red", 0}, {"blue", 1}, {"glow", 9}};
tsuchi::Vocabulary colors{"test.Color", kColors, 3, 255};
constexpr tsuchi::IdName kSizes[] = {{"small", 0}};
tsuchi::Vocabulary sizes{"test.Size", kSizes, 1, 3};

std::string id_of(const tsuchi::Vocabulary& vocabulary, uint32_t value) {
    char id[tsuchi::kIdBytes];
    tsuchi::id_of(vocabulary, value, id);
    return id;
}

void vocabulary() {
    uint32_t value = 77;
    // The game's own, by name, in any case.
    CHECK(tsuchi::id_value(colors, "red", &value) == TSUCHI_OK && value == 0);
    CHECK(tsuchi::id_value(colors, "GLOW", &value) == TSUCHI_OK && value == 9);
    CHECK(id_of(colors, 1) == "blue" && tsuchi::id_named(colors, 1));
    // One rule for what is not known. Text that is no ID is invalid, and an ID that names nothing is not found.
    value = 77;
    CHECK(tsuchi::id_value(colors, "not an id", &value) == TSUCHI_INVALID && tsuchi::id_value(colors, "", &value) == TSUCHI_INVALID);
    CHECK(tsuchi::id_value(colors, nullptr, &value) == TSUCHI_INVALID && tsuchi::id_value(colors, "plaid", &value) == TSUCHI_NOT_FOUND);
    CHECK(tsuchi::id_value(colors, "example.paint:plaid", &value) == TSUCHI_NOT_FOUND && value == 77);
    // A number with no name is never hidden: it has a placeholder, which is taken back for the same number.
    CHECK(id_of(colors, 8) == "#8" && !tsuchi::id_named(colors, 8));
    CHECK(tsuchi::id_value(colors, "#8", &value) == TSUCHI_OK && value == 8);
    CHECK(tsuchi::id_value(colors, "#255", &value) == TSUCHI_OK && value == 255 && id_of(colors, 255) == "#255");
    // Past what the game can hold there is nothing to name.
    CHECK(tsuchi::id_value(colors, "#256", &value) == TSUCHI_NOT_FOUND && value == 255);
    // A placeholder for a number that has a name is still that number; what is given back is the name.
    CHECK(tsuchi::id_value(colors, "#9", &value) == TSUCHI_OK && value == 9 && id_of(colors, 9) == "glow");
    for (uint32_t number = 0; number <= colors.most; ++number) {
        uint32_t back = 1000;
        CHECK(tsuchi::id_value(colors, id_of(colors, number).c_str(), &back) == TSUCHI_OK && back == number);
    }

    // Into a caller's buffer, whole or not at all.
    char buffer[8] = "zzz";
    uint64_t needed = 0;
    CHECK(tsuchi::id_into(colors, 9, buffer, sizeof buffer, &needed) == TSUCHI_OK && needed == 5 && std::string(buffer) == "glow");
    CHECK(tsuchi::id_into(colors, 9, buffer, 4, &needed) == TSUCHI_CAPACITY && needed == 5 && std::string(buffer) == "glow");
    CHECK(tsuchi::id_into(colors, 200, nullptr, 0, &needed) == TSUCHI_CAPACITY && needed == 5);

    // What a mod adds is named with the mod's ID, by a number the framework gave it, for as long as the mod runs.
    TSUCHI_Mod* paint = tsuchi::mod_create("example.paint");
    TSUCHI_Mod* other = tsuchi::mod_create("example.other");
    CHECK(tsuchi::id_add(colors, paint, "plaid", 10) == TSUCHI_OK && tsuchi::owned_count(paint) == 1);
    CHECK(tsuchi::id_value(colors, "example.paint:plaid", &value) == TSUCHI_OK && value == 10);
    CHECK(id_of(colors, 10) == "example.paint:plaid" && tsuchi::id_named(colors, 10));
    CHECK(tsuchi::id_value(colors, "plaid", &value) == TSUCHI_NOT_FOUND && tsuchi::id_value(colors, "example.other:plaid", &value) == TSUCHI_NOT_FOUND);
    CHECK(tsuchi::id_value(colors, "example.paint:Plaid", &value) == TSUCHI_INVALID); // a mod's name has one spelling
    CHECK(tsuchi::id_value(colors, "#10", &value) == TSUCHI_OK && value == 10);
    // Two mods never land on one number or one name, and the game's own are not taken over.
    CHECK(tsuchi::id_add(colors, other, "plaid", 10) == TSUCHI_BUSY && tsuchi::id_add(colors, other, "ruby", 0) == TSUCHI_BUSY);
    CHECK(tsuchi::id_add(colors, paint, "plaid", 11) == TSUCHI_BUSY && tsuchi::owned_count(other) == 0);
    CHECK(tsuchi::id_add(colors, other, "plaid", 11) == TSUCHI_OK && id_of(colors, 11) == "example.other:plaid");
    CHECK(tsuchi::id_add(colors, other, "Plaid", 12) == TSUCHI_INVALID && tsuchi::id_add(colors, other, "a:b", 12) == TSUCHI_INVALID);
    CHECK(tsuchi::id_add(colors, other, "big", 256) == TSUCHI_INVALID && tsuchi::id_add(colors, nullptr, "big", 12) == TSUCHI_INVALID);
    // The same name in another vocabulary is another thing.
    CHECK(tsuchi::id_add(sizes, paint, "plaid", 2) == TSUCHI_OK && tsuchi::id_value(sizes, "example.paint:plaid", &value) == TSUCHI_OK && value == 2);
    CHECK(tsuchi::id_value(sizes, "#4", &value) == TSUCHI_NOT_FOUND && tsuchi::id_value(sizes, "#3", &value) == TSUCHI_OK);
    // Taken back by the mod, or with it.
    CHECK(tsuchi::id_remove(colors, other, "ruby") == TSUCHI_NOT_FOUND && tsuchi::id_remove(colors, paint, "nothing") == TSUCHI_NOT_FOUND);
    CHECK(tsuchi::id_remove(colors, other, "plaid") == TSUCHI_OK && tsuchi::owned_count(other) == 0 && id_of(colors, 11) == "#11");
    CHECK(tsuchi::id_remove(colors, other, "plaid") == TSUCHI_NOT_FOUND && id_of(colors, 10) == "example.paint:plaid");
    tsuchi::mod_stop(paint);
    CHECK(id_of(colors, 10) == "#10" && id_of(sizes, 2) == "#2" && colors.added.count == 0 && sizes.added.count == 0);
    CHECK(tsuchi::id_value(colors, "example.paint:plaid", &value) == TSUCHI_NOT_FOUND);
    CHECK(tsuchi::id_add(colors, paint, "plaid", 10) == TSUCHI_INVALID); // a stopped mod adds nothing
    // The number is free for the next mod, and a mod that starts again starts with nothing.
    CHECK(tsuchi::id_add(colors, other, "tartan", 10) == TSUCHI_OK && id_of(colors, 10) == "example.other:tartan");
    tsuchi::mod_stop(other);
    TSUCHI_Mod* again = tsuchi::mod_create("example.paint");
    CHECK(again && tsuchi::id_value(colors, "example.paint:plaid", &value) == TSUCHI_NOT_FOUND && id_of(colors, 10) == "#10");
    tsuchi::mod_stop(again);
}

void allocation() {
    tsuchi::Vocabulary kinds{"test.Kind", kColors, 3, 255};
    auto* first = tsuchi::mod_create("example.first");
    auto* second = tsuchi::mod_create("example.second");
    uint32_t value = 99;
    CHECK(tsuchi::id_allocate(kinds, first, "ruby", 0, 3, &value) == TSUCHI_OK && value == 2);
    CHECK(tsuchi::id_allocate(kinds, first, "ruby", 0, 3, &value) == TSUCHI_OK && value == 2);
    CHECK(tsuchi::owned_count(first) == 1);
    CHECK(tsuchi::id_allocate(kinds, second, "ruby", 0, 3, &value) == TSUCHI_OK && value == 3);
    CHECK(tsuchi::id_remove(kinds, first, "ruby") == TSUCHI_OK);
    CHECK(tsuchi::id_allocate(kinds, second, "emerald", 0, 3, &value) == TSUCHI_CAPACITY && value == 3);
    CHECK(tsuchi::id_allocate(kinds, first, "ruby", 0, 3, &value) == TSUCHI_OK && value == 2);
    tsuchi::mod_stop(first);
    first = tsuchi::mod_create("example.first");
    CHECK(tsuchi::id_allocate(kinds, first, "ruby", 0, 3, &value) == TSUCHI_OK && value == 2);
    CHECK(tsuchi::id_allocate(kinds, first, "ruby", 3, 3, &value) == TSUCHI_INVALID);
    CHECK(tsuchi::id_allocate(kinds, first, "Ruby", 0, 255, &value) == TSUCHI_INVALID);
    CHECK(tsuchi::id_allocate(kinds, first, "new", 255, 254, &value) == TSUCHI_INVALID);
    CHECK(tsuchi::id_allocate(kinds, first, "new", 255, 256, &value) == TSUCHI_INVALID);
    CHECK(tsuchi::id_allocate(kinds, first, "new", 0, 255, nullptr) == TSUCHI_INVALID);
    tsuchi::mod_stop(first);
    tsuchi::mod_stop(second);
    kinds.given.clear();
}

} // namespace

int main() {
    spelling();
    vocabulary();
    allocation();
    if (failures) std::printf("%d failures\n", failures);
    return failures;
}
