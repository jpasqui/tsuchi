#include "ui_mod_list.h"
#include <string.h>
#include "holds.h"
#include "mod_manager.h"
#include "mod_choices_game.h"
#include "external_mods_game.h"
#include "ui_mod_list_native.h"
#include "ui_native.h"
#include "ui_settings.h"
#include "ui_screens.h"
#include "ui_service.h"
#include "dialog_service.h"
#include "game.h"
#include "objects.h"
#include "mods.h"
#include "cost_clock.h"
#include "packages.h"
#include "ui_cheats.h"
#include "ui_mod_page.h"
#include "actors.h"
#include "reflect.h"
#include "frame.h"
namespace tsuchi { namespace {
TSUCHI_Mod* frontend_mod = nullptr;
uint64_t current_host = 0, previous_buttons = UINT64_MAX;
uint64_t tab_input_sample = 0, tab_input_session = 0;
uint32_t tab_session_traces = 0, tab_input_traces = 0;
struct TabInputTrace {
    uint64_t at, sample, session, host, held, previous, edges;
    uint32_t state, gates, action = 0;
    bool live, attempted = false, result = false;
};
void tab_input_trace(const TabInputTrace& trace) {
    char line[512]; size_t used = 0;
    const auto text = [&](const char* part) {
        const size_t size = strlen(part);
        if (used + size >= sizeof line) return false;
        memcpy(line + used, part, size); used += size; return true;
    };
    const auto number = [&](const char* name, uint64_t value) {
        char digits[21]; char* at = digits + sizeof digits; *--at = 0;
        do { *--at = char('0' + value % 10); value /= 10; } while (value);
        return text(name) && text(at);
    };
    if (!text("Settings tab sample") || !number(" at_us=", trace.at) || !number(" sample=", trace.sample) ||
        !number(" session=", trace.session) || !number(" host=", trace.host) || !number(" state=", trace.state) ||
        !number(" live=", trace.live) || !number(" gates=", trace.gates) || !number(" held=", trace.held) ||
        !number(" previous=", trace.previous) || !number(" edges=", trace.edges) || !number(" action=", trace.action) ||
        !number(" attempted=", trace.attempted) || !number(" result=", trace.result)) return;
    line[used] = 0; mod_note(kFrameworkLogId, line);
}
ModManager manager;
struct Row { ModRecord* native; ExternalModEntry* external; };
List<Row> rows;
uint32_t native_rows = 0;
TSUCHI_Screen detail{}, settings{};
TSUCHI_Dialog question{};
enum class Question { none, dependency, save, defaults, remove };
Question asking = Question::none;
bool restart_needed = false, detail_external = false, external_overflow = false;
char selected[kModIdBytes]{}, caption[641]{}, status[641]{};
CheatMenu cheats;                // a mod's cheats, opened from its details
char removing[33]{};             // the folder of the package a question asks to remove
// A mod's entry on the page: a row after the mods, which Confirm chooses. One for each mod. An entry released
// while the page shows keeps its row, with no mod, until the page is made again.
constexpr uint32_t kEntries = 8, kEntryIdBytes = 64, kEntryLabelBytes = 81;
struct Entry { TSUCHI_Mod* mod; char id[kEntryIdBytes], label[kEntryLabelBytes], owner[kModIdBytes]; };
struct Chosen { TSUCHI_Mod* mod; char id[kEntryIdBytes]; };
List<Entry> entries;
List<Chosen> chosen;
uint32_t shown_entries = 0;      // how many of them the page showing now has
bool leaving = false;            // the page on its own is to close: the game's own Back goes through
bool screen_up() { return detail.id || settings.id || cheats.showing(); }
// A dialog on the screen, the game's own or any mod's, takes the presses. The list acts on none of them, nor
// on the Confirm that closed it until that is let go.
bool confirm_spent = false;
bool dialog_up() {
    namespace t = game::table;
    if (!t::present(t::ui_manager::open_dialog)) return false;
    const uint64_t framework = game::game_framework();
    const uint64_t screens = framework ? game::read_or<uint64_t>(framework + t::game_framework::ui) : 0;
    return screens && game::read_or<uint64_t>(screens + t::ui_manager::open_dialog) != 0;
}
bool live() { return frontend_mod && mod_usable(frontend_mod) && current_host; }
// Plain labels cannot smuggle stock rich-text markup. Keep the stored data exact.
void plain(const char* text, char out[641], uint32_t limit = 637) {
    uint32_t n = 0;
    for (const auto* at = reinterpret_cast<const unsigned char*>(text ? text : ""); *at && n < limit;) {
        const uint32_t size = *at < 0x80 ? 1 : *at < 0xE0 ? 2 : *at < 0xF0 ? 3 : 4;
        if (n + size > limit) break;
        if (*at == '[' || *at == ']') { out[n++] = *at == '[' ? '(' : ')'; ++at; }
        else if (*at < 32) { out[n++] = ' '; ++at; }
        else { for (uint32_t i = 0; i < size; ++i) out[n++] = char(*at++); }
    }
    out[n] = 0;
}
bool ordering() {
    rows.count = 0; external_overflow = false;
    for (uint32_t i = 0; i < mod_count(); ++i) if (!rows.add({mod_at(i), nullptr})) return false;
    for (uint32_t i = 1; i < rows.count; ++i) {
        const auto row = rows[i]; uint32_t j = i;
        const uint32_t rank = manager.draft().rank(row.native->id);
        while (j && (manager.draft().rank(rows[j - 1].native->id) > rank ||
            (manager.draft().rank(rows[j - 1].native->id) == rank && strcmp(rows[j - 1].native->id, row.native->id) > 0))) {
            rows[j] = rows[j - 1]; --j;
        }
        rows[j] = row;
    }
    native_rows = rows.count;
    // The entries of mods that stopped or let go of theirs leave here, between two pages. The ones that
    // stay have their rows before the external packages are counted in: a full list drops packages, not entries.
    for (uint32_t i = 0; i < entries.count;)
        if (!entries[i].mod || !mod_usable(entries[i].mod)) {
            for (uint32_t j = i; j + 1 < entries.count; ++j) entries[j] = entries[j + 1];
            --entries.count;
        } else ++i;
    shown_entries = 0;
    while (shown_entries < entries.count && rows.count + shown_entries < game::table::constant::ui_mod_list_rows_limit) ++shown_entries;
    for (uint32_t i = 0; i < external_mods().count(); ++i) {
        auto* record = external_mods().at(i);
        if (find_mod(record->id)) {
            record->manageable = false;
            record->requested = record->saved = record->observed == ExternalModState::enabled;
            if (!record->problem.set("A Tsuchi mod uses this ID. Import this external package with another ID.")) return false;
        }
        if (rows.count + shown_entries < game::table::constant::ui_mod_list_rows_limit) {
            if (!rows.add({nullptr, record})) return false;
        } else external_overflow = true;
    }
    return true;
}
const char* description(const ModRecord& record) {
    if (!record.manifest_valid) return "Blocked";
    if (manager.draft().enabled(record.id) != mod_choices().enabled(record.id) ||
        ((!manager.draft().enabled(record.id) && (record.state == ModState::running || record.state == ModState::ready)) ||
         (manager.draft().enabled(record.id) && record.state == ModState::disabled))) return "Restart";
    switch (record.state) {
        case ModState::running: return "Running";
        case ModState::ready: return "Ready";
        case ModState::disabled: return "Disabled";
        case ModState::stopped: return "Stopped";
        default: return "Blocked";
    }
}
const char* observed(const ExternalModEntry& record) {
    switch (record.observed) {
        case ExternalModState::enabled: return "Files enabled";
        case ExternalModState::disabled: return "Files disabled";
        case ExternalModState::conflict: return "Conflict";
        case ExternalModState::recovery: return "Recovery required";
    }
    return "Unknown";
}
const char* external_description(const ExternalModEntry& record) {
    if (find_mod(record.id)) return "ID conflict";
    if (!record.manageable) return record.observed == ExternalModState::conflict ||
        record.observed == ExternalModState::recovery ? observed(record) : "Detected - unmanaged";
    if (record.requested != record.saved || record.saved != (record.observed == ExternalModState::enabled))
        return "Apply with manager";
    return observed(record);
}
uint32_t count() { return rows.count + shown_entries; } // the mods, then the mods' entries
bool row(uint32_t index, UiModListRow& out) {
    if (!live()) return false;
    if (index >= rows.count && index - rows.count < shown_entries) {
        const Entry& entry = entries[index - rows.count];
        plain(entry.label, caption, 300);
        out = {entry.owner, caption, false, "", false, true};
        return true;
    }
    if (index < rows.count) {
        if (rows[index].native) {
            auto& record = *rows[index].native; plain(record.name.get()[0] ? record.name.get() : record.id, caption, 300);
            plain(description(record), status, 300);
            out = {record.id, caption, manager.draft().enabled(record.id), status, has_visible_settings(record.settings)};
        } else {
            auto& record = *rows[index].external; plain(record.name.get()[0] ? record.name.get() : record.id, caption, 300);
            plain(external_description(record), status, 300);
            out = {record.id, caption, record.manageable && record.requested, status, false};
        }
    } else return false;
    return true;
}
void note(const char* message) {
    if (!live()) return;
    TSUCHI_Dialog ignored{}; dialog::table.message(frontend_mod, message, &ignored);
}
void sync() { if (live() && ui_mod_list_native_active(current_host)) ui_mod_list_native_sync(current_host); }
// Leaves the list: for the Game tab in Settings, and out of the page when it is on its own. There the game's
// own Back closes it, sent from the next input step that the menu takes it in.
void leave(uint64_t host) {
    if (ui_mod_page_alone()) leaving = true;
    else ui_mod_list_native_release(host);
}
void ask(Question kind, const char* text) {
    if (!live() || asking != Question::none) return;
    TSUCHI_Dialog requested{};
    if (dialog::table.confirm(frontend_mod, text, &requested) == TSUCHI_OK && live()) { question = requested; asking = kind; }
    else { if (kind == Question::dependency) manager.cancel(); sync(); }
}
void review() {
    Texts text;
    if (!text.add("Apply these dependency changes?")) { manager.cancel(); return; }
    for (uint32_t i = 0; i < manager.changes().count; ++i) {
        const auto& change = manager.changes()[i];
        if (!text.add(change.enabled ? "On: " : "Off: ", change.id)) { text.clear(); manager.cancel(); return; }
    }
    Text joined;
    if (!joined.reserve(text.used + 1)) { text.clear(); manager.cancel(); return; }
    size_t at = 0;
    for (const char* line = text.first(); line; line = text.after(line)) {
        const size_t size = strlen(line); memcpy(joined.data + at, line, size); at += size; joined.data[at++] = '\n';
    }
    joined.data[at] = 0;
    ask(Question::dependency, joined.get()); joined.clear(); text.clear();
}
void close_detail() {
    const TSUCHI_Screen old = detail; detail = {}; detail_external = false;
    if (old.id && frontend_mod && mod_usable(frontend_mod)) ui::table.destroy(frontend_mod, old);
}
void show_detail(ModRecord& record) {
    if (!live() || screen_up()) return;
    TSUCHI_Screen screen{}; plain(record.name.get(), caption);
    TSUCHI_Result result = ui::table.create(frontend_mod, caption[0] ? caption : record.id, &screen);
    plain(description(record), status);
    if (result == TSUCHI_OK) result = ui::table.add_text(frontend_mod, screen, "id", "ID", record.id);
    if (result == TSUCHI_OK) result = ui::table.add_text(frontend_mod, screen, "status", "Status", status);
    if (result == TSUCHI_OK && record.problem.get()[0]) {
        plain(record.problem.get(), status); result = ui::table.add_text(frontend_mod, screen, "reason", "Reason", status);
    }
    // A mod from a downloaded package says which, and the package can be removed here.
    const package_store::Receipt* package = package_holding(record.id);
    if (result == TSUCHI_OK && package) {
        plain(package->title, caption, 300); plain(package->release, status, 300);
        if (status[0]) { const size_t n = strlen(caption); if (n + 2 + strlen(status) < sizeof caption) { caption[n] = ' '; memcpy(caption + n + 1, status, strlen(status) + 1); } }
        result = ui::table.add_text(frontend_mod, screen, "package", "Package", caption);
        plain(package->source, status, 300);
        if (result == TSUCHI_OK) result = ui::table.add_text(frontend_mod, screen, "source", "Source", status);
    }
    if (result == TSUCHI_OK) result = ui::table.add_button(frontend_mod, screen, "earlier", "Move earlier");
    if (result == TSUCHI_OK) result = ui::table.add_button(frontend_mod, screen, "later", "Move later");
    if (result == TSUCHI_OK && mod_has_cheats(record.id)) result = ui::table.add_button(frontend_mod, screen, "cheats", "Cheats");
    if (result == TSUCHI_OK && package) result = ui::table.add_button(frontend_mod, screen, "remove", "Remove package");
    if (result == TSUCHI_OK) result = ui::table.hold_open(frontend_mod, screen);
    if (result == TSUCHI_OK && live()) { detail = screen; memcpy(selected, record.id, strlen(record.id) + 1); }
    else if (screen.id && mod_usable(frontend_mod)) ui::table.destroy(frontend_mod, screen);
}
void show_external_detail(const ExternalModEntry& record) {
    if (!live() || screen_up()) return;
    TSUCHI_Screen screen{}; plain(record.name.get(), caption);
    TSUCHI_Result result = ui::table.create(frontend_mod, caption[0] ? caption : record.id, &screen);
    const char* kind = record.kind == ExternalModKind::romfs ? "RomFS files" :
        record.kind == ExternalModKind::exefs ? "ExeFS patches or modules" :
        record.kind == ExternalModKind::mixed ? "RomFS and ExeFS files" : "Cheats";
    if (result == TSUCHI_OK) result = ui::table.add_text(frontend_mod, screen, "id", "ID", record.id);
    if (result == TSUCHI_OK) result = ui::table.add_text(frontend_mod, screen, "kind", "Type", kind);
    plain(record.version.get(), caption);
    if (result == TSUCHI_OK && caption[0]) result = ui::table.add_text(frontend_mod, screen, "version", "Version", caption);
    if (result == TSUCHI_OK) result = ui::table.add_text(frontend_mod, screen, "files", "Observed files", observed(record));
    if (record.manageable) {
        const char* request = record.requested != record.saved ?
            (record.requested ? "Enable: unsaved draft" : "Disable: unsaved draft") :
            record.saved != (record.observed == ExternalModState::enabled) ?
            (record.saved ? "Enable: saved for manager" : "Disable: saved for manager") : "No pending change";
        if (result == TSUCHI_OK) result = ui::table.add_text(frontend_mod, screen, "request", "Request", request);
        if (result == TSUCHI_OK) result = ui::table.add_text(frontend_mod, screen, "apply", "First",
            "Save choices and close game.");
        if (result == TSUCHI_OK) result = ui::table.add_text(frontend_mod, screen, "apply_export", "Next",
            "Export saved requests.");
        if (result == TSUCHI_OK) result = ui::table.add_text(frontend_mod, screen, "apply_next", "Then",
            "Apply using computer manager.");
        if (result == TSUCHI_OK) result = ui::table.add_text(frontend_mod, screen, "apply_restart", "Finally",
            "Restart Pikmin 4.");
    } else if (result == TSUCHI_OK) result = ui::table.add_text(frontend_mod, screen, "request", "Control", "Detected - unmanaged");
    if (record.kind == ExternalModKind::cheats) {
        if (result == TSUCHI_OK) result = ui::table.add_text(frontend_mod, screen, "effects", "Memory",
            "Off doesn't undo earlier writes.");
        if (result == TSUCHI_OK) result = ui::table.add_text(frontend_mod, screen, "saved_effects", "Save data",
            "Earlier saved effects may remain.");
    }
    if (result == TSUCHI_OK && record.problem.get()[0]) {
        plain(record.problem.get(), status); result = ui::table.add_text(frontend_mod, screen, "reason", "Reason", status);
    }
    if (result == TSUCHI_OK) result = ui::table.hold_open(frontend_mod, screen);
    if (result == TSUCHI_OK && live()) {
        detail = screen; detail_external = true; memcpy(selected, record.id, strlen(record.id) + 1);
    } else if (screen.id && mod_usable(frontend_mod)) ui::table.destroy(frontend_mod, screen);
}
void activated(void*, const TSUCHI_UiActivated* event) {
    if (live() && cheats.showing() && event->screen.id == cheats.screen().id) { cheats.activate(*event); return; }
    if (!live() || event->screen.id != detail.id || detail_external) return;
    auto* record = find_mod(selected); if (!record) { close_detail(); return; }
    if (!strcmp(event->control, "cheats")) {
        char id[kModIdBytes]; memcpy(id, record->id, strlen(record->id) + 1);
        close_detail();
        if (cheats.open(frontend_mod, id) != TSUCHI_OK) note("The cheats could not be shown.");
        return;
    }
    if (!strcmp(event->control, "remove")) {
        const package_store::Receipt* package = package_holding(record->id);
        if (!package) return;
        memcpy(removing, package->folder, sizeof removing);
        close_detail();
        plain(package->title, caption, 300);
        char question[400] = "Remove the package ";
        const char* tail = "? Its mods stop loading at the next start.";
        const size_t used = strlen(question), n = strlen(caption), m = strlen(tail);
        if (used + n + m < sizeof question) { memcpy(question + used, caption, n); memcpy(question + used + n, tail, m + 1); }
        ask(Question::remove, question);
        return;
    }
    uint32_t index = 0; while (index < native_rows && rows[index].native != record) ++index;
    const char* before = nullptr;
    if (!strcmp(event->control, "earlier") && index && index < native_rows) before = rows[index - 1].native->id;
    if (!strcmp(event->control, "later") && index + 1 < native_rows) before = index + 2 < native_rows ? rows[index + 2].native->id : "";
    if (!before) return;
    close_detail(); const auto result = manager.move(record->id, before);
    if (result == TSUCHI_OK && live() && ordering()) {
        ui_mod_list_native_release(current_host); ui_mod_list_native_hold(current_host);
        for (uint32_t i = 0; i < native_rows; ++i) if (rows[i].native == record) ui_mod_list_native_focus(current_host, i);
    } else note("The order could not be changed.");
}
void changed(void*, const TSUCHI_UiChoiceChanged* event) {
    if (live() && cheats.showing()) cheats.change(*event);
}
void cheats_step() { if (live() && cheats.showing()) cheats.refresh(); else if (!live() && cheats.showing()) cheats.close(); }
void closed(void*, const TSUCHI_UiClosed* event) {
    if (cheats.showing() && event->screen.id == cheats.screen().id) cheats.close();
    if (event->screen.id == detail.id) close_detail();
    if (event->screen.id == settings.id) settings = {};
}
void answered(void*, const TSUCHI_DialogAnswered* event) {
    if (!live() || event->dialog.id != question.id || asking == Question::none) return;
    const auto kind = asking; asking = Question::none; question = {};
    if (kind == Question::dependency) { if (event->accepted) manager.confirm(); else manager.cancel(); }
    else if (kind == Question::remove) {
        if (event->accepted) {
            // The package by its folder: the list may have changed while the question was up.
            TSUCHI_Result removed = TSUCHI_NOT_FOUND;
            for (uint32_t i = 0; i < installed_package_count(); ++i)
                if (!strcmp(installed_package(i)->folder, removing)) { removed = remove_installed_package(i); break; }
            note(removed == TSUCHI_OK ? "Removed. Its mods stop loading when Pikmin 4 restarts." :
                 removed == TSUCHI_BUSY ? "A download is running. Remove the package when it ends." : "The package could not be removed.");
        }
        removing[0] = 0;
    }
    else if (kind == Question::defaults) {
        if (event->accepted) {
            if (manager.defaults() != TSUCHI_OK) note(manager.problem()[0] ? manager.problem() : "The defaults could not be restored.");
            else if (ordering()) {
                ui_mod_list_native_release(current_host); ui_mod_list_native_hold(current_host);
            }
        }
    } else if (kind == Question::save) {
        // The two writers commit independently. A failed writer leaves only its
        // own draft open; a retry must not rewrite choices that already saved.
        if (event->accepted) {
            const bool native_changed = manager.changed(), external_changed = external_mods().changed();
            const auto native_result = native_changed ? manager.save(save_mod_choices) : TSUCHI_OK;
            const auto external_result = external_changed ? save_external_mods() : TSUCHI_OK;
            restart_needed = restart_needed || (native_changed && native_result == TSUCHI_OK);
            if (native_result != TSUCHI_OK || external_result != TSUCHI_OK) {
                if (native_result != TSUCHI_OK && external_result != TSUCHI_OK)
                    note("Save failed; both drafts are kept.");
                else if (native_result != TSUCHI_OK)
                    note(external_changed ? "External saved. Native failed; draft kept" :
                        "Native save failed; draft kept.");
                else note(native_changed ? "Native saved. External failed; draft kept" :
                    "External save failed; draft kept.");
                sync(); return;
            }
            if (external_changed) note(native_changed ?
                "Both saved; apply requests with manager." :
                "Requests saved; apply with manager.");
            else note("Choices saved; restart Pikmin 4.");
        } else {
            if (manager.begin(mod_choices()) != TSUCHI_OK) {
                note("Discard failed; drafts are kept."); sync(); return;
            }
            external_mods().discard(); ordering();
        }
        leave(current_host);
        return;
    }
    sync();
}
bool selected_action(uint64_t host, uint32_t index, uint32_t action, bool requested) {
    if (!live() || host != current_host || asking != Question::none || screen_up()) return false;
    if (action == 5) {
        if (external_mods().unread()) { note("Saved requests unread; reopen Mods."); return true; }
        if (manager.changed() || external_mods().changed()) ask(Question::save, external_mods().changed() ?
            "Save choices and external requests?" :
            "Save changes for the next restart?");
        else leave(host);
        return true;
    }
    if (action == 7) { ask(Question::defaults, "Restore default mod activation and order?"); return true; }
    if (index >= rows.count) {
        // A mod's entry: Confirm chooses it, and its mod hears of it at the next frame.
        if (action == 4 && index - rows.count < shown_entries) {
            const Entry& entry = entries[index - rows.count];
            if (entry.mod && mod_usable(entry.mod) && chosen.reserve(chosen.count + 1)) {
                Chosen pick{entry.mod, {}};
                memcpy(pick.id, entry.id, sizeof pick.id);
                chosen.add(pick);
            }
        }
        return true;
    }
    if (rows[index].external) {
        auto& record = *rows[index].external;
        if (action == 2 || action == 3) {
            if (record.manageable && external_mods().unread())
                note("Saved requests unread; reopen Mods.");
            else if (!record.manageable || external_mods().request(record.id, requested) != TSUCHI_OK)
                note("Cannot change; see Details.");
            sync();
        } else if (action == 102) show_external_detail(record);
        return true;
    }
    auto& record = *rows[index].native;
    if (action == 2 || action == 3) {
        const auto result = manager.propose(record.id, requested);
        if (result == TSUCHI_OK) { if (manager.pending()) review(); else sync(); }
        else note(manager.problem()[0] ? manager.problem() : "This mod cannot be enabled.");
    } else if (action == 4 && has_visible_settings(record.settings)) {
        TSUCHI_Screen screen{};
        if (ui_mod_settings(record, &screen) == TSUCHI_OK) {
            if (live() && ui::table.hold_open(frontend_mod, screen) == TSUCHI_OK) settings = screen;
            else ui::table.destroy(frontend_mod, screen);
        }
    } else if (action == 102) show_detail(record);
    return true;
}
TSUCHI_Result apply(const Hold& hold, HoldNothing&, const HoldNothing&, bool fresh) {
    if (!fresh) return TSUCHI_OK;
    const auto result = manager.begin(mod_choices());
    if (result != TSUCHI_OK) return result;
    const auto external_result = load_external_mods();
    current_host = hold.object; previous_buttons = UINT64_MAX;
    if (!ordering()) { current_host = 0; manager.cleanup(); return TSUCHI_NO_MEMORY; }
    ++tab_input_session; tab_session_traces = 0;
    if (external_mods().unread()) note("Saved requests unread; reopen Mods.");
    else if (external_result != TSUCHI_OK) note(external_overflow ?
        "Load failed; menu full. Use manager." :
        "External data could not be loaded.");
    else if (external_overflow) note("Menu full; use computer manager.");
    return TSUCHI_OK;
}
bool keep(const Hold&, HoldNothing&) { return live(); }
void restore(const Hold& hold, HoldNothing&, HoldEnd) {
    if (current_host != hold.object) return;
    current_host = 0; asking = Question::none; question = {}; previous_buttons = UINT64_MAX;
    close_detail(); cheats.close(); if (settings.id && mod_usable(frontend_mod)) ui::table.destroy(frontend_mod, settings);
    settings = {}; selected[0] = 0; rows.clear(); native_rows = 0; manager.cleanup(); external_mods().discard();
    shown_entries = 0; leaving = false;
}
Holds<HoldNothing, HoldNothing, apply, keep, restore> sessions{"ui.mods", HoldOn::object, true};
void ready(uint64_t host) {
    if (!frontend_mod || !mod_usable(frontend_mod)) return;
    if (current_host && current_host != host) sessions.release(frontend_mod, current_host, 0);
    sessions.take(frontend_mod, host, 0, {});
    // On its own the page shows the list from its first picture, and the game's settings never.
    if (ui_mod_page_alone() && current_host == host && !ui_mod_list_native_active(host)) ui_mod_list_native_hold(host);
}
void lost(uint64_t host) { if (current_host == host) sessions.release(frontend_mod, host, 0); }
bool dispatch(uint64_t host, uint32_t action, bool& handled, bool& result) {
    if (!live() || host != current_host || asking != Question::none) return false;
    if (leaving && action == 5) return false;   // the game's own Back, which closes the page
    if (action == 100 || action == 101) {
        // On its own the page has no other tab to go to.
        handled = true; result = ui_mod_page_alone() || ui_mod_list_native_cycle(host, action == 101) == TSUCHI_OK;
        return true;
    }
    if (ui_active_screen()) return false;
    return ui_mod_list_native_dispatch(host, action, handled, result);
}
void buttons(uint64_t held) {
    namespace t = game::table;
    const uint64_t previous = previous_buttons, edges = held & ~previous;
    previous_buttons = held; ++tab_input_sample;
    const bool frontend_live = live();
    if (leaving && frontend_live && asking == Question::none && !screen_up() && !dialog_up()) {
        // Sent until the menu takes it in: a message over the page keeps the menu's input for a while.
        if (ui_menu_action(current_host, 5)) leaving = false;
        return;
    }
    const bool dialog = frontend_live && dialog_up();
    if (dialog) confirm_spent = true;
    else if (!(held & t::constant::ui_confirm_button)) confirm_spent = false;
    const uint32_t gates = uint32_t(!frontend_live) | (asking != Question::none ? 2u : 0u) |
        (detail.id ? 4u : 0u) | (settings.id ? 8u : 0u) | (cheats.showing() ? 16u : 0u) | (dialog ? 32u : 0u);
    const bool tracing = current_host && tab_session_traces < 12 && tab_input_traces < 48 &&
        (previous == UINT64_MAX || ((held | previous) & t::constant::ui_next_tab_button));
    TabInputTrace trace{};
    if (tracing) {
        // Reserve before native callbacks; a replacement session must not charge
        // this old sample to its own budget. Logging follows all normal actions.
        ++tab_session_traces; ++tab_input_traces;
        const uint64_t object = game::from_handle(current_host);
        trace = {cost_now(), tab_input_sample, tab_input_session, current_host, held, previous, edges,
            object ? game::read_or<uint8_t>(object + t::ui_host::state, 255) : 255u, gates, 0, frontend_live};
    }
    if (!gates) {
        uint32_t action = 0;
        if (edges & t::constant::ui_next_tab_button) action = 100;
        else if (edges & t::constant::ui_previous_tab_button) action = 101;
        else if ((edges & t::constant::ui_details_button) && ui_mod_list_native_active(current_host)) action = 102;
        else if ((edges & t::constant::ui_confirm_button) && !confirm_spent && ui_mod_list_native_active(current_host)) action = 4;
        if (action) {
            const bool result = ui_menu_action(current_host, action);
            trace.action = action; trace.attempted = true; trace.result = result;
        }
    }
    if (tracing) tab_input_trace(trace);
}
// The mods' entries on the page, through the calls that add an entry to one of the game's menus. Those of the
// other menus go on to their own adapter.
using HoldEntry = TSUCHI_Result (*)(TSUCHI_Mod*, TSUCHI_Menu, const char*, const char*, const char*);
using ReleaseEntry = TSUCHI_Result (*)(TSUCHI_Mod*, TSUCHI_Menu, const char*);
HoldEntry next_hold_entry = nullptr;
ReleaseEntry next_release_entry = nullptr;
bool text_fits(const char* text, size_t most) {
    char16_t units[301]; size_t made = 0;
    return text && strlen(text) <= most && game::utf8_to_units(text, units, 300, made);
}
TSUCHI_Result hold_entry(TSUCHI_Mod* mod, TSUCHI_Menu menu, const char* id, const char* text, const char* description) {
    if (menu != TSUCHI_MENU_MODS) return next_hold_entry ? next_hold_entry(mod, menu, id, text, description) : TSUCHI_UNSUPPORTED;
    if (!mod || !id || !text || !description || !id[0] || !text[0] || !text_fits(id, kEntryIdBytes - 1) ||
        !text_fits(text, kEntryLabelBytes - 1) || !text_fits(description, 300)) return TSUCHI_INVALID;
    uint32_t held = 0;
    for (uint32_t i = 0; i < entries.count; ++i) {
        if (entries[i].mod != mod) { held += entries[i].mod ? 1 : 0; continue; }
        if (strcmp(entries[i].id, id)) return TSUCHI_CAPACITY;   // one entry for each mod
        memcpy(entries[i].label, text, strlen(text) + 1);
        sync();
        return TSUCHI_OK;
    }
    const char* owner = nullptr;
    for (uint32_t i = 0; i < mod_count() && !owner; ++i) if (mod_at(i)->context == mod) owner = mod_at(i)->id;
    if (!owner) return TSUCHI_INVALID;
    if (held >= kEntries || !entries.reserve(entries.count + 1)) return TSUCHI_CAPACITY;
    Entry entry{mod, {}, {}, {}};
    memcpy(entry.id, id, strlen(id) + 1); memcpy(entry.label, text, strlen(text) + 1); memcpy(entry.owner, owner, strlen(owner) + 1);
    entries.add(entry);
    return TSUCHI_OK;
}
TSUCHI_Result release_entry(TSUCHI_Mod* mod, TSUCHI_Menu menu, const char* id) {
    if (menu != TSUCHI_MENU_MODS) return next_release_entry ? next_release_entry(mod, menu, id) : TSUCHI_UNSUPPORTED;
    if (!mod || !id) return TSUCHI_INVALID;
    for (uint32_t i = 0; i < entries.count; ++i)
        if (entries[i].mod == mod && !strcmp(entries[i].id, id)) { entries[i].mod = nullptr; return TSUCHI_OK; }
    return TSUCHI_NOT_FOUND;
}
// Each frame: the cheat screen, and the entries chosen since the last one.
void page_step() {
    cheats_step();
    if (!chosen.count) return;
    Chosen picks[4]; uint32_t count = 0;
    for (uint32_t i = 0; i < chosen.count && count < 4; ++i) picks[count++] = chosen[i];
    chosen.count = 0;
    for (uint32_t i = 0; i < count; ++i)
        if (mod_usable(picks[i].mod)) ui::emit_entry_chosen_to(picks[i].mod, {TSUCHI_MENU_MODS, picks[i].id});
}
// Tsuchi's own entry in the title menu opens the page on its own.
constexpr char kTitleEntry[] = "mods";
void entry_chosen(void*, const TSUCHI_UiEntryChosen* event) {
    if (event->menu != TSUCHI_MENU_TITLE || strcmp(event->entry, kTitleEntry)) return;
    if (ui_mod_page_open() != TSUCHI_OK) mod_note(kFrameworkLogId, "The Mods page did not open from the title menu.");
}
// ui.open_mods: the page on its own from the title menu, and the Mods tab while Settings takes input.
TSUCHI_Result open_mods() {
    if (!frontend_mod || !mod_usable(frontend_mod)) return TSUCHI_NOT_READY;
    if (live() && ui_mod_list_native_host() == current_host) {
        if (asking != Question::none || screen_up() || ui_active_screen()) return TSUCHI_BUSY;
        if (ui_mod_list_native_active(current_host)) return TSUCHI_OK;
        return ui_mod_list_native_show(current_host);
    }
    return ui_mod_page_open();
}
} // namespace
void ui_mod_list_start(TSUCHI_Mod* framework) {
    frontend_mod = framework; restart_needed = false; sessions.start();
    const UiModListNativeCallbacks callbacks{count, row, selected_action, ready, lost};
    ui_mod_list_native_callbacks(&callbacks); ui_menu_dispatcher(dispatch); ui_button_listener(buttons);
    TSUCHI_Subscription subscription{};
    ui::table.on_activated(frontend_mod, activated, nullptr, &subscription);
    ui::table.on_closed(frontend_mod, closed, nullptr, &subscription);
    ui::table.on_choice_changed(frontend_mod, changed, nullptr, &subscription);
    each_frame(page_step);
    dialog::table.on_answered(frontend_mod, answered, nullptr, &subscription);
    ui_mod_list_native_start(frontend_mod);
    auto& backend = ui_screen_backend();
    if (backend.hold_menu_entry != hold_entry) {
        next_hold_entry = backend.hold_menu_entry; next_release_entry = backend.release_menu_entry;
        backend.hold_menu_entry = hold_entry; backend.release_menu_entry = release_entry;
    }
    backend.open_mods = open_mods;
    ui::set_backend(&backend);
    // The title menu's own adapter takes the entry where this build has one.
    if (next_hold_entry && ui_mod_page_supported() && ui::table.hold_menu_entry(frontend_mod, TSUCHI_MENU_TITLE, kTitleEntry, "Mods",
            "Turn mods on or off and change their settings.") == TSUCHI_OK)
        ui::table.on_entry_chosen(frontend_mod, entry_chosen, nullptr, &subscription);
}
} // namespace tsuchi
