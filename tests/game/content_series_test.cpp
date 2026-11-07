#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unordered_map>
#include <vector>
#include "actor_events.h"
#include "frame.h"
#include "ordinary_lifecycle.h"
#include "parts.h"
#include "game.h"
#include "holds.h"
void* series_checked_copy(void*, const void*, size_t);
#define memcpy series_checked_copy
#include "../../framework/game/content_series.cpp"
#undef memcpy

using namespace tsuchi;
namespace {
namespace table = game::table;
int checks = 0, failures = 0;
#define CHECK(x) do { ++checks; if (!(x)) { ++failures; std::printf("line %d: %s\n", __LINE__, #x); } } while (0)
alignas(8) unsigned char manager[0x1000]{};
alignas(8) unsigned char own_row[0x20]{}, native_row[0x20]{}, later_row[0x20]{}, duplicate_row[0x20]{};
std::unordered_map<void*, uint64_t> engine_allocations;
std::unordered_map<void*, uint64_t> retired_allocations;
std::vector<void(*)()> steps;
bool manager_live = true, manager_read = true, manager_write = true, array_write = true, refuse_allocate = false;
bool stop_in_allocate = false, stop_in_release = false;
bool rebuild_in_allocate = false;
uint32_t allocations = 0, frees = 0;
uint32_t stale_reads = 0;
TSUCHI_Mod* stopping = nullptr;
uint64_t thread = 1;
uint64_t address(const void* pointer) { return reinterpret_cast<uint64_t>(pointer); }
game::ArrayMemory header() {
    game::ArrayMemory out{};
    std::memcpy(&out, manager + table::content_series_manager::rows, sizeof out);
    return out;
}
void header(const game::ArrayMemory& value) { std::memcpy(manager + table::content_series_manager::rows, &value, sizeof value); }
bool same_header(game::ArrayMemory a, game::ArrayMemory b) { return a.data == b.data && a.count == b.count && a.max == b.max; }
void row(unsigned char* bytes, uint32_t series, float rate) {
    std::memset(bytes, 0, 0x20);
    std::memcpy(bytes + table::content_series_row::series, &series, sizeof series);
    std::memcpy(bytes + table::content_series_row::rate, &rate, sizeof rate);
}
bool in(uint64_t p, uint64_t size, const void* base, uint64_t bytes) { return p >= address(base) && size <= bytes && p - address(base) <= bytes - size; }
void step() { for (auto fn : steps) fn(); }
void reset() {
    CHECK(series_holds.count() == 0);
    for (const auto& allocation : engine_allocations) std::free(allocation.first);
    for (const auto& allocation : retired_allocations) std::free(allocation.first);
    engine_allocations.clear();
    retired_allocations.clear();
    std::memset(manager, 0, sizeof manager);
    manager_live = manager_read = manager_write = array_write = true;
    refuse_allocate = stop_in_allocate = stop_in_release = rebuild_in_allocate = false;
    row(own_row, 90, 2); row(native_row, 10, 1); row(later_row, 11, 1); row(duplicate_row, 90, 3);
}
void cache(const std::vector<uint64_t>& rows, uint32_t capacity = 0) {
    const auto before = header();
    const uint32_t room = capacity > rows.size() ? capacity : uint32_t(rows.size());
    auto* pointers = room ? static_cast<uint64_t*>(game::engine_memory(uint64_t(room) * 8)) : nullptr;
    if (pointers) {
        std::memset(pointers, 0, room * 8);
        for (size_t i = 0; i < rows.size(); ++i) pointers[i] = rows[i];
    }
    header({address(pointers), int32_t(rows.size()), int32_t(room)});
    if (before.data) game::release_engine_memory(reinterpret_cast<void*>(before.data));
}
bool entries(const std::vector<uint64_t>& wanted) {
    const auto current = header();
    if (current.count != int32_t(wanted.size())) return false;
    for (size_t i = 0; i < wanted.size(); ++i)
        if (game::read_or<uint64_t>(current.data + uint64_t(i) * 8) != wanted[i]) return false;
    return true;
}
TSUCHI_Mod* start(const char* id) { auto* result = mod_create(id); CHECK(result != nullptr); return result; }
TSUCHI_Result hold(TSUCHI_Mod* mod) { return content_series_hold(mod, address(manager), address(own_row), 90); }
}
void* series_checked_copy(void* destination, const void* source, size_t bytes) {
    if (!game::readable(address(source), bytes))
        for (const auto& retired : retired_allocations)
            if (in(address(source), bytes, retired.first, retired.second)) { ++stale_reads; return destination; }
    return std::memcpy(destination, source, bytes);
}
namespace tsuchi {
bool each_frame(void (*fn)()) { steps.push_back(fn); return true; }
bool on_actor_left(void (*)(uint64_t, uint64_t, ActorLeft)) { return true; }
void ordinary_lifecycle_listener(void (*)(uint64_t, uint64_t, bool)) {}
TSUCHI_Result ordinary_lifecycle_watch(uint64_t) { return TSUCHI_OK; }
uint64_t ordinary_lifecycle_restore_part(uint64_t object, uint64_t) { return object; }
}
namespace tsuchi::game {
uint64_t from_handle(uint64_t handle) { return manager_live && handle == 1 ? address(manager) : 0; }
uint64_t to_handle(uint64_t pointer) { return manager_live && pointer == address(manager) ? 1 : 0; }
uint64_t part_owner(uint64_t) { return 0; }
bool is_game_actor(uint64_t) { return false; }
bool readable(uint64_t p, uint64_t bytes) {
    if (in(p, bytes, manager, sizeof manager)) return manager_read;
    if (in(p, bytes, own_row, sizeof own_row) || in(p, bytes, native_row, sizeof native_row) ||
        in(p, bytes, later_row, sizeof later_row) || in(p, bytes, duplicate_row, sizeof duplicate_row)) return true;
    for (const auto& allocation : engine_allocations) if (in(p, bytes, allocation.first, allocation.second)) return true;
    return false;
}
bool writable(uint64_t p, uint64_t bytes) {
    if (in(p, bytes, manager, sizeof manager)) return manager_write;
    for (const auto& allocation : engine_allocations) if (in(p, bytes, allocation.first, allocation.second)) return array_write;
    return false;
}
void* engine_memory(uint64_t bytes) {
    ++allocations;
    if (stop_in_allocate) { stop_in_allocate = false; mod_stop(stopping); }
    if (rebuild_in_allocate) { rebuild_in_allocate = false; cache({address(later_row)}); }
    if (refuse_allocate) return nullptr;
    void* block = std::malloc(size_t(bytes));
    if (block) engine_allocations[block] = bytes;
    return block;
}
void release_engine_memory(void* block) {
    if (!block) return;
    ++frees;
    const auto at = engine_allocations.find(block);
    CHECK(at != engine_allocations.end());
    if (at == engine_allocations.end()) return;
    retired_allocations[block] = at->second;
    // Keep a bounded host quarantine so stale reads cannot be hidden by allocator
    // address reuse. The next case frees these blocks.
    engine_allocations.erase(at);
    if (stop_in_release) { stop_in_release = false; mod_stop(stopping); }
}
}
int main() {
    set_thread_probe([] { return thread; }); note_game_thread();
    reset(); auto* mod = start("series.identity");
    cache({address(native_row)});
    const auto identity_before = header();
    CHECK(content_series_hold(mod, address(manager), address(own_row), 91) == TSUCHI_INVALID);
    CHECK(same_header(identity_before, header()) && series_holds.count() == 0 && owned_count(mod) == 0);
    thread = 2;
    CHECK(hold(mod) == TSUCHI_INVALID && same_header(identity_before, header()));
    thread = 1;
    mod_stop(mod);
    reset(); mod = start("series.release");
    cache({address(native_row)});
    CHECK(hold(mod) == TSUCHI_OK && series_holds.count() == 1 && owned_count(mod) == 1);
    CHECK(entries({address(native_row), address(own_row)}));
    const auto same = header(); CHECK(hold(mod) == TSUCHI_OK && same_header(same, header()));
    cache({address(native_row), address(own_row), address(later_row)}, 4);
    CHECK(series_holds.release(mod, 1, 90) == TSUCHI_OK);
    CHECK(entries({address(native_row), address(later_row)}) && owned_count(mod) == 0);
    CHECK(game::read_or<uint64_t>(header().data + 16) == 0);
    mod_stop(mod);

    reset(); mod = start("series.stopped"); cache({address(native_row)}, 4);
    const auto before_spare = header(); CHECK(hold(mod) == TSUCHI_OK);
    CHECK(header().data == before_spare.data && header().max == 4);
    mod_stop(mod); CHECK(entries({address(native_row)}) && series_holds.count() == 0);

    reset(); mod = start("series.duplicate"); cache({address(duplicate_row)});
    const auto duplicate_before = header(); const uint32_t allocations_before = allocations;
    CHECK(hold(mod) == TSUCHI_BUSY && series_holds.count() == 0 && owned_count(mod) == 0);
    CHECK(same_header(duplicate_before, header()) && allocations == allocations_before && entries({address(duplicate_row)}));
    mod_stop(mod);

    reset(); mod = start("series.refused"); cache({address(native_row)});
    const auto allocation_before = header(); refuse_allocate = true;
    CHECK(hold(mod) == TSUCHI_NO_MEMORY && series_holds.count() == 0 && owned_count(mod) == 0);
    CHECK(same_header(allocation_before, header()) && entries({address(native_row)}));
    refuse_allocate = false;
    CHECK(content_series_hold(mod, 0, address(own_row), 90) == TSUCHI_NOT_READY);
    CHECK(content_series_hold(mod, address(manager), 0, 90) == TSUCHI_NOT_READY);
    manager_read = false; CHECK(hold(mod) == TSUCHI_NOT_READY); manager_read = true;
    manager_write = false; CHECK(hold(mod) == TSUCHI_NOT_READY); manager_write = true;
    thread = 2; CHECK(hold(mod) == TSUCHI_INVALID && same_header(allocation_before, header())); thread = 1;
    mod_stop(mod);

    reset(); mod = start("series.corrupt");
    const game::ArrayMemory corrupt[]{{0, -1, 0}, {0, 2, 1}, {0, 0, int32_t(table::constant::max_array_count) + 1}, {0, 1, 1}, {0x1234, 1, 1}};
    for (const auto& bad : corrupt) { header(bad); CHECK(hold(mod) == TSUCHI_NOT_READY && same_header(bad, header()) && owned_count(mod) == 0); }
    header({}); cache({address(native_row)}, 3); array_write = false;
    const auto unwritable = header(); CHECK(hold(mod) == TSUCHI_NOT_READY && same_header(unwritable, header())); array_write = true;
    mod_stop(mod);

    reset(); mod = start("series.rebuild"); cache({address(native_row)}); CHECK(hold(mod) == TSUCHI_OK);
    cache({address(later_row)}); step(); CHECK(entries({address(later_row), address(own_row)}) && series_holds.count() == 1);
    cache({address(native_row)}); refuse_allocate = true; const auto temporary = header();
    step(); CHECK(same_header(temporary, header()) && series_holds.count() == 1 && owned_count(mod) == 1);
    refuse_allocate = false; step(); CHECK(entries({address(native_row), address(own_row)}) && series_holds.count() == 1);
    cache({}); manager_read = false; step(); CHECK(series_holds.count() == 1 && owned_count(mod) == 1);
    manager_read = true; step(); CHECK(entries({address(own_row)}));
    mod_stop(mod); CHECK(entries({}) && series_holds.count() == 0);

    reset(); mod = start("series.lost"); CHECK(hold(mod) == TSUCHI_OK);
    cache({address(native_row), address(duplicate_row)}); step();
    CHECK(series_holds.count() == 0 && owned_count(mod) == 0 && entries({address(native_row), address(duplicate_row)}));
    mod_stop(mod);

    reset(); mod = start("series.destroyed"); CHECK(hold(mod) == TSUCHI_OK);
    const auto dead_before = header(); manager_live = false; step();
    CHECK(series_holds.count() == 0 && owned_count(mod) == 0 && same_header(dead_before, header()));
    manager_live = true; mod_stop(mod);

    reset(); stopping = start("series.allocator-stop"); cache({address(native_row)});
    const auto canceled = header(); const size_t block_count = engine_allocations.size();
    stop_in_allocate = true;
    CHECK(hold(stopping) == TSUCHI_NOT_READY);
    CHECK(series_holds.count() == 0 && owned_count(stopping) == 0 && same_header(canceled, header()));
    CHECK(entries({address(native_row)}) && engine_allocations.size() == block_count);

    reset(); mod = start("series.allocator-rebuild"); cache({address(native_row)});
    rebuild_in_allocate = true;
    const uint32_t reads_before = stale_reads;
    CHECK(hold(mod) == TSUCHI_NOT_READY);
    CHECK(entries({address(later_row)}) && series_holds.count() == 0 && owned_count(mod) == 0);
    CHECK(stale_reads == reads_before && engine_allocations.size() == 1);
    mod_stop(mod);

    reset(); stopping = start("series.keep-allocator-stop"); CHECK(hold(stopping) == TSUCHI_OK);
    cache({address(native_row)}); const auto keep_canceled = header(); stop_in_allocate = true;
    step();
    CHECK(series_holds.count() == 0 && owned_count(stopping) == 0 && same_header(keep_canceled, header()));
    CHECK(entries({address(native_row)}) && engine_allocations.size() == 1);

    reset(); stopping = start("series.release-stop"); cache({address(native_row)}); stop_in_release = true;
    CHECK(hold(stopping) == TSUCHI_NOT_READY);
    CHECK(series_holds.count() == 0 && owned_count(stopping) == 0 && entries({address(native_row)}));
    reset();
    std::printf("content series: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
