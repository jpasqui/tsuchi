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
enum class Question { none, dependency, save, defaults };
Question asking = Question::none;
bool restart_needed = false, detail_external = false, external_overflow = false;
char selected[kModIdBytes]{}, caption[641]{}, status[641]{};
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
    for (uint32_t i = 0; i < external_mods().count(); ++i) {
        auto* record = external_mods().at(i);
        if (find_mod(record->id)) {
            record->manageable = false;
            record->requested = record->saved = record->observed == ExternalModState::enabled;
            if (!record->problem.set("A Tsuchi mod uses this ID. Import this external package with another ID.")) return false;
        }
        if (rows.count < game::table::constant::ui_mod_list_rows_limit) {
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
uint32_t count() { return rows.count; } // the mods alone: B offers to save or discard a draft
bool row(uint32_t index, UiModListRow& out) {
    if (!live()) return false;
    if (index < rows.count) {
        if (rows[index].native) {
            auto& record = *rows[index].native; plain(record.name.get()[0] ? record.name.get() : record.id, caption, 300);
            plain(description(record), status, 300);
            out = {record.id, caption, manager.draft().enabled(record.id), status, record.settings.list.count != 0};
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
    if (!live() || detail.id || settings.id) return;
    TSUCHI_Screen screen{}; plain(record.name.get(), caption);
    TSUCHI_Result result = ui::table.create(frontend_mod, caption[0] ? caption : record.id, &screen);
    plain(description(record), status);
    if (result == TSUCHI_OK) result = ui::table.add_text(frontend_mod, screen, "id", "ID", record.id);
    if (result == TSUCHI_OK) result = ui::table.add_text(frontend_mod, screen, "status", "Status", status);
    if (result == TSUCHI_OK && record.problem.get()[0]) {
        plain(record.problem.get(), status); result = ui::table.add_text(frontend_mod, screen, "reason", "Reason", status);
    }
    if (result == TSUCHI_OK) result = ui::table.add_button(frontend_mod, screen, "earlier", "Move earlier");
    if (result == TSUCHI_OK) result = ui::table.add_button(frontend_mod, screen, "later", "Move later");
    if (result == TSUCHI_OK) result = ui::table.hold_open(frontend_mod, screen);
    if (result == TSUCHI_OK && live()) { detail = screen; memcpy(selected, record.id, strlen(record.id) + 1); }
    else if (screen.id && mod_usable(frontend_mod)) ui::table.destroy(frontend_mod, screen);
}
void show_external_detail(const ExternalModEntry& record) {
    if (!live() || detail.id || settings.id) return;
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
    if (!live() || event->screen.id != detail.id || detail_external) return;
    auto* record = find_mod(selected); if (!record) { close_detail(); return; }
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
void closed(void*, const TSUCHI_UiClosed* event) {
    if (event->screen.id == detail.id) close_detail();
    if (event->screen.id == settings.id) settings = {};
}
void answered(void*, const TSUCHI_DialogAnswered* event) {
    if (!live() || event->dialog.id != question.id || asking == Question::none) return;
    const auto kind = asking; asking = Question::none; question = {};
    if (kind == Question::dependency) { if (event->accepted) manager.confirm(); else manager.cancel(); }
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
        ui_mod_list_native_release(current_host);
        return;
    }
    sync();
}
bool selected_action(uint64_t host, uint32_t index, uint32_t action, bool requested) {
    if (!live() || host != current_host || asking != Question::none || detail.id || settings.id) return false;
    if (action == 5) {
        if (external_mods().unread()) { note("Saved requests unread; reopen Mods."); return true; }
        if (manager.changed() || external_mods().changed()) ask(Question::save, external_mods().changed() ?
            "Save choices and external requests?" :
            "Save changes for the next restart?");
        else ui_mod_list_native_release(host);
        return true;
    }
    if (action == 7) { ask(Question::defaults, "Restore default mod activation and order?"); return true; }
    if (index >= rows.count) return true;
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
    } else if (action == 4 && record.settings.list.count) {
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
    close_detail(); if (settings.id && mod_usable(frontend_mod)) ui::table.destroy(frontend_mod, settings);
    settings = {}; selected[0] = 0; rows.clear(); native_rows = 0; manager.cleanup(); external_mods().discard();
}
Holds<HoldNothing, HoldNothing, apply, keep, restore> sessions{"ui.mods", HoldOn::object, true};
void ready(uint64_t host) {
    if (!frontend_mod || !mod_usable(frontend_mod)) return;
    if (current_host && current_host != host) sessions.release(frontend_mod, current_host, 0);
    sessions.take(frontend_mod, host, 0, {});
}
void lost(uint64_t host) { if (current_host == host) sessions.release(frontend_mod, host, 0); }
bool dispatch(uint64_t host, uint32_t action, bool& handled, bool& result) {
    if (!live() || host != current_host || asking != Question::none) return false;
    if (action == 100 || action == 101) {
        handled = true; result = ui_mod_list_native_cycle(host, action == 101) == TSUCHI_OK;
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
    const uint32_t gates = uint32_t(!frontend_live) | (asking != Question::none ? 2u : 0u) |
        (detail.id ? 4u : 0u) | (settings.id ? 8u : 0u);
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
        else if ((edges & t::constant::ui_confirm_button) && ui_mod_list_native_active(current_host)) action = 4;
        if (action) {
            const bool result = ui_menu_action(current_host, action);
            trace.action = action; trace.attempted = true; trace.result = result;
        }
    }
    if (tracing) tab_input_trace(trace);
}
} // namespace
void ui_mod_list_start(TSUCHI_Mod* framework) {
    frontend_mod = framework; restart_needed = false; sessions.start();
    const UiModListNativeCallbacks callbacks{count, row, selected_action, ready, lost};
    ui_mod_list_native_callbacks(&callbacks); ui_menu_dispatcher(dispatch); ui_button_listener(buttons);
    TSUCHI_Subscription subscription{};
    ui::table.on_activated(frontend_mod, activated, nullptr, &subscription);
    ui::table.on_closed(frontend_mod, closed, nullptr, &subscription);
    dialog::table.on_answered(frontend_mod, answered, nullptr, &subscription);
    ui_mod_list_native_start(frontend_mod);
}
} // namespace tsuchi
