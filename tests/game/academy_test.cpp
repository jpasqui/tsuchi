// The Academy schedule a mod supplies: its dates, and what read_schedule takes and refuses.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <initializer_list>

#include "fake_game.h"

// One translation unit, so the test reaches the file's own readers.
#include "../../framework/game/academy.cpp"

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "line %d: %s\n", __LINE__, #x); exit(1); } } while (0)

// What the file asks of the framework around it, which these checks do not reach.
namespace tsuchi {
void academy_deliver(uint64_t) {}
bool delivery_available = true;
bool academy_delivery_ready() { return delivery_available; }
void academy_backend_start() {}
TSUCHI_Result academy_block_uploads() { return TSUCHI_OK; }
bool academy_uploads_blocked() { return true; }
void academy_unblock_uploads() {}
bool add_manifest_section(const ManifestSection&) { return true; }
bool each_engine_frame(void (*)()) { return true; }
bool mod_file_path(Texts&, const TSUCHI_Mod*, const char*) { return false; }
void mod_note(const char*, const char*) {}
void add_row_copies(TableRowCopy (*)(uint64_t, uint64_t, uint64_t)) {}
namespace game {
TSUCHI_Result read_whole_file(const char*, char*&, uint64_t&) { return TSUCHI_NOT_FOUND; }
bool is_a(uint64_t object, ClassRef&) { return readable(object, 1); }
uint64_t from_handle(uint64_t handle) { return readable(handle, 1) ? handle : 0; }
uint64_t to_handle(uint64_t object) { return readable(object, 1) ? object : 0; }
}
}

namespace {

using namespace tsuchi;

constexpr int64_t kUnixEpoch = 621355968000000000;  // FDateTime ticks of 1970-01-01T00:00:00Z
constexpr int64_t kSecond = 10000000;

int64_t ticks_of(const char* text) {
    int64_t ticks = 0;
    CHECK(iso_ticks(text, ticks));
    return ticks;
}

void dates() {
    CHECK(ticks_of("1970-01-01T00:00:00Z") == kUnixEpoch);
    CHECK(ticks_of("1970-01-01") == kUnixEpoch);
    CHECK(ticks_of("2026-11-01T00:00:00Z") == kUnixEpoch + 1793491200 * kSecond);
    CHECK(ticks_of("2026-11-01T09:00:00+09:00") == ticks_of("2026-11-01T00:00:00Z"));
    CHECK(ticks_of("2026-10-31T19:00:00-05:00") == ticks_of("2026-11-01T00:00:00Z"));
    CHECK(ticks_of("2026-11-01T00:00:00.5Z") == ticks_of("2026-11-01T00:00:00Z") + kSecond / 2);
    CHECK(ticks_of("2024-02-29T23:59:59Z") + kSecond == ticks_of("2024-03-01T00:00:00Z"));
    CHECK(ticks_of("2000-02-29") + 86400 * kSecond == ticks_of("2000-03-01"));
    CHECK(ticks_of("2026-11-01T09:00:00+0900") == ticks_of("2026-11-01T00:00:00Z"));
    CHECK(ticks_of("2026-11-01T09:00:00+09") == ticks_of("2026-11-01T00:00:00Z"));
    CHECK(ticks_of("2026-11-01T00:00:00+23:59") == ticks_of("2026-10-31T00:01:00Z"));
    int64_t ignored = 0;
    for (const char* wrong : {"", "2026-11-01T", "2026-13-01", "2026-11-01T24:00:00Z", "2026-11-01 00:00:00",
                              "2026-11-01T00:00:00.Z", "2026-11-01T00:00:00Zjunk", "26-11-01",
                              "2026-02-29", "2026-02-31", "2026-04-31", "1900-02-29", "2026-11-32",
                              "2026-11-01T00:00:00+24:00", "2026-11-01T00:00:00+09:60", "2026-11-01T00:00:00+99:99",
                              "2026-11-01T00:00:00+09:"})
        CHECK(!iso_ticks(wrong, ignored));
    // A fraction finer than a millisecond is cut to the millisecond, so what is written back reads the same.
    CHECK(ticks_of("2026-11-01T00:00:00.0005Z") == ticks_of("2026-11-01T00:00:00.000Z"));
    CHECK(ticks_of("2026-11-01T00:00:00.1239999Z") == ticks_of("2026-11-01T00:00:00.123Z"));

}

bool reads(const char* text, Schedule& out, Text& problem) {
    const Json* root = read_json(text, strlen(text));
    CHECK(root);
    const bool good = read_schedule(*root, out, problem);
    release_json(root);
    return good;
}

void refused(const char* text, const char* expected) {
    Schedule out;
    Text problem;
    CHECK(!reads(text, out, problem));
    if (!strstr(problem.get(), expected)) {
        fprintf(stderr, "expected \"%s\" in \"%s\"\n", expected, problem.get());
        exit(1);
    }
    problem.clear();
}

void schedules() {
    Schedule out;
    Text problem;
    CHECK(reads(R"({"DDKStages": {"Set": [0, 1, 2, 11]},
                    "DDKHeldInfo": {"Latest": {"Stage": 4, "DeliveryDate": "2026-11-01T00:00:00Z",
                                               "TestDates": [{"Start": "2026-11-01T00:00:00Z", "End": "2026-12-31T23:59:59Z"}],
                                               "Ticket": 6, "UseTicketLimit": false, "Close": false},
                                    "Prev": {"Stage": 3, "DeliveryDate": "2026-10-01T00:00:00Z"}}})",
                out, problem));
    CHECK(out.set_count == 4 && out.set[3] == 11);
    CHECK(out.latest.stage == 4 && out.latest.dates == 1 && out.latest.ticket == 6 && !out.latest.ticket_limit);
    CHECK(out.latest.ends[0] > out.latest.starts[0]);
    CHECK(out.has_prev && out.prev.stage == 3 && out.prev.dates == 0);
    CHECK(out.prev.ticket == 1 && out.prev.ticket_limit);  // what the game's own empty period holds
    // One period out needs no Prev.
    CHECK(reads(R"({"DDKStages": {"Set": [5]}, "DDKHeldInfo": {"Latest": {"Stage": 1, "DeliveryDate": "2026-11-01"}}})",
                out, problem));
    CHECK(!out.has_prev && out.set[0] == 5);

    refused(R"([])", "an object with DDKStages");
    refused(R"({"DDKStages": {"Set": []}, "DDKHeldInfo": {}})", "1 to 64 exam periods");
    refused(R"({"DDKStages": {"Set": [12]}, "DDKHeldInfo": {}})", "a number from 0 to 11");
    refused(R"({"DDKStages": {"Set": [0]}})", "DDKHeldInfo is an object");
    refused(R"({"DDKStages": {"Set": [0]}, "DDKHeldInfo": {"Latest": {"Stage": 2, "DeliveryDate": "2026-11-01"}}})",
            "Latest.Stage is more than DDKStages.Set has");
    refused(R"({"DDKStages": {"Set": [0, 1]}, "DDKHeldInfo": {"Latest": {"Stage": 2, "DeliveryDate": "2026-11-01"}}})",
            "DDKHeldInfo.Prev is an object");
    refused(R"({"DDKStages": {"Set": [0]}, "DDKHeldInfo": {"Latest": {"Stage": 1, "DeliveryDate": "soon"}}})",
            "Latest.DeliveryDate is a date");
    refused(R"({"DDKStages": {"Set": [0]}, "DDKHeldInfo": {"Latest": {"Stage": 1, "DeliveryDate": "2026-11-01",
                "TestDates": [{"Start": "2026-11-02", "End": "2026-11-01"}]}}})", "End not before Start");
    refused(R"({"DDKStages": {"Set": [0]}, "DDKHeldInfo": {"Latest": {"Stage": 1, "DeliveryDate": "2026-11-01", "Close": true}}})",
            "Close can only be false");
    refused(R"({"DDKStages": {"Set": [0]}, "DDKHeldInfo": {"Latest": {"Stage": 1, "DeliveryDate": "2026-11-01", "Ticket": 100}}})",
            "Ticket is a whole number");
    refused(R"({"DDKStages": {"Set": [0, 1]}, "DDKHeldInfo": {"Latest": {"Stage": 2, "DeliveryDate": "2026-11-01"},
                "Prev": {"Stage": 1, "DeliveryDate": "2026-12-01"}}})", "Prev.DeliveryDate is after Latest's");
}

void ownership() {
    auto* first = mod_create("academy-open-exams");
    auto* second = mod_create("academy-community-board");
    CHECK(first && second);
    constexpr const char* valid = R"({"DDKStages":{"Set":[0]},"DDKHeldInfo":{"Latest":{"Stage":1,"DeliveryDate":"2026-11-01"}}})";
    constexpr const char* changed = R"({"DDKStages":{"Set":[5]},"DDKHeldInfo":{"Latest":{"Stage":1,"DeliveryDate":"2026-11-02"}}})";
    delivery_available = false;
    CHECK(academy_hold_schedule(first, valid) == TSUCHI_NOT_READY && !schedule.active && !owned_count(first));
    delivery_available = true;
    CHECK(academy_hold_schedule(first, valid) == TSUCHI_OK);
    CHECK(schedule.active && schedule.owner == first && schedule.pending && schedule.set[0] == 0);
    CHECK(owned_count(first) == 1);
    CHECK(academy_hold_schedule(second, valid) == TSUCHI_BUSY);
    CHECK(academy_release_schedule(second) == TSUCHI_NOT_FOUND);
    CHECK(academy_hold_schedule(first, "{") == TSUCHI_INVALID && schedule.set[0] == 0);
    CHECK(academy_hold_schedule(first, "{}") == TSUCHI_INVALID && schedule.set[0] == 0);
    CHECK(academy_hold_schedule(first, changed) == TSUCHI_OK && schedule.set[0] == 5);
    CHECK(owned_count(first) == 1);
    const auto previous_floor = isolate_ownership(first, ownership_mark(first));
    CHECK(academy_hold_schedule(first, valid) == TSUCHI_BUSY && schedule.set[0] == 5);
    CHECK(academy_release_schedule(first) == TSUCHI_BUSY);
    isolate_ownership(first, previous_floor);
    CHECK(academy_release_schedule(first) == TSUCHI_OK && !schedule.active);
    CHECK(owned_count(first) == 0);
    CHECK(academy_hold_schedule(second, valid) == TSUCHI_OK);
    mod_stop(second);
    CHECK(!schedule.active);
    CHECK(academy_hold_schedule(second, valid) == TSUCHI_INVALID);
    CHECK(academy_hold_schedule(first, valid) == TSUCHI_OK);
    mod_stop(first);
    CHECK(!schedule.active);
}

int allocation_call = 0, allocation_failure = 0, freed_blocks = 0;
void* native_allocate(void*, uint64_t bytes, uint32_t) {
    if (++allocation_call == allocation_failure) return nullptr;
    return reinterpret_cast<void*>(fake::memory(size_t(bytes)));
}
void native_free(void*, void* block) {
    ++freed_blocks;
    fake::unmap(reinterpret_cast<uint64_t>(block));
}

void schedule_transactions() {
    const auto set = [](auto field, uint64_t value) { t::cells[field.index] = value; };
    set(t::data::allocator, 0x10);
    set(t::data::academy_schedule, 0x18);
    set(t::allocator_vtable::malloc.offset, 0x10);
    set(t::allocator_vtable::free.offset, 0x18);
    set(t::academy_schedule::status, 4);
    set(t::academy_schedule::valid, 8);
    set(t::academy_schedule::set, 0x10);
    set(t::academy_schedule::latest, 0x40);
    set(t::academy_schedule::prev, 0xc0);
    set(t::academy_schedule::first_period, 0x150);
    set(t::academy_schedule::refresh_pending, 0x151);
    set(t::academy_schedule::uses_schedule, 0x152);
    set(t::academy_period::stage, 0);
    set(t::academy_period::delivery, 8);
    set(t::academy_period::starts, 0x10);
    set(t::academy_period::ends, 0x20);
    set(t::academy_period::ticket, 0x30);
    set(t::academy_period::ticket_limit, 0x34);
    set(t::academy_period::closed, 0x35);
    fake::main_image() = fake::memory(0x80);
    const uint64_t allocator = fake::memory(0x40), methods = fake::memory(0x40), manager = fake::memory(0x200);
    fake::put<uint64_t>(fake::main_image() + t::data::allocator, allocator);
    fake::put<uint64_t>(fake::main_image() + t::data::academy_schedule, manager);
    fake::put<uint64_t>(allocator, methods);
    fake::put<uint64_t>(methods + t::allocator_vtable::malloc, reinterpret_cast<uint64_t>(native_allocate));
    fake::put<uint64_t>(methods + t::allocator_vtable::free, reinterpret_cast<uint64_t>(native_free));
    fake::code().push_back(reinterpret_cast<uint64_t>(native_allocate));
    fake::code().push_back(reinterpret_cast<uint64_t>(native_free));
    auto* mod = mod_create("academy-transaction");
    CHECK(mod);
    constexpr const char* requested = R"({"DDKStages":{"Set":[0]},"DDKHeldInfo":{"Latest":{"Stage":1,
        "DeliveryDate":"2026-11-01","TestDates":[{"Start":"2026-11-01","End":"2026-11-02"}]}}})";
    CHECK(academy_hold_schedule(mod, requested) == TSUCHI_OK);
    unsigned char before[0x200];
    memcpy(before, reinterpret_cast<void*>(manager), sizeof before);
    for (int failure = 1; failure <= 3; ++failure) {
        allocation_failure = failure;
        allocation_call = 0;
        freed_blocks = 0;
        apply(manager);
        CHECK(!memcmp(before, reinterpret_cast<void*>(manager), sizeof before));
        CHECK(schedule.pending && !schedule.set_block && freed_blocks == failure - 1);
    }
    allocation_failure = 0;
    allocation_call = 0;
    apply(manager);
    CHECK(allocation_call == 3 && !schedule.pending && still_ours(manager));
    const uint64_t first_block = schedule.set_block;
    CHECK(game::read_or<int32_t>(first_block) == 0);
    constexpr const char* replaced = R"({"DDKStages":{"Set":[5]},"DDKHeldInfo":{"Latest":{"Stage":1,"DeliveryDate":"2026-11-02"}}})";
    CHECK(academy_hold_schedule(mod, replaced) == TSUCHI_OK);
    CHECK(schedule.pending && schedule.set_block == first_block && still_ours(manager));
    apply(manager);
    CHECK(!schedule.pending && schedule.set_block != first_block && still_ours(manager));
    CHECK(game::read_or<int32_t>(schedule.set_block) == 5);
    CHECK(!game::readable(first_block, 4));
    CHECK(academy_hold_schedule(mod, requested) == TSUCHI_OK && schedule.pending && still_ours(manager));
    CHECK(academy_release_schedule(mod) == TSUCHI_OK);
    CHECK(game::read_or<uint8_t>(manager + t::academy_schedule::valid) == 0);
    CHECK(!schedule.active);
    mod_stop(mod);
}


uint32_t soft_loads = 0;

void course_copies() {
    const auto set = [](auto field, uint64_t value) { t::cells[field.index] = value; };
    set(t::data::game_framework, 0x30); set(t::game_framework::ui, 0x40);
    set(t::ui_manager::academy_data, 0x48); set(t::ui_manager::academy_courses, 0x70); set(t::ui_manager::academy_training_courses, 0xb0);
    set(t::academy_data::test_courses, 0x20); set(t::academy_data::training_courses, 0x50);
    set(t::academy_course_entry::size, 0x50); set(t::academy_course_entry::key, 0); set(t::academy_course_entry::row, 0x10);
    set(t::academy_training_course_entry::size, 0x190); set(t::academy_training_course_entry::key, 0); set(t::academy_training_course_entry::row, 0x10);
    fake::main_image() = fake::memory(0x100);
    const auto framework = fake::memory(0x100), first_ui = fake::memory(0x200), next_ui = fake::memory(0x200);
    const auto asset = fake::memory(0x100), exam_table = fake::memory(0x40), training_table = fake::memory(0x40);
    fake::put<uint64_t>(fake::main_image() + t::data::game_framework, framework);
    fake::put<uint64_t>(framework + t::game_framework::ui, first_ui);
    fake::put<uint64_t>(asset + t::academy_data::test_courses, exam_table);
    fake::put<uint64_t>(asset + t::academy_data::training_courses, training_table);
    t::fn::load_soft_object = [](void* reference) -> void* { ++soft_loads; return reinterpret_cast<void*>(game::read_or<uint64_t>(reinterpret_cast<uint64_t>(reference))); };
    const auto make_map = [&](uint64_t ui, bool training) {
        fake::put<uint64_t>(ui + t::ui_manager::academy_data, asset);
        const uint64_t stride = training ? t::academy_training_course_entry::size : t::academy_course_entry::size;
        const uint64_t entries = fake::memory(size_t(stride * 2));
        const uint64_t at = ui + (training ? t::ui_manager::academy_training_courses : t::ui_manager::academy_courses);
        fake::put<uint64_t>(at + t::sparse_array::data, entries);
        fake::put<int32_t>(at + t::sparse_array::count, 2); fake::put<int32_t>(at + t::sparse_array::bit_count, 2);
        fake::put<uint32_t>(at + t::sparse_array::bits, 2); // the first entry is free
        fake::put<uint64_t>(entries, training ? 0x5678 : 0x1234);
        fake::put<uint64_t>(entries + stride, training ? 0x5678 : 0x1234);
        return entries + stride + 0x10;
    };
    const auto first_exam = make_map(first_ui, false), first_training = make_map(first_ui, true);
    const auto next_exam = make_map(next_ui, false), next_training = make_map(next_ui, true);
    CHECK(course_copy(exam_table, 0x1234, 0).base == first_exam && course_copy(training_table, 0x5678, 0).base == first_training);
    CHECK(course_copy(exam_table, 0x1234, 0).owner == first_ui && course_copy(training_table, 0x5678, 0).owner == first_ui);
    fake::put<uint64_t>(framework + t::game_framework::ui, next_ui);
    CHECK(course_copy(exam_table, 0x1234, 0).base == next_exam && course_copy(training_table, 0x5678, 0).base == next_training);
    CHECK(course_copy(exam_table, 0x1234, first_ui).base == first_exam && course_copy(training_table, 0x5678, first_ui).base == first_training);
    fake::unmap(first_ui);
    CHECK(!course_copy(exam_table, 0x1234, first_ui).base && !course_copy(training_table, 0x5678, first_ui).base);
    CHECK(!course_copy(training_table, 0x1234, 0).base && !course_copy(exam_table, 0x5678, 0).base);
    // Every field written into any table asks. A table is resolved only for a row the Academy has a copy of.
    const auto other_table = fake::memory(0x40);
    soft_loads = 0;
    CHECK(!course_copy(other_table, 0x9999, 0).base && soft_loads == 0);
    CHECK(!course_copy(other_table, 0x1234, 0).base && soft_loads == 1);
}

} // namespace

int main() {
    tsuchi::set_allocator({[](size_t bytes) -> void* { return malloc(bytes); }, [](void* block) { free(block); }});
    dates();
    schedules();
    ownership();
    schedule_transactions();
    course_copies();
    puts("academy schedule checks passed");
    return 0;
}
