// Native-memory stand-in. The synthetic layout differs from every supported game.
#include <limits>
#include "fake_game.h"
#include "families.h"
#include "../../framework/game/academy_backend.cpp"

#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "line %d: %s\n", __LINE__, #x); std::exit(1); } } while (0)

namespace tsuchi {
TSUCHI_Result academy_complete_score_submission(TSUCHI_Mod*, TSUCHI_AcademySubmission, bool) { return TSUCHI_UNSUPPORTED; }
TSUCHI_Result academy_hold_score_submissions(TSUCHI_Mod*, uint32_t) { return TSUCHI_UNSUPPORTED; }
TSUCHI_Result academy_release_score_submissions(TSUCHI_Mod*, uint32_t) { return TSUCHI_UNSUPPORTED; }
bool local_schedule = false;
void (*academy_frame)() = nullptr;
bool each_engine_frame(void (*work)()) { academy_frame = work; return true; }
bool academy_mod_schedule() { return local_schedule; }
bool academy_uploads_blocked() { return false; }
TSUCHI_Result academy_hold_schedule(TSUCHI_Mod*, const char*) { return TSUCHI_UNSUPPORTED; }
TSUCHI_Result academy_release_schedule(TSUCHI_Mod*) { return TSUCHI_UNSUPPORTED; }
TSUCHI_Result academy_hold_unlocked_periods(TSUCHI_Mod*, TSUCHI_FieldHold*) { return TSUCHI_UNSUPPORTED; }
TSUCHI_Result academy_hold_board(TSUCHI_Mod*, uint32_t, const char*) { return TSUCHI_UNSUPPORTED; }
TSUCHI_Result academy_release_board(TSUCHI_Mod*, uint32_t) { return TSUCHI_UNSUPPORTED; }
TSUCHI_Result academy_board_state(TSUCHI_Mod*, uint32_t, TSUCHI_AcademyBoardState*) { return TSUCHI_UNSUPPORTED; }
TSUCHI_Result academy_open_board(TSUCHI_Mod*, uint32_t) { return TSUCHI_UNSUPPORTED; }
namespace game {
}
}

namespace {
using namespace tsuchi;
namespace t = game::table;

template<class T> void cell(t::Cell<T> field, uint64_t value) { t::cells[field.index] = value; }

void layout() {
    cell(t::data::academy_schedule, 0x380);
    cell(t::game_instance::academy_save, 0x30);
    cell(t::game_framework::ui, 0x60);
    cell(t::ui_manager::academy_courses, 0x90);
    cell(t::ui_manager::academy_training_courses, 0x120);
    cell(t::ui_manager::academy_data, 0x110);
    cell(t::academy_data::training_courses, 0x28);
    cell(t::academy_course_entry::size, 0x50);
    cell(t::academy_course_entry::key, 8);
    cell(t::academy_course_entry::row, 0x18);
    cell(t::academy_training_course_entry::size, 0xa0);
    cell(t::academy_training_course_entry::key, 8);
    cell(t::academy_training_course_entry::row, 0x18);
    cell(t::academy_exam_row::courses, 0x18);
    cell(t::academy_training_row::course, 0x20);
    cell(t::academy_course::size, 0x40);
    cell(t::academy_course::level, 8);
    cell(t::academy_save::held_ids, 0x10);
    cell(t::academy_save::opened_ids, 0x20);
    cell(t::academy_save::redo_passes, 0x30);
    cell(t::academy_save::reserve_passes, 0x34);
    cell(t::academy_save::dandori_points, 0x38);
    cell(t::academy_save::dandori_rank, 0x3c);
    cell(t::academy_save::card_rank, 0x40);
    cell(t::academy_save::exam_results, 0x50);
    cell(t::academy_save::training_results, 0xb0);
    cell(t::academy_save::service_end, 0x110);
    cell(t::academy_schedule::valid, 1);
    cell(t::academy_schedule::interface, 0x158);
    cell(t::academy_schedule::set, 0x10);
    cell(t::academy_schedule::latest, 0x40);
    cell(t::academy_schedule::prev, 0xc0);
    cell(t::academy_schedule::first_period, 0x150);
    cell(t::academy_period::stage, 4);
    cell(t::academy_period::delivery, 0x18);
    cell(t::academy_period::starts, 0x28);
    cell(t::academy_period::ends, 0x48);
    cell(t::academy_period::ticket, 0x64);
    cell(t::academy_period::ticket_limit, 0x69);
    cell(t::academy_period::closed, 0x6d);
    cell(t::academy_exam_entry::size, 0xb0);
    cell(t::academy_exam_entry::key, 8);
    cell(t::academy_exam_entry::value, 0x18);
    cell(t::academy_training_entry::size, 0x40);
    cell(t::academy_training_entry::key, 8);
    cell(t::academy_training_entry::value, 0x18);
    cell(t::academy_exam_result::size, 0x90);
    const t::Cell<uint64_t> exam[] = {t::academy_exam_result::practice_score, t::academy_exam_result::score,
        t::academy_exam_result::local_score, t::academy_exam_result::practice_time, t::academy_exam_result::time,
        t::academy_exam_result::local_time, t::academy_exam_result::practice_count, t::academy_exam_result::count,
        t::academy_exam_result::medal, t::academy_exam_result::viewed_medal, t::academy_exam_result::local_medal,
        t::academy_exam_result::practice_medal, t::academy_exam_result::practice_clear, t::academy_exam_result::clear,
        t::academy_exam_result::stamp_rank, t::academy_exam_result::stamp_x, t::academy_exam_result::stamp_y,
        t::academy_exam_result::stamp_rotation};
    for (uint32_t i = 0; i < sizeof exam / sizeof exam[0]; ++i) cell(exam[i], 8 + 4 * i);
    cell(t::academy_training_result::size, 0x20);
    cell(t::academy_training_result::count, 4);
    cell(t::academy_training_result::score, 12);
    cell(t::academy_training_result::medal, 20);
    cell(t::academy_training_result::clear, 28);
}

uint64_t list(uint64_t header, std::initializer_list<int32_t> values) {
    const uint64_t data = fake::memory(values.size() * 4);
    uint32_t index = 0;
    for (const int32_t value : values) fake::put<int32_t>(data + 4 * index++, value);
    fake::put<uint64_t>(header + t::array::data, data);
    fake::put<int32_t>(header + t::array::count, int32_t(values.size()));
    fake::put<int32_t>(header + t::array::max, int32_t(values.size()));
    return data;
}

uint64_t sparse(uint64_t header, uint32_t count, uint64_t stride, uint32_t bits) {
    const uint64_t data = fake::memory(stride * count);
    fake::put<uint64_t>(header + t::sparse_array::data, data);
    fake::put<int32_t>(header + t::sparse_array::count, int32_t(count));
    fake::put<int32_t>(header + t::sparse_array::bit_count, int32_t(count));
    fake::put<uint32_t>(header + t::sparse_array::bits, bits);
    return data;
}

void period_events(TSUCHI_Mod* mod, uint64_t instance, uint64_t save) {
    struct Observed {
        std::vector<uint32_t> deliveries, unlocks;
        uint64_t instance = 0, replacement = 0;
        TSUCHI_Mod* stop = nullptr;
    } observed;
    observed.instance = instance;
    TSUCHI_Subscription delivered{}, unlocked{};
    CHECK(academy::table.on_period_delivered(mod, [](void* user, const TSUCHI_AcademyPeriodDelivered* event) {
        auto& seen = *static_cast<Observed*>(user);
        seen.deliveries.push_back(event->period);
        if (seen.replacement) fake::put<uint64_t>(seen.instance + t::game_instance::academy_save, seen.replacement);
        if (seen.stop) mod_stop(seen.stop);
    }, &observed, &delivered) == TSUCHI_OK);
    CHECK(academy::table.on_period_unlocked(mod, [](void* user, const TSUCHI_AcademyPeriodUnlocked* event) {
        static_cast<Observed*>(user)->unlocks.push_back(event->period);
    }, &observed, &unlocked) == TSUCHI_OK);
    CHECK(academy_frame);
    fake::put<uint64_t>(instance + t::game_instance::academy_save, 0);
    academy_frame();
    fake::put<uint64_t>(instance + t::game_instance::academy_save, save);
    academy_frame(); // Loading an existing save does not replay its deliveries.
    CHECK(observed.deliveries.empty() && observed.unlocks.empty());
    list(save + t::academy_save::held_ids, {5, 0, 11, 2, 2});
    list(save + t::academy_save::opened_ids, {5, 2});
    academy_frame();
    CHECK(observed.deliveries == std::vector<uint32_t>({2}));
    CHECK(observed.unlocks == std::vector<uint32_t>({2}));
    list(save + t::academy_save::held_ids, {2, 11, 0, 5});
    academy_frame(); // Reordering and duplicates do not create deliveries.
    CHECK(observed.deliveries.size() == 1 && observed.unlocks.size() == 1);
    const uint64_t invalid = list(save + t::academy_save::held_ids, {2, 11, 0, 5, 15});
    list(save + t::academy_save::opened_ids, {5, 2, 3});
    academy_frame(); // Invalid native input cannot advance either snapshot.
    CHECK(observed.deliveries.size() == 1 && observed.unlocks.size() == 1);
    fake::put<int32_t>(invalid + 16, 3);
    academy_frame();
    CHECK(observed.deliveries == std::vector<uint32_t>({2, 3}));
    CHECK(observed.unlocks == std::vector<uint32_t>({2, 3}));
    const uint64_t replacement = fake::memory(0x200);
    list(replacement + t::academy_save::held_ids, {9});
    list(replacement + t::academy_save::opened_ids, {9});
    observed.replacement = replacement;
    list(save + t::academy_save::held_ids, {2, 11, 0, 5, 3, 4, 6});
    list(save + t::academy_save::opened_ids, {5, 2, 3, 4, 6});
    academy_frame(); // A callback replaces the save, cancelling the old batch.
    CHECK(observed.deliveries == std::vector<uint32_t>({2, 3, 4}));
    CHECK(observed.unlocks == std::vector<uint32_t>({2, 3}));
    observed.replacement = 0;
    academy_frame();
    CHECK(observed.deliveries.size() == 3 && observed.unlocks.size() == 2);
    observed.stop = mod;
    list(replacement + t::academy_save::held_ids, {9, 10});
    list(replacement + t::academy_save::opened_ids, {9, 10});
    academy_frame(); // Stopping in delivery removes the later unlock callback.
    CHECK(observed.deliveries == std::vector<uint32_t>({2, 3, 4, 10}));
    CHECK(observed.unlocks.size() == 2);
    academy_frame();
    CHECK(observed.deliveries.size() == 4);
}

void checks() {
    layout();
    fake::start();
    TSUCHI_Mod* mod = mod_create("check.academy");
    CHECK(mod);
    academy_backend_start();
    TSUCHI_AcademyState state{};
    CHECK(academy::table.state(mod, &state) == TSUCHI_NOT_READY);
    const uint64_t instance = fake::memory(0x200), save = fake::memory(0x200), manager = fake::memory(0x200);
    fake::put<uint64_t>(fake::main_image() + t::data::game_instance, instance);
    fake::put<uint64_t>(instance + t::game_instance::academy_save, save);
    fake::put<uint64_t>(fake::main_image() + t::data::academy_schedule, manager);
    const uint64_t held = list(save + t::academy_save::held_ids, {5, 0, 11});
    list(save + t::academy_save::opened_ids, {5});
    fake::put<int32_t>(save + t::academy_save::redo_passes, 7);
    fake::put<int32_t>(save + t::academy_save::reserve_passes, 2);
    fake::put<int32_t>(save + t::academy_save::dandori_points, 500);
    fake::put<int32_t>(save + t::academy_save::dandori_rank, 4);
    fake::put<int32_t>(save + t::academy_save::card_rank, 3);
    CHECK(academy::table.state(mod, &state) == TSUCHI_OK);
    CHECK(!state.schedule_loaded && state.delivered_count == 3 && state.unlocked_count == 1);
    CHECK(state.redo_passes == 7 && state.reserve_passes == 2 && state.dandori_points == 500);
    CHECK(state.dandori_rank == 4 && state.card_rank == 3);
    local_schedule = true;
    fake::put<uint8_t>(manager + t::academy_schedule::valid, 1);
    list(manager + t::academy_schedule::set, {5, 5, 11});
    const uint64_t latest = manager + t::academy_schedule::latest;
    fake::put<int32_t>(latest + t::academy_period::stage, 3);
    fake::put<int64_t>(latest + t::academy_period::delivery, kEpochTicks + 42 * kTicksPerSecond);
    fake::put<int32_t>(latest + t::academy_period::ticket, 9);
    fake::put<uint8_t>(latest + t::academy_period::ticket_limit, 1);
    CHECK(academy::table.state(mod, &state) == TSUCHI_OK && state.mod_schedule && state.latest_stage == 3);
    t::fn::academy_status = [](void*) -> int32_t { return int32_t(t::constant::academy_status_open); };
    CHECK(academy::table.state(mod, &state) == TSUCHI_OK && state.status == TSUCHI_ACADEMY_STATUS_OPEN);
    t::fn::academy_status = [](void*) -> int32_t { return int32_t(t::constant::academy_status_between_exams); };
    CHECK(academy::table.state(mod, &state) == TSUCHI_OK && state.status == TSUCHI_ACADEMY_STATUS_BETWEEN_EXAMS);
    t::fn::academy_status = [](void*) -> int32_t { return int32_t(t::constant::academy_status_ended); };
    CHECK(academy::table.state(mod, &state) == TSUCHI_OK && state.status == TSUCHI_ACADEMY_STATUS_ENDED);
    t::fn::academy_status = [](void*) -> int32_t { return int32_t(t::constant::academy_status_upcoming); };
    CHECK(academy::table.state(mod, &state) == TSUCHI_OK && state.status == TSUCHI_ACADEMY_STATUS_UPCOMING);
    t::fn::academy_status = [](void*) -> int32_t { return int32_t(t::constant::academy_status_unavailable); };
    CHECK(academy::table.state(mod, &state) == TSUCHI_OK && state.status == TSUCHI_ACADEMY_STATUS_UNAVAILABLE);
    t::fn::academy_status = [](void*) -> int32_t { return 7; };
    CHECK(academy::table.state(mod, &state) == TSUCHI_NOT_READY);
    t::fn::academy_status = nullptr;
    uint32_t number = 99;
    CHECK(academy::table.period_at(mod, TSUCHI_ACADEMY_PERIOD_LIST_SCHEDULED, 1, &number) == TSUCHI_OK && number == 5);
    CHECK(academy::table.period_at(mod, TSUCHI_ACADEMY_PERIOD_LIST_DELIVERED, 2, &number) == TSUCHI_OK && number == 11);
    CHECK(academy::table.period_at(mod, TSUCHI_ACADEMY_PERIOD_LIST_UNLOCKED, 1, &number) == TSUCHI_NOT_FOUND);
    CHECK(academy::table.period_at(mod, 99, 0, &number) == TSUCHI_INVALID);
    CHECK(academy::table.period_at(mod, TSUCHI_ACADEMY_PERIOD_LIST_SCHEDULED, 64, &number) == TSUCHI_INVALID);
    fake::put<int32_t>(held, 12);
    CHECK(academy::table.state(mod, &state) == TSUCHI_NOT_READY);
    fake::put<int32_t>(held, 5);
    TSUCHI_AcademyPeriod period{};
    CHECK(academy::table.period(mod, false, &period) == TSUCHI_OK && period.delivery == 42 && period.redo_passes == 9 && period.ticket_limit);
    fake::put<uint8_t>(manager + t::academy_schedule::first_period, 1);
    CHECK(academy::table.period(mod, true, &period) == TSUCHI_NOT_FOUND);
    const uint64_t starts = list(latest + t::academy_period::starts, {});
    (void)starts;
    const uint64_t window_starts = fake::memory(8), window_ends = fake::memory(8);
    fake::put<int64_t>(window_starts, kEpochTicks + 50 * kTicksPerSecond);
    fake::put<int64_t>(window_ends, kEpochTicks + 60 * kTicksPerSecond);
    for (const uint64_t header : {latest + t::academy_period::starts, latest + t::academy_period::ends}) {
        fake::put<int32_t>(header + t::array::count, 1);
        fake::put<int32_t>(header + t::array::max, 1);
    }
    fake::put<uint64_t>(latest + t::academy_period::starts + t::array::data, window_starts);
    fake::put<uint64_t>(latest + t::academy_period::ends + t::array::data, window_ends);
    TSUCHI_AcademyWindow window{};
    CHECK(academy::table.window(mod, false, 0, &window) == TSUCHI_OK && window.start == 50 && window.finish == 60);
    CHECK(academy::table.window(mod, false, 1, &window) == TSUCHI_NOT_FOUND);
    fake::put<int64_t>(window_ends, kEpochTicks + 40 * kTicksPerSecond);
    CHECK(academy::table.window(mod, false, 0, &window) == TSUCHI_NOT_READY);
    fake::put<int64_t>(window_starts, std::numeric_limits<int64_t>::min());
    CHECK(academy::table.window(mod, false, 0, &window) == TSUCHI_NOT_READY);
    int64_t seconds = 0;
    CHECK(!unix_seconds(std::numeric_limits<int64_t>::min(), seconds));
    CHECK(!unix_seconds(std::numeric_limits<int64_t>::max(), seconds));
    CHECK(unix_seconds(kEpochTicks - 1, seconds) && seconds == -1);

    const uint64_t framework = fake::memory(0x100), ui = fake::memory(0x200);
    fake::put<uint64_t>(fake::main_image() + t::data::game_framework, framework);
    fake::put<uint64_t>(framework + t::game_framework::ui, ui);
    const uint64_t rows = sparse(ui + t::ui_manager::academy_courses, 2, t::academy_course_entry::size, 2);
    const uint64_t row = rows + t::academy_course_entry::size;
    fake::put(row + t::academy_course_entry::key, fake::name(u"Test_01"));
    const uint64_t courses = fake::memory(6 * t::academy_course::size);
    const uint64_t header = row + t::academy_course_entry::row + t::academy_exam_row::courses;
    fake::put<uint64_t>(header + t::array::data, courses);
    fake::put<int32_t>(header + t::array::count, 6);
    fake::put<int32_t>(header + t::array::max, 6);
    const auto original_level = fake::name(u"DDKOriginal"), custom_level = fake::name(u"DDKCustom");
    fake::put(courses + 2 * t::academy_course::size + t::academy_course::level, original_level);
    const auto selected_copy = fake::memory(t::academy_course::size);
    fake::put(selected_copy + t::academy_course::level, original_level);
    CHECK(academy_course_matches(8, selected_copy));
    CHECK(!academy_course_matches(7, selected_copy));
    CHECK(!academy_course_matches(8, 0));
    CHECK(!academy_course_matches(72, selected_copy));
    const uint64_t entries = sparse(save + t::academy_save::exam_results, 2, t::academy_exam_entry::size, 2);
    const uint64_t entry = entries + t::academy_exam_entry::size;
    fake::put(entry + t::academy_exam_entry::key, custom_level);
    const uint64_t result = entry + t::academy_exam_entry::value;
    TSUCHI_AcademyExamResult exam{};
    CHECK(academy::table.exam_result(mod, 8, &exam) == TSUCHI_NOT_FOUND);
    fake::put(courses + 2 * t::academy_course::size + t::academy_course::level, custom_level);
    fake::put<int32_t>(result + t::academy_exam_result::practice_score, 111);
    fake::put<int32_t>(result + t::academy_exam_result::score, 222);
    fake::put<int32_t>(result + t::academy_exam_result::local_score, 333);
    fake::put<float>(result + t::academy_exam_result::practice_time, 1.5f);
    fake::put<float>(result + t::academy_exam_result::time, 2.5f);
    fake::put<float>(result + t::academy_exam_result::local_time, 3.5f);
    fake::put<int32_t>(result + t::academy_exam_result::practice_count, 10);
    fake::put<int32_t>(result + t::academy_exam_result::count, 20);
    fake::put<uint32_t>(result + t::academy_exam_result::medal, 1);
    fake::put<uint32_t>(result + t::academy_exam_result::viewed_medal, 2);
    fake::put<uint32_t>(result + t::academy_exam_result::local_medal, 3);
    fake::put<uint32_t>(result + t::academy_exam_result::practice_medal, 4);
    fake::put<uint8_t>(result + t::academy_exam_result::practice_clear, 1);
    fake::put<int32_t>(result + t::academy_exam_result::stamp_rank, 5);
    fake::put<float>(result + t::academy_exam_result::stamp_x, 0.25f);
    fake::put<float>(result + t::academy_exam_result::stamp_y, 0.5f);
    fake::put<float>(result + t::academy_exam_result::stamp_rotation, 90);
    unsigned char before[0xb0];
    memcpy(before, reinterpret_cast<void*>(entry), sizeof before);
    CHECK(academy::table.exam_result(mod, 8, &exam) == TSUCHI_OK);
    CHECK(exam.practice_high_score == 111 && exam.high_score == 222 && exam.local_high_score == 333);
    CHECK(exam.practice_clear_time == 1.5f && exam.clear_time == 2.5f && exam.local_clear_time == 3.5f);
    CHECK(exam.practice_play_count == 10 && exam.play_count == 20);
    CHECK(exam.medal == 1 && exam.viewed_medal == 2 && exam.local_medal == 3 && exam.practice_medal == 4);
    CHECK(exam.practice_cleared && !exam.exam_cleared && exam.stamp_rank == 5);
    CHECK(exam.stamp_x == 0.25f && exam.stamp_y == 0.5f && exam.stamp_rotation == 90);
    CHECK(!memcmp(before, reinterpret_cast<void*>(entry), sizeof before));
    CHECK(academy::table.exam_result(mod, 72, &exam) == TSUCHI_INVALID);
    fake::put<float>(result + t::academy_exam_result::time, std::numeric_limits<float>::infinity());
    CHECK(academy::table.exam_result(mod, 8, &exam) == TSUCHI_NOT_READY);
    fake::put<float>(result + t::academy_exam_result::time, 2.5f);
    fake::unmap(entries);
    CHECK(academy::table.exam_result(mod, 8, &exam) == TSUCHI_NOT_READY);

    const uint64_t asset = fake::memory(0x100);
    fake::put<uint64_t>(ui + t::ui_manager::academy_data, asset);
    const uint64_t training_courses = sparse(ui + t::ui_manager::academy_training_courses, 1, t::academy_training_course_entry::size, 1);
    fake::put(training_courses + t::academy_training_course_entry::key, fake::name(u"Course_02"));
    fake::put(training_courses + t::academy_training_course_entry::row + t::academy_training_row::course + t::academy_course::level, custom_level);
    const uint64_t training = sparse(save + t::academy_save::training_results, 1, t::academy_training_entry::size, 1);
    fake::put(training + t::academy_training_entry::key, custom_level);
    const uint64_t training_value = training + t::academy_training_entry::value;
    fake::put<int32_t>(training_value + t::academy_training_result::count, 12);
    fake::put<int32_t>(training_value + t::academy_training_result::score, 345);
    fake::put<uint32_t>(training_value + t::academy_training_result::medal, 4);
    fake::put<uint8_t>(training_value + t::academy_training_result::clear, 1);
    TSUCHI_AcademyTrainingResult trained{};
    CHECK(academy::table.training_result(mod, 2, &trained) == TSUCHI_OK);
    CHECK(trained.play_count == 12 && trained.high_score == 345 && trained.medal == 4 && trained.cleared);
    CHECK(academy::table.training_result(mod, 3, &trained) == TSUCHI_INVALID);
    CHECK(academy::table.state(mod, nullptr) == TSUCHI_INVALID);
    families_absent = ~uint64_t(0);
    CHECK(academy::table.state(mod, &state) == TSUCHI_UNSUPPORTED);
    families_absent = 0;
    period_events(mod, instance, save);
    CHECK(academy::table.state(mod, &state) == TSUCHI_INVALID);
}
}

int main() {
    checks();
    std::puts("academy backend checks passed");
}
