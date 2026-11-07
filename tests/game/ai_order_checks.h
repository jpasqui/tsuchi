// Stand-in for the native reads and construction performed by BabyAI start.
// This exercises the SDK observer around the same framework serializer route.
namespace ai_order_model {
uint64_t target = 0, part = 0, type = 0, level = 0, world = 0;
std::vector<uint64_t> objects;
uint32_t native_calls = 0;
float native_values[5]{};
uint8_t native_patrol = 0;
uint64_t native_entry = 0;
auto saved_write = t::fn::ai_values_write;
auto saved_read = t::fn::ai_values_read;
Minds saved_minds;

void load_values(void* pointer, void* values, uint64_t from, uint64_t to) {
    game_ai_values_read(pointer, values, from, to);
    const uint64_t at = reinterpret_cast<uint64_t>(pointer);
    const auto& bytes = minds.set[at];
    CHECK(bytes.size() == 166);
    for (unsigned i = 0; i < 5; ++i) {
        float value = 0;
        std::memcpy(&value, bytes.data() + sizeof(float) * i, sizeof value);
        fake::put(at + t::ai_start_observation::territory + sizeof(float) * i, value);
    }
    fake::put<uint8_t>(at + t::ai_start_observation::patrol, bytes[158]);
}
void begin(void* pointer) {
    const uint64_t at = reinterpret_cast<uint64_t>(pointer);
    ++native_calls;
    for (unsigned i = 0; i < 5; ++i)
        native_values[i] = fake::get<float>(at + t::ai_start_observation::territory + sizeof(float) * i);
    native_patrol = fake::get<uint8_t>(at + t::ai_start_observation::patrol);
    // The native function allocates its new tree after consuming the parameter block.
    const uint64_t tree = fake::memory(0x40);
    const uint64_t context = fake::memory(0x30);
    fake::put(tree + t::ai_start_observation::tree_context, context);
    fake::put(context + t::ai_start_observation::patrol_reference, at + t::ai_start_observation::patrol);
    fake::put(at + t::ai_start_observation::actions, tree);
}
void install() {
    saved_write = t::fn::ai_values_write, saved_read = t::fn::ai_values_read;
    saved_minds = minds;
    t::fn::ai_values_write = game_ai_values_write, t::fn::ai_values_read = load_values;
    install_ai_native_starts();
    tsuchi::map_ai_start(tsuchi::mod_create("tsuchi.self.ai-sdk"));
    // The SDK observer must surround the actual callee that consumes the values, rather than
    // surround the framework's earlier vtable wrapper. Early manifest preparation installs that
    // wrapper before mod code now; this slot models the native function-entry seam on the host.
    native_entry = fake::memory(sizeof(uint64_t));
    fake::put(native_entry, &begin);
    fake::code().push_back(reinterpret_cast<uint64_t>(&begin));
    ai_native_observe_entry = native_entry;
    const uint64_t world_type = fake::object(u"ReleaseWorld", w.root);
    const uint64_t level_type = fake::object(u"ReleaseLevel", w.root);
    const uint64_t package = fake::object(u"/G/ReleaseAI_P", w.root, 0x40);
    world = fake::object(u"ReleaseAI_P", world_type, 0x80);
    level = fake::object(u"PersistentLevel", level_type, 0x80);
    fake::put(world + t::object::outer, package), fake::put(level + t::object::outer, world);
    fake::field(level_type, u"OwningWorld", u"ObjectProperty", 0x40, 8, world_type);
    fake::put(level + 0x40, world);
    const uint64_t class_package = fake::object(u"/G/ReleaseBaby", w.root, 0x40);
    type = fake::object(u"ReleaseBaby_C", w.root);
    fake::put(type + t::object::outer, class_package), fake::set_super(type, w.creature_class);
    objects = {package, world, level, class_package, type, world_type, level_type};
}
void observer_targets(std::string& text) {
    const size_t first = text.find('\n'), second = first == std::string::npos ? first : text.find('\n', first + 1);
    CHECK(second != std::string::npos && native_entry);
    if (second == std::string::npos || !native_entry) return;
    char targets[96];
    std::snprintf(targets, sizeof targets, "%llx\n%llx\n",
        static_cast<unsigned long long>(native_entry - fake::main_image()),
        static_cast<unsigned long long>(reinterpret_cast<uint64_t>(&begin) - fake::main_image()));
    text = targets + text.substr(second + 1); // all ten layout/placement offsets stay table-derived
}
void run_start() {
    target = actor(u"OrderingLarva", type, u"Baby", 3, t::constant::ai_order_placement, component(0, 0, 0, 0));
    fake::put(target + t::object::outer, level);
    part = fake::memory(0xB0);
    const auto& start = t::slot_group::ai_part_start;
    fake::put(part, fake::main_image() + start.tables[2]);
    fake::put(part + t::scene_component::owner, target);
    fake::put(part + t::ai_start_observation::game_owner, target);
    fake::put(target + t::game_actor::ai, part);
    minds.set[part] = ai_stream(tsuchi::ai::kLarva, 100);
    minds.set[part][158] = 1;
    for (unsigned i = 0; i < 5; ++i)
        fake::put<float>(part + t::ai_start_observation::territory + sizeof(float) * i, i == 3 ? 100.f : 0.f);
    fake::put<uint8_t>(part + t::ai_start_observation::patrol, 1);
    native_calls = 0;
    minds.log.clear();
    const auto old_flags = t::fn::story_flags;
    const uint8_t old_rule = fake::get<uint8_t>(running.context + t::game_context::rule);
    const uint64_t clock = fake::get<uint64_t>(w.framework + t::game_framework::clock);
    const uint8_t old_night = fake::get<uint8_t>(clock + t::clock::night);
    t::fn::story_flags = [](void*) -> uint64_t { return 0; };
    fake::put<uint8_t>(running.context + t::game_context::rule, uint8_t(t::constant::rule_story));
    fake::put<uint8_t>(clock + t::clock::night, 0);
    thunks::call(fake::get<uint64_t>(fake::get<uint64_t>(part) + start.offset), part);
    t::fn::story_flags = old_flags;
    fake::put<uint8_t>(running.context + t::game_context::rule, old_rule);
    fake::put<uint8_t>(clock + t::clock::night, old_night);
    CHECK(native_calls == 1 && native_patrol == 0);
    const float wanted[] = {11.25f, -22.5f, 33.75f, 87.5f, 731.25f};
    for (unsigned i = 0; i < 5; ++i) CHECK(native_values[i] == wanted[i]);
    CHECK(minds.log == "write read start:87.5/0 " && minds.calls_good);
}
void finish_run() {
    if (target) fake::destroy(target);
    minds.set.erase(part);
    target = part = 0;
}
void finish() {
    ai_native_observe_entry = 0;
    native_entry = 0;
    ai_native_observe = nullptr;
    t::fn::ai_values_write = saved_write, t::fn::ai_values_read = saved_read;
    minds = saved_minds;
    for (uint64_t object : objects) fake::destroy(object);
    objects.clear();
}
} // namespace ai_order_model
