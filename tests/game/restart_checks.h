// Restarting a mod, and its development folder, against the made-up game: see DESIGN.md, "Restarting a mod".
// Part of objects_test.cpp, which includes this inside its own namespace, after everything it uses.
namespace {

struct DevCard {
    bool readable = true;
    int begun = 0, ended = 0;
} dev_card;
bool dev_card_begin() {
    if (!dev_card.readable) return false;
    ++dev_card.begun;
    return true;
}
void dev_card_end() { ++dev_card.ended; }

std::string restarts_heard;
void heard_restart(void*, const TSUCHI_ModRestarted* event) {
    const char* const outcomes[] = {"restarted", "kept", "blocked"};
    restarts_heard += std::string(event->mod) + " " + (event->outcome < 3 ? outcomes[event->outcome] : "?") + (event->problem[0] ? " " : "") + event->problem + "\n";
}

void restarting() {
    const TSUCHI_ModService& about = *tsuchi::host.mod;
    const TSUCHI_FileService& folder = *tsuchi::host.file;
    const std::string mods_at = std::string(tsuchi::kModsFolder) + "/", dev_at = "sd:/tsuchi/dev/";
    tsuchi::forget_mods();
    const auto other_files = disk.files;
    const auto other_folders = disk.folders;
    disk.files.clear(), disk.folders.clear();
    tsuchi::set_dev_folder("sd:/tsuchi/dev", dev_card_begin, dev_card_end);
    tsuchi::set_restart_gate(nullptr);
    tsuchi::set_dev_watch(0); // the watcher has checks of its own below
    tsuchi::set_log_sink(log_sink);
    const auto manifest = [&](const std::string& id, const std::string& more = "") {
        disk.files[mods_at + id + "/mod.json"] = "{\"id\": \"" + id + "\", \"name\": \"Restart " + id + "\", \"version\": \"1.0.0\"" + more + "}";
    };
    const auto script = [&](const std::string& id, const std::string& text) { disk.files[mods_at + id + "/main.lua"] = text; };
    const auto context = [](const char* id) { return tsuchi::find_mod(id)->context; };
    const auto state = [&](const char* id) {
        TSUCHI_ModState got = 99;
        return about.state(mod, id, &got) == TSUCHI_OK ? got : 99u;
    };
    const auto problem = [&](const char* id) {
        char buffer[512] = "<failed>";
        uint64_t needed = 0;
        about.problem(mod, id, buffer, sizeof buffer, &needed);
        return std::string(buffer);
    };
    const auto heard = [](const std::string& expected) {
        const bool same = restarts_heard == expected;
        if (!same) std::printf("  heard: [%s]\n", restarts_heard.c_str());
        restarts_heard.clear();
        return same;
    };
    const auto has = [](const std::string& part) { return logged.lines.find(part) != std::string::npos; };
    const auto before = [](const std::string& first, const std::string& second) {
        const size_t a = logged.lines.find(first), b = logged.lines.find(second);
        return a != std::string::npos && b != std::string::npos && a < b;
    };
    TSUCHI_Subscription listening{};
    CHECK(about.on_restarted(mod, heard_restart, nullptr, &listening) == TSUCHI_OK);
    restarts_heard.clear();

    // A library, a mod that requires it, one that is left alone, a JavaScript mod, one that restarts itself, one
    // that fails as it starts, and one blocked before its turn.
    manifest("re.lib");
    script("re.lib", "print('lib A', kept)\nkept = 'from A'\ntime.on_step(function() print('lib A step') end)\n");
    manifest("re.app", R"(, "requires": {"re.lib": "1.0.0"})");
    script("re.app", "print('app started', mod.version_of('re.lib'))\n");
    manifest("re.other");
    script("re.other", "print('other started')\n");
    manifest("re.js");
    disk.files[mods_at + "re.js/main.js"] = "print('js A');";
    manifest("re.self");
    script("re.self", "print('self started')\ntime.on_step(function()\n"
                      "  if file.exists('again.txt') then print('self asks', mod.restart(mod.id())) end\nend)\n");
    manifest("re.bad");
    script("re.bad", "error('bad from the start')\n");
    manifest("re.off", R"(, "requires": {"re.absent": "*"})");
    disk.files[mods_at + "re.lib/data.txt"] = "rom data";

    // Found, and not started yet: a restart waits for the mods to start.
    CHECK(tsuchi::find_mods() == TSUCHI_OK);
    CHECK(state("re.lib") == TSUCHI_MOD_STATE_WAITING && about.restart(mod, "re.lib") == TSUCHI_NOT_READY && problem("re.lib").empty());
    logged.lines.clear();
    tsuchi::start_mods();
    CHECK(before("[re.lib] lib A\tnil", "[re.app] app started\t1.0.0") && has("[re.other] other started") && has("[re.js] js A") && has("[re.self] self started"));
    CHECK(state("re.lib") == TSUCHI_MOD_STATE_RUNNING && state("re.bad") == TSUCHI_MOD_STATE_BLOCKED && state("re.off") == TSUCHI_MOD_STATE_BLOCKED && state("re.nobody") == 99u);
    CHECK(problem("re.bad").find("bad from the start") != std::string::npos && problem("re.lib").empty() && problem("re.off") == "it needs re.absent, which is not installed");
    TSUCHI_ModState unused{};
    char small[4];
    uint64_t needed = 0;
    CHECK(about.state(mod, "re.nobody", &unused) == TSUCHI_NOT_FOUND && about.problem(mod, "re.nobody", small, sizeof small, &needed) == TSUCHI_NOT_FOUND);
    CHECK(about.problem(mod, "re.off", small, sizeof small, &needed) == TSUCHI_CAPACITY && needed == strlen("it needs re.absent, which is not installed") + 1);
    // Who can be restarted: any mod that had its turn, and no other.
    CHECK(about.restart(mod, "re.nobody") == TSUCHI_NOT_FOUND && about.restart(mod, "re.off") == TSUCHI_UNSUPPORTED && about.restart(mod, "") == TSUCHI_NOT_FOUND);

    // The library is edited and restarted: the request waits for the end of the step, the mod that requires it
    // stops before it and starts after it, both keep their ranks, and nothing of the old script is left.
    TSUCHI_Mod* const lib_a = context("re.lib"), *app_a = context("re.app"), *other = context("re.other");
    const uint32_t lib_rank = tsuchi::mod_rank(lib_a), app_rank = tsuchi::mod_rank(app_a);
    script("re.lib", "print('lib B', kept)\ntime.on_step(function() print('lib B step') end)\n");
    logged.lines.clear();
    CHECK(about.restart(mod, "re.lib") == TSUCHI_OK && about.restart(mod, "re.lib") == TSUCHI_OK); // twice in one step: one restart
    CHECK(context("re.lib") == lib_a && tsuchi::mod_usable(lib_a) && logged.lines.empty() && restarts_heard.empty());
    tsuchi::frame(0.03f);
    CHECK(!tsuchi::mod_usable(lib_a) && !tsuchi::mod_usable(app_a) && tsuchi::mod_usable(context("re.lib")) && tsuchi::mod_usable(context("re.app")));
    CHECK(tsuchi::mod_rank(context("re.lib")) == lib_rank && tsuchi::mod_rank(context("re.app")) == app_rank && context("re.other") == other);
    CHECK(before("[re.lib] lib A step", "[re.lib] restarted") && before("[re.lib] lib B\tnil", "[re.app] app started") && !has("[re.other]"));
    CHECK(heard("re.lib restarted\nre.app restarted\n"));
    logged.lines.clear();
    tsuchi::frame(0.03f);
    CHECK(has("[re.lib] lib B step") && !has("lib A step"));

    // A mod and the library it requires both ask in one step: each starts once, the library first.
    const auto times = [](const std::string& part) {
        int count = 0;
        for (size_t at = logged.lines.find(part); at != std::string::npos; at = logged.lines.find(part, at + 1)) ++count;
        return count;
    };
    logged.lines.clear();
    CHECK(about.restart(mod, "re.app") == TSUCHI_OK && about.restart(mod, "re.lib") == TSUCHI_OK);
    tsuchi::frame(0.03f);
    CHECK(times("[re.app] app started") == 1 && times("[re.lib] lib B\tnil") == 1 && before("[re.lib] lib B\tnil", "[re.app] app started"));
    CHECK(heard("re.lib restarted\nre.app restarted\n"));

    // A mod restarts itself from its own handler: the restart comes after its handler has returned.
    disk.files[mods_at + "re.self/again.txt"] = "once";
    logged.lines.clear();
    TSUCHI_Mod* const self_a = context("re.self");
    tsuchi::frame(0.03f);
    disk.files.erase(mods_at + "re.self/again.txt");
    CHECK(before("[re.self] self asks\ttrue", "[re.self] self started") && before("[re.self] self started", "[re.self] restarted") && context("re.self") != self_a);
    CHECK(heard("re.self restarted\n"));
    for (int i = 0; i < 3; ++i) tsuchi::frame(0.03f);
    CHECK(heard(""));

    // A script that does not compile: nothing stops, and the old code goes on.
    TSUCHI_Mod* const lib_b = context("re.lib");
    TSUCHI_Mod* const app_b = context("re.app");
    script("re.lib", "print('lib C'\n");
    logged.lines.clear();
    CHECK(about.restart(mod, "re.lib") == TSUCHI_OK);
    tsuchi::frame(0.03f);
    CHECK(context("re.lib") == lib_b && context("re.app") == app_b && tsuchi::mod_usable(lib_b) && state("re.lib") == TSUCHI_MOD_STATE_RUNNING);
    CHECK(has("[re.lib] not restarted, and it runs as before: re.lib/main.lua:2: ')' expected") && has("[re.lib] lib B step") && !has("lib C"));
    CHECK(restarts_heard.find("re.lib kept re.lib/main.lua:2:") == 0 && restarts_heard.find("re.app kept re.lib/main.lua:2:") != std::string::npos);
    restarts_heard.clear();

    // A script that fails as it runs: the mod is blocked, and so is the mod that requires it.
    script("re.lib", "error('C stops as it starts')\n");
    logged.lines.clear();
    CHECK(about.restart(mod, "re.lib") == TSUCHI_OK);
    tsuchi::frame(0.03f);
    CHECK(!context("re.lib") && !context("re.app") && !tsuchi::mod_usable(lib_b) && state("re.lib") == TSUCHI_MOD_STATE_BLOCKED && state("re.app") == TSUCHI_MOD_STATE_BLOCKED);
    CHECK(problem("re.lib") == "main.lua: re.lib/main.lua:1: C stops as it starts" && problem("re.app") == "it needs re.lib, which cannot run");
    CHECK(heard("re.lib blocked main.lua: re.lib/main.lua:1: C stops as it starts\nre.app blocked it needs re.lib, which cannot run\n"));
    // Fixed, and asked again: a blocked mod that had its turn starts, with what requires it.
    script("re.lib", "print('lib D')\n");
    CHECK(about.restart(mod, "re.lib") == TSUCHI_OK);
    logged.lines.clear();
    tsuchi::frame(0.03f);
    CHECK(state("re.lib") == TSUCHI_MOD_STATE_RUNNING && state("re.app") == TSUCHI_MOD_STATE_RUNNING && problem("re.lib").empty() && before("[re.lib] lib D", "[re.app] app started"));
    CHECK(tsuchi::mod_rank(context("re.lib")) == lib_rank && heard("re.lib restarted\nre.app restarted\n"));
    // A mod blocked as the game started can be restarted too, once it is fixed.
    script("re.bad", "print('bad fixed')\n");
    CHECK(about.restart(mod, "re.bad") == TSUCHI_OK);
    tsuchi::frame(0.03f);
    CHECK(state("re.bad") == TSUCHI_MOD_STATE_RUNNING && has("[re.bad] bad fixed") && heard("re.bad restarted\n"));

    // The development folder: made while the game runs, it is used from the next start on, file by file.
    disk.files[dev_at + "re.lib/main.lua"] = "print('lib from the card', file.read('data.txt', 0, 100), file.read('extra.txt', 0, 100))\n";
    disk.files[dev_at + "re.lib/extra.txt"] = "card only";
    disk.files[dev_at + "re.lib/more/deep.txt"] = "deep";
    uint64_t size = 0;
    CHECK(folder.size(context("re.lib"), "extra.txt", &size) == TSUCHI_NOT_FOUND); // not before the mod starts again
    logged.lines.clear();
    dev_card = DevCard{};
    CHECK(about.restart(mod, "re.lib") == TSUCHI_OK);
    tsuchi::frame(0.03f);
    CHECK(has("[re.lib] its files are read from its development folder on the SD card first") && has("[re.lib] lib from the card\trom data\tcard only"));
    CHECK(folder.size(context("re.lib"), "main.lua", &size) == TSUCHI_OK && size == disk.files[dev_at + "re.lib/main.lua"].size());
    std::vector<std::string> names;
    char listed[512];
    CHECK(folder.list(context("re.lib"), "", listed, sizeof listed, &needed) == TSUCHI_OK);
    for (const char* at = listed; *at; at += strlen(at) + 1) names.push_back(at);
    std::sort(names.begin(), names.end());
    CHECK(names == (std::vector<std::string>{"data.txt", "extra.txt", "main.lua", "mod.json", "more/"}));
    CHECK(folder.list(context("re.lib"), "more", listed, sizeof listed, &needed) == TSUCHI_OK && std::string(listed) == "deep.txt");
    CHECK(folder.list(context("re.app"), "", listed, sizeof listed, &needed) == TSUCHI_OK && !has("[re.app] its files are read"));
    CHECK(dev_card.begun > 0 && dev_card.begun == dev_card.ended && disk.open_files == 0);
    // A card that cannot be read leaves the mod's own folder.
    dev_card.readable = false;
    logged.lines.clear();
    CHECK(about.restart(mod, "re.lib") == TSUCHI_OK);
    tsuchi::frame(0.03f);
    CHECK(has("[re.lib] lib D") && !has("lib from the card"));
    dev_card.readable = true;
    // A syntax error in the development folder's script is found there, and keeps the running script.
    disk.files[dev_at + "re.lib/main.lua"] = "local = 1\n";
    logged.lines.clear();
    CHECK(about.restart(mod, "re.lib") == TSUCHI_OK);
    tsuchi::frame(0.03f);
    CHECK(has("[re.lib] not restarted, and it runs as before: re.lib/main.lua:1:") && state("re.lib") == TSUCHI_MOD_STATE_RUNNING);
    restarts_heard.clear();
    // Taken away again: the mod's own files.
    for (const char* path : {"main.lua", "extra.txt", "more/deep.txt"}) disk.files.erase(dev_at + "re.lib/" + path);
    logged.lines.clear();
    CHECK(about.restart(mod, "re.lib") == TSUCHI_OK);
    tsuchi::frame(0.03f);
    CHECK(has("[re.lib] lib D") && !has("development folder") && folder.size(context("re.lib"), "extra.txt", &size) == TSUCHI_NOT_FOUND);
    restarts_heard.clear();

    // While the game is busy, restarts wait.
    static bool busy = true;
    tsuchi::set_restart_gate([] { return !busy; });
    TSUCHI_Mod* const other_a = context("re.other");
    CHECK(about.restart(mod, "re.other") == TSUCHI_OK);
    for (int i = 0; i < 3; ++i) tsuchi::frame(0.03f);
    CHECK(context("re.other") == other_a && restarts_heard.empty());
    busy = false;
    tsuchi::frame(0.03f);
    CHECK(context("re.other") != other_a && heard("re.other restarted\n"));
    tsuchi::set_restart_gate(nullptr);

    // Watching the development folders: a save there restarts the mod with no request.
    tsuchi::set_dev_watch(2);
    TSUCHI_Mod* const watched = context("re.other");
    for (int i = 0; i < 4; ++i) tsuchi::frame(0.03f);
    CHECK(context("re.other") == watched && heard(""));
    disk.files[dev_at + "re.other/main.lua"] = "print('other from a save')\n";
    logged.lines.clear();
    for (int i = 0; i < 3; ++i) tsuchi::frame(0.03f);
    CHECK(has("[re.other] its development folder changed, so it restarts") && has("[re.other] other from a save") &&
          context("re.other") != watched && heard("re.other restarted\n"));
    // A save that does not compile keeps the running code, and is tried once.
    disk.files[dev_at + "re.other/main.lua"] = "print('other broken'\n";
    for (int i = 0; i < 6; ++i) tsuchi::frame(0.03f);
    CHECK(restarts_heard.find("re.other kept") == 0 && restarts_heard.find("re.other kept", 1) == std::string::npos &&
          state("re.other") == TSUCHI_MOD_STATE_RUNNING);
    restarts_heard.clear();
    // The same bytes saved again change nothing; fixed, it restarts.
    disk.files[dev_at + "re.other/main.lua"] = "print('other broken'\n";
    for (int i = 0; i < 4; ++i) tsuchi::frame(0.03f);
    CHECK(heard(""));
    disk.files[dev_at + "re.other/main.lua"] = "print('other fixed')\n";
    logged.lines.clear();
    for (int i = 0; i < 3; ++i) tsuchi::frame(0.03f);
    CHECK(has("[re.other] other fixed") && heard("re.other restarted\n"));
    // A folder of many files: a look lists it and reads the bytes of a few files at most, so a change of size is
    // seen at the next look and one that keeps it within a pass over the folder.
    for (int f = 0; f < 20; ++f) disk.files[dev_at + "re.other/data/f" + std::to_string(f) + ".txt"] = "data " + std::to_string(100 + f);
    for (int i = 0; i < 3; ++i) tsuchi::frame(0.03f);
    CHECK(heard("re.other restarted\n"));
    restarts_heard.clear();
    for (int i = 0; i < 20; ++i) tsuchi::frame(0.03f); // passes over the folder that find nothing changed
    CHECK(heard(""));
    size_t opened_before = disk.opened.size();
    tsuchi::frame(0.03f), tsuchi::frame(0.03f); // one look
    CHECK(disk.opened.size() - opened_before >= 1 && disk.opened.size() - opened_before <= 8);
    disk.files[dev_at + "re.other/data/f19.txt"] = "data 999"; // the same size
    for (int i = 0; i < 2; ++i) tsuchi::frame(0.03f);
    int looks = 1;
    for (; looks < 8 && restarts_heard.empty(); ++looks) tsuchi::frame(0.03f), tsuchi::frame(0.03f);
    CHECK(heard("re.other restarted\n") && looks <= 5);
    disk.files[dev_at + "re.other/data/f3.txt"] = "data 1030"; // another size: the next look
    tsuchi::frame(0.03f), tsuchi::frame(0.03f);
    CHECK(heard("re.other restarted\n"));
    for (int f = 0; f < 20; ++f) disk.files.erase(dev_at + "re.other/data/f" + std::to_string(f) + ".txt");
    for (int i = 0; i < 3; ++i) tsuchi::frame(0.03f);
    restarts_heard.clear();
    // Taken away: the mod's own files again.
    disk.files.erase(dev_at + "re.other/main.lua");
    logged.lines.clear();
    for (int i = 0; i < 3; ++i) tsuchi::frame(0.03f);
    CHECK(has("[re.other] its development folder was taken away, so it restarts") && has("[re.other] other started") &&
          heard("re.other restarted\n"));
    tsuchi::set_dev_watch(0);

    // JavaScript: the same, with its own compiler.
    const uint32_t runtimes = tsuchi_js::runtime_count();
    disk.files[mods_at + "re.js/main.js"] = "print('js B'";
    logged.lines.clear();
    CHECK(about.restart(mod, "re.js") == TSUCHI_OK);
    tsuchi::frame(0.03f);
    CHECK(has("[re.js] not restarted, and it runs as before: SyntaxError") && has("main.js:1") && tsuchi_js::runtime_count() == runtimes);
    restarts_heard.clear();
    disk.files[mods_at + "re.js/main.js"] = "import {} from './missing.js'; print('js C');";
    logged.lines.clear();
    CHECK(about.restart(mod, "re.js") == TSUCHI_OK);
    tsuchi::frame(0.03f); // its missing import is found by the compiler too
    CHECK(state("re.js") == TSUCHI_MOD_STATE_RUNNING && restarts_heard.find("re.js kept not_found") == 0 && tsuchi_js::runtime_count() == runtimes);
    restarts_heard.clear();
    disk.files[mods_at + "re.js/main.js"] = "print('js C'); throw new Error('C stops as it starts');";
    logged.lines.clear();
    CHECK(about.restart(mod, "re.js") == TSUCHI_OK);
    tsuchi::frame(0.03f);
    CHECK(state("re.js") == TSUCHI_MOD_STATE_BLOCKED && has("[re.js] js C") && restarts_heard.find("re.js blocked main.js: Error: C stops as it starts") == 0 &&
          tsuchi_js::runtime_count() == runtimes - 1);
    restarts_heard.clear();
    disk.files[mods_at + "re.js/main.js"] = "print('js D');";
    logged.lines.clear();
    CHECK(about.restart(mod, "re.js") == TSUCHI_OK);
    tsuchi::frame(0.03f);
    CHECK(has("[re.js] js D") && state("re.js") == TSUCHI_MOD_STATE_RUNNING && heard("re.js restarted\n") && tsuchi_js::runtime_count() == runtimes);
    // A JavaScript mod's development folder is watched too.
    tsuchi::set_dev_watch(2);
    disk.files[dev_at + "re.js/main.js"] = "print('js from a save');";
    logged.lines.clear();
    for (int i = 0; i < 3; ++i) tsuchi::frame(0.03f);
    CHECK(has("[re.js] js from a save") && heard("re.js restarted\n") && tsuchi_js::runtime_count() == runtimes);
    disk.files.erase(dev_at + "re.js/main.js");
    logged.lines.clear();
    for (int i = 0; i < 3; ++i) tsuchi::frame(0.03f);
    CHECK(has("[re.js] js D") && heard("re.js restarted\n"));
    tsuchi::set_dev_watch(0);

    // From a script: the calls, the event as a table, and refusals.
    manifest("re.tool");
    script("re.tool", "mod.on_restarted(function(e) print('tool heard', e.mod, e.outcome, e.problem) end)\n"
                      "print('tool', mod.state('re.lib'), mod.state('re.off'), mod.problem('re.off'), mod.restart('re.nobody'))\n"
                      "print('tool', mod.restart('re.off'))\nprint('tool', mod.restart('re.other'))\n");
    tsuchi::forget_mods();
    logged.lines.clear();
    CHECK(tsuchi::find_mods() == TSUCHI_OK);
    tsuchi::start_mods();
    CHECK(has("[re.tool] tool\trunning\tblocked\tit needs re.absent, which is not installed\tnil\tnot_found\n") && has("[re.tool] tool\tnil\tunsupported\n") &&
          has("[re.tool] tool\ttrue\n"));
    tsuchi::frame(0.03f);
    CHECK(has("[re.tool] tool heard\tre.other\trestarted\t\n"));
    restarts_heard.clear();

    tsuchi::forget_mods();
    CHECK(tsuchi::host.cancel(mod, listening) == TSUCHI_OK);
    tsuchi::set_log_sink(log_sink); // as installing() left it for the checks after these
    logged.lines.clear();
    tsuchi::set_dev_folder(nullptr, nullptr, nullptr); // the checks after these have no development folders
    disk.files = other_files, disk.folders = other_folders;
}

} // namespace
