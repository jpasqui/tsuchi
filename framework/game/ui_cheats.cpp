#include "ui_cheats.h"
#include "ui_service.h"
#include "ui_screens.h"
#include <string.h>
namespace tsuchi {
namespace {
void copy(char* out, size_t room, const char* text) { size_t n = strlen(text); if (n >= room) n = room - 1; memcpy(out, text, n); out[n] = 0; }
void row_id(uint32_t index, char (&out)[8]) { memcpy(out, "cheat0", 7); out[5] = char('0' + index); }
bool find(const char* mod, const char* name, CheatEntry& out) {
    for (uint32_t i = 0; i < cheat_count(); ++i) if (cheat_at(i, out) && !strcmp(mod, out.mod_id) && !strcmp(name, out.name)) return true;
    return false;
}
}
bool mod_has_cheats(const char* mod) {
    CheatEntry entry{};
    for (uint32_t i = 0; i < cheat_count(); ++i) if (cheat_at(i, entry) && !strcmp(mod, entry.mod_id)) return true;
    return false;
}
CheatMenu::Snapshot CheatMenu::collect(uint32_t page) const {
    Snapshot out{}; out.page = page; CheatEntry entry{};
    for (uint32_t i = 0; i < cheat_count(); ++i) {
        if (!cheat_at(i, entry) || strcmp(mod_id_, entry.mod_id)) continue;
        const uint32_t at = out.total++;
        if (at < page * page_size || out.count == page_size) continue;
        auto& row = out.rows[out.count++];
        copy(row.name, sizeof row.name, entry.name); copy(row.problem, sizeof row.problem, entry.problem); row.on = entry.on;
    }
    if (page && page * page_size >= out.total) return collect(out.total ? (out.total - 1) / page_size : 0);
    return out;
}
TSUCHI_Result CheatMenu::rebuild(const Snapshot& next, const char* focus) {
    TSUCHI_Screen fresh{};
    auto result = ui::table.create(owner_, "Cheats", &fresh);
    if (result == TSUCHI_OK) result = ui::table.add_button(owner_, fresh, "back", "Back");
    for (uint32_t i = 0; result == TSUCHI_OK && i < next.count; ++i) {
        char id[8]; row_id(i, id); const auto& row = next.rows[i];
        result = row.problem[0] ? ui::table.add_text(owner_, fresh, id, row.name, row.problem) :
            ui::table.add_choice(owner_, fresh, id, row.name, "[{\"id\":\"off\",\"label\":\"Off\"},{\"id\":\"on\",\"label\":\"On\"}]", row.on ? "on" : "off");
    }
    if (result == TSUCHI_OK && !next.count) result = ui::table.add_text(owner_, fresh, "empty", "Cheats", "No cheats are loaded for this mod and game build. Enable the mod and restart after installing or updating it.");
    if (result == TSUCHI_OK) result = ui::table.add_text(owner_, fresh, "status", "Status", message_);
    if (result == TSUCHI_OK) result = ui::table.add_text(owner_, fresh, "help", "Turning off", "Off restores patched code. Earlier memory changes remain.");
    if (result == TSUCHI_OK && next.page) result = ui::table.add_button(owner_, fresh, "previous", "Previous cheats");
    if (result == TSUCHI_OK && (next.page + 1) * page_size < next.total) result = ui::table.add_button(owner_, fresh, "next", "More cheats");
    // A trailing button lets a host that scrolls to the selection reveal the last explanation.
    if (result == TSUCHI_OK && next.total <= page_size) result = ui::table.add_button(owner_, fresh, "done", "Back");
    if (result != TSUCHI_OK) { if (fresh.id) ui::table.destroy(owner_, fresh); return result; }
    // One screen shows at a time, so the old one leaves first. Its identity changes before anything is
    // destroyed: events queued from the old view are stale.
    const auto old = screen_;
    if (old.id && reopen_frames_ == 0) ui::table.release_open(owner_, old);
    result = ui::table.hold_open(owner_, fresh);
    if (result == TSUCHI_OK) {
        screen_ = fresh; shown_ = next; reopen_frames_ = 0;
        if (old.id) ui::table.destroy(owner_, old);
        if (focus) ui_screen_focus(screen_, focus);
        return TSUCHI_OK;
    }
    // The game refused the new screen. The old one, if it shows again, keeps the menu as it was; otherwise the
    // new one waits and is tried once a frame. Either way the player is not left without a screen.
    if (old.id && reopen_frames_ == 0 && ui::table.hold_open(owner_, old) == TSUCHI_OK) {
        ui::table.destroy(owner_, fresh);
        copy(message_, sizeof message_, "The list could not be refreshed. Go back and open it again.");
        ui::table.set_text(owner_, screen_, "status", message_);
        return TSUCHI_OK;
    }
    screen_ = fresh; shown_ = next; reopen_frames_ = kReopenFrames;
    if (old.id) ui::table.destroy(owner_, old);
    return old.id ? TSUCHI_OK : result;
}
TSUCHI_Result CheatMenu::open(TSUCHI_Mod* owner, const char* mod) {
    if (!owner || !mod || !mod[0] || strlen(mod) >= sizeof mod_id_) return TSUCHI_INVALID;
    close();
    owner_ = owner; copy(mod_id_, sizeof mod_id_, mod); message_[0] = 0; shown_ = {};
    const auto result = rebuild(collect(0));
    if (result != TSUCHI_OK) close();
    return result;
}
void CheatMenu::close() {
    const auto old = screen_;
    screen_ = {}; reopen_frames_ = 0;
    if (old.id && owner_) ui::table.destroy(owner_, old);
}
void CheatMenu::refresh() {
    if (!screen_.id) return;
    if (reopen_frames_) {
        if (ui::table.hold_open(owner_, screen_) == TSUCHI_OK) {
            reopen_frames_ = 0;
        } else if (--reopen_frames_ == 0) {
            close();
            return;
        } else return;
    }
    const auto next = collect(shown_.page);
    bool changed = next.count != shown_.count || next.total != shown_.total || next.page != shown_.page;
    for (uint32_t i = 0; i < next.count && !changed; ++i) changed = strcmp(next.rows[i].name, shown_.rows[i].name) || strcmp(next.rows[i].problem, shown_.rows[i].problem);
    if (changed) {
        char focus[TSUCHI_ID_SIZE]{}; const auto* active = ui_active_screen();
        if (active && active->id == screen_.id && active->selected < active->controls.count) copy(focus, sizeof focus, active->controls[active->selected].id);
        // Keep a failed attempt's explanation visible when the runtime turns it off.
        for (uint32_t i = 0; i < next.count; ++i) if (next.rows[i].problem[0]) { copy(message_, sizeof message_, "Some cheats are unavailable; their reasons are shown above."); break; }
        rebuild(next, focus[0] ? focus : nullptr);
        return;
    }
    for (uint32_t i = 0; i < next.count; ++i)
        if (!next.rows[i].problem[0] && next.rows[i].on != shown_.rows[i].on) { char id[8]; row_id(i, id); ui::table.set_choice(owner_, screen_, id, next.rows[i].on ? "on" : "off"); }
    shown_ = next;
}
void CheatMenu::activate(const TSUCHI_UiActivated& event) {
    if (event.screen.id != screen_.id) return;
    if (!strcmp(event.control, "back") || !strcmp(event.control, "done")) { close(); return; }
    if (!strcmp(event.control, "next") && (shown_.page + 1) * page_size < shown_.total) { message_[0] = 0; rebuild(collect(shown_.page + 1)); }
    else if (!strcmp(event.control, "previous") && shown_.page) { message_[0] = 0; rebuild(collect(shown_.page - 1)); }
}
void CheatMenu::change(const TSUCHI_UiChoiceChanged& event) {
    if (event.screen.id != screen_.id || (strcmp(event.choice, "on") && strcmp(event.choice, "off"))) return;
    for (uint32_t i = 0; i < shown_.count; ++i) {
        char id[8]; row_id(i, id); if (strcmp(id, event.control)) continue;
        // A frame may have reordered or removed entries since this row was drawn.
        CheatEntry current{}; const char* name = shown_.rows[i].name; const bool desired = !strcmp(event.choice, "on");
        if (!find(mod_id_, name, current)) copy(message_, sizeof message_, "This cheat is no longer loaded.");
        else if (current.problem[0]) copy(message_, sizeof message_, current.problem);
        else {
            const auto result = set_cheat(mod_id_, name, desired);
            const bool exists = find(mod_id_, name, current);
            const bool applied = exists && current.on == desired;
            const char* text = result == TSUCHI_OK ? (desired ? "On. The cheat runs from the next frame." : "Off. Prior memory changes remain.") :
                !exists ? "This cheat is no longer loaded." : current.problem[0] ? current.problem :
                applied ? "Changed for this session, but the choice could not be saved. It may revert next launch." :
                result == TSUCHI_CAPACITY ? "Too many active cheat instructions. The combined limit is 1,024 words." : "The cheat could not be changed.";
            copy(message_, sizeof message_, text);
        }
        ui::table.set_text(owner_, screen_, "status", message_);
        // The UI already moved its selector. Reconcile it even when runtime state
        // did not change (capacity, save failure, or an entry removed this frame).
        if (find(mod_id_, name, current) && !current.problem[0]) ui::table.set_choice(owner_, screen_, id, current.on ? "on" : "off");
        refresh(); return;
    }
}
}
