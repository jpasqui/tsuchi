#include "ui_settings.h"
#include "ui_screens.h"
#include "ui_service.h"
#include "actor_events.h"
#include "ordinary_lifecycle.h"
#include <cassert>
#include <cstring>
#include <cstdio>
#include <vector>
#include <string>
#include <map>
using namespace tsuchi;
namespace {
std::vector<void(*)()> steps;
ModRecord record{};
uint32_t changes = 0, prepares = 0, closes = 0, opens = 0;
std::map<std::string, uint32_t> changes_for;
bool host_live = true;
TSUCHI_Mod* framework = nullptr;
uint64_t edit_request = 0;
UiSettingsTextDone edit_done = nullptr;
TSUCHI_Result editor_result = TSUCHI_OK;
bool stop_in_editor = false;
std::string editor_initial;
TSUCHI_Result open_view(const UiScreen&, void** out) { ++opens; *out = reinterpret_cast<void*>(1); return TSUCHI_OK; }
bool update_view(void*, const UiScreen&) { return true; }
bool live_view(void*) { return host_live; }
void close_view(void*) { ++closes; }
UiRenderer renderer{open_view, update_view, live_view, close_view};
void frame() { for (auto step : steps) step(); }
const char* control(const char* label) {
    const UiScreen* screen = ui_active_screen(); assert(screen);
    for (uint32_t i = 0; i < screen->controls.count; ++i)
        if (!std::strcmp(screen->controls[i].label.get(), label)) return screen->controls[i].id;
    assert(false); return "";
}
void choose(TSUCHI_Screen screen, const char* label, const char* value) {
    const char* id = control(label);
    assert(ui::table.set_choice(framework, screen, id, value) == TSUCHI_OK);
    ui::emit_changed_to(framework, {screen, id, 1});
}
void button(TSUCHI_Screen screen, const char* label) { ui::emit_activated({screen, control(label)}); }
TSUCHI_Result editor(const char* title, const char* current, uint32_t maximum, uint64_t request, UiSettingsTextDone done) {
    assert(!std::strcmp(title, "Name") && maximum == 12); editor_initial = current;
    edit_request = request; edit_done = done;
    if (stop_in_editor) { mod_stop(framework); done(request, TSUCHI_OK, "late"); }
    return editor_result;
}
}
namespace tsuchi {
bool ui_native_prepare_input() { return false; }
// This fixture keeps its existing world-step pipeline; engine routing is
// exercised separately by the real Showing scheduling regression.
bool each_engine_frame(void(*)()) { return false; }
bool each_frame(void(*step)()) { steps.push_back(step); return true; }
bool on_actor_left(void(*)(uint64_t,uint64_t,ActorLeft)) { return true; }
void ordinary_lifecycle_listener(void(*)(uint64_t,uint64_t,bool)) {}
TSUCHI_Result ordinary_lifecycle_watch(uint64_t) { return TSUCHI_OK; }
uint64_t ordinary_lifecycle_restore_part(uint64_t part,uint64_t) { return part; }
bool mod_file_path(Texts& out,const TSUCHI_Mod*,const char* path) { return out.add(path); }
ModRecord* find_mod(const char* id) { return !std::strcmp(id, record.id) ? &record : nullptr; }
void settings_player_prepare(ModRecord&) { ++prepares; }
void settings_player_changed(ModRecord& changed_record,const Setting& setting) {
    assert(&changed_record == &record); changed_record.settings.unsaved = true; ++changes; ++changes_for[setting.key()];
}
}
namespace tsuchi::game {
uint64_t from_handle(uint64_t id) { return id; }
uint64_t to_handle(uint64_t id) { return id; }
uint64_t part_owner(uint64_t id) { return id; }
bool is_game_actor(uint64_t) { return true; }
bool path_stays_within(const char*) { return true; }
TSUCHI_Result read_whole_file(const char*,char*&,uint64_t&) { return TSUCHI_NOT_FOUND; }
bool utf8_to_units(const char* text,char16_t* out,size_t capacity,size_t& count) {
    count = 0; const unsigned char* at = reinterpret_cast<const unsigned char*>(text);
    while (*at) {
        const uint32_t first = *at++; uint32_t code = first, extra = 0;
        if (first >= 0xf0) { code = first & 7; extra = 3; }
        else if (first >= 0xe0) { code = first & 15; extra = 2; }
        else if (first >= 0xc0) { code = first & 31; extra = 1; }
        else if (first >= 0x80) return false;
        for (uint32_t i = 0; i < extra; ++i) {
            if ((*at & 0xc0) != 0x80) return false;
            code = code * 64 + (*at++ & 63);
        }
        const size_t units = code > 0xffff ? 2 : 1;
        if (out && count + units > capacity) return false;
        if (out) {
            if (units == 1) out[count] = char16_t(code);
            else { out[count] = char16_t(0xd800 + ((code - 0x10000) >> 10)); out[count + 1] = char16_t(0xdc00 + ((code - 0x10000) & 1023)); }
        }
        count += units;
    }
    return true;
}
}
// A number setting holds what a float holds: a default, a bound or a saved value past that is refused, and a
// value past it is still written out.
void float_range() {
    for (const char* document : {R"({"far":{"type":"number","default":1e39}})", R"({"far":{"type":"number","default":0,"max":1e39}})",
                                 R"({"far":{"type":"number","default":0,"min":-1e39}})"}) {
        const Json* declared = read_json(document, std::strlen(document));
        Settings settings{};
        Text problem;
        assert(declared && !declare_settings(settings, declared, problem) && problem.get()[0]);
        problem.clear();
        forget_settings(settings);
        release_json(declared);
    }
    const char* document = R"({"near":{"type":"number","default":2,"max":3.4028235e38}})";
    const Json* declared = read_json(document, std::strlen(document));
    Settings settings{};
    Text problem;
    assert(declared && declare_settings(settings, declared, problem));
    const char saved[] = R"({"near":1e39})";
    take_settings(settings, saved, sizeof saved - 1, nullptr, nullptr);
    float value = 0;
    assert(setting_number(*find_setting(settings, "near"), value) == TSUCHI_OK && value == 2);
    char text[48];
    setting_number_text(__builtin_inff(), text);
    assert(!std::strcmp(text, "3.4028235e38"));
    setting_number_text(-__builtin_inff(), text);
    assert(!std::strcmp(text, "-3.4028235e38"));
    forget_settings(settings);
    release_json(declared);
}
int main() {
    float_range();
    set_thread_probe([]{return uint64_t(1);}); note_game_thread(); ui_start(); ui_set_renderer(&renderer);
    framework = mod_create("tsuchi.settings"); assert(framework); ui_settings_start(framework);
    ModRecord empty{}; TSUCHI_Screen empty_screen{999};
    assert(ui_mod_settings(empty,&empty_screen) == TSUCHI_NOT_FOUND && empty_screen.id == 999 && prepares == 0);
    std::memcpy(record.id,"garden",7); record.state = ModState::disabled;
    const char* document = R"({"enabled":{"type":"toggle","name":"Enabled","default":false},"count":{"type":"integer","name":"Count","min":-2,"max":5,"default":1},"rate":{"type":"number","name":"Rate","min":0.25,"max":2.75,"default":0.75},"name":{"type":"text","name":"Name","max_length":12,"default":"Olimar"},"difficulty":{"type":"choice","name":"Difficulty","choices":["normal","hard"],"default":"normal"},"colours":{"type":"set","name":"Colours","choices":["red","Blue Pikmin"],"default":["red"]},"single":{"type":"choice","name":"Single","choices":["[one]"],"default":"[one]"},"spaced":{"type":"choice","name":"Spaced","choices":["Normal Mode","Hard Mode","Hard [\"Mode\"]"],"default":"Normal Mode"}})";
    const Json* declared = read_json(document,std::strlen(document)); assert(declared); Text problem;
    assert(declare_settings(record.settings,declared,problem));
    TSUCHI_Screen screen{};
    assert(ui_mod_settings(record,nullptr) == TSUCHI_INVALID);
    assert(ui_mod_settings(record,&screen) == TSUCHI_OK && prepares == 1);
    assert(ui::table.hold_open(framework,screen) == TSUCHI_OK);
    assert(ui_active_screen()->controls.count == 11);
    choose(screen,"Enabled","on"); assert(find_setting(record.settings,"enabled")->integer == 1);
    const char* integer_id = control("Count");
    assert(ui::table.set_integer(framework,screen,integer_id,3) == TSUCHI_OK); ui::emit_changed_to(framework, {screen,integer_id, 2});
    assert(find_setting(record.settings,"count")->integer == 3);
    choose(screen,"Difficulty","hard"); assert(!std::strcmp(find_setting(record.settings,"difficulty")->text.get(),"hard"));
    choose(screen,"Colours: Blue Pikmin","on"); bool has = false;
    assert(setting_has(*find_setting(record.settings,"colours"),"Blue Pikmin",has) == TSUCHI_OK && has);
    choose(screen,"Colours: red","off"); assert(setting_has(*find_setting(record.settings,"colours"),"red",has) == TSUCHI_OK && !has);
    Setting* number = find_setting(record.settings,"rate"); assert(number->number == 0.75);
    button(screen,"Increase Rate"); assert(number->number == 1.75);
    button(screen,"Increase Rate"); assert(number->number == 2.75);
    const uint32_t at_max = changes; button(screen,"Increase Rate"); assert(changes == at_max && number->number == 2.75);
    button(screen,"Decrease Rate"); button(screen,"Decrease Rate"); button(screen,"Decrease Rate"); assert(number->number == 0.25);
    assert(record.settings.unsaved && record.state == ModState::disabled);
    char text[96]; uint64_t needed = 0;
    assert(ui::table.text(framework,screen,control("Single"),text,sizeof text,&needed) == TSUCHI_OK && !std::strcmp(text,u8"［one］"));
    assert(!std::strcmp(find_setting(record.settings,"single")->text.get(),"[one]"));
    assert(ui::table.choice(framework,screen,control("Spaced"),text,sizeof text,&needed) == TSUCHI_OK && !std::strcmp(text,"option-0"));
    choose(screen,"Spaced","option-1");
    assert(!std::strcmp(find_setting(record.settings,"spaced")->text.get(),"Hard Mode"));
    choose(screen,"Spaced","option-2");
    assert(!std::strcmp(find_setting(record.settings,"spaced")->text.get(),"Hard [\"Mode\"]"));
    const UiScreen* selected_screen = ui_active_screen();
    for (uint32_t i = 0; i < selected_screen->controls.count; ++i) {
        const UiControl& row = selected_screen->controls[i];
        if (!std::strcmp(row.label.get(),"Spaced")) {
            const char* third = row.labels.after(row.labels.after(row.labels.first()));
            assert(third && !std::strcmp(third,u8"Hard ［\"Mode\"］"));
        }
    }
    assert(ui::table.release_open(framework,screen) == TSUCHI_OK); frame();
    assert(ui::table.destroy(framework,screen) == TSUCHI_NOT_FOUND);
    // A provider makes the text edit explicit. Cancellation and failure preserve the setting.
    ui_settings_text_provider(editor);
    assert(ui_mod_settings(record,&screen) == TSUCHI_OK && ui::table.hold_open(framework,screen) == TSUCHI_OK);
    assert(ui_active_screen()->controls.count == 12);
    button(screen,"Edit Name"); assert(editor_initial == "Olimar" && edit_done);
    edit_done(edit_request,TSUCHI_NOT_READY,nullptr); assert(!std::strcmp(find_setting(record.settings,"name")->text.get(),"Olimar"));
    editor_result = TSUCHI_IO; button(screen,"Edit Name");
    assert(!std::strcmp(find_setting(record.settings,"name")->text.get(),"Olimar"));
    editor_result = TSUCHI_OK; button(screen,"Edit Name"); edit_done(edit_request,TSUCHI_OK,"Louie");
    assert(!std::strcmp(find_setting(record.settings,"name")->text.get(),"Louie"));
    // Reset all declared kinds in place, with one persistence/change request
    // per changed setting even when a set has several displayed member rows.
    const uint32_t before_reset = changes, before_reset_opens = opens, before_reset_closes = closes;
    const auto changes_before_reset = changes_for;
    const UiScreen* same_screen = ui_active_screen();
    const uint32_t same_count = same_screen->controls.count, same_selection = same_screen->selected;
    assert(ui_action(7)); frame();
    assert(ui_active_screen() == same_screen && same_screen->id == screen.id && same_screen->controls.count == same_count && same_screen->selected == same_selection);
    assert(opens == before_reset_opens && closes == before_reset_closes && changes == before_reset + 7);
    for (const char* key : {"enabled", "count", "rate", "name", "difficulty", "colours", "spaced"}) {
        const auto before = changes_before_reset.find(key);
        assert(changes_for[key] == (before == changes_before_reset.end() ? 0 : before->second) + 1);
    }
    assert(changes_for["single"] == 0);
    assert(find_setting(record.settings,"enabled")->integer == 0 && find_setting(record.settings,"count")->integer == 1);
    assert(find_setting(record.settings,"rate")->number == 0.75 && !std::strcmp(find_setting(record.settings,"name")->text.get(),"Olimar"));
    assert(!std::strcmp(find_setting(record.settings,"difficulty")->text.get(),"normal"));
    assert(setting_has(*find_setting(record.settings,"colours"),"red",has) == TSUCHI_OK && has);
    assert(setting_has(*find_setting(record.settings,"colours"),"Blue Pikmin",has) == TSUCHI_OK && !has);
    for (const auto& row : {std::pair<const char*,const char*>{"Enabled","off"}, {"Difficulty","normal"},
                            {"Colours: red","on"}, {"Colours: Blue Pikmin","off"}, {"Spaced","option-0"}}) {
        assert(ui::table.choice(framework,screen,control(row.first),text,sizeof text,&needed) == TSUCHI_OK && !std::strcmp(text,row.second));
    }
    int64_t default_count = 0;
    assert(ui::table.integer(framework,screen,control("Count"),&default_count) == TSUCHI_OK && default_count == 1);
    assert(ui::table.text(framework,screen,control("Rate"),text,sizeof text,&needed) == TSUCHI_OK && !std::strcmp(text,"0.75"));
    assert(ui::table.text(framework,screen,control("Name"),text,sizeof text,&needed) == TSUCHI_OK && !std::strcmp(text,"Olimar"));
    assert(ui::table.text(framework,screen,control("Single"),text,sizeof text,&needed) == TSUCHI_OK && !std::strcmp(text,u8"［one］"));
    // A no-op reset neither publishes another change nor requests persistence.
    record.settings.unsaved = false;
    assert(ui_action(7)); frame();
    assert(changes == before_reset + 7 && !record.settings.unsaved && opens == before_reset_opens && closes == before_reset_closes);
    // A reset event for another mod's screen cannot affect these declarations.
    TSUCHI_Mod* foreign = mod_create("settings.foreign"); assert(foreign);
    TSUCHI_Screen foreign_screen{}; assert(ui::table.create(foreign,"Other",&foreign_screen) == TSUCHI_OK);
    choose(screen,"Enabled","on"); const uint32_t before_foreign = changes;
    ui::emit_reset_requested_to(framework,{foreign_screen});
    assert(changes == before_foreign && find_setting(record.settings,"enabled")->integer == 1);
    mod_stop(foreign);
    // Even a no-op reset cancels an outstanding edit before its completion.
    assert(ui_action(7)); frame();
    button(screen,"Edit Name"); const uint64_t before_reset_request = edit_request;
    assert(ui_action(7)); frame();
    const uint32_t after_pending_reset = changes;
    edit_done(before_reset_request,TSUCHI_OK,"Late");
    assert(changes == after_pending_reset && !std::strcmp(find_setting(record.settings,"name")->text.get(),"Olimar"));
    button(screen,"Edit Name"); edit_done(edit_request,TSUCHI_OK,"Louie");
    const uint32_t after_edit = changes; edit_done(edit_request,TSUCHI_OK,"Duplicate"); assert(changes == after_edit);
    button(screen,"Edit Name"); edit_done(edit_request,TSUCHI_OK,"This name is too long");
    assert(!std::strcmp(find_setting(record.settings,"name")->text.get(),"Louie"));
    button(screen,"Edit Name"); const uint64_t closed_request = edit_request;
    host_live = false; frame(); host_live = true;
    edit_done(closed_request,TSUCHI_OK,"Late"); assert(!std::strcmp(find_setting(record.settings,"name")->text.get(),"Louie"));
    // Completion after close is refused before the queued closed event runs.
    assert(ui_mod_settings(record,&screen) == TSUCHI_OK && ui::table.hold_open(framework,screen) == TSUCHI_OK);
    button(screen,"Edit Name");
    assert(ui::table.release_open(framework,screen) == TSUCHI_OK);
    edit_done(edit_request,TSUCHI_OK,"Closed");
    assert(!std::strcmp(find_setting(record.settings,"name")->text.get(),"Louie")); frame();
    // A synchronous provider can stop the framework before it calls completion or returns.
    assert(ui_mod_settings(record,&screen) == TSUCHI_OK && ui::table.hold_open(framework,screen) == TSUCHI_OK);
    stop_in_editor = true; button(screen,"Edit Name"); frame();
    assert(!std::strcmp(find_setting(record.settings,"name")->text.get(),"Louie"));
    assert(ui_mod_settings(record,&screen) == TSUCHI_NOT_READY);
    assert(closes == 4);
    forget_settings(record.settings); release_json(declared); problem.clear();
}
