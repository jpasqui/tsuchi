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
bool add_manifest_section(const ManifestSection&) { return true; }
bool each_engine_frame(void (*)()) { return true; }
bool mod_file_path(Texts&, const TSUCHI_Mod*, const char*) { return false; }
void mod_note(const char*, const char*) {}
void add_row_copies(uint64_t (*)(uint64_t, uint64_t)) {}
namespace game {
TSUCHI_Result read_whole_file(const char*, char*&, uint64_t&) { return TSUCHI_NOT_FOUND; }
bool is_a(uint64_t, ClassRef&) { return false; }
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
    {
        char written[25];
        iso_text(ticks_of("2026-11-01T00:00:00.0005Z"), written);
        CHECK(!strcmp(written, "2026-11-01T00:00:00.000Z"));
        CHECK(ticks_of(written) == ticks_of("2026-11-01T00:00:00.0005Z"));
        iso_text(ticks_of("2026-11-01T00:00:00.1239999Z"), written);
        CHECK(!strcmp(written, "2026-11-01T00:00:00.123Z"));
        CHECK(ticks_of(written) == ticks_of("2026-11-01T00:00:00.1239999Z"));
    }
    // The game's own form of a delivery date, which the save keeps.
    char text[25];
    iso_text(ticks_of("2026-11-01T00:00:00Z"), text);
    CHECK(!strcmp(text, "2026-11-01T00:00:00.000Z"));
    iso_text(ticks_of("2099-12-31T23:59:59.123Z"), text);
    CHECK(!strcmp(text, "2099-12-31T23:59:59.123Z"));
    for (int64_t day = -1000; day < 200000; day += 37) {
        int64_t year, month, d;
        civil_from_days(day, year, month, d);
        CHECK(days_from_civil(year, month, d) == day);
    }
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

} // namespace

int main() {
    tsuchi::set_allocator({[](size_t bytes) -> void* { return malloc(bytes); }, [](void* block) { free(block); }});
    dates();
    schedules();
    puts("academy schedule checks passed");
    return 0;
}
