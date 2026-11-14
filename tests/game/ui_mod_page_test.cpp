// The Mods page opened on its own from the title menu, against a made-up pause menu presenter laid out as
// tests/fixtures/game_table.yaml says. The game's three functions are test functions that record their
// calls, and the one that opens a page keeps the callable it is handed the way the game's own code does.
#include "fake_game.h"
#include "ui_mod_page.h"
#include "ui_screens.h"
#include "ui_title.h"
#include "frame.h"
#include "mods.h"
#include "objects.h"
#include "reflect.h"
#include "shared.h"
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

using namespace tsuchi;
namespace t = game::table;

namespace {
std::vector<void (*)()> frames;
std::vector<std::string> calls, notes;
bool title_ready = true, input_held = false;
UiScreen open_screen{};
bool screen_open = false;
bool (*title_cover)() = nullptr;
uint64_t screens = 0, presenter = 0, framework_object = 0, title_block = 0;
std::u16string made_text;
std::vector<std::u16string> titles;   // what was written to the header's text block
unsigned made = 0, freed = 0;
// A callable as the game holds one: an object whose first word is its table of functions.
struct Callable { void** table; uint64_t captured; };
struct Holder { uint64_t room[4]; Callable* object; uint64_t unused; };
Callable* kept = nullptr;

bool begun_gives_widget = true;
uint64_t menu_widget = 0;
void reset(void* object, uint32_t flag) {
    assert(reinterpret_cast<uint64_t>(object) == presenter);
    calls.push_back("reset " + std::to_string(flag));
}
void* find(void* manager, uint32_t number) {
    assert(reinterpret_cast<uint64_t>(manager) == screens);
    return number == uint32_t(t::constant::game_menu_presenter) ? reinterpret_cast<void*>(presenter) : nullptr;
}
// The presenter finds its widget when it is begun.
void begin(void* object, bool flag) {
    assert(reinterpret_cast<uint64_t>(object) == presenter && !flag);
    if (begun_gives_widget) fake::put<uint64_t>(presenter + t::game_menu_presenter::view, menu_widget);
    calls.push_back("begin");
}
// The game copies what it was handed and keeps the copy until the page has closed.
void open_page(void* object, void* handed, uint32_t page) {
    assert(reinterpret_cast<uint64_t>(object) == presenter);
    auto* holder = static_cast<Holder*>(handed);
    assert(holder->object && holder->object != reinterpret_cast<Callable*>(holder));
    using Copy = Callable* (*)(const Callable*);
    kept = reinterpret_cast<Copy>(holder->object->table[2])(holder->object);
    assert(kept && kept != holder->object && kept->table == holder->object->table);
    calls.push_back("open " + std::to_string(page));
}
void page_closed() {
    using Call = void (*)(Callable*);
    using Drop = void (*)(Callable*);
    reinterpret_cast<Call>(kept->table[6])(kept);
    reinterpret_cast<Drop>(kept->table[5])(kept);
    kept = nullptr;
}
void frame() { for (auto fn : frames) fn(); }
// The engine's text from a string, and a text block set: the block must be the header's.
game::EngineText text_from_string(const void* string) {
    const uint64_t data = fake::get<uint64_t>(reinterpret_cast<uint64_t>(string) + t::array::data);
    made_text = reinterpret_cast<const char16_t*>(data);
    return {};
}
void rich_text_set(void* block, void*) {
    assert(reinterpret_cast<uint64_t>(block) == title_block);
    titles.push_back(made_text);
}
// A reflected object member of a class: `name`, at `offset` in its objects.
void member(uint64_t type, const std::u16string& name, int32_t offset) {
    static uint64_t object_kind = 0;
    if (!object_kind) { object_kind = fake::memory(0x40); fake::put(object_kind, fake::name(u"ObjectProperty")); }
    const uint64_t field = fake::memory(t::property::size);
    fake::put(field + t::property::name, fake::name(name));
    fake::put<uint64_t>(field + t::property::kind, object_kind);
    fake::put<int32_t>(field + t::property::offset, offset);
    fake::put<uint64_t>(field + t::property::next, fake::get<uint64_t>(type + t::struct_type::properties));
    fake::put<uint64_t>(type + t::struct_type::properties, field);
}
}

namespace tsuchi {
bool each_engine_frame(void (*fn)()) { frames.push_back(fn); return true; }
void mod_note(const char*, const char* text) { notes.emplace_back(text); }
bool ui_title_menu_ready() { return title_ready; }
bool ui_menu_input_held() { return input_held; }
void ui_title_cover(bool (*covered)()) { title_cover = covered; }
const UiScreen* ui_active_screen() { return screen_open ? &open_screen : nullptr; }
namespace game {
void* engine_memory(uint64_t bytes) { ++made; return std::malloc(size_t(bytes)); }
void release_engine_memory(void* block) { ++freed; std::free(block); }
uint64_t game_framework() { return framework_object; }
void drop_shared(uint64_t) {}
// ASCII is enough for the title.
bool utf8_to_units(const char* text, char16_t* out, size_t room, size_t& count) {
    count = 0;
    for (; *text; ++text, ++count) { if (count >= room) return false; out[count] = char16_t(*text); }
    return true;
}
}
}

int main() {
    note_game_thread();
    fake::start();
    const uint64_t root_class = fake::object(u"Class", 0, 0x100);
    const uint64_t presenter_class = fake::object(u"GameMenuPresenter", root_class, 0x100);
    const uint64_t other_class = fake::object(u"OptionTitlePresenter", root_class, 0x100);
    const uint64_t widget_class = fake::object(u"GameMenuWidget", root_class, 0x100);
    framework_object = fake::memory(0x200);
    screens = fake::object(u"Screens", root_class, 0x100);
    fake::put<uint64_t>(framework_object + t::game_framework::ui, screens);
    const uint64_t functions = fake::memory(0x100);
    fake::put<uint64_t>(functions + t::presenter_table::reset.offset, reinterpret_cast<uint64_t>(&reset));
    fake::code().push_back(reinterpret_cast<uint64_t>(&reset));
    menu_widget = fake::object(u"Menu", widget_class, 0x100);
    const uint64_t header_class = fake::object(u"HeaderWidget", root_class, 0x100), block_class = fake::object(u"TextBlock", root_class, 0x100);
    const uint64_t header = fake::object(u"Header", header_class, 0x100);
    title_block = fake::object(u"HeaderText", block_class, 0x100);
    member(widget_class, u"Header", 0x90); fake::put<uint64_t>(menu_widget + 0x90, header);
    member(header_class, u"HeaderText", 0x90); fake::put<uint64_t>(header + 0x90, title_block);
    t::fn::text_from_string = text_from_string; t::fn::rich_text_set = rich_text_set;
    const auto make_presenter = [&](uint64_t kind) {
        const uint64_t object = fake::object(u"Presenter", kind, 0x200);
        fake::put<uint64_t>(object, functions);
        return object;
    };
    presenter = make_presenter(presenter_class);

    // Before the family starts, and on a build without the game's functions: unsupported.
    assert(!ui_mod_page_supported() && ui_mod_page_open() == TSUCHI_UNSUPPORTED);
    TSUCHI_Mod* framework = mod_create("tsuchi.self");
    ui_mod_page_start(framework);
    assert(title_cover && !title_cover() && !ui_mod_page_supported() && ui_mod_page_open() == TSUCHI_UNSUPPORTED);
    t::fn::ui_presenter_find = find; t::fn::presenter_begin = begin; t::fn::game_menu_open_page = open_page;
    assert(ui_mod_page_supported());
    // A mod that holds the menus' input for its own page covers the title menu too.
    input_held = true; assert(title_cover()); input_held = false; assert(!title_cover());

    // Away from the title menu, and with a screen open over it: not ready and busy, and nothing is called.
    title_ready = false; assert(ui_mod_page_open() == TSUCHI_NOT_READY);
    title_ready = true; screen_open = true; assert(ui_mod_page_open() == TSUCHI_BUSY);
    screen_open = false;
    assert(calls.empty() && notes.empty() && !ui_mod_page_alone());

    // What the game's own function would not survive is refused with a note of why.
    const uint64_t good = presenter;
    presenter = 0; assert(ui_mod_page_open() == TSUCHI_NOT_READY && notes.size() == 1);
    presenter = make_presenter(other_class); assert(ui_mod_page_open() == TSUCHI_NOT_READY && notes.size() == 2);
    presenter = make_presenter(presenter_class); fake::put<uint64_t>(presenter, fake::memory(0x100));
    assert(ui_mod_page_open() == TSUCHI_NOT_READY && notes.size() == 3 && notes[2].find("function table") != std::string::npos);
    assert(notes[0].find("Mods page not opened: ") == 0 && calls.empty());
    // A presenter that has no widget once begun: the page is not handed over, and nothing waits for it to close.
    presenter = make_presenter(presenter_class); begun_gives_widget = false;
    assert(ui_mod_page_open() == TSUCHI_NOT_READY && notes.size() == 4 && notes[3].find("no widget") != std::string::npos);
    assert(calls == (std::vector<std::string>{"reset 0", "begin"}));
    calls.clear(); begun_gives_widget = true;
    presenter = good;
    assert(!ui_mod_page_alone() && !title_cover() && !made);

    // The title's Options menu's three calls in its order, with the Settings page.
    assert(ui_mod_page_open() == TSUCHI_OK);
    assert(calls == (std::vector<std::string>{"reset 0", "begin", "open " + std::to_string(unsigned(t::constant::game_menu_settings_page))}));
    assert(ui_mod_page_alone() && title_cover() && made == 1 && !freed);
    // The header says Mods at once, and again over the page's first frames, then it is left alone.
    assert(titles.size() == 1 && titles[0] == u"Mods");
    // Open already: the same answer, and the game is not asked again.
    assert(ui_mod_page_open() == TSUCHI_OK && calls.size() == 3 && made == 1 && titles.size() == 1);
    frame(); assert(ui_mod_page_alone() && titles.size() == 2);
    for (int i = 0; i < 100; ++i) frame();
    const size_t written = titles.size();
    assert(written > 2 && written < 60 && titles.back() == u"Mods");
    frame(); assert(titles.size() == written);

    // The presenter copies its callable again for the page it shows, and lets go of that copy.
    {
        using Copy = Callable* (*)(const Callable*);
        using CopyTo = void (*)(const Callable*, Callable*);
        using End = void (*)(Callable*);
        Callable* second = reinterpret_cast<Copy>(kept->table[2])(kept);
        assert(second && second != kept && made == 2);
        reinterpret_cast<End>(second->table[1])(second); assert(freed == 1);
        Callable placed{};
        reinterpret_cast<CopyTo>(kept->table[3])(kept, &placed);
        assert(placed.table == kept->table);
        reinterpret_cast<End>(placed.table[0])(&placed); reinterpret_cast<End>(placed.table[4])(&placed);
        assert(freed == 1 && ui_mod_page_alone());
    }

    // The game says the page has closed: the title menu takes its presses again, and the copy is freed.
    page_closed();
    assert(!ui_mod_page_alone() && !title_cover() && made == 2 && freed == 2);
    frame(); assert(titles.size() == written);   // closed: the header is the game's again

    // A presenter that goes away without calling back ends it at the next frame.
    assert(ui_mod_page_open() == TSUCHI_OK && ui_mod_page_alone() && calls.size() == 6);
    frame(); assert(ui_mod_page_alone());
    fake::destroy(presenter);
    frame(); assert(!ui_mod_page_alone() && !title_cover());
    std::free(kept); kept = nullptr;

    // So does Tsuchi stopping.
    presenter = make_presenter(presenter_class);
    assert(ui_mod_page_open() == TSUCHI_OK && ui_mod_page_alone());
    mod_stop(framework);
    frame(); assert(!ui_mod_page_alone());
    assert(ui_mod_page_open() == TSUCHI_NOT_READY);
    std::free(kept);

    std::puts("mod page ok");
    return 0;
}
