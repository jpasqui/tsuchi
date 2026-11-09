// The cheat menu and the Installed page's package rows, with the real screen model, a cheat runtime whose
// entries can change between frames, and packages the test lists.
#include "ui_cheats.h"
#include "packages.h"
namespace {
struct FakeCheat { std::string mod, name, problem; bool on = false; };
std::vector<FakeCheat> fake_cheats;
TSUCHI_Result cheat_result = TSUCHI_OK;
uint32_t cheat_calls = 0;
std::vector<package_store::Receipt> fake_packages;
std::vector<uint32_t> removed_packages;
TSUCHI_Result remove_result = TSUCHI_OK;
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
const package_store::Receipt* installed_package(uint32_t i) { return i < fake_packages.size() ? &fake_packages[i] : nullptr; }
const package_store::Receipt* package_holding(const char* id, uint32_t* index) {
    for (uint32_t i = 0; i < fake_packages.size(); ++i) if (package_store::has_mod(fake_packages[i], id)) { if (index) *index = i; return &fake_packages[i]; }
    return nullptr;
}
TSUCHI_Result remove_installed_package(uint32_t i) {
    if (remove_result != TSUCHI_OK) return remove_result;
    if (i >= fake_packages.size()) return TSUCHI_NOT_FOUND;
    removed_packages.push_back(i); fake_packages.erase(fake_packages.begin() + i); return TSUCHI_OK;
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
}
