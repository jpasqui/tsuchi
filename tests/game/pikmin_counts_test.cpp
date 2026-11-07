// The game's Pikmin counts with added colors, against a made-up counter laid out as the game's.
// The stand-ins recount, add and query the way the game's functions do. This proves no game behavior.
#include <cstdio>
#include <cstring>
#include <map>
#include <vector>
#include "actors.h"
#include "game.h"
#include "hooks.h"
#include "objects.h"
#include "pikmin_counts.h"
#include "registry.h"
namespace t = tsuchi::game::table;
namespace {
int failures = 0;
#define EXPECT(x) do { if (!(x)) { std::printf("%d: %s\n", __LINE__, #x); ++failures; } } while (0)
alignas(16) unsigned char counter[t::pikmin_counter::most + 16]{}, scratch[t::pikmin_counter::most + 16]{};
alignas(16) unsigned char framework[0x1000]{}, instance[0x400]{}, story[0x400]{};
alignas(16) uint64_t instance_global = 0;
struct Inner { uint64_t data; int32_t count, max; };
Inner onion_rows[22]{};
int32_t onion_cells[22][3]{};
uint64_t thread_now = 1;
// A Pikmin as the counter sees it: its color, its leaf, and the states and players it counts in.
struct Pikmin { unsigned char object[0x300]{}; uint8_t leaf = 0; std::vector<std::pair<uint32_t, uint32_t>> cells; };
std::vector<Pikmin*> field;
decltype(t::code_site::pikmin_counter_update)::Function update_hook = nullptr;
decltype(t::code_site::pikmin_counter_add)::Function add_hook = nullptr;
decltype(t::code_site::pikmin_counter_query)::Function query_hook = nullptr;
bool inside(uint64_t address, uint64_t bytes, const void* block, uint64_t size) {
    const uint64_t begin = reinterpret_cast<uint64_t>(block);
    return address >= begin && address - begin <= size && bytes <= size - (address - begin);
}
uint64_t at(const void* p) { return reinterpret_cast<uint64_t>(p); }
template <class T> T get(uint64_t address) { T v{}; memcpy(&v, reinterpret_cast<void*>(address), sizeof v); return v; }
template <class T> void put(uint64_t address, T v) { memcpy(reinterpret_cast<void*>(address), &v, sizeof v); }
uint64_t cell(uint64_t object, unsigned bank, unsigned row, unsigned leaf, unsigned state, unsigned player) {
    return object + bank * t::pikmin_counter::bank_stride + row * t::pikmin_counter_row::size + leaf * t::pikmin_counter::leaf_stride +
           state * t::pikmin_counter::mode_stride + player * t::pikmin_counter::player_stride;
}
// The game's own rows: Red 0 is row 3, Blue 1 row 2, Yellow 2 row 5 and so on to color 14 in row 15. Anything else is row 0.
unsigned row_of(uint8_t color) {
    static const unsigned rows[15] = {3, 2, 5, 7, 6, 1, 4, 8, 9, 10, 11, 12, 13, 14, 15};
    return color < 15 ? rows[color] : 0;
}
void game_add(void* raw, void* actor) {
    const uint64_t object = at(raw);
    const unsigned bank = get<int32_t>(object + t::pikmin_counter::bank) ? 1 : 0;
    for (Pikmin* p : field) {
        if (p->object != actor) continue;
        const unsigned row = row_of(get<uint8_t>(at(p->object) + t::game_actor::color));
        for (const auto& c : p->cells) { const uint64_t a = cell(object, bank, row, p->leaf + 1, c.first, c.second); put(a, get<int32_t>(a) + 1); }
    }
}
// The game copies the Onion's stock of colors 0 to 14 and 16 into state 4 of the bank it counts into.
void onion_copy(uint64_t object) {
    const unsigned bank = get<int32_t>(object + t::pikmin_counter::bank) ? 1 : 0;
    for (uint8_t color = 0; color < 17; ++color) {
        if (color == 15) continue;
        for (unsigned leaf = 0; leaf < 3; ++leaf) put(cell(object, bank, row_of(color), leaf + 1, 4, 1), onion_cells[color][leaf]);
    }
}
void clear_bank(uint64_t object, unsigned bank) { memset(reinterpret_cast<void*>(object + bank * t::pikmin_counter::bank_stride), 0, t::pikmin_counter::bank_stride); }
// The game's three ways through its update, each calling the add through the patched entry. The first
// update and one the game forces recount everything; flags bit 0 recounts the read bank in place; any
// other update only publishes the bank the field's Pikmin counted into since the last one.
void game_update(void* raw, uint64_t flags) {
    const uint64_t object = at(raw);
    const int32_t read = get<int32_t>(object + t::pikmin_counter::read_bank);
    const bool full = !get<uint8_t>(object + t::pikmin_counter::initialized) || get<uint8_t>(object + t::pikmin_counter::force_recount);
    if (full) {
        clear_bank(object, 0); clear_bank(object, 1);
        put(object + t::pikmin_counter::read_bank, int32_t(0)); put(object + t::pikmin_counter::bank, int32_t(0));
    } else if (flags & 1) {
        put(object + t::pikmin_counter::bank, read);
        clear_bank(object, unsigned(read));
    }
    onion_copy(object);
    put(object + t::pikmin_counter::initialized, uint8_t(1)); put(object + t::pikmin_counter::force_recount, uint8_t(0));
    if (!full && !(flags & 1)) {
        const int32_t write = get<int32_t>(object + t::pikmin_counter::bank);
        put(object + t::pikmin_counter::read_bank, write); put(object + t::pikmin_counter::bank, int32_t(write == 0));
        clear_bank(object, write == 0 ? 1 : 0);
        return;
    }
    for (Pikmin* p : field) add_hook(raw, p->object);
    if (full) {
        const int32_t write = get<int32_t>(object + t::pikmin_counter::bank);
        put(object + t::pikmin_counter::read_bank, write); put(object + t::pikmin_counter::bank, int32_t(write == 0));
    } else put(object + t::pikmin_counter::bank, int32_t(get<int32_t>(object + t::pikmin_counter::read_bank) == 0));
}
enum Recount { full, in_place, publish };
// Calls the update as the game does for each way: a forced recount sets the counter's own request first.
void recount(Recount kind) {
    if (kind == full) put(at(counter) + t::pikmin_counter::force_recount, uint8_t(1));
    update_hook(counter, kind == in_place ? 1 : 0);
}
// The game's query: 0 for a row is rows 1 to 16, for a leaf leaves 1 to 3, for a state states 1 and 4, for a player players 1 to 5.
int32_t game_query(void* raw, int32_t row, int32_t leaf, int32_t state, int32_t player) {
    const uint64_t object = at(raw);
    const unsigned bank = get<int32_t>(object + t::pikmin_counter::read_bank) == 1 ? 1 : 0;
    int32_t total = 0;
    const unsigned states[2] = {1, 4};
    for (int r = row ? row : 1; r <= (row ? row : 16); ++r)
        for (int l = leaf ? leaf : 1; l <= (leaf ? leaf : 3); ++l)
            for (unsigned s = 0; s < (state ? 1u : 2u); ++s)
                for (int p = player ? player : 1; p <= (player ? player : 5); ++p)
                    total += get<int32_t>(cell(object, bank, unsigned(r), unsigned(l), state ? unsigned(state) : states[s], unsigned(p)));
    return total;
}
Pikmin* pikmin(uint8_t color, uint8_t leaf, std::vector<std::pair<uint32_t, uint32_t>> cells) {
    Pikmin* p = new Pikmin;
    put(at(p->object) + t::game_actor::color, color);
    p->leaf = leaf, p->cells = std::move(cells);
    field.push_back(p);
    return p;
}
void remove(Pikmin* p) {
    for (size_t i = 0; i < field.size(); ++i) if (field[i] == p) { field.erase(field.begin() + long(i)); delete p; return; }
}
int32_t ask(int32_t row, int32_t leaf, int32_t state, int32_t player) { return query_hook(counter, row, leaf, state, player); }
} // namespace

namespace tsuchi {
uint64_t known_game_thread_token() { return thread_now; }
TSUCHI_Result hook_code_site(TSUCHI_Mod*, uint32_t site, void* function, uint64_t* id, void** before) {
    *id = site + 1;
    if (site == t::code_site::pikmin_counter_update.number) update_hook = reinterpret_cast<decltype(update_hook)>(function), *before = reinterpret_cast<void*>(&game_update);
    else if (site == t::code_site::pikmin_counter_add.number) add_hook = reinterpret_cast<decltype(add_hook)>(function), *before = reinterpret_cast<void*>(&game_add);
    else if (site == t::code_site::pikmin_counter_query.number) query_hook = reinterpret_cast<decltype(query_hook)>(function), *before = reinterpret_cast<void*>(&game_query);
    else return TSUCHI_UNSUPPORTED;
    return TSUCHI_OK;
}
}
namespace tsuchi::game {
uint64_t main_base() { return at(&instance_global) - t::data::game_instance; }
uint64_t game_framework() { return at(framework); }
bool live(uint64_t object) { return object == at(story); }
bool readable(uint64_t address, uint64_t bytes) {
    if (inside(address, bytes, counter, sizeof counter) || inside(address, bytes, scratch, sizeof scratch) || inside(address, bytes, framework, sizeof framework) ||
        inside(address, bytes, instance, sizeof instance) || inside(address, bytes, story, sizeof story) || inside(address, bytes, &instance_global, sizeof instance_global) ||
        inside(address, bytes, onion_rows, sizeof onion_rows) || inside(address, bytes, onion_cells, sizeof onion_cells)) return true;
    for (Pikmin* p : field) if (inside(address, bytes, p->object, sizeof p->object)) return true;
    return false;
}
bool writable(uint64_t address, uint64_t bytes) { return readable(address, bytes); }
}

int main() {
    using tsuchi::game::PikminCountScope;
    put(at(framework) + t::game_framework::pikmin_counters, at(counter));
    instance_global = at(instance);
    put(at(instance) + t::game_instance::story_save, at(story));
    auto* owner = reinterpret_cast<TSUCHI_Mod*>(1);
    EXPECT(tsuchi::game::pikmin_counts_start(owner) == TSUCHI_OK && update_hook && add_hook && query_hook);
    // The story's Onion rows: 17 stock colors until the framework grows them.
    for (unsigned c = 0; c < 22; ++c) onion_rows[c] = Inner{at(onion_cells[c]), 3, 3};
    put(at(story) + t::story_save::onyon_leaves + t::array::data, at(onion_rows));
    put(at(story) + t::story_save::onyon_leaves + t::array::count, int32_t(17));
    onion_cells[0][2] = 30;                       // thirty red flowers in the Onion
    onion_cells[19][0] = 4, onion_cells[19][2] = 1;
    const uint32_t field_state = t::constant::pikmin_counter_field, joined = t::constant::pikmin_counter_joined;
    const uint32_t current = t::constant::pikmin_counter_current;
    // Three reds and two color-19 flowers follow player 2; one color-21 bud waits in the field.
    for (int i = 0; i < 3; ++i) pikmin(0, 2, {{field_state, current}, {joined, current}});
    Pikmin* amber[2] = {pikmin(19, 2, {{field_state, current}, {joined, current}}), pikmin(19, 2, {{field_state, current}, {joined, current}})};
    pikmin(21, 1, {{field_state, current}});

    // Off: every call is the game's, and the totals leave the added colors out as the game does.
    recount(full);
    EXPECT(ask(0, 0, joined, current) == 3 && ask(0, 0, field_state, 0) == 3 && ask(0, 0, 0, 0) == 33);
    EXPECT(get<int32_t>(cell(at(counter), 0, 0, 3, joined, current)) == 2);   // row 0 holds them
    {
        PikminCountScope scope(19);
        EXPECT(ask(0, 0, joined, current) == 3);
    }

    // On: the totals count them, and the stock rows stay the game's.
    tsuchi::game::pikmin_counts_enable(true);
    recount(full);
    EXPECT(ask(0, 0, joined, current) == 5 && ask(0, 0, field_state, 0) == 6);
    EXPECT(ask(row_of(0), 0, joined, current) == 3 && ask(row_of(0), 0, 0, 0) == 33);
    EXPECT(ask(0, 3, joined, current) == 5 && ask(0, 2, field_state, 0) == 1 && ask(0, 3, joined, 3) == 0);
    EXPECT(ask(0, 0, 0, 0) == 36);                 // the field and the Onion, which holds no added color yet
    {
        PikminCountScope first(19);
        EXPECT(ask(0, 0, joined, current) == 2 && ask(0, 0, field_state, 0) == 2);
        {
            PikminCountScope second(21);
            EXPECT(ask(0, 0, joined, current) == 0 && ask(0, 0, field_state, 0) == 1);
            thread_now = 2;                       // another thread asks for every type
            EXPECT(ask(0, 0, joined, current) == 5);
            thread_now = 1;
        }
        EXPECT(ask(0, 0, joined, current) == 2);
        EXPECT(ask(row_of(0), 0, joined, current) == 3);   // a stock row is never narrowed
    }
    EXPECT(ask(0, 0, joined, current) == 5);
    // The same counts asked for directly, as the selector over the throw cursor does.
    EXPECT(tsuchi::game::pikmin_counts_color(19, 0, joined, current) == 2 && tsuchi::game::pikmin_counts_color(21, 0, field_state, 0) == 1);
    EXPECT(tsuchi::game::pikmin_counts_color(0, 0, joined, current) == -1 && tsuchi::game::pikmin_counts_color(19, 9, joined, current) == -1);
    // Arguments outside the counter, and another counter, are left to the game.
    EXPECT(ask(0, 9, joined, current) == game_query(counter, 0, 9, joined, current));
    EXPECT(ask(0, 0, joined, 7) == game_query(counter, 0, 0, joined, 7));
    {
        const int32_t before = ask(0, 0, joined, current);
        memset(scratch, 0, sizeof scratch);
        add_hook(scratch, amber[0]->object);
        EXPECT(get<int32_t>(cell(at(scratch), 0, 0, 3, joined, current)) == 1 && ask(0, 0, joined, current) == before);
        EXPECT(query_hook(scratch, 0, 0, joined, current) == 0);
    }

    // The Onion's stock of the added colors counts once the story's rows reach them.
    put(at(story) + t::story_save::onyon_leaves + t::array::count, int32_t(22));
    EXPECT(ask(0, 0, 4, 0) == 35 && ask(0, 1, 4, 1) == 4 && ask(0, 0, 4, 2) == 0 && ask(0, 0, 0, 0) == 41);
    {
        PikminCountScope scope(19);
        EXPECT(ask(0, 0, 4, 0) == 5 && ask(0, 0, 0, 0) == 7);
    }

    // The game reads one bank while it counts into the other. Pikmin added between recounts
    // land in the bank being counted, and a bank the game clears drops what the framework held for it.
    recount(publish);                       // the read bank is now the one counted between frames: nothing yet
    EXPECT(ask(0, 0, joined, current) == 0);
    for (Pikmin* p : field) add_hook(counter, p->object);
    recount(publish);
    EXPECT(ask(0, 0, joined, current) == 5);
    recount(publish);
    EXPECT(ask(0, 0, joined, current) == 0);
    for (Pikmin* p : field) add_hook(counter, p->object);
    recount(publish);
    EXPECT(ask(0, 0, joined, current) == 5);
    // A recount in place replaces what the framework held for the bank.
    remove(amber[1]);
    recount(in_place);
    EXPECT(ask(0, 0, joined, current) == 4 && ask(0, 0, field_state, 0) == 5);
    {
        PikminCountScope scope(19);
        EXPECT(ask(0, 0, joined, current) == 1);
    }
    remove(amber[0]);
    recount(in_place);
    EXPECT(ask(0, 0, joined, current) == 3);
    recount(full);
    EXPECT(ask(0, 0, joined, current) == 3 && ask(0, 0, field_state, 0) == 4);

    // Off again: the game's answers, with nothing kept from before.
    tsuchi::game::pikmin_counts_enable(false);
    EXPECT(ask(0, 0, field_state, 0) == 3 && ask(0, 0, 4, 0) == 30);
    EXPECT(tsuchi::game::pikmin_counts_color(21, 0, field_state, 0) == -1);
    tsuchi::game::pikmin_counts_enable(true);
    EXPECT(ask(0, 0, field_state, 0) == 3);       // until the next recount
    recount(full);
    EXPECT(ask(0, 0, field_state, 0) == 4);
    while (!field.empty()) remove(field.back());
    std::printf("Added Pikmin counts: %d failures\n", failures);
    return failures ? 1 : 0;
}
