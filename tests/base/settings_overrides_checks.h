// Runs with the real loader and settings implementation, on the manifest fixture's files.
#include <cstdlib>
namespace {
const char* target_settings = R"(,"settings":{
 "count":{"type":"integer","default":3,"min":1,"max":10},
 "enabled":{"type":"toggle","default":false},
 "rate":{"type":"number","default":0.5,"min":0,"max":2},
 "text":{"type":"text","default":"home","max_length":10},
 "mode":{"type":"choice","default":"one","choices":["one","two"]},
 "set":{"type":"set","default":["a"],"choices":["a","b"]},
 "internal":{"type":"integer","default":1,"hidden":true},
 "locked":{"type":"integer","default":1,"overridable":false}
 })";
void controller(const char* id, const char* values, const char* extra = "") {
    const std::string declarations = std::string(R"(,"requires":{"test.target":"1.0.0"},"settings_overrides":{"test.target":)") + values + "}" + extra;
    install(id, declarations.c_str());
}
Setting& target_setting(const char* key) { return *find_setting(find_mod("test.target")->settings, key); }
int64_t count_value() { int64_t value = 0; CHECK(setting_integer(target_setting("count"), value) == TSUCHI_OK); return value; }
void settings_overrides_checks() {
    reset(); set_before_start(nullptr);
    // All six types are effective before early content and before either mod's script starts.
    install("test.target", target_settings);
    controller("test.controller", R"({"count":7,"enabled":true,"rate":1.5,"text":"camp","mode":"two","set":["b","a"]})", R"(,"early2":{})");
    CHECK(find_mods() == TSUCHI_OK && count_value() == 7);
    CHECK(!target_setting("count").open); // overrides do not grant cross-mod read access
    bool yes = false;
    CHECK(setting_toggle(target_setting("enabled"), yes) == TSUCHI_OK && yes);
    float rate = 0;
    CHECK(setting_number(target_setting("rate"), rate) == TSUCHI_OK && rate == 1.5f);
    const char* text = nullptr;
    CHECK(setting_text(target_setting("text"), text) == TSUCHI_OK && !strcmp(text,"camp"));
    CHECK(setting_text(target_setting("mode"), text) == TSUCHI_OK && !strcmp(text,"two"));
    CHECK(setting_has(target_setting("set"), "b", yes) == TSUCHI_OK && yes);
    CHECK(!strcmp(target_setting("count").controllers.get(), "test.controller"));
    CHECK(target_setting("count").integer == 3 && !target_setting("count").touched);
    bool changed = true;
    CHECK(set_setting_integer(target_setting("count"), 9, changed) == TSUCHI_DENIED && !changed);
    CHECK(set_setting_toggle(target_setting("enabled"), false, changed) == TSUCHI_DENIED);
    CHECK(set_setting_number(target_setting("rate"), 1, changed) == TSUCHI_DENIED);
    CHECK(set_setting_text(target_setting("text"), "new", changed) == TSUCHI_DENIED);
    CHECK(set_setting_has(target_setting("set"), "b", false, changed) == TSUCHI_DENIED);
    CHECK(reset_setting(target_setting("count"), changed) == TSUCHI_DENIED);
    // A late save changes the preference underneath without emitting an effective change or persisting overrides.
    unsigned late = 0;
    const char* saved = R"({"count":5,"text":"saved","set":["a"]})";
    auto& settings = find_mod("test.target")->settings;
    CHECK(take_settings(settings, saved, strlen(saved), [](const Setting&, void* user) { ++*static_cast<unsigned*>(user); }, &late));
    CHECK(!late && count_value() == 7 && target_setting("count").integer == 5);
    Text serialized;
    CHECK(settings_text(settings, serialized));
    CHECK(strstr(serialized.get(), "\"count\": 5") && strstr(serialized.get(), "saved") && !strstr(serialized.get(), "camp"));
    serialized.clear();
    prepare_mods(); start_mods();
    CHECK(find_mod("test.controller")->state == ModState::running);
    unsigned releases = 0;
    static unsigned* released_settings = nullptr;
    released_settings = &releases;
    set_setting_override_changed([](ModRecord&, const Setting&) { ++*released_settings; });
    block_mod(*find_mod("test.controller"), "test controller failure");
    CHECK(count_value() == 5 && !*target_setting("count").controllers.get() && releases == 6);
    CHECK(set_setting_integer(target_setting("count"), 6, changed) == TSUCHI_OK && changed);
    // Restarting a failed controller restarts configured dependencies and restores overrides before their startup.
    const uint32_t rank = find_mod("test.target")->rank;
    find_mod("test.controller")->restart_asked = true;
    run_restarts();
    CHECK(find_mod("test.controller")->state == ModState::running && find_mod("test.target")->rank == rank);
    CHECK(count_value() == 7 && target_setting("count").integer == 6);
    set_setting_override_changed(nullptr);
    reset();
    // Equal sets may be written in different orders, and retain both controller IDs.
    install("test.target", target_settings);
    controller("test.a", R"({"count":7,"set":["a","b"]})");
    controller("test.b", R"({"count":7,"set":["b","a"]})");
    CHECK(find_mods() == TSUCHI_OK && count_value() == 7);
    CHECK(!strcmp(target_setting("count").controllers.get(), "test.a, test.b"));
    start_mods(); block_mod(*find_mod("test.a"), "removed");
    CHECK(count_value() == 7 && !strcmp(target_setting("count").controllers.get(), "test.b"));
    block_mod(*find_mod("test.b"), "removed"); CHECK(count_value() == 3);
    reset();
    // Conflicts block every claimant, including another claimant agreeing with one side. No ID-order winner.
    install("test.target", target_settings);
    controller("test.a", R"({"count":7,"text":"camp"})");
    controller("test.b", R"({"count":8})");
    controller("test.c", R"({"count":7})");
    install("test.dependent", R"(,"requires":{"test.a":"*"})");
    CHECK(find_mods() == TSUCHI_OK && count_value() == 3);
    for (const char* id : {"test.a", "test.b", "test.c", "test.dependent"}) CHECK(find_mod(id)->state == ModState::blocked);
    CHECK(strstr(problem_of("test.a"), "test.target:count"));
    CHECK(!target_setting("text").override_value);
    reset();
    for (const char* invalid : {R"({"count":11})", R"({"count":2.5})", R"({"enabled":1})", R"({"rate":3})",
                               R"({"text":"far too long"})", R"({"mode":"three"})", R"({"set":["c"]})",
                               R"({"set":["a","a"]})", R"({"count":7,"missing":true})", R"({"internal":2})", R"({"locked":2})"}) {
        install("test.target", target_settings); controller("test.bad", invalid, R"(,"early2":{})");
        CHECK(find_mods() == TSUCHI_OK && find_mod("test.bad")->state == ModState::blocked && count_value() == 3);
        prepare_mods(); CHECK(!find_mod("test.bad")->context);
        reset();
    }
    // Optional dependencies do not qualify, and disabled/incompatible targets block composition.
    install("test.target", target_settings);
    install("test.optional", R"(,"optional":{"test.target":"*"},"settings_overrides":{"test.target":{"count":7}})");
    CHECK(find_mods() == TSUCHI_OK && find_mod("test.optional")->state == ModState::blocked);
    reset();
    install("test.target", target_settings); controller("test.a", R"({"count":7})");
    activation = "p4a1:test.a:";
    CHECK(find_mods() == TSUCHI_OK && count_value() == 3);
    CHECK(find_mod("test.a")->state == ModState::disabled);
    reset();
    install("test.target", target_settings); controller("test.a", R"({"count":7})");
    activation = "p4a1:test.target:";
    CHECK(find_mods() == TSUCHI_OK && find_mod("test.a")->state == ModState::blocked && count_value() == 3);
    reset();
    // Runtime startup failure releases the value; the source's own content has no special exemption.
    install("test.target", target_settings);
    controller("test.failure", R"({"count":7})", R"(,"said":{})");
    CHECK(find_mods() == TSUCHI_OK && count_value() == 7);
    start_mods(); CHECK(find_mod("test.failure")->state == ModState::blocked && count_value() == 3);
    reset();
    // OOM while replacing provenance releases every affected override and the failed controller's resources.
    install("test.target", target_settings);
    controller("test.a", R"({"count":7})", R"(,"early2":{})");
    controller("test.b", R"({"count":7})", R"(,"early2":{})");
    CHECK(find_mods() == TSUCHI_OK); start_mods();
    TSUCHI_Mod* b_context = find_mod("test.b")->context;
    CHECK(owned_count(b_context) != 0);
    set_allocator({[](size_t)->void* { return nullptr; }, std::free});
    block_mod(*find_mod("test.a"), "test failure");
    CHECK(count_value() == 3 && !target_setting("count").override_value);
    CHECK(find_mod("test.b")->state == ModState::blocked && !mod_running(b_context) && !owned_count(b_context));
    set_allocator({std::malloc, std::free});
    reset();
}
}
