// Read-only Academy state. Layouts belong to game/table.yaml and its build files.
#include <string.h>

#include "academy_internal.h"
#include "academy_service.h"
#include "game.h"
#include "frame.h"
#include "names.h"
#include "registry.h"

namespace tsuchi {
namespace {
namespace t = game::table;

constexpr uint32_t kMostPeriods = 64;
constexpr uint32_t kMostResults = 256;
constexpr int64_t kEpochTicks = 621355968000000000;
constexpr int64_t kTicksPerSecond = 10000000;
constexpr int64_t kLastDateTicks = 3155378975999999999;

bool unix_seconds(int64_t ticks, int64_t& out) {
    if (ticks < 0 || ticks > kLastDateTicks) return false;
    const int64_t relative = ticks - kEpochTicks;
    out = relative / kTicksPerSecond;
    if (relative < 0 && relative % kTicksPerSecond) --out;
    return true;
}

struct Array {
    uint64_t data = 0;
    int32_t count = 0, max = 0;
};

bool array_at(uint64_t at, uint64_t stride, uint32_t most, Array& out) {
    Array next;
    if (!at || !game::read(at + t::array::data, next.data) ||
        !game::read(at + t::array::count, next.count) || !game::read(at + t::array::max, next.max) ||
        next.count < 0 || next.max < next.count || uint32_t(next.count) > most ||
        (next.count && (!next.data || !game::readable(next.data, uint64_t(next.count) * stride)))) return false;
    out = next;
    return true;
}

struct Sparse {
    uint64_t data = 0, bits = 0, stride = 0;
    int32_t count = 0;
};

bool sparse_at(uint64_t at, uint64_t stride, uint32_t most, Sparse& out) {
    Sparse next;
    uint64_t heap = 0;
    int32_t bit_count = 0;
    if (!at || !stride || !game::read(at + t::sparse_array::data, next.data) ||
        !game::read(at + t::sparse_array::count, next.count) ||
        !game::read(at + t::sparse_array::more_bits, heap) ||
        !game::read(at + t::sparse_array::bit_count, bit_count) ||
        next.count < 0 || uint32_t(next.count) > most || bit_count < next.count) return false;
    next.bits = heap ? heap : at + t::sparse_array::bits;
    if (next.count && (!next.data || !game::readable(next.data, stride * uint32_t(next.count)) ||
        !game::readable(next.bits, 4 * ((uint32_t(next.count) + 31) / 32)))) return false;
    next.stride = stride;
    out = next;
    return true;
}

bool occupied(const Sparse& map, uint32_t index) {
    return (game::read_or<uint32_t>(map.bits + uint64_t(index / 32) * 4) >> (index % 32)) & 1;
}

uint64_t academy_save() {
    const uint64_t instance = game::global<uint64_t>(t::data::game_instance);
    return instance ? game::read_or<uint64_t>(instance + t::game_instance::academy_save) : 0;
}

uint64_t academy_ui() {
    const uint64_t framework = game::global<uint64_t>(t::data::game_framework);
    return framework ? game::read_or<uint64_t>(framework + t::game_framework::ui) : 0;
}

uint64_t loaded_manager() {
    const uint64_t manager = game::global<uint64_t>(t::data::academy_schedule);
    return manager && game::read_or<uint8_t>(manager + t::academy_schedule::valid) == 1 ? manager : 0;
}

bool period_numbers(uint64_t at, uint32_t most, Array& list) {
    if (!array_at(at, sizeof(int32_t), most, list)) return false;
    for (int32_t i = 0; i < list.count; ++i) {
        const int32_t number = game::read_or<int32_t>(list.data + uint64_t(i) * 4, -1);
        if (number < 0 || number > 11) return false;
    }
    return true;
}

TSUCHI_Result get_state(TSUCHI_AcademyState* out) {
    const uint64_t save = academy_save();
    if (!save) return TSUCHI_NOT_READY;
    TSUCHI_AcademyState next{};
    next.status = TSUCHI_ACADEMY_STATUS_UNAVAILABLE;
    Array delivered, unlocked;
    uint8_t ended = 0;
    if (!period_numbers(save + t::academy_save::held_ids, kMostPeriods, delivered) ||
        !period_numbers(save + t::academy_save::opened_ids, 12, unlocked) ||
        !game::read(save + t::academy_save::service_end, ended) ||
        !game::read(save + t::academy_save::redo_passes, next.redo_passes) ||
        !game::read(save + t::academy_save::reserve_passes, next.reserve_passes) ||
        !game::read(save + t::academy_save::dandori_points, next.dandori_points) ||
        !game::read(save + t::academy_save::dandori_rank, next.dandori_rank) ||
        !game::read(save + t::academy_save::card_rank, next.card_rank)) return TSUCHI_NOT_READY;
    next.service_ended = ended != 0;
    next.delivered_count = uint32_t(delivered.count);
    next.unlocked_count = uint32_t(unlocked.count);
    next.mod_schedule = academy_mod_schedule();
    next.uploads_blocked = academy_uploads_blocked();
    if (const uint64_t manager = loaded_manager()) {
        Array set;
        int32_t stage = 0;
        if (!period_numbers(manager + t::academy_schedule::set, kMostPeriods, set) ||
            !game::read(manager + t::academy_schedule::latest + t::academy_period::stage, stage) ||
            stage < 1 || stage > set.count) return TSUCHI_NOT_READY;
        next.schedule_loaded = true;
        next.scheduled_count = uint32_t(set.count);
        next.latest_stage = uint32_t(stage);
        if (t::bound(t::fn::academy_status)) {
            const int32_t status = t::fn::academy_status(reinterpret_cast<void*>(manager + t::academy_schedule::interface));
            if (status == int32_t(t::constant::academy_status_open)) next.status = TSUCHI_ACADEMY_STATUS_OPEN;
            else if (status == int32_t(t::constant::academy_status_between_exams)) next.status = TSUCHI_ACADEMY_STATUS_BETWEEN_EXAMS;
            else if (status == int32_t(t::constant::academy_status_ended)) next.status = TSUCHI_ACADEMY_STATUS_ENDED;
            else if (status == int32_t(t::constant::academy_status_upcoming)) next.status = TSUCHI_ACADEMY_STATUS_UPCOMING;
            else if (status == int32_t(t::constant::academy_status_unavailable)) next.status = TSUCHI_ACADEMY_STATUS_UNAVAILABLE;
            else return TSUCHI_NOT_READY;
        }
    }
    *out = next;
    return TSUCHI_OK;
}

TSUCHI_Result period_address(bool previous, uint64_t& out) {
    const uint64_t manager = loaded_manager();
    if (!manager) return TSUCHI_NOT_READY;
    uint8_t first = 0;
    if (!game::read(manager + t::academy_schedule::first_period, first)) return TSUCHI_NOT_READY;
    if (previous && first) return TSUCHI_NOT_FOUND;
    out = manager + (previous ? t::academy_schedule::prev : t::academy_schedule::latest);
    return TSUCHI_OK;
}

TSUCHI_Result get_period(bool previous, TSUCHI_AcademyPeriod* out) {
    uint64_t at = 0;
    const TSUCHI_Result found = period_address(previous, at);
    if (found != TSUCHI_OK) return found;
    TSUCHI_AcademyPeriod next{};
    int32_t stage = 0;
    int64_t delivery = 0;
    uint8_t limited = 0, closed = 0;
    Array starts, ends;
    if (!game::read(at + t::academy_period::stage, stage) || stage < 1 || stage > 64 ||
        !game::read(at + t::academy_period::delivery, delivery) ||
        !array_at(at + t::academy_period::starts, 8, kMostPeriods, starts) ||
        !array_at(at + t::academy_period::ends, 8, kMostPeriods, ends) || starts.count != ends.count ||
        !game::read(at + t::academy_period::ticket, next.redo_passes) ||
        !game::read(at + t::academy_period::ticket_limit, limited) ||
        !game::read(at + t::academy_period::closed, closed)) return TSUCHI_NOT_READY;
    next.stage = uint32_t(stage);
    if (!unix_seconds(delivery, next.delivery)) return TSUCHI_NOT_READY;
    next.window_count = uint32_t(starts.count);
    next.ticket_limit = limited != 0;
    next.closed = closed != 0;
    *out = next;
    return TSUCHI_OK;
}

TSUCHI_Result get_window(bool previous, uint32_t index, TSUCHI_AcademyWindow* out) {
    if (index >= kMostPeriods) return TSUCHI_INVALID;
    uint64_t at = 0;
    const TSUCHI_Result found = period_address(previous, at);
    if (found != TSUCHI_OK) return found;
    Array starts, ends;
    if (!array_at(at + t::academy_period::starts, 8, kMostPeriods, starts) ||
        !array_at(at + t::academy_period::ends, 8, kMostPeriods, ends) || starts.count != ends.count) return TSUCHI_NOT_READY;
    if (index >= uint32_t(starts.count)) return TSUCHI_NOT_FOUND;
    int64_t start = 0, end = 0;
    if (!game::read(starts.data + uint64_t(index) * 8, start) ||
        !game::read(ends.data + uint64_t(index) * 8, end) || end < start) return TSUCHI_NOT_READY;
    TSUCHI_AcademyWindow next{};
    if (!unix_seconds(start, next.start) || !unix_seconds(end, next.finish)) return TSUCHI_NOT_READY;
    *out = next;
    return TSUCHI_OK;
}

TSUCHI_Result get_period_at(TSUCHI_AcademyPeriodList which, uint32_t index, uint32_t* out) {
    if (index >= kMostPeriods) return TSUCHI_INVALID;
    uint64_t at = 0;
    switch (which) {
    case TSUCHI_ACADEMY_PERIOD_LIST_SCHEDULED: {
        const uint64_t manager = loaded_manager();
        if (manager) at = manager + t::academy_schedule::set;
        break;
    }
    case TSUCHI_ACADEMY_PERIOD_LIST_DELIVERED:
    case TSUCHI_ACADEMY_PERIOD_LIST_UNLOCKED: {
        const uint64_t save = academy_save();
        if (save) at = save + (which == TSUCHI_ACADEMY_PERIOD_LIST_DELIVERED ? t::academy_save::held_ids : t::academy_save::opened_ids);
        break;
    }
    default: return TSUCHI_INVALID;
    }
    Array list;
    if (!at || !period_numbers(at, which == TSUCHI_ACADEMY_PERIOD_LIST_UNLOCKED ? 12 : kMostPeriods, list)) return TSUCHI_NOT_READY;
    if (index >= uint32_t(list.count)) return TSUCHI_NOT_FOUND;
    *out = uint32_t(game::read_or<int32_t>(list.data + uint64_t(index) * 4));
    return TSUCHI_OK;
}

TSUCHI_Result course_data(bool training, uint32_t course, uint64_t& address) {
    if (course >= (training ? 3u : 72u)) return TSUCHI_INVALID;
    const uint64_t ui = academy_ui();
    if (!ui) return TSUCHI_NOT_READY;
    {
        Sparse rows;
        if (!sparse_at(ui + (training ? t::ui_manager::academy_training_courses : t::ui_manager::academy_courses),
                       training ? t::academy_training_course_entry::size : t::academy_course_entry::size, 128, rows)) return TSUCHI_NOT_READY;
        char row_name[] = "Test_00";
        row_name[5] = char('0' + course / 6 / 10);
        row_name[6] = char('0' + course / 6 % 10);
        char training_name[] = "Course_00";
        training_name[8] = char('0' + course);
        for (uint32_t i = 0; i < uint32_t(rows.count); ++i) {
            if (!occupied(rows, i)) continue;
            const uint64_t entry = rows.data + rows.stride * i;
            game::Name name{};
            if (!game::read(entry + (training ? t::academy_training_course_entry::key : t::academy_course_entry::key), name)) return TSUCHI_NOT_READY;
            if (!game::name_is(name, training ? training_name : row_name)) continue;
            if (training) {
                address = entry + t::academy_training_course_entry::row + t::academy_training_row::course;
                return TSUCHI_OK;
            }
            Array courses;
            if (!array_at(entry + t::academy_course_entry::row + t::academy_exam_row::courses, t::academy_course::size, 6, courses)) return TSUCHI_NOT_READY;
            if (course % 6 >= uint32_t(courses.count)) return TSUCHI_NOT_FOUND;
            address = courses.data + (course % 6) * t::academy_course::size;
            return TSUCHI_OK;
        }
        return TSUCHI_NOT_READY;
    }
}

TSUCHI_Result course_key(bool training, uint32_t course, game::Name& key) {
    uint64_t address = 0;
    const auto found = course_data(training, course, address);
    if (found != TSUCHI_OK) return found;
    return game::read(address + t::academy_course::level, key) ? TSUCHI_OK : TSUCHI_NOT_READY;
}

TSUCHI_Result result_address(bool training, uint32_t course, uint64_t& result) {
    game::Name key{};
    const TSUCHI_Result found = course_key(training, course, key);
    if (found != TSUCHI_OK) return found;
    const uint64_t save = academy_save();
    if (!save) return TSUCHI_NOT_READY;
    Sparse map;
    if (!sparse_at(save + (training ? t::academy_save::training_results : t::academy_save::exam_results),
                   training ? t::academy_training_entry::size : t::academy_exam_entry::size, kMostResults, map)) return TSUCHI_NOT_READY;
    for (uint32_t i = 0; i < uint32_t(map.count); ++i) {
        if (!occupied(map, i)) continue;
        const uint64_t entry = map.data + map.stride * i;
        game::Name name{};
        if (!game::read(entry + (training ? t::academy_training_entry::key : t::academy_exam_entry::key), name)) return TSUCHI_NOT_READY;
        if (name.index != key.index || name.number != key.number) continue;
        result = entry + (training ? t::academy_training_entry::value : t::academy_exam_entry::value);
        return TSUCHI_OK;
    }
    return TSUCHI_NOT_FOUND;
}

TSUCHI_Result get_exam_result(uint32_t course, TSUCHI_AcademyExamResult* out) {
    uint64_t at = 0;
    const TSUCHI_Result found = result_address(false, course, at);
    if (found != TSUCHI_OK) return found;
    if (!game::readable(at, t::academy_exam_result::size)) return TSUCHI_NOT_READY;
    namespace r = t::academy_exam_result;
    TSUCHI_AcademyExamResult next{};
    next.practice_high_score = game::read_or<int32_t>(at + r::practice_score);
    next.high_score = game::read_or<int32_t>(at + r::score);
    next.local_high_score = game::read_or<int32_t>(at + r::local_score);
    next.practice_clear_time = game::read_or<float>(at + r::practice_time);
    next.clear_time = game::read_or<float>(at + r::time);
    next.local_clear_time = game::read_or<float>(at + r::local_time);
    next.practice_play_count = game::read_or<int32_t>(at + r::practice_count);
    next.play_count = game::read_or<int32_t>(at + r::count);
    next.medal = game::read_or<uint32_t>(at + r::medal);
    next.viewed_medal = game::read_or<uint32_t>(at + r::viewed_medal);
    next.local_medal = game::read_or<uint32_t>(at + r::local_medal);
    next.practice_medal = game::read_or<uint32_t>(at + r::practice_medal);
    next.practice_cleared = game::read_or<uint8_t>(at + r::practice_clear) != 0;
    next.exam_cleared = game::read_or<uint8_t>(at + r::clear) != 0;
    next.stamp_rank = game::read_or<int32_t>(at + r::stamp_rank);
    next.stamp_x = game::read_or<float>(at + r::stamp_x);
    next.stamp_y = game::read_or<float>(at + r::stamp_y);
    next.stamp_rotation = game::read_or<float>(at + r::stamp_rotation);
    if (next.medal > 5 || next.viewed_medal > 5 || next.local_medal > 5 || next.practice_medal > 5 ||
        !finite(next.practice_clear_time) || !finite(next.clear_time) || !finite(next.local_clear_time) ||
        !finite(next.stamp_x) || !finite(next.stamp_y) || !finite(next.stamp_rotation)) return TSUCHI_NOT_READY;
    *out = next;
    return TSUCHI_OK;
}

TSUCHI_Result get_training_result(uint32_t course, TSUCHI_AcademyTrainingResult* out) {
    uint64_t at = 0;
    const TSUCHI_Result found = result_address(true, course, at);
    if (found != TSUCHI_OK) return found;
    namespace r = t::academy_training_result;
    TSUCHI_AcademyTrainingResult next{};
    uint8_t cleared = 0;
    if (!game::read(at + r::count, next.play_count) || !game::read(at + r::score, next.high_score) ||
        !game::read(at + r::medal, next.medal) || next.medal > 5 ||
        !game::read(at + r::clear, cleared)) return TSUCHI_NOT_READY;
    next.cleared = cleared != 0;
    *out = next;
    return TSUCHI_OK;
}

uint64_t observed_save = 0;
uint32_t observed_delivered = 0, observed_unlocked = 0;

uint32_t period_mask(const Array& list) {
    uint32_t mask = 0;
    for (int32_t i = 0; i < list.count; ++i) mask |= 1u << uint32_t(game::read_or<int32_t>(list.data + uint64_t(i) * 4));
    return mask;
}

void observe_periods() {
    const uint64_t save = academy_save();
    if (!save) { observed_save = 0; return; }
    Array delivered, unlocked;
    if (!period_numbers(save + t::academy_save::held_ids, kMostPeriods, delivered) ||
        !period_numbers(save + t::academy_save::opened_ids, 12, unlocked)) return;
    const uint32_t now_delivered = period_mask(delivered), now_unlocked = period_mask(unlocked);
    const uint32_t new_deliveries = observed_save == save ? now_delivered & ~observed_delivered : 0;
    const uint32_t new_unlocks = observed_save == save ? now_unlocked & ~observed_unlocked : 0;
    // Capture before callbacks: callbacks can stop mods, change schedules, or
    // replace the active save. A replacement starts its own observation.
    observed_save = save;
    observed_delivered = now_delivered;
    observed_unlocked = now_unlocked;
    for (uint32_t period = 0; period < 12; ++period) {
        if (academy_save() != save) return;
        if (new_deliveries & (1u << period)) academy::emit_period_delivered({period});
        if (academy_save() != save) return;
        if (new_unlocks & (1u << period)) academy::emit_period_unlocked({period});
    }
}

const academy::Backend service{academy_hold_board, academy_release_board, academy_board_state,
                              get_state, get_period, get_window, get_period_at, get_exam_result, get_training_result,
                              academy_hold_schedule, academy_release_schedule, academy_hold_unlocked_periods, academy_open_board,
                              academy_hold_score_submissions, academy_release_score_submissions, academy_complete_score_submission};
} // namespace

bool academy_course_matches(uint32_t course, uint64_t address) {
    game::Name selected{}, current{};
    return address && game::read(address + t::academy_course::level, selected) &&
        course_key(false, course, current) == TSUCHI_OK &&
        selected.index == current.index && selected.number == current.number;
}

void academy_backend_start() {
    academy::set_backend(&service);
    each_engine_frame(observe_periods);
}
} // namespace tsuchi
