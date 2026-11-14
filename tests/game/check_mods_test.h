// The log's file, the SDK's helper for checks, and the check mods among the fixtures, each run against the made-up
// game. Part of objects_test.cpp, which includes this inside its own namespace, after everything it uses.
// `check_mods_data.h`, which is made as the checks are built, has the check mods' files, the SDK's helper for
// scripts, and the starts of the compiled check mods, each built for this machine.

const std::string kLogAt = "save:/tsuchi/tsuchi.log";

// Where every line goes in the game, less the debug output: the log's file.
void log_to_file(const char* mod_id, const char* text) {
    logged.lines += std::string("[") + mod_id + "] " + text + "\n";
    tsuchi::log_file_line(mod_id, text);
}

bool checking_engine_ticks = false, check_cleanup_spawn = false;
TSUCHI_Mod* check_cleanup_context = nullptr;
uint32_t check_cleanup_owned_before_refusal = 0;
TSUCHI_Result check_cleanup_start(const TSUCHI_Host* host, TSUCHI_Mod* mod) {
    check_cleanup_context = mod;
    for (const auto& code : check_mod_starts) if (code.mod && !std::strcmp(code.mod, "check.cleanup-producer")) {
        const auto result = code.start(host, mod);
        check_cleanup_owned_before_refusal = tsuchi::owned_count(mod);
        return result;
    }
    return TSUCHI_NOT_FOUND;
}
// This SDK mod wraps only the float-bearing engine update. Select the host's
// typed adapter while its real start publishes those continuations, then restore
// the pointer/integer adapter used by the other native SDK fixtures.
TSUCHI_Result check_typed_hooks_start(const TSUCHI_Host* host, TSUCHI_Mod* mod) {
    const bool prior = thunks::engine_ticks;
    thunks::engine_ticks = true;
    TSUCHI_Result result = TSUCHI_NOT_FOUND;
    for (const auto& code : check_mod_starts) if (code.mod && !std::strcmp(code.mod, "check.hooks")) {
        result = code.start(host, mod);
        break;
    }
    thunks::engine_ticks = prior;
    return result;
}
// Only the new native-callee slot and its continuation use this exact one-pointer ABI.
TSUCHI_Result check_ai_entry_start(const TSUCHI_Host* host, TSUCHI_Mod* mod) {
    const bool prior = thunks::ai_entry;
    thunks::ai_entry = true;
    TSUCHI_Result result = TSUCHI_NOT_FOUND;
    for (const auto& code : check_mod_starts) if (code.mod && !std::strcmp(code.mod, "check.ai-order")) {
        result = code.start(host, mod);
        break;
    }
    thunks::ai_entry = prior;
    return result;
}
struct CheckBody { uint64_t part; float ground; };
std::map<uint64_t, CheckBody> check_bodies;
std::set<uint64_t> check_throws;
uint64_t check_revive_class = 0;
std::set<uint64_t> check_revive_requests;
std::map<uint64_t, int> check_defeats, check_carcasses, check_revivals;
void check_make_revive(void* request, void* sender) {
    game_revive_request(request, sender);
    check_revive_requests.insert(reinterpret_cast<uint64_t>(request));
}
void check_world_creatures() {
    for (auto at = check_defeats.begin(); at != check_defeats.end();) {
        if (--at->second) { ++at; continue; }
        fake::put<uint8_t>(at->first + t::game_actor::removed, 1);
        check_carcasses[at->first] = 3;
        at = check_defeats.erase(at);
    }
    for (auto at = check_carcasses.begin(); at != check_carcasses.end();) {
        if (--at->second) { ++at; continue; }
        fake::put<uint32_t>(at->first + t::game_actor::state_flags, t::constant::state_carcass);
        at = check_carcasses.erase(at);
    }
    for (auto at = check_revivals.begin(); at != check_revivals.end();) {
        if (--at->second) { ++at; continue; }
        const uint64_t old_root = fake::get<uint64_t>(at->first + t::game_actor::root);
        const uint64_t made = actor(u"NativeRevival", check_revive_class, u"KOCHAPPY", 0, ~uint64_t(0), component(
            fake::get<float>(old_root + t::scene_component::location), fake::get<float>(old_root + t::scene_component::location + 4),
            fake::get<float>(old_root + t::scene_component::location + 8), 0));
        fake::put(made, table_a());
        fake::put(made + t::game_actor::group, fake::get<tsuchi::game::Name>(fake::main_image() + t::data::creature_group));
        give_life(made, 50, 50);
        registers(made); // native registration, independently of the SDK's spawn entry
        at = check_revivals.erase(at);
    }
}
bool check_dismiss_delayed = false;
uint64_t check_dismiss_target = 0;
uint32_t check_dismiss_requests = 0;
int check_dismiss_steps = 0;
void check_world_dismissal() {
    if (check_dismiss_steps && --check_dismiss_steps == 0) {
        for (auto& member : leading.leaders) if (member.second == check_dismiss_target) member.second = 0;
        check_dismiss_target = 0;
    }
}
uint64_t check_ride_captain = 0, check_ride_dog = 0;
int check_ride_steps = 0, check_off_steps = 0;
void check_request_ride(void* part) {
    CHECK(reinterpret_cast<uint64_t>(part) == fake::get<uint64_t>(check_ride_captain + t::game_actor::ai));
    game_request_ride(part);
    check_ride_steps = 2;
}
void check_request_get_off(void* seat) {
    CHECK(reinterpret_cast<uint64_t>(seat) == fake::get<uint64_t>(check_ride_dog + t::game_actor::player));
    game_request_get_off(seat);
    check_off_steps = 2;
}
void check_world_riding() {
    if (!check_ride_captain) return;
    const uint64_t grip = fake::get<uint64_t>(check_ride_captain + t::game_actor::stick);
    if (check_ride_steps && --check_ride_steps == 0) {
        fake::put(grip + t::stick_component::target, check_ride_dog);
        // The stock stand-in carries the captain's squad on the dog; dismount
        // leaves it there, so the check must restore each recorded leader.
        for (auto& [actor, leader] : leading.leaders) if (leader == check_ride_captain) leader = check_ride_dog;
    }
    if (check_off_steps && --check_off_steps == 0) fake::put<uint64_t>(grip + t::stick_component::target, 0);
}
void check_start_throw(void* captain, void* pikmin, void* nothing) {
    game_start_throw(captain, pikmin, nothing);
    CHECK(nothing == nullptr);
    check_throws.insert(reinterpret_cast<uint64_t>(pikmin));
}
// The fake game advances free bodies independently of the framework's holds and reports landing.
void check_world_physics() {
    for (const auto& [actor, state] : check_bodies) {
        if (!tsuchi::game::live(actor) || !part_states[state.part].simulating) continue;
        fake::put<uint8_t>(actor + t::game_actor::body_awake, 1);
        const uint64_t root = fake::get<uint64_t>(actor + t::game_actor::root), z = root + t::scene_component::location + 8;
        float speed = fake::get<float>(actor + t::game_actor::velocity + 8), height = fake::get<float>(z);
        if (height > state.ground || speed > 0) {
            speed -= 980 * 0.03f;
            height += speed * 0.03f;
            if (height <= state.ground) height = state.ground, speed = 0;
        }
        fake::put<float>(z, height), fake::put<float>(actor + t::game_actor::velocity + 8, speed);
        fake::put<uint32_t>(actor + t::game_actor::air_steps, height > state.ground ? fake::get<uint32_t>(actor + t::game_actor::air_steps) + 1 : 0);
    }
}
uint32_t check_engine_stock_calls = 0, check_dialog_answers = 0;
uint64_t check_journey_request = 0;
int check_journey_age = 0, check_title_age = 0;
void check_return_to_title(bool title) {
    game_return_to_title(title);
    if (title) check_title_age = 2;
}
void check_push_journey(void* queue, void* request) {
    game_push_request(queue, request);
    CHECK(check_journey_request == 0);
    check_journey_request = reinterpret_cast<uint64_t>(request);
    check_journey_age = 0;
}
void check_story_frame() {
    if (check_title_age && --check_title_age == 0) {
        in_stages({u"Title"});
        in_world(u"Title_P");
    }
    if (!check_journey_request) return;
    ++check_journey_age;
    const uint32_t id = fake::get<uint32_t>(check_journey_request + t::request::id);
    if (check_journey_age == 1) {
        in_stages({u"Story", u"StoryInGame", u"FastTravel"});
        return;
    }
    if (check_journey_age != 3) return;
    std::u16string destination;
    if (id == t::constant::leave_request_id) destination = u"Area002_P";
    else if (id == t::constant::portal_request_id) destination = u"Cave004_F01";
    else {
        char area[64] = "";
        CHECK(tsuchi::game::name_text(fake::get<tsuchi::game::Name>(check_journey_request + t::travel_request::area), area, sizeof area));
        for (const char* at = area; *at; ++at) destination.push_back(char16_t(*at));
        CHECK(fake::get<int32_t>(check_journey_request + t::travel_request::base) == -1);
    }
    in_stages({u"Story", u"StoryInGame"});
    const uint64_t top = fake::get<uint64_t>(running.flow + t::flow_stage::child), play = fake::get<uint64_t>(top + t::flow_stage::child);
    fake::put(top, fake::main_image() + t::data::story_flow_table);
    fake::put(play, fake::main_image() + t::data::story_play_table);
    fake::put<int32_t>(play + t::flow_stage::status, 1);
    in_world(destination.c_str());
    check_journey_request = 0;
}

// The system's controllers and keyboard, for the input check: a Pro Controller with nothing held, and a keyboard
// with A down.
uint32_t check_npad_style(const void*) { return uint32_t(t::constant::npad_full_key); }
void check_npad_state(void* state, const void*) {
    std::memset(state, 0, t::npad_state::size);
    fake::put<uint32_t>(reinterpret_cast<uint64_t>(state) + t::input_state::connected, 1);
}
uint32_t check_keyboard_starts = 0, check_keyboard_reads = 0;
void check_keyboard_start() { ++check_keyboard_starts; }
void check_keyboard_state(void* state) {
    ++check_keyboard_reads;
    std::memset(state, 0, t::keyboard_state::size);
    fake::put<uint32_t>(reinterpret_cast<uint64_t>(state) + t::keyboard_state::keys, 1u << 4);
}

void check_engine_tick(void* engine, float seconds, bool idle) {
    CHECK(engine == reinterpret_cast<void*>(0x123456) && seconds == 0.03125f && !idle);
    ++check_engine_stock_calls;
}
void run_frames(int count) {
    for (int i = 0; i < count; ++i) {
        if (checking_engine_ticks) {
            const auto& slot = t::slot::engine_tick;
            const uint64_t entry = fake::get<uint64_t>(fake::main_image() + slot.table + slot.offset);
            reinterpret_cast<thunks::EngineTick>(entry)(reinterpret_cast<void*>(0x123456), 0.03125f, false);
            reinterpret_cast<thunks::EngineTick>(tsuchi_hook_target(tsuchi::kCodeSiteThunk | t::code_site::engine_tick_function))(
                reinterpret_cast<void*>(0x123456), 0.03125f, false);
        }
        if (checking_engine_ticks) {
            world_update();
            check_story_frame();
            check_world_creatures();
            check_world_dismissal();
            check_world_riding();
            for (const uint64_t actor : check_throws) {
                CHECK(tsuchi::game::live(actor) && check_bodies.count(actor));
                leading.leaders[actor] = 0;
                fake::put<float>(actor + t::game_actor::velocity + 8, 600);
            }
            check_throws.clear();
            check_world_physics();
        }
        if (checking_engine_ticks && !talk.posted.empty()) {
            const uint64_t request = talk.posted.front();
            talk.posted.erase(talk.posted.begin());
            const uint64_t shown = game_shows(request);
            game_answers(shown, 0x55, t::constant::dialog_accept + (check_dialog_answers % 3 == 1 ? 1 : 0));
            game_lets_go(shown);
            ++check_dialog_answers;
        }
        tsuchi::frame(0.03f);
    }
}

// ---- The log's file -----------------------------------------------------------------------------------

void logging() {
    const uint64_t main = fake::main_image();
    const auto changed = [](const std::string& expected) {
        const bool same = disk.changes == expected;
        if (!same) std::printf("  changed: [%s]\n", disk.changes.c_str());
        disk.changes.clear();
        return same;
    };
    const auto counts = [] { return tsuchi::log_file_counts(); };
    const auto resized = [&](size_t bytes) { return "resize:" + kLogAt + "=" + std::to_string(bytes) + " "; };
    const auto written = [&](size_t bytes) { return "write:" + kLogAt + "+" + std::to_string(bytes) + " keep: "; };
    const auto other_files = disk.files;
    const std::string stale = "what the run before left, which is longer than the first lines of this run\n";
    disk.files = {{"save:/B/single.sav", "a slot's"}, {kLogAt, stale}};
    disk.folders.clear();
    disk.log.clear(), disk.changes.clear();

    // With nothing to write, the save is left alone.
    tsuchi::log_file_start();
    run_frames(3);
    CHECK(changed("") && disk.log.empty() && counts().lines == 0);

    // A line waits in memory. On the next frame it is written: the first write of a run takes the place of what the
    // run before left, with the save opened around it, told to keep the change, and closed. A line break in a line
    // is a space in the file, so one call is one line.
    tsuchi::log_file_line("some.mod", "hello");
    tsuchi::log_file_line("tsuchi", "two\nlines\r\nin one, [some.mod] and no more");
    CHECK(disk.files[kLogAt] == stale && changed("") && counts().lines == 2);
    std::string expected = "[some.mod] hello\n[tsuchi] two lines  in one, [some.mod] and no more\n";
    tsuchi::frame(0.03f);
    CHECK(disk.files[kLogAt] == expected && changed(resized(expected.size()) + written(expected.size())) && closed() && disk.log == "begin end ");
    CHECK(counts().writes == 1 && counts().written_bytes == expected.size() && counts().waiting_bytes == 0);
    disk.log.clear();

    // The file is written at most once in two seconds: a line that comes meanwhile waits, and is added to the end.
    tsuchi::log_file_line("some.mod", "later");
    run_frames(59);
    CHECK(disk.files[kLogAt] == expected && changed("") && disk.log.empty() && counts().waiting_bytes == 17);
    run_frames(2);
    expected += "[some.mod] later\n";
    CHECK(disk.files[kLogAt] == expected && changed(resized(expected.size()) + written(17)) && closed() && counts().writes == 2);
    disk.log.clear();

    // While the game is busy with a save of its own the lines go on waiting, and nothing is asked of the system.
    run_frames(61);
    fake::put<uint8_t>(main + t::data::save_async_state + 1, 1);
    tsuchi::log_file_line("some.mod", "while busy");
    run_frames(200);
    CHECK(disk.files[kLogAt] == expected && changed("") && disk.log.empty() && counts().refused == 0);
    fake::put<uint8_t>(main + t::data::save_async_state + 1, 0);
    tsuchi::frame(0.03f);
    expected += "[some.mod] while busy\n";
    CHECK(disk.files[kLogAt] == expected && closed() && counts().writes == 3);
    disk.changes.clear(), disk.log.clear();

    // The save refuses a step: nothing is told to be kept, room that was made and not written to is taken back,
    // and the lines wait ten seconds before they are tried again. None is lost.
    run_frames(61);
    tsuchi::log_file_line("some.mod", "refused once");
    for (const char* step : {"resize", "write", "keep"}) {
        const size_t before = expected.size();
        disk.refuse = step;
        tsuchi::frame(0.03f);
        disk.refuse.clear();
        CHECK(closed() && disk.changes.find("keep:") == std::string::npos && counts().waiting_bytes == 24);
        if (std::string(step) == "write") CHECK(changed(resized(before + 24) + resized(before)) && disk.files[kLogAt] == expected);
        disk.changes.clear();
        run_frames(300);
        CHECK(changed("")); // not before the ten seconds are over
        if (std::string(step) != "keep") continue;
        // What the save was not told to keep is gone on the console. Here the bytes are in the file already, and
        // the next write adds them once more: a line twice is better than a line lost.
        expected += "[some.mod] refused once\n";
        CHECK(disk.files[kLogAt] == expected);
    }
    CHECK(counts().refused == 3);
    tsuchi::frame(0.03f);
    expected += "[some.mod] refused once\n";
    CHECK(disk.files[kLogAt] == expected && closed() && counts().writes == 4);
    disk.changes.clear(), disk.log.clear();

    // A file that was taken away meanwhile is made again, with what is new. So are the folders it is in.
    run_frames(61);
    disk.files.erase(kLogAt);
    tsuchi::log_file_line("some.mod", "anew");
    tsuchi::frame(0.03f);
    CHECK(disk.files[kLogAt] == "[some.mod] anew\n" && changed("folder:save:/tsuchi create:" + kLogAt + "=16 " + written(16)) && closed());
    // One that cannot be made is not left half made.
    run_frames(61);
    disk.files.erase(kLogAt);
    tsuchi::log_file_line("some.mod", "anew");
    disk.refuse = "write";
    tsuchi::frame(0.03f);
    disk.refuse.clear();
    CHECK(!disk.files.count(kLogAt) && closed());
    run_frames(301);
    CHECK(disk.files[kLogAt] == "[some.mod] anew\n" && closed());
    disk.changes.clear(), disk.log.clear();

    // A line longer than 1,000 bytes is cut there, and not within a character.
    run_frames(61);
    tsuchi::log_file_line("some.mod", (std::string(999, 'a') + "\xC3\xA9 and more").c_str());
    tsuchi::log_file_line("some.mod", (std::string(998, 'b') + "\xC3\xA9 and more").c_str());
    tsuchi::frame(0.03f);
    CHECK(disk.files[kLogAt] == "[some.mod] anew\n[some.mod] " + std::string(999, 'a') + "\n[some.mod] " + std::string(998, 'b') + "\xC3\xA9\n" && closed());

    // With no memory for a line, the line is dropped and counted, and nothing more. The room there is holds four
    // of these, and the next two would each need more.
    run_frames(61);
    const uint32_t dropped = counts().dropped;
    allocations_left = 0;
    for (int i = 0; i < 6; ++i) tsuchi::log_file_line("some.mod", std::string(990, 'c').c_str());
    allocations_left = -1;
    tsuchi::log_file_line("some.mod", "after");
    tsuchi::frame(0.03f);
    const std::string kept_lines = disk.files[kLogAt].substr(disk.files[kLogAt].size() - 4 * 1002 - 17);
    CHECK(counts().dropped == dropped + 2 && kept_lines.find("[some.mod] " + std::string(990, 'c') + "\n") == 0 && kept_lines.rfind("[some.mod] after\n") == 4 * 1002 && closed());
    // And with none to say where the file is, the lines wait.
    run_frames(61);
    tsuchi::log_file_line("some.mod", "patient");
    allocations_left = 0;
    tsuchi::frame(0.03f);
    allocations_left = -1;
    CHECK(counts().waiting_bytes == 19 && closed());
    tsuchi::frame(0.03f);
    CHECK(counts().waiting_bytes == 0 && closed() && disk.well_formed);

    run_frames(61);
    disk.files = other_files;
    disk.folders.clear();
    disk.log.clear(), disk.changes.clear();
}

// ---- What the framework says of the mods it found ----------------------------------------------------------

void override_service_checks() {
    tsuchi::forget_mods();
    const auto saved_files = disk.files;
    disk.files.clear();
    const std::string root = std::string(tsuchi::kModsFolder) + "/";
    disk.files[root + "override.target/mod.json"] = R"({"id":"override.target","name":"Target","version":"1.0.0","settings":{
        "count":{"type":"integer","default":3,"min":1,"max":10,"public":true},
        "private":{"type":"toggle","default":false}}})";
    disk.files[root + "override.rules/mod.json"] = R"({"id":"override.rules","name":"Rules","version":"1.0.0",
        "requires":{"override.target":"1.0.0"},"settings_overrides":{"override.target":{"count":7}}})";
    disk.files[root + "override.target/main.lua"] = R"(assert(settings.integer('count') == 7)
        assert(settings.controllers('count') == 'override.rules'))";
    disk.files[root + "override.rules/main.js"] = R"(if (settings.integer('override.target:count') !== 7n ||
        settings.controllers('override.target:count') !== 'override.rules') throw new Error('override binding');)";
    CHECK(tsuchi::find_mods() == TSUCHI_OK);
    tsuchi::start_mods();
    auto* target = tsuchi::find_mod("override.target");
    auto* rules = tsuchi::find_mod("override.rules");
    if (target && target->state != tsuchi::ModState::running) std::printf("override target: %s\n",target->problem.get());
    if (rules && rules->state != tsuchi::ModState::running) std::printf("override rules: %s\n",rules->problem.get());
    CHECK(target && rules && target->state == tsuchi::ModState::running && rules->state == tsuchi::ModState::running);
    if (target && rules && target->context && rules->context) {
        char controllers[64] = "unchanged"; uint64_t needed = 0;
        CHECK(tsuchi::host.settings->controllers(target->context,"count",controllers,1,&needed) == TSUCHI_CAPACITY && needed == 15);
        CHECK(!strcmp(controllers,"unchanged"));
        CHECK(tsuchi::host.settings->controllers(target->context,"count",controllers,sizeof controllers,&needed) == TSUCHI_OK &&
              !strcmp(controllers,"override.rules"));
        CHECK(tsuchi::host.settings->controllers(rules->context,"override.target:private",controllers,sizeof controllers,&needed) == TSUCHI_DENIED);
        CHECK(tsuchi::host.settings->set_integer(target->context,"count",9) == TSUCHI_DENIED);
        CHECK(tsuchi::host.settings->set_integer(rules->context,"override.target:count",9) == TSUCHI_DENIED);
        CHECK(tsuchi::host.settings->reset(target->context,"count") == TSUCHI_DENIED);
        CHECK(tsuchi::host.settings->controllers(target->context,"missing",controllers,sizeof controllers,&needed) == TSUCHI_NOT_FOUND);
        unsigned changes = 0; TSUCHI_Subscription subscription{};
        CHECK(tsuchi::host.settings->on_changed(target->context, [](void* user, const TSUCHI_SettingsChanged* event) {
            if (!strcmp(event->mod,"override.target") && !strcmp(event->key,"count")) ++*static_cast<unsigned*>(user);
        }, &changes, &subscription) == TSUCHI_OK);
        // A runtime can stop a context directly; the settings step must release its overrides too.
        tsuchi::mod_stop(rules->context);
        run_frames(1);
        int64_t value = 0;
        CHECK(changes == 1 && tsuchi::host.settings->integer(target->context,"count",&value) == TSUCHI_OK && value == 3);
        CHECK(tsuchi::host.settings->controllers(target->context,"count",controllers,sizeof controllers,&needed) == TSUCHI_OK && !*controllers);
        CHECK(tsuchi::host.settings->set_integer(target->context,"count",5) == TSUCHI_OK);
    }
    tsuchi::forget_mods(); run_frames(1);
    disk.files = saved_files;
}

void telling_of_mods() {
    override_service_checks();
    const std::string mods_at = std::string(tsuchi::kModsFolder) + "/";
    const auto log_is = [](const std::string& expected) {
        const bool same = logged.lines == expected;
        if (!same) std::printf("  logged: [%s]\n", logged.lines.c_str());
        logged.lines.clear();
        return same;
    };
    tsuchi::forget_mods();
    const auto other_files = disk.files;
    disk.files.clear();
    logged.lines.clear();
    // Nobody listens: nothing is said, and nothing is needed for it.
    const size_t out = blocks_out.size();
    tsuchi::log_mods();
    CHECK(blocks_out.size() == out);
    tsuchi::set_log_sink(log_to_file);
    // No mods at all.
    CHECK(tsuchi::find_mods() == TSUCHI_OK);
    tsuchi::start_mods();
    tsuchi::log_mods();
    CHECK(log_is(std::string("[tsuchi] api ") + std::to_string(TSUCHI_API_VERSION) + ", game " + static_cast<const char*>(t::version) + ": 0 mods, 0 running, 0 blocked\n"));
    // Each mod by its ID: one that runs with its version, and one that cannot with the reason the mod list has.
    disk.files[mods_at + "told.one/mod.json"] = R"({"id": "told.one", "name": "One", "version": "1.2.3"})";
    disk.files[mods_at + "told.needy/mod.json"] = R"({"id": "told.needy", "name": "Needy", "version": "0.1.0", "requires": {"told.absent": "*"}})";
    disk.files[mods_at + "told.broken/mod.json"] = "{\n  \"id\": \"told.broken\",\n  \"name\":\n}\n";
    disk.files[mods_at + "Not An ID/readme.txt"] = "x";
    CHECK(tsuchi::find_mods() == TSUCHI_OK);
    tsuchi::start_mods();
    tsuchi::log_mods();
    const std::string summary = std::string("[tsuchi] api ") + std::to_string(TSUCHI_API_VERSION) + ", game " + static_cast<const char*>(t::version) + ": 4 mods, 1 running, 3 blocked\n";
    CHECK(logged.lines.find(summary) == 0 && logged.lines.find("[tsuchi] mod told.one 1.2.3 running\n") != std::string::npos &&
          logged.lines.find("[tsuchi] mod told.needy blocked: it needs told.absent, which is not installed\n") != std::string::npos &&
          logged.lines.find("[tsuchi] mod told.broken blocked: mod.json, line 4, column 1: ") != std::string::npos &&
          logged.lines.find("[tsuchi] mod ? blocked: its folder's name is no ID: Not An ID\n") != std::string::npos);
    CHECK(std::count(logged.lines.begin(), logged.lines.end(), '\n') == 5);
    logged.lines.clear();
    // Choices that cannot be read keep no mod from running: they all run in the default order, and the log says so.
    static TSUCHI_Result choices_result = TSUCHI_INVALID;
    static uint32_t choices_attempts = 0;
    static const char* choices_activation = "", *choices_order = "";
    tsuchi::set_mod_choices_loader([](tsuchi::ModChoices& choices) {
        ++choices_attempts;
        return choices_result == TSUCHI_OK ? choices.load(choices_activation, choices_order) : choices_result;
    });
    choices_result = TSUCHI_OK;
    CHECK(tsuchi::load_mod_choices() == TSUCHI_OK);
    for (const TSUCHI_Result failed : {TSUCHI_INVALID, TSUCHI_CAPACITY, TSUCHI_IO}) {
        choices_result = failed;
        CHECK(tsuchi::find_mods() == TSUCHI_OK);
        CHECK(tsuchi::find_mod("told.one") && tsuchi::find_mod("told.one")->state == tsuchi::ModState::ready);
        CHECK(logged.lines.find("could not be read, so every mod runs in the default order") != std::string::npos);
        tsuchi::forget_mods(); logged.lines.clear();
    }

    // A busy save retains one parsed catalogue privately, within the bound of 300 attempts. Its settings and names
    // are the original snapshot when choices arrive, even if files change meanwhile.
    const auto before_waiting = disk.files;
    disk.files[mods_at + "told.one/mod.json"] = R"({"id":"told.one","name":"Retained One","version":"1.2.3","settings":{"loud":{"type":"toggle","default":true}}})";
    disk.files[mods_at + "told.off/mod.json"] = R"({"id":"told.off","name":"Off","version":"1.0.0"})";
    disk.files[mods_at + "told.zlast/mod.json"] = R"({"id":"told.zlast","name":"Last","version":"1.0.0"})";
    disk.files["save:/tsuchi/told.one/settings.json"] = R"({"loud":false})";
    choices_result = TSUCHI_NOT_READY;
    choices_attempts = 0;
    const size_t folders_before_wait = disk.listings.size(), files_before_wait = disk.opened.size();
    for (uint32_t attempt = 0; attempt < 299; ++attempt) {
        CHECK(tsuchi::find_mods() == TSUCHI_NOT_READY);
        CHECK(tsuchi::mod_count() == 0 && !tsuchi::mod_at(0) && !tsuchi::find_mod("told.one"));
        CHECK(disk.listings.size() == folders_before_wait + 1 && disk.opened.size() == files_before_wait + 5 && closed());
        if (!attempt) disk.files[mods_at + "told.one/mod.json"] = "{ changed while waiting";
    }
    for (const char* id : {"told.one", "told.needy", "told.broken", "told.off", "told.zlast"})
        CHECK(std::count(disk.opened.begin() + files_before_wait, disk.opened.end(), mods_at + id + "/mod.json") == 1);
    CHECK(std::count(disk.opened.begin() + files_before_wait, disk.opened.end(), "save:/tsuchi/told.one/settings.json") == 0);
    choices_activation = "p4a1:told.off:";
    choices_order = "p4o1:told.zlast:told.one:";
    choices_result = TSUCHI_OK;
    CHECK(tsuchi::find_mods() == TSUCHI_OK && choices_attempts == 300 && tsuchi::mod_count() == 6);
    CHECK(disk.listings.size() == folders_before_wait + 1 && disk.opened.size() == files_before_wait + 5 && closed());
    auto* retained = tsuchi::find_mod("told.one");
    auto* disabled = tsuchi::find_mod("told.off");
    auto* first = tsuchi::find_mod("told.zlast");
    CHECK(retained && disabled && first);
    if (retained && disabled && first) {
        bool loud = false;
        auto* declared = tsuchi::find_setting(retained->settings, "loud");
        CHECK(std::string(retained->name.get()) == "Retained One" && !retained->context && !retained->settings.read &&
              declared && tsuchi::setting_toggle(*declared, loud) == TSUCHI_OK && loud);
        CHECK(disabled->state == tsuchi::ModState::disabled && !disabled->context && first->order < retained->order);
        tsuchi::start_mods();
        CHECK(retained->state == tsuchi::ModState::running &&
              tsuchi::host.settings->toggle(retained->context, "loud", &loud) == TSUCHI_OK && !loud);
        CHECK(disabled->state == tsuchi::ModState::disabled && !disabled->context);
    }
    tsuchi::forget_mods();
    disk.files = before_waiting;
    choices_activation = choices_order = "";
    CHECK(tsuchi::load_mod_choices() == TSUCHI_OK);
    logged.lines.clear();

    // Cancelling a pending catalogue releases it. A new pending search counts its own attempts and
    // stays private without repeating folder or manifest IO until the bound of 300.
    choices_result = TSUCHI_NOT_READY;
    choices_attempts = 0;
    const size_t before_pending = blocks_out.size();
    for (uint32_t attempt = 0; attempt < 75; ++attempt) {
        CHECK(tsuchi::find_mods() == TSUCHI_NOT_READY && tsuchi::mod_count() == 0);
    }
    CHECK(choices_attempts == 75 && blocks_out.size() > before_pending);
    tsuchi::forget_mods();
    CHECK(tsuchi::mod_count() == 0 && blocks_out.size() == before_pending && closed());
    choices_attempts = 0;
    const size_t folders_before_pending = disk.listings.size(), files_before_pending = disk.opened.size();
    for (uint32_t attempt = 0; attempt < 299; ++attempt) {
        CHECK(tsuchi::find_mods() == TSUCHI_NOT_READY && tsuchi::mod_count() == 0);
        CHECK(disk.listings.size() == folders_before_pending + 1 && disk.opened.size() == files_before_pending + 3 && closed());
    }
    CHECK(logged.lines.empty() && choices_attempts == 299);
    // On the 300th the mods run in the default order, and the log says why.
    CHECK(tsuchi::find_mods() == TSUCHI_OK && tsuchi::mod_count() == 4 && choices_attempts == 300);
    CHECK(disk.listings.size() == folders_before_pending + 1 && disk.opened.size() == files_before_pending + 3 && closed());
    CHECK(logged.lines.find("the save storage did not open for the Mods menu's choices, so every mod runs in the default order") != std::string::npos);
    tsuchi::forget_mods(); logged.lines.clear();
    tsuchi::set_mod_choices_loader(nullptr);
    // One mod is `1 mod`. With no memory for a line the line is lost, and the rest are said.
    disk.files.erase(mods_at + "told.needy/mod.json"), disk.files.erase(mods_at + "told.broken/mod.json"), disk.files.erase(mods_at + "Not An ID/readme.txt");
    CHECK(tsuchi::find_mods() == TSUCHI_OK);
    tsuchi::start_mods();
    allocations_left = 0;
    tsuchi::log_mods();
    allocations_left = -1;
    CHECK(log_is(""));
    tsuchi::log_mods();
    CHECK(log_is(std::string("[tsuchi] api ") + std::to_string(TSUCHI_API_VERSION) + ", game " + static_cast<const char*>(t::version) + ": 1 mod, 1 running, 0 blocked\n[tsuchi] mod told.one 1.2.3 running\n"));
    tsuchi::forget_mods();
    tsuchi::set_log_sink(nullptr);
    disk.files = other_files;
    disk.log.clear();
}

// ---- The SDK's helper for checks ------------------------------------------------------------------------------

void helping() {
    const std::string mods_at = std::string(tsuchi::kModsFolder) + "/";
    const auto log_is = [](const std::string& expected) {
        const bool same = logged.lines == expected;
        if (!same) std::printf("  logged: [%s]\n", logged.lines.c_str());
        logged.lines.clear();
        return same;
    };
    tsuchi::forget_mods();
    const auto other_files = disk.files;
    disk.files.clear();
    logged.lines.clear();
    tsuchi::set_log_sink(log_to_file);

    // For compiled mods. A check is planned, may wait, and ends once: with `pass`, or with `fail` and the first
    // expectation that did not hold, with what was got and what was wanted.
    TSUCHI_Mod* const helped = tsuchi::mod_create("helped.compiled");
    tsuchi_check_start(&tsuchi::host, helped);
    tsuchi_check_plan("one"), tsuchi_check_plan("two");
    CHECK(log_is("[helped.compiled] check one planned\n[helped.compiled] check two planned\n"));
    // What a check waits for is said when it changes, for each check by itself.
    tsuchi_check_waiting("one", "a level"), tsuchi_check_waiting("one", "a level"), tsuchi_check_waiting("two", "a level"), tsuchi_check_waiting("one", "a level");
    tsuchi_check_waiting("one", "the player in control"), tsuchi_check_waiting("two", "a level"), tsuchi_check_waiting("one", "a level");
    CHECK(log_is("[helped.compiled] check one waiting a level\n[helped.compiled] check two waiting a level\n[helped.compiled] check one waiting the player in control\n"
                 "[helped.compiled] check one waiting a level\n"));
    tsuchi_check_begin("one");
    CHECK(tsuchi_expect(true, "something true") && tsuchi_expect_result(TSUCHI_OK, TSUCHI_OK, "a call") && tsuchi_expect_number(-7, -7, "a number") &&
          tsuchi_expect_near(1.25, 1.0, 0.25, "a number near another") && tsuchi_expect_text("same", "same", "a text") && tsuchi_expect_bytes("abc", "abd", 2, "bytes") &&
          tsuchi_check_holds() && tsuchi_check_end());
    tsuchi_check_begin("two");
    CHECK(!tsuchi_expect_result(TSUCHI_NOT_FOUND, TSUCHI_OK, "reading the piece") && !tsuchi_check_holds() && !tsuchi_expect_number(5, 6, "told second, so not told") &&
          tsuchi_expect(true, "what holds after a failure changes nothing") && !tsuchi_check_end());
    CHECK(log_is("[helped.compiled] check one pass\n[helped.compiled] check two fail reading the piece: got not_found, wanted ok\n"));
    // Each kind of expectation as the line its failure writes.
    const auto fails_with = [&](const char* why, bool held) {
        const bool passed = tsuchi_check_end();
        return !held && !passed && log_is(std::string("[helped.compiled] check each fail ") + why + "\n");
    };
    tsuchi_check_begin("each");
    CHECK(fails_with("the count: got -5, wanted 6", tsuchi_expect_number(-5, 6, "the count")));
    tsuchi_check_begin("each");
    CHECK(fails_with("a large one: got -9223372036854775808, wanted 9223372036854775807", tsuchi_expect_number(INT64_MIN, INT64_MAX, "a large one")));
    tsuchi_check_begin("each");
    CHECK(fails_with("the name: got \"abc\", wanted \"abd\"", tsuchi_expect_text("abc", "abd", "the name")));
    tsuchi_check_begin("each");
    CHECK(fails_with("the life: got 1.235, wanted 2.000", tsuchi_expect_near(1.2345, 2, 0.5, "the life")));
    tsuchi_check_begin("each");
    CHECK(fails_with("below: got -0.250, wanted 0.000", tsuchi_expect_near(-0.25, 0, 0.125, "below")));
    tsuchi_check_begin("each");
    CHECK(fails_with("no number: got nan, wanted 1000000.000", tsuchi_expect_near(std::nan(""), 1e6, 1, "no number")));
    tsuchi_check_begin("each");
    CHECK(fails_with("too large: got large, wanted 0.000", tsuchi_expect_near(1e300, 0, 1, "too large")));
    tsuchi_check_begin("each");
    CHECK(fails_with("what was read: byte 2 is 120, wanted 105", tsuchi_expect_bytes("gax", "gai", 3, "what was read")));
    tsuchi_check_begin("each");
    CHECK(fails_with("a call: got unknown, wanted no_memory", tsuchi_expect_result(TSUCHI_Result(77), TSUCHI_NO_MEMORY, "a call")));
    tsuchi_check_begin("each");
    CHECK(fails_with("plainly false", tsuchi_expect(false, "plainly false")));
    // A note with a pass, and a failure far longer than a line: as much of it as a line holds.
    tsuchi_check_begin("noted");
    CHECK(tsuchi_check_end_with("fresh") && log_is("[helped.compiled] check noted pass fresh\n"));
    tsuchi_check_begin("long");
    tsuchi_expect_text(std::string(900, 'x').c_str(), "short", "a text without end");
    CHECK(!tsuchi_check_end() && logged.lines.size() > 400 && logged.lines.size() < 520 && logged.lines.find("[helped.compiled] check long fail a text without end: got \"xxx") == 0);
    logged.lines.clear();
    // The names of the things a check acts on, a line each, from a file of the mod's.
    char names[64];
    const char* lines[4] = {};
    disk.files[mods_at + "helped.compiled/targets.txt"] = "# what the check acts on\n/G/Sheet.Sheet\r\n\nCHAPPY\nlast";
    CHECK(tsuchi_check_lines("targets.txt", names, sizeof names, lines, 4) == 3 && !std::strcmp(lines[0], "/G/Sheet.Sheet") && !std::strcmp(lines[1], "CHAPPY") && !std::strcmp(lines[2], "last"));
    CHECK(tsuchi_check_lines("targets.txt", names, sizeof names, lines, 2) == -1 && tsuchi_check_lines("targets.txt", names, 20, lines, 4) == -1 &&
          tsuchi_check_lines("absent.txt", names, sizeof names, lines, 4) == -1);
    disk.files[mods_at + "helped.compiled/targets.txt"] = "";
    CHECK(tsuchi_check_lines("targets.txt", names, sizeof names, lines, 4) == 0);
    tsuchi::mod_stop(helped);

    // For scripts: the same lines. An error in a check fails the check and no more, and a check that waits is run
    // again as steps go by, until it ends.
    disk.files.clear();
    disk.files[mods_at + "helped.script/mod.json"] = R"({"id": "helped.script", "name": "Helped", "version": "1.0.0"})";
    disk.files[mods_at + "helped.script/check.lua"] = std::string(check_mods_helper, check_mods_helper_size);
    disk.files[mods_at + "helped.script/main.lua"] = R"(local check = require("check")
check.plan("one", "two", "three")
print(check.run("one", function()
    check.expect(true, "something true")
    check.equal("a", "a", "a text")
    check.near(1.25, 1, 0.25, "a number near another")
    print(check.result("ok", "a call that gives a value", 5), check.result("ok", "a call that gives none", nil), check.result("not_found", "a call that fails", nil, "not_found"))
end))
print(check.run("two", function()
    check.result("ok", "reading the piece", nil, "not_found")
    check.equal(5, 6, "told second, so not told")
end))
print(check.run("three", function() return "fresh" end))
for _, body in ipairs({
    function() check.expect(false, "plainly false") end,
    function() check.expect(nil, "nothing is not true") end,
    function() check.equal(5, 6, "the count") end,
    function() check.equal("abc", "abd", "the name") end,
    function() check.equal(nil, true, "a value") end,
    function() check.near(1.25, 2, 0.5, "the life") end,
    function() check.near("1.25", 1.25, 0.5, "text for a number") end,
    function() check.result("invalid", "a name that leads out", true) end,
    function() local missing = nil; return missing.field end,
    function() error("on purpose") end,
    function() error({code = 7}) end,
    function() check.expect(false, "a failure cannot become a wait"); check.wait("the world") end,
}) do check.run("each", body) end
-- A check that waits: tried again every other step, and no more once it has ended.
local tries = 0
check.retry(function()
    return check.run("patient", function()
        tries = tries + 1
        if tries < 3 then check.wait("the save") end
        if tries < 5 then check.wait("a level") end
        return "after " .. tries .. " tries"
    end)
end, 2)
check.retry(function() return check.run("at once", function() end) end)
)";
    CHECK(tsuchi::find_mods() == TSUCHI_OK);
    tsuchi::start_mods();
    const tsuchi::ModRecord* script = tsuchi::find_mod("helped.script");
    CHECK(script && script->state == tsuchi::ModState::running);
    if (script && script->state != tsuchi::ModState::running) std::printf("  helped.script: %s\n", script->problem.get());
    const std::string by = "[helped.script] ";
    CHECK(log_is(by + "check one planned\n" + by + "check two planned\n" + by + "check three planned\n" + by + "5\tnil\tnil\n" + by + "check one pass\n" + by + "pass\n" + by +
                 "check two fail reading the piece: got not_found, wanted ok\n" + by + "fail\n" + by + "check three pass fresh\n" + by + "pass\n" + by + "check each fail plainly false\n" + by +
                 "check each fail nothing is not true\n" + by + "check each fail the count: got 5, wanted 6\n" + by + "check each fail the name: got \"abc\", wanted \"abd\"\n" + by +
                 "check each fail a value: got nil, wanted true\n" + by + "check each fail the life: got 1.25, wanted 2\n" + by + "check each fail text for a number: got \"1.25\", wanted 1.25\n" +
                 by + "check each fail a name that leads out: got ok, wanted invalid\n" + by + "check each fail error: helped.script/main.lua:23: attempt to index a nil value (local 'missing')\n" +
                 by + "check each fail error: helped.script/main.lua:24: on purpose\n" + by + "check each fail error: a value that is no text\n" + by + "check each fail a failure cannot become a wait\n" + by + "check patient waiting the save\n" + by +
                 "check at once pass\n"));
    logged.lines.clear();
    run_frames(1);
    CHECK(log_is(""));
    run_frames(1); // the second step: the second try
    CHECK(log_is(""));
    run_frames(2); // the third try: it waits for something else
    CHECK(log_is(by + "check patient waiting a level\n"));
    run_frames(2);
    CHECK(log_is(""));
    run_frames(2);
    CHECK(log_is(by + "check patient pass after 5 tries\n"));
    const uint32_t owned = tsuchi::owned_count(script->context);
    run_frames(10);
    CHECK(log_is("") && tsuchi::owned_count(script->context) == owned && owned == 1); // the script's state, and no listener left
    tsuchi::forget_mods();
    tsuchi::set_log_sink(nullptr);
    run_frames(61); // what is left for the log's file is written, and none of it is this check's to look at
    disk.files = other_files;
    disk.folders.clear();
    disk.log.clear(), disk.changes.clear();
}

#include "ai_order_checks.h"

// ---- The check mods among the fixtures ------------------------------------------------------------------------

// What the made-up game has for the check mods to act on, under the names tests/game/check_mods gives them: a
// table that is an asset with a path of its own, with one of the creatures' rows in it.
void check_world() {
    // The actor check asks the engine for the same place independently of the service.
    const uint64_t result = t::constant::property_param | t::constant::property_out | t::constant::property_return;
    const uint64_t vector = fake::memory(0x80);
    fake::put<uint64_t>(vector, fake::engine().vtable);
    for (int i = 0; i < 3; ++i) fake::field(vector, std::u16string(1, char16_t(u'X' + i)), u"FloatProperty", 4 * i, 4);
    const uint64_t where = fake::function(actor_base_class, function_type, u"K2_GetActorLocation", 12, 4, [](uint64_t object, uint64_t arguments) {
        // As the engine's: an actor without a root is at the origin.
        const uint64_t root = fake::get<uint64_t>(object + t::game_actor::root);
        for (int i = 0; i < 3; ++i) fake::put(arguments + 4 * i, root ? fake::get<float>(root + t::scene_component::location + 4 * i) : 0.0f);
    });
    fake::flag(fake::field(where, u"ReturnValue", u"StructProperty", 0, 12, vector), result);
    const uint64_t creatures = w.creature_table;
    const uint64_t package = fake::object(u"/G/Sheet", w.component_class, 0x40), sheet = fake::object(u"Sheet", fake::get<uint64_t>(creatures + t::object::class_), 0x60);
    fake::put<uint64_t>(package + t::object::outer, 0), fake::put(sheet + t::object::outer, package);
    fake::put(sheet, fake::get<uint64_t>(creatures)), fake::put(sheet + t::data_table::row_struct, fake::get<uint64_t>(creatures + t::data_table::row_struct));
    const uint64_t row = fake::memory(kRowBytes);
    std::memcpy(reinterpret_cast<void*>(row), reinterpret_cast<const void*>(sheets.rows[{creatures, row_name(u"CHAPPY")}]), kRowBytes);
    sheets.rows[{sheet, row_name(u"CHAPPY")}] = row;
    std::u16string path = u"/G/Sheet.Sheet";
    shelf.assets[fake::engine_name(path.data(), path.size(), 1)] = sheet;
    // A second table holds the reference, so the check holds and releases two different assets.
    const uint64_t ref_package = fake::object(u"/G/Refs", w.component_class, 0x40), refs = fake::object(u"Refs", fake::get<uint64_t>(creatures + t::object::class_), 0x60);
    fake::put<uint64_t>(ref_package + t::object::outer, 0), fake::put(refs + t::object::outer, ref_package);
    fake::put(refs, fake::get<uint64_t>(creatures)), fake::put(refs + t::data_table::row_struct, fake::get<uint64_t>(creatures + t::data_table::row_struct));
    const uint64_t ref_row = fake::memory(kRowBytes);
    std::memcpy(reinterpret_cast<void*>(ref_row), reinterpret_cast<const void*>(row), kRowBytes);
    sheets.rows[{refs, row_name(u"CHAPPY")}] = ref_row;
    path = u"/G/Refs.Refs";
    shelf.assets[fake::engine_name(path.data(), path.size(), 1)] = refs;
    // A carryable object for the carry check, beside the first captain: the game's carry component with a weight
    // of one and empty carrier records.
    {
        const uint64_t carry_class = fake::object(u"TestCarryAffordanceComponent", w.root);
        fake::set_super(carry_class, w.component_class);
        const uint64_t chief_root = fake::get<uint64_t>(w.chief + t::game_actor::root);
        const float x = chief_root ? fake::get<float>(chief_root + t::scene_component::location) + 100.0f : 100.0f;
        const uint64_t pellet = tabled(table_a(), u"Pellet", u"Pellet", 0, x);
        registers(pellet);
        check_carry_actor = pellet;
        check_carry_part = fake::object(u"PelletCarry", carry_class, 0x700);
        fake::put(pellet + t::carry_actor::component, check_carry_part);
        fake::put<int32_t>(check_carry_part + t::carry_component::weight, 1);
        fake::put<int32_t>(check_carry_part + t::carry_component::maximum, 2);
        const uint64_t at = check_carry_part + t::carry_component::carriers;
        fake::put(at + t::array::data, fake::memory(32 * 8));
        fake::put<int32_t>(at + t::array::count, 0), fake::put<int32_t>(at + t::array::max, 32);
        fake::put(check_carry_part + t::carry_component::info, fake::memory(0x100));
    }
}

// A stock spawned actor also has a skeletal mesh, as the game makes with the actor's Blueprint.
uint64_t check_spawn_source = 0;
void* check_spawn_now(void* framework, void* request) {
    void* made = game_spawn_now(framework, request);
    if (!made) return nullptr;
    const uint64_t actor = reinterpret_cast<uint64_t>(made), mesh = fake::engine_object(u"CheckMesh", skeletal_class, 0xC0);
    // As the game does: a request's team goes onto the actor, and the default leaves the class's own.
    if (spawner.team == int32_t(t::constant::spawn_team_default) && check_spawn_source)
        fake::put(actor + t::game_actor::team, fake::get<int32_t>(check_spawn_source + t::game_actor::team));
    fake::put(mesh + t::scene_component::owner, actor);
    fake::put<uint8_t>(mesh + kAnimationModeAt, uint8_t(t::constant::animation_by_blueprint));
    parts_of[actor].push_back(mesh);
    const uint64_t body = physical(actor), root = fake::get<uint64_t>(actor + t::game_actor::root), table = fake::memory(0x60);
    std::memcpy(reinterpret_cast<void*>(table), reinterpret_cast<const void*>(primitive_table), 0x60);
    fake::put(table + t::object_vtable::process_event, &fake::engine_process), fake::put(body, table);
    part_states[body] = {true, kCollisionBoth, 0};
    parts_of[actor].push_back(body);
    const uint64_t children = fake::memory(8);
    fake::put(children, body), fake::put(root + t::scene_component::attach_children + t::array::data, children);
    fake::put<int32_t>(root + t::scene_component::attach_children + t::array::count, 1);
    if (tsuchi::game::derives_from(fake::get<uint64_t>(actor + t::object::class_), w.pikmin_class)) {
        const uint64_t part = fake::object(u"OwnedPikminPart", w.component_class, 0x40);
        fake::put(actor + t::game_actor::pikmin, part), fake::put(actor + t::game_actor::kind, fake::name(u"PikminRed"));
        fake::put<uint8_t>(actor + t::game_actor::color, 0), fake::put<uint8_t>(part + t::pikmin_component::leaf, 0);
        fake::put<uint8_t>(part + t::pikmin_component::in_army, 0);
    }
    if (check_revive_class && fake::get<uint64_t>(actor + t::object::class_) == check_revive_class) {
        fake::put(actor + t::game_actor::kind, fake::name(u"KOCHAPPY"));
        fake::put(actor + t::game_actor::group, fake::get<tsuchi::game::Name>(fake::main_image() + t::data::creature_group));
    }
    if (!tsuchi::game::is_pikmin(actor)) {
        // Life-bearing host copies, including the generic CHAPPY source, receive messages through their decisions queue.
        const uint64_t mind = fake::object(u"OwnedCreatureBrain", tick_component_class, 0x80);
        const uint64_t functions = fake::memory(0x40), queue = fake::memory(0x40);
        fake::put(functions + t::component_table::set_tick_enabled, &game_component_tick);
        fake::put(functions + t::object_vtable::process_event, &fake::engine_process);
        fake::put(mind, functions), fake::put(queue, fake::main_image() + t::data::message_queue_table);
        fake::put(mind + t::ai_component::messages, queue), fake::put(actor + t::game_actor::ai, mind);
        fake::put<uint8_t>(mind + t::game_component::tick_requested, 1);
        const uint64_t movement = fake::object(u"OwnedCreatureMover", tick_component_class, 0x80);
        fake::put(movement, functions);
        fake::put<uint8_t>(movement + t::game_component::tick_requested, 1);
        fake::put(movement + t::scene_component::owner, actor);
        fake::put(mind + t::scene_component::owner, actor);
        fake::put(actor + t::game_actor::movement, movement);
        parts_of[actor].push_back(mind), parts_of[actor].push_back(movement);
    }
    if (check_cleanup_spawn) {
        // This model supplies the same native prerequisites that a successful game startup must have.
        fake::put<uint8_t>(actor + t::game_actor::tick_requested, 1);
        const uint64_t mind = fake::get<uint64_t>(actor + t::game_actor::ai);
        fake::put(mind + t::object::class_, tick_component_class);
        fake::put(mind + t::scene_component::owner, actor);
        fake::put<uint8_t>(mind + t::game_component::tick_requested, 1);
        const uint64_t mover = fake::engine_object(u"CleanupMover", tick_component_class, 0x100);
        const uint64_t functions = fake::memory(0x60);
        fake::put(functions + t::component_table::set_tick_enabled, &game_component_tick);
        fake::put(functions + t::object_vtable::process_event, &fake::engine_process);
        fake::put(mover, functions); fake::put(mover + t::scene_component::owner, actor);
        fake::put<uint8_t>(mover + t::game_component::tick_requested, 1);
        fake::put(actor + t::game_actor::movement, mover); parts_of[actor].push_back(mover);
        const uint64_t cleanup_children = fake::memory(8 * 64);
        fake::put(cleanup_children, body);
        fake::put(root + t::scene_component::attach_children + t::array::data, cleanup_children);
        draw(mesh, true); slots_of[mesh] = 2; attach_under(root, mesh);
    }
    give_life(actor, 50, 50);
    check_bodies[actor] = {body, fake::get<float>(root + t::scene_component::location + 8)};
    return made;
}

std::set<uint64_t> check_charge_messages;
std::map<uint64_t, uint64_t> check_worker_brains;
void check_charge_order(void* order, void* sender) {
    game_charge_order(order, sender);
    check_charge_messages.insert(reinterpret_cast<uint64_t>(order));
}

// The fake game's message handler applies hits to the owned creatures, through its stock life chain.
bool check_post_message(void* victim, void* memory) {
    const uint64_t actor = reinterpret_cast<uint64_t>(victim), message = reinterpret_cast<uint64_t>(memory);
    if (check_revive_requests.erase(message)) {
        const bool accepted = game_post_revive(victim, memory);
        CHECK(revived.creature == actor && revived.sender == actor);
        if (accepted) check_revivals[actor] = 3;
        return accepted;
    }
    if (check_charge_messages.erase(message)) {
        const bool accepted = game_post_order(victim, memory);
        if (accepted && check_worker_brains.count(actor)) now_doing(check_worker_brains[actor], {u"Work"});
        return accepted;
    }
    const bool hit = fake::get<uint64_t>(message) == fake::main_image() + t::data::damage_message_table;
    const uint64_t attacker = fake::get<uint64_t>(message + 8);
    const float amount = hit ? fake::get<float>(message + t::damage_message::amount) : 0;
    const int kind = hit ? fake::get<uint8_t>(message + t::damage_message::attack) : 0;
    TSUCHI_EntityParts held = 0;
    CHECK(tsuchi::host.entity->suspended(mod, TSUCHI_Actor{tsuchi::game::to_handle(actor)}, &held) == TSUCHI_OK);
    if (held & TSUCHI_ENTITY_PARTS_LOGIC) {
        // The ordinary host hit constructor keeps a raw sender; its native queue uses a weak identity.
        const uint64_t sender = attacker ? tsuchi::game::to_weak_value(attacker) : uint64_t(0xFFFFFFFF);
        fake::put(message + t::message::sender, sender);
        fake::put<int32_t>(message + t::message::references, 1);
        switches.waiting.push_back(message);
        return true;
    }
    const bool accepted = take_message(victim, memory);
    if (accepted && hit && check_bodies.count(actor)) {
        game_hits(fake::get<uint64_t>(actor + t::game_actor::life), attacker, kind, amount);
        if (check_revive_class && fake::get<uint64_t>(actor + t::object::class_) == check_revive_class &&
            fake::get<float>(fake::get<uint64_t>(actor + t::game_actor::life) + t::life_component::current) <= 0) check_defeats[actor] = 2;
    }
    return accepted;
}

// The fake game counts a non-quiet Pikmin removal through its stock report hook, then queues removal.
void check_kill_actor(void* record_pointer, void* which) {
    const uint64_t record = reinterpret_cast<uint64_t>(record_pointer), actor = reinterpret_cast<uint64_t>(which);
    if (!fake::get<uint8_t>(record + t::kill_record::quiet) && tsuchi::game::is_pikmin(actor)) {
        using ReportKill = uint64_t (*)(uint64_t, uint64_t, uint64_t);
        const auto counted = reinterpret_cast<ReportKill>(tsuchi_hook_target(tsuchi::kCodeSiteThunk | t::code_site::pikmin_report_kill));
        CHECK(counted(actor, fake::get<uint64_t>(record + t::kill_record::killer), fake::get<uint8_t>(record + t::kill_record::cause)) == 77);
    }
    game_kill(record_pointer, which);
}

// The fake game creates and counts the Pikmin its Onion-style eject function gives out.
void check_eject_pikmin(void* leader, void* transform, int32_t type, int32_t leaf, int32_t count, uint64_t sublevel, bool not_counted, int32_t decoration) {
    game_eject_pikmin(leader, transform, type, leaf, count, sublevel, not_counted, decoration);
    const uint64_t owner = reinterpret_cast<uint64_t>(leader), head = w.framework + t::game_framework::pikmin_list;
    const auto* at = static_cast<const float*>(transform);
    for (int32_t i = 0; i < count; ++i) {
        const uint64_t made = actor(u"CheckAdded", w.pikmin_class, u"Pikmin", fake::get<int32_t>(owner + t::game_actor::team), ~uint64_t(0), component(at[4], at[5], at[6], 0));
        fake::put(made, table_a());
        const uint64_t part = fake::object(u"CheckAddedPikmin", w.component_class, 0x40), node = fake::memory(t::actor_node::size);
        fake::put(made + t::game_actor::pikmin, part);
        fake::put<uint8_t>(made + t::game_actor::color, uint8_t(type)), fake::put<uint8_t>(part + t::pikmin_component::leaf, uint8_t(leaf));
        fake::put<uint8_t>(part + t::pikmin_component::in_army, !not_counted);
        leading.leaders[made] = owner;
        const uint64_t brain = give_brain(made);
        now_doing(brain, {u"Follow"}), check_worker_brains[made] = brain;
        const uint64_t body = physical(made);
        part_states[body] = {true, kCollisionBoth, 0};
        check_bodies[made] = {body, 0};
        fake::put(node + t::actor_node::actor, made), fake::put(node + t::actor_node::next, fake::get<uint64_t>(head + t::actor_node::next));
        fake::put(head + t::actor_node::next, node);
        registers(made);
    }
}
void check_dismiss_squad(void* order) {
    game_dismiss_squad(order);
    if (check_dismiss_delayed) {
        CHECK(!check_dismiss_target || check_dismiss_target == herd.dismissed);
        check_dismiss_target = herd.dismissed;
        ++check_dismiss_requests;
        // The first native request stays pending beyond the old 90-update
        // deadline; only the repeat can release it, on a later native update.
        if (check_dismiss_requests == 2) check_dismiss_steps = 2;
        return;
    }
    for (auto& member : leading.leaders) if (member.second == herd.dismissed) member.second = 0;
}

// The game's warp turns a body and drops its old velocity at the new ground.
void check_warp_actor(void* which, void* location, void* angles, bool turn, bool ground, bool height) {
    game_warp(which, location, angles, turn, ground, height);
    const uint64_t actor = reinterpret_cast<uint64_t>(which);
    auto found = check_bodies.find(actor);
    if (found == check_bodies.end()) return;
    const uint64_t root = fake::get<uint64_t>(actor + t::game_actor::root);
    const float* rotation = static_cast<const float*>(angles);
    float quaternion[4];
    quaternion_of(rotation[0], rotation[1], rotation[2], quaternion);
    for (int i = 0; i < 4; ++i) fake::put(root + t::scene_component::rotation + 4 * i, quaternion[i]);
    found->second.ground = fake::get<float>(root + t::scene_component::location + 8);
    for (int i = 0; i < 3; ++i) fake::put<float>(actor + t::game_actor::velocity + 4 * i, 0);
    fake::put<uint32_t>(actor + t::game_actor::air_steps, 0);
}

// What is printed before and after the log of one run of the check mods.
constexpr char kCheckLogBegins[] = "==== the log of a run of the check mods\n", kCheckLogEnds[] = "==== its end\n";

// The lines one mod wrote of its checks, by what they say of each check.
struct Reported {
    std::set<std::string> planned, passed, failed;
    std::string lines;
};
Reported reported_by(const std::string& log, const std::string& id) {
    Reported found;
    const std::string by = "[" + id + "] ";
    for (size_t at = 0; at < log.size();) {
        const size_t end = log.find('\n', at);
        const std::string line = log.substr(at, end - at);
        at = end + 1;
        if (line.compare(0, by.size(), by)) continue;
        found.lines += "    " + line + "\n";
        if (line.compare(by.size(), 6, "check ")) continue;
        const size_t name_at = by.size() + 6, word_at = line.find(' ', name_at);
        if (word_at == std::string::npos) continue;
        const std::string name = line.substr(name_at, word_at - name_at), word = line.substr(word_at + 1, line.find(' ', word_at + 1) - word_at - 1);
        if (word == "planned") found.planned.insert(name);
        if (word == "pass") found.passed.insert(name);
        if (word == "fail") found.failed.insert(name);
    }
    return found;
}

bool selected_check_fixture(const char* id, const char* selected) {
    // The failed-startup producer belongs only to its observer, not to other fixture boots.
    if (!std::strcmp(id, "check.cleanup-producer")) return selected && !std::strcmp(selected, "check.cleanup");
    for (const char* explicit_id : check_mod_explicit_ids) {
        if (!explicit_id) continue;
        if (!std::strcmp(id, explicit_id)) return selected && !std::strcmp(id, selected);
    }
    for (const char* check_id : check_mod_ids) {
        if (check_id && !std::strcmp(id, check_id)) return selected == nullptr;
    }
    return true; // a library dependency has no check group of its own
}

// Every check mod is installed as it is among the fixtures, with the mods it needs, and started as the game starts
// its mods. Each has to run, plan its checks, and pass every one of them. Then all of it once more on the save
// the first time left, as the game started again would find it, and a third time on a save that was prepared.
void checking_fixtures(bool ai_only = false) {
    const std::string mods_at = std::string(tsuchi::kModsFolder) + "/";
    const Talk old_talk = talk;
    talk.posted.clear(), talk.log.clear(), talk.answers.clear();
    tsuchi::forget_mods();
    const auto other_files = disk.files;
    disk.files.clear(), disk.folders.clear();
    disk.changes.clear(), disk.log.clear();
    tsuchi::set_log_sink(log_to_file);
    const uint64_t old_story = fake::get<uint64_t>(running.instance + t::game_instance::story_save);
    const uint64_t old_clock = fake::get<uint64_t>(w.framework + t::game_framework::clock);
    const Journey old_journey = journey;
    const std::string old_journey_log = journey.log, old_game_log = running.log;
    const auto old_title_function = t::fn::return_to_title;
    const auto old_travel_function = t::fn::travel_request;
    const uint64_t old_requests = fake::get<uint64_t>(running.context + t::game_context::requests);
    const uint64_t old_portal_class = fake::get<uint64_t>(fake::main_image() + t::data::portal_part_class);
    const uint64_t old_find_component = fake::get<uint64_t>(table_a() + t::actor_table::find_component);
    // The input check reads the system's controller and keyboard. The keyboard is in the system's library, beside
    // the loader the compiled mods are started with.
    const auto old_npad_style = t::fn::npad_style_set;
    const auto old_npad_full_key = t::fn::npad_full_key_state;
    t::fn::npad_style_set = check_npad_style, t::fn::npad_full_key_state = check_npad_state;
    const uint64_t anchor_at = fake::main_image() + t::data::system_library_anchor, old_anchor = fake::get<uint64_t>(anchor_at);
    const uint64_t old_library = tsuchi::game::module_containing(old_anchor);
    std::vector<fake::Offered> library = {{"_ZN2nn3hid18InitializeKeyboardEv", reinterpret_cast<uint64_t>(&check_keyboard_start)},
                                          {"_ZN2nn3hid16GetKeyboardStateEPNS0_13KeyboardStateE", reinterpret_cast<uint64_t>(&check_keyboard_state)}};
    for (const char* name : {"_ZN2nn2ro10InitializeEv", "_ZN2nn2ro18RegisterModuleInfoEPNS0_16RegistrationInfoEPKv",
                             "_ZN2nn2ro20UnregisterModuleInfoEPNS0_16RegistrationInfoE", "_ZN2nn2ro13GetBufferSizeEPmPKv",
                             "_ZN2nn2ro10LoadModuleEPNS0_6ModuleEPKvPvmi", "_ZN2nn2ro18LookupModuleSymbolEPmPKNS0_6ModuleEPKc"})
        if (const uint64_t at = old_library ? tsuchi::game::module_symbol(old_library, name) : 0) library.push_back({name, at});
    fake::put<uint64_t>(anchor_at, fake::loaded_module(library) + 0x2100);
    t::fn::return_to_title = check_return_to_title;
    t::fn::travel_request = game_travel_request;
    const uint64_t request_queue = fake::memory(0x20), request_table = fake::memory(0x20);
    fake::put(request_queue, request_table);
    fake::put(request_table + t::request_queue_table::push, &check_push_journey);
    fake::put(running.context + t::game_context::requests, request_queue);
    journey.posted.clear();
    int old_pause_counts[32];
    std::memcpy(old_pause_counts, running.counts, sizeof old_pause_counts);
    const uint32_t old_pause_active = fake::get<uint32_t>(running.pauses + t::pause_manager::active);
    const uint64_t config = fake::get<uint64_t>(running.instance + t::game_instance::config_save);
    CHECK(config != 0);
    const int32_t old_save_slot = fake::get<int32_t>(config + t::config_save::slot);
    fake::put<int32_t>(config + t::config_save::slot, 1);
    disk.files["save:/B/single.sav"] = "the check reads this existing save without changing it";
    const std::string old_announcement_log = announced.log;
    const auto old_sounds = announced.sounds;
    announced.sounds["Whistle_Charge"] = true;
    const uint64_t story = fake::object(u"CheckStory", w.component_class, 0x60), clock = fake::memory(0x20);
    fake::put<uint32_t>(story + t::story_save::day, 7);
    fake::put(running.instance + t::game_instance::story_save, story);
    fake::put<uint64_t>(clock, fake::main_image() + t::data::clock_table);
    fake::put(clock + t::clock::progress, 0.25f), fake::put(w.framework + t::game_framework::clock, clock);
    fake::put(fake::main_image() + t::data::clock_table + t::clock_table::set_progress, &game_clock_set);
    check_world();
    fake::name(u"Area002_P"); fake::name(u"Cave004_F01");
    // The existing story group uses original public identities in the game; this model supplies its own corresponding cave mouth.
    const Hoard old_hoard = hoard;
    const auto old_find_treasure = t::fn::find_treasure_row;
    const auto old_find_record = t::fn::find_story_record;
    const auto old_collect_treasure = t::fn::collect_treasure;
    const auto old_add_sparklium = t::fn::add_sparklium;
    const uint64_t treasure_row = fake::memory(0x400);
    const uint64_t treasure_name = row_name(u"OTADARTS");
    fake::put<int32_t>(treasure_row + t::treasure_row::sparklium, 60);
    fake::put<int32_t>(treasure_row + t::treasure_row::poko, 7);
    hoard = Hoard{}; hoard.story = story; hoard.context = running.context; hoard.rows[treasure_name] = treasure_row;
    t::fn::find_treasure_row = game_find_treasure_row; t::fn::find_story_record = game_find_story_record;
    t::fn::collect_treasure = game_collect_treasure; t::fn::add_sparklium = game_add_sparklium;
    const uint64_t portal_actor_class = fake::engine_object(u"CheckCaveMouth_C", w.root);
    fake::set_super(portal_actor_class, w.bulborb_class);
    const uint64_t portal_actor = actor(u"CheckCaveMouth", portal_actor_class, u"CHECKPORTAL", 0, 9001, component(100, 100, 0, 0));
    fake::put(portal_actor, table_a()); registers(portal_actor);
    const uint64_t portal_part_class = fake::engine_object(u"CheckPortalPart", w.root), portal_part = fake::memory(0x100);
    fake::put<int32_t>(portal_part + t::portal_part::leads_to, 0);
    fake::put(fake::main_image() + t::data::portal_part_class, portal_part_class);
    fake::put(table_a() + t::actor_table::find_component, &game_find_component);
    journey.portal_part = portal_part;
    // The stock template for the owned-actor check; the SDK chooses it by the same public kind as in the game.
    const std::string old_switch_log = switches.log;
    const auto old_charge_order = t::fn::charge_order;
    const auto old_ordered = ordered;
    const auto old_charge_messages = check_charge_messages;
    const auto old_worker_brains = check_worker_brains;
    t::fn::charge_order = check_charge_order;
    const auto old_kill_actor = t::fn::kill_actor;
    t::fn::kill_actor = check_kill_actor;
    const auto old_eject_pikmin = t::fn::eject_pikmin;
    const auto old_dismiss_squad = t::fn::dismiss_squad;
    const bool old_dismiss_delayed = check_dismiss_delayed;
    const uint64_t old_dismiss_target = check_dismiss_target;
    const uint32_t old_dismiss_requests = check_dismiss_requests;
    const int old_dismiss_steps = check_dismiss_steps;
    const auto old_herd = herd;
    t::fn::eject_pikmin = check_eject_pikmin, t::fn::dismiss_squad = check_dismiss_squad;
    const auto old_post_message = t::fn::post_message;
    const uint64_t old_revive_class = check_revive_class;
    const auto old_revive_request = t::fn::revive_request;
    const auto old_revived = revived;
    const auto old_revive_requests = check_revive_requests;
    const auto old_defeats = check_defeats, old_carcasses = check_carcasses, old_revivals = check_revivals;
    t::fn::revive_request = check_make_revive;
    const uint64_t revive_package = fake::object(u"/G/Revive", w.component_class, 0x40);
    check_revive_class = fake::engine_object(u"Revive_C", w.root);
    fake::set_super(check_revive_class, w.bulborb_class);
    fake::put<uint64_t>(revive_package + t::object::outer, 0), fake::put(check_revive_class + t::object::outer, revive_package);
    std::u16string revive_path = u"/G/Revive.Revive_C";
    shelf.assets[fake::engine_name(revive_path.data(), revive_path.size(), 1)] = check_revive_class;
    const auto old_posted = posted;
    const std::string old_life_log = life_log;
    const auto old_heard_drops = heard_drops;
    t::fn::post_message = check_post_message;
    const auto old_spawn_now = t::fn::spawn_now;
    const auto old_warp_actor = t::fn::warp_actor;
    t::fn::warp_actor = check_warp_actor;
    t::fn::spawn_now = check_spawn_now;
    const std::string old_part_log = part_log;
    const auto old_check_bodies = check_bodies;
    const auto old_check_throws = check_throws;
    const auto old_start_throw = t::fn::start_throw;
    t::fn::start_throw = check_start_throw;
    const auto old_part_states = part_states;
    const uint64_t simulating_function = fake::function(w.primitive_class, function_type, u"IsSimulatingPhysics", 9, 8,
        [](uint64_t object, uint64_t arguments) { fake::put<uint8_t>(arguments + 8, part_states[object].simulating ? 1 : 0); });
    fake::flag(fake::field(simulating_function, u"BoneName", u"NameProperty", 0, 8), t::constant::property_param);
    fake::flag(fake::bit_field(simulating_function, u"ReturnValue", 8, 0, 0xFF),
               t::constant::property_param | t::constant::property_out | t::constant::property_return);
    const uint64_t package_clip = fake::object(u"/G/Idle", w.component_class, 0x40), check_clip = fake::engine_object(u"Idle", clip_class);
    fake::put<uint64_t>(package_clip + t::object::outer, 0), fake::put(check_clip + t::object::outer, package_clip);
    std::u16string clip_path = u"/G/Idle.Idle";
    shelf.assets[fake::engine_name(clip_path.data(), clip_path.size(), 1)] = check_clip;
    const uint64_t spawn_source = tabled(table_a(), u"CheckSource", u"CHAPPY", 0, 0);
    check_spawn_source = spawn_source;
    registers(spawn_source);
    // The player's fake actor has the same reflected dispatch as an engine actor.
    TSUCHI_Actor captain{};
    CHECK(tsuchi::host.player->captain(mod, 0, &captain) == TSUCHI_OK);
    const uint64_t captain_object = tsuchi::game::from_handle(captain.id), old_captain_table = fake::get<uint64_t>(captain_object);
    fake::put(captain_object, fake::engine().vtable);
    // A player's captain has its AvatarComponent, cursor and living squad in a loaded world.
    const uint64_t old_captain_ai = fake::get<uint64_t>(captain_object + t::game_actor::ai);
    const uint64_t dog_object = w.hound;
    const uint64_t captain_seat = fake::get<uint64_t>(captain_object + t::game_actor::player);
    const uint64_t old_captain_grip = fake::get<uint64_t>(captain_object + t::game_actor::stick);
    const uint64_t old_ridden_dog = fake::get<uint64_t>(captain_seat + t::player_component::ridden_dog);
    const uint64_t old_dog_seat = fake::get<uint64_t>(dog_object + t::game_actor::player);
    const uint8_t old_dog_sort = fake::get<uint8_t>(dog_object + t::game_actor::sort);
    const auto old_request_ride = t::fn::request_ride, old_request_get_off = t::fn::request_get_off;
    const uint64_t prior_ride_captain = check_ride_captain, prior_ride_dog = check_ride_dog;
    const int prior_ride_steps = check_ride_steps, prior_off_steps = check_off_steps;
    check_ride_captain = captain_object, check_ride_dog = dog_object;
    fake::put(captain_object + t::game_actor::stick, fake::object(u"CheckRideGrip", w.component_class, 0x60));
    fake::put(dog_object + t::game_actor::player, fake::object(u"CheckDogSeat", w.component_class, 0x60));
    fake::put<uint8_t>(dog_object + t::game_actor::sort, uint8_t(old_dog_sort | t::constant::sort_dog));
    fake::put(captain_seat + t::player_component::ridden_dog, dog_object);
    t::fn::request_ride = check_request_ride, t::fn::request_get_off = check_request_get_off;
    const uint64_t old_leading_cursor = leading.cursor;
    leading.cursor = fake::memory(0x40);
    for (int i = 0; i < 3; ++i) fake::put<float>(leading.cursor + t::cursor::position + 4 * i, float(i + 1));
    fake::put(captain_object + t::game_actor::ai, fake::get<uint64_t>(w.chief + t::game_actor::ai));
    const uint64_t pikmin_head = w.framework + t::game_framework::pikmin_list;
    const uint64_t old_pikmin_next = fake::get<uint64_t>(pikmin_head + t::actor_node::next);
    const auto old_pikmin_leaders = leading.leaders;
    uint64_t check_pikmin[3]{};
    for (int i = 0; i < 3; ++i) {
        const uint64_t actor = tabled(table_a(), u"CheckPikmin", u"Pikmin", 0, float(i)), part = fake::object(u"CheckPikminPart", w.component_class, 0x40);
        fake::put(actor + t::game_actor::pikmin, part);
        fake::put(actor + t::object::class_, w.pikmin_class);
        fake::put<uint8_t>(actor + t::game_actor::color, uint8_t(i)), fake::put<uint8_t>(part + t::pikmin_component::leaf, uint8_t(i));
        fake::put<uint8_t>(part + t::pikmin_component::in_army, 1);
        leading.leaders[actor] = captain_object;
        registers(actor);
        check_pikmin[i] = actor;
    }
    ring_pikmin({check_pikmin[0], check_pikmin[1], check_pikmin[2]});
    const uint64_t check_creature = tabled(table_a(), u"CheckCreature", u"CHAPPY", 0, 0);
    fake::put(check_creature + t::game_actor::group, fake::get<tsuchi::game::Name>(fake::main_image() + t::data::creature_group));
    give_life(check_creature, 50, 50);
    registers(check_creature);
    // An enabled primitive with a record of its own, also read through the engine's reflected function.
    const uint64_t get_collision = fake::function(w.primitive_class, function_type, u"GetCollisionEnabled", 1, 1,
        [](uint64_t object, uint64_t arguments) { fake::put<uint8_t>(arguments, part_states[object].mode); });
    fake::flag(fake::field(get_collision, u"ReturnValue", u"ByteProperty", 0, 1),
               t::constant::property_param | t::constant::property_out | t::constant::property_return);
    const auto old_captain_parts = parts_of[captain_object];
    const uint64_t old_captain_tree = fake::get<uint64_t>(captain_object + t::game_actor::collision_tree);
    const std::string old_body_log = body_log;
    const uint64_t collision_part = fake::engine_object(u"CheckSolid", w.primitive_class, 0x80), collision_table = fake::memory(0x60);
    std::memcpy(reinterpret_cast<void*>(collision_table), reinterpret_cast<const void*>(primitive_table), 0x60);
    // Reflection and physics have distinct typed slots in the made-up game.
    fake::put(collision_table + t::object_vtable::process_event, &fake::engine_process);
    fake::put(collision_part, collision_table);
    fake::put(collision_part + t::scene_component::owner, captain_object);
    part_states[collision_part] = {false, kCollisionBoth, 0};
    parts_of[captain_object].push_back(collision_part);
    const uint64_t collision_tree = fake::object(u"CheckTree", w.component_class, 0x80), collision_record = fake::memory(t::collision_record::size);
    fake::put(collision_tree + t::scene_component::owner, captain_object), fake::put(collision_tree + t::collision_tree::actor, captain_object);
    fake::put(collision_tree + t::collision_tree::records, collision_record);
    fake::put<int32_t>(collision_tree + t::collision_tree::count, 1), fake::put<int32_t>(collision_tree + t::collision_tree::capacity, 1);
    fake::put(collision_record + t::collision_record::component, collision_part);
    for (uint64_t flag : t::tables::collision_record_flags) fake::put<uint8_t>(collision_record + flag, 1);
    fake::put(captain_object + t::game_actor::collision_tree, collision_tree);
    thunks::engine_ticks = true;
    const auto& tick_slot = t::slot::engine_tick;
    const uint64_t tick_entry = fake::main_image() + tick_slot.table + tick_slot.offset, old_tick_entry = fake::get<uint64_t>(tick_entry);
    fake::code().push_back(reinterpret_cast<uint64_t>(&check_engine_tick));
    fake::code().push_back(reinterpret_cast<uint64_t>(&check_push_journey));
    fake::put(tick_entry, &check_engine_tick);
    fake::patch_mark(), fake::patch_site(t::code_site::engine_tick_function);
    CHECK(tsuchi::find_patch() == tsuchi::Patch::right);
    TSUCHI_Mod* const engine_side = tsuchi::mod_create("the.game.engine-tick");
    uint64_t engine_hook = 0;
    void* engine_original = nullptr;
    CHECK(tsuchi::hook_code_site(engine_side, t::code_site::engine_tick_function, reinterpret_cast<void*>(&check_engine_tick), &engine_hook, &engine_original) == TSUCHI_OK);
    // Only the installed engine gate needs the host's floating-point ABI.
    // Later SDK hooks use integer/pointer arguments, as their native gates do.
    thunks::engine_ticks = false;
    for (auto function : thunks::all) fake::code().push_back(reinterpret_cast<uint64_t>(function));
    for (auto function : thunks::all_engine_ticks) fake::code().push_back(reinterpret_cast<uint64_t>(function));
    checking_engine_ticks = true, check_engine_stock_calls = 0;
    const uint64_t package = fake::object(u"/G/Fresh", w.component_class, 0x40), fresh = fake::object(u"Fresh_C", w.root);
    fake::put<uint64_t>(package + t::object::outer, 0), fake::put(fresh + t::object::outer, package);
    std::u16string fresh_path = u"/G/Fresh.Fresh_C";
    shelf.assets[fake::engine_name(fresh_path.data(), fresh_path.size(), 1)] = fresh;
    // One existing part is within an asset's package, as a part in a loaded level is.
    uint64_t nested = 0, old_outer = 0;
    TSUCHI_Actor in_play[4096]{};
    uint64_t actors_count = 0;
    CHECK(tsuchi::host.entity->list(mod, {}, in_play, 4096, &actors_count) == TSUCHI_OK);
    for (uint64_t i = 0; i < actors_count && !nested; ++i) {
        TSUCHI_Component listed[256]{};
        uint64_t count = 0;
        if (tsuchi::host.component->list(mod, in_play[i], listed, 256, &count) != TSUCHI_OK) continue;
        for (uint64_t at = 0; at < count; ++at) {
            const uint64_t part = tsuchi::game::from_handle(listed[at].id);
            if (tsuchi::game::is_scene_part(part)) continue;
            nested = part;
            break;
        }
    }
    CHECK(nested != 0);
    if (nested) old_outer = fake::get<uint64_t>(nested + t::object::outer), fake::put(nested + t::object::outer, w.idol_asset);
    std::u16string cleanup_actor_path = u"/G/Revive.Revive_C", cleanup_material_path = u"/G/Red.Red";
    shelf.assets[fake::engine_name(cleanup_actor_path.data(), cleanup_actor_path.size(), 1)] = check_revive_class;
    shelf.assets[fake::engine_name(cleanup_material_path.data(), cleanup_material_path.size(), 1)] = red_material;
    if (ai_only) ai_order_model::install();
    std::vector<const char*> selections;
    if (ai_only) selections.push_back("check.ai-order");
    else {
        selections.push_back(nullptr);
        // Engine-actor donor qualification runs in the actual game. Its backend
        // has a separate bounded host test, without fabricated world or mobility data.
        // The presenter's check measures the game's own frames and clock; its logic has the test of test_presenter.py.
        // Gimmick and battle proof needs a real scene. Its backends have a focused host test.
        // Ground contact's observer has a dedicated host fixture; flight and landing need the game.
        // The restart check waits for a tool outside the game to edit files on the SD card; restart_checks.h has
        // the restarts against this game.
        for (const char* id : check_mod_explicit_ids)
            if (id && std::strcmp(id, "check.ai-order") && std::strcmp(id, "check.engine-actors") && std::strcmp(id, "check.presenter") && std::strcmp(id, "check.ui") &&
                std::strcmp(id, "check.restart") &&
                std::strcmp(id, "check.ordinary-lifetime") && std::strcmp(id, "check.ordinary-lifetime-startup") &&
                std::strcmp(id, "check.ordinary-lifetime-startup-producer") && std::strcmp(id, "check.gimmicks-battle") &&
                std::strcmp(id, "check.ground")) selections.push_back(id);
    }
    std::string runs[3];
    std::vector<std::pair<const char*, std::string>> explicit_runs;
    uint32_t locked_switch_tick_calls = 0, delayed_crew_tick_calls = 0, tick_matrix_calls = 0, fixture_runs = 0;
    for (const char* selected : selections) {
    for (auto at = disk.files.begin(); at != disk.files.end();) {
        if (at->first.compare(0, mods_at.size(), mods_at) == 0) at = disk.files.erase(at);
        else ++at;
    }
    disk.folders.clear();
    for (const CheckModFile& file : check_mod_files)
        if (file.mod && selected_check_fixture(file.mod, selected)) disk.files[mods_at + file.mod + "/" + file.path] = std::string(file.bytes, file.size);
    if (ai_only) ai_order_model::observer_targets(disk.files[mods_at + "check.ai-order/targets.txt"]);
    uint32_t pages = 40;
    for (const CheckModStart& code : check_mod_starts) {
        if (!code.mod || !selected_check_fixture(code.mod, selected)) continue;
        made_loader.code.push_back(!std::strcmp(code.mod, "check.cleanup-producer") ? check_cleanup_start :
                                   !std::strcmp(code.mod, "check.hooks") ? check_typed_hooks_start :
                                   !std::strcmp(code.mod, "check.ai-order") ? check_ai_entry_start : code.start);
        disk.files[mods_at + code.mod + "/mod.nro"] = module_image(int32_t(made_loader.code.size() - 1), pages++, 0x1000);
    }
    std::string group_runs[3];
    for (std::string& run : group_runs) {
        // A fresh process observes the model's actual native callee independently of the
        // persistent framework wrapper, which now installs during early manifest preparation.
        if (selected && !std::strcmp(selected, "check.ai-order") && &run != &group_runs[0]) continue;
        ++fixture_runs;
        check_dismiss_delayed = selected && !std::strcmp(selected, "check.crew");
        check_dismiss_target = 0, check_dismiss_requests = 0, check_dismiss_steps = 0;
        check_dialog_answers = 0;
        check_journey_request = 0; check_journey_age = check_title_age = 0;
        logged.lines.clear();
        // The third run's save holds settings for the settings check, as whoever runs the checks in the game prepares one.
        if (!selected && &run == &group_runs[2])
            disk.files["save:/tsuchi/check.settings/settings.json"] = R"({"loud": false, "runs": 41, "speed": 0.75, "mode": "wild", "greeting": "hi there", "extras": ["boots"]})";
        in_stages({u"Title"});
        check_cleanup_spawn = selected && !std::strcmp(selected, "check.cleanup");
        in_world(check_cleanup_spawn ? u"Title_P" : nullptr);
        if (check_cleanup_spawn) {
            char targets[768];
            std::snprintf(targets, sizeof targets, "%llx\n%llx\n0\n%llx\n%llx\n%llx\n%llx\n/G/Revive.Revive_C\n/G/Red.Red\n",
                          static_cast<unsigned long long>(t::tables::actor_tables[0]), static_cast<unsigned long long>(t::actor_table::request_tick),
                          static_cast<unsigned long long>(t::game_actor::ai), static_cast<unsigned long long>(t::game_actor::movement),
                          static_cast<unsigned long long>(t::game_actor::tick_requested), static_cast<unsigned long long>(t::game_component::tick_requested));
            for (const char* id : {"check.cleanup", "check.cleanup-producer"}) disk.files[mods_at + id + "/targets.txt"] = targets;
        }
        CHECK(tsuchi::find_mods() == TSUCHI_OK);
        const auto suspended_before = tsuchi::suspend_counts().actors, hidden_before = tsuchi::hidden_counts().holds;
        const auto added_before = tsuchi::added_count(), materials_before = tsuchi::material_holds();
        check_cleanup_context = nullptr; check_cleanup_owned_before_refusal = 0;
        if (check_cleanup_spawn) thunks::engine_ticks = false; // pointer/bool native setter, rather than engine float/bool
        tsuchi::start_mods();
        thunks::engine_ticks = false;
        if (check_cleanup_spawn) {
            const auto* producer = tsuchi::find_mod("check.cleanup-producer");
            CHECK(producer && producer->state == tsuchi::ModState::blocked && !producer->context &&
                  producer->problem.get() && !std::strcmp(producer->problem.get(), "mod.nro: its start answered not_ready"));
            CHECK(check_cleanup_context && !tsuchi::mod_usable(check_cleanup_context) &&
                  tsuchi::owned_count(check_cleanup_context) == 0 && check_cleanup_owned_before_refusal >= 9);
            CHECK(logged.lines.find("cleanup prepared: AI movement physics collision subtree visibility distinct material two additions owned actor callback") != std::string::npos);
            CHECK(tsuchi::suspend_counts().actors == suspended_before && tsuchi::hidden_counts().holds == hidden_before &&
                  tsuchi::added_count() == added_before && tsuchi::material_holds() == materials_before);
        }
        tsuchi::log_mods();
        run_frames(5);
        if (check_cleanup_spawn) CHECK(logged.lines.find("cleanup forbidden callback") == std::string::npos);
        TSUCHI_Mode startup_mode{};
        bool startup_control = true;
        CHECK(tsuchi::host.game->mode(mod, &startup_mode) == TSUCHI_OK && startup_mode == TSUCHI_MODE_TITLE);
        CHECK(tsuchi::host.game->has_control(mod, &startup_control) == TSUCHI_OK && !startup_control);
        for (const char* id : {"check.story", "check.actors", "check.behavior", "check.combat", "check.game", "check.players", "check.spawn", "check.anim", "check.body", "check.motion", "check.fight", "check.aim", "check.crew", "check.switch", "check.death", "check.work", "check.throw", "check.ride", "check.revive"}) {
            if (!selected_check_fixture(id, selected)) continue;
            const Reported startup = reported_by(logged.lines, id);
            CHECK(!startup.planned.empty() && startup.passed.empty() && startup.failed.empty());
        }
        // A save enters its playable world after the title checks have already started.
        in_stages({u"Story", u"StoryInGame"});
        in_world(u"Area001_P");
        const uint64_t story_top = fake::get<uint64_t>(running.flow + t::flow_stage::child);
        const uint64_t story_play = fake::get<uint64_t>(story_top + t::flow_stage::child);
        fake::put(story_top, fake::main_image() + t::data::story_flow_table);
        fake::put(story_play, fake::main_image() + t::data::story_play_table);
        fake::put<int32_t>(story_play + t::flow_stage::status, 1);
        if (selected && !std::strcmp(selected, "check.ai-order")) ai_order_model::run_start();
        if (selected && !std::strcmp(selected, "check.switch")) {
            const auto before_switch = people.steered;
            fake::put<uint8_t>(story + t::story_save::switch_unlocked, 0);
            run_frames(3);
            locked_switch_tick_calls += 2 * 3;
            const Reported locked = reported_by(logged.lines, "check.switch");
            CHECK(locked.passed.count("found") && locked.passed.count("listened") && !locked.passed.count("asked") &&
                  locked.failed.empty() && people.steered == before_switch);
            fake::put<uint8_t>(story + t::story_save::switch_unlocked, 1);
            run_frames(145);
            CHECK(people.steered == before_switch);
        } else if (check_dismiss_delayed) {
            run_frames(95);
            const Reported pending = reported_by(logged.lines, "check.crew");
            CHECK(pending.passed.count("dismissed") && !pending.passed.count("restored") && pending.failed.empty());
            CHECK(check_dismiss_requests == 1 && check_dismiss_target == captain_object && !check_dismiss_steps);
            uint32_t following = 0;
            for (const auto& member : leading.leaders) if (member.second == captain_object && tsuchi::game::is_pikmin(member.first)) ++following;
            CHECK(following == 6); // all three originals and three additions still follow
            run_frames(105);
            delayed_crew_tick_calls += 2 * (200 - 145);
            CHECK(check_dismiss_requests == 2 && !check_dismiss_target && !check_dismiss_steps);
            CHECK(logged.lines.find("crew dismiss age=45 members=6") != std::string::npos &&
                  logged.lines.find("crew dismiss age=150 members=6") != std::string::npos &&
                  logged.lines.find("crew dismiss repeated age=150 result=ok") != std::string::npos);
        } else if (selected && !std::strcmp(selected, "check.tick-state")) {
            run_frames(600);
            tick_matrix_calls += 2 * (600 - 145);
        } else run_frames(145);
        tsuchi::forget_mods();
        if (selected && !std::strcmp(selected, "check.ai-order")) ai_order_model::finish_run();
        run_frames(61); // every line is in the file
        CHECK(check_dialog_answers == (selected && !std::strcmp(selected, "check.dialogs") ? 3 : 0) && talk.posted.empty() && tsuchi::dialogs_waiting() == 0);
        if (check_cleanup_spawn) CHECK(logged.lines.find("cleanup forbidden callback") == std::string::npos);
        run = logged.lines;
        for (const char* id : check_mod_ids) {
            if (!id || !selected_check_fixture(id, selected)) continue;
            const Reported found = reported_by(run, id);
            const bool right = !found.planned.empty() && found.passed == found.planned && found.failed.empty();
            CHECK(right);
            if (!right) std::printf("  %s in run %d\n%s%s", id, int(&run - group_runs) + 1, found.lines.c_str(), reported_by(run, "tsuchi").lines.c_str());
        }
    }
    if (selected) for (const std::string& run : group_runs) {
        if (!run.empty()) explicit_runs.push_back({selected, run});
    }
    else for (int i = 0; i < 3; ++i) runs[i] = group_runs[i];
    }
    if (ai_only) ai_order_model::finish();
    if (!ai_only) for (const std::string& run : runs) CHECK(run.find("[check.mods] the public mods log check\n") != std::string::npos);
    // What the second run found of the first: a mod's saved data and its settings are there the next time.
    if (!ai_only) for (const char* line : {"[check.storage] check left pass fresh\n", "[check.settings] check found pass fresh\n"}) CHECK(runs[0].find(line) != std::string::npos);
    if (!ai_only) for (const char* line : {"[check.storage] check left pass kept\n", "[check.settings] check found pass kept\n"}) CHECK(runs[1].find(line) != std::string::npos);
    if (!ai_only) for (const char* line : {"[check.storage] check left pass kept\n", "[check.settings] check found pass prepared\n"}) CHECK(runs[2].find(line) != std::string::npos);
    // The log's file holds every line of them all, as the sink was given them, at its end.
    const std::string& file = disk.files[kLogAt];
    std::string both = runs[0] + runs[1] + runs[2];
    for (const auto& run : explicit_runs) both += run.second;
    CHECK(file.size() >= both.size() && !file.compare(file.size() - both.size(), both.size(), both) && closed() && made_loader.well_formed);
    // For whoever runs this: each run's log, between two marks, to be read as a log from the game is.
    if (!ai_only) for (const std::string& run : runs) std::printf("%s%s%s", kCheckLogBegins, run.c_str(), kCheckLogEnds);
    for (const auto& run : explicit_runs) std::printf("==== the log of an explicit SDK run %s\n%s%s", run.first, run.second.c_str(), kCheckLogEnds);
    CHECK(disk.files["save:/B/single.sav"] == "the check reads this existing save without changing it");
    fake::put<int32_t>(config + t::config_save::slot, old_save_slot);
    announced.log = old_announcement_log, announced.sounds = old_sounds;
    CHECK(std::memcmp(old_pause_counts, running.counts, sizeof old_pause_counts) == 0 &&
          fake::get<uint32_t>(running.pauses + t::pause_manager::active) == old_pause_active);
    running.log = old_game_log;
    fake::put(running.instance + t::game_instance::story_save, old_story);
    fake::put(w.framework + t::game_framework::clock, old_clock);
    fake::destroy(portal_actor); fake::destroy(portal_actor_class); fake::destroy(portal_part_class);
    hoard = old_hoard;
    t::fn::find_treasure_row = old_find_treasure; t::fn::find_story_record = old_find_record;
    t::fn::collect_treasure = old_collect_treasure; t::fn::add_sparklium = old_add_sparklium;
    journey = old_journey;
    t::fn::return_to_title = old_title_function;
    t::fn::travel_request = old_travel_function;
    fake::put(running.context + t::game_context::requests, old_requests);
    fake::put(fake::main_image() + t::data::portal_part_class, old_portal_class);
    fake::put(table_a() + t::actor_table::find_component, old_find_component);
    t::fn::npad_style_set = old_npad_style, t::fn::npad_full_key_state = old_npad_full_key;
    fake::put<uint64_t>(anchor_at, old_anchor);
    // The keyboard was started once, and read when the input check asked, once in a step.
    CHECK(check_keyboard_starts == (ai_only ? 0u : 1u) && check_keyboard_reads >= (ai_only ? 0u : 3u));
    if (nested) fake::put(nested + t::object::outer, old_outer);
    CHECK(check_engine_stock_calls == 2 * (150 + 61) * fixture_runs + locked_switch_tick_calls + delayed_crew_tick_calls + tick_matrix_calls);
    CHECK(spawner.queued.empty());
    ends_play(spawn_source), fake::destroy(spawn_source);
    switches.log = old_switch_log;
    CHECK(check_revive_requests.empty() && check_defeats.empty() && check_carcasses.empty() && check_revivals.empty());
    t::fn::revive_request = old_revive_request;
    revived = old_revived, check_revive_class = old_revive_class;
    check_revive_requests = old_revive_requests, check_defeats = old_defeats, check_carcasses = old_carcasses, check_revivals = old_revivals;
    CHECK(check_charge_messages.empty());
    t::fn::charge_order = old_charge_order;
    ordered = old_ordered, check_charge_messages = old_charge_messages, check_worker_brains = old_worker_brains;
    t::fn::kill_actor = old_kill_actor;
    t::fn::eject_pikmin = old_eject_pikmin, t::fn::dismiss_squad = old_dismiss_squad;
    check_dismiss_delayed = old_dismiss_delayed, check_dismiss_target = old_dismiss_target;
    check_dismiss_requests = old_dismiss_requests, check_dismiss_steps = old_dismiss_steps;
    herd = old_herd;
    t::fn::post_message = old_post_message;
    posted = old_posted, life_log = old_life_log, heard_drops = old_heard_drops;
    t::fn::spawn_now = old_spawn_now;
    t::fn::warp_actor = old_warp_actor;
    part_log = old_part_log;
    talk = old_talk;
    checking_engine_ticks = check_cleanup_spawn = false;
    tsuchi::mod_stop(engine_side);
    fake::put(tick_entry, old_tick_entry);
    thunks::engine_ticks = false;
    CHECK(part_states[collision_part].mode == kCollisionBoth && tsuchi::parts_switched_off() == 0);
    for (uint64_t flag : t::tables::collision_record_flags) CHECK(fake::get<uint8_t>(collision_record + flag) == 1);
    fake::put(captain_object + t::game_actor::collision_tree, old_captain_tree);
    parts_of[captain_object] = old_captain_parts;
    body_log = old_body_log;
    part_states.erase(collision_part);
    CHECK(check_throws.empty());
    check_bodies = old_check_bodies, part_states = old_part_states;
    check_throws = old_check_throws, t::fn::start_throw = old_start_throw;
    fake::destroy(collision_part), fake::destroy(collision_tree);
    fake::put(captain_object + t::game_actor::ai, old_captain_ai);
    leading.cursor = old_leading_cursor;
    CHECK(check_ride_steps == 0 && check_off_steps == 0);
    fake::put(captain_object + t::game_actor::stick, old_captain_grip);
    fake::put(captain_seat + t::player_component::ridden_dog, old_ridden_dog);
    fake::put(dog_object + t::game_actor::player, old_dog_seat);
    fake::put(dog_object + t::game_actor::sort, old_dog_sort);
    t::fn::request_ride = old_request_ride, t::fn::request_get_off = old_request_get_off;
    check_ride_captain = prior_ride_captain, check_ride_dog = prior_ride_dog;
    check_ride_steps = prior_ride_steps, check_off_steps = prior_off_steps;
    fake::put(pikmin_head + t::actor_node::next, old_pikmin_next);
    leading.leaders = old_pikmin_leaders;
    for (uint64_t actor : check_pikmin) ends_play(actor), fake::destroy(actor);
    ends_play(check_creature), fake::destroy(check_creature);
    fake::put(captain_object, old_captain_table);
    tsuchi::set_log_sink(nullptr);
    logged.lines.clear();
    disk.files = other_files;
    disk.folders.clear();
    disk.changes.clear(), disk.log.clear();
}

// ---- A log that is full ---------------------------------------------------------------------------------------

// A run writes a bounded amount to the log's file. Past it a line is counted and dropped, and the file's last line
// says so. This comes last: the made-up game's one run has no room in its log afterward.
void filling_the_log() {
    const auto other_files = disk.files;
    disk.files.clear(), disk.folders.clear();
    const std::string full = "[tsuchi] the log is full: what is written after this is not in the file\n";
    const tsuchi::LogFileCounts before = tsuchi::log_file_counts();
    for (int i = 0; i < 400; ++i) tsuchi::log_file_line("loud.mod", std::string(990, 'z').c_str());
    run_frames(61);
    const std::string file = disk.files[kLogAt];
    const tsuchi::LogFileCounts after = tsuchi::log_file_counts();
    CHECK(after.written_bytes <= 256 * 1024 && after.written_bytes > 255 * 1024 && after.lines == before.lines + 400 && after.dropped > before.dropped && after.dropped < before.dropped + 400);
    CHECK(file.size() > full.size() && !file.compare(file.size() - full.size(), full.size(), full) && file.find(full) == file.size() - full.size() && closed());
    // Nothing more is written, however long the game runs.
    tsuchi::log_file_line("loud.mod", "one more");
    run_frames(400);
    CHECK(disk.files[kLogAt] == file && tsuchi::log_file_counts().dropped == after.dropped + 1 && tsuchi::log_file_counts().writes == after.writes && closed());
    disk.files = other_files;
    disk.folders.clear();
    disk.changes.clear(), disk.log.clear();
}

void checking_in_the_game(bool ai_only = false) {
    logging();
    telling_of_mods();
    helping();
    checking_fixtures(ai_only);
    filling_the_log();
}
