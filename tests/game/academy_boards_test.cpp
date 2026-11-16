// A mod's board, given to a made-up game the way the game's own download gives one: stored in the schedule
// manager with the game's calls, and the board's request answered.
#include <string>
#include <vector>
#include "fake_game.h"
#include "../../framework/game/academy_boards.cpp"

#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "line %d: %s\n", __LINE__, #x); std::exit(1); } } while (0)

namespace {
using namespace tsuchi;
TSUCHI_Result guard_result = TSUCHI_OK;
bool uploads_blocked = false;
uint64_t thread = 1;
void (*request_hook)(void*, const void*, uint32_t, const void*) = nullptr;
bool (*apply_hook)(void*, void*, const void*) = nullptr;
uint8_t style_during = 0;
bool change_style = false;
bool native_apply(void* chart, void*, const void*) {
    const auto at = reinterpret_cast<uint64_t>(chart) + t::academy_chart::face_enabled;
    style_during = fake::get<uint8_t>(at);
    if (change_style) fake::put<uint8_t>(at, 0x80);
    return true;
}
uint32_t native_requests = 0, slots = 0, copies = 0, links = 0, answers = 0;
uint64_t boards_set = 0, element = 0, callback_target = 0;

// What the made-up game was given, read back out of its own records.
struct Given {
    std::vector<std::u16string> names, ids;
    std::vector<int32_t> scores, kings, types, colors;
    uint32_t key = 0, link_key = 0, state = 0;
    int32_t next = 0, link_index = 0;
    uint8_t answered = 0;
} given;

template <class T>
T field(const void* input, uint64_t offset) {
    T value{};
    memcpy(&value, static_cast<const uint8_t*>(input) + offset, sizeof value);
    return value;
}
std::u16string text_of(const void* record, uint64_t at) {
    const auto* units = reinterpret_cast<const char16_t*>(field<uint64_t>(record, at + t::array::data));
    const int32_t count = field<int32_t>(record, at + t::array::count);
    CHECK(units && count > 1 && units[count - 1] == 0 && field<int32_t>(record, at + t::array::max) == count);
    return std::u16string(units, size_t(count - 1));
}

void native_request(void*, const void*, uint32_t, const void*) { ++native_requests; }
game::EngineSlot native_slot(void* set) {
    CHECK(reinterpret_cast<uint64_t>(set) == boards_set);
    ++slots;
    element = fake::memory(0x40);
    memset(reinterpret_cast<void*>(element), 0xcc, 0x40); // a new element is not cleared
    return {7, reinterpret_cast<void*>(element)};
}
void native_copy(void* list, const void* records, uint32_t count) {
    namespace m = t::academy_member;
    const uint64_t at = reinterpret_cast<uint64_t>(list);
    CHECK(at == element + t::academy_board_list::members && fake::get<uint64_t>(at + t::array::data) == 0);
    ++copies;
    given = Given{};
    for (uint32_t i = 0; i < count; ++i) {
        const auto* record = static_cast<const uint8_t*>(records) + i * uint64_t(m::size);
        given.names.push_back(text_of(record, m::nickname));
        given.ids.push_back(text_of(record, m::id));
        given.scores.push_back(field<int32_t>(record, m::score));
        given.kings.push_back(field<int32_t>(record, m::kings));
        given.types.push_back(field<int32_t>(record, m::type));
        given.colors.push_back(field<int32_t>(record, m::color));
    }
    fake::put<int32_t>(at + t::array::count, int32_t(count));
    fake::put<int32_t>(at + t::array::max, int32_t(count));
}
void native_link(void* set, uint32_t key, void* linked, int32_t index) {
    CHECK(reinterpret_cast<uint64_t>(set) == boards_set && reinterpret_cast<uint64_t>(linked) == element && copies == slots);
    ++links;
    given.link_key = key;
    given.link_index = index;
    given.key = fake::get<uint32_t>(element + t::academy_board_list::key);
    given.next = fake::get<int32_t>(element + t::academy_board_list::next);
}
void board_callback(void* target, void* reply) {
    CHECK(reinterpret_cast<uint64_t>(target) == callback_target && links == slots); // the entries are stored first
    ++answers;
    given.state = field<uint32_t>(reply, t::academy_board_reply::state);
    given.answered = field<uint8_t>(reply, t::academy_board_reply::answered);
}

const char* first_json = R"({"entries":[
 {"id":"seed","nickname":"Seed","score":400,"sticker_type":1,"sticker_color":2},
 {"id":"rival.2","nickname":"Rival","score":500,"sticker_type":2,"sticker_color":1,"king_count":3}]})";
const char* second_json = R"({"entries":[{"id":"seed","nickname":"Changed","score":600,"sticker_type":1,"sticker_color":2}]})";
}

namespace tsuchi {
void mod_note(const char*, const char*) {}
TSUCHI_Result academy_block_uploads() { if (guard_result == TSUCHI_OK) uploads_blocked = true; return guard_result; }
bool academy_uploads_blocked() { return uploads_blocked; }
void academy_unblock_uploads() { uploads_blocked = false; }
const NativeHooks* native_hooks_backend() {
    static const NativeHooks backend{
        [](TSUCHI_Mod*, uint64_t at, uint32_t expected, void* fn, uint64_t* id, void** original) -> TSUCHI_Result {
            CHECK(expected == t::constant::academy_hook_pac);
            if (at == t::data::academy_board_request) {
                request_hook = reinterpret_cast<decltype(request_hook)>(fn);
                *original = reinterpret_cast<void*>(native_request);
            } else {
                CHECK(at == t::data::academy_board_apply);
                apply_hook = reinterpret_cast<decltype(apply_hook)>(fn);
                *original = reinterpret_cast<void*>(native_apply);
            }
            *id = 2;
            return TSUCHI_OK;
        }, nullptr, nullptr, nullptr, nullptr};
    return &backend;
}
namespace game {
bool utf8_to_units(const char* text, char16_t* out, size_t capacity, size_t& count) {
    // This fixture uses ASCII names. The reflection tests check the real conversion.
    count = strlen(text);
    for (size_t i = 0; i < count; ++i) {
        if (uint8_t(text[i]) > 127) return false;
        if (i < capacity) out[i] = char16_t(text[i]);
    }
    return true;
}
}
}

int main() {
    fake::start();
    set_thread_probe([] { return thread; });
    note_game_thread();
    const auto cell = [](auto key, uint64_t value) { t::cells[key.index] = value; };
    cell(t::data::academy_board_request, 0x500);
    cell(t::data::academy_board_apply, 0x520);
    cell(t::data::academy_schedule, 0x3b0); cell(t::data::game_context, 0x3a0); cell(t::data::game_framework, 0x3a8);
    cell(t::academy_schedule::interface, 0x10); cell(t::academy_schedule::boards, 0x60);
    cell(t::academy_context::course, 0x20); cell(t::academy_context::board_key, 0x24);
    cell(t::game_framework::ui, 0x18);
    cell(t::academy_sticker_data::mask_count, 4); cell(t::academy_sticker_data::color_count, 8);
    cell(t::academy_member::nickname, 8); cell(t::academy_member::id, 32);
    cell(t::academy_member::score, 56); cell(t::academy_member::kings, 60);
    cell(t::academy_member::type, 64); cell(t::academy_member::color, 68);
    cell(t::academy_board_list::size, 0x28); cell(t::academy_board_list::key, 4);
    cell(t::academy_board_list::members, 0x10); cell(t::academy_board_list::next, 0x20);
    cell(t::academy_board_reply::size, 56); cell(t::academy_board_reply::state, 8); cell(t::academy_board_reply::answered, 44);
    const uint64_t context = fake::memory(0x80), framework = fake::memory(0x80), ui = fake::memory(0x80), manager = fake::memory(0x200);
    fake::put<uint64_t>(fake::main_image() + t::data::game_context, context);
    fake::put<uint64_t>(fake::main_image() + t::data::game_framework, framework);
    fake::put<uint64_t>(fake::main_image() + t::data::academy_schedule, manager);
    fake::put<uint64_t>(framework + t::game_framework::ui, ui);
    fake::put<uint8_t>(context + t::academy_context::course, 8);
    fake::put<int32_t>(context + t::academy_context::board_key, 1008);
    fake::put<int32_t>(ui + t::academy_sticker_data::mask_count, 4);
    fake::put<int32_t>(ui + t::academy_sticker_data::color_count, 3);
    boards_set = manager + t::academy_schedule::boards;
    void* const interface = reinterpret_cast<void*>(manager + t::academy_schedule::interface);
    // The board's callback, as the game's standard function object keeps one: a target with a table of functions.
    const uint64_t callback = fake::memory(0x40), methods = fake::memory(0x80);
    callback_target = fake::memory(0x20);
    fake::put<uint64_t>(callback + t::academy_local_callback::target, callback_target);
    fake::put<uint64_t>(callback_target, methods);
    fake::put<uint64_t>(methods + t::academy_local_callback_table::invoke, reinterpret_cast<uint64_t>(board_callback));
    fake::code().push_back(reinterpret_cast<uint64_t>(board_callback));
    const auto ask = [&](uint32_t key) { request_hook(interface, reinterpret_cast<void*>(callback), key, nullptr); };
    t::fn::academy_board_slot = native_slot;
    t::fn::academy_board_copy = native_copy;
    t::fn::academy_board_link = native_link;

    auto* owner = mod_create("academy-community-board");
    auto* competitor = mod_create("academy-ghost-board");
    CHECK(academy_hold_board(owner, 8, first_json) == TSUCHI_NOT_READY);
    // A record of the game's that does not fit the room kept for it leaves the service off.
    cell(t::academy_member::size, 200);
    academy_board_hooks_start(owner);
    CHECK(!ready && !request_hook);
    cell(t::academy_member::size, 88);
    academy_board_hooks_start(owner);
    CHECK(ready && request_hook);

    // Holding: one owner a course, a refused request keeps what was held, and uploads are blocked only by a hold.
    guard_result = TSUCHI_BUSY;
    CHECK(academy_hold_board(owner, 8, first_json) == TSUCHI_BUSY && !uploads_blocked);
    guard_result = TSUCHI_OK;
    CHECK(academy_hold_board(owner, 8, "{}") == TSUCHI_INVALID && !uploads_blocked && !boards[8]);
    CHECK(academy_hold_board(owner, 8, first_json) == TSUCHI_OK && uploads_blocked && owned_count(owner) == 1);
    TSUCHI_AcademyBoardState state{};
    CHECK(academy_board_state(owner, 8, &state) == TSUCHI_OK && state.entries == 2 && state.pending && !state.rendered && !*state.failure);
    CHECK(academy_hold_board(competitor, 8, first_json) == TSUCHI_BUSY);
    CHECK(academy_release_board(competitor, 8) == TSUCHI_DENIED && academy_board_state(competitor, 8, &state) == TSUCHI_DENIED);
    CHECK(academy_board_state(owner, 9, &state) == TSUCHI_NOT_FOUND && academy_board_state(owner, 72, &state) == TSUCHI_INVALID);
    const Board* held = boards[8];
    const std::string too_long(65537, ' ');
    CHECK(academy_hold_board(owner, 72, first_json) == TSUCHI_INVALID);
    CHECK(academy_hold_board(owner, 8, too_long.c_str()) == TSUCHI_INVALID);
    CHECK(academy_hold_board(owner, 8, R"({"entries":[{"id":"a","nickname":"A","score":1,"sticker_type":64,"sticker_color":0}]})") == TSUCHI_INVALID);
    CHECK(academy_hold_board(owner, 8, R"({"entries":[{"id":"a b","nickname":"A","score":1,"sticker_type":0,"sticker_color":0}]})") == TSUCHI_INVALID);
    CHECK(academy_hold_board(owner, 8, R"({"entries":[{"id":"a","nickname":"A","score":1,"sticker_type":0,"sticker_color":0},
        {"id":"a","nickname":"B","score":2,"sticker_type":0,"sticker_color":0}]})") == TSUCHI_INVALID);
    CHECK(boards[8] == held && owned_count(owner) == 1);
    const auto floor = isolate_ownership(owner, ownership_mark(owner));
    CHECK(academy_hold_board(owner, 8, first_json) == TSUCHI_BUSY && academy_release_board(owner, 8) == TSUCHI_BUSY);
    isolate_ownership(owner, floor);

    // The game's request is left to the game for another course's key, from another thread, and for a manager or
    // a callback that is not what the request is known to take.
    ask(1009);
    thread = 2; ask(1008); thread = 1;
    request_hook(reinterpret_cast<void*>(manager), reinterpret_cast<void*>(callback), 1008, nullptr);
    fake::put<uint64_t>(callback + t::academy_local_callback::target, 0);
    ask(1008);
    fake::put<uint64_t>(callback + t::academy_local_callback::target, callback_target);
    CHECK(native_requests == 4 && !slots && !answers);
    CHECK(academy_board_state(owner, 8, &state) == TSUCHI_OK && !state.pending && !state.rendered && !strcmp(state.failure, "not_ready"));

    // So is a board whose stickers the game has not loaded, or does not have.
    fake::put<int32_t>(ui + t::academy_sticker_data::mask_count, 0);
    ask(1008);
    CHECK(native_requests == 5 && !slots && !strcmp(boards[8]->failure, "not_ready"));
    fake::put<int32_t>(ui + t::academy_sticker_data::mask_count, 4);
    fake::put<int32_t>(ui + t::academy_sticker_data::color_count, 2);
    ask(1008);
    CHECK(native_requests == 6 && !slots && !strcmp(boards[8]->failure, "invalid"));
    fake::put<int32_t>(ui + t::academy_sticker_data::color_count, 3);

    // The selected course's board: stored with the game's three calls in the game's order, then answered.
    ask(1008);
    CHECK(native_requests == 6 && slots == 1 && copies == 1 && links == 1 && answers == 1);
    CHECK(given.key == 1008 && given.link_key == 1008 && given.link_index == 7 && given.next == -1);
    CHECK(given.state == t::constant::academy_board_answered && given.answered == 1);
    CHECK(given.names == std::vector<std::u16string>({u"Seed", u"Rival"}));
    CHECK(given.ids == std::vector<std::u16string>({u"tsuchi.local.seed", u"tsuchi.local.rival.2"}));
    CHECK(given.scores == std::vector<int32_t>({400, 500}) && given.kings == std::vector<int32_t>({0, 3}));
    CHECK(given.types == std::vector<int32_t>({1, 2}) && given.colors == std::vector<int32_t>({2, 1}));
    CHECK(academy_board_state(owner, 8, &state) == TSUCHI_OK && !state.pending && state.rendered == 2 && !*state.failure);

    // A new request waits for the game to ask again, and each time the game asks it is given what is held then.
    CHECK(academy_hold_board(owner, 8, second_json) == TSUCHI_OK && owned_count(owner) == 1);
    CHECK(academy_board_state(owner, 8, &state) == TSUCHI_OK && state.entries == 1 && state.pending && !state.rendered);
    ask(1008);
    CHECK(slots == 2 && answers == 2 && given.names == std::vector<std::u16string>({u"Changed"}) && given.scores == std::vector<int32_t>({600}));
    CHECK(academy_hold_board(owner, 8, R"({"entries":[]})") == TSUCHI_OK);
    ask(1008);
    CHECK(slots == 3 && answers == 3 && given.names.empty() && fake::get<int32_t>(element + t::academy_board_list::members + t::array::count) == 0);
    CHECK(academy_board_state(owner, 8, &state) == TSUCHI_OK && !state.pending && !state.rendered && !*state.failure);

    // Another course is its own board, and release or the mod's stop leaves the request to the game again.
    CHECK(academy_hold_board(competitor, 9, first_json) == TSUCHI_OK);
    fake::put<uint8_t>(context + t::academy_context::course, 9);
    fake::put<int32_t>(context + t::academy_context::board_key, 1009);
    ask(1009);
    CHECK(slots == 4 && answers == 4 && given.key == 1009 && given.names.size() == 2);
    CHECK(academy_release_board(competitor, 9) == TSUCHI_OK && owned_count(competitor) == 0 && !boards[9]);
    ask(1009);
    CHECK(native_requests == 7 && slots == 4);
    CHECK(academy_release_board(competitor, 9) == TSUCHI_NOT_FOUND);
    fake::put<uint8_t>(context + t::academy_context::course, 8);
    fake::put<int32_t>(context + t::academy_context::board_key, 1008);
    mod_stop(owner);
    CHECK(!boards[8]);
    ask(1008);
    CHECK(native_requests == 8 && slots == 4 && answers == 4);
    fake::put<uint8_t>(context + t::academy_context::course, 75); // no course is selected
    ask(1008);
    CHECK(native_requests == 9);
    // Cached local entries keep their mask style even after the hold ends.
    // Nintendo entries and other threads keep the chart's original face mode.
    const uint64_t chart = fake::memory(0x80), member = fake::memory(0x80);
    constexpr char16_t local_id[] = u"tsuchi.local.seed";
    const uint64_t id_text = fake::memory(sizeof local_id);
    memcpy(reinterpret_cast<void*>(id_text), local_id, sizeof local_id);
    fake::put<uint64_t>(member + t::academy_member::id + t::array::data, id_text);
    fake::put<int32_t>(member + t::academy_member::id + t::array::count, sizeof(local_id) / 2);
    fake::put<uint8_t>(chart + t::academy_chart::face_enabled, 0x83);
    CHECK(apply_hook(reinterpret_cast<void*>(chart), nullptr, reinterpret_cast<void*>(member)) && style_during == 0x82);
    CHECK(fake::get<uint8_t>(chart + t::academy_chart::face_enabled) == 0x83);
    change_style = true;
    CHECK(apply_hook(reinterpret_cast<void*>(chart), nullptr, reinterpret_cast<void*>(member)));
    CHECK(fake::get<uint8_t>(chart + t::academy_chart::face_enabled) == 0x81);
    change_style = false;
    fake::put<uint8_t>(chart + t::academy_chart::face_enabled, 0x83);
    fake::put<char16_t>(id_text, u'n');
    CHECK(apply_hook(reinterpret_cast<void*>(chart), nullptr, reinterpret_cast<void*>(member)) && style_during == 0x83);
    fake::put<char16_t>(id_text, u't'); thread = 2;
    CHECK(apply_hook(reinterpret_cast<void*>(chart), nullptr, reinterpret_cast<void*>(member)) && style_during == 0x83);
    thread = 1;
    std::puts("academy native board ownership checks passed");
}
