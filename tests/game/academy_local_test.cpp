#include "fake_game.h"
#include "../../framework/game/academy_local.cpp"

#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "line %d: %s\n", __LINE__, #x); std::exit(1); } } while (0)

namespace {
using namespace tsuchi;
namespace t = game::table;
bool blocked = false, owned_schedule = false, fail_install = false;
uint64_t thread = 1;
int32_t status = 0;
uint32_t courses = 0, preflights = 0, refreshes = 0, callbacks = 0;
uint64_t empty_callback = 0, supplied_callback = 0;
uint64_t expected_launch_presenter = 0;
uint32_t retry_continuations = 0;
bool result_value = true;
bool viewport = true, advance_phase = false;
uint32_t visibility = 0, details_calls = 0, guards = 0;
TSUCHI_Result board_result = TSUCHI_OK, guard_result = TSUCHI_OK;
unsigned admissions = 0;
void stock_provider_decision(TSUCHI_InlineContext*) { ++admissions; }
void stock_details(void*, const void*, const void*, bool, bool) { ++details_calls; }
void stock_preflight(void*, const void*, bool) { ++preflights; }
void stock_refresh(void*, const void*, const void*) { ++refreshes; }
void completed(void*, void* reply) {
    const auto address = reinterpret_cast<uint64_t>(reply);
    CHECK(fake::get<uint32_t>(address + t::academy_local_reply::state) == t::constant::academy_local_refresh_success);
    CHECK(fake::get<uint64_t>(address + t::academy_local_reply::owned_data) == 0);
    CHECK(fake::get<uint64_t>(address + t::academy_local_reply::owned_data + 8) == 0);
    ++callbacks;
}
bool stock_course(void* presenter) {
    ++courses;
    preflight(nullptr, reinterpret_cast<void*>(empty_callback), false);
    schedule_refresh(nullptr, reinterpret_cast<void*>(supplied_callback), nullptr);
    if (presenter && advance_phase)
        fake::put<uint8_t>(reinterpret_cast<uint64_t>(presenter) + t::academy_local_presenter::phase, 99);
    return result_value;
}
}

namespace tsuchi {
void mod_note(const char*, const char*) {}
bool academy_mod_schedule() { return owned_schedule; }
bool academy_uploads_blocked() { return blocked; }
bool academy_course_matches(uint32_t course, uint64_t address) { return course == 0 && address == 1234; }
TSUCHI_Result academy_board_state(TSUCHI_Mod*, uint32_t course, TSUCHI_AcademyBoardState*) {
    return course >= 72 ? TSUCHI_INVALID : board_result;
}
TSUCHI_Result academy_block_uploads() { ++guards; return guard_result; }
namespace game {
uint64_t to_handle(uint64_t object) { return readable(object, 1) ? object : 0; }
uint64_t from_handle(uint64_t handle) { return readable(handle, 1) ? handle : 0; }
}
const NativeHooks* native_hooks_backend() {
    static const NativeHooks native{+[](TSUCHI_Mod*, uint64_t at, uint32_t word, void*, uint64_t* id, void** original) {
        CHECK(word == t::constant::academy_hook_pac);
        if (fail_install) return TSUCHI_BUSY;
        if (at == t::data::academy_local_course_refresh) *original = reinterpret_cast<void*>(stock_course);
        else if (at == t::data::academy_local_course_details) *original = reinterpret_cast<void*>(stock_details);
        else if (at == t::data::academy_local_preflight) *original = reinterpret_cast<void*>(stock_preflight);
        else { CHECK(at == t::data::academy_local_schedule_refresh); *original = reinterpret_cast<void*>(stock_refresh); }
        *id = at;
        return TSUCHI_OK;
    }, +[](TSUCHI_Mod*, uint64_t at, uint32_t word, void*, uint64_t* id, void** original) {
        CHECK((at == t::data::academy_local_provider_gate && word == t::constant::academy_local_provider_instruction) ||
              (at == t::data::academy_local_admission_gate && word == t::constant::academy_local_admission_instruction));
        if (fail_install) return TSUCHI_BUSY;
        *original = reinterpret_cast<void*>(stock_provider_decision); *id = at; return TSUCHI_OK;
    }, nullptr, nullptr, nullptr};
    return &native;
}
}

int main() {
    using namespace tsuchi;
    fake::start();
    set_thread_probe([] { return thread; }); note_game_thread();
    t::cells[t::data::academy_schedule.index] = 0x380;
    t::cells[t::data::academy_local_provider_gate.index] = 0x1400;
    t::cells[t::data::academy_local_admission_gate.index] = 0x1404;
    t::cells[t::academy_context::course.index] = 0x30;
    t::cells[t::academy_schedule::valid.index] = 4;
    t::cells[t::academy_schedule::interface.index] = 0x20;
    const uint64_t context = fake::memory(0x1000), manager = fake::memory(0x500);
    fake::put<uint64_t>(game::main_base() + t::data::game_context, context);
    fake::put<uint64_t>(game::main_base() + t::data::academy_schedule, manager);
    fake::put<uint8_t>(context + t::academy_context::course, 0);
    fake::put<uint8_t>(manager + t::academy_schedule::valid, 1);
    status = int32_t(t::constant::academy_status_open);
    t::fn::academy_status = +[](void*) { return status; };
    t::fn::ui_widget_in_viewport = +[](void*) { return viewport; };
    t::fn::ui_get_visibility = +[](void*) { return visibility; };
    empty_callback = fake::memory(64);
    supplied_callback = fake::memory(64);
    const uint64_t inner = fake::memory(32), methods = fake::memory(80);
    fake::put<uint64_t>(supplied_callback + t::academy_local_callback::target, inner);
    fake::put<uint64_t>(inner, methods);
    fake::put<uint64_t>(methods + t::academy_local_callback_table::invoke, reinterpret_cast<uint64_t>(completed));
    fake::code().push_back(reinterpret_cast<uint64_t>(completed));
    t::cells[t::data::academy_local_callback_table.index] = methods - game::main_base();
    t::cells[t::data::academy_local_callback_invoke.index] = reinterpret_cast<uint64_t>(completed) - game::main_base();
    fail_install = true; academy_local_hooks_start(nullptr); CHECK(!ready);
    fail_install = false; academy_local_hooks_start(nullptr); CHECK(ready);
    CHECK(course_refresh(nullptr) && courses == 1 && preflights == 1 && refreshes == 1 && callbacks == 0);
    blocked = true;
    CHECK(course_refresh(nullptr) && preflights == 2 && refreshes == 2);
    owned_schedule = true;
    status = int32_t(t::constant::academy_status_unavailable);
    CHECK(local_course());
    status = int32_t(t::constant::academy_status_open);
    CHECK(course_refresh(nullptr) && preflights == 2 && refreshes == 2 && callbacks == 1 && local_depth == 0);
    result_value = false;
    CHECK(!course_refresh(nullptr) && callbacks == 2 && local_depth == 0);
    result_value = true;
    // Calls outside the exact native course check continue to Nintendo unchanged.
    preflight(nullptr, reinterpret_cast<void*>(empty_callback), false);
    schedule_refresh(nullptr, reinterpret_cast<void*>(supplied_callback), nullptr);
    CHECK(preflights == 3 && refreshes == 3 && callbacks == 2);
    status = int32_t(t::constant::academy_status_ended);
    CHECK(course_refresh(nullptr) && preflights == 4 && refreshes == 4);
    status = int32_t(t::constant::academy_status_open);
    fake::put<uint8_t>(context + t::academy_context::course, 72);
    CHECK(local_course()); // an unset/stale gameplay course cannot disqualify the frontend
    owned_schedule = false;
    CHECK(course_refresh(nullptr) && preflights == 5 && refreshes == 5);
    owned_schedule = true;
    fake::put<uint8_t>(context + t::academy_context::course, 0);
    fake::put<uint8_t>(manager + t::academy_schedule::valid, 0);
    CHECK(course_refresh(nullptr) && preflights == 6 && refreshes == 6);
    fake::put<uint8_t>(manager + t::academy_schedule::valid, 1);
    thread = 2;
    CHECK(course_refresh(nullptr) && preflights == 7 && refreshes == 7);
    thread = 1;
    // A changed callback table, unknown callback, or unreadable request is forwarded.
    fake::put<uint64_t>(inner, methods + 8);
    CHECK(course_refresh(nullptr) && refreshes == 8 && callbacks == 2);
    fake::put<uint64_t>(inner, methods);
    t::cells[t::data::academy_local_callback_invoke.index] += 4;
    CHECK(course_refresh(nullptr) && refreshes == 9 && callbacks == 2);
    t::cells[t::data::academy_local_callback_invoke.index] -= 4;
    fake::put<uint64_t>(empty_callback + t::academy_local_callback::target, inner);
    CHECK(course_refresh(nullptr) && preflights == 8 && callbacks == 3);
    fake::put<uint64_t>(empty_callback + t::academy_local_callback::target, 0);
    // Opening a supplied chart requires this owner's matching, idle course screen.
    CHECK(academy_open_board(nullptr, 72) == TSUCHI_INVALID);
    board_result = TSUCHI_NOT_FOUND;
    CHECK(academy_open_board(nullptr, 0) == TSUCHI_NOT_FOUND);
    board_result = TSUCHI_DENIED;
    CHECK(academy_open_board(nullptr, 0) == TSUCHI_DENIED);
    board_result = TSUCHI_OK;
    CHECK(academy_open_board(nullptr, 0) == TSUCHI_NOT_READY);
    const uint64_t presenter = fake::memory(0x100), widget = fake::memory(0x80);
    fake::put<uint64_t>(presenter + t::academy_local_presenter::widget, widget);
    fake::put<uint8_t>(presenter + t::academy_local_presenter::phase, uint8_t(t::constant::academy_local_idle_phase));
    course_details(reinterpret_cast<void*>(presenter), nullptr, reinterpret_cast<void*>(1234), true, false);
    CHECK(details_calls == 1 && course_presenter == presenter);
    CHECK(academy_open_board(nullptr, 1) == TSUCHI_NOT_READY && guards == 0);
    viewport = false;
    CHECK(academy_open_board(nullptr, 0) == TSUCHI_NOT_READY);
    viewport = true; visibility = 1;
    CHECK(academy_open_board(nullptr, 0) == TSUCHI_NOT_READY);
    visibility = 0;
    fake::put<uint8_t>(context + t::academy_context::course, 255);
    fake::put<uint8_t>(presenter + t::academy_local_presenter::busy, 1);
    CHECK(academy_open_board(nullptr, 0) == TSUCHI_BUSY);
    fake::put<uint8_t>(presenter + t::academy_local_presenter::busy, 0);
    fake::put<uint8_t>(presenter + t::academy_local_presenter::phase, 0);
    CHECK(academy_open_board(nullptr, 0) == TSUCHI_BUSY);
    fake::put<uint8_t>(presenter + t::academy_local_presenter::phase, uint8_t(t::constant::academy_local_idle_phase));
    guard_result = TSUCHI_BUSY;
    CHECK(academy_open_board(nullptr, 0) == TSUCHI_BUSY && guards == 1);
    guard_result = TSUCHI_OK; result_value = false;
    CHECK(academy_open_board(nullptr, 0) == TSUCHI_NOT_READY && guards == 2);
    CHECK(fake::get<uint8_t>(presenter + t::academy_local_presenter::phase) == t::constant::academy_local_idle_phase);
    advance_phase = true;
    CHECK(academy_open_board(nullptr, 0) == TSUCHI_NOT_READY);
    CHECK(fake::get<uint8_t>(presenter + t::academy_local_presenter::phase) == 99);
    result_value = true; // a refresh that starts moves the screen on, as the game's does
    fake::put<uint8_t>(presenter + t::academy_local_presenter::phase, uint8_t(t::constant::academy_local_idle_phase));
    CHECK(academy_open_board(nullptr, 0) == TSUCHI_OK && callbacks == 6);
    CHECK(fake::get<uint8_t>(presenter + t::academy_local_presenter::phase) == 99);
    CHECK(academy_open_board(nullptr, 0) == TSUCHI_BUSY);
    // The exact exam-start preflight continues the native launch action only
    // for this live presenter and a validated owned schedule.
    uint32_t launches = 0;
    static uint32_t* launch_count = &launches;
    expected_launch_presenter = presenter;
    t::fn::academy_local_launch_ready = +[](void* screen, uint64_t action) {
        CHECK(reinterpret_cast<uint64_t>(screen) == expected_launch_presenter && action == 123);
        ++*launch_count;
    };
    t::cells[t::data::academy_local_launch_action.index] = 0x398;
    fake::put<uint64_t>(game::main_base() + t::data::academy_local_launch_action, 123);
    t::cells[t::data::academy_local_launch_callback_table.index] = methods - game::main_base();
    t::cells[t::data::academy_local_launch_callback_invoke.index] = reinterpret_cast<uint64_t>(completed) - game::main_base();
    fake::put<uint64_t>(inner + t::academy_local_callback::presenter, presenter);
    fake::put<uint8_t>(context + t::academy_context::course, 255);
    preflight(nullptr, reinterpret_cast<void*>(supplied_callback), false);
    CHECK(launches == 1);
    fake::put<uint8_t>(context + t::academy_context::course, 0);
    const auto forwarded_preflights = preflights;
    preflight(nullptr, reinterpret_cast<void*>(supplied_callback), true);
    CHECK(launches == 1 && preflights == forwarded_preflights + 1);
    fake::put<uint64_t>(inner + t::academy_local_callback::presenter, presenter + 8);
    preflight(nullptr, reinterpret_cast<void*>(supplied_callback), false);
    CHECK(launches == 1 && preflights == forwarded_preflights + 2);
    fake::put<uint64_t>(inner + t::academy_local_callback::presenter, presenter);
    owned_schedule = false;
    preflight(nullptr, reinterpret_cast<void*>(supplied_callback), false);
    CHECK(launches == 1 && preflights == forwarded_preflights + 3);
    owned_schedule = true;
    t::cells[t::data::academy_local_launch_callback_invoke.index] += 4;
    preflight(nullptr, reinterpret_cast<void*>(supplied_callback), false);
    CHECK(launches == 1 && preflights == forwarded_preflights + 4);
    t::cells[t::data::academy_local_launch_callback_invoke.index] -= 4;
    const auto reopened_presenter = fake::memory(0x100);
    fake::put<uint64_t>(reopened_presenter + t::academy_local_presenter::widget, widget);
    fake::put<uint64_t>(inner + t::academy_local_callback::presenter, reopened_presenter);
    expected_launch_presenter = reopened_presenter;
    preflight(nullptr, reinterpret_cast<void*>(supplied_callback), false);
    CHECK(launches == 2 && course_presenter == presenter); // a world reload can replace the captured presenter
    viewport = false;
    preflight(nullptr, reinterpret_cast<void*>(supplied_callback), false);
    CHECK(launches == 2);
    viewport = true;
    const auto before_empty_probe = preflights;
    preflight(nullptr, reinterpret_cast<void*>(empty_callback), false);
    CHECK(preflights == before_empty_probe);
    uint64_t x[31]{};
    TSUCHI_InlineContext registers{};
    registers.registers = x;
    x[19] = reopened_presenter;
    x[8] = 3;
    provider_decision(&registers); CHECK(launches == 2 && x[8] == 2 && admissions == 1);
    owned_schedule = false; x[8] = 3;
    provider_decision(&registers); CHECK(launches == 2 && x[8] == 3);
    owned_schedule = true; thread = 2;
    provider_decision(&registers); CHECK(launches == 2 && x[8] == 3);
    thread = 1; viewport = false;
    provider_decision(&registers); CHECK(launches == 2 && x[8] == 3);
    viewport = true;
    x[0] = 0;
    admission(&registers); CHECK(launches == 2 && x[0] == 0);
    x[0] = 3;
    admission(&registers); CHECK(launches == 3 && x[0] == 2);
    owned_schedule = false; x[0] = 3;
    admission(&registers); CHECK(launches == 3 && x[0] == 3);
    owned_schedule = true; viewport = false;
    admission(&registers); CHECK(launches == 3 && x[0] == 3);
    viewport = true;
    t::cells[t::data::academy_local_retry_callback_table.index] = methods - game::main_base();
    t::cells[t::data::academy_local_retry_callback_invoke.index] = reinterpret_cast<uint64_t>(completed) - game::main_base();
    t::cells[t::data::academy_local_launch_callback_invoke.index] += 4;
    t::fn::academy_local_retry_ready = +[](void* screen) {
        CHECK(reinterpret_cast<uint64_t>(screen) == expected_launch_presenter);
        ++retry_continuations;
    };
    preflight(nullptr, reinterpret_cast<void*>(supplied_callback), false);
    CHECK(retry_continuations == 1 && launches == 3);
    preflight(nullptr, reinterpret_cast<void*>(supplied_callback), true);
    CHECK(retry_continuations == 1);
    owned_schedule = false;
    preflight(nullptr, reinterpret_cast<void*>(supplied_callback), false);
    CHECK(retry_continuations == 1);
    owned_schedule = true;
    t::cells[t::data::academy_local_retry_callback_invoke.index] += 4;
    preflight(nullptr, reinterpret_cast<void*>(supplied_callback), false);
    CHECK(retry_continuations == 1);
    fake::unmap(presenter);
    CHECK(academy_open_board(nullptr, 0) == TSUCHI_NOT_READY);
    fake::unmap(supplied_callback);
    CHECK(course_refresh(nullptr) && refreshes == 10 && callbacks == 6);
    CHECK(local_depth == 0);
    std::puts("academy owned schedule refresh checks passed");
}
