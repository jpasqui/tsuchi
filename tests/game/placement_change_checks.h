// The "placement_changes" of a maps entry, included inside the placement fixture block: rows of the game's own
// placement lists moved and removed as the game reads them, the asset itself never written.
{
    namespace x = t::engine_transform;
    for (const char* id : made) disk.files.erase(mods_at + id + "/mod.json");
    // A map whose world settings name a list of lists, as the game's maps do, which holds two lists.
    const uint64_t set_type = fake::object(u"PlaceListHolder", w.root), settings_type = fake::object(u"PlaceSettings", w.root);
    fake::field(set_type, u"AssetList", u"ArrayProperty", int32_t(list_reads.held_at), 0x10, fake::loose_field(u"ObjectProperty", 8, list_type));
    fake::field(settings_type, u"ActorPlacementInfoListAssetsCache", u"ArrayProperty", int32_t(list_reads.settings_at), 0x10, fake::loose_field(u"ObjectProperty", 8, set_type));
    CHECK(list_reads.rows_at == uint32_t(kRowsAt));
    const auto array_of = [&](uint64_t at, std::vector<uint64_t> items) {
        const uint64_t data = fake::memory(items.size() * 8 + 8);
        for (size_t i = 0; i < items.size(); ++i) fake::put(data + i * 8, items[i]);
        fake::put(at + t::array::data, data), fake::put<int32_t>(at + t::array::count, int32_t(items.size())), fake::put<int32_t>(at + t::array::max, int32_t(items.size()));
    };
    // A game list: each row stands where its list puts it, in both of its transforms.
    const auto game_list = [&](const std::u16string& package, const std::u16string& name, std::vector<Row> rows) {
        const uint64_t asset = list_of(package, name, rows), data = fake::get<uint64_t>(asset + kRowsAt);
        fake::put(asset + t::object::outer, fake::object(package, w.root, 0x40));
        for (size_t i = 0; i < rows.size(); ++i)
            for (uint32_t transform : {uint32_t(g::init_transform), uint32_t(g::transform)}) {
                fake::put(data + i * g::size + transform + x::translation, rows[i].x);
                fake::put(data + i * g::size + transform + x::rotation + 12, 1.0f);
                for (int axis = 0; axis < 3; ++axis) fake::put(data + i * g::size + transform + x::scale + 4 * axis, 1.0f);
            }
        return asset;
    };
    const uint64_t objects = game_list(u"/G/C/Objects", u"Objects", {{0x7001, u"/G/Pot.Pot_C", 1, 10}, {0x7002, u"/G/Pot.Pot_C", 1, 20}, {0x7003, u"/G/Pot.Pot_C", 1, 30}});
    const uint64_t teki = game_list(u"/G/C/Teki", u"Teki", {{0x7101, u"/G/Bug.Bug_C", 1, 40}});
    const uint64_t holder = fake::object(u"APL_Cave", set_type, 0x60), settings = fake::object(u"Settings", settings_type, 0x60);
    array_of(holder + list_reads.held_at, {objects, teki});
    array_of(settings + list_reads.settings_at, {holder});
    const uint64_t cave = fake::object(u"Cave_P", world_type, 0x100), level = fake::object(u"PersistentLevel", w.root, 0x100), playing = fake::memory(0x100);
    fake::put(cave + t::object::outer, fake::object(u"/G/Cave_P", w.root, 0x40));
    fake::put(cave + t::world::persistent_level, level), fake::put(level + t::engine_level::owning_world, cave), fake::put(level + t::engine_level::world_settings, settings);
    fake::put<uint8_t>(playing + t::actor_play::state, uint8_t(t::constant::actor_play_state_mask));
    array_of(level + t::engine_level::actors, {playing});
    keeper = keeper_of(0, true);
    enter(cave);
    // The bytes of both lists as the asset holds them, to see that no read writes them.
    const auto bytes_of = [&](uint64_t asset) {
        const uint64_t data = fake::get<uint64_t>(asset + kRowsAt);
        const int32_t count = fake::get<int32_t>(asset + kRowsAt + t::array::count);
        std::string out(reinterpret_cast<const char*>(asset + kRowsAt), 0x10);
        return out + std::string(reinterpret_cast<const char*>(data), size_t(count) * g::size);
    };
    const std::string objects_bytes = bytes_of(objects), teki_bytes = bytes_of(teki);
    const auto unchanged = [&] { return bytes_of(objects) == objects_bytes && bytes_of(teki) == teki_bytes; };
    const auto build_now = [&] {
        list_reads.log.clear(), logged.lines.clear();
        thunks::call(tsuchi_hook_target(tsuchi::kCodeSiteThunk | t::code_site::placement_build.number), keeper, cave, settings + list_reads.settings_at);
        return list_reads.log;
    };
    const auto restore_now = [&](bool restored) {
        list_reads.log.clear(), logged.lines.clear(), list_reads.restored = restored;
        const bool said = (thunks::call(tsuchi_hook_target(tsuchi::kCodeSiteThunk | t::code_site::placement_restore.number), keeper, cave, 1) & 0xFF) != 0;
        CHECK(said == restored);
        return list_reads.log;
    };
    // With no mod, the game reads its lists as they are.
    CHECK(build_now() == "build: 7001@10/10 7002@20/20 7003@30/30 7101@40/40 " && logged.lines.empty());

    // Four mods change the map, in the order they start: the first moves one row and removes another, the second
    // moves the first one's row and another of the same list and removes a row of the second list, the third names
    // a row the list does not have, and the fourth a list the map does not have. A night entry is not for a day.
    const std::string map = R"(, "maps": [{"map": "/G/Cave_P", "placement_changes": )";
    plain("change.a", "1.0.0", map + R"({"/G/C/Objects": {"move": {"28673": {"location": [100, 1, 2], "rotation": [0, 90, 0]}}, "remove": ["28674"]}}}])");
    plain("change.b", "1.0.0", map + R"({"/G/C/Objects": {"move": {"28673": {"location": [111, 0, 0], "rotation": [0, 0, 0]},
                                                          "28675": {"location": [300, 0, 0], "rotation": [0, 0, 0], "scale": [2, 2, 2]}}},
                                         "/g/c/TEKI": {"remove": ["28929"]}}}])");
    plain("change.c", "1.0.0", map + R"({"/G/C/Objects": {"move": {"28673": {"location": [5, 5, 5], "rotation": [0, 0, 0]}}, "remove": ["99999"]}}}])");
    plain("change.d", "1.0.0", map + R"({"/G/C/Missing": {"remove": ["1"]}}}])");
    plain("change.e", "1.0.0", map + R"({"/G/C/Teki": {"move": {"28929": {"location": [9, 9, 9], "rotation": [0, 0, 0]}}}}, "mode": "night"}])");
    // Written wrong: a row named by a number, a row that is no number, a list both moved and removed, a key it
    // does not know, a list named twice, and an object's path.
    plain("change.number", "1.0.0", map + R"({"/G/C/Objects": {"remove": [28674]}}}])");
    plain("change.word", "1.0.0", map + R"({"/G/C/Objects": {"move": {"pot": {"location": [0, 0, 0], "rotation": [0, 0, 0]}}}}}])");
    plain("change.both", "1.0.0", map + R"({"/G/C/Objects": {"move": {"28673": {"location": [0, 0, 0], "rotation": [0, 0, 0]}}, "remove": ["28673"]}}}])");
    plain("change.key", "1.0.0", map + R"({"/G/C/Objects": {"set": {}}}}])");
    plain("change.twice", "1.0.0", map + R"({"/G/C/Objects": {"remove": ["1"]}, "/g/c/objects": {"remove": ["2"]}}}])");
    plain("change.dot", "1.0.0", map + R"({"/G/C/Objects.Objects": {"remove": ["1"]}}}])");
    const char* const changers[] = {"change.a", "change.b", "change.c", "change.d", "change.e", "change.number", "change.word", "change.both", "change.key", "change.twice", "change.dot"};
    CHECK(tsuchi::find_mods() == TSUCHI_OK);
    CHECK(problem("change.number") == "mod.json: \"maps\": \"placement_changes\": /G/C/Objects: \"remove\": a value is no row: a row is named by its placement number, in decimal digits and as text, such as \"17179869271\"");
    CHECK(problem("change.word") == "mod.json: \"maps\": \"placement_changes\": /G/C/Objects: \"move\": pot is no row: a row is named by its placement number, in decimal digits and as text, such as \"17179869271\"");
    CHECK(problem("change.both") == "mod.json: \"maps\": \"placement_changes\": /G/C/Objects both moves and removes row 28673");
    CHECK(problem("change.key") == "mod.json: \"maps\": \"placement_changes\" holds the game's placement lists by the path of each package, each with rows to \"move\" and rows to \"remove\"");
    CHECK(problem("change.twice") == "mod.json: \"maps\": \"placement_changes\" names /g/c/objects twice");
    CHECK(problem("change.dot") == std::string("mod.json: \"maps\": \"placement_changes\": /G/C/Objects.Objects") + tsuchi::content_path::kNoPackage);
    tsuchi::start_mods();
    CHECK(problem("change.a") == "<running>" && problem("change.b") == "<running>" && problem("change.c") == "<running>" && problem("change.d") == "<running>" &&
          problem("change.e") == "<running>");

    // The game reads the lists as the mods say: row 7001 where the later mod put it, 7002 and 7101 not at all,
    // 7003 moved. The asset is as it was.
    const tsuchi::PlacementChangeCounts before_changes = tsuchi::placement_change_counts();
    CHECK(build_now() == "build: 7001@111/111 7003@300/300 " && unchanged());
    const tsuchi::PlacementChangeCounts after_changes = tsuchi::placement_change_counts();
    CHECK(after_changes.lists_changed == before_changes.lists_changed + 2 && after_changes.rows_moved == before_changes.rows_moved + 2 &&
          after_changes.rows_removed == before_changes.rows_removed + 2 && after_changes.lists_refused == before_changes.lists_refused + 1);
    CHECK(told("[change.a] maps: /G/C/Objects, row 28673: change.b changes it too, and the game uses that change, as that mod started later\n") &&
          told("[change.b] maps: /G/C/Objects, row 28673: this mod's change is used in place of change.a's, as this mod started later\n") &&
          told("[change.a] maps: /G/C/Objects, row 28674 was removed as the game read the list for /G/Cave_P\n") &&
          told("[change.b] maps: /G/C/Objects, row 28673 was moved as the game read the list for /G/Cave_P\n") &&
          told("[change.b] maps: /G/C/Objects, row 28675 was moved as the game read the list for /G/Cave_P\n") &&
          told("[change.b] maps: /G/C/Teki, row 28929 was removed as the game read the list for /G/Cave_P\n"));
    CHECK(told("[change.c] maps: the placement list /G/C/Objects has no row 99999 as the game reads it for /G/Cave_P, so none of this mod's changes to the list were made\n") &&
          !told("[change.c] maps: /G/C/Objects, row") && !told("change.c changes it too"));
    CHECK(told("[change.d] maps: /G/Cave_P has no placement list /G/C/Missing, so the changes to it were not made\n") && !told("[change.e]"));
    // The read itself, as the game's reader has it: the moved row's transforms, both, with its turn and its scale.
    {
        CHECK(fake::get<int32_t>(objects + kRowsAt + t::array::count) == 3);
        uint64_t seen = 0;
        float turn[4] = {}, scale[3] = {}, place[3] = {};
        const auto look = [&](void*, void*, void* lists) {
            const uint64_t held = fake::get<uint64_t>(fake::get<uint64_t>(reinterpret_cast<uint64_t>(lists) + t::array::data));
            const uint64_t list = fake::get<uint64_t>(fake::get<uint64_t>(held + list_reads.held_at + t::array::data));
            const uint64_t row = fake::get<uint64_t>(list + kRowsAt);
            seen = fake::get<uint64_t>(row + g::placement);
            for (int i = 0; i < 4; ++i) turn[i] = fake::get<float>(row + g::transform + x::rotation + 4 * i);
            for (int i = 0; i < 3; ++i) scale[i] = fake::get<float>(row + g::init_transform + x::scale + 4 * i), place[i] = fake::get<float>(row + g::init_transform + x::translation + 4 * i);
            CHECK(fake::get<int32_t>(list + kRowsAt + t::array::count) == 2 && fake::get<uint64_t>(row + g::size + g::placement) == 0x7003 &&
                  fake::get<float>(row + g::size + g::transform + x::scale) == 2 && fake::get<float>(row + g::size + g::init_transform + x::scale + 8) == 2);
        };
        list_reads.peek = [&](uint64_t lists) { look(nullptr, nullptr, reinterpret_cast<void*>(lists)); };
        build_now();
        list_reads.peek = {};
        CHECK(seen == 0x7001 && place[0] == 111 && place[1] == 0 && scale[0] == 1 && turn[2] == 0 && turn[3] > 0.99999f);
        CHECK(unchanged());
    }
    // A mod earlier in the order declares a change to the same row while the game runs, after the later mod's
    // change was declared: the later mod's change is still the one the game reads.
    {
        TSUCHI_Mod* earlier = tsuchi::mod_create("change.early", 1);
        CHECK(earlier && tsuchi::mod_rank(earlier) < tsuchi::mod_rank(tsuchi::find_mod("change.b")->context));
        const char* declared = R"([{"map": "/G/Cave_P", "placement_changes": {"/G/C/Objects": {"move": {"28675": {"location": [222, 0, 0], "rotation": [0, 0, 0]}}}}}])";
        const tsuchi::Json* section = tsuchi::read_json(declared, std::strlen(declared));
        tsuchi::Text wrong;
        CHECK(section && tsuchi::manifest_part("maps", earlier, *section, wrong, true) == TSUCHI_OK);
        wrong.clear();
        CHECK(build_now() == "build: 7001@111/111 7003@300/300 " && unchanged());
        CHECK(told("[change.early] maps: /G/C/Objects, row 28675: change.b changes it too, and the game uses that change, as that mod started later\n"));
        tsuchi::mod_stop(earlier);
        CHECK(build_now() == "build: 7001@111/111 7003@300/300 " && !told("[change.early]"));
        tsuchi::release_json(section);
    }
    // As the game makes the map from a save, the lists it reads are set out the same way, and each mod's log
    // says that the save keeps its own record.
    CHECK(restore_now(true) == "restore: 7001@111/111 7003@300/300 " && unchanged());
    CHECK(told("[change.a] maps: the game made /G/Cave_P from the save's record of the objects placed there, which keeps each where the save has it: "
               "changes to its placement lists reach a save that has not been to /G/Cave_P yet\n") && !told("[change.e]"));
    CHECK(restore_now(false) == "restore: 7001@111/111 7003@300/300 " && !told("from the save's record"));
    // A map loaded from the title screen: nothing in it plays yet and it is not the loaded world, so the story's
    // own state gives the mode. When that cannot be read either, no change is made, and the logs say so.
    {
        const uint64_t old_story = fake::get<uint64_t>(running.instance + t::game_instance::story_save), story = fake::memory(0x400);
        fake::put(running.instance + t::game_instance::story_save, story);
        fake::put<uint8_t>(playing + t::actor_play::state, 0);
        enter(far);
        CHECK(build_now() == "build: 7001@111/111 7003@300/300 " && !told("mode could not be read"));
        fake::put<uint64_t>(running.instance + t::game_instance::story_save, 0);
        CHECK(build_now() == "build: 7001@10/10 7002@20/20 7003@30/30 7101@40/40 " && unchanged() &&
              told("[change.a] maps: the game's mode could not be read as the game read the placement lists of /G/Cave_P, so the changes to them were not made\n"));
        fake::put(running.instance + t::game_instance::story_save, old_story);
        fake::put<uint8_t>(playing + t::actor_play::state, uint8_t(t::constant::actor_play_state_mask));
        enter(cave);
    }

    // A mod that stops: its changes are not made again, and the other mods' are. Row 7001 is where the first mod
    // puts it, 7003 back where its list has it, and 7101 is read again.
    tsuchi::mod_stop(tsuchi::find_mod("change.b")->context);
    CHECK(build_now() == "build: 7001@100/100 7003@30/30 7101@40/40 " && unchanged() && !told("[change.a] maps: /G/C/Objects, row 28673: "));
    // In the night, only the night entry's.
    fake::put<uint8_t>(clock + t::clock::night, 1);
    CHECK(build_now() == "build: 7001@10/10 7002@20/20 7003@30/30 7101@9/9 " && unchanged() && !told("[change.a]"));
    fake::put<uint8_t>(clock + t::clock::night, 0);
    tsuchi::forget_mods();
    CHECK(build_now() == "build: 7001@10/10 7002@20/20 7003@30/30 7101@40/40 " && unchanged() && logged.lines.empty());
    for (const char* id : changers) disk.files.erase(mods_at + id + "/mod.json");
    keeper = keeper_of(0, true);
}
