// The title menu's mod entries, Tsuchi's host over it, the title's Options entries and the Number of Players
// choice, against a made-up title screen laid out as tests/fixtures/game_table.yaml says. The game's own
// handlers are test functions that count their calls.
#include "fake_game.h"
#include "ui_title.h"
#include "ui_screens.h"
#include "ui_service.h"
#include "game_service.h"
#include "player_choice.h"
#include "hooks.h"
#include "frame.h"
#include "mods.h"
#include "objects.h"
#include "ui_rich_text.h"
#include "tsuchi/input.h"
#include <cassert>
#include <cstdio>
#include <string>
#include <vector>

using namespace tsuchi;
namespace t = game::table;

namespace {
std::vector<void (*)()> frames;
std::vector<uint32_t> actions, routed;          // the host's own actions, and those the title's handlers route
bool hosted_screen = false;                      // a screen is open in the host over the title menu
const UiRenderer* title_renderer = nullptr;
int stock_confirms = 0, stock_backs = 0, stock_ways = 0, options_confirms = 0, options_ways = 0;
std::vector<std::string> chosen;
uint64_t root_class, button_class, slot_class, box_class, input_class;
using Tick = void (*)(void*, float);
Tick title_tick = nullptr, options_tick = nullptr;
using Update = void (*)(void*, float);
Update flow_update = nullptr;

bool stock_confirm(void*) { ++stock_confirms; return true; }
bool stock_back(void*) { ++stock_backs; return true; }
bool stock_direction(void*, int32_t) { ++stock_ways; return true; }
bool options_confirm(void*) { ++options_confirms; return true; }
bool options_direction(void*, bool) { ++options_ways; return true; }
bool options_gate(void*) { return true; }
void no_tick(void*, float) {}
void no_update(void*, float) {}
uint64_t name_from_text(void*, uint64_t, uint64_t) { return 1; }
void* load_soft_object(void*) { return reinterpret_cast<void*>(button_class); }
void* create_widget(void*, void*, void*) { return reinterpret_cast<void*>(fake::object(u"Button", button_class, 0x600)); }
void* add_child(void*, void*) { return reinterpret_cast<void*>(fake::object(u"Slot", slot_class, 0x80)); }
void slot_sync(void*) {}
void list_grow(void*) { assert(false); }
bool remove_child_at(void*, int32_t) { return true; }
void highlight(void*, bool, bool) {}
}

// What the units under test reach outside themselves.
namespace tsuchi {
TSUCHI_Result hook_wrap(TSUCHI_Mod*, uint64_t entry, uint64_t, void* function, uint32_t, uint64_t* id, void** original) {
    const uint64_t main = game::main_base();
    if (entry == main + t::slot::title_presenter_tick.table + t::slot::title_presenter_tick.offset) { title_tick = reinterpret_cast<Tick>(function); *original = reinterpret_cast<void*>(&no_tick); }
    else if (entry == main + t::slot::options_presenter_tick.table + t::slot::options_presenter_tick.offset) { options_tick = reinterpret_cast<Tick>(function); *original = reinterpret_cast<void*>(&no_tick); }
    else if (entry == main + t::slot::title_flow_update.table + t::slot::title_flow_update.offset) { flow_update = reinterpret_cast<Update>(function); *original = reinterpret_cast<void*>(&no_update); }
    else return TSUCHI_UNSUPPORTED;
    *id = 1;
    return TSUCHI_OK;
}
bool each_frame(void (*fn)()) { frames.push_back(fn); return true; }
void mod_note(const char*, const char*) {}
ui::Backend& ui_screen_backend() { static ui::Backend backend{}; return backend; }
void ui_set_title_renderer(const UiRenderer* value) { title_renderer = value; }
bool ui_active_over_menu() { return hosted_screen; }
bool ui_route_action(uint64_t, uint32_t action) { routed.push_back(action); return true; }
bool ui_action(uint32_t action) { actions.push_back(action); return true; }
game_service::Backend& game_service_backend() { static game_service::Backend backend{}; return backend; }
namespace game {
// ASCII is enough for these texts.
bool utf8_to_units(const char* text, char16_t* out, size_t room, size_t& count) {
    count = 0;
    for (; *text; ++text, ++count) { if (uint8_t(*text) > 127) return false; if (out) { if (count >= room) return false; out[count] = char16_t(*text); } }
    return true;
}
void drop_shared(uint64_t) {}
} // namespace game
} // namespace tsuchi

namespace {
void frame() { for (auto fn : frames) fn(); }
uint64_t class_named(const std::u16string& name) { return fake::object(name, root_class, 0x100); }
struct Menu { uint64_t widget, input, box; std::vector<uint64_t> instances; };
// A presenter's input map: four keys, each with a handler instance of the expected kind.
uint64_t input_map(uint64_t input_class, const std::vector<std::pair<uint32_t, std::pair<uint64_t, uint64_t>>>& keys, std::vector<uint64_t>& instances) {
    const uint64_t input = fake::object(u"Input", input_class, 0x100), data = fake::memory(32 * keys.size());
    fake::put(input + t::title_input::data, data);
    fake::put<int32_t>(input + t::title_input::count, int32_t(keys.size()));
    fake::put<int32_t>(input + t::title_input::capacity, int32_t(keys.size()));
    for (size_t i = 0; i < keys.size(); ++i) {
        const uint64_t instance = fake::memory(48), cell = data + 32 * i;
        fake::put(instance, game::main_base() + keys[i].second.first);
        fake::put(instance + t::title_handler::function, keys[i].second.second);
        fake::put<uint32_t>(cell + t::title_handler::key, keys[i].first);
        fake::put(cell + t::title_handler::instance, instance);
        fake::put<uint32_t>(cell + t::title_handler::kind, 3);
        instances.push_back(instance);
    }
    return input;
}
// A widget whose own tree's root is its box of buttons, with two stock slots in it.
uint64_t box_in(uint64_t widget) {
    const uint64_t tree = fake::object(u"Tree", class_named(u"WidgetTree"), 0x100), box = fake::object(u"Buttons", box_class, 0x200);
    fake::put(widget + t::ui_host::tree, tree);
    fake::put(tree + t::ui_tree::root, box);
    const uint64_t slots = fake::memory(16);
    for (int i = 0; i < 2; ++i) {
        const uint64_t slot = fake::object(u"Slot", slot_class, 0x80);
        fake::put(slot + t::ui_panel_slot::content, fake::object(u"Stock", button_class, 0x600));
        fake::put(slots + 8 * i, slot);
    }
    fake::put(box + t::ui_panel::slots, slots);
    fake::put<int32_t>(box + t::ui_panel::slots + 8, 2);
    return box;
}
uint64_t handler_function(uint64_t instance) { return fake::get<uint64_t>(instance + t::title_handler::function); }
template <class F> F as(uint64_t function) { return reinterpret_cast<F>(function); }
struct ButtonList { uint64_t data; int32_t count, room; };
ButtonList list_of(uint64_t widget, uint64_t buttons, uint64_t count, uint64_t room) {
    return {fake::get<uint64_t>(widget + buttons), fake::get<int32_t>(widget + count), fake::get<int32_t>(widget + room)};
}
void on_chosen(void*, const TSUCHI_UiEntryChosen* event) { chosen.push_back(event->entry); }

void title_menu() {
    const uint64_t view_class = class_named(u"TitleView");
    const uint64_t presenter = fake::object(u"Presenter", class_named(u"TitlePresenter"), 0x200), widget = fake::object(u"Title", view_class, 0x800);
    std::vector<uint64_t> instances;
    const uint64_t input = input_map(input_class, {{uint32_t(t::constant::title_confirm_key), {t::data::title_input_delegate_table, reinterpret_cast<uint64_t>(&stock_confirm)}},
                                                   {uint32_t(t::constant::title_back_key), {t::data::title_input_delegate_table, reinterpret_cast<uint64_t>(&stock_back)}},
                                                   {0, {t::data::title_direction_delegate_table, reinterpret_cast<uint64_t>(&stock_direction)}},
                                                   {1, {t::data::title_direction_delegate_table, reinterpret_cast<uint64_t>(&stock_direction)}}}, instances);
    fake::put(presenter + t::title_presenter::view, widget);
    fake::put(presenter + t::title_presenter::input, input);
    fake::put<int32_t>(presenter + t::title_presenter::state, int32_t(t::constant::title_menu_state));
    box_in(widget);
    // Three stock entries, room for eight.
    const uint64_t buttons = fake::memory(8 * 8);
    for (int i = 0; i < 3; ++i) fake::put(buttons + 8 * i, fake::object(u"Stock", button_class, 0x600));
    fake::put(widget + t::title_view::buttons, buttons);
    fake::put<int32_t>(widget + t::title_view::count, 3);
    fake::put<int32_t>(widget + t::title_view::room, 8);
    fake::put<int32_t>(widget + t::title_view::hidden, -1);
    fake::put<int32_t>(widget + t::title_view::hidden_other, -1);

    TSUCHI_Mod* mod = mod_create("menu.mod");
    TSUCHI_Subscription subscription{};
    assert(ui::table.on_entry_chosen(mod, on_chosen, nullptr, &subscription) == TSUCHI_OK);
    auto& backend = ui_screen_backend();
    assert(backend.hold_menu_entry(mod, TSUCHI_MENU_TITLE, "online", "Online", "Play online") == TSUCHI_OK);
    title_tick(reinterpret_cast<void*>(presenter), 0.f);
    ButtonList list = list_of(widget, t::title_view::buttons, t::title_view::count, t::title_view::room);
    assert(list.count == 4 && handler_function(instances[0]) != reinterpret_cast<uint64_t>(&stock_confirm));
    const uint64_t ours = fake::get<uint64_t>(list.data + 24);
    const auto confirm = as<bool (*)(void*)>(handler_function(instances[0]));
    const auto back = as<bool (*)(void*)>(handler_function(instances[1]));
    // A press on the mod's entry is the mod's.
    fake::put<int32_t>(presenter + t::title_presenter::cursor, 3);
    assert(!confirm(reinterpret_cast<void*>(presenter)) && stock_confirms == 0);
    frame();
    assert(chosen.size() == 1 && chosen[0] == "online");
    // Released: it leaves the list, and if a press reaches its button all the same, the game's Confirm never sees it.
    assert(backend.release_menu_entry(mod, TSUCHI_MENU_TITLE, "online") == TSUCHI_OK);
    list = list_of(widget, t::title_view::buttons, t::title_view::count, t::title_view::room);
    assert(list.count == 3 && fake::get<int32_t>(presenter + t::title_presenter::cursor) == 2);
    fake::put(list.data + 24, ours); fake::put<int32_t>(widget + t::title_view::count, 4); fake::put<int32_t>(presenter + t::title_presenter::cursor, 3);
    assert(!confirm(reinterpret_cast<void*>(presenter)) && stock_confirms == 0);
    frame();
    assert(chosen.size() == 1);
    fake::put<int32_t>(widget + t::title_view::count, 3);
    // An entry whose mod stops is taken out the same way.
    TSUCHI_Mod* brief = mod_create("brief.mod");
    assert(backend.hold_menu_entry(brief, TSUCHI_MENU_TITLE, "brief", "Brief", "") == TSUCHI_OK);
    title_tick(reinterpret_cast<void*>(presenter), 0.f);
    list = list_of(widget, t::title_view::buttons, t::title_view::count, t::title_view::room);
    assert(list.count == 4);
    const uint64_t brief_button = fake::get<uint64_t>(list.data + 24);
    mod_stop(brief);
    frame();
    list = list_of(widget, t::title_view::buttons, t::title_view::count, t::title_view::room);
    assert(list.count == 3);
    fake::put(list.data + 24, brief_button); fake::put<int32_t>(widget + t::title_view::count, 4); fake::put<int32_t>(presenter + t::title_presenter::cursor, 3);
    assert(!confirm(reinterpret_cast<void*>(presenter)) && stock_confirms == 0);
    fake::put<int32_t>(widget + t::title_view::count, 3);
    // A stock entry is the game's.
    fake::put<int32_t>(presenter + t::title_presenter::cursor, 0);
    assert(confirm(reinterpret_cast<void*>(presenter)) && stock_confirms == 1);

    // A screen in the host over the menu: one B press is one Back. The title's Back handler routes it, and the
    // host's own reading of the buttons does not add a second.
    assert(title_renderer && title_renderer->buttons);
    alignas(8) static uint8_t view[16384]{};
    hosted_screen = true;
    title_renderer->buttons(view, 0);
    title_renderer->buttons(view, TSUCHI_BUTTONS_B);
    assert(!back(reinterpret_cast<void*>(presenter)) && stock_backs == 0);
    assert(routed.size() == 1 && routed[0] == 5 && actions.empty());
    // Left and Right still come from the buttons.
    title_renderer->buttons(view, TSUCHI_BUTTONS_B | TSUCHI_BUTTONS_LEFT);
    assert(actions.size() == 1 && actions[0] == 2);
    hosted_screen = false;
    assert(back(reinterpret_cast<void*>(presenter)) && stock_backs == 1);

    // A page of the game's own over the menu: the menu acts on no press, its own entries' and a mod's alike,
    // and nothing is routed to a screen. The presses are the menu's again once the page is gone.
    static bool page_over = false;
    const auto direction = as<bool (*)(void*, int32_t)>(handler_function(instances[2]));
    ui_title_cover([] { return page_over; });
    page_over = true;
    const size_t routed_before = routed.size(), chosen_before = chosen.size();
    const int ways_before = stock_ways;
    fake::put<int32_t>(presenter + t::title_presenter::cursor, 0);
    assert(!confirm(reinterpret_cast<void*>(presenter)) && stock_confirms == 1);
    assert(!back(reinterpret_cast<void*>(presenter)) && stock_backs == 1);
    assert(!direction(reinterpret_cast<void*>(presenter), 1) && stock_ways == ways_before);
    frame();
    assert(routed.size() == routed_before && chosen.size() == chosen_before);
    page_over = false;
    assert(confirm(reinterpret_cast<void*>(presenter)) && stock_confirms == 2);
    assert(back(reinterpret_cast<void*>(presenter)) && stock_backs == 2);
    assert(direction(reinterpret_cast<void*>(presenter), 1) && stock_ways == ways_before + 1);
    ui_title_cover(nullptr);
    tsuchi::cancel(mod, subscription);
    mod_stop(mod);
    frame();
}

void options_menu() {
    const uint64_t view_class = class_named(u"OptionsView");
    const uint64_t presenter = fake::object(u"Options", class_named(u"OptionsPresenter"), 0x200), widget = fake::object(u"OptionsView", view_class, 0x800);
    std::vector<uint64_t> instances;
    const uint64_t input = input_map(input_class, {{uint32_t(t::constant::options_up_key), {t::data::options_direction_delegate_table, reinterpret_cast<uint64_t>(&options_direction)}},
                                                   {uint32_t(t::constant::options_down_key), {t::data::options_direction_delegate_table, reinterpret_cast<uint64_t>(&options_direction)}},
                                                   {uint32_t(t::constant::title_confirm_key), {t::data::options_input_delegate_table, reinterpret_cast<uint64_t>(&options_confirm)}}}, instances);
    fake::put(presenter + t::options_presenter::view, widget);
    fake::put(presenter + t::options_presenter::input, input);
    box_in(widget);
    const uint64_t buttons = fake::memory(8 * 8);
    for (int i = 0; i < 4; ++i) fake::put(buttons + 8 * i, fake::object(u"Stock", button_class, 0x600));
    fake::put(widget + t::options_view::buttons, buttons);
    fake::put<int32_t>(widget + t::options_view::count, 4);
    fake::put<int32_t>(widget + t::options_view::room, 8);
    TSUCHI_Mod* mod = mod_create("options.mod");
    auto& backend = ui_screen_backend();
    assert(backend.hold_menu_entry(mod, TSUCHI_MENU_TITLE_OPTIONS, "first", "First", "") == TSUCHI_OK);
    assert(backend.hold_menu_entry(mod, TSUCHI_MENU_TITLE_OPTIONS, "second", "Second", "") == TSUCHI_OK);
    assert(backend.hold_menu_entry(mod, TSUCHI_MENU_TITLE_OPTIONS, "third", "Third", "") == TSUCHI_CAPACITY);
    options_tick(reinterpret_cast<void*>(presenter), 0.f);
    assert(fake::get<int32_t>(widget + t::options_view::count) == 6);
    const auto down = as<bool (*)(void*, bool)>(handler_function(instances[1]));
    const auto confirm = as<bool (*)(void*)>(handler_function(instances[2]));
    assert(down != &options_direction && confirm != &options_confirm);
    // Down from the last stock entry reaches the first mod entry, then the second.
    fake::put<int32_t>(widget + t::options_view::selected, int32_t(t::constant::options_last_stock));
    assert(down(reinterpret_cast<void*>(presenter), true) && fake::get<int32_t>(widget + t::options_view::selected) == 4);
    // Released: its button stays in the list until the menu is made again. The cursor passes over it and a press
    // on it is swallowed: the game's Confirm never sees a button it does not have.
    assert(backend.release_menu_entry(mod, TSUCHI_MENU_TITLE_OPTIONS, "first") == TSUCHI_OK);
    assert(confirm(reinterpret_cast<void*>(presenter)) && options_confirms == 0);
    fake::put<int32_t>(widget + t::options_view::selected, int32_t(t::constant::options_last_stock));
    assert(down(reinterpret_cast<void*>(presenter), true) && fake::get<int32_t>(widget + t::options_view::selected) == 5);
    assert(confirm(reinterpret_cast<void*>(presenter)) && options_confirms == 0);
    frame();
    // Its place is free for another entry; a mod that stops leaves its entry the same way.
    assert(backend.hold_menu_entry(mod, TSUCHI_MENU_TITLE_OPTIONS, "third", "Third", "") == TSUCHI_OK);
    mod_stop(mod);
    frame();
    fake::put<int32_t>(widget + t::options_view::selected, 5);
    assert(confirm(reinterpret_cast<void*>(presenter)) && options_confirms == 0);
    fake::put<int32_t>(widget + t::options_view::selected, 1);
    assert(confirm(reinterpret_cast<void*>(presenter)) && options_confirms == 1);
}

void player_choice() {
    TSUCHI_Mod* mod = mod_create("choice.mod");
    const uint64_t flow = fake::memory(0x200);
    for (uint64_t at : {t::data::dialog_answer_yes, t::data::dialog_answer_no, t::data::dialog_answer_cancel, t::data::dialog_answer_return})
        fake::put<uint64_t>(game::main_base() + at, 0x100 + at);
    auto& backend = game_service_backend();
    assert(flow_update && backend.hold_player_choice(mod) == TSUCHI_OK);
    const auto wait = [&] { fake::put<int32_t>(flow + t::title_flow::state, int32_t(t::constant::title_flow_players_wait)); };
    const auto player_answers = [&](uint64_t name) { fake::put<uint64_t>(flow + t::title_flow::answer, name); fake::put<uint8_t>(flow + t::title_flow::answered, 1); };
    wait();
    player_answers(0x100 + t::data::dialog_answer_no);
    flow_update(reinterpret_cast<void*>(flow), 0.f);
    assert(!fake::get<uint8_t>(flow + t::title_flow::answered)); // taken: the flow keeps waiting
    assert(backend.answer_player_choice(mod, TSUCHI_PLAYER_CHOICE_ONE) == TSUCHI_OK);
    // The flow leaves the question before its next update reads the answer: the answer is for that question only.
    fake::put<int32_t>(flow + t::title_flow::state, 0);
    flow_update(reinterpret_cast<void*>(flow), 0.f);
    assert(backend.answer_player_choice(mod, TSUCHI_PLAYER_CHOICE_ONE) == TSUCHI_NOT_READY);
    // The question comes back: nothing stale is written into it.
    wait();
    flow_update(reinterpret_cast<void*>(flow), 0.f);
    assert(!fake::get<uint8_t>(flow + t::title_flow::answered));
    // A fresh answer goes through as before.
    player_answers(0x100 + t::data::dialog_answer_yes);
    flow_update(reinterpret_cast<void*>(flow), 0.f);
    assert(backend.answer_player_choice(mod, TSUCHI_PLAYER_CHOICE_TWO) == TSUCHI_OK);
    flow_update(reinterpret_cast<void*>(flow), 0.f);
    assert(fake::get<uint8_t>(flow + t::title_flow::answered) == 1 && fake::get<uint64_t>(flow + t::title_flow::answer) == 0x100 + t::data::dialog_answer_no);
    mod_stop(mod);
}
} // namespace

int main() {
    note_game_thread();
    fake::start();
    // A larger image: the table's data for the title screen lies past the usual made-up one.
    const uint64_t small = fake::main_image(), large = fake::memory(0x20000);
    std::memcpy(reinterpret_cast<void*>(large), reinterpret_cast<const void*>(small), 0x4000);
    fake::main_image() = large;
    root_class = fake::object(u"Class", 0, 0x100);
    box_class = class_named(u"VerticalBox");
    input_class = class_named(u"InputMap");
    button_class = class_named(u"TitleButton");
    slot_class = class_named(u"VerticalBoxSlot");
    for (auto function : {reinterpret_cast<uint64_t>(&stock_confirm), reinterpret_cast<uint64_t>(&stock_back), reinterpret_cast<uint64_t>(&stock_direction)})
        fake::code().push_back(function);
    t::fn::title_confirm = stock_confirm; t::fn::title_back = stock_back; t::fn::title_direction = stock_direction;
    t::fn::options_confirm = options_confirm; t::fn::options_direction = options_direction; t::fn::options_gate = options_gate;
    t::fn::name_from_text = name_from_text; t::fn::load_soft_object = load_soft_object;
    t::fn::ui_create_widget = create_widget; t::fn::ui_add_child = add_child; t::fn::ui_vertical_slot_sync = slot_sync;
    t::fn::title_list_grow = list_grow; t::fn::options_list_grow = list_grow; t::fn::ui_remove_child_at = remove_child_at;
    t::fn::options_button_highlight = highlight;
    TSUCHI_Mod* framework = mod_create("tsuchi.self");
    ui_title_start(framework);
    player_choice_start(framework);
    assert(title_tick && options_tick && flow_update);
    title_menu();
    options_menu();
    player_choice();
    std::puts("title ok");
    return 0;
}
