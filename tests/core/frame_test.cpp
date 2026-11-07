// The real core phases: service sampling, mod writes, consumers, then state commits.
#include "frame.h"
#include "registry.h"
#include "time_service.h"
#include <cstdio>
#include <cstdlib>
#include <string>
using namespace tsuchi;
static unsigned checks = 0;
#define CHECK(x)                                                                                                                           \
    do {                                                                                                                                   \
        ++checks;                                                                                                                          \
        if (!(x)) {                                                                                                                        \
            std::fprintf(stderr, "line %d: %s\n", __LINE__, #x);                                                                           \
            std::exit(1);                                                                                                                  \
        }                                                                                                                                  \
    } while (0)
static uint64_t thread = 1;
static uint64_t probe() {
    return thread;
}
static bool no_memory = false;
static void* memory(size_t bytes) {
    return no_memory ? nullptr : std::malloc(bytes);
}
static std::string order;
static int state = 0, history = 0;
static long long number = 0;
static TSUCHI_Mod* mod;
static const Kind state_kind{"phase.state"};
static void restore(void*, uint64_t) {
    CHECK(frame_stepping());
    state = 0;
    order += 'r';
}
static void sample() {
    CHECK(frame_stepping());
    CHECK(history == state);
    order += 's';
}
static void mod_write(void*, const TSUCHI_TimeStep* event) {
    CHECK(frame_stepping());
    CHECK(event->number == ++number);
    order += 'm';
    state = 1;
    CHECK(own(mod, state_kind, 1, restore, nullptr, true) == TSUCHI_OK);
}
static bool stopping = false, peek_from_another_thread = false;
static void consume() {
    CHECK(frame_stepping());
    if (peek_from_another_thread) {
        thread = 2;
        CHECK(!frame_stepping()); // another thread never sees a step
        thread = 1;
    }
    order += 'u';
    CHECK(state == 1);
    if (stopping)
        mod_stop(mod); // This is the same path as an action stopping its own mod.
}
static void commit() {
    CHECK(frame_stepping());
    order += 'c';
    history = state;
}
static void picture(void*, const TSUCHI_TimePresented*) {
    CHECK(!frame_stepping());
    order += 'p';
}
int main() {
    set_thread_probe(probe);
    frame_begin();
    set_allocator({memory, std::free});
    CHECK(!each_frame(nullptr) && !after_step(nullptr));
    no_memory = true;
    CHECK(!after_step(commit));
    no_memory = false;
    CHECK(each_frame(sample));
    CHECK(after_step(consume));
    CHECK(after_step(commit));
    mod = mod_create("phase.mod");
    TSUCHI_Mod* observer = mod_create("phase.observer");
    TSUCHI_Subscription steps{}, pictures{};
    CHECK(time::table.on_step(mod, mod_write, nullptr, &steps) == TSUCHI_OK);
    CHECK(time::table.on_presented(observer, picture, nullptr, &pictures) == TSUCHI_OK);
    CHECK(!frame_stepping());
    frame_picture(.5f, 1.0f / 60.0f);
    CHECK(order == "p" && state == 0 && history == 0 && number == 0);
    order.clear();
    frame_step();
    CHECK(order == "smuc" && state == 1 && history == 1 && number == 1 && !frame_stepping());
    order.clear();
    frame_picture(.5f, 1.0f / 60.0f);
    frame_picture(1, 1.0f / 60.0f);
    CHECK(order == "pp" && history == 1 && number == 1); // No-step pictures never commit twice.
    order.clear();
    peek_from_another_thread = true;
    frame_step();
    peek_from_another_thread = false;
    CHECK(order == "smuc" && number == 2 && history == 1 && !frame_stepping());
    order.clear();
    stopping = true;
    frame_step();
    CHECK(order == "smurc" && state == 0 && history == 0 && number == 3);
    CHECK(owned_count(mod) == 0 && !mod_usable(mod) && !frame_stepping());
    std::printf("frame phases: %u checks passed\n", checks);
}
