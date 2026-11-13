// The manifest's "academy_schedule": a Dandori Academy exam schedule a mod supplies, for the Switch 2 Edition.
//
// The Academy decides which exam periods are open from two application configs it downloads, DDKStages and
// DDKHeldInfo, and keeps what it parsed of them in its schedule manager. Without them (offline, or once the service
// is gone) no exam stays open. A mod names a file that holds both configs in the same shape:
//
//   {"DDKStages":   {"Set": [0, 1, ..., 11]},
//    "DDKHeldInfo": {"Latest": {"Stage": 12, "DeliveryDate": "2026-11-01T00:00:00Z",
//                               "TestDates": [{"Start": "...", "End": "..."}], "Ticket": 6, "UseTicketLimit": false},
//                    "Prev":   {"Stage": 11, ...}}}
//
// `Set` lists the exam periods in the order they come out, by the period's number in the course table (0 to 11);
// `Stage` is how many of them are out, counting from 1. `Prev` is the period before `Latest`, used until
// `Latest`'s DeliveryDate, and is needed when `Latest.Stage` is more than 1. Dates are ISO 8601. While the mod runs,
// the framework keeps the schedule manager holding what the game's own parser would make of these configs, and makes
// the delivery the game makes when it is signed in, so the Academy opens, lists, announces and keeps its exams the way
// it does online, the save included. Nothing is sent.
#include <string.h>

#include "files.h"
#include "game.h"
#include "json.h"
#include "mods.h"
#include "objects.h"
#include "registry.h"
#include "frame.h"
#include "services.h"
#include "shared.h"
#include "text.h"
#include "texts.h"

namespace tsuchi {
namespace {

namespace t = game::table;

constexpr uint32_t kMostPeriods = 64;     // entries of Set
constexpr uint32_t kMostTestDates = 64;   // windows of one period
constexpr int32_t kCoursePeriods = 12;    // rows of the game's exam course table: Test_00 to Test_11

struct Period {
    int32_t stage = 0;
    int64_t delivery = 0; // FDateTime ticks
    int64_t starts[kMostTestDates] = {};
    int64_t ends[kMostTestDates] = {};
    uint32_t dates = 0;
    int32_t ticket = 1;         // what the game's own empty schedule holds
    bool ticket_limit = true;
};

struct Schedule {
    bool active = false;
    TSUCHI_Mod* owner = nullptr;
    int32_t set[kMostPeriods] = {};
    uint32_t set_count = 0;
    Period latest, prev;
    bool has_prev = false;
    // The blocks last written into the game's arrays, so a schedule the game has replaced is noticed.
    uint64_t set_block = 0;
    uint32_t applied = 0;
};

Schedule schedule;
const Kind kSchedule{"academy.schedule"};

// ---- Dates ------------------------------------------------------------------

int64_t days_from_civil(int64_t year, int64_t month, int64_t day) {
    year -= month <= 2;
    const int64_t era = (year >= 0 ? year : year - 399) / 400;
    const int64_t yoe = year - era * 400;
    const int64_t doy = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
    const int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468; // days since 1970-01-01
}

bool digits(const char*& at, int count, int64_t& out) {
    out = 0;
    for (int i = 0; i < count; ++i, ++at) {
        if (*at < '0' || *at > '9') return false;
        out = out * 10 + (*at - '0');
    }
    return true;
}

// ISO 8601 as the game's own parser takes it: YYYY-MM-DD, optionally THH:MM[:SS[.fraction]], optionally Z or
// +HH:MM / -HH:MM. The result is an FDateTime: 100-nanosecond ticks since 0001-01-01, in UTC.
bool iso_ticks(const char* text, int64_t& ticks) {
    const char* at = text;
    int64_t year, month, day, hour = 0, minute = 0, second = 0, fraction = 0;
    if (!digits(at, 4, year) || *at++ != '-' || !digits(at, 2, month) || *at++ != '-' || !digits(at, 2, day)) return false;
    if (month < 1 || month > 12 || day < 1 || day > 31 || year < 1) return false;
    if (*at == 'T' || *at == 't') {
        ++at;
        if (!digits(at, 2, hour) || *at++ != ':' || !digits(at, 2, minute)) return false;
        if (*at == ':') {
            ++at;
            if (!digits(at, 2, second)) return false;
            if (*at == '.') {
                ++at;
                int places = 0;
                while (*at >= '0' && *at <= '9') {
                    if (places < 7) fraction = fraction * 10 + (*at - '0'), ++places;
                    ++at;
                }
                if (!places) return false;
                while (places < 7) fraction *= 10, ++places;
            }
        }
        if (hour > 23 || minute > 59 || second > 59) return false;
        int64_t offset = 0;
        if (*at == 'Z' || *at == 'z') {
            ++at;
        } else if (*at == '+' || *at == '-') {
            const bool ahead = *at++ == '+';
            int64_t oh, om = 0;
            if (!digits(at, 2, oh)) return false;
            if (*at == ':') ++at;
            if (*at && !digits(at, 2, om)) return false;
            offset = (oh * 60 + om) * (ahead ? 1 : -1);
        }
        minute -= offset;
    }
    if (*at) return false;
    constexpr int64_t kTicksPerSecond = 10000000;
    const int64_t days = days_from_civil(year, month, day) + 719162; // 0001-01-01 to 1970-01-01
    ticks = days * 86400 * kTicksPerSecond + (hour * 3600 + minute * 60 + second) * kTicksPerSecond + fraction;
    return true;
}

// The inverse of days_from_civil.
void civil_from_days(int64_t days, int64_t& year, int64_t& month, int64_t& day) {
    days += 719468;
    const int64_t era = (days >= 0 ? days : days - 146096) / 146097;
    const int64_t doe = days - era * 146097;
    const int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const int64_t mp = (5 * doy + 2) / 153;
    day = doy - (153 * mp + 2) / 5 + 1;
    month = mp < 10 ? mp + 3 : mp - 9;
    year = yoe + era * 400 + (month <= 2);
}

// Ticks as the game writes a delivery date: %Y-%m-%dT%H:%M:%S.%sZ, %s being milliseconds. 25 bytes with the end.
void iso_text(int64_t ticks, char (&out)[25]) {
    constexpr int64_t kTicksPerSecond = 10000000;
    const int64_t seconds = ticks / kTicksPerSecond;
    int64_t year, month, day;
    civil_from_days(seconds / 86400 - 719162, year, month, day);
    const int64_t of_day = seconds % 86400;
    const int64_t parts[] = {year, month, day, of_day / 3600, of_day / 60 % 60, of_day % 60, ticks % kTicksPerSecond / 10000};
    const int widths[] = {4, 2, 2, 2, 2, 2, 3};
    const char after[] = {'-', '-', 'T', ':', ':', '.', 'Z'};
    char* at = out;
    for (int i = 0; i < 7; ++i) {
        for (int place = widths[i] - 1; place >= 0; --place) {
            int64_t value = parts[i];
            for (int k = 0; k < place; ++k) value /= 10;
            *at++ = char('0' + value % 10);
        }
        *at++ = after[i];
    }
    *at = 0;
}

// ---- Reading the file ----------------------------------------------------------

bool whole_text(const Json* value) { return value && value->kind == Json::Kind::text && strlen(value->text) == value->bytes && value->bytes; }
bool whole_number(const Json* value) { return value && value->kind == Json::Kind::number && value->whole; }

// Keeps the parts, one after another, as `out`. False when there is no memory.
bool join(Text& out, const char* a, const char* b = "", const char* c = "", const char* d = "") {
    const char* const parts[] = {a, b, c, d};
    size_t bytes = 1;
    for (const char* part : parts) bytes += strlen(part);
    Text joined;
    if (!joined.reserve(bytes)) return false;
    char* at = joined.data;
    for (const char* part : parts) {
        const size_t length = strlen(part);
        memcpy(at, part, length);
        at += length;
    }
    *at = 0;
    const bool kept = out.set(joined.get());
    joined.clear();
    return kept;
}

// Says what is wrong, for the mod's author, and gives false back.
bool fail(Text& problem, const char* a, const char* b = "", const char* c = "") {
    join(problem, a, b, c);
    return false;
}

bool read_period(const Json* value, const char* name, Period& out, Text& problem) {
    if (!value || value->kind != Json::Kind::object) return fail(problem, "DDKHeldInfo.", name, " is an object: a period");
    const Json* stage = value->find("Stage");
    if (!whole_number(stage) || stage->integer < 1 || stage->integer > kMostPeriods)
        return fail(problem, "DDKHeldInfo.", name, ".Stage is how many periods of DDKStages.Set are out, from 1");
    out.stage = int32_t(stage->integer);
    const Json* delivery = value->find("DeliveryDate");
    if (!whole_text(delivery) || !iso_ticks(delivery->text, out.delivery))
        return fail(problem, "DDKHeldInfo.", name, ".DeliveryDate is a date and time in ISO 8601, such as 2026-11-01T00:00:00Z");
    out.dates = 0;
    if (const Json* dates = value->find("TestDates")) {
        if (dates->kind != Json::Kind::list || dates->count > kMostTestDates)
            return fail(problem, "DDKHeldInfo.", name, ".TestDates is a list of up to 64 {\"Start\": date, \"End\": date}");
        for (const Json* window = dates->first; window; window = window->next) {
            const Json* start = window->kind == Json::Kind::object ? window->find("Start") : nullptr;
            const Json* end = window->kind == Json::Kind::object ? window->find("End") : nullptr;
            int64_t from = 0, to = 0;
            if (!whole_text(start) || !whole_text(end) || !iso_ticks(start->text, from) || !iso_ticks(end->text, to) || to < from)
                return fail(problem, "DDKHeldInfo.", name, ".TestDates: each window is {\"Start\": date, \"End\": date} in ISO 8601, End not before Start");
            out.starts[out.dates] = from;
            out.ends[out.dates] = to;
            ++out.dates;
        }
    }
    if (const Json* ticket = value->find("Ticket")) {
        if (!whole_number(ticket) || ticket->integer < 0 || ticket->integer > 99) return fail(problem, "DDKHeldInfo.", name, ".Ticket is a whole number of Redo Passes, 0 to 99");
        out.ticket = int32_t(ticket->integer);
    }
    if (const Json* limit = value->find("UseTicketLimit")) {
        if (limit->kind != Json::Kind::boolean) return fail(problem, "DDKHeldInfo.", name, ".UseTicketLimit is true or false");
        out.ticket_limit = limit->boolean;
    }
    if (const Json* close = value->find("Close")) {
        // The game marks the player's save as past the end of the service for good when the current period is
        // closed, so a mod's schedule cannot close one.
        if (close->kind != Json::Kind::boolean || close->boolean)
            return fail(problem, "DDKHeldInfo.", name, ".Close can only be false: a closed period marks the save as past the end of the service");
    }
    return true;
}

bool read_schedule(const Json& root, Schedule& out, Text& problem) {
    if (root.kind != Json::Kind::object) return fail(problem, "the schedule is an object with DDKStages and DDKHeldInfo");
    const Json* stages = root.find("DDKStages");
    const Json* set = stages && stages->kind == Json::Kind::object ? stages->find("Set") : nullptr;
    if (!set || set->kind != Json::Kind::list || !set->count || set->count > kMostPeriods)
        return fail(problem, "DDKStages.Set is a list of 1 to 64 exam periods, by their number in the course table");
    out.set_count = 0;
    for (const Json* period = set->first; period; period = period->next) {
        if (!whole_number(period) || period->integer < 0 || period->integer >= kCoursePeriods)
            return fail(problem, "DDKStages.Set: each exam period is a number from 0 to 11");
        out.set[out.set_count++] = int32_t(period->integer);
    }
    const Json* held = root.find("DDKHeldInfo");
    if (!held || held->kind != Json::Kind::object) return fail(problem, "DDKHeldInfo is an object with Latest, and Prev when Latest.Stage is more than 1");
    if (!read_period(held->find("Latest"), "Latest", out.latest, problem)) return false;
    if (uint32_t(out.latest.stage) > out.set_count) return fail(problem, "DDKHeldInfo.Latest.Stage is more than DDKStages.Set has");
    out.has_prev = out.latest.stage > 1;
    if (out.has_prev) {
        if (!read_period(held->find("Prev"), "Prev", out.prev, problem)) return false;
        if (out.prev.stage > out.latest.stage) return fail(problem, "DDKHeldInfo.Prev.Stage is more than Latest.Stage");
        if (out.prev.delivery > out.latest.delivery) return fail(problem, "DDKHeldInfo.Prev.DeliveryDate is after Latest's");
    }
    return true;
}

// ---- The game's schedule manager -----------------------------------------------

// Memory from the engine's own allocator, which the game frees its arrays with.
void* engine_allocate(uint64_t bytes) {
    const uint64_t allocator = game::global<uint64_t>(t::data::allocator);
    const auto allocate = allocator ? game::method(allocator, t::allocator_vtable::malloc) : nullptr;
    if (!allocate || !game::method(allocator, t::allocator_vtable::free)) return nullptr;
    return allocate(reinterpret_cast<void*>(allocator), bytes, 0);
}

void engine_release(uint64_t block) {
    const uint64_t allocator = game::global<uint64_t>(t::data::allocator);
    const auto release = allocator && block ? game::method(allocator, t::allocator_vtable::free) : nullptr;
    if (release) release(reinterpret_cast<void*>(allocator), reinterpret_cast<void*>(block));
}

// Replaces one of the game's arrays with `count` elements. The block, or 0 when there was no memory (nothing changed).
uint64_t put_array(uint64_t header, const void* elements, uint32_t count, uint32_t element_bytes) {
    if (!game::writable(header, t::array::size)) return 0;
    uint64_t block = 0;
    if (count) {
        block = reinterpret_cast<uint64_t>(engine_allocate(uint64_t(count) * element_bytes));
        if (!block) return 0;
        memcpy(reinterpret_cast<void*>(block), elements, size_t(count) * element_bytes);
    }
    const uint64_t old = game::read_or<uint64_t>(header + t::array::data);
    game::write(header + t::array::data, block);
    game::write(header + t::array::count, int32_t(count));
    game::write(header + t::array::max, int32_t(count));
    if (old && old != block) engine_release(old);
    return block ? block : 1;
}

bool put_period(uint64_t at, const Period& period) {
    game::write(at + t::academy_period::stage, period.stage);
    game::write(at + t::academy_period::delivery, period.delivery);
    if (!put_array(at + t::academy_period::starts, period.starts, period.dates, sizeof(int64_t))) return false;
    if (!put_array(at + t::academy_period::ends, period.ends, period.dates, sizeof(int64_t))) return false;
    game::write(at + t::academy_period::ticket, period.ticket);
    game::write(at + t::academy_period::ticket_limit, uint8_t(period.ticket_limit));
    game::write(at + t::academy_period::closed, uint8_t(0));
    return true;
}

// Whether the manager still holds the schedule written last: the game puts its own empty one back when a tab changes
// while it has none. The download's status is not part of it, as the game marks each failed retry there.
bool still_ours(uint64_t manager) {
    return schedule.set_block && game::read_or<uint8_t>(manager + t::academy_schedule::valid) == 1 &&
           game::read_or<uint8_t>(manager + t::academy_schedule::uses_schedule) == 1 &&
           game::read_or<uint8_t>(manager + t::academy_schedule::refresh_pending) == 0 &&
           game::read_or<uint64_t>(manager + t::academy_schedule::set + t::array::data) == schedule.set_block &&
           game::read_or<int32_t>(manager + t::academy_schedule::set + t::array::count) == int32_t(schedule.set_count) &&
           game::read_or<int32_t>(manager + t::academy_schedule::latest + t::academy_period::stage) == schedule.latest.stage;
}

void apply(uint64_t manager) {
    const uint64_t set = put_array(manager + t::academy_schedule::set, schedule.set, schedule.set_count, sizeof(int32_t));
    if (!set || !put_period(manager + t::academy_schedule::latest, schedule.latest)) return;
    if (schedule.has_prev && !put_period(manager + t::academy_schedule::prev, schedule.prev)) return;
    game::write(manager + t::academy_schedule::first_period, uint8_t(!schedule.has_prev));
    game::write(manager + t::academy_schedule::refresh_pending, uint8_t(0));
    game::write(manager + t::academy_schedule::valid, uint8_t(1));
    game::write(manager + t::academy_schedule::uses_schedule, uint8_t(1));
    game::write(manager + t::academy_schedule::status, int32_t(1));
    schedule.set_block = set;
    if (++schedule.applied == 1 && schedule.owner) mod_note(kFrameworkLogId, "the Academy's exam schedule is the one a mod supplies");
}

// The delivery date in the save, as ticks. False when it holds none the game's parser would take.
bool delivered_at(uint64_t save, int64_t& ticks) {
    const uint64_t text = save + t::academy_save::delivery_text;
    const uint64_t units = game::read_or<uint64_t>(text + t::array::data);
    const int32_t count = game::read_or<int32_t>(text + t::array::count);
    if (!units || count < 2 || count > 64 || !game::readable(units, uint64_t(count) * 2)) return false;
    char ascii[64];
    for (int32_t i = 0; i < count; ++i) {
        const char16_t unit = game::read_or<char16_t>(units + uint64_t(i) * 2);
        if (unit > 0x7e) return false;
        ascii[i] = char(unit);
    }
    ascii[count - 1] = 0;
    return iso_ticks(ascii, ticks);
}

// The game's own delivery, which its schedule update runs only while it is signed in to the service: when the period
// in effect came out after what the save last had delivered, the save's delivered periods become the ones open now,
// and its delivery date that period's. The Academy then lists those periods and announces the new ones, and the game
// keeps them in the save as it does online.
void deliver(uint64_t manager) {
    if (!t::bound(t::fn::academy_open_periods) || !t::bound(t::fn::academy_period_start)) return;
    const uint64_t instance = game::global<uint64_t>(t::data::game_instance);
    const uint64_t save = instance ? game::read_or<uint64_t>(instance + t::game_instance::academy_save) : 0;
    if (!save || !game::writable(save + t::academy_save::held_ids, t::array::size) ||
        !game::writable(save + t::academy_save::delivery_text, t::array::size) ||
        !game::readable(save + t::academy_save::service_end, 1))
        return;
    if (game::read_or<uint8_t>(save + t::academy_save::service_end) == 1) return;
    const auto* record = reinterpret_cast<const void*>(manager + t::academy_schedule::record);
    const int64_t start = t::fn::academy_period_start(record, 0);
    int64_t delivered = 0;
    if (!start || (delivered_at(save, delivered) && delivered >= start)) return;
    const game::EngineArray open = t::fn::academy_open_periods(record);  // empty until the period has come out
    const uint64_t block = reinterpret_cast<uint64_t>(open.data);
    if (open.count <= 0 || open.count > int32_t(kMostPeriods) || !game::readable(block, uint64_t(open.count) * 4)) {
        engine_release(block);
        return;
    }
    char date[25];
    iso_text(start, date);
    char16_t units[25];
    for (int i = 0; i < 25; ++i) units[i] = char16_t(date[i]);
    const bool kept = put_array(save + t::academy_save::held_ids, open.data, uint32_t(open.count), sizeof(int32_t)) &&
                      put_array(save + t::academy_save::delivery_text, units, 25, sizeof(char16_t));
    engine_release(block);
    if (!kept) return;
    const bool announce = game::read_or<uint8_t>(save + t::academy_save::disable_test_guide) ||
                          game::read_or<uint8_t>(manager + t::academy_schedule::tab_checked);
    game::write(save + t::academy_save::update_delivery, uint8_t(announce));
    mod_note(kFrameworkLogId, "the Academy delivered the exam periods of the mod's schedule");
}

// ---- The course table ---------------------------------------------------------
//
// The Academy copies each exam period's row of its course table (the period's six courses, with their levels, time
// limits, medal scores and rewards) once, as the game starts, and lists and starts its courses from that copy. The
// tables service is told where the copy of each row is, so a change a mod makes to a course reaches the Academy.

game::ClassRef table_class{t::name::data_table_class};

// The table the Academy made its copy from, through its data asset's reference to it, as the game loads it.
uint64_t courses_table(uint64_t ui) {
    const uint64_t data = game::read_or<uint64_t>(ui + t::ui_manager::academy_data);
    if (!data || !t::bound(t::fn::load_soft_object) || !game::writable(data + t::academy_data::test_courses, t::soft_object::most)) return 0;
    const auto table = reinterpret_cast<uint64_t>(t::fn::load_soft_object(reinterpret_cast<void*>(data + t::academy_data::test_courses)));
    return table && game::is_a(table, table_class) ? table : 0;
}

// Where the Academy keeps its copy of a row of its course table, or 0: for a row of another table, or before the
// Academy has made its copy.
uint64_t course_copy(uint64_t table, uint64_t row) {
    const uint64_t framework = game::global<uint64_t>(t::data::game_framework);
    const uint64_t ui = framework ? game::read_or<uint64_t>(framework + t::game_framework::ui) : 0;
    if (!ui || !row) return 0;
    const uint64_t entries = ui + t::ui_manager::academy_courses;
    const uint64_t data = game::read_or<uint64_t>(entries + t::sparse_array::data);
    const int32_t count = game::read_or<int32_t>(entries + t::sparse_array::count);
    if (!data || count <= 0 || courses_table(ui) != table) return 0;
    const uint64_t more = game::read_or<uint64_t>(entries + t::sparse_array::more_bits);
    const uint64_t bits = more ? more : entries + t::sparse_array::bits;
    const int32_t held = game::read_or<int32_t>(entries + t::sparse_array::bit_count);
    for (int32_t i = 0; i < count && i < held; ++i) {
        if (!(game::read_or<uint32_t>(bits + uint64_t(i / 32) * 4) >> (i % 32) & 1)) continue;  // a free entry
        const uint64_t entry = data + uint64_t(i) * t::academy_course_entry::size;
        if (game::read_or<uint64_t>(entry + t::academy_course_entry::key) == row) return entry + t::academy_course_entry::row;
    }
    return 0;
}

void each_frame() {
    if (!schedule.active) return;
    const uint64_t manager = game::global<uint64_t>(t::data::academy_schedule);
    if (!manager || !game::writable(manager + t::academy_schedule::valid, 1)) return;
    if (!still_ours(manager)) {
        apply(manager);
        if (!still_ours(manager)) return;
    } else if (game::read_or<int32_t>(manager + t::academy_schedule::status) != 1) {
        game::write(manager + t::academy_schedule::status, int32_t(1));  // the game's download retry failed again
    }
    deliver(manager);
}

// The owner stopped: the manager is left as the game has it with no download.
void ended(void*, uint64_t) {
    const uint64_t manager = game::global<uint64_t>(t::data::academy_schedule);
    if (manager && still_ours(manager)) {
        game::write(manager + t::academy_schedule::valid, uint8_t(0));
        game::write(manager + t::academy_schedule::uses_schedule, uint8_t(0));
        game::write(manager + t::academy_schedule::status, int32_t(0));
    }
    schedule.active = false;
    schedule.owner = nullptr;
    schedule.set_block = 0;
}

bool well_formed(const Json& part, Text& wrong) {
    if (!whole_text(&part)) return wrong.set("\"academy_schedule\" is the path of a JSON file in the mod that holds the DDKStages and DDKHeldInfo schedule"), false;
    return true;
}

TSUCHI_Result begin(TSUCHI_Mod* mod, const Json& part, Text& problem) {
    if (schedule.active && schedule.owner != mod) {
        fail(problem, "\"academy_schedule\": another mod supplies the Academy's schedule");
        return TSUCHI_BUSY;
    }
    Texts where;
    char* text = nullptr;
    uint64_t bytes = 0;
    const TSUCHI_Result read = mod_file_path(where, mod, part.text) ? game::read_whole_file(where.data, text, bytes) : TSUCHI_NO_MEMORY;
    where.clear();
    if (read != TSUCHI_OK) {
        fail(problem, "\"academy_schedule\": ", part.text, read == TSUCHI_NOT_FOUND ? ": the mod has no such file" : ": it could not be read");
        return read;
    }
    JsonError error{};
    const Json* root = read_json(text, size_t(bytes), &error);
    release(text);
    if (!root) {
        fail(problem, "\"academy_schedule\": ", part.text, error.what ? ": it is not JSON" : ": there is no memory to read it");
        return error.what ? TSUCHI_INVALID : TSUCHI_NO_MEMORY;
    }
    Schedule next;
    Text wrong;
    const bool good = read_schedule(*root, next, wrong);
    release_json(root);
    if (!good) {
        join(problem, "\"academy_schedule\": ", part.text, ": ", wrong.get());
        wrong.clear();
        return TSUCHI_INVALID;
    }
    wrong.clear();
    const TSUCHI_Result owned = own(mod, kSchedule, 1, ended, nullptr, true);
    if (owned != TSUCHI_OK) {
        fail(problem, "\"academy_schedule\": another mod supplies the Academy's schedule");
        return owned;
    }
    next.active = true;
    next.owner = mod;
    next.set_block = 0;
    schedule = next;
    return TSUCHI_OK;
}

} // namespace

void academy_start() {
    add_manifest_section({"academy_schedule", well_formed, begin});
    each_engine_frame(each_frame);
    add_row_copies(course_copy);
}

} // namespace tsuchi
