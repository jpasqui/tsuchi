#include "ui_screens.h"
#include "ui_native.h"
#include "ui_rich_text.h"
#include "ui_service.h"
#include "files.h"
#include "mods.h"
#include "holds.h"
#include "frame.h"
#include "ids.h"
#include "json.h"
#include "reflect.h"
#include <string.h>
namespace tsuchi {
namespace {
struct Definition { UiScreen* screen; };
struct Showing { uint64_t id; void* view; UiRenderer* renderer = nullptr; bool accepted = false; bool over = false; };
UiRenderer renderer{}, title_renderer{};
bool engine_listening = false;
uint64_t next_id = 1;
uint32_t tab_revision = 1;
struct Notice { TSUCHI_Mod* mod; uint64_t screen; uint8_t kind; char control[TSUCHI_ID_SIZE], choice[TSUCHI_ID_SIZE], previous_choice[TSUCHI_ID_SIZE]; int64_t integer, previous_integer; };
List<Notice> notices;
uint32_t notices_dropped = 0;
void notify(TSUCHI_Mod* mod, uint64_t screen, uint8_t kind, const char* control = "") {
    Notice notice{}; notice.mod = mod; notice.screen = screen; notice.kind = kind;
    memcpy(notice.control, control, strlen(control) + 1);
    if (!notices.add(notice)) ++notices_dropped;
}
void deliver() {
    List<Notice> telling = notices; notices = {};
    for (uint32_t i = 0; i < telling.count; ++i) {
        const Notice& notice = telling[i];
        if (!mod_usable(notice.mod)) continue;
        if (notice.kind == 1) ui::emit_activated_to(notice.mod, {{notice.screen}, notice.control});
        else if (notice.kind == 2) {
            ui::emit_choice_changed_to(notice.mod, {{notice.screen}, notice.control, notice.choice, notice.previous_choice});
            if (mod_usable(notice.mod)) ui::emit_changed_to(notice.mod, {{notice.screen}, notice.control, 1});
        } else if (notice.kind == 3) {
            ui::emit_integer_changed_to(notice.mod, {{notice.screen}, notice.control, notice.integer, notice.previous_integer});
            if (mod_usable(notice.mod)) ui::emit_changed_to(notice.mod, {{notice.screen}, notice.control, 2});
        } else if (notice.kind == 4) ui::emit_closed_to(notice.mod, {{notice.screen}});
        else if (notice.kind == 5) ui::emit_reset_requested_to(notice.mod, {{notice.screen}});
    }
    telling.clear();
}
void clear(UiControl& control) { control.label.clear(); control.text.clear(); control.choices.clear(); control.labels.clear(); }
void free_screen(UiScreen* screen) {
    for (uint32_t i = 0; i < screen->controls.count; ++i) clear(screen->controls[i]);
    screen->controls.clear(); screen->title.clear(); screen->footer.clear(); release(screen);
}
TSUCHI_Result define(const Hold&, Definition& kept, const Definition& wanted, bool fresh) {
    if (!fresh) return TSUCHI_BUSY;
    kept = wanted; return TSUCHI_OK;
}
void undefine(const Hold&, Definition& kept, HoldEnd) { if (kept.screen->settings_tab) ++tab_revision; free_screen(kept.screen); }
Holds<Definition, Definition, define, nullptr, undefine> definitions{"ui.screen", HoldOn::nothing, true};
UiScreen* find(uint64_t id) { auto* definition = definitions.find(0, id); return definition ? definition->screen : nullptr; }
TSUCHI_Result own_screen(TSUCHI_Mod* mod, TSUCHI_Screen handle, UiScreen*& screen) {
    screen = find(handle.id);
    return !screen ? TSUCHI_NOT_FOUND : screen->mod != mod ? TSUCHI_DENIED : TSUCHI_OK;
}
void focus(UiScreen& screen) {
    if (screen.selected < screen.controls.count && screen.controls[screen.selected].kind != UiKind::text) return;
    const uint32_t previous = screen.selected;
    screen.selected = kUiNoSelection;
    for (uint32_t i = 0; i < screen.controls.count; ++i) {
        if (screen.controls[i].kind == UiKind::text) continue;
        screen.selected = i;
        break;
    }
    if (screen.selected != previous) ++screen.revision;
}
bool update_showing(const Hold& hold, Showing& kept) {
    const Showing current = kept;
    if (!current.renderer) return false;
    const UiRenderer captured = *current.renderer;
    const auto guard = hold_call_guard(hold);
    if (!find(current.id) || !captured.live(current.view) || !guard.permits()) return false;
    const UiScreen* screen = find(current.id);
    return screen && captured.update(current.view, *screen) && guard.permits() && find(current.id);
}
void end_showing(const Hold& hold);
TSUCHI_Result show(const Hold& hold, Showing& kept, const Showing& wanted, bool fresh) {
    if (!fresh) {
        if (kept.id != wanted.id) return TSUCHI_BUSY;
        if (update_showing(hold, kept)) return TSUCHI_OK;
        end_showing(hold);
        return TSUCHI_NOT_READY;
    }
    UiScreen* screen = find(wanted.id);
    if (!screen) return TSUCHI_NOT_FOUND;
    if (!screen->controls.count) return TSUCHI_INVALID;
    const UiRenderer opening = screen->native_renderer ? *screen->native_renderer : wanted.over ? title_renderer : renderer;
    if (!opening.open || !opening.update || !opening.live || !opening.close) return TSUCHI_NOT_READY;
    auto* captured = static_cast<UiRenderer*>(allocate(sizeof(UiRenderer)));
    if (!captured) return TSUCHI_NO_MEMORY;
    *captured = opening;
    focus(*screen);
    const auto guard = hold_call_guard(hold);
    void* view = nullptr;
    const TSUCHI_Result result = opening.open(*screen, &view);
    if (result != TSUCHI_OK) { release(captured); return result; }
    if (!guard.permits() || !find(wanted.id)) { opening.close(view); release(captured); return TSUCHI_NOT_READY; }
    // The native model needs this staged association during its first update.
    // The hold operation retains it until callbacks return, including stop.
    kept = {wanted.id, view, captured, false, wanted.over};
    hold_apply_checkpoint(hold);
    if (update_showing(hold, kept)) { kept.accepted = true; return TSUCHI_OK; }
    end_showing(hold);
    return TSUCHI_NOT_READY;
}
bool keep(const Hold& hold, Showing& kept) {
    const Showing current = kept;
    if (!current.renderer) return false;
    const UiRenderer captured = *current.renderer;
    const auto guard = hold_call_guard(hold);
    if (!find(current.id) || !captured.live(current.view) || !guard.permits()) return false;
    const UiScreen* screen = find(current.id);
    // Keep the existing world-step pipeline if engine registration failed.
    // Normal engine maintenance must not update twice on stepping frames.
    return screen && (engine_listening || captured.update(current.view, *screen));
}
void hide(const Hold& hold, Showing& kept, HoldEnd) {
    const Showing closed = kept;
    kept = {};
    const UiRenderer captured = closed.renderer ? *closed.renderer : UiRenderer{};
    release(closed.renderer);
    if (captured.close && closed.view) captured.close(closed.view);
    if (closed.accepted) notify(hold.mod, closed.id, 4);
}
Holds<Showing, Showing, show, keep, hide> showing{"ui.open", HoldOn::nothing, true};
void end_showing(const Hold& hold) {
    if (hold_call_guard(hold).permits() && showing.count() && showing.hold_at(0).id == hold.id)
        showing.release(hold.mod, 0, 0);
}
void update_engine_ui() {
    const auto* kept = showing.find(0, 0);
    if (!kept || !kept->renderer) return;
    const Showing current = *kept;
    ui_sync_screen(current.id, current.view);
}
bool opened(uint64_t id) { const auto* kept = showing.find(0, 0); return kept && kept->id == id; }
bool plain(const char* text, bool empty = false) {
    if (!text || (!*text && !empty) || strlen(text) > 640) return false;
    // Screen titles remain plain. UTF-8 uses the same converter as reflection.
    for (const unsigned char* at = reinterpret_cast<const unsigned char*>(text); *at; ++at)
        if (*at < 32 || *at == '[' || *at == ']') return false;
    char16_t units[641]; size_t count = 0;
    return game::utf8_to_units(text, units, sizeof units / sizeof *units, count);
}
bool glyph_text(const char* text, bool empty = false) {
    if (!ui_glyph_text_syntax(text, empty)) return false;
    char16_t units[kUiTextBytes + 1]; size_t count = 0;
    return game::utf8_to_units(text, units, sizeof units / sizeof *units, count);
}
TSUCHI_Result create(TSUCHI_Mod* mod, const char* title, TSUCHI_Screen* out) {
    if (!plain(title)) return TSUCHI_INVALID;
    auto* screen = static_cast<UiScreen*>(allocate(sizeof(UiScreen)));
    if (!screen) return TSUCHI_NO_MEMORY;
    *screen = {}; screen->mod = mod; screen->id = next_id++; screen->selected = kUiNoSelection;
    if (!screen->title.set(title)) { free_screen(screen); return TSUCHI_NO_MEMORY; }
    const TSUCHI_Result result = definitions.take(mod, 0, screen->id, Definition{screen});
    if (result != TSUCHI_OK) { free_screen(screen); return result; }
    out->id = screen->id; return TSUCHI_OK;
}
TSUCHI_Result destroy(TSUCHI_Mod* mod, TSUCHI_Screen handle) {
    UiScreen* screen = nullptr;
    const auto result = own_screen(mod, handle, screen);
    if (result != TSUCHI_OK) return result;
    if (opened(handle.id)) showing.release(mod, 0, 0);
    return definitions.release(mod, 0, handle.id);
}
TSUCHI_Result add(TSUCHI_Mod* mod, TSUCHI_Screen handle, const char* id, const char* label, UiControl& control) {
    UiScreen* screen = nullptr;
    const auto result = own_screen(mod, handle, screen);
    if (result != TSUCHI_OK) return result;
    if (opened(handle.id)) return TSUCHI_BUSY;
    if (!id_valid(id) || !glyph_text(label)) return TSUCHI_INVALID;
    for (uint32_t i = 0; i < screen->controls.count; ++i) if (!strcmp(screen->controls[i].id, id)) return TSUCHI_INVALID;
    if (!control.label.set(label)) return TSUCHI_NO_MEMORY;
    memcpy(control.id, id, strlen(id) + 1);
    if (!screen->controls.add(control)) return TSUCHI_NO_MEMORY;
    if (control.kind == UiKind::choice || control.kind == UiKind::integer) screen->resettable = true;
    control = {}; ++screen->revision; return TSUCHI_OK;
}
TSUCHI_Result add_button(TSUCHI_Mod* mod, TSUCHI_Screen handle, const char* id, const char* label) {
    UiControl control{}; control.kind = UiKind::button;
    const auto result = add(mod, handle, id, label, control); clear(control); return result;
}
const char* word(const Json* value) {
    return value && value->kind == Json::Kind::text && value->bytes == strlen(value->text) ? value->text : nullptr;
}
bool keys(const Json& object, const char* const* allowed, uint32_t count) {
    if (object.kind != Json::Kind::object) return false;
    for (const Json* part = object.first; part; part = part->next) {
        bool known = false;
        for (uint32_t i = 0; i < count; ++i) known = known || !strcmp(part->name, allowed[i]);
        if (!known) return false;
    }
    return true;
}
TSUCHI_Result options(UiControl& control, const Json& options, const char* initial) {
    if (options.kind != Json::Kind::list || options.count < 2 || !id_valid(initial)) return TSUCHI_INVALID;
    bool selected = false; uint32_t index = 0;
    const char* const allowed[] = {"id", "label"};
    for (const Json* option = options.first; option; option = option->next, ++index) {
        if (!keys(*option, allowed, 2)) return TSUCHI_INVALID;
        const char* id = word(option->find("id")); const char* label = word(option->find("label"));
        if (!id || !id_valid(id) || !glyph_text(label)) return TSUCHI_INVALID;
        for (const char* old = control.choices.first(); old; old = control.choices.after(old)) if (!strcmp(old, id)) return TSUCHI_INVALID;
        if (!control.choices.add(id) || !control.labels.add(label)) return TSUCHI_NO_MEMORY;
        if (!strcmp(id, initial)) { selected = true; control.selected = index; }
    }
    return selected ? TSUCHI_OK : TSUCHI_NOT_FOUND;
}
TSUCHI_Result add_choice(TSUCHI_Mod* mod, TSUCHI_Screen handle, const char* id, const char* label, const char* json, const char* initial) {
    UiControl control{}; control.kind = UiKind::choice;
    JsonError error{}; const Json* parsed = read_json(json, strlen(json), &error);
    TSUCHI_Result result = parsed ? options(control, *parsed, initial) : TSUCHI_INVALID;
    if (result == TSUCHI_OK) result = add(mod, handle, id, label, control);
    release_json(parsed); clear(control); return result;
}
bool grid(int64_t value, int64_t minimum, int64_t maximum, int64_t step) {
    return minimum <= maximum && step > 0 && value >= minimum && value <= maximum &&
           (uint64_t(value) - uint64_t(minimum)) % uint64_t(step) == 0;
}
TSUCHI_Result add_integer(TSUCHI_Mod* mod, TSUCHI_Screen handle, const char* id, const char* label, int64_t minimum, int64_t maximum, int64_t step, int64_t initial) {
    if (!grid(initial, minimum, maximum, step)) return TSUCHI_INVALID;
    UiControl control{}; control.kind = UiKind::integer;
    control.minimum = minimum; control.maximum = maximum; control.step = step; control.integer = initial;
    const auto result = add(mod, handle, id, label, control); clear(control); return result;
}
TSUCHI_Result add_text(TSUCHI_Mod* mod, TSUCHI_Screen handle, const char* id, const char* label, const char* initial) {
    if (!glyph_text(initial, true)) return TSUCHI_INVALID;
    UiControl control{}; control.kind = UiKind::text;
    if (!control.text.set(initial)) return TSUCHI_NO_MEMORY;
    const auto result = add(mod, handle, id, label, control); clear(control); return result;
}
TSUCHI_Result hold_open(TSUCHI_Mod* mod, TSUCHI_Screen handle) {
    UiScreen* screen = nullptr;
    const auto result = own_screen(mod, handle, screen);
    return result == TSUCHI_OK ? showing.take(mod, 0, 0, Showing{handle.id, nullptr}) : result;
}
TSUCHI_Result hold_open_over(TSUCHI_Mod* mod, TSUCHI_Screen handle, TSUCHI_Menu menu) {
    if (menu != TSUCHI_MENU_TITLE) return TSUCHI_INVALID;
    if (!title_renderer.open) return TSUCHI_UNSUPPORTED;
    UiScreen* screen = nullptr;
    const auto result = own_screen(mod, handle, screen);
    return result == TSUCHI_OK ? showing.take(mod, 0, 0, Showing{handle.id, nullptr, nullptr, false, true}) : result;
}
TSUCHI_Result release_open(TSUCHI_Mod* mod, TSUCHI_Screen handle) {
    UiScreen* screen = nullptr;
    const auto result = own_screen(mod, handle, screen);
    if (result != TSUCHI_OK) return result;
    return opened(handle.id) ? showing.release(mod, 0, 0) : TSUCHI_NOT_FOUND;
}
TSUCHI_Result is_open(TSUCHI_Mod* mod, TSUCHI_Screen handle, bool* out) {
    UiScreen* screen = nullptr; const auto result = own_screen(mod, handle, screen);
    if (result != TSUCHI_OK) return result;
    *out = opened(handle.id); return TSUCHI_OK;
}
UiControl* find_control(UiScreen* screen, const char* id, UiKind kind) {
    if (screen) for (uint32_t i = 0; i < screen->controls.count; ++i) {
        auto& control = screen->controls[i];
        if (!strcmp(control.id, id) && control.kind == kind) return &control;
    }
    return nullptr;
}
const char* at_word(const Texts& texts, uint32_t index) {
    const char* text = texts.first();
    while (text && index--) text = texts.after(text);
    return text;
}
TSUCHI_Result give(const char* text, char* buffer, uint64_t capacity, uint64_t* out) {
    *out = strlen(text) + 1;
    if (capacity < *out) return TSUCHI_CAPACITY;
    memcpy(buffer, text, *out); return TSUCHI_OK;
}
TSUCHI_Result glyph(TSUCHI_GlyphButton button, char* buffer, uint64_t capacity, uint64_t* out) {
    const char* token = ui_glyph_token(button);
    return token ? give(token, buffer, capacity, out) : TSUCHI_INVALID;
}
TSUCHI_Result set_footer(TSUCHI_Mod* mod, TSUCHI_Screen handle, const char* value) {
    UiScreen* screen = nullptr; const auto result = own_screen(mod, handle, screen);
    if (result != TSUCHI_OK) return result;
    if (!glyph_text(value, true)) return TSUCHI_INVALID;
    if (!screen->footer.set(value)) return TSUCHI_NO_MEMORY;
    ++screen->revision; return TSUCHI_OK;
}
TSUCHI_Result choice(TSUCHI_Mod* mod, TSUCHI_Screen handle, const char* id, char* buffer, uint64_t capacity, uint64_t* out) {
    UiScreen* screen = nullptr; const auto result = own_screen(mod, handle, screen);
    if (result != TSUCHI_OK) return result;
    auto* control = find_control(screen, id, UiKind::choice);
    return control ? give(at_word(control->choices, control->selected), buffer, capacity, out) : TSUCHI_NOT_FOUND;
}
TSUCHI_Result integer(TSUCHI_Mod* mod, TSUCHI_Screen handle, const char* id, int64_t* out) {
    UiScreen* screen = nullptr; const auto result = own_screen(mod, handle, screen);
    if (result != TSUCHI_OK) return result;
    auto* control = find_control(screen, id, UiKind::integer);
    if (!control) return TSUCHI_NOT_FOUND;
    *out = control->integer; return TSUCHI_OK;
}
TSUCHI_Result text(TSUCHI_Mod* mod, TSUCHI_Screen handle, const char* id, char* buffer, uint64_t capacity, uint64_t* out) {
    UiScreen* screen = nullptr; const auto result = own_screen(mod, handle, screen);
    if (result != TSUCHI_OK) return result;
    auto* control = find_control(screen, id, UiKind::text);
    return control ? give(control->text.get(), buffer, capacity, out) : TSUCHI_NOT_FOUND;
}
TSUCHI_Result set_choice(TSUCHI_Mod* mod, TSUCHI_Screen handle, const char* id, const char* value) {
    UiScreen* screen = nullptr; const auto result = own_screen(mod, handle, screen);
    if (result != TSUCHI_OK) return result;
    auto* control = find_control(screen, id, UiKind::choice); if (!control) return TSUCHI_NOT_FOUND;
    uint32_t i = 0;
    for (const char* option = control->choices.first(); option; option = control->choices.after(option), ++i)
        if (!strcmp(option, value)) { control->selected = i; ++screen->revision; return TSUCHI_OK; }
    return TSUCHI_NOT_FOUND;
}
TSUCHI_Result set_integer(TSUCHI_Mod* mod, TSUCHI_Screen handle, const char* id, int64_t value) {
    UiScreen* screen = nullptr; const auto result = own_screen(mod, handle, screen);
    if (result != TSUCHI_OK) return result;
    auto* control = find_control(screen, id, UiKind::integer); if (!control) return TSUCHI_NOT_FOUND;
    if (!grid(value, control->minimum, control->maximum, control->step)) return TSUCHI_INVALID;
    control->integer = value; ++screen->revision; return TSUCHI_OK;
}
TSUCHI_Result set_text(TSUCHI_Mod* mod, TSUCHI_Screen handle, const char* id, const char* value) {
    UiScreen* screen = nullptr; const auto result = own_screen(mod, handle, screen);
    if (result != TSUCHI_OK) return result;
    auto* control = find_control(screen, id, UiKind::text); if (!control) return TSUCHI_NOT_FOUND;
    if (!glyph_text(value, true)) return TSUCHI_INVALID;
    if (!control->text.set(value)) return TSUCHI_NO_MEMORY;
    ++screen->revision; return TSUCHI_OK;
}
// The file form calls the same builders as the code form, then publishes the
// complete screen. A malformed row destroys everything made for that screen.
TSUCHI_Result load(TSUCHI_Mod* mod, const char* path, TSUCHI_Screen* out) {
    Texts where;
    if (!mod_file_path(where, mod, path) || !game::path_stays_within(path)) { where.clear(); return TSUCHI_INVALID; }
    char* bytes = nullptr; uint64_t size = 0;
    TSUCHI_Result result = game::read_whole_file(where.data, bytes, size); where.clear();
    if (result != TSUCHI_OK) return result;
    JsonError error{}; const Json* document = read_json(bytes, size, &error); release(bytes);
    const char* const document_keys[] = {"title", "controls", "footer"};
    const Json* controls = document ? document->find("controls") : nullptr;
    if (!document || !keys(*document, document_keys, 3) || !controls || controls->kind != Json::Kind::list) result = TSUCHI_INVALID;
    TSUCHI_Screen handle{};
    if (result == TSUCHI_OK) result = create(mod, word(document->find("title")), &handle);
    if (result == TSUCHI_OK && document->find("footer")) result = set_footer(mod, handle, word(document->find("footer")));
    for (const Json* row = result == TSUCHI_OK ? controls->first : nullptr; row && result == TSUCHI_OK; row = row->next) {
        const char* const allowed[] = {"id", "kind", "label", "initial", "options", "minimum", "maximum", "step"};
        if (!keys(*row, allowed, 8)) { result = TSUCHI_INVALID; break; }
        const char* id = word(row->find("id")); const char* label = word(row->find("label")); const char* kind = word(row->find("kind"));
        if (!id || !kind || !label) { result = TSUCHI_INVALID; break; }
        if (!strcmp(kind, "button")) result = add_button(mod, handle, id, label);
        else if (!strcmp(kind, "text")) result = add_text(mod, handle, id, label, word(row->find("initial")));
        else if (!strcmp(kind, "choice")) {
            UiControl control{}; control.kind = UiKind::choice;
            const Json* values = row->find("options");
            result = values ? options(control, *values, word(row->find("initial"))) : TSUCHI_INVALID;
            if (result == TSUCHI_OK) result = add(mod, handle, id, label, control);
            clear(control);
        } else if (!strcmp(kind, "integer")) {
            const Json* minimum = row->find("minimum"), *maximum = row->find("maximum"), *step = row->find("step"), *initial = row->find("initial");
            if (!minimum || !maximum || !step || !initial || !minimum->whole || !maximum->whole || !step->whole || !initial->whole) result = TSUCHI_INVALID;
            else result = add_integer(mod, handle, id, label, minimum->integer, maximum->integer, step->integer, initial->integer);
        } else result = TSUCHI_INVALID;
    }
    release_json(document);
    if (result != TSUCHI_OK) { if (handle.id) destroy(mod, handle); return result; }
    *out = handle; return TSUCHI_OK;
}
TSUCHI_Result hold_tab(TSUCHI_Mod* mod, TSUCHI_Screen handle) {
    UiScreen* screen = nullptr; const auto result = own_screen(mod, handle, screen);
    if (result != TSUCHI_OK) return result;
    if (!screen->controls.count) return TSUCHI_INVALID;
    if (!screen->settings_tab && ui_settings_tab_count() >= 6) return TSUCHI_CAPACITY;
    if (!screen->settings_tab) { screen->settings_tab = true; ++tab_revision; }
    return TSUCHI_OK;
}
TSUCHI_Result release_tab(TSUCHI_Mod* mod, TSUCHI_Screen handle) {
    UiScreen* screen = nullptr; const auto result = own_screen(mod, handle, screen);
    if (result != TSUCHI_OK) return result;
    if (!screen->settings_tab) return TSUCHI_NOT_FOUND;
    screen->settings_tab = false; ++tab_revision;
    if (opened(handle.id)) showing.release(mod, 0, 0);
    return TSUCHI_OK;
}
ui::Backend backend{create, load, destroy, add_button, add_choice, add_integer, add_text, hold_open, release_open, is_open, choice, set_choice, integer, set_integer, text, set_text, hold_tab, release_tab,
    nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
    glyph, set_footer, nullptr, hold_open_over, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
    nullptr, nullptr, nullptr};
} // namespace
ui::Backend& ui_screen_backend() { return backend; }
void ui_screen_resettable(TSUCHI_Screen handle) { if (auto* screen = find(handle.id)) { screen->resettable = true; ++screen->revision; } }
bool ui_screen_focus(TSUCHI_Screen handle, const char* control) {
    auto* screen=find(handle.id);if(!screen)return false;
    for(uint32_t i=0;i<screen->controls.count;++i)if(screen->controls[i].kind!=UiKind::text&&!strcmp(screen->controls[i].id,control)){screen->selected=i;++screen->revision;return true;}
    return false;
}
void ui_start() { ui_native_prepare_input(); definitions.start(); showing.start(); ui::set_backend(&backend); each_frame(deliver); }
uint32_t ui_events_dropped() { return notices_dropped; }
void ui_set_title_renderer(const UiRenderer* value) { title_renderer = value ? *value : UiRenderer{}; }
bool ui_active_over_menu() { const auto* kept = showing.find(0, 0); return kept && kept->over && kept->view; }
void ui_set_renderer(const UiRenderer* value) {
    renderer = value ? *value : UiRenderer{};
    // Native startup registers its layout epoch before publishing the renderer.
    // Keep the order: epoch first, then UI maintenance in that same frame.
    if (value && !engine_listening) engine_listening = each_engine_frame(update_engine_ui);
}
bool ui_sync_screen(uint64_t id, void* view) {
    if (!on_game_thread()) return false;
    const auto* kept = showing.find(0, 0);
    if (!kept || !kept->renderer || kept->id != id || kept->view != view) return false;
    TSUCHI_Mod* owner = showing.holder(0, 0);
    return owner && showing.take(owner, 0, 0, Showing{id, nullptr}) == TSUCHI_OK;
}
const UiScreen* ui_active_screen() { const auto* kept = showing.find(0, 0); return kept ? find(kept->id) : nullptr; }
void* ui_active_view() { const auto* kept = showing.find(0, 0); return kept ? kept->view : nullptr; }
bool ui_route_action(uint64_t host, uint32_t action) {
    if (!on_game_thread()) return false;
    const auto* kept = showing.find(0, 0);
    if (!kept) return false;
    const Showing current = *kept;
    if (!current.renderer) return false;
    const UiRenderer captured = *current.renderer;
    const UiScreen* screen = find(current.id);
    if (!screen || !mod_usable(screen->mod)) return false;
    return captured.action ? captured.action(current.view, host, action) : ui_action(action);
}
void ui_route_buttons(uint64_t held) {
    if (!on_game_thread()) return;
    const auto* kept = showing.find(0, 0);
    if (!kept) return;
    const Showing current = *kept;
    if (!current.renderer) return;
    const UiRenderer captured = *current.renderer;
    const UiScreen* screen = find(current.id);
    if (screen && mod_usable(screen->mod) && captured.buttons) captured.buttons(current.view, held);
}
void ui_host_lost(uint64_t host) {
    if (!on_game_thread()) return;
    const auto* kept = showing.find(0, 0);
    if (!kept) return;
    const Showing current = *kept;
    if (!current.renderer) return;
    const UiRenderer captured = *current.renderer;
    const uint64_t generation = showing.hold_at(0).id;
    TSUCHI_Mod* owner = showing.holder(0, 0);
    if (!owner || !captured.uses_host || !captured.uses_host(current.view, host)) return;
    kept = showing.find(0, 0);
    if (kept && showing.hold_at(0).id == generation && captured.detach_host)
        captured.detach_host(current.view, host);
    kept = showing.find(0, 0);
    if (kept && showing.hold_at(0).id == generation) showing.release(owner, 0, 0);
}
bool ui_action(uint32_t action) {
    if (!on_game_thread()) return false;
    UiScreen* screen = const_cast<UiScreen*>(ui_active_screen());
    if (!screen) return false;
    if (action == 5) { showing.release(screen->mod, 0, 0); return true; }
    if (action == 7) { notify(screen->mod, screen->id, 5); return true; }
    const uint32_t count = screen->controls.count;
    focus(*screen);
    if (screen->selected == kUiNoSelection) return true;
    if (action == 0 || action == 1) {
        uint32_t selected = screen->selected;
        for (uint32_t i = 0; i < count; ++i) {
            selected = action == 0 ? (selected ? selected - 1 : count - 1) : (selected + 1) % count;
            if (screen->controls[selected].kind == UiKind::text) continue;
            if (selected != screen->selected) { screen->selected = selected; ++screen->revision; }
            return true;
        }
        return true;
    }
    UiControl& control = screen->controls[screen->selected];
    if (action == 4 && control.kind == UiKind::button) { notify(screen->mod, screen->id, 1, control.id); return true; }
    if (action != 2 && action != 3 && action != 4) return true;
    const bool previous = action == 2;
    Notice notice{}; notice.mod = screen->mod; notice.screen = screen->id; memcpy(notice.control, control.id, strlen(control.id) + 1);
    if (control.kind == UiKind::choice) {
        const uint32_t old = control.selected;
        const char* before = at_word(control.choices, old); memcpy(notice.previous_choice, before, strlen(before) + 1);
        uint32_t count_options = 0; for (const char* at = control.choices.first(); at; at = control.choices.after(at)) ++count_options;
        if (previous && control.selected) --control.selected;
        else if (!previous && control.selected + 1 < count_options) ++control.selected;
        if (old == control.selected) return true;
        notice.kind = 2; const char* after = at_word(control.choices, control.selected); memcpy(notice.choice, after, strlen(after) + 1);
    } else if (control.kind == UiKind::integer) {
        notice.kind = 3; notice.previous_integer = control.integer;
        const uint64_t distance = previous ? uint64_t(control.integer) - uint64_t(control.minimum) : uint64_t(control.maximum) - uint64_t(control.integer);
        if (distance < uint64_t(control.step)) return true;
        control.integer = previous ? int64_t(uint64_t(control.integer) - uint64_t(control.step)) : int64_t(uint64_t(control.integer) + uint64_t(control.step));
    } else return true;
    ++screen->revision;
    notice.integer = control.integer;
    if (!notices.add(notice)) ++notices_dropped; return true;
}
} // namespace tsuchi

namespace tsuchi {
uint32_t ui_settings_tab_revision() { return tab_revision; }
uint32_t ui_settings_tab_count() {
    uint32_t count = 0;
    for (uint32_t i = 0; i < definitions.count(); ++i) {
        const auto* screen = definitions.kept_at(i).screen;
        if (screen->settings_tab && mod_usable(screen->mod)) ++count;
    }
    return count;
}
const UiScreen* ui_settings_tab(uint32_t index) {
    for (uint32_t i = 0; i < definitions.count(); ++i) {
        const auto* screen = definitions.kept_at(i).screen;
        if (screen->settings_tab && mod_usable(screen->mod) && !index--) return screen;
    }
    return nullptr;
}
TSUCHI_Result ui_settings_tab_open(uint32_t index) {
    const auto* screen = ui_settings_tab(index);
    return screen ? hold_open(screen->mod, {screen->id}) : TSUCHI_NOT_FOUND;
}
void ui_settings_tab_close() {
    const auto* screen = ui_active_screen();
    if (screen && screen->settings_tab) showing.release(screen->mod, 0, 0);
}
}

namespace tsuchi {
bool ui_screen_native_renderer(TSUCHI_Mod* owner,TSUCHI_Screen id,const UiRenderer* renderer){
 auto* screen=find(id.id);if(!on_game_thread()||!screen||screen->mod!=owner||ui_active_screen()==screen)return false;
 screen->native_renderer=renderer;return true;
}
}
