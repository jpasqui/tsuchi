// Included inside shading(): reflected renderer users and instance lifetimes.
// These are host checks. Actual donor passes and pixels need the game fixture.
{
    const auto saved_files = disk.files;
    const auto saved_render_log = rendered.log;
    const uint64_t saved_stock = stock_material;
    disk.files.clear();
    SetMaterial target_row = lava_material();
    target_row.material = "/G/Mods/MI_Target.MI_Target";
    set_file("shade.targets", shader_set({target_row}));
    CHECK(tsuchi::find_mods() == TSUCHI_OK);
    const uint64_t target = mod_material(u"MI_Target");
    tsuchi::start_mods();
    steps(30);
    copy_comes(6);
    look(), look();
    CHECK(counted().copies_ready > before.copies_ready);
    stock_material = target;

    // Particle slots inherit PrimitiveComponent calls but store EmitterMaterials.
    const uint64_t particle_type = fake::object(u"ParticleSystemComponent", w.root);
    fake::set_super(particle_type, w.primitive_class);
    fake::field(particle_type, u"EmitterMaterials", u"ArrayProperty", kOverridesAt, 0x10,
                fake::loose_field(u"ObjectProperty", 8, material_class));
    const uint64_t particle = fake::engine_object(u"TargetParticle", particle_type);
    slots_of[particle] = 2;
    put_override(particle, 1, target);
    const uint64_t translucent = mesh(u"TargetTranslucent", 1);
    put_override(translucent, 0, target);

    // A decal has one direct material and uses its own setter argument name.
    const uint64_t decal_type = fake::object(u"DecalComponent", w.root);
    fake::set_super(decal_type, w.component_class);
    fake::field(decal_type, u"DecalMaterial", u"ObjectProperty", 0x80, 8, material_class);
    const uint64_t get_decal = fake::function(decal_type, function_type, u"GetDecalMaterial", 8, 8,
        [](uint64_t object, uint64_t args) { fake::put(args, fake::get<uint64_t>(object + 0x80)); });
    fake::flag(fake::field(get_decal, u"ReturnValue", u"ObjectProperty", 0, 8, material_class), kResult);
    const uint64_t set_decal = fake::function(decal_type, function_type, u"SetDecalMaterial", 8, 8,
        [](uint64_t object, uint64_t args) { fake::put(object + 0x80, fake::get<uint64_t>(args)); });
    fake::flag(fake::field(set_decal, u"NewDecalMaterial", u"ObjectProperty", 0, 8, material_class), kIn);
    const uint64_t decal = fake::engine_object(u"TargetDecal", decal_type);
    fake::put(decal + 0x80, target);

    // The component fixtures have admitted gameplay owners in the split runtime.
    const uint64_t target_owner = tabled(table_a(), u"RenderOwner", u"RenderOwner", 0, 0);
    fake::put(particle + t::scene_component::owner, target_owner);
    fake::put(decal + t::scene_component::owner, target_owner);
    fake::put(translucent + t::scene_component::owner, target_owner);
    parts_of[target_owner] = {particle, decal, translucent}; registers(target_owner);

    // A real Image brush is a struct, independent of WidgetComponent's mesh.
    const uint64_t brush_type = fake::object(u"SlateBrush", w.root);
    fake::put<int32_t>(brush_type + t::struct_type::bytes, 136);
    fake::field(brush_type, u"ResourceObject", u"ObjectProperty", 72, 8);
    tsuchi::game::ClassRef native_image{"Image"};
    const uint64_t base_image = tsuchi::game::find_class(native_image);
    const uint64_t image_type = fake::object(base_image ? u"TargetImageClass" : u"Image", w.root);
    if (base_image) fake::set_super(image_type, base_image);
    fake::field(image_type, u"Brush", u"StructProperty", 0x80, 136, brush_type);
    const uint64_t set_brush = fake::function(image_type, function_type, u"SetBrushResourceObject", 8, 8,
        [](uint64_t object, uint64_t args) { fake::put(object + 0x80 + 72, fake::get<uint64_t>(args)); });
    fake::flag(fake::field(set_brush, u"ResourceObject", u"ObjectProperty", 0, 8), kIn);
    const uint64_t image = fake::engine_object(u"TargetImage", image_type, 0x180);
    std::memset(reinterpret_cast<void*>(image + 0x80), 0x5a, 136);
    fake::put(image + 0x80 + 72, target);
    const auto brush_unchanged = [&] {
        for (uint32_t i = 0; i < 136; ++i)
            if ((i < 72 || i >= 80) && fake::get<uint8_t>(image + 0x80 + i) != 0x5a) return false;
        return true;
    };
    look(), steps(4);
    const uint64_t direct = tsuchi::drawn_as(target);
    CHECK(direct != target && tsuchi::game::is_material(direct));
    CHECK(override_in(particle, 0) == direct && override_in(particle, 1) == direct);
    CHECK(fake::get<uint64_t>(decal + 0x80) == direct);
    CHECK(fake::get<uint64_t>(image + 0x80 + 72) == direct && brush_unchanged());
    CHECK(override_in(translucent, 0) == direct);
    CHECK(!tsuchi::game::set_slot_material(decal, 1, target));
    CHECK(!tsuchi::game::set_slot_material(image, 1, target));

    // What asking costs. A user other than a mesh waits in the queue once, however many looks come before it is
    // asked: in the game a scene's particles and decals outnumbered what a look's steps ask, and the queue grew
    // until every step asked its most (run lead-material-09). And a material whose parents never reach a mod's
    // instance is not checked in full.
    {
        std::vector<uint64_t> crowd;
        for (int i = 0; i < 2100; ++i) crowd.push_back(fake::engine_object(u"CrowdParticle", particle_type));
        const uint32_t asked_before = counted().parts_asked, walks_before = counted().full_walks;
        steps(60); // four looks while the world settles: one of them alone queues more than its fifteen steps ask
        const uint32_t asked = counted().parts_asked - asked_before;
        CHECK(asked >= 2100 && asked <= 2 * 2100 + 200);
        // The meshes' own materials are the game's, and the overrides are the target's: only those reach it.
        const uint32_t walks_per_round = counted().full_walks - walks_before;
        const uint64_t game_mesh = mesh(u"GameMaterialMesh", 1);
        const uint64_t game_material = fake::engine_object(u"GameOnlyMaterial", instance_type);
        fake::put(game_material + 0xa0, fake::engine_object(u"GameOnlyParent", material_class));
        put_override(game_mesh, 0, game_material);
        const uint32_t walks_again = counted().full_walks;
        steps(60);
        CHECK(counted().full_walks - walks_again == walks_per_round);
        // With a clock, a step walks the engine's objects for at most a few milliseconds and the next goes on from
        // there: in the game a whole walk took 0.8 seconds in one step. A slot found late in the walk still gets the
        // stand-in once the walk ends. The crowd makes the table long enough to take several pieces.
        static uint64_t now_us = 0;
        tsuchi::cost_clock = [] { return now_us += 1000; }; // every reading a millisecond later
        const uint32_t whole_before = counted().walks, pieces_before = counted().walk_pieces;
        const uint64_t late = mesh(u"LateWalkMesh", 1);
        put_override(late, 0, target);
        uint32_t taken = 0;
        while (override_in(late, 0) != direct && taken < 600) steps(1), ++taken;
        tsuchi::cost_clock = nullptr;
        CHECK(override_in(late, 0) == direct);
        CHECK(counted().walks > whole_before && counted().walk_pieces - pieces_before > counted().walks - whole_before);
        fake::destroy(late), slots_of.erase(late);
        for (const uint64_t particle_user : crowd) fake::destroy(particle_user);
        fake::destroy(game_mesh), slots_of.erase(game_mesh);
        steps(15);
    }
    // A held slot that shows what it did, while nothing a material is drawn as has changed, is not resolved again.
    {
        const auto before = tsuchi::material_hold_counts();
        steps(5);
        const auto after = tsuchi::material_hold_counts();
        CHECK(after.kept > before.kept && after.unchanged - before.unchanged == after.kept - before.kept && after.set == before.set);
    }

    // Distinct descendants retain inherited values and their independent live overrides.
    fake::field(dynamic_type, u"Parent", u"ObjectProperty", 0xa0, 8, material_class);
    // An instance keeps its values as the engine's UMaterialInstance does: arrays of structs, each element led by
    // the parameter's name (ParameterInfo: name, association, index) with its value after it. A terminal UMaterial
    // has no such arrays; its defaults come from its expressions, kept here by object.
    struct Values {
        std::map<std::string, float> scalars;
        std::map<std::string, std::vector<float>> vectors;
        std::map<std::string, uint64_t> textures;
    };
    std::map<uint64_t, Values> expression_defaults;
    constexpr int32_t kScalarsAt = 0xb0, kVectorsAt = 0xc0, kTexturesAt = 0xd0;
    constexpr int32_t kScalarBytes = 0x24, kVectorBytes = 0x30, kTextureBytes = 0x28;
    for (const uint64_t type : {instance_type, dynamic_type}) {
        const auto values_array = [&](const char16_t* name, int32_t at, const char16_t* element, int32_t bytes) {
            fake::field(type, name, u"ArrayProperty", at, 0x10, fake::loose_field(u"StructProperty", bytes, fake::engine_object(element, w.root)));
        };
        values_array(u"ScalarParameterValues", kScalarsAt, u"ScalarParameterValue", kScalarBytes);
        values_array(u"VectorParameterValues", kVectorsAt, u"VectorParameterValue", kVectorBytes);
        values_array(u"TextureParameterValues", kTexturesAt, u"TextureParameterValue", kTextureBytes);
    }
    const auto parameter_name = [](uint64_t packed) {
        for (const auto& known : fake::engine().names)
            if (known.second == packed) return std::string(known.first.begin(), known.first.end());
        return std::string();
    };
    const auto values_of = [&](uint64_t object) {
        const uint64_t type = tsuchi::game::class_of(object);
        if (type != instance_type && type != dynamic_type) return expression_defaults[object];
        Values values;
        const auto each = [&](int32_t at, int32_t bytes, const auto& take) {
            const uint64_t data = fake::get<uint64_t>(object + at + t::array::data);
            for (int32_t i = 0; i < fake::get<int32_t>(object + at + t::array::count); ++i)
                take(parameter_name(fake::get<uint64_t>(data + uint64_t(i) * bytes)), data + uint64_t(i) * bytes + 0x10);
        };
        each(kScalarsAt, kScalarBytes, [&](const std::string& name, uint64_t value) { values.scalars[name] = fake::get<float>(value); });
        each(kVectorsAt, kVectorBytes, [&](const std::string& name, uint64_t value) {
            values.vectors[name] = {fake::get<float>(value), fake::get<float>(value + 4), fake::get<float>(value + 8), fake::get<float>(value + 12)};
        });
        each(kTexturesAt, kTextureBytes, [&](const std::string& name, uint64_t value) { values.textures[name] = fake::get<uint64_t>(value); });
        return values;
    };
    // Writes an instance's arrays in place where they have room, as the engine's setters do.
    const auto set_values = [&](uint64_t object, const Values& values) {
        const auto write = [&](int32_t at, int32_t bytes, const auto& entries, const auto& put_value) {
            const int32_t count = int32_t(entries.size());
            uint64_t data = fake::get<uint64_t>(object + at + t::array::data);
            if (fake::get<int32_t>(object + at + t::array::max) < count) {
                data = fake::memory(size_t(count) * bytes);
                fake::put(object + at + t::array::data, data), fake::put<int32_t>(object + at + t::array::max, count);
            }
            int32_t i = 0;
            for (const auto& entry : entries) {
                const uint64_t element = data + uint64_t(i++) * bytes;
                std::memset(reinterpret_cast<void*>(element), 0, bytes);
                const std::u16string name(entry.first.begin(), entry.first.end());
                fake::put<uint64_t>(element, fake::engine_name(const_cast<char16_t*>(name.data()), name.size(), 1));
                fake::put<int32_t>(element + 0xc, -1);
                put_value(element + 0x10, entry.second);
            }
            fake::put<int32_t>(object + at + t::array::count, count);
        };
        write(kScalarsAt, kScalarBytes, values.scalars, [](uint64_t at, float value) { fake::put(at, value); });
        write(kVectorsAt, kVectorBytes, values.vectors, [](uint64_t at, const std::vector<float>& value) {
            for (size_t i = 0; i < 4 && i < value.size(); ++i) fake::put(at + 4 * i, value[i]);
        });
        write(kTexturesAt, kTextureBytes, values.textures, [](uint64_t at, uint64_t value) { fake::put(at, value); });
    };
    const auto edit = [&](uint64_t object, const std::function<void(Values&)>& change) {
        Values values = values_of(object);
        change(values);
        set_values(object, values);
    };
    const uint64_t parameter_texture_a = fake::engine_object(u"InstanceTextureA", w.component_class);
    const uint64_t parameter_texture_b = fake::engine_object(u"InstanceTextureB", w.component_class);
    const uint64_t parameter_texture_default = fake::engine_object(u"DefaultTexture", w.component_class);
    // The source's terminal game UMaterial resource supplies its own defaults.
    // Authored graph defaults are materialized in the shipped creator MIC.
    const uint64_t donor_base = fake::engine_object(u"DonorBase", material_class, 0x100);
    expression_defaults[donor_base] = {{{"FromBase", 6}, {"CollisionDefault", 0.1f}}, {{"GameTint", {1, 0, 0, 1}}}, {{"GameTexture", parameter_texture_b}}};
    const uint64_t target_parent = fake::engine_object(u"TargetParent", instance_type);
    fake::put(target + 0xa0, target_parent);
    fake::put(target_parent + 0xa0, donor_base);
    const uint64_t source_a = fake::engine_object(u"TargetA", dynamic_type);
    const uint64_t source_b = fake::engine_object(u"TargetB", dynamic_type);
    fake::put(source_a + 0xa0, target), fake::put(source_b + 0xa0, source_a);
    // Parameter value and object-identity checks use the real copy call path.
    // They do not establish texture sampling or native GC graph traversal.
    set_values(target_parent, {{{"FromParent", 4}}, {{"ParentTint", {0.1f, 0.2f, 0.3f, 0.4f}}}, {}});
    set_values(target, {{{"Inherited", 7}, {"Color", 1}, {"Alpha", 0.5f}, {"CollisionDefault", 0.5f}}, {{"Tint", {0.6f, 0.7f, 0.8f, 1}}},
                        {{"Mask", parameter_texture_default}}});
    set_values(source_a, {{{"Color", 2}, {"Alpha", 0.25f}, {"OwnOnly", 9}}, {{"Tint", {0.2f, 0.4f, 0.6f, 0.8f}}}, {{"Mask", parameter_texture_a}}});
    set_values(source_b, {{{"Color", 3}, {"Alpha", 0.75f}}, {{"Tint", {0.9f, 0.8f, 0.7f, 0.6f}}}, {{"Mask", parameter_texture_b}}});
    const uint64_t catalogue_copy = fake::engine_object(u"CatalogueCopy", dynamic_type);
    uint64_t world_on_copy = 0, callback_made = 0;
    TSUCHI_Mod* stop_on_copy = nullptr;
    bool* deny_on_copy = nullptr;
    uint64_t direct_copy_trigger = 0, direct_stop_source = 0;
    TSUCHI_Mod* direct_stop_provider = nullptr;
    bool direct_stop_exercised = false;
    bool verify_cached_copy_roots = false, verified_cached_copy_roots = false;
    uint32_t parameter_copies = 0;
    const uint64_t copy_all = fake::function(dynamic_type, function_type, u"K2_CopyMaterialInstanceParameters", 16, 8,
        [&](uint64_t object, uint64_t args) {
            ++parameter_copies;
            const bool uniform_copy = fake::get<uint8_t>(args + 8) != 0;
            CHECK(uniform_copy || object == catalogue_copy);
            uint64_t chain[32]; uint32_t n = 0;
            for (uint64_t source = fake::get<uint64_t>(args); source && n < 32;
                 source = fake::get<uint64_t>(source + 0xa0)) chain[n++] = source;
            Values merged;
            const Values catalogue = expression_defaults[donor_base];
            while (n) {
                const Values inherited = values_of(chain[--n]);
                // FALSE enumerates the target donor UMaterial's catalogue.
                // TRUE merges all arrays from the source's complete hierarchy.
                for (const auto& value : inherited.scalars)
                    if (uniform_copy || catalogue.scalars.count(value.first)) merged.scalars[value.first] = value.second;
                for (const auto& value : inherited.vectors)
                    if (uniform_copy || catalogue.vectors.count(value.first)) merged.vectors[value.first] = value.second;
                for (const auto& value : inherited.textures)
                    if (uniform_copy || catalogue.textures.count(value.first)) merged.textures[value.first] = value.second;
            }
            set_values(object, merged);
            if (world_on_copy) {
                callback_made = object;
                scene.world = world_on_copy; world_on_copy = 0;
                fake::put(main + t::data::world, scene.world);
            }
            if (deny_on_copy) {
                CHECK(tsuchi::game::object_kept(fake::get<uint64_t>(args)) && tsuchi::game::object_kept(object));
                callback_made = object;
                *deny_on_copy = false; deny_on_copy = nullptr;
            }
            if (direct_copy_trigger && fake::get<uint64_t>(args) == direct_copy_trigger) {
                direct_copy_trigger = 0;
                stop_on_copy = direct_stop_provider;
                CHECK(tsuchi::drawn_as(direct_stop_source) == direct_stop_source);
                // The outer per-step copy remains pinned after the nested cached
                // call stops the provider and drops both cache entries.
                CHECK(tsuchi::game::object_kept(fake::get<uint64_t>(args)) && tsuchi::game::object_kept(object));
                direct_stop_exercised = true;
            }
            if (stop_on_copy) {
                callback_made = object;
                TSUCHI_Mod* provider = stop_on_copy; stop_on_copy = nullptr;
                tsuchi::mod_stop(provider);
                if (verify_cached_copy_roots) {
                    CHECK(tsuchi::game::object_kept(fake::get<uint64_t>(args)));
                    CHECK(tsuchi::game::object_kept(object));
                    verified_cached_copy_roots = true;
                }
            }
        });
    const uint64_t source_argument = fake::field(copy_all, u"Source", u"ObjectProperty", 0, 8, material_class);
    fake::flag(source_argument, kIn);
    fake::flag(fake::bit_field(copy_all, u"bQuickParametersOnly", 8, 0, 0xff), kIn);
    {
        tsuchi::game::EngineCall donor_only(catalogue_copy, "K2_CopyMaterialInstanceParameters");
        CHECK(donor_only.set("Source", tsuchi::game::object_value(tsuchi::game::to_handle(source_a))) == TSUCHI_OK);
        CHECK(donor_only.set("bQuickParametersOnly", tsuchi::game::boolean_value(false)) == TSUCHI_OK);
        CHECK(donor_only.invoke() == TSUCHI_OK);
        Values copied = values_of(catalogue_copy);
        CHECK(copied.scalars.size() == 2 && copied.scalars["FromBase"] == 6);
        CHECK(!copied.scalars.count("Alpha") && !copied.scalars.count("OwnOnly"));
        CHECK(copied.vectors.size() == 1 && !copied.vectors.count("Tint"));
        CHECK(copied.textures.size() == 1 && !copied.textures.count("Mask"));
    }
    const uint64_t shown_a = tsuchi::drawn_as(source_a), shown_b = tsuchi::drawn_as(source_b);
    CHECK(shown_a != source_a && shown_b != source_b && shown_a != shown_b);
    {
        Values a = values_of(shown_a), b = values_of(shown_b);
        CHECK(a.scalars["Inherited"] == 7 && a.scalars["Color"] == 2 && a.scalars["FromParent"] == 4);
        CHECK(b.scalars["Inherited"] == 7 && b.scalars["Color"] == 3);
        CHECK(a.scalars["FromBase"] == 6 && b.scalars["FromBase"] == 6);
        CHECK(a.scalars["CollisionDefault"] == 0.5f && b.scalars["CollisionDefault"] == 0.5f);
        CHECK(values_of(donor_base).scalars["CollisionDefault"] == 0.1f);
        CHECK(a.scalars["Alpha"] == 0.25f && b.scalars["Alpha"] == 0.75f);
        CHECK(a.vectors["Tint"] != b.vectors["Tint"] && b.vectors["ParentTint"] == values_of(target_parent).vectors["ParentTint"]);
        CHECK(a.textures["Mask"] == parameter_texture_a && b.textures["Mask"] == parameter_texture_b);
    }
    edit(source_b, [](Values& v) { v.vectors.erase("Tint"), v.textures.erase("Mask"); });
    edit(source_a, [](Values& v) { v.vectors["Tint"] = {0.5f, 0.4f, 0.3f, 0.2f}; });
    CHECK(tsuchi::game::object_kept(source_a) && tsuchi::game::object_kept(shown_a));
    edit(source_b, [](Values& v) { v.scalars.erase("Color"), v.scalars.erase("Alpha"); });
    edit(source_a, [](Values& v) { v.scalars["Color"] = 9, v.scalars["Alpha"] = 0.4f; });
    steps(1);
    {
        Values a = values_of(shown_a), b = values_of(shown_b), source = values_of(source_a);
        CHECK(a.scalars["Color"] == 9 && b.scalars["Color"] == 9);
        CHECK(a.vectors["Tint"] == source.vectors["Tint"] && b.vectors["Tint"] == source.vectors["Tint"]);
        CHECK(b.textures["Mask"] == parameter_texture_a);
        CHECK(a.scalars["Alpha"] == 0.4f && b.scalars["Alpha"] == 0.4f);
    }
    edit(source_a, [](Values& v) { v.scalars.erase("Alpha"), v.scalars.erase("OwnOnly"), v.vectors.erase("Tint"), v.textures.erase("Mask"); });
    steps(1);
    {
        Values a = values_of(shown_a), b = values_of(shown_b), inherited = values_of(target);
        CHECK(a.scalars["Alpha"] == 0.5f && b.scalars["Alpha"] == 0.5f);
        CHECK(!a.scalars.count("OwnOnly") && !b.scalars.count("OwnOnly"));
        CHECK(a.vectors["Tint"] == inherited.vectors["Tint"] && b.vectors["Tint"] == inherited.vectors["Tint"]);
        CHECK(a.textures["Mask"] == parameter_texture_default && b.textures["Mask"] == parameter_texture_default);
    }
    // A step in which no source changed asks the engine for no copy: each copy makes the renderer rebuild the
    // stand-in's uniforms. A change anywhere in the parent chain reaches every descendant in the step it is made,
    // and a value written back as it was is no change.
    const uint32_t before_quiet = parameter_copies;
    steps(3);
    CHECK(parameter_copies == before_quiet);
    edit(target, [](Values& v) { v.scalars["Inherited"] = 11; });
    steps(1);
    CHECK(parameter_copies == before_quiet + 2);
    CHECK(values_of(shown_a).scalars["Inherited"] == 11 && values_of(shown_b).scalars["Inherited"] == 11);
    edit(source_b, [](Values& v) { v.vectors["Tint"] = {0.9f, 0.8f, 0.7f, 0.6f}; });
    steps(1);
    CHECK(parameter_copies == before_quiet + 3 && values_of(shown_b).vectors["Tint"][0] == 0.9f);
    edit(source_b, [](Values&) {});
    steps(1);
    CHECK(parameter_copies == before_quiet + 3);
    edit(target, [](Values& v) { v.scalars["Inherited"] = 7; });
    edit(source_b, [](Values& v) { v.vectors.erase("Tint"); });
    steps(1);
    CHECK(parameter_copies == before_quiet + 5 && values_of(shown_a).scalars["Inherited"] == 7);
    // A readable null Parent ends the walk. A dead non-null object is not null.
    fake::put<uint64_t>(target_parent + 0xa0, 0);
    CHECK(tsuchi::drawn_as(source_a) != source_a);
    const uint64_t dead_parent = fake::engine_object(u"DeadMaterialParent", instance_type);
    fake::destroy(dead_parent);
    fake::put(target_parent + 0xa0, dead_parent);
    const uint32_t before_dead_parent = parameter_copies;
    CHECK(tsuchi::drawn_as(target) == target && tsuchi::drawn_as(source_a) == source_a);
    CHECK(parameter_copies == before_dead_parent);
    fake::put(target_parent + 0xa0, donor_base);
    fake::put(source_a + 0xa0, source_b); // cycle cannot match the shader material
    CHECK(tsuchi::drawn_as(source_a) == source_a && tsuchi::drawn_as(source_b) == source_b);
    fake::put(source_a + 0xa0, target);
    // Reaching the mod's instance does not make a later cycle safe: the native
    // uniform-copy function walks all the way to the terminal UMaterial.
    fake::put(target_parent + 0xa0, target);
    const uint32_t before_parent_cycle = parameter_copies;
    CHECK(tsuchi::drawn_as(target) == target && tsuchi::drawn_as(source_a) == source_a);
    CHECK(parameter_copies == before_parent_cycle);
    std::vector<uint64_t> long_tail;
    for (uint32_t i = 0; i < 32; ++i) long_tail.push_back(fake::engine_object(u"LongMaterialTail", instance_type));
    for (uint32_t i = 0; i < long_tail.size(); ++i)
        fake::put(long_tail[i] + 0xa0, i + 1 < long_tail.size() ? long_tail[i + 1] : donor_base);
    fake::put(target_parent + 0xa0, long_tail.front());
    CHECK(tsuchi::drawn_as(target) == target && tsuchi::drawn_as(source_a) == source_a);
    CHECK(parameter_copies == before_parent_cycle);
    fake::put(target_parent + 0xa0, donor_base);

    // A changed brush stays format-identical. A replacement original is retained.
    fake::put(image + 0x80 + 72, source_b);
    steps(1);
    CHECK(fake::get<uint64_t>(image + 0x80 + 72) == tsuchi::drawn_as(source_b) && brush_unchanged());
    const uint64_t prior_world = scene.world;
    scene.world = in_world(u"/G/Targets_P", u"Targets_P");
    fake::put(main + t::data::world, scene.world);
    steps(1); // restore persistent Image before any stand-in for the new world
    CHECK(fake::get<uint64_t>(image + 0x80 + 72) == source_b && brush_unchanged());
    CHECK(!tsuchi::game::object_kept(direct));
    steps(33);
    CHECK(fake::get<uint64_t>(image + 0x80 + 72) != source_b && brush_unchanged());
    CHECK(values_of(tsuchi::drawn_as(target)).scalars["FromParent"] == 4);

    // Assigning a new MID uses its own values without waiting for a world scan.
    const uint64_t source_c = fake::engine_object(u"TargetC", dynamic_type);
    fake::put(source_c + 0xa0, target);
    set_values(source_c, {{{"Color", 8}}, {}, {}});
    fake::put(image + 0x80 + 72, source_c);
    steps(1);
    const uint64_t shown_c = tsuchi::drawn_as(source_c);
    CHECK(fake::get<uint64_t>(image + 0x80 + 72) == shown_c && brush_unchanged());
    CHECK(values_of(shown_c).scalars["Color"] == 8 && values_of(shown_c).scalars["FromParent"] == 4);
    // Guarded restoration shares the resolver and its in-flight leases.
    bool restoration_allowed = false;
    const tsuchi::game::CallGuard restoration_guard{
        [](void* context) { return *static_cast<bool*>(context); }, &restoration_allowed};
    uint64_t guarded_shown = 0;
    const uint32_t before_veto = parameter_copies;
    CHECK(!tsuchi::drawn_as_restoring(target, restoration_guard, guarded_shown) && guarded_shown == target);
    CHECK(parameter_copies == before_veto);
    restoration_allowed = true;
    CHECK(tsuchi::drawn_as_restoring(target, restoration_guard, guarded_shown) && guarded_shown == tsuchi::drawn_as(target));
    const uint64_t guarded_source = fake::engine_object(u"GuardedSource", dynamic_type);
    fake::put(guarded_source + 0xa0, target);
    deny_on_copy = &restoration_allowed; callback_made = 0;
    CHECK(!tsuchi::drawn_as_restoring(guarded_source, restoration_guard, guarded_shown));
    CHECK(callback_made && !tsuchi::game::object_kept(callback_made) && !tsuchi::game::object_kept(guarded_source));
    restoration_allowed = true;
    const uint64_t guarded_create_source = fake::engine_object(u"GuardedCreateSource", dynamic_type);
    fake::put(guarded_create_source + 0xa0, target);
    const auto guarded_original_make = fake::engine().bodies[make];
    const uint32_t before_create_veto = parameter_copies;
    fake::engine().bodies[make] = [&](uint64_t object, uint64_t args) {
        guarded_original_make(object, args);
        callback_made = fake::get<uint64_t>(args + 0x10);
        restoration_allowed = false;
    };
    CHECK(!tsuchi::drawn_as_restoring(guarded_create_source, restoration_guard, guarded_shown));
    fake::engine().bodies[make] = guarded_original_make;
    CHECK(parameter_copies == before_create_veto && callback_made && !tsuchi::game::object_kept(callback_made));
    CHECK(!tsuchi::game::object_kept(guarded_create_source));
    restoration_allowed = true;
    // Native lookup/allocation/write/invoke refusal is independent of owner veto.
    const auto copy_function_key = std::make_pair(dynamic_type, fake::engine_name(const_cast<char16_t*>(u"K2_CopyMaterialInstanceParameters"), 33, 1));
    CHECK(fake::engine().functions.at(copy_function_key) == copy_all);
    // Inject the native argument-write refusal into the field created above.
    // find_place requires readable argument memory, so base zero is not a field lookup.
    const uint64_t source_argument_kind = fake::get<uint64_t>(source_argument + t::property::kind);
    for (int refusal = 0; refusal < 4; ++refusal) {
        const uint64_t refusal_source = fake::engine_object(u"CopyRefusalSource", dynamic_type);
        fake::put(refusal_source + 0xa0, target);
        const uint32_t copies_before_refusal = parameter_copies;
        const uint32_t stands_before_refusal = counted().stand_ins;
        uint64_t refusal_table = 0;
        if (refusal == 0) {
            CHECK(fake::engine().functions.erase(copy_function_key) == 1);
            CHECK(!tsuchi::game::find_function(dynamic_type, "K2_CopyMaterialInstanceParameters"));
        }
        if (refusal == 2) fake::put(source_argument + t::property::kind, fake::kind_record(u"IntProperty"));
        fake::engine().bodies[make] = [&](uint64_t object, uint64_t args) {
            guarded_original_make(object, args);
            callback_made = fake::get<uint64_t>(args + 0x10);
            if (refusal == 1) fake::engine().out_of_memory = true;
            if (refusal == 3) {
                refusal_table = fake::memory(0x40);
                std::memcpy(reinterpret_cast<void*>(refusal_table), reinterpret_cast<void*>(fake::engine().vtable), 0x40);
                fake::put<uint64_t>(refusal_table + t::object_vtable::process_event, 0);
                fake::put(callback_made, refusal_table);
            }
        };
        callback_made = 0;
        CHECK(tsuchi::drawn_as_restoring(refusal_source, restoration_guard, guarded_shown) && guarded_shown == refusal_source);
        fake::engine().out_of_memory = false;
        fake::engine().bodies[make] = guarded_original_make;
        fake::engine().functions[copy_function_key] = copy_all;
        fake::put(source_argument + t::property::kind, source_argument_kind);
        CHECK(parameter_copies == copies_before_refusal && counted().stand_ins == stands_before_refusal);
        CHECK(callback_made && !tsuchi::game::object_kept(callback_made) && !tsuchi::game::object_kept(refusal_source));
        if (refusal_table) { fake::put(callback_made, fake::engine().vtable); fake::unmap(refusal_table); }
        fake::destroy(callback_made); fake::destroy(refusal_source);
    }
    // Native creation/copy can synchronously change the world. A provisional
    // instance cannot be published into a source record from the departing world.
    const uint64_t world_source = fake::engine_object(u"ReenteredWorldSource", dynamic_type);
    fake::put(world_source + 0xa0, target);
    const uint64_t copy_world = scene.world, other_world = in_world(u"/G/Reentered_P", u"Reentered_P");
    world_on_copy = other_world; callback_made = 0;
    CHECK(tsuchi::drawn_as(world_source) == world_source);
    CHECK(callback_made && !tsuchi::game::object_kept(callback_made) && !tsuchi::game::object_kept(world_source));
    CHECK(tsuchi::drawn_as(target) == target); // no cached plain MID from the departing world
    CHECK(!tsuchi::drawn_as_restoring(target, restoration_guard, guarded_shown) && guarded_shown == target);
    const uint64_t ordinary_original = fake::engine_object(u"OrdinaryOriginal", material_class);
    const uint32_t before_ordinary_restore = parameter_copies;
    CHECK(tsuchi::drawn_as_restoring(0, restoration_guard, guarded_shown) && guarded_shown == 0);
    CHECK(tsuchi::drawn_as_restoring(ordinary_original, restoration_guard, guarded_shown) && guarded_shown == ordinary_original);
    CHECK(parameter_copies == before_ordinary_restore);
    CHECK(fake::get<uint64_t>(image + 0x80 + 72) == shown_c && brush_unchanged());
    scene.world = copy_world; fake::put(main + t::data::world, copy_world);
    CHECK(tsuchi::drawn_as(world_source) != world_source);
    const uint64_t creation_source = fake::engine_object(u"ReenteredCreationSource", dynamic_type);
    fake::put(creation_source + 0xa0, target);
    const auto original_make = fake::engine().bodies[make];
    const uint32_t copies_before_creation = parameter_copies;
    fake::engine().bodies[make] = [&](uint64_t object, uint64_t args) {
        original_make(object, args);
        callback_made = fake::get<uint64_t>(args + 0x10);
        scene.world = other_world; fake::put(main + t::data::world, other_world);
    };
    CHECK(tsuchi::drawn_as(creation_source) == creation_source);
    fake::engine().bodies[make] = original_make;
    CHECK(callback_made && !tsuchi::game::object_kept(callback_made) && !tsuchi::game::object_kept(creation_source));
    CHECK(parameter_copies == copies_before_creation);
    scene.world = copy_world; fake::put(main + t::data::world, copy_world);
    CHECK(tsuchi::drawn_as(creation_source) != creation_source);
    const uint64_t unknown = fake::engine_object(u"UnrecognizedMaterial", material_class);
    fake::put(image + 0x80 + 72, unknown);
    steps(1);
    CHECK(fake::get<uint64_t>(image + 0x80 + 72) == shown_c && tsuchi::game::object_kept(unknown));

    // A pooled particle/decal owner leaves with its originals, then can be reused.
    const uint64_t pooled = tabled(table_a(), u"TargetOwner", u"TargetOwner", 0, 0);
    fake::put(particle + t::scene_component::owner, pooled);
    fake::put(decal + t::scene_component::owner, pooled);
    parts_of[pooled] = {particle, decal};
    registers(pooled);
    steps(1);
    pool_return(kPool, pooled);
    CHECK(override_in(particle, 0) == 0 && override_in(particle, 1) == target);
    CHECK(fake::get<uint64_t>(decal + 0x80) == target);
    fake::put<uint32_t>(pooled + t::game_actor::rest, 0);
    pool_gives = pooled;
    CHECK(pool_take() == pooled);
    steps(1);
    CHECK(override_in(particle, 0) != 0 && fake::get<uint64_t>(decal + 0x80) != target);

    // Destroying a target removes its hold without calling its setter.
    const uint64_t destroyed = fake::engine_object(u"DestroyedImage", image_type, 0x180);
    fake::put(destroyed + 0x80 + 72, target);
    CHECK(tsuchi::hold_material(nullptr, destroyed, 0, target) == TSUCHI_OK);
    fake::destroy(destroyed);
    steps(1);

    // A brush can change from a material to a texture while its material is held.
    const uint64_t texture = fake::engine_object(u"BrushTexture", w.component_class);
    fake::put(image + 0x80 + 72, texture);
    steps(1);
    CHECK(fake::get<uint64_t>(image + 0x80 + 72) == shown_c && brush_unchanged());
    CHECK(tsuchi::game::object_kept(texture) && !tsuchi::game::object_kept(unknown));

    // Provider cancellation vetoes guarded restoration even if its caller and
    // actual world remain eligible. Ordinary copy refusal above is different.
    const uint64_t guarded_stop_source = fake::engine_object(u"GuardedProviderStop", dynamic_type);
    fake::put(guarded_stop_source + 0xa0, target);
    stop_on_copy = tsuchi::find_mod("shade.targets")->context; callback_made = 0;
    CHECK(!tsuchi::drawn_as_restoring(guarded_stop_source, restoration_guard, guarded_shown));
    CHECK(restoration_allowed && callback_made && !tsuchi::game::object_kept(callback_made));
    CHECK(!tsuchi::game::object_kept(guarded_stop_source));
    // Stop restores a null particle override, explicit overrides, decal and Image.
    tsuchi::mod_stop(tsuchi::find_mod("shade.targets")->context);
    CHECK(override_in(particle, 0) == 0 && override_in(particle, 1) == target);
    CHECK(fake::get<uint64_t>(decal + 0x80) == target);
    CHECK(fake::get<uint64_t>(image + 0x80 + 72) == texture && brush_unchanged());
    CHECK(override_in(translucent, 0) == target);
    CHECK(!tsuchi::game::object_kept(source_a) && !tsuchi::game::object_kept(source_b) && !tsuchi::game::object_kept(source_c) && !tsuchi::game::object_kept(texture));
    tsuchi::forget_mods();
    // Ownership moves with a ready source from another shader set.
    disk.files.clear();
    SetMaterial alpha_row = target_row, beta_row = target_row;
    alpha_row.material = "/G/Mods/MI_Alpha.MI_Alpha";
    beta_row.material = "/G/Mods/MI_Beta.MI_Beta";
    set_file("shade.alpha", shader_set({alpha_row}));
    set_file("shade.beta", shader_set({beta_row}));
    CHECK(tsuchi::find_mods() == TSUCHI_OK);
    tsuchi::start_mods();
    steps(30);
    copy_comes(8); look(), look();
    copy_comes(9); look(), look();
    const uint64_t alpha = mod_material(u"MI_Alpha"), beta = mod_material(u"MI_Beta");
    const uint64_t crossing = fake::engine_object(u"CrossSetImage", image_type, 0x180);
    fake::put(crossing + 0x80 + 72, alpha);
    tsuchi::drawn_as(alpha), tsuchi::drawn_as(beta);
    look(), steps(1);
    CHECK(fake::get<uint64_t>(crossing + 0x80 + 72) == tsuchi::drawn_as(alpha));
    fake::put(crossing + 0x80 + 72, beta);
    steps(1);
    const uint64_t beta_shown = tsuchi::drawn_as(beta);
    CHECK(beta_shown != beta && fake::get<uint64_t>(crossing + 0x80 + 72) == beta_shown);
    // Explicit mod holds stay fixed. Their release exposes the latest source,
    // even when release writes its already-created stand-in into the renderer.
    TSUCHI_Mod* overlay = tsuchi::mod_create("shade.overlay");
    const uint64_t crossing_mesh = mesh(u"CrossSetMesh", 1);
    put_override(crossing_mesh, 0, alpha);
    look();
    CHECK(tsuchi::hold_material(overlay, crossing_mesh, 0, alpha) == TSUCHI_OK);
    put_override(crossing_mesh, 0, beta);
    steps(1);
    CHECK(override_in(crossing_mesh, 0) == tsuchi::drawn_as(alpha));
    CHECK(tsuchi::restore_material(overlay, crossing_mesh, 0) == TSUCHI_OK);
    steps(1);
    CHECK(override_in(crossing_mesh, 0) == beta_shown);
    tsuchi::mod_stop(overlay);

    tsuchi::mod_stop(tsuchi::find_mod("shade.alpha")->context);
    CHECK(fake::get<uint64_t>(crossing + 0x80 + 72) == beta_shown && override_in(crossing_mesh, 0) == beta_shown && tsuchi::game::object_kept(beta_shown));
    // Stopping the provider during a native parameter copy frees its source
    // record. No derived entry or retained provisional instance may survive it.
    const uint64_t trigger_source = fake::engine_object(u"CopyTriggerSource", dynamic_type);
    fake::put(trigger_source + 0xa0, beta);
    const uint64_t trigger_shown = tsuchi::drawn_as(trigger_source);
    CHECK(trigger_shown != trigger_source);
    const uint64_t stop_source = fake::engine_object(u"ReenteredStopSource", dynamic_type);
    fake::put(stop_source + 0xa0, beta);
    const uint64_t cached_stop_shown = tsuchi::drawn_as(stop_source);
    CHECK(cached_stop_shown != stop_source);
    direct_stop_provider = tsuchi::find_mod("shade.beta")->context; callback_made = 0;
    direct_copy_trigger = trigger_source; direct_stop_source = stop_source;
    verify_cached_copy_roots = true;
    // Both sources change, so the step copies into both stand-ins.
    edit(trigger_source, [](Values& v) { v.scalars["Color"] = 5; });
    edit(stop_source, [](Values& v) { v.scalars["Color"] = 6; });
    steps(1); // refresh A calls cached B before B's refresh; B stops their provider
    CHECK(direct_stop_exercised && verified_cached_copy_roots && callback_made == cached_stop_shown);
    CHECK(!tsuchi::game::object_kept(trigger_source) && !tsuchi::game::object_kept(trigger_shown));
    CHECK(tsuchi::drawn_as(stop_source) == stop_source);
    CHECK(callback_made && !tsuchi::game::object_kept(callback_made) && !tsuchi::game::object_kept(stop_source));
    tsuchi::mod_stop(tsuchi::find_mod("shade.beta")->context);
    CHECK(fake::get<uint64_t>(crossing + 0x80 + 72) == beta && override_in(crossing_mesh, 0) == beta && !tsuchi::game::object_kept(beta_shown));
    tsuchi::forget_mods();
    fake::destroy(crossing), fake::destroy(crossing_mesh), fake::destroy(alpha), fake::destroy(beta), fake::destroy(stop_source), fake::destroy(trigger_source);
    slots_of.erase(crossing_mesh);

    // Nested creation of the same source publishes one cache entry. The outer
    // provisional MID must not overwrite it or keep an unreachable root.
    disk.files.clear();
    SetMaterial nested_row = target_row;
    nested_row.material = "/G/Mods/MI_Nested.MI_Nested";
    set_file("shade.nested", shader_set({nested_row}));
    CHECK(tsuchi::find_mods() == TSUCHI_OK);
    const uint64_t nested_plain = mod_material(u"MI_Nested");
    tsuchi::start_mods(); steps(30); copy_comes(10);
    std::vector<uint64_t> nested_made;
    uint64_t nested_source = nested_plain, nested_published = 0;
    bool nest_once = true;
    fake::engine().bodies[make] = [&](uint64_t object, uint64_t args) {
        original_make(object, args);
        nested_made.push_back(fake::get<uint64_t>(args + 0x10));
        if (nest_once) {
            nest_once = false;
            nested_published = tsuchi::drawn_as(nested_source);
        }
    };
    look(), look();
    CHECK(nested_made.size() == 2 && nested_published == nested_made[1]);
    CHECK(tsuchi::drawn_as(nested_plain) == nested_published);
    CHECK(!tsuchi::game::object_kept(nested_made[0]) && tsuchi::game::object_kept(nested_made[1]));
    const uint64_t nested_child = fake::engine_object(u"NestedChild", dynamic_type);
    fake::put(nested_child + 0xa0, nested_plain);
    nested_source = nested_child; nest_once = true;
    const uint64_t nested_child_shown = tsuchi::drawn_as(nested_child);
    CHECK(nested_child_shown == nested_published);
    CHECK(nested_made.size() == 4 && nested_published == nested_made[3]);
    CHECK(!tsuchi::game::object_kept(nested_made[2]) && tsuchi::game::object_kept(nested_made[3]));
    CHECK(tsuchi::drawn_as(nested_child) == nested_published);
    fake::engine().bodies[make] = original_make;
    tsuchi::mod_stop(tsuchi::find_mod("shade.nested")->context);
    for (uint64_t made : nested_made) CHECK(!tsuchi::game::object_kept(made));
    CHECK(!tsuchi::game::object_kept(nested_child));
    tsuchi::forget_mods(); fake::destroy(nested_plain); fake::destroy(nested_child);

    // A step makes at most one stand-in: in the game each was a dynamic instance and a copy of values, and five
    // made in one look were one 130 ms step. In a new world every ready material needs one again at once.
    {
        disk.files.clear();
        std::vector<SetMaterial> trio_rows;
        for (const char* path : {"/G/Mods/MI_Trio1.MI_Trio1", "/G/Mods/MI_Trio2.MI_Trio2", "/G/Mods/MI_Trio3.MI_Trio3"}) {
            SetMaterial row = target_row;
            row.material = path;
            trio_rows.push_back(row);
        }
        set_file("shade.trio", shader_set(trio_rows));
        CHECK(tsuchi::find_mods() == TSUCHI_OK);
        const uint64_t trio[] = {mod_material(u"MI_Trio1"), mod_material(u"MI_Trio2"), mod_material(u"MI_Trio3")};
        tsuchi::start_mods(); steps(30);
        copy_comes(11); look(), look();
        copy_comes(12); look(), look();
        copy_comes(13); look(), look();
        for (const uint64_t plain : trio) CHECK(tsuchi::drawn_as(plain) != plain);
        const uint64_t trio_world = scene.world;
        scene.world = in_world(u"/G/Trio_P", u"Trio_P");
        fake::put(main + t::data::world, scene.world);
        uint32_t most = 0, made = 0;
        for (int i = 0; i < 45; ++i) {
            const uint32_t before_step = counted().stand_ins;
            steps(1);
            const uint32_t now_made = counted().stand_ins - before_step;
            most = now_made > most ? now_made : most, made += now_made;
        }
        CHECK(most == 1 && made == 3);
        for (const uint64_t plain : trio) CHECK(tsuchi::drawn_as(plain) != plain);
        tsuchi::mod_stop(tsuchi::find_mod("shade.trio")->context);
        tsuchi::forget_mods();
        for (const uint64_t plain : trio) fake::destroy(plain);
        scene.world = trio_world, fake::put(main + t::data::world, trio_world);
        steps(1);
    }

    fake::engine().bodies.erase(copy_all); // the callback captured this block's parameter table
    parts_of.erase(pooled); parts_of.erase(target_owner);
    for (uint64_t object : {particle, decal, image, translucent, target_owner, target, source_a, source_b, pooled, texture, target_parent, source_c, unknown, world_source, creation_source, parameter_texture_a, parameter_texture_b, guarded_source, guarded_create_source, guarded_stop_source, ordinary_original}) fake::destroy(object);
    slots_of.erase(particle); slots_of.erase(translucent);
    scene.world = prior_world;
    fake::put(main + t::data::world, prior_world);
    stock_material = saved_stock;
    disk.files = saved_files;
    rendered.log = saved_render_log; // previous checks retain their own six-shader log
    steps(1);
}
