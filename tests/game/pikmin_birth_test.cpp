// Native restoration scheduling and storage policy against a stand-in.
// These checks do not qualify native game construction, routes, or save files.
#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <set>
#include <vector>
#include "game.h"
#include "hooks.h"
#include "pikmin_birth.h"
#include "pikmin_content.h"
namespace {
namespace t = tsuchi::game::table;
int failures = 0;
#define EXPECT(x) do { if (!(x)) { std::printf("%d: %s\n", __LINE__, #x); ++failures; } } while (0)
struct Region { std::unique_ptr<uint8_t[]> data; uint64_t at, bytes; bool write = true; };
std::deque<Region> regions;
std::set<uint64_t> live_objects;
uint64_t memory(uint64_t bytes) {
    Region r{std::make_unique<uint8_t[]>(bytes), 0, bytes, true};
    memset(r.data.get(), 0, bytes); r.at = reinterpret_cast<uint64_t>(r.data.get());
    regions.push_back(std::move(r)); return regions.back().at;
}
template <class T> void put(uint64_t at, T value) { memcpy(reinterpret_cast<void*>(at), &value, sizeof value); }
template <class T> T get(uint64_t at) { T value{}; memcpy(&value, reinterpret_cast<void*>(at), sizeof value); return value; }
bool mapped(uint64_t at, uint64_t bytes, bool write) {
    for (const auto& r : regions) if (at >= r.at && at - r.at <= r.bytes && bytes <= r.bytes - (at - r.at)) return !write || r.write;
    return false;
}
void read_only(uint64_t at) { for (auto& r : regions) if (at >= r.at && at - r.at < r.bytes) r.write = false; }
uint64_t main_address = 0;
using Batch = decltype(t::code_site::pikmin_restore_batch)::Function;
using Step = decltype(t::code_site::pikmin_restore_step)::Function;
Batch hooked_batch = nullptr;
Step hooked_step = nullptr;
int stock_left = 0, stock_calls = 0, step_result = 0, batch_return = -1;
int ready19 = TSUCHI_OK, ready21 = TSUCHI_OK, public_ready_calls = 0;
struct Birth { uint64_t leader; int32_t color, leaf, count, deco; uint64_t sublevel; bool alone; float transform[12]; };
std::vector<Birth> births;
std::vector<int32_t> ordinals;
std::vector<uint32_t> hooked_sites;
// A story whose Onion leaf row for color 19 the eject moves, as a repair that
// grows the row's array would.
uint64_t move_leaves_of = 0, moved_leaves = 0;
void move_leaves() {
    const uint64_t rows = get<uint64_t>(move_leaves_of + t::story_save::onyon_leaves + t::array::data);
    const uint64_t header = rows + 19 * t::array::size, old = get<uint64_t>(header + t::array::data);
    moved_leaves = memory(4 * 4);
    memcpy(reinterpret_cast<void*>(moved_leaves), reinterpret_cast<const void*>(old), 4 * 4);
    put(header + t::array::data, moved_leaves);
    move_leaves_of = 0;
}
void eject(void* leader, void* transform, int32_t color, int32_t leaf, int32_t count, uint64_t sublevel, bool alone, int32_t decoration) {
    Birth b{}; b.leader = reinterpret_cast<uint64_t>(leader); b.color = color; b.leaf = leaf; b.count = count;
    b.sublevel = sublevel; b.alone = alone; b.deco = decoration;
    memcpy(b.transform, transform, sizeof b.transform); births.push_back(b);
    if (move_leaves_of) move_leaves();
}
uint32_t field_deco(void*, int32_t color, int32_t ordinal) { EXPECT(color == 19 || color == 21); ordinals.push_back(ordinal); return uint32_t(ordinal % 24); }
uint32_t stored_deco(void*, int32_t color, int32_t ordinal) { EXPECT(color == 19 || color == 21); EXPECT(ordinal == 0); return 3; }
float cosine(float a) { return std::cos(a); }
float sine(float a) { return std::sin(a); }
void header(uint64_t at, uint64_t data, int32_t count) {
    put(at + t::array::data, data); put(at + t::array::count, count); put(at + t::array::max, count);
}
uint64_t colors(uint64_t at, uint32_t cells) {
    const uint64_t rows = memory(22 * t::array::size);
    for (unsigned c = 0; c < 22; ++c) header(rows + c * t::array::size, memory(cells * 4), int32_t(cells));
    header(at, rows, 22); return rows;
}
uint64_t cell(uint64_t at, uint32_t color, uint32_t leaf) {
    const uint64_t rows = get<uint64_t>(at + t::array::data);
    return get<uint64_t>(rows + color * t::array::size + t::array::data) + leaf * 4;
}
uint64_t snapshot(uint64_t at) {
    const uint64_t out = memory(t::array::size); colors(out, 4);
    for (unsigned c = 0; c < 22; ++c) for (unsigned l = 0; l < 4; ++l) put(cell(out, c, l), get<int32_t>(cell(at, c, l)));
    for (unsigned c = 0; c < 22; ++c) read_only(cell(out, c, 0));
    return out;
}
int32_t stock_step(void*, void*, const void*, void*, const void*, void* cursor) {
    ++stock_calls;
    EXPECT(get<int32_t>(reinterpret_cast<uint64_t>(cursor)) == 0x11223344);
    if (step_result) return step_result;
    if (stock_left) { --stock_left; return 1; }
    return 0;
}
int32_t stock_batch(void* raw) {
    const uint64_t state = reinterpret_cast<uint64_t>(raw);
    const uint64_t first = state + t::pikmin_birth::source_first, second = state + t::pikmin_birth::source_second;
    const uint64_t initial1 = snapshot(first), initial2 = snapshot(second), cursor = memory(17 * 4);
    for (unsigned i = 0; i < 17; ++i) put(cursor + i * 4, int32_t(0x11223344));
    const int32_t limit = get<int32_t>(state + t::pikmin_birth::limit);
    int32_t total = 0;
    for (int guard = 0; guard < 100 && (limit < 0 || total < limit); ++guard) {
        int32_t n = hooked_step(raw, reinterpret_cast<void*>(first), reinterpret_cast<void*>(initial1), reinterpret_cast<void*>(second), reinterpret_cast<void*>(initial2), reinterpret_cast<void*>(cursor));
        for (unsigned i = 0; i < 17; ++i) EXPECT(get<int32_t>(cursor + i * 4) == 0x11223344);
        if (!n) break;
        total += n;
        if (n != 1) break;
    }
    return batch_return >= 0 ? batch_return : total;
}
struct Fixture {
    uint64_t state, story, context, leader1, leader2, config;
    Fixture() {
        births.clear(); ordinals.clear(); stock_left = stock_calls = step_result = 0; batch_return = -1; ready19 = ready21 = TSUCHI_OK;
        state = memory(0x90); story = memory(0x400); context = memory(0x100); config = memory(0xF00);
        leader1 = memory(8); leader2 = memory(8);
        const uint64_t instance = memory(0x220);
        live_objects.insert(story); put(context + t::game_context::snapshot_owner, story); live_objects.insert(config); live_objects.insert(leader1); live_objects.insert(leader2);
        put(main_address + t::data::game_context, context); put(main_address + t::data::game_instance, instance);
        put(instance + t::game_instance::story_save, story);
        put(context + t::game_context::rule, int32_t(1));
        put(main_address + t::data::pikmin_scatter_config, config); put(main_address + t::data::pikmin_scatter_override, uint64_t(0));
        put(config + t::pikmin_scatter::minimum, 10.0f); put(config + t::pikmin_scatter::maximum, 20.0f);
        uint32_t rng[] = {1, 2, 3, 4}; memcpy(reinterpret_cast<void*>(main_address + t::data::random_stream), rng, sizeof rng);
        put(state + t::pikmin_birth::leader_first, leader1); put(state + t::pikmin_birth::leader_second, leader2);
        put(state + t::pikmin_birth::limit, int32_t(-1)); put(state + t::pikmin_birth::sublevel, uint64_t(0xCAFEBEEF)); put(state + t::pikmin_birth::alone, uint8_t(1));
        const float origin[] = {100, 200, 300}; memcpy(reinterpret_cast<void*>(state + t::pikmin_birth::origin), origin, sizeof origin);
        const float transform[] = {0, 0, 0, 1, 0, 0, 0, 0, 1, 1, 1, 0}; memcpy(reinterpret_cast<void*>(state + t::pikmin_birth::transform), transform, sizeof transform);
        colors(state + t::pikmin_birth::source_first, 4); colors(state + t::pikmin_birth::source_second, 4);
        colors(story + t::story_save::onyon_leaves, 4); colors(story + t::story_save::onyon_deco, 25); colors(story + t::story_save::outside_deco, 25);
    }
    uint64_t source(unsigned side, unsigned color, unsigned leaf) { return cell(state + (side ? t::pikmin_birth::source_second : t::pikmin_birth::source_first), color, leaf); }
    int run() { return hooked_batch(reinterpret_cast<void*>(state)); }
};
}
namespace tsuchi::game {
uint64_t main_base() { return main_address; }
bool readable(uint64_t at, uint64_t bytes) { return mapped(at, bytes, false); }
bool writable(uint64_t at, uint64_t bytes) { return mapped(at, bytes, true); }
bool executable(uint64_t) { return false; }
bool live(uint64_t object) { return live_objects.count(object) != 0; }
uint64_t from_weak_reference(uint64_t at) { const uint64_t object = read_or<uint64_t>(at); return live(object) ? object : 0; }
}
namespace tsuchi {
TSUCHI_Result added_pikmin_ready(uint8_t) { ++public_ready_calls; EXPECT(false); return TSUCHI_NOT_READY; }
TSUCHI_Result added_pikmin_native_ready(uint8_t color) { return TSUCHI_Result(color == 19 ? ready19 : ready21); }
TSUCHI_Result hook_code_site(TSUCHI_Mod*, uint32_t site, void* wrapper, uint64_t* id, void** original) {
    hooked_sites.push_back(site); *id = uint64_t(site) + 1;
    if (site == uint32_t(t::code_site::pikmin_restore_batch)) { hooked_batch = reinterpret_cast<Batch>(wrapper); *original = reinterpret_cast<void*>(&stock_batch); }
    else if (site == uint32_t(t::code_site::pikmin_restore_step)) { hooked_step = reinterpret_cast<Step>(wrapper); *original = reinterpret_cast<void*>(&stock_step); }
    else return TSUCHI_INVALID;
    return TSUCHI_OK;
}
TSUCHI_Result hook_remove(TSUCHI_Mod*, uint64_t) { return TSUCHI_OK; }
}
int main() {
    const uint64_t global_memory = memory(t::data::random_stream - t::data::game_context + 32);
    main_address = global_memory - t::data::game_context;
    t::fn::eject_pikmin = eject; t::fn::pikmin_deco_field = field_deco; t::fn::pikmin_deco_stored = stored_deco;
    t::fn::pikmin_scatter_cos = cosine; t::fn::pikmin_scatter_sin = sine;
    EXPECT(tsuchi::game::pikmin_birth_start(reinterpret_cast<TSUCHI_Mod*>(1)) == TSUCHI_OK);
    EXPECT(hooked_sites.size() == 2 && hooked_sites[0] == uint32_t(t::code_site::pikmin_restore_step));
    { Fixture f; put(f.source(0,19,2), int32_t(2)); put(f.source(0,19,1), int32_t(1)); put(f.source(0,21,0), int32_t(1)); put(f.source(1,19,2), int32_t(2)); stock_left = 2;
      EXPECT(f.run() == 8 && births.size() == 6);
      EXPECT(births[0].color == 19 && births[0].leaf == 2 && births[1].leaf == 2 && births[2].leaf == 1 && births[3].color == 21 && births[3].leaf == 0);
      EXPECT(births[4].leader == f.leader2 && births[5].leader == f.leader2 && ordinals == std::vector<int32_t>({0,1,2,3}));
      EXPECT(births[0].count == 1 && births[0].alone && births[0].sublevel == 0xCAFEBEEF && births[0].transform[3] == 1 && births[0].transform[8] == 1);
      EXPECT(std::abs(births[0].transform[4] - 110.0f) < 0.0001f && births[0].transform[6] == 300);
    }
    { Fixture f; put(f.source(0,19,2), int32_t(2)); put(f.state + t::pikmin_birth::limit, int32_t(2)); stock_left = 1;
      EXPECT(f.run() == 2 && births.size() == 1 && get<int32_t>(f.source(0,19,2)) == 1);
      EXPECT(get<uint32_t>(main_address + t::data::random_stream) == 3 && get<uint32_t>(main_address + t::data::random_stream + 4) == 4);
      EXPECT(get<uint32_t>(main_address + t::data::random_stream + 8) == 2061 && get<uint32_t>(main_address + t::data::random_stream + 12) == 6175);
    }
    { Fixture f; put(f.source(0,19,0), int32_t(1)); put(f.state + t::pikmin_birth::limit, int32_t(0)); EXPECT(f.run() == 0 && births.empty() && stock_calls == 0); }
    { Fixture f; put(f.source(0,19,0), int32_t(1)); step_result = -7; batch_return = 77; EXPECT(f.run() == 77 && births.empty() && get<int32_t>(f.source(0,19,0)) == 1); }
    // Public HUD readiness is deliberately unavailable in this stand-in. Saved
    // birth needs the native data readiness rather than the SDK add contract.
    { Fixture f; put(f.source(0,19,0), int32_t(1)); EXPECT(f.run() == 1 && births.size() == 1 && get<int32_t>(f.source(0,19,0)) == 0 && public_ready_calls == 0); }
    { Fixture f; put(f.source(0,19,0), int32_t(1)); ready19 = TSUCHI_NOT_READY; EXPECT(f.run() == 0 && births.empty() && get<int32_t>(f.source(0,19,0)) == 1 && public_ready_calls == 0); EXPECT(get<uint32_t>(main_address + t::data::random_stream) == 1); }
    { Fixture f; put(f.source(0,19,0), int32_t(1)); ready19 = TSUCHI_NOT_FOUND; EXPECT(f.run() == 0 && births.empty() && get<int32_t>(f.source(0,19,0)) == 1); EXPECT(get<uint32_t>(main_address + t::data::random_stream) == 1); }
    { Fixture f; EXPECT(!tsuchi::game::live(f.context)); put(f.source(0,19,0), int32_t(1)); put(f.context + t::game_context::rule, int32_t(6)); EXPECT(f.run() == 0 && births.empty() && get<int32_t>(f.source(0,19,0)) == 1); }
    { Fixture f; put(f.source(0,19,0), int32_t(1)); put(f.context + t::game_context::snapshot_owner, uint64_t(0)); EXPECT(f.run() == 0 && births.empty() && get<int32_t>(f.source(0,19,0)) == 1); }
    { Fixture f; put(f.source(0,19,0), int32_t(1)); read_only(f.source(0,19,0)); EXPECT(f.run() == 0 && births.empty() && get<int32_t>(f.source(0,19,0)) == 1); }
    { Fixture f; put(f.source(0,19,0), int32_t(1)); put(f.state + t::pikmin_birth::source_first + t::array::count, int32_t(17)); EXPECT(f.run() == 0 && births.empty() && get<int32_t>(f.source(0,19,0)) == 1); }
    { Fixture f; put(f.source(0,19,2), int32_t(1)); put(f.state + t::pikmin_birth::onion, uint8_t(1));
      put(cell(f.story + t::story_save::onyon_deco, 19,3), int32_t(2)); put(cell(f.story + t::story_save::outside_deco,19,3), int32_t(99999)); put(cell(f.story + t::story_save::onyon_leaves,19,2), int32_t(2));
      EXPECT(f.run() == 1 && births.size() == 1 && births[0].deco == 3);
      EXPECT(get<int32_t>(cell(f.story + t::story_save::onyon_deco,19,3)) == 1 && get<int32_t>(cell(f.story + t::story_save::outside_deco,19,3)) == 99999 && get<int32_t>(cell(f.story + t::story_save::onyon_leaves,19,2)) == 1);
    }
    { Fixture f; put(f.source(0,19,2), int32_t(1)); put(f.state + t::pikmin_birth::onion, uint8_t(1));
      put(cell(f.story + t::story_save::onyon_deco, 19,3), int32_t(2)); put(cell(f.story + t::story_save::onyon_leaves,19,2), int32_t(2));
      const uint64_t before = cell(f.story + t::story_save::onyon_leaves,19,2);
      move_leaves_of = f.story;
      EXPECT(f.run() == 1 && births.size() == 1 && moved_leaves);
      EXPECT(get<int32_t>(moved_leaves + 2 * 4) == 1 && get<int32_t>(before) == 2);
    }
    { Fixture f; put(f.source(0,19,2), int32_t(1)); put(f.state + t::pikmin_birth::onion, uint8_t(1)); read_only(cell(f.story + t::story_save::outside_deco,19,0)); EXPECT(f.run() == 0 && births.empty() && get<int32_t>(f.source(0,19,2)) == 1); }
    EXPECT(tsuchi::game::pikmin_birth_start(reinterpret_cast<TSUCHI_Mod*>(1)) == TSUCHI_OK && hooked_sites.size() == 2);
    std::printf("Pikmin restoration birth policy: %d failures\n", failures);
    return failures ? 1 : 0;
}
