#include <limits>
#include "fake_game.h"
#include "families.h"
#include "../../framework/game/academy_events.cpp"

#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "line %d: %s\n", __LINE__, #x); std::exit(1); } } while (0)

namespace {
using namespace tsuchi;
namespace t = game::table;
void (*frame_work)() = nullptr;
void (*wrapped_update)(void*, float) = nullptr;
void (*wrapped_finish)(TSUCHI_InlineContext*) = nullptr;
void (*wrapped_points_start)(TSUCHI_InlineContext*) = nullptr;
void (*wrapped_time_start)(TSUCHI_InlineContext*) = nullptr;
uint32_t updates = 0, events = 0, visibility = 0;
uint32_t continued_finishes = 0;
uint32_t continued_points = 0, continued_times = 0, exam_checks = 0;
bool exam_mode = true;
std::vector<TSUCHI_AcademyExamFinished> exam_events;
std::vector<TSUCHI_AcademyExamStarted> start_events;
std::vector<char> attempt_order;
uint64_t thread = 1, expected_state = 0;
float last_seconds = 0;
bool viewport = false;
TSUCHI_Result install_result = TSUCHI_NOT_READY;
void stock_update(void* state, float seconds) {
    CHECK(reinterpret_cast<uint64_t>(state) == expected_state);
    last_seconds = seconds;
    ++updates;
}
}

namespace tsuchi {
void mod_note(const char*, const char*) {}
void academy_prepare_score_submission() {}
bool each_engine_frame(void (*work)()) { CHECK(!frame_work); frame_work = work; return true; }
const NativeHooks* native_hooks_backend() {
    static const NativeHooks native{nullptr, [](TSUCHI_Mod*, uint64_t at, uint32_t word, void* handler, uint64_t* id, void** original) {
        const bool finish = at == t::data::academy_exam_finish_site;
        CHECK(finish || at == t::data::academy_exam_points_start_site || at == t::data::academy_exam_time_start_site);
        CHECK(word == (finish ? t::constant::academy_exam_finish_instruction : t::constant::academy_exam_start_instruction));
        if (install_result != TSUCHI_OK) return install_result;
        if (finish) {
            wrapped_finish = reinterpret_cast<void (*)(TSUCHI_InlineContext*)>(handler);
            *original = reinterpret_cast<void*>(+[](TSUCHI_InlineContext*) { ++continued_finishes; });
        } else if (at == t::data::academy_exam_points_start_site) {
            wrapped_points_start = reinterpret_cast<void (*)(TSUCHI_InlineContext*)>(handler);
            *original = reinterpret_cast<void*>(+[](TSUCHI_InlineContext*) { ++continued_points; });
        } else {
            wrapped_time_start = reinterpret_cast<void (*)(TSUCHI_InlineContext*)>(handler);
            *original = reinterpret_cast<void*>(+[](TSUCHI_InlineContext*) { ++continued_times; });
        }
        *id = at;
        return TSUCHI_OK;
    }, nullptr, nullptr, nullptr};
    return &native;
}
TSUCHI_Result hook_wrap(TSUCHI_Mod*, uint64_t entry, uint64_t stock, void* function,
                        uint32_t flags, uint64_t* id, void** original) {
    CHECK(entry == game::main_base() + t::data::academy_menu_update_entry);
    CHECK(stock == reinterpret_cast<uint64_t>(stock_update));
    CHECK(flags == (kHookFirst | kHookNeedsStock));
    if (install_result != TSUCHI_OK) return install_result;
    wrapped_update = reinterpret_cast<void (*)(void*, float)>(function);
    *id = 7;
    *original = reinterpret_cast<void*>(stock_update);
    return TSUCHI_OK;
}
namespace game {
uint64_t to_handle(uint64_t object) { return object && readable(object, 0x80) ? object : 0; }
uint64_t from_handle(uint64_t handle) { return to_handle(handle); }
}
}

int main() {
    fake::start();
    set_thread_probe([] { return thread; });
    note_game_thread();
    t::cells[t::data::academy_menu_update_entry.index] = 0x380;
    t::cells[t::data::academy_exam_finish_site.index] = 0x400;
    t::cells[t::data::academy_exam_points_start_site.index] = 0x404;
    t::cells[t::data::academy_exam_time_start_site.index] = 0x408;
    t::cells[t::academy_menu_state::parent.index] = 0x20;
    t::cells[t::academy_menu_presenter::widget.index] = 0x30;
    t::fn::academy_menu_update_stock = stock_update;
    t::fn::ui_widget_in_viewport = [](void*) { return viewport; };
    t::fn::ui_get_visibility = [](void*) { return visibility; };
    t::fn::academy_is_exam = [](void* context) {
        CHECK(reinterpret_cast<uint64_t>(context) == game::global<uint64_t>(t::data::game_context));
        ++exam_checks;
        return exam_mode;
    };
    auto* owner = mod_create("academy-native-hooks");
    auto* mod = mod_create("academy-open-exams");
    TSUCHI_Subscription subscription{};
    CHECK(academy::table.on_opened(mod, [](void*, const TSUCHI_AcademyOpened* event) {
        CHECK(event->widget.id && game::from_handle(event->widget.id));
        ++events;
    }, nullptr, &subscription) == TSUCHI_OK);
    academy_event_hooks_start(owner);
    CHECK(!frame_work && !wrapped_update && !wrapped_finish && !wrapped_points_start && !wrapped_time_start);
    install_result = TSUCHI_OK;
    academy_event_hooks_start(owner);
    CHECK(frame_work && wrapped_update && wrapped_finish && wrapped_points_start && wrapped_time_start);
    expected_state = fake::memory(0x80);
    const uint64_t parent = fake::memory(0x80), widget = fake::memory(0x80);
    fake::put<uint64_t>(expected_state + t::academy_menu_state::parent, parent);
    fake::put<uint64_t>(parent + t::academy_menu_presenter::widget, widget);
    wrapped_update(reinterpret_cast<void*>(expected_state), 0.125f);
    CHECK(updates == 1 && last_seconds == 0.125f && events == 0);
    frame_work();
    CHECK(events == 0);
    viewport = true;
    visibility = 1; // Collapsed widgets still belong to the viewport.
    frame_work();
    CHECK(events == 0);
    visibility = 0;
    frame_work();
    CHECK(events == 1);
    wrapped_update(reinterpret_cast<void*>(expected_state), 0.25f);
    frame_work();
    CHECK(events == 1 && last_seconds == 0.25f);
    visibility = 2; frame_work(); // Hidden, then shown without allocating a widget.
    visibility = 4; frame_work();
    CHECK(events == 2);
    viewport = false; frame_work();
    viewport = true; frame_work();
    CHECK(events == 3);
    fake::unmap(widget);
    frame_work();
    const uint64_t next_widget = fake::memory(0x80);
    fake::put<uint64_t>(parent + t::academy_menu_presenter::widget, next_widget);
    thread = 2;
    wrapped_update(reinterpret_cast<void*>(expected_state), 0.5f);
    CHECK(updates == 3 && last_seconds == 0.5f);
    thread = 1;
    frame_work();
    CHECK(events == 3); // Foreign-thread invocations do not publish observations.
    wrapped_update(reinterpret_cast<void*>(expected_state), 0.75f);
    CHECK(events == 3);
    frame_work();
    CHECK(events == 4);

    t::cells[t::data::game_context.index] = 0x3a0;
    t::cells[t::data::game_instance.index] = 0x3a8;
    t::cells[t::game_instance::academy_save.index] = 0x38;
    t::cells[t::academy_context::course.index] = 0x60;
    t::cells[t::game_context::rule.index] = 0x30;
    t::cells[t::academy_end_report::score.index] = 0x18;
    t::cells[t::academy_end_report::elapsed.index] = 0x28;
    t::cells[t::academy_end_report::cleared.index] = 8;
    const uint64_t context = fake::memory(0x80), instance = fake::memory(0x80);
    const uint64_t save = fake::memory(0x80), report = fake::memory(0x80);
    fake::put<uint64_t>(fake::main_image() + t::data::game_context, context);
    fake::put<uint64_t>(fake::main_image() + t::data::game_instance, instance);
    fake::put<uint64_t>(instance + t::game_instance::academy_save, save);
    fake::put<uint8_t>(context + t::academy_context::course, 8);
    fake::put<uint8_t>(context + t::game_context::rule, 4);
    fake::put<int32_t>(report + t::academy_end_report::score, 321);
    // A points report leaves the timed report's clock uninitialized. This is
    // the real nonzero points finish that the old observer rejected.
    fake::put<float>(report + t::academy_end_report::elapsed, std::numeric_limits<float>::quiet_NaN());
    fake::put<uint8_t>(report + t::academy_end_report::cleared, 1);
    TSUCHI_Subscription result_subscription{};
    CHECK(academy::table.on_exam_finished(mod, [](void*, const TSUCHI_AcademyExamFinished* event) {
        exam_events.push_back(*event);
        attempt_order.push_back('F');
    }, nullptr, &result_subscription) == TSUCHI_OK);
    TSUCHI_Subscription start_subscription{};
    CHECK(academy::table.on_exam_started(mod, [](void*, const TSUCHI_AcademyExamStarted* event) {
        start_events.push_back(*event);
        attempt_order.push_back('S');
    }, nullptr, &start_subscription) == TSUCHI_OK);
    uint64_t x[31]{};
    x[21] = report; x[11] = 3;
    TSUCHI_InlineContext registers{x, nullptr, 0x1230, 0, 0, 0};
    uint64_t before[31]; memcpy(before, x, sizeof before);
    wrapped_finish(&registers);
    CHECK(continued_finishes == 1 && exam_events.empty());
    CHECK(!memcmp(before, x, sizeof before));
    frame_work();
    CHECK(exam_events.size() == 1);
    const auto result = exam_events.back();
    CHECK(result.course == 8 && result.score == 321 && result.elapsed == 0);
    CHECK(result.medal == 3 && result.cleared && !result.timed);
    // The report supplies the attempt; no saved high score is read or changed.
    for (uint32_t i = 0; i < 0x80; ++i) CHECK(fake::get<uint8_t>(save + i) == 0);
    fake::put<uint8_t>(report + t::academy_end_report::cleared, 2);
    wrapped_finish(&registers); frame_work();
    CHECK(exam_events.size() == 2 && !exam_events.back().cleared);
    CHECK(exam_events.back().score == 321 && !exam_events.back().timed);
    exam_events.pop_back(); attempt_order.pop_back();
    fake::put<uint8_t>(report + t::academy_end_report::cleared, 1);
    fake::put<uint8_t>(context + t::game_context::rule, 5);
    fake::put<float>(report + t::academy_end_report::elapsed, 5.75f);
    x[11] = 1;
    wrapped_finish(&registers); frame_work();
    CHECK(exam_events.size() == 2 && exam_events.back().score == 5 && exam_events.back().timed);
    CHECK(exam_events.back().medal == 1); // A worse attempt keeps its own medal.
    x[11] = 6; wrapped_finish(&registers); frame_work();
    CHECK(exam_events.size() == 2);
    x[11] = 2;
    fake::put<uint8_t>(context + t::academy_context::course, 72);
    wrapped_finish(&registers); frame_work(); CHECK(exam_events.size() == 2);
    fake::put<uint8_t>(context + t::academy_context::course, 8);
    fake::put<float>(report + t::academy_end_report::elapsed, -1);
    wrapped_finish(&registers); frame_work(); CHECK(exam_events.size() == 2);
    fake::put<float>(report + t::academy_end_report::elapsed, 10.5f);
    wrapped_finish(&registers);
    fake::put<uint64_t>(instance + t::game_instance::academy_save, fake::memory(0x80));
    frame_work(); CHECK(exam_events.size() == 2);
    thread = 2; wrapped_finish(&registers); thread = 1;
    frame_work(); CHECK(exam_events.size() == 2);

    fake::put<uint8_t>(context + t::game_context::rule, 4);
    memcpy(before, x, sizeof before);
    wrapped_points_start(&registers);
    CHECK(continued_points == 1 && start_events.empty());
    CHECK(!memcmp(before, x, sizeof before));
    frame_work();
    CHECK(start_events.size() == 1 && start_events.back().course == 8 && !start_events.back().timed);
    fake::put<uint8_t>(context + t::game_context::rule, 5);
    wrapped_time_start(&registers); frame_work();
    CHECK(continued_times == 1 && start_events.size() == 2 && start_events.back().timed);
    // Both stages also serve non-exam play. Native mode checks exclude practice;
    // course IDs and rule checks exclude training and unrelated stages.
    exam_mode = false;
    wrapped_time_start(&registers); frame_work(); CHECK(start_events.size() == 2);
    exam_mode = true;
    fake::put<uint8_t>(context + t::academy_context::course, 72);
    wrapped_time_start(&registers); frame_work(); CHECK(start_events.size() == 2);
    fake::put<uint8_t>(context + t::academy_context::course, 8);
    wrapped_points_start(&registers); frame_work(); CHECK(start_events.size() == 2);
    const uint32_t checked = exam_checks;
    thread = 2; wrapped_time_start(&registers); thread = 1;
    frame_work(); CHECK(start_events.size() == 2 && exam_checks == checked);
    wrapped_time_start(&registers);
    fake::put<uint64_t>(instance + t::game_instance::academy_save, fake::memory(0x80));
    frame_work(); CHECK(start_events.size() == 2);
    const uint64_t current_save = fake::get<uint64_t>(instance + t::game_instance::academy_save);
    fake::put<uint64_t>(instance + t::game_instance::academy_save, 0);
    wrapped_time_start(&registers); frame_work(); CHECK(start_events.size() == 2);
    fake::put<uint64_t>(instance + t::game_instance::academy_save, current_save);
    // Finish followed by a retry start in the same frame retains native order.
    attempt_order.clear();
    wrapped_finish(&registers); wrapped_time_start(&registers); wrapped_finish(&registers);
    frame_work();
    CHECK(attempt_order == std::vector<char>({'F', 'S', 'F'}));
    CHECK(exam_events.size() == 4 && start_events.size() == 3);
    // Bounded queue; a retry emits once for each entry, including the same course.
    for (uint32_t i = 0; i < 17; ++i) wrapped_time_start(&registers);
    frame_work(); CHECK(start_events.size() == 19);
    wrapped_time_start(&registers); // A pending callback cannot outlive its mod.
    mod_stop(mod);
    frame_work(); CHECK(start_events.size() == 19);
    viewport = false; frame_work(); viewport = true; frame_work();
    CHECK(events == 4);
    std::puts("academy menu event checks passed");
}
