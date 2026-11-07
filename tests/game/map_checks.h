#include "placement_mode.h"
// Checks of what the parts of the manifest's "maps" share. Included by objects_test.cpp.

// A content path is split at the dot after its last `/`, has no other dot, and is compared whatever the case
// of its letters.
void content_paths() {
    namespace cp = tsuchi::content_path;
    cp::Parts parts;
    const auto package_of = [&](const char* path) { return cp::split(path, parts) ? std::string(path, parts.package) : std::string("<none>"); };
    const auto object_of = [&](const char* path) { return cp::split(path, parts) && parts.object ? std::string(parts.object, parts.object_bytes) : std::string("<none>"); };
    CHECK(package_of("/Game/Mods/my_mod/Mine") == "/Game/Mods/my_mod/Mine" && object_of("/Game/Mods/my_mod/Mine") == "<none>");
    CHECK(package_of("/Game/Mods/my_mod/Rock.Rock_C") == "/Game/Mods/my_mod/Rock" && object_of("/Game/Mods/my_mod/Rock.Rock_C") == "Rock_C");
    CHECK(package_of("/G") == "/G" && package_of("/G/a.b") == "/G/a" && object_of("/G/a.b") == "b");
    // No dot but that one: none in a folder's name, and one at most after the last `/`.
    for (const char* none : {"", "/", "Game/Mine", "/Game//Mine", "/Game/Mine/", "/Game/Mods/my.mod/Mine", "/Game/Mods/my.mod/Rock.Rock_C", "/Game/Rock.Rock.Rock_C", "/Game/.Rock",
                             "/Game/Rock.", ".", "/.", "/Game.x/Rock"})
        CHECK(package_of(none) == "<none>" && cp::wrong(none, cp::Names::either) == cp::kNoPath);
    CHECK(!cp::split(nullptr, 5, parts) && !cp::split("/Game/Mine\0x", 12, parts) && cp::split("/Game/Mine.x", 10, parts) && !parts.object);
    // What a part wants a path to name, and what is told when it names the other.
    CHECK(!cp::wrong("/Game/Mine", cp::Names::package) && !cp::wrong("/Game/Rock.Rock_C", cp::Names::object) && !cp::wrong("/Game/Mine", cp::Names::either) &&
          !cp::wrong("/Game/Rock.Rock_C", cp::Names::either));
    CHECK(cp::wrong("/Game/Rock.Rock_C", cp::Names::package) == cp::kNoPackage && cp::wrong("/Game/Mine", cp::Names::object) == cp::kNoObject &&
          cp::wrong("/Game/a.b/Mine", cp::Names::package) == cp::kNoPath && cp::wrong("/Game/a.b/Rock.Rock_C", cp::Names::object) == cp::kNoPath);
    // Two paths are the same whatever the case of their letters, and no two of different lengths are.
    CHECK(cp::same("/Game/Cave_P", "/game/cave_p") && cp::same("", "") && !cp::same("/Game/Cave_P", "/Game/Cave_Q") && !cp::same("/Game/Cave_P", "/Game/Cave_") &&
          !cp::same("/Game/Cave", "/Game/Cave_P"));
    CHECK(cp::same("/G/Ab", 5, "/g/aB", 5) && !cp::same("/G/Ab", 5, "/g/aB", 4) && cp::same("/G/Ab", 4, "/g/aB", 4) && !cp::same("/G/A@", 5, "/G/A`", 5));
}

// Shared ground and explicit mode overrides, separate from object-mode defaults.
void ground_modes() {
    const char* text = R"([{"map":"/G/Area_P","levels":["/G/Shared"]},
        {"map":"/g/area_p","mode":"night","navigation":"night.nav"},
        {"map":"/G/Area_P","mode":"night","placements":["/G/NightObjects"]},
        {"map":"/G/Other_P","mode":"night","levels":[]},
        {"map":"/G/Area_P","mode":"olimar","levels":[]}])";
    const tsuchi::Json* entries = tsuchi::read_json(text, strlen(text));
    CHECK(entries != nullptr);
    if (!entries) return;
    const auto& shared = *entries->at(0);
    using Mode = tsuchi::PlacementMode;
    CHECK(tsuchi::placement_mode(shared) == Mode::story);
    for (Mode mode : {Mode::story, Mode::night, Mode::olimar}) CHECK(tsuchi::map_ground_matches(shared, mode));
    CHECK(!tsuchi::map_ground_matches(shared, Mode::invalid));
    CHECK(tsuchi::map_ground_overrides(shared, *entries->at(1), Mode::night));
    CHECK(!tsuchi::map_ground_overrides(shared, *entries->at(1), Mode::story));
    CHECK(!tsuchi::map_ground_overrides(shared, *entries->at(1), Mode::olimar));
    CHECK(!tsuchi::map_ground_overrides(shared, *entries->at(2), Mode::night));
    CHECK(!tsuchi::map_ground_overrides(shared, *entries->at(3), Mode::night));
    CHECK(tsuchi::map_ground_overrides(shared, *entries->at(4), Mode::olimar));
    CHECK(!tsuchi::map_ground_matches(*entries->at(1), Mode::story));
    CHECK(tsuchi::placement_mode(*entries->at(2)) == Mode::night);
    tsuchi::release_json(entries);
}
