// Included inside mapping()'s AI fixture, after the original AI mods have stopped.
// These are stand-in checks; the reader's real internal state behavior remains unproved.
{
    // The AI file uses the same path grammar as the manifest. A folder cannot contain a dot, and an
    // object suffix belongs after the final slash. Case of valid names is covered by mind.two above.
    const auto read_file = [&](const std::string& map, const std::string& type) {
        const std::string bytes = file_of(map, {{0x7001, type, 3, {{tsuchi::ai::kTerritory, ai_territory(12)}}}});
        const char* named = nullptr;
        uint32_t at = 0, count = 0;
        tsuchi::ai::Read result = tsuchi::ai::read_head(reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size(), named, at, count);
        tsuchi::ai::Placement placement{};
        return result == tsuchi::ai::Read::ok ? tsuchi::ai::read_placement(reinterpret_cast<const uint8_t*>(bytes.data()), uint32_t(bytes.size()), at, placement) : result;
    };
    CHECK(read_file("/g/den_p", "/g/baby.baby_c") == tsuchi::ai::Read::ok);
    CHECK(read_file("/G/folder.with_dot/Den_P", "/G/Baby.Baby_C") == tsuchi::ai::Read::bad_path);
    CHECK(read_file("/G/Den_P", "/G/folder.with_dot/Baby.Baby_C") == tsuchi::ai::Read::bad_path);
    CHECK(read_file("/G/Den_P", "/G/Baby.Baby.Baby_C") == tsuchi::ai::Read::bad_path);
    CHECK(read_file("/G/Den_P", "/G/Baby") == tsuchi::ai::Read::no_object);
    CHECK(read_file("/G/Den_P.Den_P", "/G/Baby.Baby_C") == tsuchi::ai::Read::no_package);
    CHECK(read_file("/G//Den_P", "/G/Baby.Baby_C") == tsuchi::ai::Read::bad_path);

    // Remove the old manifests so the next discovery starts only this fixture's AI mods.
    for (const char* id : made) disk.files.erase(mods_at + id + "/mod.json");
    static ClassRef stream_ref{"LevelStreamingDynamic"};
    const uint64_t stream_type = find_class(stream_ref);
    CHECK(stream_type);
    const uint64_t kIn = t::constant::property_param, kOut = kIn | t::constant::property_out, kResult = kOut | t::constant::property_return;
    bool visible = false;
    uint64_t waiting_stream = 0;
    const uint64_t load = fake::function(stream_type, function_type, u"LoadLevelInstance", 0x28, 8, [&](uint64_t, uint64_t arguments) {
        std::string path;
        const uint64_t data = fake::get<uint64_t>(arguments + 8 + t::array::data);
        for (int32_t i = 0; i + 1 < fake::get<int32_t>(arguments + 8 + t::array::count); ++i) path += char(fake::get<uint16_t>(data + 2 * i));
        const bool accepted = path == "/G/Mods/AiWaiting";
        const uint64_t stream = accepted ? fake::engine_object(u"AiStream", stream_type) : 0;
        if (stream) waiting_stream = stream;
        fake::put<uint8_t>(arguments + 0x18, accepted), fake::put(arguments + 0x20, stream);
    }, t::constant::function_static);
    fake::flag(fake::field(load, u"WorldContextObject", u"ObjectProperty", 0, 8, world_type), kIn);
    fake::flag(fake::field(load, u"LevelName", u"StrProperty", 8, 0x10), kIn);
    fake::flag(fake::bit_field(load, u"bOutSuccess", 0x18, 0, 0xFF), kOut);
    fake::flag(fake::field(load, u"ReturnValue", u"ObjectProperty", 0x20, 8, stream_type), kResult);
    const uint64_t shown = fake::function(stream_type, function_type, u"IsLevelVisible", 1, 1,
                                        [&](uint64_t, uint64_t arguments) { fake::put<uint8_t>(arguments, visible); });
    fake::flag(fake::bit_field(shown, u"ReturnValue", 0, 0, 0xFF), kResult);
    const uint64_t unload = fake::function(stream_type, function_type, u"SetShouldBeLoaded", 1, 1, [](uint64_t, uint64_t) {});
    fake::flag(fake::bit_field(unload, u"bInShouldBeLoaded", 0, 0, 0xFF), kIn);
    const std::string guarded_file = file_of("/g/guard_p", {{0x7101, "/g/baby.baby_c", 3, {{tsuchi::ai::kTerritory, ai_territory(500)}}},
                                                         {0x7102, "/G/Baby.Baby_C", 3, {{tsuchi::ai::kTerritory, ai_territory(600)}}},
                                                         {0x7103, "/G/Bee.Bee_C", 1, {{tsuchi::ai::kDrops, drops(2, 5)}}}});
    const auto late_plain = [&](const std::string& id, const std::string& more) {
        plain(id, "1.0.0");
        disk.files[mods_at + id + "/main.lua"] = "content.add([==[{" + more.substr(1) + "}]==])\n";
    };
    late_plain("mind.guarded", R"(, "maps": [{"map": "/G/Guard_P", "levels": ["/G/Mods/AiWaiting"], "ai": "maps/den.ai"}])");
    late_plain("mind.failed", R"(, "maps": [{"map": "/G/Guard_P", "levels": ["/G/Mods/AiRefused"], "ai": "maps/den.ai"}])");
    disk.files[mods_at + "mind.guarded/maps/den.ai"] = guarded_file;
    disk.files[mods_at + "mind.failed/maps/den.ai"] = file_of("/G/Guard_P", {{0x7101, "/G/Baby.Baby_C", 3, {{tsuchi::ai::kTerritory, ai_territory(900)}}},
                                                                         {0x7104, "/G/Baby.Baby_C", 3, {{tsuchi::ai::kTerritory, ai_territory(900)}}}});
    CHECK(tsuchi::find_mods() == TSUCHI_OK);
    tsuchi::start_mods();
    CHECK(problem("mind.guarded") == "<running>" && problem("mind.failed") == "<running>");
    CHECK(tsuchi::map_parts() == 0); // these two live additions are queued by their actual main.lua
    frames(1); // apply the files before the path-validation probes temporarily replace one
    CHECK(tsuchi::map_parts() == 2);
    // Startup diagnostics carry the same path messages as other map declarations, including whether
    // this position wanted a package or an object. The invalid read never adds a running AI file.
    const char* path_entry_text = R"({"map":"/G/Guard_P","ai":"maps/den.ai"})";
    const tsuchi::Json* path_entry = tsuchi::read_json(path_entry_text, std::strlen(path_entry_text));
    CHECK(path_entry);
    if (path_entry) {
        const auto refused_path = [&](const std::string& map, const std::string& type, const char* message) {
            disk.files[mods_at + "mind.guarded/maps/den.ai"] = file_of(map, {{0x7101, type, 3, {{tsuchi::ai::kTerritory, ai_territory(10)}}}});
            tsuchi::Text diagnostic;
            const TSUCHI_Result result = tsuchi::ai_begin(0, tsuchi::find_mod("mind.guarded")->context, *path_entry, diagnostic);
            CHECK(result == TSUCHI_INVALID && std::string(diagnostic.get()) == std::string("\"maps\": maps/den.ai contains a path that") + message);
            diagnostic.clear();
        };
        refused_path("/G/folder.with_dot/Guard_P", "/G/Baby.Baby_C", tsuchi::content_path::kNoPath);
        refused_path("/G/Guard_P.Guard_P", "/G/Baby.Baby_C", tsuchi::content_path::kNoPackage);
        refused_path("/G/Guard_P", "/G/Baby", tsuchi::content_path::kNoObject);
        disk.files[mods_at + "mind.guarded/maps/den.ai"] = guarded_file;
        tsuchi::release_json(path_entry);
    }
    const uint64_t old_world = fake::get<uint64_t>(main + t::data::world);
    const uint8_t old_begun = fake::get<uint8_t>(w.framework + t::game_framework::world_begun);
    const uint64_t guard_level = level_in(u"/G/Guard_P", u"Guard_P"), guard_world = fake::get<uint64_t>(guard_level + kOwningAt);
    fake::put(main + t::data::world, guard_world), fake::put<uint8_t>(w.framework + t::game_framework::world_begun, 1);
    const uint32_t sets_before = tsuchi::map_ai_counts().set;
    const uint64_t first_waiting = creature(guard_level, u"Waiting", baby, 0x7101, 3);
    CHECK(starts(first_waiting) == "write read start:500/0 ");
    const uint64_t expired_waiting = creature(guard_level, u"ExpiredWaiting", baby, 0x7101, 3);
    CHECK(starts(expired_waiting) == "write read start:500/0 ");
    tsuchi::game::end_life(fake::get<uint64_t>(expired_waiting + t::scene_component::owner));
    frames(300);
    CHECK(waiting_stream && problem("mind.failed") == "<running>" && !tsuchi::find_mod("mind.failed")->problem.get()[0]);
    // This placement is named only by the failed part. No healthy later writer can hide an erroneous
    // application: neither the serializer's writer nor reader is called, and its stock radius remains.
    const uint64_t failed_only = creature(guard_level, u"FailedOnly", baby, 0x7104, 3);
    CHECK(starts(failed_only) == "start:100/0 " && tsuchi::map_ai_counts().set == sets_before + 2);
    const uint64_t second_waiting = creature(guard_level, u"StillWaiting", baby, 0x7101, 3);
    CHECK(starts(second_waiting) == "write read start:500/0 " && tsuchi::map_ai_counts().set == sets_before + 3);

    // A table outside the three supported ones never reaches an AI start hook. Its entered event queues
    // a diagnostic, but it is withheld while the part's level is waiting, and appears once it is ready.
    const uint64_t unsupported = creature(guard_level, u"Unsupported", baby, 0x7102, 3);
    const uint64_t unsupported_actor = fake::get<uint64_t>(unsupported + t::scene_component::owner);
    fake::put(unsupported, fake::memory(8)), fake::put(unsupported_actor, table_a());
    const uint32_t refused_before = tsuchi::map_ai_counts().refused;
    logged.lines.clear();
    registers(unsupported_actor);
    frames(1);
    CHECK(tsuchi::map_ai_counts().refused == refused_before && !told("has no supported kind of AI part"));
    tsuchi::game::Place loaded{};
    CHECK(tsuchi::game::find_place(tsuchi::game::type_of(waiting_stream), waiting_stream, "LoadedLevel", loaded) == TSUCHI_OK);
    if (loaded.address) fake::put(loaded.address, guard_level);
    visible = true;
    frames(15);
    // The values were already read before Start; level arrival does not read or start them again.
    float first_value = 0, second_value = 0;
    std::memcpy(&first_value, minds.set[first_waiting].data() + 12, 4), std::memcpy(&second_value, minds.set[second_waiting].data() + 12, 4);
    float expired_value = 0, failed_value = 0;
    std::memcpy(&expired_value, minds.set[expired_waiting].data() + 12, 4);
    std::memcpy(&failed_value, minds.set[failed_only].data() + 12, 4);
    CHECK(failed_value == 100);
    CHECK(first_value == 500 && second_value == 500 && expired_value == 500 && tsuchi::map_ai_counts().set == sets_before + 3);
    CHECK(starts(creature(guard_level, u"Ready", baby, 0x7101, 3)) == "write read start:500/0 " && tsuchi::map_ai_counts().set == sets_before + 4);
    CHECK(told("[mind.guarded] maps: the AI values for placement 0x0000000000007102 in /G/Guard_P were not set: the creature there has no supported kind of AI part\n") &&
          tsuchi::map_ai_counts().refused == refused_before + 1);
    logged.lines.clear();
    registers(unsupported_actor);
    frames(1);
    const uint64_t unsupported_again = creature(guard_level, u"UnsupportedAgain", baby, 0x7102, 3);
    const uint64_t unsupported_actor_again = fake::get<uint64_t>(unsupported_again + t::scene_component::owner);
    fake::put(unsupported_again, fake::memory(8)), fake::put(unsupported_actor_again, table_a());
    registers(unsupported_actor_again);
    frames(1);
    CHECK(!told("has no supported kind of AI part") && tsuchi::map_ai_counts().refused == refused_before + 1);
    ends_play(unsupported_actor), ends_play(unsupported_actor_again);

    // Shared geometry does not grant story-only AI edits to the night list.
    fake::put<uint8_t>(ai_clock + t::clock::night, 1);
    CHECK(starts(creature(guard_level, u"Night", baby, 0x7101, 3)) == "start:100/0 " && tsuchi::map_ai_counts().set == sets_before + 4);
    fake::put<uint8_t>(ai_clock + t::clock::night, 0);

    // Successful enum handles detect unloading. A missing enum is walked once, then cached across repeated
    // starts; a newly loaded enum becomes visible after the bounded sixty-frame retry interval.
    fake::destroy(condition_enum);
    const uint32_t walks = tsuchi::map_ai_counts().enum_walks;
    const uint64_t missing_drop = creature(guard_level, u"MissingDropEnum", bee, 0x7103, 1);
    CHECK(starts(missing_drop) == "start:100 " && tsuchi::map_ai_counts().enum_walks == walks + 1);
    CHECK(starts(missing_drop) == "start:100 " && tsuchi::map_ai_counts().enum_walks == walks + 1);
    enum_of(u"EDropCondition", {0, 1, 2});
    CHECK(starts(missing_drop) == "start:100 " && tsuchi::map_ai_counts().enum_walks == walks + 1);
    frames(60);
    CHECK(starts(missing_drop) == "write read start:100 " && tsuchi::map_ai_counts().enum_walks == walks + 2);
    tsuchi::forget_mods();
    fake::put(main + t::data::world, old_world), fake::put<uint8_t>(w.framework + t::game_framework::world_begun, old_begun);
    for (const char* id : {"mind.guarded", "mind.failed"}) disk.files.erase(mods_at + id + "/mod.json"), disk.files.erase(mods_at + id + "/main.lua"), disk.files.erase(mods_at + id + "/maps/den.ai");
    fake::destroy(load), fake::destroy(shown), fake::destroy(unload);
    if (waiting_stream) fake::destroy(waiting_stream);
}
