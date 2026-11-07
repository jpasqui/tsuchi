// Why a mod is blocked when a part of its manifest is refused as the mod starts, against made-up mod files.
#include "files.h"
#include "file_service.h"
#include "mod_service.h"
#include "mods.h"
#include "mod_choices.h"
#include "tsuchi/core.h"
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <map>
#include <set>
#include <string>
#include <vector>
using namespace tsuchi;
namespace {
int failed = 0;
#define CHECK(x) do { if (!(x)) { std::printf("line %d: %s\n", __LINE__, #x); ++failed; } } while (0)

std::map<std::string, std::string> files;
std::vector<std::string> events;
std::map<std::string, TSUCHI_Mod*> expected_contexts;
std::map<std::string, int> released;
uint32_t folder_reads = 0, manifest_reads = 0, choices_attempts = 0, before_calls = 0, code_calls = 0;
TSUCHI_Result choices_result = TSUCHI_OK;
const char* activation = "", *order = "";
const Kind resource{"test.manifest-resource"};

const std::string* find_file(const char* path) {
    const auto found = files.find(path);
    return found == files.end() ? nullptr : &found->second;
}

void install(const char* id, const char* declarations = "", const char* code = nullptr) {
    const std::string prefix = std::string(kModsFolder) + "/" + id + "/";
    files[prefix + "mod.json"] = std::string("{\"id\":\"") + id + "\",\"name\":\"Test\",\"version\":\"1.0.0\"" + declarations + "}";
    if (code) files[prefix + "test.code"] = code;
}

void end_resource(void* context, uint64_t key) {
    const std::string id = mod_id(static_cast<TSUCHI_Mod*>(context));
    events.push_back("end:" + id + ":" + std::to_string(key));
    ++released[id];
    if (id == "release.blocker") block_mod(*find_mod("release.target"), "refused by a release callback");
}

void hold(TSUCHI_Mod* mod, uint64_t key) {
    CHECK(own(mod, resource, key, end_resource, mod) == TSUCHI_OK);
}

void before(ModRecord& record) {
    CHECK(record.state == ModState::running && record.prepared && !record.settings.read);
    CHECK(record.context == expected_contexts[record.id]);
    ++before_calls;
    events.push_back(std::string("before:") + record.id);
    record.settings.read = true; // stand in for the settings loader, which must not run in preparation
}

TSUCHI_Result early_begin(TSUCHI_Mod* mod, const Json& value, Text& problem) {
    const ModRecord* record = find_mod(mod_id(mod));
    CHECK(record && record->state == ModState::ready && !record->settings.read && !record->prepared);
    events.push_back(std::string("early:") + mod_id(mod));
    hold(mod, 1);
    if (value.kind != Json::Kind::text) return TSUCHI_OK;
    if (!strcmp(value.text, "fail")) { problem.set("early failure"); return TSUCHI_INVALID; }
    if (!strcmp(value.text, "refuse")) mod_block(mod, "early refusal");
    if (!strcmp(value.text, "reenter")) {
        const size_t count = events.size();
        prepare_mods(); start_mods();
        CHECK(find_mods() == TSUCHI_NOT_READY && events.size() == count && !before_calls && !code_calls);
    }
    if (!strcmp(value.text, "forget")) {
        forget_mods();
        CHECK(mod_usable(mod) && find_mods() == TSUCHI_NOT_READY);
        events.push_back("early:return-before-forget");
    }
    return TSUCHI_OK;
}

TSUCHI_Result second_begin(TSUCHI_Mod* mod, const Json& value, Text& problem) {
    events.push_back(std::string("early2:") + mod_id(mod));
    hold(mod, 2);
    if (value.kind == Json::Kind::text && !strcmp(value.text, "fail")) {
        problem.set("second early failure"); return TSUCHI_INVALID;
    }
    return TSUCHI_OK;
}

TSUCHI_Result late_begin(TSUCHI_Mod* mod, const Json& value, Text&) {
    const ModRecord* record = find_mod(mod_id(mod));
    CHECK(record && record->state == ModState::running && record->settings.read && record->prepared);
    CHECK(mod == expected_contexts[mod_id(mod)]);
    events.push_back(std::string("late:") + mod_id(mod));
    hold(mod, 3);
    if (value.kind == Json::Kind::text && !strcmp(value.text, "forget")) {
        forget_mods();
        CHECK(mod_usable(mod));
        events.push_back("late:return-before-forget");
    }
    return TSUCHI_OK;
}

TSUCHI_Result run(TSUCHI_Mod* mod, Text& problem) {
    CHECK(mod == expected_contexts[mod_id(mod)]);
    CHECK(find_mod(mod_id(mod))->state == ModState::running);
    ++code_calls;
    events.push_back(std::string("code:") + mod_id(mod));
    const std::string path = std::string(kModsFolder) + "/" + mod_id(mod) + "/test.code";
    if (files[path] == "fail") {
        hold(mod, 4);
        problem.set("runtime failure"); return TSUCHI_INVALID;
    }
    return TSUCHI_OK;
}

void remember_contexts() {
    for (uint32_t i = 0; i < mod_count(); ++i) {
        ModRecord& record = *mod_at(i);
        if (record.state == ModState::ready) {
            CHECK(record.prepared && record.context && mod_usable(record.context) && !record.settings.read);
            expected_contexts[record.id] = record.context;
        }
    }
}

void reset() {
    forget_mods();
    for (const auto& [id, context] : expected_contexts) {
        (void)id;
        CHECK(!mod_running(context) && !owned_count(context));
    }
    files.clear(); events.clear(); expected_contexts.clear(); released.clear();
    before_calls = code_calls = choices_attempts = 0;
    choices_result = TSUCHI_OK; activation = order = "";
    CHECK(load_mod_choices() == TSUCHI_OK);
}

bool any_part(const Json&, Text&) { return true; }
// Refusals that give no reason of their own, as several content parts did.
TSUCHI_Result busy_begin(TSUCHI_Mod*, const Json&, Text&) { return TSUCHI_BUSY; }
TSUCHI_Result memory_begin(TSUCHI_Mod*, const Json&, Text&) { return TSUCHI_NO_MEMORY; }
TSUCHI_Result said_begin(TSUCHI_Mod*, const Json&, Text& problem) {
    problem.set("its own reason");
    return TSUCHI_INVALID;
}
const char* problem_of(const char* id) {
    const ModRecord* record = find_mod(id);
    return record ? record->problem.get() : "";
}
} // namespace

namespace tsuchi {
const char* result_name(TSUCHI_Result result) {
    switch (result) {
    case TSUCHI_BUSY: return "busy";
    case TSUCHI_NO_MEMORY: return "no_memory";
    case TSUCHI_INVALID: return "invalid";
    default: return "other";
    }
}
namespace mod { void set_backend(const Backend*) {} void emit_restarted(const TSUCHI_ModRestarted&) {} }
namespace file { void set_backend(const Backend*) {} }
namespace game {
bool files_known() { return true; }
bool path_stays_within(const char*) { return true; }
TSUCHI_Result list_folder(const char* path, Texts&, Texts& folders) {
    if (strcmp(path, "rom:/tsuchi/mods")) return TSUCHI_NOT_FOUND;
    ++folder_reads;
    std::set<std::string> ids;
    const std::string prefix = std::string(kModsFolder) + "/";
    for (const auto& [where, text] : files) {
        (void)text;
        if (where.compare(0, prefix.size(), prefix)) continue;
        const std::string rest = where.substr(prefix.size());
        ids.insert(rest.substr(0, rest.find('/')));
    }
    for (const auto& id : ids) if (!folders.add(id.c_str())) return TSUCHI_NO_MEMORY;
    return TSUCHI_OK;
}
TSUCHI_Result file_size(const char* path, uint64_t& size) {
    const std::string* file = find_file(path);
    if (!file) return TSUCHI_NOT_FOUND;
    size = file->size();
    return TSUCHI_OK;
}
TSUCHI_Result read_file_part(const char*, uint64_t, void*, uint64_t, uint64_t&) { return TSUCHI_IO; }
TSUCHI_Result read_whole_file(const char* path, char*& data, uint64_t& size) {
    const std::string* file = find_file(path);
    if (!file) return TSUCHI_NOT_FOUND;
    ++manifest_reads;
    size = file->size();
    data = static_cast<char*>(allocate(size + 1));
    if (!data) return TSUCHI_NO_MEMORY;
    memcpy(data, file->c_str(), size + 1);
    return TSUCHI_OK;
}
} // namespace game
} // namespace tsuchi

int main() {
    CHECK(add_manifest_section({"busy", any_part, busy_begin}));
    CHECK(add_manifest_section({"memory", any_part, memory_begin}));
    CHECK(add_manifest_section({"said", any_part, said_begin}));
    // A key whose family this build of the game cannot serve is told as that, and not as a key nobody knows.
    CHECK(add_manifest_key_off("off_key", "off_family"));
    install("check.key_off", ",\"off_key\":{}");
    install("check.key_unknown", ",\"nobody_knows\":{}");
    CHECK(find_mods() == TSUCHI_OK);
    CHECK(!strcmp(problem_of("check.key_off"), "mod.json uses off_key, and this build of the game has that family disabled: off_family"));
    CHECK(!strcmp(problem_of("check.key_unknown"), "mod.json has a key the framework does not know: nobody_knows"));
    forget_mods();
    files.clear();
    install("check.busy", ",\"busy\":{}");
    install("check.memory", ",\"memory\":{}");
    install("check.said", ",\"said\":{}");
    CHECK(find_mods() == TSUCHI_OK);
    start_mods();
    // A refusal without a reason of its own is told by its result, not as a lack of memory.
    CHECK(!strcmp(problem_of("check.busy"), "mod.json: \"busy\" answered busy"));
    CHECK(!strcmp(problem_of("check.memory"), "mod.json: there is no memory for what it declares"));
    CHECK(!strcmp(problem_of("check.said"), "mod.json: its own reason"));
    if (failed) {
        for (const char* id : {"check.busy", "check.memory", "check.said"}) std::printf("%s: %s\n", id, problem_of(id));
    }
    forget_mods();
    files.clear();

    CHECK(add_manifest_section({"early", any_part, early_begin, true}));
    CHECK(add_manifest_section({"early2", any_part, second_begin, true}));
    CHECK(add_manifest_section({"late", any_part, late_begin})); // legacy three-field aggregate stays late
    CHECK(add_mod_runtime({"test.code", run}));
    set_before_start(before);
    set_mod_choices_loader([](ModChoices& choices) {
        ++choices_attempts;
        return choices_result == TSUCHI_OK ? choices.load(activation, order) : choices_result;
    });

    // Busy attempts within the bound of 300 neither publish candidates nor repeat their file IO. Once resolved, saved
    // activation/order and required dependencies determine preparation, while scripts/settings remain untouched.
    install("early.a", ",\"early\":{}", "ok");
    install("early.z", ",\"early\":{},\"early2\":{},\"late\":{}", "ok");
    install("early.consumer", ",\"requires\":{\"early.a\":\"*\"},\"early\":{},\"late\":{}", "ok");
    install("early.off", ",\"early\":{}", "ok");
    install("ordinary", ",\"late\":{}", "ok");
    choices_result = TSUCHI_NOT_READY;
    const uint32_t folders_before = folder_reads, manifests_before = manifest_reads;
    for (uint32_t attempt = 0; attempt < 299; ++attempt) {
        CHECK(find_mods() == TSUCHI_NOT_READY && !mod_count());
        prepare_mods();
        CHECK(events.empty() && !before_calls && !code_calls);
    }
    CHECK(folder_reads == folders_before + 1 && manifest_reads == manifests_before + 5 && choices_attempts == 299);
    choices_result = TSUCHI_NO_MEMORY;
    CHECK(find_mods() == TSUCHI_NO_MEMORY && !mod_count());
    prepare_mods();
    CHECK(events.empty() && !before_calls && !code_calls && choices_attempts == 300);
    CHECK(folder_reads == folders_before + 1 && manifest_reads == manifests_before + 5);
    activation = "p4a1:early.off:";
    order = "p4o1:ordinary:early.z:early.consumer:early.a:";
    choices_result = TSUCHI_OK;
    CHECK(find_mods() == TSUCHI_OK && choices_attempts == 301);
    prepare_mods(); remember_contexts();
    CHECK((events == std::vector<std::string>{"early:early.z", "early2:early.z", "early:early.a", "early:early.consumer"}));
    CHECK(find_mod("early.off")->state == ModState::disabled && !find_mod("early.off")->context);
    CHECK(mod_rank(expected_contexts["ordinary"]) < mod_rank(expected_contexts["early.z"]) &&
          mod_rank(expected_contexts["early.z"]) < mod_rank(expected_contexts["early.a"]) &&
          mod_rank(expected_contexts["early.a"]) < mod_rank(expected_contexts["early.consumer"]));
    const auto prepared_events = events;
    prepare_mods();
    CHECK(events == prepared_events && !before_calls && !code_calls);
    start_mods();
    CHECK(before_calls == 4 && code_calls == 4 && events.size() == prepared_events.size() + 11);
    CHECK(find_mod("early.z")->context == expected_contexts["early.z"] && find_mod("early.z")->state == ModState::running);
    const auto started_events = events;
    prepare_mods(); remember_contexts(); start_mods();
    CHECK(events == started_events);
    reset();

    // Forgetting before startup releases every provisional record in reverse dependency order.
    install("provider", ",\"early\":{}");
    install("dependent", ",\"requires\":{\"provider\":\"*\"},\"early\":{}");
    CHECK(find_mods() == TSUCHI_OK); prepare_mods(); remember_contexts();
    forget_mods();
    CHECK((events == std::vector<std::string>{"early:provider", "early:dependent", "end:dependent:1", "end:provider:1"}));
    CHECK(!before_calls && !code_calls && !mod_count());
    reset();

    // A later early-section failure unwinds the same context and prevents its dependent from preparing.
    install("provider", ",\"early\":{},\"early2\":\"fail\"");
    install("dependent", ",\"requires\":{\"provider\":\"*\"},\"early\":{}");
    CHECK(find_mods() == TSUCHI_OK); prepare_mods();
    CHECK(find_mod("provider")->state == ModState::blocked && !find_mod("provider")->context && !find_mod("provider")->prepared);
    CHECK(find_mod("dependent")->state == ModState::blocked && !find_mod("dependent")->context);
    CHECK(!strcmp(problem_of("provider"), "mod.json: second early failure") && released["provider"] == 2);
    const auto failed_events = events;
    prepare_mods(); remember_contexts(); start_mods();
    CHECK(events == failed_events && !before_calls && !code_calls);
    reset();

    // A provider can prepare successfully and fail later in code. Its already prepared dependent releases its
    // records without ever running settings, late declarations or code; an independent mod still starts.
    install("provider", ",\"early\":{}", "fail");
    install("dependent", ",\"requires\":{\"provider\":\"*\"},\"early\":{},\"late\":{}", "ok");
    install("independent", ",\"early\":{}", "ok");
    order = "p4o1:provider:dependent:independent:";
    CHECK(find_mods() == TSUCHI_OK); prepare_mods(); remember_contexts(); start_mods();
    CHECK(find_mod("provider")->state == ModState::blocked && !find_mod("provider")->context);
    CHECK(find_mod("dependent")->state == ModState::blocked && !find_mod("dependent")->context && !find_mod("dependent")->settings.read);
    CHECK(released["provider"] == 2 && released["dependent"] == 1 && before_calls == 2 && code_calls == 2);
    CHECK(find_mod("independent")->state == ModState::running && mod_usable(find_mod("independent")->context));
    CHECK(!strcmp(problem_of("provider"), "test.code: runtime failure"));
    reset();

    // An external refusal between phases also retires provisional dependent ownership immediately.
    install("provider", ",\"early\":{}");
    install("dependent", ",\"requires\":{\"provider\":\"*\"},\"early\":{}");
    CHECK(find_mods() == TSUCHI_OK); prepare_mods(); remember_contexts();
    CHECK(!block_mod(*find_mod("provider"), "blocked after preparation"));
    CHECK(!find_mod("provider")->context && !find_mod("dependent")->context && released["provider"] == 1 && released["dependent"] == 1);
    reset();

    // Cleanup may refuse a ready context that the reverse-order pass already visited. It is retired in the
    // same rollback, without waiting for another world frame or allowing its declarations to run again.
    install("release.blocker", ",\"early\":{}");
    install("release.target", ",\"early\":{}");
    order = "p4o1:release.blocker:release.target:";
    CHECK(find_mods() == TSUCHI_OK); prepare_mods(); remember_contexts();
    CHECK(!block_mod(*find_mod("release.blocker"), "start rollback"));
    CHECK(!find_mod("release.blocker")->context && !find_mod("release.target")->context);
    CHECK(released["release.blocker"] == 1 && released["release.target"] == 1);
    reset();

    // Nested lifecycle entry does not start code early. A callback's forget invalidates the phase but leaves
    // its records/context alive until that callback returns, then releases them without a second begin.
    install("nested", ",\"early\":\"reenter\",\"late\":{}", "ok");
    CHECK(find_mods() == TSUCHI_OK); prepare_mods(); remember_contexts(); start_mods();
    CHECK(before_calls == 1 && code_calls == 1);
    reset();
    install("forgotten", ",\"early\":\"forget\"", "ok");
    CHECK(find_mods() == TSUCHI_OK); prepare_mods();
    CHECK(!mod_count() && !before_calls && !code_calls && released["forgotten"] == 1);
    CHECK((events == std::vector<std::string>{"early:forgotten", "early:return-before-forget", "end:forgotten:1"}));
    reset();
    install("lateforgotten", ",\"early\":{},\"late\":\"forget\"", "ok");
    CHECK(find_mods() == TSUCHI_OK); prepare_mods(); remember_contexts(); start_mods();
    CHECK(!mod_count() && before_calls == 1 && !code_calls && released["lateforgotten"] == 2);
    reset();
    install("refused", ",\"early\":\"refuse\"");
    CHECK(find_mods() == TSUCHI_OK); prepare_mods();
    CHECK(find_mod("refused")->state == ModState::blocked && !find_mod("refused")->context && released["refused"] == 1);
    CHECK(!strcmp(problem_of("refused"), "early refusal"));
    reset();
    // Choices that never become readable, such as a save session that does not open, do not keep every mod
    // from starting: the 300th busy attempt publishes the default activation and order, and the log says why.
    static std::string told;
    set_log_sink([](const char* id, const char* text) { told += std::string(id) + " " + text + "\n"; });
    install("stuck.a", ",\"early\":{}", "ok");
    install("stuck.b", ",\"late\":{}", "ok");
    activation = "p4a1:stuck.a:"; // read, this would turn stuck.a off
    choices_result = TSUCHI_NOT_READY;
    for (uint32_t attempt = 1; attempt < 300; ++attempt) CHECK(find_mods() == TSUCHI_NOT_READY && !mod_count());
    CHECK(find_mods() == TSUCHI_OK && mod_count() == 2);
    const auto stuck = [](const char* id, ModState state) { const ModRecord* record = find_mod(id); return record && record->state == state; };
    CHECK(stuck("stuck.a", ModState::ready) && stuck("stuck.b", ModState::ready));
    CHECK(told.find("did not open") != std::string::npos);
    prepare_mods(); remember_contexts(); start_mods();
    CHECK(stuck("stuck.a", ModState::running) && stuck("stuck.b", ModState::running));
    set_log_sink(nullptr); choices_result = TSUCHI_OK; activation = "";
    reset();
    // A later search waits its own 300 attempts again.
    install("stuck.c", "", "ok");
    choices_result = TSUCHI_NOT_READY;
    CHECK(find_mods() == TSUCHI_NOT_READY && !mod_count());
    choices_result = TSUCHI_OK;
    CHECK(find_mods() == TSUCHI_OK && mod_count() == 1);
    reset();
    set_before_start(nullptr); set_mod_choices_loader(nullptr);
    if (!failed) std::puts("manifest phases: private pending selection, adoption, ordering, rollback and reentry checked");
    return failed;
}
