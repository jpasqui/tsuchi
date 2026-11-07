// Additional placement checks, included inside the existing placement fixture block.
{
    TSUCHI_Mod* probe_mod = tsuchi::mod_create("place.probe");
    CHECK(probe_mod && tsuchi::game::object_kept(pot) && tsuchi::game::object_kept(bug)); // declaring mods stopped; generators keep their classes
    constexpr uint64_t probe_part = 0x701;
    const char* probe_text = R"({"map":"/G/Probe_P","placements":["/G/P/Probe"]})";
    const tsuchi::Json* probe_entry = tsuchi::read_json(probe_text, std::strlen(probe_text));
    CHECK(probe_entry && tsuchi::placements_begin(probe_part, probe_mod, *probe_entry));
    const auto fresh_probe = [&] {
        keeper = keeper_of(0, true);
        const uint64_t probe_world = world_in(u"/G/Probe_P", u"Probe_P");
        enter(probe_world);
        placing.short_of = placing.grow_of = placing.moved_of = placing.bad_start_of = 0;
        placing.grow_refused = false;
        placing.start_callback = {};
        placing.log.clear(), logged.lines.clear();
        return tsuchi::game::to_handle(probe_world);
    };
    const auto look_probe = [&](uint64_t world) { tsuchi::placements_look(world, "/g/probe_p", [](uint64_t) { return false; }); };
    const auto number_is_there = [&](uint64_t number) {
        for (int32_t i = 0; i < generators_of(keeper); ++i)
            if (fake::get<uint64_t>(generator_at(keeper, i) + g::placement) == number) return true;
        return false;
    };
    const auto probe_list = [&](uint64_t number, uint32_t amount) {
        return list_of(u"/G/P/Probe", u"Probe", {{0, u"", 0, 0}, {number, u"/G/Pot.Pot_C", amount, 60}});
    };
    // Deep copy the authored serialization; relocating the outer buffer must preserve its private allocation.
    const uint64_t source_asset = probe_list(0x6101, 2);
    const uint64_t source_rows = fake::get<uint64_t>(source_asset + kRowsAt), source_data = fake::get<uint64_t>(source_rows + g::size + kRowBytesAt);
    uint64_t probe_world = fresh_probe();
    placing.grow_of = 0x6101;
    look_probe(probe_world);
    const uint64_t kept_data = generators_of(keeper) > 2 && number_is_there(0x6101) ? fake::get<uint64_t>(generator_at(keeper, 2) + kRowBytesAt) : 0;
    CHECK(placing.copied_independently && tsuchi::game::readable(kept_data, 1) && kept_data != source_data && fake::get<uint8_t>(kept_data) == 0xA1 &&
          fake::get<uint8_t>(source_data) == 0xA1 && generators_of(keeper) == 5 && number_is_there(0x6101) && number_is_there(0xFA001));
    CHECK(generators_of(keeper) == 5 && number_is_there(0xFA001) && fake::get<int32_t>(generator_at(keeper, 3) + g::index) == 3 && placing.log.find("spawn:6101 grow start start") != std::string::npos);

    // Refuse zero/oversized/already-populated authored counts before any append.
    for (uint32_t amount : {0u, 4097u}) {
        probe_list(0x6102, amount);
        probe_world = fresh_probe();
        look_probe(probe_world);
        CHECK(generators_of(keeper) == 2 && placing.log.empty() && told("it asks for no actors, for too many, or has made some already"));
    }
    const uint64_t populated = probe_list(0x6102, 1), populated_rows = fake::get<uint64_t>(populated + kRowsAt);
    fake::put<int32_t>(populated_rows + g::size + g::actors + t::array::count, 1);
    probe_world = fresh_probe();
    look_probe(probe_world);
    CHECK(generators_of(keeper) == 2 && placing.log.empty() && told("has made some already"));

    // A clock can select day mode while its native day method is unavailable.
    probe_list(0x6103, 1);
    probe_world = fresh_probe();
    fake::put<uint64_t>(clock_table + t::clock_table::day, 0);
    look_probe(probe_world);
    CHECK(generators_of(keeper) == 2 && placing.log.empty() && told("the map has no clock to take the day from"));
    fake::put(clock_table + t::clock_table::day, &game_day);
    probe_world = fresh_probe();
    placing.grow_refused = true;
    look_probe(probe_world);
    CHECK(generators_of(keeper) == 2 && placing.log == "no-grow " && told("there is no memory for one more"));

    // A partial row is fully removed, including started and unstarted actors. Save/revisit can then retry it.
    const uint64_t partial_asset = probe_list(0x6104, 3), partial_rows = fake::get<uint64_t>(partial_asset + kRowsAt);
    probe_world = fresh_probe();
    placing.short_of = placing.grow_of = 0x6104;
    const size_t made_before = placing.made.size();
    const int destroyed_before = placing.destroyed;
    look_probe(probe_world);
    CHECK(!number_is_there(0x6104) && number_is_there(0xFA001) && generators_of(keeper) == 3 && placing.destroyed == destroyed_before + 1);
    CHECK(generators_of(keeper) == 3 && (number_is_there(0xFA001) || number_is_there(0xFA003)) && fake::get<int32_t>(generator_at(keeper, 2) + g::index) == 2);
    CHECK(placing.destroyed_bytes && placing.destroyed_actors && !tsuchi::game::readable(placing.destroyed_bytes, 1) &&
          !tsuchi::game::readable(placing.destroyed_actors, 8) && fake::get<int32_t>(partial_rows + g::size + g::actors + t::array::count) == 0 &&
          fake::get<uint8_t>(fake::get<uint64_t>(partial_rows + g::size + kRowBytesAt)) == 0xA1);
    for (size_t i = made_before; i < placing.made.size(); ++i) CHECK(!tsuchi::game::live(placing.made[i]));
    const uint64_t saved_probe = keeper;
    probe_world = fresh_probe();
    keeper = keeper_of(saved_probe, true);
    look_probe(probe_world);
    CHECK(number_is_there(0x6104) && number_is_there(0xFA001) && generators_of(keeper) == 5 && placing.log.find("spawn:6104 start start start") != std::string::npos);

    // Failure halfway through actor initialization must remove every produced actor and leave the source alone.
    probe_list(0x6105, 3);
    probe_world = fresh_probe();
    placing.bad_start_of = 0x6105;
    const size_t start_before = placing.made.size();
    look_probe(probe_world);
    CHECK(generators_of(keeper) == 2 && !number_is_there(0x6105) && told("the game made fewer actors"));
    for (size_t i = start_before; i < placing.made.size(); ++i) CHECK(!tsuchi::game::live(placing.made[i]));

    // A callback inserts a foreign row before ours after a grow. The old index is no longer ours; remove by identity.
    probe_list(0x6106, 2);
    probe_world = fresh_probe();
    placing.moved_of = 0x6106;
    const size_t moved_before = placing.made.size();
    look_probe(probe_world);
    CHECK(generators_of(keeper) == 3 && !number_is_there(0x6106) && number_is_there(0xFA001) && told("the game did not keep its generator"));
    for (size_t i = moved_before; i < placing.made.size(); ++i) CHECK(!tsuchi::game::live(placing.made[i]));

    // A start callback can move the generator or remove it using its native destruction path. The caller's
    // actor snapshot must still clean up all produced actors, including the ones that have not started yet.
    for (bool removed : {false, true}) {
        const uint64_t number = removed ? 0x6109 : 0x6108;
        probe_list(number, 3);
        probe_world = fresh_probe();
        const size_t before_start = placing.made.size();
        bool start_callback = false;
        placing.start_callback = [&](uint64_t) {
            if (start_callback) return;
            start_callback = true;
            const int32_t before = generators_of(keeper);
            const uint64_t array = keeper + k::generators;
            fake::put<int32_t>(array + t::array::count, before + 1), fake::put<int32_t>(array + t::array::max, before);
            game_generator_array_grow(reinterpret_cast<void*>(array));
            const uint64_t own = generator_at(keeper, before - 1), foreign = generator_at(keeper, before);
            fake::put<uint64_t>(foreign + g::placement, 0xFA003), fake::put<int32_t>(foreign + g::index, before);
            uint8_t detached[g::most];
            std::memcpy(detached, reinterpret_cast<void*>(own), g::size);
            std::memcpy(reinterpret_cast<void*>(own), reinterpret_cast<void*>(foreign), g::size);
            fake::put<int32_t>(own + g::index, before - 1);
            if (removed) {
                std::memset(reinterpret_cast<void*>(foreign), 0, g::size);
                fake::put<int32_t>(array + t::array::count, before);
                game_generator_destroy(detached);
            } else {
                std::memcpy(reinterpret_cast<void*>(foreign), detached, g::size);
                fake::put<int32_t>(foreign + g::index, before);
            }
        };
        look_probe(probe_world);
        CHECK(start_callback && generators_of(keeper) == 3 && !number_is_there(number) && number_is_there(0xFA003) && told("the game did not keep its generator"));
        for (size_t i = before_start; i < placing.made.size(); ++i) CHECK(!tsuchi::game::live(placing.made[i]));
        CHECK(generators_of(keeper) == 3 && (number_is_there(0xFA001) || number_is_there(0xFA003)) && fake::get<int32_t>(generator_at(keeper, 2) + g::index) == 2);
        placing.start_callback = {};
    }
    // Retirement itself can append/reallocate generators and recycle another produced actor. Keep the foreign
    // row, and use the snapshot's life handles so the recycled actor is not destroyed under its former identity.
    probe_list(0x6107, 3);
    probe_world = fresh_probe();
    placing.short_of = 0x6107;
    uint64_t recycled = 0;
    bool retirement_callback = false;
    fake::function(w.actor_class, function_type, u"K2_DestroyActor", 0, 0, [&](uint64_t object, uint64_t) {
        if (!retirement_callback) {
            retirement_callback = true;
            recycled = placing.made.back();
            tsuchi::game::end_life(recycled);
            fake::put<uint64_t>(recycled + t::game_actor::placement, 0xFA002);
            const uint64_t array = keeper + k::generators;
            const int32_t before = generators_of(keeper);
            fake::put<int32_t>(array + t::array::count, before + 1), fake::put<int32_t>(array + t::array::max, before);
            game_generator_array_grow(reinterpret_cast<void*>(array));
            const uint64_t foreign = generator_at(keeper, before), pointers = fake::memory(8);
            fake::put(pointers, recycled);
            fake::put<uint64_t>(foreign + g::placement, 0xFA002), fake::put(foreign + g::class_, pot), fake::put<int32_t>(foreign + g::index, before);
            fake::put(foreign + g::actors, pointers), fake::put<int32_t>(foreign + g::actors + t::array::count, 1), fake::put<int32_t>(foreign + g::actors + t::array::max, 1);
        }
        fake::destroy(object);
    });
    look_probe(probe_world);
    CHECK(retirement_callback && recycled && tsuchi::game::live(recycled) && tsuchi::game::actor_placement(recycled) == 0xFA002 &&
          generators_of(keeper) == 3 && !number_is_there(0x6107) && number_is_there(0xFA002));
    CHECK(retirement_callback && recycled && generators_of(keeper) == 3 && number_is_there(0xFA002) && !number_is_there(0x6107) &&
          fake::get<int32_t>(generator_at(keeper, 2) + g::index) == 2);
    CHECK(retirement_callback && recycled && generators_of(keeper) == 3 && number_is_there(0xFA002) && !number_is_there(0x6107) &&
          fake::get<uint64_t>(generator_at(keeper, 2) + g::placement) == 0xFA002 &&
          tsuchi::game::readable(fake::get<uint64_t>(generator_at(keeper, 2) + g::actors), 8) &&
          fake::get<uint64_t>(fake::get<uint64_t>(generator_at(keeper, 2) + g::actors)) == recycled);
    fake::function(w.actor_class, function_type, u"K2_DestroyActor", 0, 0, [](uint64_t object, uint64_t) { fake::destroy(object); });
    // The same package at night gets no default story objects. An explicitly selected night list does.
    probe_list(0x6201, 1);
    probe_world = fresh_probe();
    fake::put<uint8_t>(clock + t::clock::night, 1);
    look_probe(probe_world);
    CHECK(generators_of(keeper) == 2 && placing.log.empty());
    tsuchi::placements_end(probe_part);
    const char* night_text = R"({"map":"/G/Probe_P","mode":"night","placements":["/G/P/Probe"]})";
    const tsuchi::Json* night_entry = tsuchi::read_json(night_text, std::strlen(night_text));
    CHECK(night_entry && tsuchi::placements_begin(probe_part, probe_mod, *night_entry));
    probe_world = fresh_probe();
    look_probe(probe_world);
    CHECK(generators_of(keeper) == 4 && number_is_there(0x6201));
    fake::put<uint8_t>(clock + t::clock::night, 0);
    probe_world = fresh_probe();
    look_probe(probe_world);
    CHECK(generators_of(keeper) == 2 && placing.log.empty());
    CHECK(tsuchi::placement_mode_matches(tsuchi::PlacementMode::story));
    t::fn::story_flags = [](void*) -> uint64_t { return t::constant::story_of_olimar; };
    CHECK(!tsuchi::placement_mode_matches(tsuchi::PlacementMode::story) && !tsuchi::placement_mode_matches(tsuchi::PlacementMode::night) &&
          tsuchi::placement_mode_matches(tsuchi::PlacementMode::olimar));
    t::fn::story_flags = [](void*) -> uint64_t { return 0; };
    fake::put<uint8_t>(running.context + t::game_context::rule, uint8_t(t::tables::story_rules[1]));
    CHECK(tsuchi::placement_mode_matches(tsuchi::PlacementMode::story));
    // Another known story rule still admits the selected night list.
    fake::put<uint8_t>(clock + t::clock::night, 1);
    probe_world = fresh_probe();
    look_probe(probe_world);
    CHECK(generators_of(keeper) == 4 && number_is_there(0x6201));
    // An unreadable clock must not consume this world's one chance to give its lists.
    probe_world = fresh_probe();
    fake::put<uint64_t>(w.framework + t::game_framework::clock, 0);
    look_probe(probe_world);
    CHECK(generators_of(keeper) == 2 && placing.log.empty());
    fake::put(w.framework + t::game_framework::clock, clock);
    look_probe(probe_world);
    CHECK(generators_of(keeper) == 4 && number_is_there(0x6201));
    fake::put<uint8_t>(clock + t::clock::night, 0);
    fake::put<uint8_t>(running.context + t::game_context::rule, uint8_t(t::constant::rule_story));
    tsuchi::placements_end(probe_part);
    tsuchi::release_json(probe_entry), tsuchi::release_json(night_entry);

    // A list of placement lists, as the game names its own: each list it holds is given in turn, by the path of
    // its own package and with its own mark. A list has the same mark whether the mod names it or the list of
    // lists that holds it, so a save given one is given nothing of the other.
    {
        constexpr int32_t kHeldAt = 0x40;
        const uint64_t set_type = fake::object(u"PlaceListSet", w.root);
        fake::field(set_type, u"AssetList", u"ArrayProperty", kHeldAt, 0x10, fake::loose_field(u"ObjectProperty", 8, list_type));
        const auto set_of = [&](const std::u16string& package, const std::u16string& name, std::vector<uint64_t> held) {
            const uint64_t asset = fake::object(name, set_type, 0x80), data = fake::memory(held.size() * 8 + 8);
            for (size_t i = 0; i < held.size(); ++i) fake::put(data + i * 8, held[i]);
            fake::put(asset + kHeldAt + t::array::data, data);
            fake::put<int32_t>(asset + kHeldAt + t::array::count, int32_t(held.size())), fake::put<int32_t>(asset + kHeldAt + t::array::max, int32_t(held.size()));
            shelve(package + u"." + name, asset);
            return asset;
        };
        const auto packaged = [&](const std::u16string& package, const std::u16string& name, std::vector<Row> rows) {
            const uint64_t asset = list_of(package, name, rows);
            fake::put(asset + t::object::outer, fake::object(package, w.root, 0x40));
            return asset;
        };
        const uint64_t inner = packaged(u"/G/L/Inner", u"Inner", {{0, u"", 0, 0}, {0x6301, u"/G/Pot.Pot_C", 1, 70}});
        const uint64_t second = packaged(u"/G/L/Second", u"Second", {{0, u"", 0, 0}, {0x6302, u"/G/Pot.Pot_C", 1, 80}});
        const uint64_t unmarked = packaged(u"/G/L/Unmarked", u"Unmarked", {{0x6303, u"/G/Pot.Pot_C", 1, 0}});
        const uint64_t twin = list_of(u"/G/L/Twin", u"Twin", {{0, u"", 0, 0}, {0x6304, u"/G/Pot.Pot_C", 1, 0}});
        fake::put(twin + t::object::outer, fake::get<uint64_t>(inner + t::object::outer)); // in Inner's package
        set_of(u"/G/L/Set", u"Set", {inner});
        set_of(u"/G/L/Both", u"Both", {inner, second, unmarked, 0, twin});
        set_of(u"/G/L/Empty", u"Empty", {});
        const auto give = [&](const char* list, uint64_t saved = 0) {
            const std::string text = std::string(R"({"map":"/G/Probe_P","placements":[")") + list + "\"]}";
            const tsuchi::Json* entry = tsuchi::read_json(text.data(), text.size());
            CHECK(entry && tsuchi::placements_begin(probe_part, probe_mod, *entry));
            const uint64_t world = fresh_probe();
            if (saved) keeper = keeper_of(saved, true);
            look_probe(world);
            tsuchi::placements_end(probe_part);
            tsuchi::release_json(entry);
        };
        const auto mark_at = [&](int32_t place) { return fake::get<uint64_t>(generator_at(keeper, place) + g::placement); };

        // One list held: its row, then its mark.
        tsuchi::PlacementCounts was = tsuchi::map_placement_counts();
        give("/G/L/Set");
        CHECK(placing.log == "grow copy spawn:6301 start copy " && generators_of(keeper) == 4 && number_is_there(0x6301) &&
              tsuchi::map_placement_counts().lists_given == was.lists_given + 1 && logged.lines.empty());
        const uint64_t inner_mark = mark_at(3), saved_set = keeper;
        CHECK(inner_mark && fake::get<uint64_t>(generator_at(keeper, 3) + g::class_) == 0);
        // Named by itself, the list has the same mark, so the save that had it through the list of lists is given
        // nothing of it, and the other way round.
        give("/G/L/Inner");
        CHECK(generators_of(keeper) == 4 && mark_at(3) == inner_mark);
        const uint64_t saved_inner = keeper;
        was = tsuchi::map_placement_counts();
        give("/G/L/Inner", saved_set);
        CHECK(placing.log.empty() && generators_of(keeper) == 4 && tsuchi::map_placement_counts().lists_known == was.lists_known + 1);
        give("/G/L/Set", saved_inner);
        CHECK(placing.log.empty() && generators_of(keeper) == 4 && tsuchi::map_placement_counts().lists_known == was.lists_known + 2);

        // Several lists held: each with its own mark. One with no mark, an empty place, and one that shares
        // another's package are told and passed over.
        was = tsuchi::map_placement_counts();
        give("/G/L/Both");
        CHECK(placing.log == "grow copy spawn:6301 start copy copy spawn:6302 start grow copy " && generators_of(keeper) == 6 && number_is_there(0x6301) &&
              number_is_there(0x6302) && !number_is_there(0x6303) && !number_is_there(0x6304));
        CHECK(mark_at(3) == inner_mark && mark_at(5) && mark_at(5) != inner_mark && mark_at(5) != ~uint64_t(0));
        CHECK(told("[place.probe] maps: the placement list /G/L/Unmarked was not given to /g/probe_p: it has no mark: its first row has to name no class\n") &&
              told("[place.probe] maps: the placement list /G/L/Both was not given to /g/probe_p: its AssetList[3] holds no placement list\n") &&
              told("[place.probe] maps: the placement list /G/L/Both was not given to /g/probe_p: its AssetList[4] is in the package of an earlier one: each placement list needs a package of its own\n"));
        const tsuchi::PlacementCounts now = tsuchi::map_placement_counts();
        CHECK(now.lists_given == was.lists_given + 2 && now.lists_refused == was.lists_refused + 3 && now.rows_given == was.rows_given + 2);

        // A list of lists that holds none.
        give("/G/L/Empty");
        CHECK(placing.log.empty() && generators_of(keeper) == 2 &&
              told("[place.probe] maps: the placement list /G/L/Empty was not given to /g/probe_p: its AssetList holds no placement lists, or more than 64\n"));

        // A list declared after the world's lists were dealt with is given in that world. The one given before
        // is not given again.
        {
            const char* first_text = R"({"map":"/G/Probe_P","placements":["/G/L/Inner"]})";
            const char* later_text = R"({"map":"/G/Probe_P","placements":["/G/L/Second"]})";
            const tsuchi::Json* first_entry = tsuchi::read_json(first_text, std::strlen(first_text));
            const tsuchi::Json* later_entry = tsuchi::read_json(later_text, std::strlen(later_text));
            CHECK(first_entry && later_entry && tsuchi::placements_begin(probe_part, probe_mod, *first_entry));
            const uint64_t world = fresh_probe();
            look_probe(world);
            CHECK(placing.log == "grow copy spawn:6301 start copy " && generators_of(keeper) == 4);
            placing.log.clear();
            CHECK(tsuchi::placements_begin(probe_part + 1, probe_mod, *later_entry));
            look_probe(world);
            CHECK(placing.log.find("spawn:6302") != std::string::npos && placing.log.find("spawn:6301") == std::string::npos &&
                  generators_of(keeper) == 6 && number_is_there(0x6302));
            placing.log.clear();
            look_probe(world);
            CHECK(placing.log.empty() && generators_of(keeper) == 6);
            tsuchi::placements_end(probe_part + 1);
            tsuchi::placements_end(probe_part);
            tsuchi::release_json(later_entry);
            tsuchi::release_json(first_entry);
        }
    }

    for (const char* invalid : {R"({"mode":"all"})", R"({"mode":["story"]})", R"({"placements":["/Game/Mods/dotted.id/List"]})", R"({"placements":["/Game//List"]})"}) {
        const tsuchi::Json* entry = tsuchi::read_json(invalid, std::strlen(invalid));
        tsuchi::Text wrong;
        CHECK(entry && !tsuchi::placements_well_formed(*entry, wrong) && wrong.get()[0]);
        tsuchi::release_json(entry);
    }
    tsuchi::mod_stop(probe_mod);
    CHECK(tsuchi::game::object_kept(pot));
}