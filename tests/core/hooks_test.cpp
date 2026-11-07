// Checks of hooks against a made-up game. The thunks here are only addresses:
// `call` follows them the way the real pieces of code would jump.
// Exits with the number of failures.
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>
#include <string>

#include "fake_game.h"
#include "hooks.h"
#include "host.h"
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

constexpr uint64_t kThunks = 0x7000000000000000ull;
uint32_t thunk_supply = 64;
constexpr unsigned update_site = fake::t::code_site::update;
constexpr unsigned draw_site = fake::t::code_site::draw_call;

std::string trace;
void call(uint64_t address);
void dispatch(uint32_t thunk) {
    uint64_t stack = 0;
    const auto selected = tsuchi_hook_enter(thunk, reinterpret_cast<uint64_t>(&stack), 123);
    CHECK(selected.target);
    if (selected.target) call(selected.target);
    if (selected.tracked) CHECK(tsuchi_hook_leave(reinterpret_cast<uint64_t>(&stack)) == 123);
}
void call(uint64_t address) {
    if (address >= kThunks && address < kThunks + 8ull * thunk_supply) {
        dispatch(uint32_t((address - kThunks) / 8));
        return;
    }
    // A code site's way back is in the patch block, which is not code here.
    for (unsigned i = 0; i < fake::t::code_site_count; ++i) {
        if (address != fake::main_image() + fake::t::code_sites[i].original) continue;
        // Keep route labels stable when unrelated named sites are added.
        trace += i == update_site ? "game0 " : i == draw_site ? "game1 " : "game" + std::to_string(i) + " ";
        return;
    }
    reinterpret_cast<void (*)()>(address)();
}
void call(void* address) { call(reinterpret_cast<uint64_t>(address)); }

uint64_t entry = 0; // the game's table entry under test
void game_calls_entry() {
    trace.clear();
    call(fake::get<uint64_t>(entry));
}

void* original_a = nullptr;
void* original_b = nullptr;
void* original_c = nullptr;
void stock() { trace += "stock "; }
void other() { trace += "other "; }
void hook_a() { trace += "a ", call(original_a); }
void hook_b() { trace += "b ", call(original_b); }
void hook_swallow() { trace += "swallow "; }
void hook_c() { trace += "c ", call(original_c); }

uint64_t address_of(void (*function)()) { return reinterpret_cast<uint64_t>(function); }
void* pointer_to(void (*function)()) { return reinterpret_cast<void*>(function); }

// A fresh table entry at an address no earlier test used: the framework
// remembers every entry it has wrapped, as it would for the life of the game.
void new_entry(void (*holds)()) {
    fake::start();
    fake::code().push_back(address_of(stock));
    fake::code().push_back(address_of(other));
    entry = reinterpret_cast<uint64_t>(new uint64_t[2]());
    fake::ranges().push_back({entry, entry + 16, true});
    fake::put<uint64_t>(entry, address_of(holds));
}

void chains() {
    new_entry(stock);
    TSUCHI_Mod* a = tsuchi::mod_create("hooks.a");
    TSUCHI_Mod* b = tsuchi::mod_create("hooks.b");
    uint64_t id_a = 0, id_b = 0;
    game_calls_entry();
    CHECK(trace == "stock ");

    CHECK(tsuchi::hook_wrap(a, entry, address_of(stock), pointer_to(hook_a), 0, &id_a, &original_a) == TSUCHI_OK);
    CHECK(id_a && original_a && tsuchi::hook_count(entry) == 1 && tsuchi::hook_entry_was_stock(entry));
    CHECK(tsuchi::hook_entry_owned(entry));
    const uint64_t tracked_entry = fake::get<uint64_t>(entry);
    fake::put<uint64_t>(entry, address_of(other));
    CHECK(!tsuchi::hook_entry_owned(entry));
    fake::put<uint64_t>(entry, tracked_entry);
    CHECK(tsuchi::hook_entry_owned(entry));
    CHECK(fake::get<uint64_t>(entry) >= kThunks); // the entry now goes through the framework
    game_calls_entry();
    CHECK(trace == "a stock ");

    CHECK(tsuchi::hook_wrap(b, entry, address_of(stock), pointer_to(hook_b), 0, &id_b, &original_b) == TSUCHI_OK);
    CHECK(id_b != id_a && tsuchi::hook_count(entry) == 2);
    game_calls_entry();
    CHECK(trace == "b a stock "); // the newest runs first

    // The inner hook goes while the outer one stays.
    CHECK(tsuchi::hook_remove(b, id_a) == TSUCHI_NOT_FOUND); // a's, not b's
    CHECK(tsuchi::hook_remove(a, id_a) == TSUCHI_OK && tsuchi::hook_remove(a, id_a) == TSUCHI_NOT_FOUND);
    game_calls_entry();
    CHECK(trace == "b stock " && tsuchi::hook_count(entry) == 1);
    // Its original expires after its running calls return.
    CHECK(tsuchi_hook_target(uint32_t((reinterpret_cast<uint64_t>(original_a) - kThunks) / 8)) == 0);

    // A mod that stops takes its hooks with it.
    tsuchi::mod_stop(b);
    game_calls_entry();
    CHECK(trace == "stock " && tsuchi::hook_count(entry) == 0);

    // The entry can be hooked again afterwards.
    CHECK(tsuchi::hook_wrap(a, entry, address_of(stock), pointer_to(hook_a), 0, &id_a, &original_a) == TSUCHI_OK);
    game_calls_entry();
    CHECK(trace == "a stock ");
    tsuchi::mod_stop(a);
}

// A hook taken with kHookFirst stays outermost: mods' hooks added after it run inside it, newest first.
void a_first_hook_stays_outermost() {
    new_entry(stock);
    TSUCHI_Mod* framework = tsuchi::mod_create("hooks.first");
    TSUCHI_Mod* a = tsuchi::mod_create("hooks.later-a");
    TSUCHI_Mod* b = tsuchi::mod_create("hooks.later-b");
    uint64_t id_first = 0, id_a = 0, id_b = 0;
    CHECK(tsuchi::hook_wrap(framework, entry, address_of(stock), pointer_to(hook_c), tsuchi::kHookFirst, &id_first, &original_c) == TSUCHI_OK);
    CHECK(tsuchi::hook_wrap(a, entry, address_of(stock), pointer_to(hook_a), 0, &id_a, &original_a) == TSUCHI_OK);
    game_calls_entry();
    CHECK(trace == "c a stock ");
    CHECK(tsuchi::hook_wrap(b, entry, address_of(stock), pointer_to(hook_b), 0, &id_b, &original_b) == TSUCHI_OK);
    game_calls_entry();
    CHECK(trace == "c b a stock ");
    // Removing the first one leaves the others in their order; removing one inside it keeps it first.
    CHECK(tsuchi::hook_remove(a, id_a) == TSUCHI_OK);
    game_calls_entry();
    CHECK(trace == "c b stock ");
    tsuchi::mod_stop(framework);
    game_calls_entry();
    CHECK(trace == "b stock ");
    tsuchi::mod_stop(a), tsuchi::mod_stop(b);
}

void a_hook_may_not_continue() {
    new_entry(stock);
    TSUCHI_Mod* a = tsuchi::mod_create("hooks.inner");
    TSUCHI_Mod* b = tsuchi::mod_create("hooks.swallow");
    uint64_t id = 0;
    CHECK(tsuchi::hook_wrap(a, entry, 0, pointer_to(hook_a), 0, &id, &original_a) == TSUCHI_OK);
    CHECK(tsuchi::hook_wrap(b, entry, 0, pointer_to(hook_swallow), 0, &id, &original_c) == TSUCHI_OK);
    game_calls_entry();
    CHECK(trace == "swallow ");
    tsuchi::mod_stop(b);
    game_calls_entry();
    CHECK(trace == "a stock ");
    tsuchi::mod_stop(a);
}

// Removing itself while it runs, then continuing.
uint64_t leaving_id = 0;
TSUCHI_Mod* leaving_mod = nullptr;
void* leaving_original = nullptr;
void* replacement_original = nullptr;
uint64_t replacement_id = 0;
void hook_leaves() {
    trace += "leaving ";
    CHECK(tsuchi::hook_remove(leaving_mod, leaving_id) == TSUCHI_OK);
    CHECK(tsuchi::hook_wrap(leaving_mod, entry, 0, pointer_to(hook_swallow), 0, &replacement_id, &replacement_original) == TSUCHI_OK);
    CHECK(replacement_original != leaving_original); // still in flight, so it cannot be reused
    call(fake::get<uint64_t>(entry)); // reentrant entry reaches the replacement
    call(leaving_original); // this call's continuation still reaches the old base
}

void a_hook_can_remove_itself() {
    new_entry(stock);
    leaving_mod = tsuchi::mod_create("hooks.leaving");
    CHECK(tsuchi::hook_wrap(leaving_mod, entry, 0, pointer_to(hook_leaves), 0, &leaving_id, &leaving_original) == TSUCHI_OK);
    TSUCHI_Mod* outer = tsuchi::mod_create("hooks.leaving-outer");
    uint64_t outer_id = 0;
    CHECK(tsuchi::hook_wrap(outer, entry, 0, pointer_to(hook_b), 0, &outer_id, &original_b) == TSUCHI_OK);
    game_calls_entry();
    CHECK(trace == "b leaving swallow stock ");
    CHECK(tsuchi::hook_remove(leaving_mod, replacement_id) == TSUCHI_OK);
    // Reusing the removed inner slot must not reconnect the outer continuation.
    CHECK(tsuchi::hook_wrap(leaving_mod, entry, 0, pointer_to(hook_swallow), 0, &replacement_id, &replacement_original) == TSUCHI_OK);
    trace.clear();
    call(original_b);
    CHECK(trace == "stock ");
    tsuchi::mod_stop(leaving_mod);
    game_calls_entry();
    CHECK(trace == "b stock ");
    tsuchi::mod_stop(outer);
}

void entries_that_are_not_stock() {
    // Something else changed the entry before the framework saw it.
    new_entry(other);
    TSUCHI_Mod* mod = tsuchi::mod_create("hooks.late");
    uint64_t id = 0;
    CHECK(tsuchi::hook_wrap(mod, entry, address_of(stock), pointer_to(hook_a), tsuchi::kHookNeedsStock, &id, &original_a) == TSUCHI_BUSY);
    CHECK(fake::get<uint64_t>(entry) == address_of(other)); // refused: nothing changed
    CHECK(tsuchi::hook_wrap(mod, entry, address_of(stock), pointer_to(hook_a), 0, &id, &original_a) == TSUCHI_OK);
    CHECK(!tsuchi::hook_entry_was_stock(entry));
    game_calls_entry();
    CHECK(trace == "a other "); // what was there is kept in the chain
    CHECK(tsuchi::hook_wrap(mod, entry, address_of(stock), pointer_to(hook_b), tsuchi::kHookNeedsStock, &id, &original_b) == TSUCHI_BUSY);
    tsuchi::mod_stop(mod);
}

// Something wraps the entry after the framework has, the way another module would.
uint64_t under_wrapper = 0;
void wrapper() { trace += "wrapper ", call(under_wrapper); }

void something_wraps_on_top() {
    new_entry(stock);
    fake::code().push_back(address_of(wrapper));
    TSUCHI_Mod* a = tsuchi::mod_create("hooks.under-a");
    TSUCHI_Mod* b = tsuchi::mod_create("hooks.under-b");
    uint64_t id_a = 0, id_b = 0;
    CHECK(tsuchi::hook_wrap(a, entry, address_of(stock), pointer_to(hook_a), 0, &id_a, &original_a) == TSUCHI_OK);
    under_wrapper = fake::get<uint64_t>(entry);
    fake::put<uint64_t>(entry, address_of(wrapper));
    game_calls_entry();
    CHECK(trace == "wrapper a stock ");
    // Hooks still come and go underneath it.
    CHECK(tsuchi::hook_wrap(b, entry, address_of(stock), pointer_to(hook_b), 0, &id_b, &original_b) == TSUCHI_OK);
    CHECK(fake::get<uint64_t>(entry) == address_of(wrapper)); // its entry is left alone
    game_calls_entry();
    CHECK(trace == "wrapper b a stock ");
    tsuchi::mod_stop(a);
    tsuchi::mod_stop(b);
    game_calls_entry();
    CHECK(trace == "wrapper stock ");
}

void entries_that_cannot_be_hooked() {
    new_entry(stock);
    TSUCHI_Mod* mod = tsuchi::mod_create("hooks.refused");
    uint64_t id = 0;
    void* original = nullptr;
    CHECK(tsuchi::hook_wrap(nullptr, entry, 0, pointer_to(hook_a), 0, &id, &original) == TSUCHI_INVALID);
    CHECK(tsuchi::hook_wrap(mod, entry, 0, nullptr, 0, &id, &original) == TSUCHI_INVALID);
    CHECK(tsuchi::hook_wrap(mod, entry, 0, pointer_to(hook_a), 0, nullptr, &original) == TSUCHI_INVALID);
    CHECK(tsuchi::hook_wrap(mod, 8, 0, pointer_to(hook_a), 0, &id, &original) == TSUCHI_NOT_READY); // not memory
    fake::put<uint64_t>(entry + 8, 0x1234);                                                        // holds no function
    CHECK(tsuchi::hook_wrap(mod, entry + 8, 0, pointer_to(hook_a), 0, &id, &original) == TSUCHI_NOT_READY);
    fake::protect(entry, false);
    CHECK(tsuchi::hook_wrap(mod, entry, 0, pointer_to(hook_a), 0, &id, &original) == TSUCHI_NOT_READY); // read-only
    CHECK(fake::get<uint64_t>(entry) == address_of(stock) && tsuchi::hook_count(entry) == 0);
    const uint64_t unaligned = fake::memory(16) + 1;
    fake::put<uint64_t>(unaligned, address_of(stock));
    CHECK(tsuchi::hook_wrap(mod, unaligned, 0, pointer_to(hook_a), 0, &id, &original) == TSUCHI_NOT_READY);
    CHECK(fake::get<uint64_t>(unaligned) == address_of(stock)); // no partial pointer publication
    tsuchi::mod_stop(mod);
    CHECK(tsuchi::hook_wrap(mod, entry, 0, pointer_to(hook_a), 0, &id, &original) == TSUCHI_INVALID); // a stopped mod
}

void* no_memory(size_t) { return nullptr; }
void allocation_failure_does_not_publish() {
    TSUCHI_Mod* mod = tsuchi::mod_create("hooks.no-memory");
    tsuchi::set_allocator({no_memory, std::free});
    // Failure on fresh entries must not consume the permanent entry supply.
    for (unsigned i = 0; i < 2 * thunk_supply; ++i) {
        new_entry(stock);
        uint64_t id = 0;
        void* original = nullptr;
        CHECK(tsuchi::hook_wrap(mod, entry, 0, pointer_to(hook_swallow), 0, &id, &original) == TSUCHI_NO_MEMORY);
        CHECK(!id && !original && !tsuchi::owned_count(mod) && !tsuchi::hook_count(entry));
        CHECK(fake::get<uint64_t>(entry) == address_of(stock));
    }
    tsuchi::set_allocator({std::malloc, std::free});
    uint64_t id = 0;
    void* original = nullptr;
    CHECK(tsuchi::hook_wrap(mod, entry, 0, pointer_to(hook_swallow), 0, &id, &original) == TSUCHI_OK);
    tsuchi::mod_stop(mod);
}

// The game reaching a code site: the patch's block asks the framework where the site goes.
void game_reaches_site(uint32_t site) {
    trace.clear();
    dispatch(tsuchi::kCodeSiteThunk | site);
}

// Capacity is live hooks plus permanently wrapped entries, not lifetime additions.
void active_capacity_and_reuse() {
    new_entry(stock);
    TSUCHI_Mod* mod = tsuchi::mod_create("hooks.many");
    std::vector<uint64_t> ids;
    uint64_t id = 0;
    void* original = nullptr;
    while (tsuchi::hook_wrap(mod, entry, 0, pointer_to(hook_swallow), 0, &id, &original) == TSUCHI_OK) ids.push_back(id);
    CHECK(!ids.empty() && ids.size() < thunk_supply);
    id = 0, original = nullptr;
    CHECK(tsuchi::hook_wrap(mod, entry, 0, pointer_to(hook_swallow), 0, &id, &original) == TSUCHI_CAPACITY && !id && !original);
    CHECK(tsuchi::owned_count(mod) == ids.size() && tsuchi::hook_count(entry) == ids.size());
    const uint64_t first = entry;
    new_entry(stock);
    CHECK(tsuchi::hook_wrap(mod, entry, 0, pointer_to(hook_swallow), 0, &id, &original) == TSUCHI_CAPACITY);
    CHECK(fake::get<uint64_t>(entry) == address_of(stock) && !tsuchi::hook_count(entry));
    fake::patch_mark();
    fake::patch_site(update_site);
    CHECK(tsuchi::hook_code_site(mod, update_site, pointer_to(hook_swallow), &id, &original) == TSUCHI_CAPACITY);
    game_reaches_site(update_site);
    CHECK(trace == "game0 ");

    // Free one original at full capacity, then use it on a code site.
    CHECK(tsuchi::hook_remove(mod, ids.back()) == TSUCHI_OK);
    ids.pop_back();
    CHECK(tsuchi::hook_code_site(mod, update_site, pointer_to(hook_swallow), &id, &original) == TSUCHI_OK);
    CHECK(tsuchi::hook_remove(mod, id) == TSUCHI_OK);
    for (uint64_t held : ids) CHECK(tsuchi::hook_remove(mod, held) == TSUCHI_OK);
    CHECK(!tsuchi::owned_count(mod));
    for (unsigned i = 0; i < 4096; ++i) {
        CHECK(tsuchi::hook_wrap(mod, first, 0, pointer_to(hook_swallow), 0, &id, &original) == TSUCHI_OK);
        CHECK(tsuchi::hook_remove(mod, id) == TSUCHI_OK);
    }
    trace.clear();
    call(fake::get<uint64_t>(first));
    CHECK(trace == "stock ");

    // A build supplying more thunks still cannot exceed the fixed tables.
    thunk_supply = tsuchi::kHookThunks + 64;
    ids.clear();
    uint64_t highest = 0;
    while (tsuchi::hook_wrap(mod, first, 0, pointer_to(hook_swallow), 0, &id, &original) == TSUCHI_OK) {
        ids.push_back(id);
        if (reinterpret_cast<uint64_t>(original) > highest) highest = reinterpret_cast<uint64_t>(original);
    }
    CHECK(ids.size() > 900 && highest == kThunks + 8ull * (tsuchi::kHookThunks - 1));
    CHECK(tsuchi::hook_count(first) == ids.size() && tsuchi::owned_count(mod) == ids.size());
    CHECK(tsuchi_hook_target(tsuchi::kHookThunks) == 0 && tsuchi_hook_target(0x7FFFFFFF) == 0);
    tsuchi::mod_stop(mod);
    CHECK(!tsuchi::hook_count(first));
    TSUCHI_Mod* late = tsuchi::mod_create("hooks.late-comer");
    CHECK(tsuchi::hook_wrap(late, first, 0, pointer_to(hook_swallow), 0, &id, &original) == TSUCHI_OK);
    CHECK(tsuchi::hook_remove(late, id) == TSUCHI_OK);
    tsuchi::mod_stop(late);
}

// Which thread the made-up game is on.
uint64_t running_thread = 1;
uint64_t thread_now() { return running_thread; }

// A hook that asks the framework for something from wherever the game called it.
TSUCHI_Mod* asking_mod = nullptr;
void* asking_original = nullptr;
TSUCHI_Result asked = TSUCHI_OK;
void hook_asks() {
    uint64_t id = 0;
    void* original = nullptr;
    trace += "asks ";
    asked = tsuchi::hook_wrap(asking_mod, entry, 0, pointer_to(hook_swallow), 0, &id, &original);
    call(asking_original);
}

// Hooks are put on and taken off on the game's thread. The game may call a hooked function on any thread.
void calls_from_another_thread() {
    new_entry(stock);
    tsuchi::hooks_start();
    TSUCHI_Mod* a = tsuchi::mod_create("threads.a");
    uint64_t id_a = 0, id_b = 0;
    CHECK(tsuchi::hook_wrap(a, entry, address_of(stock), pointer_to(hook_a), 0, &id_a, &original_a) == TSUCHI_OK);
    tsuchi::set_thread_probe(thread_now);
    running_thread = 2;
    CHECK(tsuchi::on_game_thread() && tsuchi::mod_usable(a)); // no frame has shown which thread is the game's
    running_thread = 1;
    tsuchi::note_game_thread();

    // On another thread the game's call through a hooked entry goes where it would on its own.
    running_thread = 2;
    const uint32_t refused = tsuchi::off_thread_calls();
    game_calls_entry();
    CHECK(trace == "a stock " && tsuchi::off_thread_calls() == refused);
    // What is asked of the framework there is refused, and nothing changes.
    TSUCHI_Hook hook{};
    void* original_d = nullptr;
    CHECK(tsuchi::hook_wrap(a, entry, address_of(stock), pointer_to(hook_b), 0, &id_b, &original_d) == TSUCHI_INVALID && !id_b && !original_d);
    CHECK(tsuchi::host.hook->wrap_slot(a, "actor_end_play", pointer_to(hook_b), &hook) == TSUCHI_INVALID && !hook.id.id);
    CHECK(tsuchi::host.hook->wrap_address(a, entry - fake::main_image(), 0, pointer_to(hook_b), &hook) == TSUCHI_INVALID && !hook.id.id);
    CHECK(tsuchi::hook_code_site(a, update_site, pointer_to(hook_b), &id_b, &original_d) == TSUCHI_INVALID && !tsuchi::mod_block_reason(a));
    CHECK(tsuchi::hook_remove(a, id_a) == TSUCHI_INVALID && tsuchi::host.hook->remove(a, TSUCHI_HookId{id_a}) == TSUCHI_INVALID);
    tsuchi::mod_stop(a); // a mod is not stopped from there either
    CHECK(tsuchi::off_thread_calls() == refused + 7 && tsuchi::hook_count(entry) == 1);
    game_calls_entry();
    CHECK(trace == "a stock ");

    // A hook the game calls there runs, and what it asks for is refused like anything else.
    running_thread = 1;
    CHECK(tsuchi::owned_count(a) == 1);
    asking_mod = tsuchi::mod_create("threads.asking");
    uint64_t id = 0;
    CHECK(tsuchi::hook_wrap(asking_mod, entry, 0, pointer_to(hook_asks), 0, &id, &asking_original) == TSUCHI_OK);
    running_thread = 2;
    game_calls_entry();
    CHECK(trace == "asks a stock " && asked == TSUCHI_INVALID && tsuchi::hook_count(entry) == 2);
    running_thread = 1;
    game_calls_entry();
    CHECK(trace == "asks a stock " && asked == TSUCHI_OK && tsuchi::hook_count(entry) == 3);
    game_calls_entry();
    CHECK(trace == "swallow ");
    tsuchi::mod_stop(asking_mod);
    CHECK(tsuchi::hook_remove(a, id_a) == TSUCHI_OK);
    game_calls_entry();
    CHECK(trace == "stock ");
    tsuchi::mod_stop(a);
    tsuchi::set_thread_probe(nullptr);
}

void the_public_service() {
    new_entry(stock);
    tsuchi::hooks_start();
    TSUCHI_Mod* mod = tsuchi::mod_create("hooks.public");
    // The made-up game's table names one entry, at 0x3000 + 0x48 from the start of its code.
    const uint64_t named = fake::main_image() + 0x3000 + 0x48;
    fake::code().push_back(fake::main_image() + 0x1234);
    fake::put<uint64_t>(named, fake::main_image() + 0x1234);
    TSUCHI_Hook hook{};
    CHECK(tsuchi::host.hook->wrap_slot(mod, "no_such_entry", pointer_to(hook_a), &hook) == TSUCHI_NOT_FOUND);
    CHECK(tsuchi::host.hook->wrap_slot(mod, nullptr, pointer_to(hook_a), &hook) == TSUCHI_INVALID);
    CHECK(tsuchi::host.hook->wrap_slot(mod, "actor_end_play", nullptr, &hook) == TSUCHI_INVALID);
    CHECK(tsuchi::host.hook->wrap_slot(mod, "actor_end_play", pointer_to(hook_a), &hook) == TSUCHI_OK);
    CHECK(hook.id.id && hook.original && tsuchi::hook_entry_was_stock(named) && tsuchi::hook_count(named) == 1);
    CHECK(tsuchi_hook_target(uint32_t((fake::get<uint64_t>(named) - kThunks) / 8)) == address_of(hook_a));
    CHECK(tsuchi::host.hook->remove(mod, hook.id) == TSUCHI_OK && tsuchi::host.hook->remove(mod, hook.id) == TSUCHI_NOT_FOUND);
    CHECK(tsuchi::hook_count(named) == 0);

    TSUCHI_Hook by_address{};
    const uint64_t offset = entry - fake::main_image(); // counted from the start of the game's code, as a mod gives it
    fake::code().push_back(address_of(stock));
    CHECK(tsuchi::host.hook->wrap_address(mod, offset, 0, pointer_to(hook_a), &by_address) == TSUCHI_OK);
    original_a = by_address.original;
    game_calls_entry();
    CHECK(trace == "a stock ");
    tsuchi::mod_stop(mod);
}

void code_sites() {
    new_entry(stock);
    tsuchi::hooks_start();
    namespace t = fake::t;
    const uint64_t main = fake::main_image();
    TSUCHI_Mod* a = tsuchi::mod_create("sites.a");
    TSUCHI_Mod* b = tsuchi::mod_create("sites.b");
    uint64_t id_a = 0, id_b = 0;

    const auto every_site = [](bool patched) {
        for (unsigned site = 0; site < t::code_site_count; ++site) fake::patch_site(site, patched);
    };
    const auto refused = [&](const char* id, const char* why) {
        TSUCHI_Mod* mod = tsuchi::mod_create(id);
        uint64_t hook_id = 0;
        void* original = nullptr;
        TSUCHI_Hook hook{};
        const bool by_number = tsuchi::hook_code_site(mod, update_site, pointer_to(hook_a), &hook_id, &original) == TSUCHI_NOT_READY && !hook_id && !original;
        const bool by_name = tsuchi::host.hook->wrap_site(mod, "draw_call", pointer_to(hook_a), &hook) == TSUCHI_NOT_READY && !hook.id.id;
        const char* const noted = tsuchi::mod_block_reason(mod);
        const bool told = noted && std::string(noted) == std::string("it hooks the game's code, and ") + why && std::string(tsuchi::patch_problem()) == why;
        if (!told) std::printf("  noted: [%s]\n", noted ? noted : "<nothing>");
        const bool nothing_made = tsuchi::owned_count(mod) == 0;
        tsuchi::mod_stop(mod);
        return by_number && by_name && told && nothing_made;
    };

    // Without the patch the game's code is as it shipped. A site cannot be hooked, and the mod that asked is
    // noted as one that cannot run, with the reason.
    every_site(false);
    fake::no_patch_mark();
    CHECK(tsuchi::patch_found() == tsuchi::Patch::missing); // before anything has looked
    CHECK(tsuchi::find_patch() == tsuchi::Patch::missing && tsuchi::patch_found() == tsuchi::Patch::missing && !tsuchi::code_site_patched(update_site));
    CHECK(refused("sites.early", "the framework's patch is not installed"));
    game_reaches_site(update_site);
    CHECK(trace == "game0 "); // and the block sends the game straight back
    // A mod that hooks table entries only is none the worse for it.
    CHECK(tsuchi::hook_wrap(a, entry, address_of(stock), pointer_to(hook_a), 0, &id_a, &original_a) == TSUCHI_OK && !tsuchi::mod_block_reason(a));
    CHECK(tsuchi::hook_remove(a, id_a) == TSUCHI_OK);

    // A patch made from another table: its block says so, though every site jumps where this build's would.
    every_site(true);
    fake::patch_mark(t::stub::patch_hash ^ 1);
    CHECK(tsuchi::find_patch() == tsuchi::Patch::other && !tsuchi::code_site_patched(update_site) && !tsuchi::code_site_patched(draw_site));
    CHECK(refused("sites.other", "the framework's patch is from another version"));
    // A patch from before blocks said what they were made from: nothing at the start of the block, and the
    // game's code changed at a site. One site is enough.
    fake::no_patch_mark();
    CHECK(tsuchi::find_patch() == tsuchi::Patch::other);
    every_site(false);
    fake::patch_site(t::code_site_count - 1);
    CHECK(tsuchi::find_patch() == tsuchi::Patch::other && refused("sites.old", "the framework's patch is from another version"));
    // Something else in the block's place is no patch.
    every_site(false);
    fake::put<uint64_t>(main + t::stub::block, t::stub::patch_hash);
    fake::put<uint64_t>(main + t::stub::block + 8, t::stub::patch_hash);
    CHECK(tsuchi::find_patch() == tsuchi::Patch::missing);
    // And a game whose code cannot be read has none.
    fake::patch_mark();
    const uint64_t image = fake::main_image();
    fake::main_image() = 0;
    CHECK(tsuchi::find_patch() == tsuchi::Patch::missing);
    fake::main_image() = image;

    // The patch this build was made with. What was found at start-up is kept: the framework has told the block
    // by then whether to come in, and does not change its mind.
    CHECK(tsuchi::patch_found() == tsuchi::Patch::missing && refused("sites.late", "the framework's patch is not installed"));
    CHECK(tsuchi::find_patch() == tsuchi::Patch::right && tsuchi::patch_found() == tsuchi::Patch::right && !tsuchi::patch_problem());

    // The patch puts a jump to the site's entry there; for a call, a call.
    CHECK(!tsuchi::code_site_patched(update_site));
    fake::patch_site(update_site), fake::patch_site(draw_site);
    CHECK(fake::get<uint32_t>(main + t::code_sites[update_site].address) == (0x14000000u | uint32_t((t::code_sites[update_site].entry - t::code_sites[update_site].address) >> 2)));
    CHECK(fake::get<uint32_t>(main + t::code_sites[draw_site].address) == (0x94000000u | uint32_t((t::code_sites[draw_site].entry - t::code_sites[draw_site].address) >> 2)));
    CHECK(tsuchi::code_site_patched(update_site) && tsuchi::code_site_patched(draw_site) && !tsuchi::code_site_patched(t::code_site::actor_pool_take.number));
    CHECK(t::code_sites[draw_site].call && !t::code_sites[update_site].call);

    CHECK(tsuchi::hook_code_site(a, update_site, pointer_to(hook_a), &id_a, &original_a) == TSUCHI_OK);
    game_reaches_site(update_site);
    CHECK(trace == "a game0 ");
    CHECK(tsuchi::hook_code_site(b, update_site, pointer_to(hook_b), &id_b, &original_b) == TSUCHI_OK);
    game_reaches_site(update_site);
    CHECK(trace == "b a game0 ");
    game_reaches_site(draw_site);
    CHECK(trace == "game1 "); // another site is another chain
    CHECK(tsuchi::hook_remove(a, id_a) == TSUCHI_OK);
    game_reaches_site(update_site);
    CHECK(trace == "b game0 ");
    tsuchi::mod_stop(b);
    game_reaches_site(update_site);
    CHECK(trace == "game0 ");
    CHECK(tsuchi::hook_code_site(a, t::code_site_count, pointer_to(hook_a), &id_a, &original_a) == TSUCHI_INVALID); // no such site

    // By name, through the public service.
    TSUCHI_Hook hook{};
    CHECK(tsuchi::host.hook->wrap_site(a, "no_such_place", pointer_to(hook_a), &hook) == TSUCHI_NOT_FOUND);
    CHECK(tsuchi::host.hook->wrap_site(a, "draw_call", pointer_to(hook_a), &hook) == TSUCHI_OK);
    original_a = hook.original;
    game_reaches_site(draw_site);
    CHECK(trace == "a game1 ");
    CHECK(!tsuchi::mod_block_reason(a) && !tsuchi::mod_block_reason(b)); // nothing was refused them
    tsuchi::mod_stop(a);
    game_reaches_site(draw_site);
    CHECK(trace == "game1 ");
}

// Concurrent hooks retain their own continuation until every selected call returns.
std::atomic<unsigned> waiting{0}, completed{0};
std::atomic<bool> go{false};
void* overlapping_original = nullptr;
void overlap_base() { completed.fetch_add(1); }
void overlap_hook() {
    waiting.fetch_add(1);
    while (!go.load()) std::this_thread::yield();
    call(overlapping_original);
}
void overlapping_calls() {
    new_entry(overlap_base);
    fake::code().push_back(address_of(overlap_base));
    TSUCHI_Mod* mod = tsuchi::mod_create("hooks.overlap");
    uint64_t id = 0;
    CHECK(tsuchi::hook_wrap(mod, entry, 0, pointer_to(overlap_hook), 0, &id, &overlapping_original) == TSUCHI_OK);
    const uint64_t entry_thunk = fake::get<uint64_t>(entry);
    std::thread one([&] { call(entry_thunk); });
    std::thread two([&] { call(entry_thunk); });
    while (waiting.load() != 2) std::this_thread::yield();
    CHECK(tsuchi::hook_remove(mod, id) == TSUCHI_OK);
    for (unsigned i = 0; i < 2048; ++i) {
        void* next = nullptr;
        CHECK(tsuchi::hook_wrap(mod, entry, 0, pointer_to(hook_swallow), 0, &id, &next) == TSUCHI_OK);
        CHECK(next != overlapping_original);
        CHECK(tsuchi::hook_remove(mod, id) == TSUCHI_OK);
    }
    go.store(true);
    one.join();
    two.join();
    CHECK(completed.load() == 2);
    // Once both callers return, its original can be reused.
    void* next = nullptr;
    CHECK(tsuchi::hook_wrap(mod, entry, 0, pointer_to(hook_swallow), 0, &id, &next) == TSUCHI_OK);
    CHECK(next == overlapping_original);
    tsuchi::mod_stop(mod);
}

thread_local uint64_t provenance_thread = 1;
uint64_t thread_identity() { return provenance_thread; }
void invocation_provenance() {
    new_entry(stock);
    tsuchi::set_thread_probe(thread_identity);
    tsuchi::note_game_thread();
    TSUCHI_Mod* mod = tsuchi::mod_create("hooks.provenance");
    uint64_t inner = 0, outer = 0, other_id = 0;
    void* inner_next = nullptr;
    void* outer_next = nullptr;
    CHECK(tsuchi::hook_wrap(mod, entry, 0, pointer_to(hook_swallow), 0, &inner, &inner_next) == TSUCHI_OK);
    CHECK(tsuchi::hook_wrap(mod, entry, 0, pointer_to(hook_swallow), 0, &outer, &outer_next) == TSUCHI_OK);
    const uint32_t entry_thunk = uint32_t((fake::get<uint64_t>(entry) - kThunks) / 8);
    const uint32_t continuation = uint32_t((reinterpret_cast<uint64_t>(outer_next) - kThunks) / 8);
    CHECK(!tsuchi::hook_invocation(inner).token);
    CHECK(tsuchi_hook_enter(entry_thunk, 1000, 0x05BA9728).tracked);
    const auto original = tsuchi::hook_invocation(outer);
    CHECK(original.caller == 0x05BA9728 && original.thread == 1 && original.token);
    // A normal call to the next wrapper uses a different stack/LR; it inherits
    // the original native caller from its tracked owning invocation.
    CHECK(tsuchi_hook_enter(continuation, 900, 777).tracked);
    CHECK(tsuchi::hook_invocation(inner).caller == original.caller);
    CHECK(tsuchi_hook_leave(900) == 777);
    // Recursing through the entry is a new invocation, never an inherited one.
    CHECK(tsuchi_hook_enter(entry_thunk, 800, 0x1234).tracked);
    CHECK(tsuchi_hook_enter(continuation, 700, 888).tracked);
    CHECK(tsuchi::hook_invocation(inner).caller == 0x1234);
    CHECK(tsuchi_hook_leave(700) == 888 && tsuchi_hook_leave(800) == 0x1234);
    CHECK(tsuchi::hook_invocation(outer).token == original.token);
    // Different-site nesting does not contaminate the first site's provenance.
    const uint64_t first_entry = entry;
    new_entry(stock);
    void* other_next = nullptr;
    CHECK(tsuchi::hook_wrap(mod, entry, 0, pointer_to(hook_swallow), 0, &other_id, &other_next) == TSUCHI_OK);
    const uint32_t different = uint32_t((fake::get<uint64_t>(entry) - kThunks) / 8);
    CHECK(tsuchi_hook_enter(different, 800, 0x4567).tracked);
    CHECK(tsuchi::hook_invocation(other_id).caller == 0x4567);
    CHECK(tsuchi::hook_invocation(outer).caller == original.caller);
    CHECK(tsuchi_hook_leave(800) == 0x4567);
    provenance_thread = 2;
    CHECK(!tsuchi::hook_invocation(outer).token);
    CHECK(!tsuchi_hook_leave(1000)); // another thread cannot unwind it
    CHECK(tsuchi_hook_enter(continuation, 900, 999).tracked);
    CHECK(!tsuchi::hook_invocation(inner).caller); // no owning frame on this thread
    CHECK(tsuchi_hook_leave(900) == 999);
    provenance_thread = 1;
    CHECK(tsuchi_hook_leave(1000) == 0x05BA9728);
    CHECK(!tsuchi::hook_invocation(outer).token);
    CHECK(tsuchi_hook_enter(continuation, 900, 123).tracked);
    CHECK(!tsuchi::hook_invocation(inner).caller); // unpaired original fails closed
    CHECK(tsuchi_hook_leave(900) == 123);
    CHECK(tsuchi_hook_target(uint32_t((fake::get<uint64_t>(first_entry) - kThunks) / 8)));
    CHECK(!tsuchi::hook_invocation(outer).token); // untracked selection creates no evidence
    tsuchi::mod_stop(mod);
    tsuchi::set_thread_probe(nullptr);
}

void bounded_dispatch_frames() {
    new_entry(stock);
    TSUCHI_Mod* mod = tsuchi::mod_create("hooks.frames");
    uint64_t id = 0;
    void* next = nullptr;
    CHECK(tsuchi::hook_wrap(mod, entry, 0, pointer_to(hook_swallow), 0, &id, &next) == TSUCHI_OK);
    const uint32_t thunk = uint32_t((fake::get<uint64_t>(entry) - kThunks) / 8);
    // Tail calls can have the same stack address. Their return chain is LIFO.
    for (uint32_t i = 0; i < tsuchi::kHookThunks; ++i) {
        const auto selected = tsuchi_hook_enter(thunk, 1234, 100 + i);
        CHECK(selected.tracked && selected.target == address_of(hook_swallow));
    }
    const auto exhausted = tsuchi_hook_enter(thunk, 1234, 9999);
    CHECK(!exhausted.tracked && exhausted.target == address_of(stock));
    const auto full = tsuchi::hook_stats();
    CHECK(full.frames_active == tsuchi::kHookThunks && full.frames_peak == tsuchi::kHookThunks);
    CHECK(full.thunks_used && full.thunks_used <= full.thunks_limit && full.live_records == 1 && !full.retired_records);
    CHECK(tsuchi::hook_remove(mod, id) == TSUCHI_OK);
    const auto retiring = tsuchi::hook_stats();
    CHECK(!retiring.live_records && retiring.retired_records == 1 && retiring.thunks_used == full.thunks_used);
    for (uint32_t i = tsuchi::kHookThunks; i-- > 0;) CHECK(tsuchi_hook_leave(1234) == 100 + i);
    const auto reclaimed = tsuchi::hook_stats();
    CHECK(!reclaimed.frames_active && !reclaimed.retired_records && reclaimed.thunks_used + 1 == full.thunks_used);
    CHECK(tsuchi::hook_wrap(mod, entry, 0, pointer_to(hook_swallow), 0, &id, &next) == TSUCHI_OK);
    CHECK(tsuchi_hook_enter(thunk, 1234, 5678).tracked);
    CHECK(tsuchi_hook_leave(1234) == 5678);
    tsuchi::mod_stop(mod);
}

void frames_end_out_of_order_and_calls_are_sampled() {
    new_entry(stock);
    tsuchi::set_thread_probe(thread_identity);
    TSUCHI_Mod* mod = tsuchi::mod_create("hooks.samples");
    uint64_t id = 0;
    void* next = nullptr;
    CHECK(tsuchi::hook_wrap(mod, entry, 0, pointer_to(hook_swallow), 0, &id, &next) == TSUCHI_OK);
    const uint32_t thunk = uint32_t((fake::get<uint64_t>(entry) - kThunks) / 8);
    tsuchi::HookSample hot[4];
    uint32_t all = 0;
    tsuchi::hook_sample(hot, 4, &all);
    // Two threads' calls end out of order: each returns to its own caller, and a later call takes a freed frame.
    provenance_thread = 1;
    CHECK(tsuchi_hook_enter(thunk, 500, 11).tracked);
    provenance_thread = 2;
    CHECK(tsuchi_hook_enter(thunk, 600, 22).tracked);
    provenance_thread = 1;
    CHECK(tsuchi_hook_leave(500) == 11);
    provenance_thread = 2;
    CHECK(tsuchi_hook_enter(thunk, 700, 33).tracked);
    CHECK(tsuchi_hook_leave(700) == 33 && tsuchi_hook_leave(600) == 22);
    CHECK(!tsuchi_hook_leave(600)); // nothing left to return through
    provenance_thread = 1;
    // Three calls came through the entry since the last sample: the most called place, and every call.
    const unsigned found = tsuchi::hook_sample(hot, 4, &all);
    CHECK(found == 1 && hot[0].calls == 3 && all == 3 && !std::strcmp(hot[0].name, "a table entry"));
    CHECK(tsuchi::hook_sample(hot, 4, &all) == 0 && all == 0); // the counts start again
    tsuchi::mod_stop(mod);
    tsuchi::set_thread_probe(nullptr);
}

} // namespace

namespace tsuchi {
uint32_t hook_thunk_count() { return thunk_supply; }
uint64_t hook_thunk_address(uint32_t thunk) { return kThunks + 8ull * thunk; }
} // namespace tsuchi

int main() {
    chains();
    a_first_hook_stays_outermost();
    a_hook_may_not_continue();
    a_hook_can_remove_itself();
    entries_that_are_not_stock();
    something_wraps_on_top();
    entries_that_cannot_be_hooked();
    allocation_failure_does_not_publish();
    the_public_service();
    code_sites();
    calls_from_another_thread();
    overlapping_calls();
    invocation_provenance();
    bounded_dispatch_frames();
    frames_end_out_of_order_and_calls_are_sampled();
    active_capacity_and_reuse();
    if (failures) std::printf("%d failed\n", failures);
    return failures;
}
