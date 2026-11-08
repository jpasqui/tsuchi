// Focused made-up level lifecycle. Native order is exercised through the hook
// service, without the SDK campaign or a running game.
struct EarlyObserved { float health, x, shape_x, shape_y; uint64_t mesh; int32_t mode; };
std::map<uint64_t, EarlyObserved> early_observed;
uint64_t early_actor_type = 0;
uint64_t early_flags = 0;
std::map<uint64_t, uint64_t> early_registered_mesh;
std::map<uint64_t, bool> early_registered_collision;
constexpr int32_t early_health = 0x80, early_x = 0x84, early_shape = 0x88, early_mesh = 0x90, early_mode = 0x98;
// References another actor may hold: directly, in an array, and in a struct.
constexpr int32_t early_linked = 0xA8, early_links = 0xB0, early_link = 0xC0;
int early_mobility_calls = 0;

void early_native_initialize(uint64_t level, int32_t budget, bool reconstruct, void* context) {
    CHECK(budget == 7 && !reconstruct && context == reinterpret_cast<void*>(0x1234));
    const uint64_t actors = fake::get<uint64_t>(level + t::engine_level::actors);
    const int32_t count = fake::get<int32_t>(level + t::engine_level::actors + t::array::count);
    for (int32_t i = 0; i < count; ++i) {
        const uint64_t actor = fake::get<uint64_t>(actors + uint64_t(i) * 8);
        if (!actor || !live(actor) || !derives_from(type_of(actor), early_actor_type)) continue;
        if (!early_registered_mesh.count(actor)) {
            const uint64_t part = fake::get<uint64_t>(actor + 0xA0);
            early_registered_mesh[actor] = fake::get<uint64_t>(part + early_mesh);
            early_registered_collision[actor] = fake::get<uint8_t>(part + 0x9C) != 0;
        }
    }
}

// As the game's actors do (AActor::ProcessEvent): every call is dropped until play has begun, without a word.
void early_actor_process(void* object, void* function, void* arguments) {
    if (!(fake::get<uint8_t>(reinterpret_cast<uint64_t>(object) + t::actor_play::state) & t::constant::actor_play_state_mask)) return;
    fake::engine_process(object, function, arguments);
}
uint64_t early_actor_vtable = 0;

void early_begin_level(uint64_t level) {
    const uint64_t actors = fake::get<uint64_t>(level + t::engine_level::actors);
    const int32_t count = fake::get<int32_t>(level + t::engine_level::actors + t::array::count);
    for (int32_t i = 0; i < count; ++i) {
        const uint64_t actor = fake::get<uint64_t>(actors + uint64_t(i) * 8);
        if (!actor || !live(actor) || !derives_from(type_of(actor), early_actor_type)) continue;
        CHECK(!(fake::get<uint8_t>(actor + t::actor_play::state) & t::constant::actor_play_state_mask));
        // Values copied only at BeginPlay. Later field writes cannot alter these.
        early_observed[actor] = {fake::get<float>(actor + early_health), fake::get<float>(actor + early_x),
            fake::get<float>(actor + early_shape), fake::get<float>(actor + early_shape + 4),
            fake::get<uint64_t>(actor + early_mesh), fake::get<int32_t>(actor + early_mode)};
        fake::put<uint8_t>(actor + t::actor_play::state, uint8_t(t::constant::actor_play_state_begun));
    }
}

void early_maps() {
    fake::start_engine();
    const uint64_t main = fake::main_image();
    const uint64_t actor_type = fake::object(u"Actor", w.root), functions = fake::object(u"Function", w.root);
    fake::set_super(w.actor_class, actor_type);
    early_actor_type = fake::object(u"EarlyActor", w.root);
    fake::set_super(early_actor_type, actor_type);
    // A class keeps the function table every object has; an actor has its own, which drops calls before play.
    fake::put<uint64_t>(early_actor_type, fake::engine().vtable);
    early_actor_vtable = fake::memory(0x40);
    for (uint64_t at = 0; at < 0x40; at += 8) fake::put<uint64_t>(early_actor_vtable + at, fake::get<uint64_t>(fake::engine().vtable + at));
    fake::code().push_back(reinterpret_cast<uint64_t>(&early_actor_process));
    fake::put(early_actor_vtable + t::object_vtable::process_event, &early_actor_process);
    const uint64_t world_type = fake::object(u"World", w.root), level_type = fake::object(u"Level", w.root);
    fake::field(level_type, u"OwningWorld", u"ObjectProperty", t::engine_level::owning_world, 8, world_type);
    const uint64_t shape_type = fake::object(u"EarlyShape", w.root);
    fake::field(shape_type, u"X", u"FloatProperty", 0, 4), fake::field(shape_type, u"Y", u"FloatProperty", 4, 4);
    fake::field(early_actor_type, u"Health", u"FloatProperty", early_health, 4);
    fake::field(early_actor_type, u"Shape", u"StructProperty", early_shape, 8, shape_type);
    fake::field(early_actor_type, u"Mesh", u"ObjectProperty", early_mesh, 8, w.component_class);
    const uint64_t enum_type = fake::object(u"EarlyMode", w.root), names = fake::memory(2 * t::enum_entry::size);
    fake::put(enum_type + t::enum_type::names, names), fake::put<int32_t>(enum_type + t::enum_type::names + t::array::count, 2);
    fake::put(names, fake::name(u"EarlyMode::Normal")), fake::put<int64_t>(names + t::enum_entry::value, 0);
    fake::put(names + t::enum_entry::size, fake::name(u"EarlyMode::Resistant")), fake::put<int64_t>(names + t::enum_entry::size + t::enum_entry::value, 4);
    const uint64_t mode_property = fake::field(early_actor_type, u"Mode", u"EnumProperty", early_mode, 4, fake::loose_field(u"IntProperty", 4));
    fake::put(mode_property + t::property::meta_class, enum_type);
    fake::bit_field(early_actor_type, u"bCollision", 0x9C, 0, 1);
    fake::field(early_actor_type, u"Part", u"ObjectProperty", 0xA0, 8, w.component_class);
    fake::field(w.component_class, u"Mesh", u"ObjectProperty", early_mesh, 8, w.component_class);
    fake::field(early_actor_type, u"Linked", u"ObjectProperty", early_linked, 8, actor_type);
    fake::field(early_actor_type, u"Links", u"ArrayProperty", early_links, 0x10, fake::loose_field(u"ObjectProperty", 8, actor_type));
    const uint64_t link_type = fake::object(u"EarlyLink", w.root);
    fake::field(link_type, u"Weight", u"FloatProperty", 0, 4), fake::field(link_type, u"Target", u"ObjectProperty", 8, 8, actor_type);
    fake::field(early_actor_type, u"Link", u"StructProperty", early_link, 0x10, link_type);
    fake::bit_field(w.component_class, u"bCollision", 0x9C, 0, 1);
    const uint64_t in = t::constant::property_param, out = in | t::constant::property_out | t::constant::property_return;
    fake::function(early_actor_type, functions, u"K2_DestroyActor", 0, 1, [](uint64_t actor, uint64_t) { fake::destroy(actor); });
    const uint64_t root = fake::engine_object(u"EarlyRoot", w.component_class);
    fake::field(w.component_class, u"Mobility", u"ByteProperty", 0x80, 1);
    constexpr int32_t attached_children = 0xD0;
    fake::field(w.component_class, u"AttachChildren", u"ArrayProperty", attached_children, 0x10,
                fake::loose_field(u"ObjectProperty", 8, w.component_class));
    const uint64_t get_root = fake::function(early_actor_type, functions, u"K2_GetRootComponent", 8, 8,
        [=](uint64_t, uint64_t args) { fake::put(args, root); });
    fake::flag(fake::field(get_root, u"ReturnValue", u"ObjectProperty", 0, 8, w.component_class), out);
    const uint64_t mobility = fake::function(w.component_class, functions, u"SetMobility", 1, 1,
        [](uint64_t object, uint64_t args) {
            ++early_mobility_calls;
            const uint8_t mobility = fake::get<uint8_t>(args);
            fake::put<uint8_t>(object + 0x80, mobility);
            // Movable spreads to attached children. Restoring the root does not restore them.
            if (mobility == t::constant::mobility_movable) {
                const uint64_t children = fake::get<uint64_t>(object + attached_children);
                const int32_t count = fake::get<int32_t>(object + attached_children + t::array::count);
                for (int32_t i = 0; i < count; ++i)
                    fake::put<uint8_t>(fake::get<uint64_t>(children + uint64_t(i) * 8) + 0x80, mobility);
            }
        });
    fake::flag(fake::field(mobility, u"NewMobility", u"ByteProperty", 0, 1), in);
    const uint64_t triple = fake::object(u"EarlyVector", w.root), turn = fake::object(u"EarlyRotator", w.root);
    for (int32_t i = 0; i < 3; ++i) {
        fake::field(triple, i == 0 ? u"X" : i == 1 ? u"Y" : u"Z", u"FloatProperty", i * 4, 4);
        fake::field(turn, i == 0 ? u"Pitch" : i == 1 ? u"Yaw" : u"Roll", u"FloatProperty", i * 4, 4);
    }
    const uint64_t move = fake::function(early_actor_type, functions, u"K2_SetActorLocationAndRotation", 0x20, 4,
        [](uint64_t actor, uint64_t args) { fake::put<float>(actor + early_x, fake::get<float>(args)); });
    fake::flag(fake::field(move, u"NewLocation", u"StructProperty", 0, 12, triple), in);
    fake::flag(fake::field(move, u"NewRotation", u"StructProperty", 12, 12, turn), in);
    fake::flag(fake::bit_field(move, u"bTeleport", 24, 0, 1), in);
    const uint64_t where = fake::function(early_actor_type, functions, u"K2_GetActorLocation", 12, 4,
        [](uint64_t actor, uint64_t args) { fake::put<float>(args, fake::get<float>(actor + early_x)); fake::put<float>(args + 4, 0); fake::put<float>(args + 8, 0); });
    fake::flag(fake::field(where, u"ReturnValue", u"StructProperty", 0, 12, triple), out);
    const uint64_t facing = fake::function(early_actor_type, functions, u"K2_GetActorRotation", 12, 4,
        [](uint64_t, uint64_t args) { fake::put<float>(args, 0); fake::put<float>(args + 4, 360); fake::put<float>(args + 8, 0); });
    fake::flag(fake::field(facing, u"ReturnValue", u"StructProperty", 0, 12, turn), out);
    const uint64_t stock_mesh = fake::engine_object(u"StockMesh", w.component_class), changed_mesh = fake::engine_object(u"ChangedMesh", w.component_class);
    std::u16string mesh_path = u"/G/Mesh.Mesh";
    shelf.assets[fake::engine_name(mesh_path.data(), mesh_path.size(), 1)] = changed_mesh;
    t::fn::load_soft_object = game_load_soft_object;
    t::fn::fs_open_file = game_fs_open_file, t::fn::fs_close_file = game_fs_close_file;
    t::fn::fs_file_size = game_fs_file_size, t::fn::fs_read_file = game_fs_read_file;
    t::fn::fs_open_folder = game_fs_open_folder, t::fn::fs_read_folder = game_fs_read_folder, t::fn::fs_close_folder = game_fs_close_folder;
    tsuchi::set_log_sink(log_sink);
    disk.files.clear();
    const std::string folder = std::string(tsuchi::kModsFolder) + "/";
    const auto declare = [&](const std::string& id, const std::string& changes, const std::string& mode = "story") {
        disk.files[folder + id + "/mod.json"] = R"({"id":")" + id + R"(","name":"Early maps","version":"1.0.0","maps":[{"map":"/G/Early_P","changes":"changes.json","mode":")" + mode + R"("}]})";
        disk.files[folder + id + "/changes.json"] = changes;
    };
    declare("map.early.a", R"({"levels":{"Early_P":{"remove":["Remove"],"move":{"Move":{"location":[100,0,0],"rotation":[0,0,0]}},"set":{"Gate":{"Health":40,"Mesh":"/G/Mesh.Mesh","Mode":"Resistant","Shape":{"X":9,"Y":6},"bCollision":true,"Part.Mesh":"/G/Mesh.Mesh","Part.bCollision":true}}},"Stream":{"set":{"Gate":{"Health":40}}},"Nested":{"set":{"Gate":{"Health":40}}}}})");
    declare("map.early.b", R"({"levels":{"Early_P":{"set":{"Gate":{"health":75,"Shape.X":15,"Mesh":{"asset":"/G/Mesh.Mesh"}}}},"Stream":{"set":{"Gate":{"Health":75}}},"Nested":{"set":{"Gate":{"Health":75}}}}})");
    declare("map.early.bad", R"({"levels":{"Early_P":{"set":{"Gate":{"Health":[1]}}}}})");
    const auto& site = t::code_sites[t::code_site::level_prepare.number];
    fake::put<uint32_t>(main + site.address, 0x14000000u | uint32_t((site.entry - site.address) >> 2));
    const uint64_t package = fake::object(u"/G/Early_P", w.root), world = fake::object(u"Early_P", world_type);
    fake::put(world + t::object::outer, package);
    fake::put(main + t::data::world, world), fake::put(main + t::data::game_framework, w.framework);
    const auto make_level = [&](const std::u16string& name) {
        const uint64_t saved_world = name == u"Early_P" ? world : fake::object(name, world_type);
        const uint64_t level = fake::object(u"PersistentLevel", level_type);
        fake::put(level + t::object::outer, saved_world), fake::put(level + t::engine_level::owning_world, world);
        return level;
    };
    const auto actor = [&](uint64_t level, const std::u16string& name) {
        const uint64_t object = fake::engine_object(name, early_actor_type);
        fake::put<uint64_t>(object, early_actor_vtable);
        fake::put(object + t::object::outer, level), fake::put<float>(object + early_health, 10);
        fake::put<float>(object + early_shape, 1), fake::put<float>(object + early_shape + 4, 2), fake::put(object + early_mesh, stock_mesh);
        const uint64_t part = fake::engine_object(name + u"Part", w.component_class);
        fake::put(object + 0xA0, part), fake::put(part + early_mesh, stock_mesh);
        return object;
    };
    const auto actors_in = [](uint64_t level, const std::vector<uint64_t>& objects) {
        const uint64_t data = fake::memory(objects.size() * 8);
        for (size_t i = 0; i < objects.size(); ++i) fake::put(data + i * 8, objects[i]);
        fake::put(level + t::engine_level::actors, data), fake::put<int32_t>(level + t::engine_level::actors + t::array::count, int32_t(objects.size()));
    };
    const uint64_t persistent = make_level(u"Early_P"), gate = actor(persistent, u"Gate"), moved_actor = actor(persistent, u"Move"), removed_actor = actor(persistent, u"Remove");
    const uint64_t rule_type = fake::object(u"CarrotGameRule", w.root), rule = fake::engine_object(u"Rule", rule_type);
    fake::set_super(rule_type, actor_type);
    fake::field(rule_type, u"GameRule", u"ByteProperty", 0x80, 1);
    fake::put<uint8_t>(rule + 0x80, t::constant::rule_story);
    fake::put(rule + t::object::outer, persistent);
    actors_in(persistent, {gate, moved_actor, removed_actor, rule});
    // Others hold the actor a mod removes. Before play they are left as a level saved without it would have them.
    const uint64_t links = fake::memory(16);
    fake::put(gate + early_linked, removed_actor), fake::put(links, moved_actor), fake::put(links + 8, removed_actor);
    fake::put(gate + early_links + t::array::data, links), fake::put<int32_t>(gate + early_links + t::array::count, 2);
    fake::put<int32_t>(gate + early_links + t::array::max, 2);
    fake::put(moved_actor + early_link + 8, removed_actor), fake::put(moved_actor + early_linked, gate);
    const uint64_t start_context = fake::memory(0x200), instance = fake::memory(0x100), story = fake::memory(0x200);
    fake::put(main + t::data::game_context, start_context), fake::put(main + t::data::game_instance, instance);
    fake::put(instance + t::game_instance::story_save, story);
    fake::put<uint8_t>(start_context + t::game_context::rule, 111); // the previous world must not decide this mode
    t::fn::story_flags = [](void*) -> uint64_t { return early_flags; };
    fake::put(world + t::world::persistent_level, persistent);
    CHECK(tsuchi::find_mods() == TSUCHI_OK);
    tsuchi::start_mods();
    CHECK(tsuchi::find_mod("map.early.a")->state == tsuchi::ModState::running);
    CHECK(tsuchi::find_mod("map.early.b")->state == tsuchi::ModState::running);
    CHECK(tsuchi::find_mod("map.early.bad")->state == tsuchi::ModState::blocked);
    const auto prepare = [&](uint64_t level) { thunks::call(tsuchi_hook_target(tsuchi::kCodeSiteThunk | t::code_site::level_prepare.number), level, 7, 0, 0x1234); };
    const auto initialize = [&](uint64_t level) { prepare(level); early_begin_level(level); };
    logged.lines.clear();
    prepare(persistent);
    CHECK(early_observed.empty() && early_registered_mesh[gate] == changed_mesh && early_registered_collision[gate]);
    // A root that may not move is moved as it is: its level has not registered it yet.
    CHECK(early_mobility_calls == 0 && fake::get<uint8_t>(root + 0x80) == 0);
    CHECK(fake::get<uint64_t>(gate + early_linked) == 0 && fake::get<uint64_t>(links) == moved_actor && fake::get<uint64_t>(links + 8) == 0);
    CHECK(fake::get<uint64_t>(moved_actor + early_link + 8) == 0 && fake::get<uint64_t>(moved_actor + early_linked) == gate);
    for (const char* said : {"[map.early.a] maps: Linked of Gate in Early_P now refers to nothing, as it referred to the removed Remove",
                             "[map.early.a] maps: Links[1] of Gate in Early_P now refers to nothing, as it referred to the removed Remove",
                             "[map.early.a] maps: Link.Target of Move in Early_P now refers to nothing, as it referred to the removed Remove"})
        CHECK(logged.lines.find(said) != std::string::npos);
    for (const char* said : {"[map.early.a] Removed Remove from Early_P for this map.",
                             "[map.early.a] Moved Move in Early_P for this map.",
                             "[map.early.b] maps: health of Gate in Early_P was set for this map."})
        CHECK(logged.lines.find(said) != std::string::npos);
    CHECK(logged.lines.find("[map.early.a] maps: Health of Gate in Early_P was set for this map.") == std::string::npos);
    const auto first_counts = tsuchi::map_counts();
    const std::string first_lines = logged.lines;
    prepare(persistent); // incremental registration can return and come through again
    CHECK(tsuchi::map_counts().entries_taken == first_counts.entries_taken && tsuchi::map_counts().removed == first_counts.removed);
    CHECK(logged.lines == first_lines);
    CHECK(tsuchi::game::object_kept(stock_mesh));
    early_begin_level(persistent);
    CHECK(early_observed[gate].health == 75 && early_observed[gate].shape_x == 15 && early_observed[gate].shape_y == 6);
    if (early_observed[gate].mesh != changed_mesh || early_observed[gate].mode != 4 || fake::get<uint8_t>(gate + 0x9C) != 1)
        std::printf("early value diagnostics: mesh %llu/%llu mode %d collision %u\n%s", (unsigned long long)early_observed[gate].mesh,
            (unsigned long long)changed_mesh, early_observed[gate].mode, fake::get<uint8_t>(gate + 0x9C), logged.lines.c_str());
    CHECK(early_observed[gate].mesh == changed_mesh && early_observed[gate].mode == 4 && fake::get<uint8_t>(gate + 0x9C) == 1);
    CHECK(early_observed[moved_actor].x == 100 && !early_observed.count(removed_actor) && !live(removed_actor));
    tsuchi::frame(0.03f); // no first game frame yet, so early originals must survive
    CHECK(fake::get<float>(gate + early_health) == 75);
    for (const auto& name : {u"Stream", u"Nested"}) {
        const uint64_t level = make_level(name), streamed_gate = actor(level, u"Gate");
        actors_in(level, {streamed_gate});
        initialize(level);
        CHECK(early_observed[streamed_gate].health == 75);
    }
    tsuchi::mod_stop(tsuchi::find_mod("map.early.b")->context);
    CHECK(fake::get<float>(gate + early_health) == 40 && fake::get<float>(gate + early_shape) == 9);
    tsuchi::mod_stop(tsuchi::find_mod("map.early.a")->context);
    CHECK(fake::get<float>(gate + early_health) == 10 && fake::get<float>(gate + early_shape) == 1 && fake::get<float>(gate + early_shape + 4) == 2);
    CHECK(fake::get<uint64_t>(gate + early_mesh) == stock_mesh && fake::get<int32_t>(gate + early_mode) == 0);
    CHECK(fake::get<float>(moved_actor + early_x) == 100 && !live(removed_actor));
    CHECK(!tsuchi::game::object_kept(stock_mesh));
    // An already playing world gets the late fallback once. A constructed level
    // present at mod start must still wait for its own native initialization.
    const uint64_t later_level = make_level(u"Later"), later_gate = actor(later_level, u"Gate");
    actors_in(later_level, {later_gate});
    const uint64_t clock = fake::memory(0x20), context = fake::memory(0x200);
    fake::put(w.framework + t::game_framework::clock, clock), fake::put<uint8_t>(w.framework + t::game_framework::world_begun, 1);
    fake::put(main + t::data::game_context, context), fake::put<uint8_t>(context + t::game_context::rule, t::constant::rule_story);
    t::fn::story_flags = [](void*) -> uint64_t { return early_flags; };
    disk.files.clear();
    declare("map.early.late", R"({"levels":{"Early_P":{"set":{"Gate":{"Health":31}}},"Later":{"set":{"Gate":{"Health":31}}}}})");
    CHECK(tsuchi::find_mods() == TSUCHI_OK); tsuchi::start_mods();
    logged.lines.clear();
    for (int i = 0; i < 60; ++i) tsuchi::frame(0.03f);
    CHECK(fake::get<float>(gate + early_health) == 31 && early_observed[gate].health == 75);
    CHECK(fake::get<float>(later_gate + early_health) == 10);
    const std::string warning = "was already playing when this mod started";
    const size_t first = logged.lines.find(warning);
    CHECK(first != std::string::npos && logged.lines.find(warning, first + 1) == std::string::npos);
    initialize(later_level);
    CHECK(early_observed[later_gate].health == 31);
    for (int i = 0; i < 120; ++i) tsuchi::frame(0.03f);
    CHECK(logged.lines.find(warning, first + 1) == std::string::npos);
    tsuchi::forget_mods();
    CHECK(fake::get<float>(gate + early_health) == 10 && fake::get<float>(later_gate + early_health) == 10);
    // Late moves restore every child, including the last snapshot slot. Larger trees stay untouched.
    for (int count : {1, 63, 64}) {
        std::vector<uint64_t> children;
        const uint64_t data = fake::memory(size_t(count) * 8);
        for (int i = 0; i < count; ++i) {
            const uint64_t child = fake::engine_object(u"Attached", w.component_class);
            children.push_back(child);
            fake::put(data + uint64_t(i) * 8, child), fake::put<uint8_t>(child + 0x80, uint8_t(i % 2));
        }
        fake::put(root + attached_children, data), fake::put<int32_t>(root + attached_children + t::array::count, count);
        fake::put<uint8_t>(root + 0x80, 0), fake::put<float>(moved_actor + early_x, 0);
        early_mobility_calls = 0;
        disk.files.clear();
        declare("map.early.mobility", R"({"levels":{"Early_P":{"move":{"Move":{"location":[200,0,0],"rotation":[0,0,0]}}}}})");
        CHECK(tsuchi::find_mods() == TSUCHI_OK); tsuchi::start_mods();
        logged.lines.clear();
        for (int i = 0; i < 60; ++i) tsuchi::frame(0.03f);
        CHECK(fake::get<float>(moved_actor + early_x) == (count < 64 ? 200 : 0));
        CHECK(fake::get<uint8_t>(root + 0x80) == 0);
        for (int i = 0; i < count; ++i) CHECK(fake::get<uint8_t>(children[size_t(i)] + 0x80) == uint8_t(i % 2));
        if (count == 64) {
            CHECK(early_mobility_calls == 0);
            CHECK(logged.lines.find("Move in Early_P could not be moved") != std::string::npos);
        } else CHECK(early_mobility_calls == count + 2);
        tsuchi::forget_mods();
    }
    fake::put<int32_t>(root + attached_children + t::array::count, 0);
    // An actor its level puts back as play begins is moved again once it plays. One that kept its move, here
    // facing a whole turn round, is left. Each early move is looked at once.
    {
        disk.files.clear();
        declare("map.early.back", R"({"levels":{"Back":{"move":{"Undone":{"location":[300,0,0],"rotation":[0,0,0]},"Kept":{"location":[250,0,0],"rotation":[0,0,0]}}}}})");
        CHECK(tsuchi::find_mods() == TSUCHI_OK); tsuchi::start_mods();
        const uint64_t level = make_level(u"Back"), undone = actor(level, u"Undone"), kept = actor(level, u"Kept");
        actors_in(level, {undone, kept});
        logged.lines.clear();
        prepare(level);
        CHECK(fake::get<float>(undone + early_x) == 300 && fake::get<float>(kept + early_x) == 250);
        for (int i = 0; i < 3; ++i) tsuchi::frame(0.03f); // not playing yet: nothing is looked at
        CHECK(logged.lines.find("was back in its saved place") == std::string::npos);
        early_begin_level(level);
        fake::put<float>(undone + early_x, 7);
        for (int i = 0; i < 3; ++i) tsuchi::frame(0.03f);
        CHECK(fake::get<float>(undone + early_x) == 300 && fake::get<float>(kept + early_x) == 250);
        CHECK(logged.lines.find("[map.early.back] maps: Undone in Back was back in its saved place once its level began play, so it was moved again") != std::string::npos);
        CHECK(logged.lines.find("Kept in Back was back") == std::string::npos);
        fake::put<float>(undone + early_x, 7);
        for (int i = 0; i < 3; ++i) tsuchi::frame(0.03f);
        CHECK(fake::get<float>(undone + early_x) == 7);
        tsuchi::forget_mods();
    }
    // Two mods move one actor: the mod started last wins, and the look once it plays keeps that move,
    // whether its level put the actor back or not.
    for (bool put_back : {false, true}) {
        disk.files.clear();
        declare("map.early.rank.a", R"({"levels":{"Ranked":{"move":{"Both":{"location":[300,0,0],"rotation":[0,0,0]}}}}})");
        declare("map.early.rank.b", R"({"levels":{"Ranked":{"move":{"Both":{"location":[400,0,0],"rotation":[0,0,0]}}}}})");
        CHECK(tsuchi::find_mods() == TSUCHI_OK); tsuchi::start_mods();
        const uint64_t level = make_level(u"Ranked"), both = actor(level, u"Both");
        actors_in(level, {both});
        logged.lines.clear();
        prepare(level);
        CHECK(fake::get<float>(both + early_x) == 400);
        early_begin_level(level);
        if (put_back) fake::put<float>(both + early_x, 7);
        for (int i = 0; i < 3; ++i) tsuchi::frame(0.03f);
        CHECK(fake::get<float>(both + early_x) == 400);
        CHECK(logged.lines.find("[map.early.rank.a] maps: Both in Ranked was back") == std::string::npos);
        CHECK((logged.lines.find("[map.early.rank.b] maps: Both in Ranked was back in its saved place") != std::string::npos) == put_back);
        tsuchi::forget_mods();
    }
    for (bool olimar : {false, true}) {
        disk.files.clear();
        early_flags = olimar ? t::constant::story_of_olimar : 0;
        fake::put<uint8_t>(story + t::story_save::night, olimar ? 0 : 1);
        fake::put<uint8_t>(w.framework + t::game_framework::world_begun, 0);
        declare("map.early.mode", R"({"levels":{"Mode":{"set":{"Gate":{"Health":52,"Mode":"EarlyMode::Resistant","Shape":{"Missing":4}}}}}})", olimar ? "olimar" : "night");
        CHECK(tsuchi::find_mods() == TSUCHI_OK); tsuchi::start_mods();
        const uint64_t level = make_level(u"Mode"), target = actor(level, u"Gate");
        actors_in(level, {target});
        initialize(level);
        CHECK(early_observed[target].health == 52 && early_observed[target].mode == 4);
        CHECK(logged.lines.find("Shape.Missing of Gate in Mode could not be set") != std::string::npos);
        tsuchi::forget_mods();
    }
    // Unknown mode must not select shared ground when a mode-specific variant exists.
    disk.files.clear();
    const std::string unknown_changes = R"({"levels":{"Unknown":{"set":{"Gate":{"Health":99}}}}})";
    declare("map.early.unknown", unknown_changes);
    disk.files[folder + "map.early.unknown/mod.json"] = R"({"id":"map.early.unknown","name":"Mode variants","version":"1.0.0","maps":[{"map":"/G/Early_P","changes":"changes.json"},{"map":"/G/Early_P","mode":"night","changes":"changes.json"}]})";
    fake::put<uint8_t>(rule + 0x80, 111);
    CHECK(tsuchi::find_mods() == TSUCHI_OK); tsuchi::start_mods();
    const uint64_t unknown = make_level(u"Unknown"), untouched = actor(unknown, u"Gate");
    actors_in(unknown, {untouched});
    logged.lines.clear();
    initialize(unknown);
    CHECK(early_observed[untouched].health == 10);
    CHECK(logged.lines.find("the mode could not be read before Unknown begins play") != std::string::npos);
    tsuchi::forget_mods();
    std::puts("early maps: persistent, streamed, nested, stacking, assets, enums, structs, stop, and late fallback checked");
}
