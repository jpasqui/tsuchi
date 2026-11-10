#include "container_live.h"
#include "game.h"
#include <cassert>
#include <cstring>
#include <vector>
using namespace tsuchi;
namespace t = game::table;
namespace io = t::container_io;
namespace m = t::container_map;
namespace {
alignas(8) unsigned char dispatcher[16]{}, impl[0x500]{}, reader[0x100]{}, store[0x100]{};
uint64_t global_dispatcher = reinterpret_cast<uint64_t>(dispatcher);
uint64_t readers[2] = {reinterpret_cast<uint64_t>(reader), 0};
struct Row { uint64_t key, value; int32_t next, hash; } rows[2]{}, moved[2]{};
struct Range { uint64_t at, bytes; bool write; };
std::vector<Range> ranges;
std::vector<uint64_t> held;
unsigned flushes = 0;
template<class T> void put(unsigned char* memory, uint64_t at, T value) { memcpy(memory + at, &value, sizeof value); }
void lock(void* at) {
    const uint64_t address = reinterpret_cast<uint64_t>(at);
    if (held.empty()) assert(address == reinterpret_cast<uint64_t>(impl) + io::backend + io::lock);
    else assert(held.size() == 1 && address == reinterpret_cast<uint64_t>(store) + io::package_lock);
    held.push_back(address);
}
void unlock(void* at) { assert(!held.empty() && held.back() == reinterpret_cast<uint64_t>(at)); held.pop_back(); }
void flush(int32_t request) { assert(request == -1 && held.empty()); ++flushes; }
void add(void* memory, uint64_t bytes) { ranges.push_back({reinterpret_cast<uint64_t>(memory), bytes, true}); }
}
namespace tsuchi::game {
uint64_t main_base() { return reinterpret_cast<uint64_t>(&global_dispatcher) - uint64_t(t::data::io_dispatcher); }
bool readable(uint64_t at, uint64_t bytes) {
    for (const auto& r : ranges) if (at >= r.at && at - r.at <= r.bytes && bytes <= r.bytes - (at - r.at)) return true;
    return false;
}
bool writable(uint64_t at, uint64_t bytes) {
    for (const auto& r : ranges) if (r.write && at >= r.at && at - r.at <= r.bytes && bytes <= r.bytes - (at - r.at)) return true;
    return false;
}
bool executable(uint64_t) { return false; }
}
int main() {
    static_assert(sizeof(Row) == 24);
    add(dispatcher,sizeof dispatcher); add(impl,sizeof impl); add(reader,sizeof reader);
    add(store,sizeof store); add(readers,sizeof readers); add(rows,sizeof rows);
    add(moved,sizeof moved); add(&global_dispatcher,sizeof global_dispatcher);
    t::fn::lock_mutex = lock; t::fn::unlock_mutex = unlock; t::fn::flush_async_loading = flush;
    put(dispatcher, io::implementation, reinterpret_cast<uint64_t>(impl));
    put(impl, uint64_t(io::backend) + io::readers, reinterpret_cast<uint64_t>(readers));
    put(impl, uint64_t(io::backend) + io::readers_count, int32_t(1));
    put(reader, io::container_id, uint64_t(123));
    // An immutable populated reader TMap plus a cache/ownership index outside it.
    memset(reader + io::toc, 0x6a, size_t(io::toc_bytes));
    put(reader, uint64_t(io::toc) + m::count, int32_t(7));
    put(reader, uint64_t(io::toc) + m::free_count, int32_t(0));
    put(reader, 0xf8, uint32_t(45));
    unsigned char original[sizeof reader]; memcpy(original,reader,sizeof reader);
    rows[0] = {77, 900, -1, 0}; rows[1] = {78, 901, 0, 0};
    auto* map = store + io::package_map;
    put(map, 0, reinterpret_cast<uint64_t>(rows));
    put(map, m::count, int32_t(2)); put(map, m::free_count, int32_t(0));
    put(map, m::hash_inline, int32_t(1)); put(map, m::hash_data, uint64_t(0)); put(map, m::hash_count, uint32_t(1));
    ContainerReaderState state;
    ContainerLookup changes[2] = {{77,900,800},{78,901,0}};
    const auto change = [&](bool enabled) { return change_container_reader(123,reinterpret_cast<uint64_t>(store),state,enabled,changes,2); };
    assert(change(false) == TSUCHI_OK && state.detached && rows[0].value == 800 && rows[1].value == 0);
    assert(!memcmp(state.toc, original + io::toc, size_t(io::toc_bytes)));
    assert(game::read_or<int32_t>(reinterpret_cast<uint64_t>(reader) + io::toc + m::count) == 0);
    assert(game::read_or<int32_t>(reinterpret_cast<uint64_t>(reader) + io::toc + m::first_free) == -1);
    assert(!memcmp(reader + uint64_t(io::toc) + io::toc_bytes, original + uint64_t(io::toc) + io::toc_bytes,
                   sizeof reader - uint64_t(io::toc) - io::toc_bytes)); // pending read's index/file data stays intact
    assert(readers[0] == reinterpret_cast<uint64_t>(reader) && held.empty());
    changes[0] = {77,800,900}; changes[1] = {78,0,901};
    // A subsequent mount relocates the package map. No stale slot is reused.
    memcpy(moved,rows,sizeof rows); put(map,0,reinterpret_cast<uint64_t>(moved));
    assert(change(true) == TSUCHI_OK && !state.detached && moved[0].value == 900 && moved[1].value == 901);
    assert(rows[0].value == 800 && rows[1].value == 0 && !memcmp(reader,original,sizeof reader));
    // An unexpected writer/partial metadata failure is transactional.
    changes[0] = {77,900,800}; changes[1] = {78,999,0};
    assert(change(false) == TSUCHI_BUSY && !state.detached && moved[0].value == 900 && !memcmp(reader,original,sizeof reader));
    changes[1] = {78,901,0};
    for (auto& r : ranges) if (r.at == reinterpret_cast<uint64_t>(moved)) r.write = false;
    assert(change(false) == TSUCHI_BUSY && !state.detached && !memcmp(reader,original,sizeof reader));
    for (auto& r : ranges) r.write = true;
    // Duplicate container identities are refused without touching either reader.
    readers[1] = readers[0]; put(impl,uint64_t(io::backend) + io::readers_count,int32_t(2));
    assert(change(false) == TSUCHI_BUSY && !state.detached);
    put(impl,uint64_t(io::backend) + io::readers_count,int32_t(1));
    // Repeated off/on cycles restore exactly the original map, with no new reader
    // indices, mounts, allocations or deleted files.
    for (unsigned i=0;i<1000;++i) {
        changes[0] = {77,900,800}; changes[1] = {78,901,0}; assert(change(false) == TSUCHI_OK);
        changes[0] = {77,800,900}; changes[1] = {78,0,901}; assert(change(true) == TSUCHI_OK);
        assert(!memcmp(reader,original,sizeof reader) && held.empty());
    }
    const auto entry = t::data::io_dispatcher;
    const auto old = t::cells[entry.index]; t::cells[entry.index] = t::absent_value;
    const unsigned before = flushes;
    assert(change(false) == TSUCHI_UNSUPPORTED && flushes == before && !state.detached);
    t::cells[entry.index] = old;
    assert(!memcmp(reader,original,sizeof reader) && held.empty());
}
