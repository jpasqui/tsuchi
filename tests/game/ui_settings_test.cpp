#include "ui_settings.h"
#include "ui_screens.h"
#include "ui_service.h"
#include "ui_keyboard.h"
#include "input_service.h"
#include "game_table.h"
#include "actor_events.h"
#include "ordinary_lifecycle.h"
#include <cassert>
#include <cstring>
#include <cstdio>
#include <atomic>
#include <chrono>
#include <thread>
#include <vector>
#include <string>
#include <map>
using namespace tsuchi;
namespace {
std::vector<void(*)()> steps;
void frame_steps() { for (auto step : steps) step(); }
ModRecord record{}, voice{};
uint32_t changes = 0, prepares = 0, closes = 0, opens = 0;
std::map<std::string, uint32_t> changes_for;
bool host_live = true;
TSUCHI_Mod* framework = nullptr;
uint64_t edit_request = 0;
UiSettingsTextDone edit_done = nullptr;
TSUCHI_Result editor_result = TSUCHI_OK;
bool stop_in_editor = false;
std::string editor_initial, editor_title;
uint32_t editor_maximum = 0;
bool editor_number = false;

// A stand-in for the input service: four players' controllers and a keyboard, as a test sets them.
uint32_t pad_held[4] = {};
bool pad_routed[4] = {true, true, false, false};
bool keys_down[256] = {};
bool keyboard_there = true;
uint32_t pad_hidden[4] = {};
TSUCHI_Result fake_pad(uint32_t player, TSUCHI_PadState* out) {
    if (player >= 4) return TSUCHI_INVALID;
    if (!pad_routed[player]) return TSUCHI_NOT_READY;
    *out = {}; out->connected = true; out->held = pad_held[player]; return TSUCHI_OK;
}
TSUCHI_Result fake_key(uint32_t usage, bool* out) {
    if (usage > 255) return TSUCHI_INVALID;
    if (!keyboard_there) return TSUCHI_NOT_READY;
    *out = keys_down[usage]; return TSUCHI_OK;
}
TSUCHI_Result fake_hold_hidden(TSUCHI_Mod*, uint32_t player, TSUCHI_Buttons buttons) {
    if (player >= 4) return TSUCHI_INVALID;
    if (!pad_routed[player]) return TSUCHI_NOT_READY;
    pad_hidden[player] = buttons; return TSUCHI_OK;
}
TSUCHI_Result fake_release_hidden(TSUCHI_Mod*, uint32_t player) {
    if (player >= 4) return TSUCHI_INVALID;
    if (!pad_hidden[player]) return TSUCHI_NOT_FOUND;
    pad_hidden[player] = 0; return TSUCHI_OK;
}
const input::Backend fake_input{fake_pad, fake_key, nullptr, nullptr, nullptr, fake_hold_hidden, fake_release_hidden, nullptr};
bool any_hidden() { for (uint32_t hidden : pad_hidden) if (hidden) return true; return false; }

// A stand-in for the system's software keyboard, shown on the framework's keyboard thread.
namespace fn = game::table::fn;
std::atomic<uint32_t> shown{0}, show_result{0};
std::string shown_prompt, shown_initial, keyboard_reply;
uint32_t shown_most = 0, shown_mode = 0;
void* thread_entry = nullptr; void* thread_argument = nullptr;
uint32_t fake_create_thread(void*, void* entry, void* argument, void*, uint64_t, int32_t) { thread_entry = entry; thread_argument = argument; return 0; }
void fake_start_thread(void*) { std::thread(reinterpret_cast<void (*)(void*)>(thread_entry), thread_argument).detach(); }
void fake_sleep(int64_t nanoseconds) { std::this_thread::sleep_for(std::chrono::nanoseconds(nanoseconds)); }
void* fake_current_thread() { static int self; return &self; }
int32_t fake_priority(void*) { return 44; }
void fake_preset(void* arguments, uint32_t) { std::memset(arguments, 0, game::table::ui_keyboard::size); }
uint64_t fake_work_size(bool) { return 0x1000; }
uint64_t fake_text_size() { return 0x1000; }
void fake_initial(void*, const char* text) { shown_initial = text; }
void fake_guide(void*, const char* text) { shown_prompt = text; }
uint32_t fake_show(void* output, const void* arguments) {
    struct Output { char* data; uint64_t bytes; } given{};
    std::memcpy(&given, output, sizeof given);
    std::memcpy(&shown_most, static_cast<const char*>(arguments) + game::table::ui_keyboard::maximum, 4);
    std::memcpy(&shown_mode, arguments, 4);
    std::memcpy(given.data, keyboard_reply.c_str(), keyboard_reply.size() + 1);
    const uint32_t result = show_result;
    ++shown;
    return result;
}
void use_fake_keyboard() {
    fn::os_create_thread = fake_create_thread; fn::os_start_thread = fake_start_thread; fn::os_sleep_thread = fake_sleep;
    fn::os_current_thread = fake_current_thread; fn::os_thread_priority = fake_priority; fn::os_set_thread_name = nullptr;
    fn::ui_keyboard_preset = fake_preset; fn::ui_keyboard_work_size = fake_work_size; fn::ui_keyboard_text_size = fake_text_size;
    fn::ui_keyboard_initial = fake_initial; fn::ui_keyboard_guide = fake_guide; fn::ui_keyboard_show = fake_show;
}
// Frames until the keyboard has been shown `count` times and its text told, or two seconds.
void frames_until_shown(uint32_t count) {
    for (int i = 0; i < 400 && shown < count; ++i) { frame_steps(); std::this_thread::sleep_for(std::chrono::milliseconds(5)); }
    assert(shown == count);
    // The thread marks the keyboard closed just after it returns; the text is told at the start of a later frame.
    for (int i = 0; i < 10; ++i) { std::this_thread::sleep_for(std::chrono::milliseconds(5)); frame_steps(); }
}
uint64_t internal_request = 0; TSUCHI_Result internal_result = TSUCHI_IO; std::string internal_text;
void internal_done(uint64_t request, TSUCHI_Result result, const char* text) { internal_request = request; internal_result = result; internal_text = text ? text : "<none>"; }
uint32_t entered = 0;
void text_entered(void*, const TSUCHI_UiTextEntered* event) { ++entered; internal_text = event->accepted ? event->text : "<cancelled>"; }
TSUCHI_Result open_view(const UiScreen&, void** out) { ++opens; *out = reinterpret_cast<void*>(1); return TSUCHI_OK; }
bool update_view(void*, const UiScreen&) { return true; }
bool live_view(void*) { return host_live; }
void close_view(void*) { ++closes; }
UiRenderer renderer{open_view, update_view, live_view, close_view};
void frame() { frame_steps(); }
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
TSUCHI_Result editor(const char* title, const char* current, uint32_t maximum, bool number, uint64_t request, UiSettingsTextDone done) {
    editor_title = title; editor_maximum = maximum; editor_number = number; editor_initial = current;
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
ModRecord* find_mod(const char* id) { return !std::strcmp(id, record.id) ? &record : !std::strcmp(id, voice.id) ? &voice : nullptr; }
std::vector<std::string> notes;
void mod_note(const char* id, const char* text) { notes.push_back(std::string(id) + ": " + text); }
void settings_player_prepare(ModRecord&) { ++prepares; }
void settings_player_changed(ModRecord& changed_record,const Setting& setting) {
    assert(&changed_record == &record || &changed_record == &voice); changed_record.settings.unsaved = true; ++changes; ++changes_for[setting.key()];
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
// A binding is declared with an optional `accepts`, holds `pad:<button>` or `key:<usage>` as its text, and refuses
// what its `accepts` does not allow, from a call and from the save.
void bindings_declared() {
    for (const char* document : {R"({"a":{"type":"binding"}})", R"({"a":{"type":"binding","default":"pad:zz"}})",
                                 R"({"a":{"type":"binding","default":"key:05"}})", R"({"a":{"type":"binding","default":"key:256"}})",
                                 R"({"a":{"type":"binding","default":"key:"}})", R"({"a":{"type":"binding","default":"ZL"}})",
                                 R"({"a":{"type":"binding","default":"pad:zl","accepts":"key"}})", R"({"a":{"type":"binding","default":"key:4","accepts":"pad"}})",
                                 R"({"a":{"type":"binding","default":"pad:zl","accepts":"mouse"}})", R"({"a":{"type":"binding","default":"pad:zl","accepts":true}})",
                                 R"({"a":{"type":"toggle","default":true,"accepts":"pad"}})", R"({"a":{"type":"binding","default":"pad:zl","max_length":8}})",
                                 R"({"a":{"type":"binding","default":"pad:zl","choices":["a"]}})"}) {
        const Json* declared = read_json(document, std::strlen(document));
        Settings settings{}; Text problem;
        assert(declared && !declare_settings(settings, declared, problem) && problem.get()[0]);
        problem.clear(); forget_settings(settings); release_json(declared);
    }
    {
        const char* document = R"({"a":{"type":"switch","default":true}})";
        const Json* declared = read_json(document, std::strlen(document));
        Settings settings{}; Text problem;
        assert(declared && !declare_settings(settings, declared, problem));
        assert(!std::strcmp(problem.get(), "the setting \"a\" needs a \"type\": toggle, integer, number, text, choice, set or binding"));
        problem.clear(); forget_settings(settings); release_json(declared);
    }
    const char* document = R"({"talk":{"type":"binding","default":"pad:zl","accepts":"pad"},"chat":{"type":"binding","default":"key:23","accepts":"key"},"either":{"type":"binding","default":"pad:left_stick"}})";
    const Json* declared = read_json(document, std::strlen(document));
    Settings settings{}; Text problem;
    assert(declared && declare_settings(settings, declared, problem));
    Setting& talk = *find_setting(settings, "talk"), &chat = *find_setting(settings, "chat"), &either = *find_setting(settings, "either");
    const char* value = nullptr; bool changed = false;
    assert(setting_text(talk, value) == TSUCHI_OK && !std::strcmp(value, "pad:zl"));
    assert(binding_accepts(talk) == uint32_t(BindingDevice::pad) && binding_accepts(chat) == uint32_t(BindingDevice::key) && binding_accepts(either) == 3);
    for (const char* refused : {"key:4", "pad:", "pad:ZL", "pad:zl ", "zl", "key:1"}) assert(set_setting_text(talk, refused, changed) == TSUCHI_INVALID);
    for (const char* refused : {"pad:a", "key:-1", "key:+4", "key:256", "key:0004"}) assert(set_setting_text(chat, refused, changed) == TSUCHI_INVALID);
    assert(set_setting_text(talk, "pad:right_stick", changed) == TSUCHI_OK && changed);
    assert(set_setting_text(either, "key:0", changed) == TSUCHI_OK && changed && set_setting_text(either, "key:255", changed) == TSUCHI_OK);
    // Integer and number calls refuse a binding, and it reads as no other kind.
    int64_t whole = 0; bool flag = false;
    assert(set_setting_integer(talk, 1, changed) == TSUCHI_INVALID && setting_integer(talk, whole) == TSUCHI_INVALID && setting_has(talk, "a", flag) == TSUCHI_INVALID);
    // Kept in the save as its text; a saved value its `accepts` refuses is passed over.
    Text written;
    assert(settings_text(settings, written) && !std::strcmp(written.get(), "{\n  \"talk\": \"pad:right_stick\",\n  \"either\": \"key:255\"\n}\n"));
    written.clear();
    bool reset_changed = false;
    assert(reset_setting(talk, reset_changed) == TSUCHI_OK && reset_changed && reset_setting(either, reset_changed) == TSUCHI_OK);
    talk.touched = either.touched = false;
    const char saved[] = R"({"talk":"key:4","chat":"key:44","either":"pad:minus"})";
    assert(take_settings(settings, saved, sizeof saved - 1, nullptr, nullptr));
    assert(!std::strcmp(talk.text.get(), "pad:zl") && !std::strcmp(chat.text.get(), "key:44") && !std::strcmp(either.text.get(), "pad:minus"));
    forget_settings(settings); release_json(declared); problem.clear();

    // As a player reads them.
    char label[48];
    const std::pair<const char*, const char*> labels[] = {{"pad:zl", "ZL"}, {"pad:a", "A"}, {"pad:plus", "+"}, {"pad:minus", "-"},
        {"pad:up", "D-Pad Up"}, {"pad:left_stick", "L Stick Button"}, {"key:23", "Keyboard: T"}, {"key:4", "Keyboard: A"},
        {"key:44", "Keyboard: Space"}, {"key:41", "Keyboard: Esc"}, {"key:225", "Keyboard: Left Shift"}, {"key:58", "Keyboard: F1"},
        {"key:0", "Keyboard: Key 0"}, {"key:200", "Keyboard: Key 200"}, {"nonsense", "nonsense"}};
    for (const auto& each : labels) {
        binding_label(each.first, label);
        if (std::strcmp(label, each.second)) std::printf("%s reads as %s\n", each.first, label);
        assert(!std::strcmp(label, each.second));
    }
    char text[24];
    binding_text({BindingDevice::pad, 9, 0}, text); assert(!std::strcmp(text, "pad:zr"));
    binding_text({BindingDevice::key, 0, 100}, text); assert(!std::strcmp(text, "key:100"));
    BindingValue read{};
    assert(read_binding("pad:down", read) && read.device == BindingDevice::pad && read.button == 15);
    assert(read_binding("key:7", read) && read.device == BindingDevice::key && read.usage == 7);

    // Typed text, read as an integer or a number.
    int64_t integer = 0; double number = 0;
    assert(read_setting_integer("42", integer) && integer == 42 && read_setting_integer(" -7 ", integer) && integer == -7);
    assert(read_setting_integer("+3", integer) && integer == 3);
    assert(read_setting_integer("9223372036854775807", integer) && integer == INT64_MAX);
    assert(read_setting_integer("-9223372036854775808", integer) && integer == INT64_MIN);
    for (const char* refused : {"", " ", "-", "9223372036854775808", "-9223372036854775809", "1.5", "1e3", "12a", "0x10", "1 2"})
        assert(!read_setting_integer(refused, integer));
    assert(read_setting_number("1.5", number) && number == 1.5 && read_setting_number(".5", number) && number == 0.5);
    assert(read_setting_number("-.25", number) && number == -0.25 && read_setting_number("2.", number) && number == 2);
    assert(read_setting_number("+1e3", number) && number == 1000 && read_setting_number(" 7 ", number) && number == 7);
    for (const char* refused : {"", ".", "-", "abc", "1.2.3", "1,5", "--1", "1 2", "nan", "inf"}) assert(!read_setting_number(refused, number));
    setting_integer_text(-12, text); assert(!std::strcmp(text, "-12"));
}
int main() {
    float_range();
    bindings_declared();
    set_thread_probe([]{return uint64_t(1);}); note_game_thread(); ui_start(); ui_set_renderer(&renderer);
    framework = mod_create("tsuchi.settings"); assert(framework); ui_settings_start(framework);
    input::set_backend(&fake_input);
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
    assert(ui_active_screen()->controls.count == 14);
    button(screen,"Edit Name"); assert(editor_initial == "Olimar" && edit_done && editor_title == "Name" && editor_maximum == 12 && !editor_number);
    edit_done(edit_request,TSUCHI_NOT_READY,nullptr); assert(!std::strcmp(find_setting(record.settings,"name")->text.get(),"Olimar"));
    editor_result = TSUCHI_IO; button(screen,"Edit Name");
    assert(!std::strcmp(find_setting(record.settings,"name")->text.get(),"Olimar"));
    editor_result = TSUCHI_OK; button(screen,"Edit Name"); edit_done(edit_request,TSUCHI_OK,"Louie");
    assert(!std::strcmp(find_setting(record.settings,"name")->text.get(),"Louie"));
    // Integers and numbers are typed too. The integer keeps its arrows, and what its declaration refuses keeps the old value.
    button(screen,"Edit Count"); assert(editor_title == "Count" && editor_initial == "3" && !editor_number && editor_maximum == 20);
    edit_done(edit_request,TSUCHI_OK,"4");
    int64_t typed = 0;
    assert(find_setting(record.settings,"count")->integer == 4 && ui::table.integer(framework,screen,control("Count"),&typed) == TSUCHI_OK && typed == 4);
    for (const char* refused : {"9", "-3", "four", "", "4.5", "99999999999999999999"}) {
        const uint32_t before = changes;
        button(screen,"Edit Count"); edit_done(edit_request,TSUCHI_OK,refused);
        assert(changes == before && find_setting(record.settings,"count")->integer == 4);
    }
    button(screen,"Edit Count"); edit_done(edit_request,TSUCHI_NOT_READY,nullptr); assert(find_setting(record.settings,"count")->integer == 4);
    button(screen,"Edit Count"); edit_done(edit_request,TSUCHI_OK," -2 ");
    assert(find_setting(record.settings,"count")->integer == -2 && ui::table.integer(framework,screen,control("Count"),&typed) == TSUCHI_OK && typed == -2);
    char typed_text[96]; uint64_t typed_bytes = 0;
    button(screen,"Edit Rate"); assert(editor_title == "Rate" && editor_initial == "0.25" && !editor_number);
    edit_done(edit_request,TSUCHI_OK,"1.5");
    assert(find_setting(record.settings,"rate")->number == 1.5 && ui::table.text(framework,screen,control("Rate"),typed_text,sizeof typed_text,&typed_bytes) == TSUCHI_OK && !std::strcmp(typed_text,"1.5"));
    for (const char* refused : {"3", "0.2", "abc", "1e40", ""}) {
        const uint32_t before = changes;
        button(screen,"Edit Rate"); edit_done(edit_request,TSUCHI_OK,refused);
        assert(changes == before && find_setting(record.settings,"rate")->number == 1.5);
    }
    button(screen,"Edit Rate"); edit_done(edit_request,TSUCHI_OK,".5");
    assert(find_setting(record.settings,"rate")->number == 0.5 && ui::table.text(framework,screen,control("Rate"),typed_text,sizeof typed_text,&typed_bytes) == TSUCHI_OK && !std::strcmp(typed_text,"0.5"));
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
    // A binding: its row reads as a player reads it, and Change and its name takes the next button or key pressed.
    std::memcpy(voice.id,"voice",6); voice.state = ModState::running;
    const char* voice_document = R"({"talk":{"type":"binding","name":"Push to Talk Button","default":"pad:zl","accepts":"pad"},"chat":{"type":"binding","name":"Chat Key","default":"key:23","accepts":"key"},"either":{"type":"binding","default":"pad:a"},"players":{"type":"integer","name":"Players","min":1,"max":4,"default":2}})";
    const Json* voice_declared = read_json(voice_document,std::strlen(voice_document)); assert(voice_declared);
    assert(declare_settings(voice.settings,voice_declared,problem));
    TSUCHI_Screen voice_screen{};
    assert(ui_mod_settings(voice,&voice_screen) == TSUCHI_OK && ui::table.hold_open(framework,voice_screen) == TSUCHI_OK);
    assert(ui_active_screen()->controls.count == 8);
    auto row = [&](const char* label) {
        char shown_text[96]; uint64_t size = 0;
        assert(ui::table.text(framework,voice_screen,control(label),shown_text,sizeof shown_text,&size) == TSUCHI_OK);
        return std::string(shown_text);
    };
    auto value = [&](const char* key) { return std::string(find_setting(voice.settings,key)->text.get()); };
    auto footer = [&]() { return std::string(ui_active_screen()->footer.get()); };
    assert(row("Push to Talk Button") == "ZL" && row("Chat Key") == "Keyboard: T" && row("either") == "A");
    // An integer that can't be below zero is typed on the number pad.
    button(voice_screen,"Edit Players"); assert(editor_number && editor_initial == "2"); edit_done(edit_request,TSUCHI_OK,"3");
    assert(find_setting(voice.settings,"players")->integer == 3);
    // The A that chose the row is still down. It is passed over, and so is a key, which this binding doesn't take.
    pad_held[0] = TSUCHI_BUTTONS_A;
    button(voice_screen,"Change Push to Talk Button");
    assert(row("Push to Talk Button") == "Press a button..." && footer() == "Wait 5 seconds to cancel");
    assert(pad_hidden[0] == 0xFFFF && pad_hidden[1] == 0xFFFF && !pad_hidden[2] && !pad_hidden[3]);
    for (int i = 0; i < 10; ++i) frame();
    keys_down[23] = true; frame(); keys_down[23] = false; frame();
    assert(value("talk") == "pad:zl" && changes_for["talk"] == 0);
    // A second capture waits for this one.
    button(voice_screen,"Change either"); frame(); assert(row("either") == "A");
    // The next button down on any player's controller is taken; its press stays hidden from the game until it is let go.
    pad_held[1] = TSUCHI_BUTTONS_ZR; frame();
    assert(value("talk") == "pad:zr" && changes_for["talk"] == 1 && row("Push to Talk Button") == "ZR" && footer().empty());
    frame(); assert(pad_hidden[0] && pad_hidden[1]);
    pad_held[0] = 0; frame(); assert(pad_hidden[1]);
    pad_held[1] = 0; frame(); assert(!any_hidden());
    // A button held as it began counts once it has been let go and pressed again. Of two at once, the lower is taken.
    pad_held[0] = TSUCHI_BUTTONS_A; button(voice_screen,"Change Push to Talk Button"); frame(); assert(value("talk") == "pad:zr");
    pad_held[0] = 0; frame(); assert(value("talk") == "pad:zr");
    pad_held[0] = TSUCHI_BUTTONS_A; frame(); assert(value("talk") == "pad:a" && row("Push to Talk Button") == "A");
    pad_held[0] = 0; frame(); assert(!any_hidden());
    button(voice_screen,"Change Push to Talk Button"); pad_held[1] = TSUCHI_BUTTONS_DOWN | TSUCHI_BUTTONS_L; frame();
    assert(value("talk") == "pad:l" && row("Push to Talk Button") == "L");
    pad_held[1] = 0; frame(); assert(!any_hidden());
    // Five seconds without a press, or Escape, cancels and keeps the value.
    const uint32_t before_cancel = changes;
    button(voice_screen,"Change Push to Talk Button");
    for (int i = 0; i < 149; ++i) frame();
    assert(row("Push to Talk Button") == "Press a button...");
    frame(); assert(row("Push to Talk Button") == "L" && value("talk") == "pad:l" && footer().empty());
    frame(); assert(!any_hidden());
    button(voice_screen,"Change Push to Talk Button"); keys_down[41] = true; frame(); keys_down[41] = false;
    assert(row("Push to Talk Button") == "L" && value("talk") == "pad:l" && changes == before_cancel);
    frame(); assert(!any_hidden());
    // A keyboard binding takes the next key that goes down, and no controller button.
    keys_down[4] = true;
    button(voice_screen,"Change Chat Key");
    assert(row("Chat Key") == "Press a key..." && footer() == "Press Esc or wait 5 seconds to cancel");
    frame(); assert(value("chat") == "key:23");
    pad_held[0] = TSUCHI_BUTTONS_B; frame(); pad_held[0] = 0; frame(); assert(value("chat") == "key:23");
    keys_down[4] = false; frame(); keys_down[4] = true; frame();
    assert(value("chat") == "key:4" && row("Chat Key") == "Keyboard: A" && changes_for["chat"] == 1);
    keys_down[4] = false; frame(); assert(!any_hidden());
    // With both allowed, a controller button and a key in one step: the controller's is taken. Then a modifier key.
    button(voice_screen,"Change either"); assert(row("either") == "Press a button or key...");
    pad_held[0] = TSUCHI_BUTTONS_Y; keys_down[44] = true; frame();
    assert(value("either") == "pad:y" && row("either") == "Y");
    pad_held[0] = 0; keys_down[44] = false; frame(); assert(!any_hidden());
    button(voice_screen,"Change either"); keys_down[225] = true; frame(); keys_down[225] = false;
    assert(value("either") == "key:225" && row("either") == "Keyboard: Left Shift");
    frame();
    // Without a keyboard to read, a keyboard binding waits for its five seconds.
    keyboard_there = false;
    button(voice_screen,"Change Chat Key");
    for (int i = 0; i < 150; ++i) frame();
    assert(row("Chat Key") == "Keyboard: A" && value("chat") == "key:4");
    keyboard_there = true; frame(); assert(!any_hidden());
    // Reset puts the bindings back, as their player reads them.
    assert(ui_action(7)); frame();
    assert(value("talk") == "pad:zl" && row("Push to Talk Button") == "ZL" && row("Chat Key") == "Keyboard: T" && row("either") == "A");
    // A page that closes ends its capture without a change, and gives the buttons back once they are up.
    pad_held[0] = TSUCHI_BUTTONS_A;
    button(voice_screen,"Change Push to Talk Button");
    assert(ui::table.release_open(framework,voice_screen) == TSUCHI_OK);
    pad_held[1] = TSUCHI_BUTTONS_X; frame(); frame();
    assert(value("talk") == "pad:zl" && any_hidden());
    pad_held[0] = pad_held[1] = 0; frame(); assert(!any_hidden());

    // The system's keyboard as the page's editor: the text comes back to the page, and to no mod's text_entered.
    use_fake_keyboard(); ui_keyboard_start(); ui_settings_text_provider(ui_keyboard_ask);
    TSUCHI_Mod* listener = mod_create("keyboard.listener"); assert(listener);
    TSUCHI_Subscription listening{}; assert(ui::table.on_text_entered(listener, text_entered, nullptr, &listening) == TSUCHI_OK);
    assert(ui_mod_settings(record,&screen) == TSUCHI_OK && ui::table.hold_open(framework,screen) == TSUCHI_OK);
    keyboard_reply = "5"; show_result = 0;
    button(screen,"Edit Count"); frames_until_shown(1);
    assert(shown_prompt == "Count" && shown_initial == "1" && shown_mode == 0 && shown_most == 20);
    assert(find_setting(record.settings,"count")->integer == 5 && ui::table.integer(framework,screen,control("Count"),&typed) == TSUCHI_OK && typed == 5);
    keyboard_reply = "Alph"; button(screen,"Edit Name"); frames_until_shown(2);
    assert(shown_prompt == "Name" && shown_initial == "Louie" && shown_most == 12 && !std::strcmp(find_setting(record.settings,"name")->text.get(),"Alph"));
    // Cancel keeps the old value, and tells the log.
    keyboard_reply = "4"; show_result = 0x1234; button(screen,"Edit Count"); frames_until_shown(3);
    assert(find_setting(record.settings,"count")->integer == 5 && !notes.empty() && notes.back().find("keyboard closed without text") != std::string::npos);
    show_result = 0;
    assert(entered == 0);
    // The framework's own requests: the number pad, the limit held to 1 to 256, a long prompt cut where a character
    // starts, and one request at a time.
    std::string long_prompt(255, 'p'); long_prompt += "\xc3\xa9";
    keyboard_reply = "7";
    assert(ui_keyboard_ask(long_prompt.c_str(), "", 0, true, 77, internal_done) == TSUCHI_OK);
    assert(ui_keyboard_ask("Again", "", 4, false, 78, internal_done) == TSUCHI_BUSY);
    frames_until_shown(4);
    assert(internal_request == 77 && internal_result == TSUCHI_OK && internal_text == "7" && shown_mode == 1 && shown_most == 1 && shown_prompt == std::string(255, 'p'));
    assert(ui_keyboard_ask("Most", "", 5000, false, 79, internal_done) == TSUCHI_OK); frames_until_shown(5);
    assert(internal_request == 79 && shown_most == 256 && entered == 0);
    show_result = 0x1234;
    assert(ui_keyboard_ask("Cancelled", "", 4, false, 80, internal_done) == TSUCHI_OK); frames_until_shown(6);
    assert(internal_request == 80 && internal_result != TSUCHI_OK && internal_text == "<none>");
    show_result = 0;
    // A mod's request still comes back as its text_entered.
    keyboard_reply = "9";
    assert(ui::table.ask_text(listener, "Code", "", 4, TSUCHI_TEXT_KIND_NUMBER) == TSUCHI_OK); frames_until_shown(7);
    assert(entered == 1 && internal_text == "9" && internal_request == 80 && shown_mode == 1);
    mod_stop(listener);
    assert(ui::table.release_open(framework,screen) == TSUCHI_OK); frame();
    ui_settings_text_provider(editor);

    // A synchronous provider can stop the framework before it calls completion or returns.
    assert(ui_mod_settings(record,&screen) == TSUCHI_OK && ui::table.hold_open(framework,screen) == TSUCHI_OK);
    stop_in_editor = true; button(screen,"Edit Name"); frame();
    assert(!std::strcmp(find_setting(record.settings,"name")->text.get(),"Alph"));
    assert(ui_mod_settings(record,&screen) == TSUCHI_NOT_READY);
    assert(closes == 6);
    forget_settings(record.settings); release_json(declared); problem.clear();
    forget_settings(voice.settings); release_json(voice_declared);
}
