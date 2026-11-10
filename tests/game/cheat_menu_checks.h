// The cheat menu and the Installed page's package rows, with the real screen model, a cheat runtime whose
// entries can change between frames, and packages the test lists.
#include "ui_cheats.h"
#include "packages.h"
#include <map>
#include <set>
namespace {
struct FakeCheat { std::string mod, name, problem; bool on = false; };
std::vector<FakeCheat> fake_cheats;
TSUCHI_Result cheat_result = TSUCHI_OK;
uint32_t cheat_calls = 0;
std::vector<package_store::Receipt> fake_packages;
std::vector<uint32_t> removed_packages;
TSUCHI_Result remove_result = TSUCHI_OK;
uint64_t fake_package_revision = 1;
uint32_t description_reads = 0;
TSUCHI_Result description_result = TSUCHI_OK;
std::map<std::string, std::string> package_manifests;
std::set<std::string> mounted_packages;
}
namespace tsuchi {
uint32_t cheat_count() { return uint32_t(fake_cheats.size()); }
bool cheat_at(uint32_t index, CheatEntry& out) { if (index >= fake_cheats.size()) return false; const auto& c = fake_cheats[index]; out = {c.mod.c_str(), c.name.c_str(), c.on, c.problem.c_str()}; return true; }
TSUCHI_Result set_cheat(const char* mod, const char* name, bool on) {
    ++cheat_calls;
    for (auto& c : fake_cheats) if (c.mod == mod && c.name == name) { if (!c.problem.empty()) return TSUCHI_UNSUPPORTED; if (cheat_result == TSUCHI_CAPACITY) return cheat_result; c.on = on; return cheat_result; }
    return TSUCHI_NOT_FOUND;
}
uint32_t installed_package_count() { return uint32_t(fake_packages.size()); }
uint64_t installed_packages_revision() { return fake_package_revision; }
bool container_mounted_for(const char* id) { return mounted_packages.count(id) != 0; }
TSUCHI_Result installed_mod_description(uint32_t p, uint32_t m, ModRecord& out) {
    ++description_reads;
    if (description_result != TSUCHI_OK) return description_result;
    if (p >= fake_packages.size() || m >= fake_packages[p].count) return TSUCHI_NOT_FOUND;
    const char* id = fake_packages[p].mods[m];
    const auto found = package_manifests.find(id);
    if (found == package_manifests.end()) return TSUCHI_NOT_FOUND;
    memcpy(out.id, id, strlen(id) + 1);
    out.manifest = read_json(found->second.c_str(), found->second.size());
    assert(out.manifest && out.manifest->find("name") && out.manifest->find("version"));
    assert(out.name.set(out.manifest->find("name")->text) && out.version.set(out.manifest->find("version")->text));
    out.manifest_valid = true; out.state = ModState::ready;
    assert(declare_settings(out.settings, out.manifest->find("settings"), out.problem));
    return TSUCHI_OK;
}
void forget_mod_description(ModRecord& record) {
    assert(!record.context);
    record.name.clear(); record.version.clear(); record.problem.clear();
    forget_settings(record.settings); release_json(record.manifest);
    record = {};
}
const package_store::Receipt* installed_package(uint32_t i) { return i < fake_packages.size() ? &fake_packages[i] : nullptr; }
const package_store::Receipt* package_holding(const char* id, uint32_t* index) {
    for (uint32_t i = 0; i < fake_packages.size(); ++i) if (package_store::has_mod(fake_packages[i], id)) { if (index) *index = i; return &fake_packages[i]; }
    return nullptr;
}
TSUCHI_Result remove_installed_package(uint32_t i) {
    if (remove_result != TSUCHI_OK) return remove_result;
    if (i >= fake_packages.size()) return TSUCHI_NOT_FOUND;
    removed_packages.push_back(i); fake_packages.erase(fake_packages.begin() + i); ++fake_package_revision; return TSUCHI_OK;
}
}
namespace {
void cheat_menu_checks() {
    fake_cheats = {{"example", "First", "", false}, {"unrelated", "Foreign", "", false}, {"example", "Blocked", "no code access", false}, {"example", "Third", "", false}, {"example", "Fourth", "", false}};
    CheatMenu menu; assert(mod_has_cheats("example") && !mod_has_cheats("missing")); assert(menu.open(framework, "example") == TSUCHI_OK);
    assert(control("cheat0")->kind == UiKind::choice && control("cheat0")->selected == 0);
    assert(control("cheat1")->kind == UiKind::text && !strcmp(control("cheat1")->text.get(), "no code access")); assert(control("next"));
    const auto first = menu.screen(); menu.activate({first, "next"}); assert(menu.screen().id != first.id && !strcmp(control("cheat0")->label.get(), "Fourth"));
    menu.change({first, "cheat0", "on", "off"}); assert(cheat_calls == 0); // queued event from old page
    menu.activate({menu.screen(), "previous"});
    std::swap(fake_cheats[0], fake_cheats[4]); // old displayed name must win over new array index
    menu.change({menu.screen(), "cheat0", "on", "off"}); assert(fake_cheats[4].on && !fake_cheats[0].on && !fake_cheats[1].on);
    assert(!strcmp(control("cheat0")->label.get(), "Fourth"));
    cheat_result = TSUCHI_CAPACITY; ui::table.set_choice(framework, menu.screen(), "cheat0", "on"); menu.change({menu.screen(), "cheat0", "on", "off"});
    assert(!fake_cheats[0].on && control("cheat0")->selected == 0 && strstr(control("status")->text.get(), "1,024"));
    cheat_result = TSUCHI_IO; menu.change({menu.screen(), "cheat0", "on", "off"}); assert(fake_cheats[0].on && control("cheat0")->selected == 1 && strstr(control("status")->text.get(), "session"));
    const auto before_fault = menu.screen(); fake_cheats[0].on = false; fake_cheats[0].problem = "it ran too long without finishing"; menu.refresh();
    assert(menu.screen().id != before_fault.id && control("cheat0")->kind == UiKind::text && strstr(control("cheat0")->text.get(), "too long"));
    const auto calls = cheat_calls; menu.change({menu.screen(), "cheat0", "on", "off"}); assert(cheat_calls == calls);
    fake_cheats = {{"example", "Code patch", "it changes the game's code, and this install has no code access", false}}; menu.refresh();
    assert(control("done") && control("cheat0")->kind == UiKind::text);
    fake_cheats.clear(); menu.refresh(); assert(control("empty") && !control("cheat0"));
    menu.change({before_fault, "cheat0", "on", "off"}); assert(cheat_calls == calls);
    menu.activate({menu.screen(), "back"}); assert(!menu.screen().id && !ui_active_screen()); cheat_result = TSUCHI_OK;

    // A refresh the game refuses to show: the old screen shows again, and the menu is never left without one.
    fake_cheats = {{"example", "First", "", false}, {"example", "Second", "", false}};
    assert(menu.open(framework, "example") == TSUCHI_OK);
    const auto shown = menu.screen();
    fail_opens = 1; fake_cheats[1].problem = "it faulted"; menu.refresh();
    assert(menu.showing() && menu.screen().id == shown.id && ui_active_screen() && ui_active_screen()->id == shown.id);
    assert(strstr(control("status")->text.get(), "could not be refreshed"));
    // Refused twice, the old one too: the new screen waits and shows on a later frame.
    fail_opens = 2; fake_cheats[0].problem = "it faulted too"; menu.refresh();
    assert(menu.showing() && menu.screen().id != shown.id && !ui_active_screen());
    menu.refresh(); assert(ui_active_screen() && ui_active_screen()->id == menu.screen().id && control("cheat0")->kind == UiKind::text);
    // Refused for good: it gives up after its frames and closes, leaving nothing half shown.
    fake_cheats[1].problem.clear(); fail_opens = 1000; menu.refresh();
    for (uint32_t i = 0; i < CheatMenu::kReopenFrames && menu.showing(); ++i) menu.refresh();
    assert(!menu.showing() && !ui_active_screen()); fail_opens = 0;
    // Opening refused: no screen, and nothing kept.
    fail_opens = 1; assert(menu.open(framework, "example") != TSUCHI_OK && !menu.showing() && !ui_active_screen()); fail_opens = 0;
    fake_cheats.clear();
}

package_store::Receipt package_of(const char* folder, const char* title, std::initializer_list<const char*> mods) {
    package_store::Receipt r{};
    memcpy(r.folder, folder, strlen(folder) + 1);
    const auto set = [](char* out, const char* text) { memcpy(out, text, strlen(text) + 1); };
    set(r.source, "github"); set(r.key, "author/mod"); set(r.title, title); set(r.release, "v2"); memset(r.digest, 'a', 64);
    for (const char* id : mods) set(r.mods[r.count++], id);
    return r;
}
// The Installed page: a mod from a downloaded package shows its package and can remove it; one with cheats
// opens them; none of it needs a downloader mod.
void installed_page_checks(uint64_t host) {
    callbacks.host_ready(host); assert(ui_mod_list_native_hold(host) == TSUCHI_OK);
    fake_packages = {package_of("00000000000000000000000000000001", "Other [mod]", {"leaf"})};
    fake_cheats = {{"leaf", "Infinite", "", false}};
    const uint32_t other = find_row("leaf");
    callbacks.action(host, other, 102, true);
    assert(ui_active_screen() && control("package") && !strcmp(control("package")->text.get(), "Other (mod) v2") && !strcmp(control("source")->text.get(), "github"));
    assert(control("remove") && control("cheats"));
    // Cheats: the details give way to the cheat screen, and a toggle reaches the runtime.
    select("cheats"); step();
    assert(ui_active_screen() && !strcmp(ui_active_screen()->title.get(), "Cheats") && control("cheat0"));
    ui::emit_choice_changed_to(framework, {{ui_active_screen()->id}, "cheat0", "on", "off"}); step();
    assert(fake_cheats[0].on);
    select("back"); step(); assert(!ui_active_screen());
    // Remove asks first; no keeps the package.
    callbacks.action(host, other, 102, true); select("remove");
    assert(!ui_active_screen() && prompt.find("Remove the package Other (mod)?") == 0);
    answer(false); assert(fake_packages.size() == 1 && removed_packages.empty());
    callbacks.action(host, other, 102, true); select("remove"); answer(true);
    assert(fake_packages.empty() && removed_packages.size() == 1 && message.find("Removed.") == 0);
    // A mod of the game's files has neither.
    callbacks.action(host, find_row("base"), 102, true);
    assert(ui_active_screen() && !control("package") && !control("remove") && !control("cheats"));
    ui_action(5); step();
    remove_result = TSUCHI_BUSY; fake_packages = {package_of("00000000000000000000000000000002", "Other", {"leaf"})};
    callbacks.action(host, other, 102, true); select("remove"); answer(true);
    assert(fake_packages.size() == 1 && message.find("A download is running") == 0);
    remove_result = TSUCHI_OK; fake_packages.clear(); fake_cheats.clear(); removed_packages.clear();
    callbacks.host_lost(host);
}

// Installing while Get Mods owns input must add real metadata on return,
// preserve unsaved choices, and never start or relocate runtime records.
void downloaded_catalogue_checks(uint64_t host) {
    save_result = external_save_result = external_load_result = TSUCHI_OK;
    Text original_activation, original_order;
    assert(mod_choices().activation_text(original_activation) && mod_choices().order_text(original_order));
    external.clear(); fake_packages.clear(); ++fake_package_revision;
    callbacks.host_ready(host); assert(ui_mod_list_native_hold(host) == TSUCHI_OK);
    const bool base_draft = !mod_choices().enabled("base");
    const uint64_t asked_before = asked.id;
    callbacks.action(host, find_row("base"), base_draft ? 3 : 2, base_draft);
    if (asked.id != asked_before) answer(true);
    assert(request(find_row("base")) == base_draft);
    assert(ui::table.hold_menu_input(framework) == TSUCHI_OK);
    fake_packages = {package_of("00000000000000000000000000000003", "Natural Hair colors", {"base", "new.hair"})};
    package_manifests["new.hair"] = "{\"id\":\"new.hair\",\"name\":\"Natural Hair colors\",\"version\":\"1.0.0\"}";
    mounted_packages.insert("new.hair"); ++fake_package_revision;
    const uint32_t reads_before = description_reads;
    step(); assert(callbacks.count() == 3 && description_reads == reads_before);
    assert(ui::table.release_menu_input(framework) == TSUCHI_OK);
    physical_buttons = 0; listener(0); step();
    assert(callbacks.count() == 4 && mod_count() == 3 && !find_mod("new.hair"));
    UiModListRow row{};
    assert(callbacks.row(find_row("new.hair"), row) && !strcmp(row.name, "Natural Hair colors") &&
           !strcmp(row.status, "Active for new loads") && row.requested && !row.has_settings);
    assert(request(find_row("base")) == base_draft);
    callbacks.action(host, find_row("new.hair"), 102, true);
    assert(control("remove") && control("package") && control("loading") && !control("cheats"));
    select("earlier"); step(); assert(!ui_active_screen() && callbacks.count() == 4);
    assert(request(find_row("base")) == base_draft);

    // Multiple new script mods appear separately, without pretending they ran.
    fake_packages.push_back(package_of("00000000000000000000000000000004", "Scripts", {"new.script", "new.library"}));
    package_manifests["new.library"] = "{\"id\":\"new.library\",\"name\":\"Library\",\"version\":\"1.0.0\"}";
    package_manifests["new.script"] = "{\"id\":\"new.script\",\"name\":\"Script\",\"version\":\"1.0.0\",\"requires\":{\"new.library\":\"*\",\"base\":\"*\"},\"settings\":{\"enabled\":{\"type\":\"toggle\",\"default\":true}}}";
    ++fake_package_revision; step();
    assert(callbacks.count() == 6 && mod_count() == 3 && !find_mod("new.script"));
    assert(callbacks.row(find_row("new.script"), row) && !strcmp(row.status, "Restart to load") && !row.has_settings);
    assert(request(find_row("base")) == base_draft);
    callbacks.action(host, find_row("new.library"), 2, false); answer(true);
    assert(!request(find_row("new.script")) && !request(find_row("new.library")));
    callbacks.action(host, find_row("new.script"), 3, true);
    assert(prompt.find("new.library") != std::string::npos);
    answer(true); assert(request(find_row("new.script")) && request(find_row("new.library")) && request(find_row("base")));

    // Removal refreshes the visible catalogue and keeps other draft changes.
    callbacks.action(host, find_row("new.hair"), 102, true); select("remove"); answer(true); step();
    assert(callbacks.count() == 5 && fake_packages.size() == 1);
    for (uint32_t i = 0; i < callbacks.count(); ++i) { assert(callbacks.row(i, row)); assert(strcmp(row.id, "new.hair")); }
    assert(request(find_row("new.script")));

    // A failed read keeps the previous catalogue and is not retried per frame.
    fake_packages.push_back(package_of("00000000000000000000000000000005", "Later", {"new.later"}));
    package_manifests["new.later"] = "{\"id\":\"new.later\",\"name\":\"Later\",\"version\":\"1.0.0\"}";
    description_result = TSUCHI_IO; ++fake_package_revision; step();
    const uint32_t failed_reads = description_reads;
    for (uint32_t i = 0; i < 20; ++i) step();
    assert(callbacks.count() == 5 && description_reads == failed_reads && request(find_row("new.script")));
    description_result = TSUCHI_OK; ++fake_package_revision; step(); assert(callbacks.count() == 6);
    // Discard/reopen retains the catalogue; save persists a choice made before first boot.
    callbacks.action(host, find_row("new.later"), 2, false); answer(true);
    callbacks.action(host, 0, 5, false); answer(false);
    assert(ui_mod_list_native_hold(host) == TSUCHI_OK && request(find_row("new.later")));
    callbacks.action(host, find_row("new.later"), 2, false); answer(true);
    callbacks.action(host, 0, 5, false); answer(true);
    assert(!mod_choices().enabled("new.later"));
    callbacks.host_lost(host); callbacks.host_ready(host); assert(ui_mod_list_native_hold(host) == TSUCHI_OK);
    assert(!request(find_row("new.later")) && mod_count() == 3);
    callbacks.host_lost(host);
    fake_packages.clear(); package_manifests.clear(); mounted_packages.clear(); ++fake_package_revision;
    ModChoices restored; assert(restored.load(original_activation.get(), original_order.get()) == TSUCHI_OK);
    swap_mod_choices(restored); restored.clear(); original_activation.clear(); original_order.clear();
}
}
