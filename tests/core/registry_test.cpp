// Checks of the core: mods, ownership and events. Exits with the number of failures.
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "registry.h"

namespace {

int failures = 0;
#define CHECK(condition) \
    do { \
        if (!(condition)) { \
            std::printf("%s:%d: %s\n", __FILE__, __LINE__, #condition); \
            ++failures; \
        } \
    } while (0)

std::vector<std::string> journal; // what happened, in order
void note(const std::string& text) { journal.push_back(text); }
std::string joined() {
    std::string out;
    for (const std::string& line : journal) out += line + ' ';
    journal.clear();
    return out;
}

struct Ping {
    int value;
};
using PingFn = void (*)(void* user, const Ping* event);
tsuchi::Topic ping{"test.ping", [](void (*function)(), void* user, const void* event) {
                    reinterpret_cast<PingFn>(function)(user, static_cast<const Ping*>(event));
                }};
TSUCHI_Result listen(TSUCHI_Mod* mod, PingFn function, void* user, TSUCHI_Subscription* out) {
    return tsuchi::subscribe(ping, mod, reinterpret_cast<void (*)()>(function), user, out);
}
void heard(void* user, const Ping* event) { note(std::string(static_cast<const char*>(user)) + std::to_string(event->value)); }

const tsuchi::Kind kActor{"actor"}, kCamera{"camera"};
void released(void* context, uint64_t key) { note(std::string(static_cast<const char*>(context)) + std::to_string(key)); }

void mods() {
    CHECK(!tsuchi::mod_create(nullptr) && !tsuchi::mod_create("") && !tsuchi::mod_create("Has Capitals"));
    CHECK(!tsuchi::mod_create("this-id-is-far-too-long-to-be-a-valid-mod-identifier"));
    TSUCHI_Mod* a = tsuchi::mod_create("test.a");
    CHECK(a && tsuchi::mod_usable(a) && std::string(tsuchi::mod_id(a)) == "test.a");
    CHECK(!tsuchi::mod_create("test.a")); // in use
    CHECK(!tsuchi::mod_usable(nullptr));
    tsuchi::mod_stop(a);
    CHECK(!tsuchi::mod_usable(a));
    tsuchi::mod_stop(a); // a second stop does nothing
    TSUCHI_Mod* again = tsuchi::mod_create("test.a"); // a stopped mod's id is free again
    CHECK(again && again != a && tsuchi::mod_usable(again) && !tsuchi::mod_usable(a));
    // A reason a mod cannot run is kept with its context: the first one given, and for that start of the mod only.
    static const char first[] = "it lacks one thing", second[] = "it lacks another";
    CHECK(!tsuchi::mod_block_reason(again) && !tsuchi::mod_block_reason(nullptr));
    tsuchi::mod_block(again, first), tsuchi::mod_block(again, second), tsuchi::mod_block(nullptr, second);
    CHECK(tsuchi::mod_block_reason(again) == first && !tsuchi::mod_block_reason(a));
    tsuchi::mod_stop(again);
    CHECK(tsuchi::mod_block_reason(again) == first);
    TSUCHI_Mod* third = tsuchi::mod_create("test.a");
    CHECK(third && !tsuchi::mod_block_reason(third));
    tsuchi::mod_stop(third);
}

void events() {
    TSUCHI_Mod* a = tsuchi::mod_create("events.a");
    TSUCHI_Mod* b = tsuchi::mod_create("events.b");
    TSUCHI_Subscription first{}, second{}, third{};
    char name_a[] = "a", name_b[] = "b", name_c[] = "c";
    CHECK(listen(a, heard, name_a, &first) == TSUCHI_OK);
    CHECK(listen(b, heard, name_b, &second) == TSUCHI_OK);
    CHECK(listen(a, heard, name_c, &third) == TSUCHI_OK);
    CHECK(first.id && second.id && third.id && first.id != second.id && second.id != third.id);
    CHECK(listen(a, nullptr, nullptr, &first) == TSUCHI_INVALID);
    CHECK(listen(a, heard, nullptr, nullptr) == TSUCHI_INVALID);
    CHECK(tsuchi::subscriber_count(ping) == 3 && tsuchi::owned_count(a) == 2);

    const Ping one{1};
    tsuchi::emit(ping, &one);
    CHECK(joined() == "a1 b1 c1 "); // in the order they subscribed

    CHECK(tsuchi::cancel(a, second) == TSUCHI_NOT_FOUND); // b's, not a's
    CHECK(tsuchi::cancel(a, first) == TSUCHI_OK);
    CHECK(tsuchi::cancel(a, first) == TSUCHI_NOT_FOUND);
    const Ping two{2};
    tsuchi::emit(ping, &two);
    CHECK(joined() == "b2 c2 ");

    tsuchi::mod_stop(b); // its subscription ends with it
    CHECK(tsuchi::subscriber_count(ping) == 1);
    CHECK(listen(b, heard, name_b, &second) == TSUCHI_INVALID);
    CHECK(tsuchi::cancel(b, second) == TSUCHI_INVALID);
    const Ping three{3};
    tsuchi::emit(ping, &three);
    CHECK(joined() == "c3 ");
    tsuchi::mod_stop(a);
    CHECK(tsuchi::subscriber_count(ping) == 0);
}

// A subscriber that cancels itself and another, and adds a new one, while it runs.
struct Meddler {
    TSUCHI_Mod* mod;
    TSUCHI_Subscription self, other, added;
};
void meddle(void* user, const Ping* event) {
    auto* m = static_cast<Meddler*>(user);
    note("meddler" + std::to_string(event->value));
    CHECK(tsuchi::cancel(m->mod, m->self) == TSUCHI_OK);
    CHECK(tsuchi::cancel(m->mod, m->other) == TSUCHI_OK);
    static char late[] = "late";
    CHECK(listen(m->mod, heard, late, &m->added) == TSUCHI_OK);
}

void events_changed_while_delivered() {
    TSUCHI_Mod* mod = tsuchi::mod_create("events.meddler");
    Meddler meddler{mod, {}, {}, {}};
    char skipped[] = "skipped", kept[] = "kept";
    CHECK(listen(mod, meddle, &meddler, &meddler.self) == TSUCHI_OK);
    CHECK(listen(mod, heard, skipped, &meddler.other) == TSUCHI_OK);
    TSUCHI_Subscription last{};
    CHECK(listen(mod, heard, kept, &last) == TSUCHI_OK);
    const Ping one{1};
    tsuchi::emit(ping, &one);
    CHECK(joined() == "meddler1 kept1 "); // the canceled one is skipped; the new one waits
    CHECK(tsuchi::subscriber_count(ping) == 2);
    const Ping two{2};
    tsuchi::emit(ping, &two);
    CHECK(joined() == "kept2 late2 ");
    tsuchi::mod_stop(mod);
}

void many_subscriptions() {
    TSUCHI_Mod* mod = tsuchi::mod_create("events.many");
    static int calls;
    calls = 0;
    TSUCHI_Subscription last{};
    for (int i = 0; i < 5000; ++i)
        CHECK(listen(mod, [](void*, const Ping*) { ++calls; }, nullptr, &last) == TSUCHI_OK);
    const Ping one{1};
    tsuchi::emit(ping, &one);
    CHECK(calls == 5000 && tsuchi::owned_count(mod) == 5000);
    tsuchi::mod_stop(mod);
    tsuchi::emit(ping, &one);
    CHECK(calls == 5000 && tsuchi::subscriber_count(ping) == 0);
}

void ownership() {
    TSUCHI_Mod* a = tsuchi::mod_create("own.a");
    TSUCHI_Mod* b = tsuchi::mod_create("own.b");
    char by_a[] = "a:", by_b[] = "b:";
    CHECK(tsuchi::own(a, kActor, 1, released, by_a) == TSUCHI_OK);
    CHECK(tsuchi::own(a, kActor, 2, released, by_a) == TSUCHI_OK);
    CHECK(tsuchi::own(a, kActor, 2, released, by_a) == TSUCHI_OK); // not exclusive: a second record
    CHECK(tsuchi::owned_count(a) == 3);
    CHECK(tsuchi::disown(a, kActor, 2) == TSUCHI_OK && tsuchi::owned_count(a) == 2);
    CHECK(tsuchi::disown(a, kActor, 9) == TSUCHI_NOT_FOUND);
    CHECK(tsuchi::disown(b, kActor, 1) == TSUCHI_NOT_FOUND); // a's, not b's
    CHECK(joined().empty());                          // giving a thing up does not run its release

    // One owner at a time for an exclusive thing.
    CHECK(tsuchi::own(a, kCamera, 0, released, by_a, true) == TSUCHI_OK);
    CHECK(tsuchi::own(a, kCamera, 0, released, by_a, true) == TSUCHI_OK && tsuchi::owned_count(a) == 3);
    CHECK(tsuchi::own(b, kCamera, 0, released, by_b, true) == TSUCHI_BUSY);
    CHECK(tsuchi::owner(kCamera, 0) == a && tsuchi::owner(kCamera, 1) == nullptr);
    CHECK(tsuchi::own(b, kCamera, 1, released, by_b, true) == TSUCHI_OK); // another key is another thing

    // Stopping releases what is left, newest first, and frees it for others.
    tsuchi::mod_stop(a);
    CHECK(joined() == "a:0 a:2 a:1 ");
    CHECK(tsuchi::owner(kCamera, 0) == nullptr);
    CHECK(tsuchi::own(b, kCamera, 0, released, by_b, true) == TSUCHI_OK);
    CHECK(tsuchi::own(a, kActor, 5, released, by_a) == TSUCHI_INVALID); // a stopped mod owns nothing
    tsuchi::mod_stop(b);
    CHECK(joined() == "b:0 b:1 ");
}

// A release that reaches back into the core while its owner is stopping.
const tsuchi::Kind kNested{"nested"};
TSUCHI_Mod* stopping = nullptr;
void release_nested(void*, uint64_t key) {
    note("nested" + std::to_string(key));
    CHECK(!tsuchi::mod_usable(stopping));
    CHECK(tsuchi::own(stopping, kNested, 99, nullptr, nullptr) == TSUCHI_INVALID);
    CHECK(tsuchi::disown(stopping, kNested, 1) == TSUCHI_NOT_FOUND);
}

void release_during_stop() {
    stopping = tsuchi::mod_create("own.nested");
    CHECK(tsuchi::own(stopping, kNested, 1, release_nested, nullptr) == TSUCHI_OK);
    CHECK(tsuchi::own(stopping, kNested, 2, release_nested, nullptr) == TSUCHI_OK);
    tsuchi::mod_stop(stopping);
    CHECK(joined() == "nested2 nested1 ");
}

// A mod that takes a new thing before it gives up the old one, over and over, owns two things at
// most at a time, and its list takes no more room than that however long it goes on.
int rooms = 0;
void* counted_allocate(size_t bytes) {
    ++rooms;
    return std::malloc(bytes);
}

void records_given_up_make_room() {
    tsuchi::set_allocator({counted_allocate, std::free});
    TSUCHI_Mod* mod = tsuchi::mod_create("own.churn");
    char by[] = "c:";
    CHECK(tsuchi::own(mod, kCamera, 7, released, by) == TSUCHI_OK && tsuchi::own(mod, kActor, 0, released, by) == TSUCHI_OK);
    const int before = rooms;
    for (uint64_t key = 1; key <= 1000; ++key) {
        CHECK(tsuchi::own(mod, kActor, key, released, by) == TSUCHI_OK);
        CHECK(tsuchi::disown(mod, kActor, key - 1) == TSUCHI_OK);
    }
    CHECK(rooms == before && tsuchi::owned_count(mod) == 2);
    // What is left is still released newest first.
    CHECK(tsuchi::own(mod, kCamera, 8, released, by) == TSUCHI_OK);
    tsuchi::mod_stop(mod);
    CHECK(joined() == "c:8 c:1000 c:7 ");
    tsuchi::set_allocator({std::malloc, std::free});
}

// The core is the game thread's. A call that comes on another thread is refused before it touches anything.
uint64_t running_thread = 1;
bool reset_in_probe=false, renote_in_probe=false;
uint64_t thread_now() {
    uint64_t current=running_thread;
    if(reset_in_probe) { reset_in_probe=false; tsuchi::set_thread_probe(thread_now); }
    if(renote_in_probe) { renote_in_probe=false; running_thread=9; tsuchi::note_game_thread(); running_thread=current; }
    return current;
}

void the_games_thread() {
    TSUCHI_Mod* mod = tsuchi::mod_create("thread.own");
    char by[] = "t:";
    static char name[] = "heard";
    TSUCHI_Subscription first{}, second{};
    CHECK(tsuchi::own(mod, kActor, 1, released, by) == TSUCHI_OK && listen(mod, heard, name, &first) == TSUCHI_OK);
    // With no way to tell threads apart, and with one before any frame has shown which is the game's, every thread passes.
    CHECK(tsuchi::on_game_thread());
    CHECK(tsuchi::known_game_thread_token()==0);
    tsuchi::note_game_thread();
    tsuchi::set_thread_probe(thread_now);
    CHECK(tsuchi::known_game_thread_token()==0);
    running_thread = 2;
    CHECK(tsuchi::known_game_thread_token()==0);
    CHECK(tsuchi::on_game_thread() && tsuchi::mod_usable(mod) && tsuchi::off_thread_calls() == 0);
    running_thread = 1;
    tsuchi::note_game_thread();
    CHECK(tsuchi::known_game_thread_token()==1);
    CHECK(tsuchi::on_game_thread() && tsuchi::mod_usable(mod));

    running_thread = 2;
    CHECK(tsuchi::known_game_thread_token()==0);
    CHECK(!tsuchi::on_game_thread() && tsuchi::mod_running(mod) && !tsuchi::mod_usable(mod) && tsuchi::off_thread_calls() == 1);
    CHECK(tsuchi::own(mod, kActor, 2, released, by) == TSUCHI_INVALID && tsuchi::disown(mod, kActor, 1) == TSUCHI_NOT_FOUND);
    CHECK(listen(mod, heard, name, &second) == TSUCHI_INVALID && !second.id && tsuchi::cancel(mod, first) == TSUCHI_INVALID);
    const Ping lost{7}, kept{8};
    tsuchi::emit(ping, &lost); // reaches nobody: a subscriber's code runs on the game's thread only
    tsuchi::mod_stop(mod);        // and a mod is not stopped from there
    CHECK(joined() == "" && tsuchi::mod_running(mod) && tsuchi::off_thread_calls() >= 6);

    // Back on the game's thread everything is as it was left.
    running_thread = 1;
    CHECK(tsuchi::owned_count(mod) == 2 && tsuchi::subscriber_count(ping) == 1);
    tsuchi::emit(ping, &kept);
    CHECK(joined() == "heard8 ");
    // The game's thread is the one the last frame came on.
    running_thread = 3;
    CHECK(!tsuchi::mod_usable(mod));
    tsuchi::note_game_thread();
    CHECK(tsuchi::known_game_thread_token()==3);
    CHECK(tsuchi::mod_usable(mod) && tsuchi::cancel(mod, first) == TSUCHI_OK);
    tsuchi::mod_stop(mod);
    CHECK(joined() == "t:1 " && !tsuchi::mod_running(mod));
    reset_in_probe=true;
    CHECK(tsuchi::known_game_thread_token()==0);
    tsuchi::note_game_thread();
    renote_in_probe=true;
    CHECK(tsuchi::known_game_thread_token()==0);
    tsuchi::note_game_thread();
    CHECK(tsuchi::known_game_thread_token()==3);
    running_thread=0;
    tsuchi::note_game_thread();
    CHECK(tsuchi::known_game_thread_token()==0);
    tsuchi::set_thread_probe(nullptr);
    CHECK(tsuchi::known_game_thread_token()==0 && tsuchi::on_game_thread());
}

int pauses = 0;
void count_pause() { ++pauses; }

// A lock's waiter spins at first, then pauses each time it finds the lock still held; with no pause given, it spins.
void a_waiter_pauses() {
    uint32_t tries = 0;
    for (int i = 0; i < 63; ++i) tsuchi::lock_wait(tries);
    CHECK(tries == 63);
    tsuchi::set_thread_pause(count_pause);
    tries = 0;
    for (int i = 0; i < 63; ++i) tsuchi::lock_wait(tries);
    CHECK(pauses == 0);
    tsuchi::lock_wait(tries);
    tsuchi::lock_wait(tries);
    CHECK(pauses == 2);
    tsuchi::set_thread_pause(nullptr);
    tsuchi::lock_wait(tries);
    CHECK(pauses == 2);
}

} // namespace

int main() {
    the_games_thread();
    a_waiter_pauses();
    mods();
    events();
    events_changed_while_delivered();
    many_subscriptions();
    ownership();
    release_during_stop();
    records_given_up_make_room();
    if (failures) std::printf("%d failed\n", failures);
    return failures;
}
