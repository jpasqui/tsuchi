// The real maps.cpp through its incoming-world hook and exact typed native
// adapters. The made-up mode-2 flush visits only its entry snapshot: descendants
// added from level initialization require a later native flush, just as a final
// native iteration is allowed to leave deferred admissions unprocessed.
namespace initial_fixture {
constexpr uint32_t kBytes = 0x200, kAsset = 0x40, kPut = 0x80, kStarts = 0xB0,
                   kVolumes = 0xC0, kCustom = 0xD0, kPriority = kCustom + 4, kStreams = 0xA0,
                   kLevelVisible = 0x1A0;
struct Scene {
    uint64_t world_type = 0, level_type = 0, stream_type = 0, dynamic_type = 0, custom_type = 0, volume_type = 0, functions = 0;
    uint64_t world = 0, persistent = 0, rule_type = 0, part = 0;
    std::vector<uint64_t> constructed, duplicated, added, removed;
    std::vector<uint64_t> late_streams;
    std::map<uint64_t, std::vector<uint64_t>> enrolled;
    std::map<uint64_t, uint64_t> duplicate_source;
    std::map<std::string, std::vector<uint64_t>> children;
    std::map<uint64_t, bool> visible;
    std::set<uint64_t> blocked_arrivals;
    std::vector<std::pair<uint64_t, uint32_t>> flushes;
    std::vector<std::string> order;
    std::vector<uint16_t> policy_at_add;
    std::function<void()> on_construct, on_duplicate, on_package, on_add, on_flush, on_mode;
    std::function<void(uint64_t)> on_visible;
    bool fail_construct = false, fail_duplicate = false, refuse_add = false, never_arrive = false;
    bool wrong_class = false, wrong_outer = false, stale_loaded = false;
    uint32_t corrupt_clone = 0;
    uint16_t new_policy = 0x4512;
};
Scene* scene = nullptr;
uint64_t next_fixture_part = 1; // this focused process declares one maps part per preparation
uint64_t legacy_visible_calls = 0;

void array(uint64_t at, const std::vector<uint64_t>& values) {
    const uint64_t data = values.empty() ? 0 : fake::memory(values.size() * 8);
    for (size_t i = 0; i < values.size(); ++i) fake::put(data + i * 8, values[i]);
    fake::put(at + t::array::data, data);
    fake::put<int32_t>(at + t::array::count, int32_t(values.size()));
    fake::put<int32_t>(at + t::array::max, int32_t(values.size()));
}
std::string text(uint64_t packed) {
    char out[256] = {};
    name_text(Name{uint32_t(packed), uint32_t(packed >> 32)}, out, sizeof out);
    return out;
}
std::string package(uint64_t stream) {
    std::string path = text(fake::get<uint64_t>(stream + kAsset + t::soft_object::path));
    const size_t dot = path.find('.');
    if (dot != std::string::npos) path.resize(dot);
    return path;
}
void identity(uint64_t stream) {
    fake::put<float>(stream + kPut + 12, 1);
    for (uint32_t i = 7; i < 10; ++i) fake::put<float>(stream + kPut + i * 4, 1);
}
uint64_t descriptor(uint64_t type, uint64_t outer, const std::u16string& name) {
    const uint64_t made = fake::engine_object(name, type, kBytes);
    fake::put(made + t::object::outer, outer);
    fake::put<uint16_t>(made + t::streaming_level::policy, scene->new_policy);
    identity(made);
    return made;
}
void callback(const std::function<void()>& action) {
    // Copy before invoking: a callback can replace its own slot.
    const auto retained = action;
    if (retained) retained();
}
uint64_t mode_flags(void*) {
    if (scene) callback(scene->on_mode);
    return 0;
}
void* construct(const void* arguments) {
    auto& s = *scene;
    const auto* words = static_cast<const uint64_t*>(arguments);
    CHECK(words[0] == s.dynamic_type && words[1] == s.world);
    for (uint32_t i = 2; i < 8; ++i) CHECK(words[i] == 0);
    s.order.push_back("construct");
    if (s.fail_construct) return nullptr;
    const uint64_t made = descriptor(s.wrong_class ? w.component_class : words[0], s.wrong_outer ? 0 : words[1], u"InitialRoot");
    if (s.stale_loaded) fake::put<uint64_t>(made + t::streaming_level::loaded, s.persistent);
    s.constructed.push_back(made);
    callback(s.on_construct);
    return reinterpret_cast<void*>(made);
}
void* duplicate(const void* source_pointer, void* outer_pointer, uint64_t name, uint32_t flags,
                void* destination_class, int32_t mode, uint32_t internal_flags) {
    auto& s = *scene;
    const uint64_t source = reinterpret_cast<uint64_t>(source_pointer), outer = reinterpret_cast<uint64_t>(outer_pointer);
    CHECK(source && outer == s.world && destination_class == nullptr && mode == 0);
    CHECK(name == 0);
    CHECK(flags == 0x0FFFFFFFu && internal_flags == uint32_t(t::constant::duplicate_internal_flags));
    if (!source || !readable(source, kBytes)) return nullptr;
    s.order.push_back("duplicate");
    if (s.fail_duplicate) return nullptr;
    const uint64_t made = descriptor((s.wrong_class || (s.corrupt_clone & 64)) ? w.component_class : type_of(source), s.wrong_outer ? 0 : outer, u"InitialChild");
    std::memcpy(reinterpret_cast<void*>(made + t::object::size), reinterpret_cast<const void*>(source + t::object::size),
                kBytes - t::object::size);
    std::vector<uint64_t> volumes;
    const uint64_t values = fake::get<uint64_t>(source + kVolumes + t::array::data);
    const int32_t count = fake::get<int32_t>(source + kVolumes + t::array::count);
    for (int32_t i = 0; i < count; ++i) volumes.push_back(fake::get<uint64_t>(values + uint64_t(i) * 8));
    array(made + kVolumes, volumes);
    // Serialized copying omits the native transient level references.
    fake::put<uint64_t>(made + t::streaming_level::loaded, s.stale_loaded ? s.persistent : 0);
    fake::put<uint64_t>(made + t::streaming_level::pending_unload, 0);
    fake::put<uint8_t>(made + t::streaming_level::state, 0);
    if (s.corrupt_clone & 1) fake::put<int32_t>(made + kPriority, fake::get<int32_t>(made + kPriority) + 1);
    if (s.corrupt_clone & 2) fake::put<uint16_t>(made + t::streaming_level::policy, uint16_t(fake::get<uint16_t>(made + t::streaming_level::policy) ^ 0x4000));
    for (uint32_t i = 0; i < 4; ++i)
        if (s.corrupt_clone & (4u << i)) fake::put<uint8_t>(made + kStarts + i, uint8_t(!fake::get<uint8_t>(made + kStarts + i)));
    s.duplicated.push_back(made), s.duplicate_source[made] = source;
    callback(s.on_duplicate);
    return reinterpret_cast<void*>(made);
}
void set_package(void* pointer, uint64_t packed) {
    auto& s = *scene;
    const uint64_t stream = reinterpret_cast<uint64_t>(pointer);
    const std::string path = text(packed);
    const size_t slash = path.find_last_of('/');
    const std::string asset = path + "." + path.substr(slash == std::string::npos ? 0 : slash + 1);
    std::u16string wide(asset.begin(), asset.end());
    fake::put<uint64_t>(stream + kAsset + t::soft_object::path, fake::engine_name(wide.data(), wide.size(), 1));
    s.order.push_back("package:" + path);
    callback(s.on_package);
}
void add(void* world_pointer, void* stream_pointer) {
    auto& s = *scene;
    const uint64_t world = reinterpret_cast<uint64_t>(world_pointer), stream = reinterpret_cast<uint64_t>(stream_pointer);
    CHECK(world == s.world && live(stream) && outer_of(stream) == world && derives_from(type_of(stream), s.stream_type));
    if (!world || !readable(world, kBytes) || !stream || !readable(stream, kBytes)) return;
    CHECK(fake::get<uint8_t>(stream + t::streaming_level::state) == 0);
    CHECK(fake::get<uint64_t>(stream + t::streaming_level::loaded) == 0);
    CHECK(fake::get<uint64_t>(stream + t::streaming_level::pending_unload) == 0);
    s.order.push_back("add:" + package(stream));
    s.added.push_back(stream), s.policy_at_add.push_back(fake::get<uint16_t>(stream + t::streaming_level::policy));
    if (!s.refuse_add) {
        auto& streams = s.enrolled[world];
        CHECK(std::find(streams.begin(), streams.end(), stream) == streams.end());
        streams.push_back(stream), array(world + kStreams, streams);
        fake::put<uint8_t>(stream + t::streaming_level::state, 1);
    }
    callback(s.on_add);
}
bool remove(void* world_pointer, void* stream_pointer) {
    auto& s = *scene;
    const uint64_t world = reinterpret_cast<uint64_t>(world_pointer), stream = reinterpret_cast<uint64_t>(stream_pointer);
    s.order.push_back("remove:" + package(stream)), s.removed.push_back(stream);
    auto& streams = s.enrolled[world];
    const auto at = std::find(streams.begin(), streams.end(), stream);
    if (at == streams.end()) return false;
    streams.erase(at), array(world + kStreams, streams);
    fake::put<uint8_t>(stream + t::streaming_level::state, 0);
    return true;
}
void enter(uint64_t world, uint32_t mode = 2) {
    thunks::Dispatch dispatch(tsuchi::kCodeSiteThunk | t::code_site::map_initial_flush.number);
    thunks::call(dispatch.selected.target, world, mode);
}
uint64_t make_world(const std::u16string& path = u"/G/Initial_P") {
    auto& s = *scene;
    const uint64_t owner = fake::object(path, w.root), world = fake::engine_object(u"Initial_P", s.world_type, kBytes);
    fake::put(world + t::object::outer, owner);
    const uint64_t level = fake::engine_object(u"PersistentLevel", s.level_type, kBytes);
    fake::put(level + t::object::outer, world), fake::put(level + t::engine_level::owning_world, world);
    fake::put(world + t::world::persistent_level, level);
    const uint64_t rule = fake::engine_object(u"Rule", s.rule_type, kBytes);
    fake::put(rule + t::object::outer, level), fake::put<uint8_t>(rule + 0x80, t::constant::rule_story);
    array(level + t::engine_level::actors, {rule});
    return world;
}
void declare(const char* id, const char* levels = R"(["/G/Mods/Root"])" ) {
    const std::string folder = std::string(tsuchi::kModsFolder) + "/" + id + "/mod.json";
    disk.files[folder] = std::string(R"({"id":")") + id + R"(","name":"Initial maps","version":"1.0.0","maps":[{"map":"/G/Initial_P","levels":)" + levels + "}]}";
}
TSUCHI_Mod* prepare(const char* id = "map.initial", const char* levels = R"(["/G/Mods/Root"])" ) {
    disk.files.clear(), logged.lines.clear();
    declare(id, levels);
    CHECK(tsuchi::find_mods() == TSUCHI_OK);
    tsuchi::prepare_mods();
    auto* record = tsuchi::find_mod(id);
    CHECK(record && record->state == tsuchi::ModState::ready && record->context);
    scene->part = next_fixture_part++;
    CHECK(tsuchi::map_parts() == 1 && !tsuchi::map_part_failed(scene->part));
    return record ? record->context : nullptr;
}
bool ready_after_play() {
    // Observe the service at the next native play boundary without running a
    // world step that could mask late loading or alter the admission counts.
    fake::put<uint8_t>(w.framework + t::game_framework::world_begun, 1);
    const bool ready = tsuchi::map_part_ready(scene->part);
    fake::put<uint8_t>(w.framework + t::game_framework::world_begun, 0);
    return ready;
}
void refusal_stays_unready() {
    auto& s = *scene;
    const auto counts = tsuchi::map_counts();
    const size_t added = s.added.size(), constructed = s.constructed.size(), flushes = s.flushes.size();
    fake::put<uint8_t>(w.framework + t::game_framework::world_begun, 1);
    for (uint32_t i = 0; i < 33; ++i) tsuchi::frame(0.03f);
    CHECK(!tsuchi::map_part_ready(s.part));
    CHECK(tsuchi::map_part_failed(s.part)); // terminal refusal, not another delayed loading attempt
    fake::put<uint8_t>(w.framework + t::game_framework::world_begun, 0);
    const auto after = tsuchi::map_counts();
    CHECK(after.levels_asked == counts.levels_asked && after.levels_refused == counts.levels_refused);
    CHECK(s.added.size() == added && s.constructed.size() == constructed && s.flushes.size() == flushes);
}
uint64_t child(const char* asset, bool loaded = true, bool visible = true, uint64_t volume = 0) {
    auto& s = *scene;
    const uint64_t made = descriptor(s.custom_type, 0, u"SavedChild");
    const std::string path(asset);
    std::u16string wide(path.begin(), path.end());
    fake::put<uint64_t>(made + kAsset + t::soft_object::path, fake::engine_name(wide.data(), wide.size(), 1));
    fake::put<uint8_t>(made + kStarts, loaded), fake::put<uint8_t>(made + kStarts + 1, visible);
    fake::put<uint8_t>(made + kStarts + 2, loaded), fake::put<uint8_t>(made + kStarts + 3, visible);
    fake::put<uint32_t>(made + kCustom, 0xA1B2C3D4);
    fake::put<int32_t>(made + kPriority, -7);
    if (volume) array(made + kVolumes, {volume});
    return made;
}
void reset(Scene& s) {
    tsuchi::forget_mods();
    s.constructed.clear(), s.duplicated.clear(), s.added.clear(), s.removed.clear(), s.late_streams.clear(), s.duplicate_source.clear();
    s.enrolled.clear(), s.children.clear(), s.visible.clear(), s.blocked_arrivals.clear(), s.flushes.clear(), s.order.clear(), s.policy_at_add.clear();
    s.on_construct = {}, s.on_duplicate = {}, s.on_package = {}, s.on_add = {}, s.on_flush = {}, s.on_mode = {};
    s.on_visible = {};
    s.fail_construct = s.fail_duplicate = s.refuse_add = s.never_arrive = false;
    s.wrong_class = s.wrong_outer = s.stale_loaded = false;
    s.corrupt_clone = 0;
    s.world = make_world(), s.persistent = fake::get<uint64_t>(s.world + t::world::persistent_level);
    fake::put(fake::main_image() + t::data::world, s.world);
    fake::put<uint8_t>(w.framework + t::game_framework::world_begun, 0);
}
} // namespace initial_fixture

void initial_native_flush(uint64_t world, uint32_t mode) {
    using namespace initial_fixture;
    if (!scene) return;
    auto& s = *scene;
    s.flushes.push_back({world, mode}), s.order.push_back("flush:" + std::to_string(mode));
    const auto snapshot = s.enrolled[world];
    callback(s.on_flush);
    for (uint64_t stream : snapshot) {
        if (s.never_arrive || s.blocked_arrivals.count(stream) || fake::get<uint64_t>(stream + t::streaming_level::loaded) ||
            !(fake::get<uint8_t>(stream + t::streaming_level::policy) & t::constant::stream_block_load) ||
            !fake::get<uint8_t>(stream + kStarts + 2)) continue;
        const std::string path = package(stream);
        const std::u16string saved_name(path.begin(), path.end());
        const uint64_t saved = fake::engine_object(saved_name, s.world_type, kBytes), level = fake::engine_object(u"PersistentLevel", s.level_type, kBytes);
        fake::put(level + t::object::outer, saved), fake::put(level + t::engine_level::owning_world, world);
        array(saved + kStreams, s.children[path]);
        // These are actors of the level that actually arrived, not an invented
        // remapping performed by the descriptor-duplication mock.
        for (uint64_t source : s.children[path]) {
            fake::put(source + t::object::outer, saved);
            const uint64_t volumes = fake::get<uint64_t>(source + kVolumes + t::array::data);
            const int32_t count = fake::get<int32_t>(source + kVolumes + t::array::count);
            for (int32_t i = 0; i < count; ++i)
                fake::put(fake::get<uint64_t>(volumes + uint64_t(i) * 8) + t::object::outer, level);
        }
        fake::put(stream + t::streaming_level::loaded, level);
        s.visible[stream] = fake::get<uint8_t>(stream + kStarts + 3) != 0;
        fake::put<uint8_t>(level + kLevelVisible, s.visible[stream] ? 0x20 : 0);
        fake::put<uint8_t>(stream + t::streaming_level::state, s.visible[stream] ? 5 : 4);
        thunks::Dispatch dispatch(tsuchi::kCodeSiteThunk | t::code_site::level_prepare.number);
        thunks::call(dispatch.selected.target, level, 7, 0, 0x1234);
    }
}

void initial_streaming() {
    using namespace initial_fixture;
    Scene s; scene = &s;
    fake::start_engine();
    s.functions = fake::object(u"Function", w.root);
    s.world_type = fake::object(u"World", w.root), s.level_type = fake::object(u"Level", w.root);
    s.stream_type = fake::object(u"LevelStreaming", w.root), s.dynamic_type = fake::object(u"LevelStreamingDynamic", w.root);
    s.custom_type = fake::object(u"InitialCustomStreaming", w.root);
    fake::set_super(s.dynamic_type, s.stream_type), fake::set_super(s.custom_type, s.stream_type);
    const uint64_t actor_type = fake::object(u"Actor", w.root);
    fake::set_super(w.actor_class, actor_type);
    s.volume_type = fake::object(u"LevelStreamingVolume", w.root), fake::set_super(s.volume_type, actor_type);
    s.rule_type = fake::object(u"CarrotGameRule", w.root);
    fake::set_super(s.rule_type, actor_type);
    fake::field(s.rule_type, u"GameRule", u"ByteProperty", 0x80, 1);
    fake::field(s.level_type, u"OwningWorld", u"ObjectProperty", t::engine_level::owning_world, 8, s.world_type);
    fake::bit_field(s.level_type, u"bIsVisible", kLevelVisible, 0, 0x20);
    fake::field(s.world_type, u"StreamingLevels", u"ArrayProperty", kStreams, 0x10, fake::loose_field(u"ObjectProperty", 8, s.stream_type));
    fake::field(s.stream_type, u"WorldAsset", u"SoftObjectProperty", kAsset, 0x30, s.world_type);
    fake::field(s.stream_type, u"LoadedLevel", u"ObjectProperty", t::streaming_level::loaded, 8, s.level_type);
    fake::field(s.stream_type, u"StreamingPriority", u"IntProperty", kPriority, 4);
    fake::field(s.stream_type, u"EditorStreamingVolumes", u"ArrayProperty", kVolumes, 0x10, fake::loose_field(u"ObjectProperty", 8, s.volume_type));
    for (const auto& field : {std::pair<const char16_t*, uint32_t>{u"bInitiallyLoaded", kStarts},
                            {u"bInitiallyVisible", kStarts + 1}, {u"bShouldBeLoaded", kStarts + 2}, {u"bShouldBeVisible", kStarts + 3}})
        fake::bit_field(s.stream_type, field.first, int32_t(field.second), 0, 0xFF);
    const uint64_t vector = fake::object(u"InitialVector", w.root), quaternion = fake::object(u"InitialQuat", w.root), transform = fake::object(u"InitialTransform", w.root);
    for (uint32_t i = 0; i < 3; ++i) fake::field(vector, i == 0 ? u"X" : i == 1 ? u"Y" : u"Z", u"FloatProperty", int32_t(i * 4), 4);
    for (uint32_t i = 0; i < 4; ++i) fake::field(quaternion, i == 0 ? u"X" : i == 1 ? u"Y" : i == 2 ? u"Z" : u"W", u"FloatProperty", int32_t(i * 4), 4);
    fake::field(transform, u"Rotation", u"StructProperty", 0, 16, quaternion);
    fake::field(transform, u"Translation", u"StructProperty", 16, 12, vector);
    fake::field(transform, u"Scale3D", u"StructProperty", 28, 12, vector);
    fake::field(s.stream_type, u"LevelTransform", u"StructProperty", kPut, 40, transform);
    const uint64_t in = t::constant::property_param, result = in | t::constant::property_out | t::constant::property_return;
    const uint64_t shown = fake::function(s.stream_type, s.functions, u"IsLevelVisible", 1, 1,
        [&](uint64_t object, uint64_t args) {
            ++legacy_visible_calls;
            const bool visible = s.visible[object];
            const auto retained = s.on_visible;
            if (retained) retained(object);
            fake::put<uint8_t>(args, visible);
        });
    fake::flag(fake::bit_field(shown, u"ReturnValue", 0, 0, 0xFF), result);
    for (const auto& setter : {std::pair<const char16_t*, uint32_t>{u"SetShouldBeLoaded", kStarts + 2}, {u"SetShouldBeVisible", kStarts + 3}}) {
        const uint32_t offset = setter.second;
        const uint64_t function = fake::function(s.stream_type, s.functions, setter.first, 1, 1,
            [offset](uint64_t object, uint64_t args) { fake::put<uint8_t>(object + offset, fake::get<uint8_t>(args)); });
        fake::flag(fake::bit_field(function, offset == kStarts + 2 ? u"bInShouldBeLoaded" : u"bInShouldBeVisible", 0, 0, 0xFF), in);
    }
    t::fn::ui_construct = construct, t::fn::map_duplicate_stream = duplicate, t::fn::map_set_package = set_package;
    t::fn::map_add_stream = add, t::fn::map_remove_stream = remove;
    t::fn::fs_open_file = game_fs_open_file, t::fn::fs_close_file = game_fs_close_file;
    t::fn::fs_file_size = game_fs_file_size, t::fn::fs_read_file = game_fs_read_file;
    t::fn::fs_open_folder = game_fs_open_folder, t::fn::fs_read_folder = game_fs_read_folder, t::fn::fs_close_folder = game_fs_close_folder;
    const uint64_t context = fake::memory(0x200), instance = fake::memory(0x100), story = fake::memory(0x200);
    fake::put(fake::main_image() + t::data::game_context, context), fake::put(fake::main_image() + t::data::game_instance, instance);
    fake::put(instance + t::game_instance::story_save, story);
    fake::put<uint8_t>(context + t::game_context::rule, t::constant::rule_story);
    fake::put(fake::main_image() + t::data::game_framework, w.framework);
    fake::put(w.framework + t::game_framework::clock, fake::memory(0x20));
    t::fn::story_flags = mode_flags;
    tsuchi::set_log_sink(log_sink);

    // Cases are kept below the adapters so they exercise the real hook/service.
    reset(s);
    TSUCHI_Mod* context_before = prepare();
    CHECK(s.added.empty() && s.flushes.empty());
    enter(s.world);
    CHECK(s.constructed.size() == 1 && s.added.size() == 1 && s.flushes.size() == 1);
    CHECK(ready_after_play());
    CHECK(s.order.size() >= 4 && s.order[0] == "construct" && s.order[1] == "package:/G/Mods/Root" && s.order[2] == "add:/G/Mods/Root" && s.order[3] == "flush:2");
    if (!s.constructed.empty()) {
        const uint64_t root = s.constructed.front();
        CHECK(s.visible[root] && fake::get<uint16_t>(root + t::streaming_level::policy) == s.new_policy);
        CHECK(!s.policy_at_add.empty());
        if (!s.policy_at_add.empty()) CHECK(s.policy_at_add[0] == uint16_t(s.new_policy | t::constant::stream_block_load));
    }
    enter(s.world);
    CHECK(s.constructed.size() == 1 && s.added.size() == 1 && s.flushes.size() == 2);
    tsuchi::start_mods();
    const auto* adopted = tsuchi::find_mod("map.initial");
    CHECK(adopted && adopted->context == context_before);

    // A flush may finish its last native iteration immediately after a child
    // was queued. The framework must perform bounded additional mode-2 work,
    // without replacing it by 300 world steps or a different flush mode.
    reset(s);
    const uint64_t first = child("/G/Mods/First.First"), second = child("/G/Mods/Second.Second");
    fake::put<float>(first + kPut + 16, 120), fake::put<uint16_t>(first + t::streaming_level::policy, 0x3535);
    fake::put<uint16_t>(second + t::streaming_level::policy, 0x1292); // an existing block request remains set
    s.children["/G/Mods/Root"] = {first};
    s.children["/G/Mods/First"] = {second};
    prepare();
    enter(s.world);
    CHECK(s.constructed.size() == 1 && s.duplicated.size() == 2 && s.added.size() == 3 && s.flushes.size() == 3);
    CHECK(ready_after_play());
    for (const auto& flushed : s.flushes) CHECK(flushed.first == s.world && flushed.second == 2);
    for (uint64_t made : s.duplicated) {
        const uint64_t source = s.duplicate_source[made];
        CHECK(type_of(made) == s.custom_type && outer_of(made) == s.world);
        CHECK(fake::get<uint32_t>(made + kCustom) == 0xA1B2C3D4);
        CHECK(fake::get<int32_t>(made + kPriority) == -7);
        CHECK(fake::get<uint16_t>(made + t::streaming_level::policy) == fake::get<uint16_t>(source + t::streaming_level::policy));
        CHECK(s.visible[made] && fake::get<uint64_t>(made + t::streaming_level::loaded));
        if (source == first) CHECK(fake::get<float>(made + kPut + 16) == 120);
    }
    for (size_t i = 0; i < s.added.size(); ++i) {
        const auto source = s.duplicate_source.find(s.added[i]);
        const uint16_t before = source == s.duplicate_source.end() ? s.new_policy : fake::get<uint16_t>(source->second + t::streaming_level::policy);
        CHECK(s.policy_at_add[i] == uint16_t(before | t::constant::stream_block_load));
    }
    CHECK(fake::get<uint64_t>(first + t::streaming_level::loaded) == 0 && fake::get<uint64_t>(second + t::streaming_level::loaded) == 0);
    const size_t enrolled_once = s.added.size();
    enter(s.world);
    CHECK(s.added.size() == enrolled_once && s.flushes.size() == 4);

    // The final frontier can invalidate a level accepted during an earlier
    // frontier. Every accepted descriptor must be revalidated before readiness.
    for (uint32_t mutation = 0; mutation < 5; ++mutation) {
        reset(s);
        s.children["/G/Mods/Root"] = {child("/G/Mods/First.First")};
        s.children["/G/Mods/First"] = {child("/G/Mods/Second.Second")};
        prepare(); logged.lines.clear();
        bool changed = false;
        s.on_flush = [&] {
            if (s.flushes.size() != 3 || s.constructed.empty()) return;
            changed = true;
            const uint64_t root = s.constructed.front();
            if (mutation == 0) {
                fake::put<uint64_t>(root + t::streaming_level::loaded, 0);
                s.blocked_arrivals.insert(root); // leave this unload observable
            } else if (mutation == 1) {
                const uint64_t saved = fake::engine_object(u"ReplacementSaved", s.world_type, kBytes);
                const uint64_t level = fake::engine_object(u"ReplacementLevel", s.level_type, kBytes);
                fake::put(level + t::object::outer, saved), fake::put(level + t::engine_level::owning_world, s.world);
                fake::put<uint8_t>(level + kLevelVisible, 0x20);
                fake::put(root + t::streaming_level::loaded, level);
            } else if (mutation == 2) {
                s.visible[root] = false;
                fake::put<uint8_t>(fake::get<uint64_t>(root + t::streaming_level::loaded) + kLevelVisible, 0);
            }
            else fake::put<uint8_t>(root + kStarts + (mutation == 3 ? 2 : 3), 0);
        };
        enter(s.world);
        CHECK(changed && s.added.size() == 3 && s.duplicated.size() == 2 && s.flushes.size() == 3);
        CHECK(!ready_after_play() && tsuchi::map_part_failed(s.part));
        CHECK(logged.lines.find("initial streaming refused") != std::string::npos && s.removed.empty());
        for (uint64_t stream : s.added) CHECK(fake::get<uint16_t>(stream + t::streaming_level::policy) == s.new_policy);
    }

    // The legacy native visibility getter remains installed, but initial graph
    // qualification uses Level.bIsVisible without entering that callback seam.
    reset(s);
    s.children["/G/Mods/Root"] = {child("/G/Mods/First.First")};
    s.children["/G/Mods/First"] = {child("/G/Mods/Second.Second")};
    prepare(); logged.lines.clear();
    bool legacy_getter_called = false;
    s.on_visible = [&](uint64_t) {
        legacy_getter_called = true;
        if (!s.constructed.empty())
            fake::put<uint64_t>(s.constructed.front() + t::streaming_level::loaded, 0);
    };
    enter(s.world); s.on_visible = {};
    CHECK(!legacy_getter_called && legacy_visible_calls == 0 && s.added.size() == 3 && s.duplicated.size() == 2 && s.flushes.size() == 3);
    CHECK(ready_after_play() && !tsuchi::map_part_failed(s.part));

    reset(s);
    const uint64_t repeated = child("/G/Mods/Repeat.Repeat");
    s.children["/G/Mods/Root"] = {repeated, repeated};
    prepare(); logged.lines.clear(); enter(s.world);
    CHECK(s.duplicated.size() == 1 && s.added.size() == 2 && s.flushes.size() == 1);
    CHECK(logged.lines.find("initial streaming refused") != std::string::npos);

    // An initially loaded but hidden descendant is loaded in the same initial
    // phase and stays hidden. Volume references still name the actual arrived
    // parent level, and the serialized custom class and data survive copying.
    reset(s);
    const uint64_t volume = fake::engine_object(u"InitialVolume", s.volume_type, kBytes);
    const uint64_t hidden = child("/G/Mods/Hidden.Hidden", true, false, volume);
    s.children["/G/Mods/Root"] = {hidden};
    prepare(); enter(s.world);
    CHECK(s.duplicated.size() == 1 && s.flushes.size() == 2);
    CHECK(ready_after_play());
    if (!s.duplicated.empty()) {
        const uint64_t copy = s.duplicated.front(), values = fake::get<uint64_t>(copy + kVolumes + t::array::data);
        CHECK(type_of(copy) == s.custom_type && fake::get<uint32_t>(copy + kCustom) == 0xA1B2C3D4);
        CHECK(fake::get<uint64_t>(copy + t::streaming_level::loaded) && !s.visible[copy]);
        CHECK(fake::get<uint8_t>(copy + kStarts + 1) == 0 && fake::get<uint8_t>(copy + kStarts + 3) == 0);
        CHECK(fake::get<int32_t>(copy + kVolumes + t::array::count) == 1 && values);
        if (values) CHECK(fake::get<uint64_t>(values) == volume);
        CHECK(!s.constructed.empty());
        if (!s.constructed.empty()) CHECK(outer_of(volume) == fake::get<uint64_t>(s.constructed.front() + t::streaming_level::loaded));
        CHECK(fake::get<uint16_t>(copy + t::streaming_level::policy) == s.new_policy);
        CHECK(values != fake::get<uint64_t>(hidden + kVolumes + t::array::data));
        CHECK(s.policy_at_add.size() == 2);
        if (s.policy_at_add.size() == 2) CHECK(s.policy_at_add[1] == uint16_t(s.new_policy | t::constant::stream_block_load));
    }

    // Unloaded descendants retain their load/visibility policy; they cannot be
    // force-loaded merely to claim that their own nested graph was inspected.
    reset(s);
    const uint64_t dormant = child("/G/Mods/Dormant.Dormant", false, false);
    s.children["/G/Mods/Root"] = {dormant};
    prepare(); logged.lines.clear(); enter(s.world);
    CHECK(s.duplicated.size() == 1 && s.flushes.size() <= 2);
    if (!s.duplicated.empty()) {
        const uint64_t copy = s.duplicated.front();
        CHECK(fake::get<uint64_t>(copy + t::streaming_level::loaded) == 0);
        CHECK(fake::get<uint8_t>(copy + kStarts + 2) == 0 && fake::get<uint8_t>(copy + kStarts + 3) == 0);
        CHECK(fake::get<uint16_t>(copy + t::streaming_level::policy) == s.new_policy);
    }
    CHECK(logged.lines.find("required graph is unavailable, unloaded") != std::string::npos);
    CHECK(logged.lines.find("no delayed fallback was requested") != std::string::npos);
    CHECK(!ready_after_play() && tsuchi::map_part_failed(s.part));

    // Native duplication must preserve the serialized priority, whole policy
    // word, and each initial/current load/visibility flag before admission.
    for (uint32_t corruption : {1u, 2u, 4u, 8u, 16u, 32u, 64u}) {
        reset(s);
        const uint64_t authored = child("/G/Mods/Policy.Policy");
        s.children["/G/Mods/Root"] = {authored};
        prepare(); logged.lines.clear(); s.corrupt_clone = corruption; enter(s.world);
        CHECK(s.constructed.size() == 1 && s.duplicated.size() == 1 && s.added.size() == 1 && s.flushes.size() == 1);
        CHECK(!ready_after_play() && tsuchi::map_part_failed(s.part));
        CHECK(logged.lines.find("initial streaming refused") != std::string::npos);
        CHECK(fake::get<int32_t>(authored + kPriority) == -7);
        CHECK(fake::get<uint16_t>(authored + t::streaming_level::policy) == s.new_policy);
        for (uint32_t i = 0; i < 4; ++i) CHECK(fake::get<uint8_t>(authored + kStarts + i) == 1);
    }

    // A copied pointer alone is not a valid volume reference: its actor class
    // still has to qualify against the native streaming-volume class.
    reset(s);
    const uint64_t invalid_volume = fake::engine_object(u"WrongVolumeClass", w.component_class, kBytes);
    s.children["/G/Mods/Root"] = {child("/G/Mods/Volume.Volume", true, false, invalid_volume)};
    prepare(); logged.lines.clear(); enter(s.world);
    CHECK(s.duplicated.size() == 1 && s.added.size() == 1 && s.flushes.size() == 1);
    CHECK(!ready_after_play() && logged.lines.find("initial streaming refused") != std::string::npos);

    // Native construction and returned identity are checked before enrollment.
    for (uint32_t failure = 0; failure < 4; ++failure) {
        reset(s); prepare();
        s.fail_construct = failure == 0, s.wrong_class = failure == 1;
        s.wrong_outer = failure == 2, s.stale_loaded = failure == 3;
        enter(s.world);
        CHECK(s.added.empty() && s.flushes.size() == 1 && s.flushes[0].second == 2);
    }

    // Missing arrival is bounded; the caller's block policy is restored even
    // when the package never completes its modeled native transition.
    reset(s); prepare(); s.never_arrive = true; enter(s.world);
    CHECK(s.added.size() == 1 && !s.flushes.empty() && s.flushes.size() <= 9);
    if (!s.constructed.empty()) CHECK(fake::get<uint16_t>(s.constructed.front() + t::streaming_level::policy) == s.new_policy);

    // Void native enrollment is checked by its resulting native state, rather
    // than treating the native return alone as success.
    reset(s); prepare(); s.refuse_add = true; enter(s.world);
    CHECK(s.added.size() == 1 && s.enrolled[s.world].empty() && s.flushes.size() == 1);
    if (!s.constructed.empty()) CHECK(fake::get<uint16_t>(s.constructed.front() + t::streaming_level::policy) == s.new_policy);

    // A failed descendant copy happens after its parent has progressed. It
    // cannot be represented as unloading visible content through the narrow
    // provisional-remove adapter.
    reset(s);
    s.children["/G/Mods/Root"] = {child("/G/Mods/Failed.Failed")};
    prepare(); s.fail_duplicate = true; enter(s.world);
    CHECK(s.constructed.size() == 1 && s.duplicated.empty() && s.added.size() == 1 && s.flushes.size() == 1);
    CHECK(s.removed.empty());
    if (!s.constructed.empty()) CHECK(fake::get<uint16_t>(s.constructed.front() + t::streaming_level::policy) == s.new_policy);

    // A graph deeper than the admitted bound terminates in this initial phase.
    // It does not schedule an unbounded series of native flushes.
    reset(s);
    for (uint32_t depth = 0; depth < 10; ++depth) {
        const std::string parent = depth ? "/G/Mods/Depth" + std::to_string(depth - 1) : "/G/Mods/Root";
        const std::string asset = "/G/Mods/Depth" + std::to_string(depth) + ".Depth" + std::to_string(depth);
        s.children[parent] = {child(asset.c_str())};
    }
    prepare(); enter(s.world);
    CHECK(s.added.size() <= 9 && s.flushes.size() <= 9 && !ready_after_play());
    CHECK(logged.lines.find("exceeded the bounded initial phase") != std::string::npos);
    for (uint64_t stream : s.added)
        CHECK(fake::get<uint16_t>(stream + t::streaming_level::policy) == s.new_policy);

    // Every native boundary may run arbitrary class callbacks. Reentry cannot
    // duplicate an admission, and stopping/forgetting cannot leave stale part
    // references alive after their ownership callbacks compact the part list.
    reset(s); prepare();
    bool reentered = false;
    s.on_construct = [&] { if (!reentered) { reentered = true; enter(s.world); } };
    enter(s.world);
    CHECK(reentered && s.constructed.size() == 1 && s.added.empty() && s.flushes.size() == 2);
    for (uint32_t boundary = 0; boundary < 3; ++boundary) {
        reset(s); TSUCHI_Mod* stopped = prepare();
        const auto stop = [stopped] { tsuchi::mod_stop(stopped); };
        if (boundary == 0) s.on_construct = stop;
        else if (boundary == 1) s.on_package = stop;
        else s.on_add = stop;
        enter(s.world);
        CHECK(!tsuchi::mod_usable(stopped) && s.flushes.size() == 1);
        CHECK(s.enrolled[s.world].empty());
        if (boundary < 2) CHECK(s.added.empty());
        else CHECK(s.added.size() == 1 && s.removed.size() == 1);
    }
    reset(s); prepare(); s.on_construct = [] { tsuchi::forget_mods(); }; enter(s.world);
    CHECK(tsuchi::mod_count() == 0 && s.added.empty() && s.flushes.size() == 1);
    reset(s);
    s.children["/G/Mods/Root"] = {child("/G/Mods/Stopped.Stopped")};
    TSUCHI_Mod* stopped_copy = prepare();
    s.on_duplicate = [stopped_copy] { tsuchi::mod_stop(stopped_copy); };
    enter(s.world);
    CHECK(!tsuchi::mod_usable(stopped_copy) && s.duplicated.size() == 1 && s.added.size() == 1 && s.flushes.size() == 1);
    CHECK(s.removed.empty());
    if (!s.constructed.empty()) CHECK(fake::get<uint16_t>(s.constructed.front() + t::streaming_level::policy) == s.new_policy);

    // Once native progression starts, failure stops further graph admission.
    // The provisional remover must not pretend to cancel that native work.
    for (bool forget : {false, true}) {
        reset(s);
        s.children["/G/Mods/Root"] = {child("/G/Mods/After.After")};
        TSUCHI_Mod* owner = prepare();
        s.on_flush = [owner, forget] {
            if (forget) tsuchi::forget_mods(); else tsuchi::mod_stop(owner);
        };
        enter(s.world);
        CHECK(s.added.size() == 1 && s.duplicated.empty() && s.flushes.size() == 1 && s.removed.empty());
        if (!s.constructed.empty()) CHECK(fake::get<uint16_t>(s.constructed.front() + t::streaming_level::policy) == s.new_policy);
    }

    // A world replaced during a native callback cannot receive this entry's
    // remaining work. Its original flush still sees the original arguments.
    for (bool after_add : {false, true}) {
        reset(s); prepare();
        const uint64_t incoming = s.world, replacement = make_world(u"/G/Other_P");
        const auto replace = [replacement] { fake::put(fake::main_image() + t::data::world, replacement); };
        if (after_add) s.on_add = replace; else s.on_construct = replace;
        enter(incoming);
        CHECK(s.flushes.size() == 1 && s.flushes[0].first == incoming && s.flushes[0].second == 2);
        CHECK(s.enrolled[incoming].empty() && s.enrolled[replacement].empty());
        CHECK(s.added.size() == size_t(after_add));
        if (after_add) CHECK(s.removed.size() == 1);
    }

    // Mode lookup crosses a native callback before recipes are snapshotted.
    // Reentry or ownership release there cannot retain a Part/manifest pointer.
    reset(s); prepare();
    uint32_t mode_calls = 0;
    s.on_mode = [&] { if (++mode_calls == 1) enter(s.world); };
    enter(s.world); s.on_mode = {};
    CHECK(mode_calls == 1 && s.constructed.empty() && s.added.empty() && s.flushes.size() == 2 && !ready_after_play());
    reset(s); TSUCHI_Mod* mode_owner = prepare();
    mode_calls = 0;
    s.on_mode = [&] { ++mode_calls; tsuchi::mod_stop(mode_owner); };
    enter(s.world); s.on_mode = {};
    CHECK(mode_calls == 1 && !tsuchi::mod_usable(mode_owner) && s.constructed.empty() && s.added.empty() && s.flushes.size() == 1);

    // Refuse the oversized recipe before constructing any of its 257 roots.
    reset(s);
    std::string excessive = "[";
    for (uint32_t i = 0; i < 257; ++i) {
        if (i) excessive += ',';
        excessive += "\"/G/Mods/Root" + std::to_string(i) + "\"";
    }
    excessive += ']';
    prepare("map.initial", excessive.c_str()); logged.lines.clear(); enter(s.world);
    CHECK(s.constructed.empty() && s.added.empty() && s.flushes.size() == 1 && s.flushes[0].second == 2);
    CHECK(!ready_after_play() && tsuchi::map_part_failed(s.part));
    CHECK(logged.lines.find("initial streaming refused") != std::string::npos);
    refusal_stays_unready();

    // Refuse selected framework allocation boundaries after declarations have
    // prepared. Restore normal allocation before status queries and cleanup.
    for (int budget = 0; budget <= 8; ++budget) {
        reset(s); prepare();
        watched_fail_after = budget;
        tsuchi::set_allocator({watched_allocate, watched_release});
        enter(s.world);
        watched_fail_after = -1;
        tsuchi::set_allocator({std::malloc, std::free});
        CHECK(s.flushes.size() == 1 && s.flushes[0].first == s.world && s.flushes[0].second == 2);
        CHECK(s.added.size() <= 1 && s.duplicated.empty());
        const uint64_t root = s.added.empty() ? 0 : s.added.front();
        const uint64_t level = root ? fake::get<uint64_t>(root + t::streaming_level::loaded) : 0;
        const bool completed = root && level && (fake::get<uint8_t>(level + kLevelVisible) & 0x20);
        if (!completed) {
            CHECK(!ready_after_play());
            refusal_stays_unready();
        }
        if (root) CHECK(fake::get<uint16_t>(root + t::streaming_level::policy) == s.new_policy);
    }

    // Wrong mode preserves the exact native call and does not admit recipes.
    reset(s); prepare(); enter(s.world, 1);
    CHECK(s.constructed.empty() && s.added.empty() && s.flushes.size() == 1 && s.flushes[0].second == 1);
    CHECK(legacy_visible_calls == 0);
    tsuchi::forget_mods();

    // Runtime code is a separate public path. Give the existing dynamic helper
    // its reflected stand-in, and execute an actual main.lua through startup.
    tsuchi::content_service_start();
    tsuchi_lua::register_script_mods();
    fake::put(s.dynamic_type + t::class_type::default_object, fake::engine_object(u"RuntimeDefault", s.dynamic_type, kBytes));
    const uint64_t dynamic_load = fake::function(s.dynamic_type, s.functions, u"LoadLevelInstance", 0x28, 8,
        [&](uint64_t, uint64_t args) {
            CHECK(fake::get<uint64_t>(args) == s.world);
            const uint64_t characters = fake::get<uint64_t>(args + 8 + t::array::data);
            const int32_t count = fake::get<int32_t>(args + 8 + t::array::count);
            const bool valid = count > 0 && count <= 256 && characters && readable(characters, uint64_t(count) * 2);
            CHECK(valid);
            if (!valid) { fake::put<uint8_t>(args + 0x18, 0); fake::put<uint64_t>(args + 0x20, 0); return; }
            std::u16string path;
            for (int32_t i = 0; i + 1 < count; ++i) path += char16_t(fake::get<uint16_t>(characters + uint64_t(i) * 2));
            const uint64_t stream = descriptor(s.dynamic_type, s.world, u"RuntimeStream");
            set_package(reinterpret_cast<void*>(stream), fake::engine_name(path.data(), path.size(), 1));
            const uint64_t saved = fake::engine_object(u"RuntimeSaved", s.world_type, kBytes);
            const uint64_t level = fake::engine_object(u"PersistentLevel", s.level_type, kBytes);
            fake::put(level + t::object::outer, saved), fake::put(level + t::engine_level::owning_world, s.world);
            array(saved + kStreams, {});
            fake::put<uint8_t>(level + kLevelVisible, 0x20), fake::put(stream + t::streaming_level::loaded, level);
            for (uint32_t i = 0; i < 4; ++i) fake::put<uint8_t>(stream + kStarts + i, 1);
            s.visible[stream] = true, s.late_streams.push_back(stream);
            fake::put<uint8_t>(args + 0x18, 1), fake::put(args + 0x20, stream);
        }, t::constant::function_static);
    fake::flag(fake::field(dynamic_load, u"WorldContextObject", u"ObjectProperty", 0, 8, s.world_type), in);
    fake::flag(fake::field(dynamic_load, u"LevelName", u"StrProperty", 8, 0x10), in);
    fake::flag(fake::bit_field(dynamic_load, u"bOutSuccess", 0x18, 0, 0xFF), in | t::constant::property_out);
    fake::flag(fake::field(dynamic_load, u"ReturnValue", u"ObjectProperty", 0x20, 8, s.stream_type), result);
    const char* runtime_definition = R"({"maps":[{"map":"/G/Initial_P","levels":["/G/Mods/Runtime"]}]})";
    const auto no_map_mod = [&](const char* id, bool script) {
        reset(s); disk.files.clear(), logged.lines.clear();
        const std::string folder = std::string(tsuchi::kModsFolder) + "/" + id + "/";
        disk.files[folder + "mod.json"] = std::string(R"({"id":")") + id + R"(","name":"Runtime maps","version":"1.0.0"})";
        if (script) disk.files[folder + "main.lua"] = std::string("local declaration,result=content.add([==[") + runtime_definition +
            "]==]); assert(declaration,result); print('runtime-queued')\n";
        CHECK(tsuchi::find_mods() == TSUCHI_OK);
        tsuchi::prepare_mods();
        auto* record = tsuchi::find_mod(id);
        CHECK(record && record->state == tsuchi::ModState::ready && record->prepared && record->context);
        CHECK(tsuchi::map_parts() == 0);
        return record ? record->context : nullptr;
    };
    const auto runtime_arrives = [&] {
        enter(s.world);
        CHECK(s.constructed.empty() && s.added.empty() && s.late_streams.empty());
        CHECK(s.flushes.size() == 1 && s.flushes[0].second == 2 && !ready_after_play());
        fake::put<uint8_t>(w.framework + t::game_framework::world_begun, 1);
        for (uint32_t i = 0; i < 331; ++i) tsuchi::frame(0.03f);
        CHECK(s.late_streams.size() == 1 && s.constructed.empty() && s.added.empty());
        CHECK(tsuchi::map_part_ready(s.part) && !tsuchi::map_part_failed(s.part));
        fake::put<uint8_t>(w.framework + t::game_framework::world_begun, 0);
    };
    TSUCHI_Mod* script_owner = no_map_mod("map.runtime.script", true);
    tsuchi::start_mods();
    const auto* script_record = tsuchi::find_mod("map.runtime.script");
    CHECK(script_record && script_record->state == tsuchi::ModState::running && script_record->prepared && script_record->context == script_owner);
    CHECK(logged.lines.find("[map.runtime.script] runtime-queued") != std::string::npos && tsuchi::map_parts() == 0);
    tsuchi::frame(0.03f);
    s.part = next_fixture_part++;
    CHECK(tsuchi::map_parts() == 1 && !tsuchi::map_part_failed(s.part));
    runtime_arrives();
    tsuchi::mod_stop(script_owner);
    CHECK(tsuchi::map_parts() == 0);

    // A prepared context remains Ready until ordinary script startup. A public
    // declaration applied in that interval must still be a runtime declaration.
    TSUCHI_Mod* ready_owner = no_map_mod("map.runtime.ready", false);
    TSUCHI_ContentDeclaration declaration{};
    CHECK(tsuchi::host.content->add(ready_owner, runtime_definition, &declaration) == TSUCHI_OK);
    TSUCHI_DeclarationState state = TSUCHI_DECLARATION_STATE_FAILED;
    CHECK(tsuchi::host.content->state(ready_owner, declaration, &state) == TSUCHI_OK && state == TSUCHI_DECLARATION_STATE_QUEUED);
    tsuchi::frame(0.03f);
    s.part = next_fixture_part++;
    const auto* ready_record = tsuchi::find_mod("map.runtime.ready");
    CHECK(ready_record && ready_record->state == tsuchi::ModState::ready && ready_record->prepared && ready_record->context == ready_owner);
    CHECK(tsuchi::map_parts() == 1);
    CHECK(tsuchi::host.content->state(ready_owner, declaration, &state) == TSUCHI_OK && state == TSUCHI_DECLARATION_STATE_READY);
    tsuchi::start_mods();
    ready_record = tsuchi::find_mod("map.runtime.ready");
    CHECK(ready_record && ready_record->state == tsuchi::ModState::running && ready_record->context == ready_owner);
    runtime_arrives();
    CHECK(tsuchi::host.content->retract(ready_owner, declaration) == TSUCHI_OK && tsuchi::map_parts() == 0);
    CHECK(tsuchi::host.content->state(ready_owner, declaration, &state) == TSUCHI_NOT_FOUND);

    // A prepared levels entry whose world arrives where the initial flush hook did not see it (another load
    // path, or an entry prepared after that world's LoadMap) still loads its levels: through the late streaming
    // path, as on main, and the log says why.
    reset(s);
    TSUCHI_Mod* missed_owner = prepare("map.initial.missed");
    tsuchi::start_mods();
    CHECK(s.flushes.empty() && !tsuchi::map_part_failed(s.part));
    fake::put<uint8_t>(w.framework + t::game_framework::world_begun, 1);
    for (uint32_t i = 0; i < 331; ++i) tsuchi::frame(0.03f);
    CHECK(s.late_streams.size() == 1 && s.constructed.empty() && s.added.empty());
    CHECK(tsuchi::map_part_ready(s.part) && !tsuchi::map_part_failed(s.part));
    CHECK(logged.lines.find("missed its incoming initial streaming boundary, so its levels load as the world runs") != std::string::npos);
    fake::put<uint8_t>(w.framework + t::game_framework::world_begun, 0);
    tsuchi::mod_stop(missed_owner);
    tsuchi::forget_mods();
    scene = nullptr;
    std::puts("initial streaming: native ordering, nested progress, serialized policy, callback invalidation, rollback and block restoration checked");
}
