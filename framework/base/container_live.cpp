#include "container_live.h"
#include "game.h"
#include <string.h>
namespace tsuchi {
namespace {
namespace t = game::table;
// Find the current slot by key. Native TMap storage can move on a later mount;
// a pointer captured by the mount callback must never be reused as a slot.
uint64_t package_slot(uint64_t map, uint64_t key) {
    namespace m = t::container_map;
    const int32_t count = game::read_or<int32_t>(map + m::count, -1);
    const int32_t free = game::read_or<int32_t>(map + m::free_count, -1);
    const uint32_t buckets = game::read_or<uint32_t>(map + m::hash_count);
    if (count <= free || free < 0 || count > 4000000 || !buckets || (buckets & (buckets - 1))) return 0;
    const uint64_t data = game::read_or<uint64_t>(map);
    const uint64_t hash = game::read_or<uint64_t>(map + m::hash_data, map + m::hash_inline);
    const uint64_t hashes = hash ? hash : map + m::hash_inline;
    int32_t index = game::read_or<int32_t>(hashes + (uint32_t(key) & (buckets - 1)) * 4, -1);
    for (int32_t visited = 0; index >= 0 && index < count && visited < count; ++visited) {
        const uint64_t entry = data + uint64_t(index) * m::entry_bytes;
        if (game::read_or<uint64_t>(entry, ~key) == key) return entry + m::value;
        index = game::read_or<int32_t>(entry + m::next, -1);
    }
    return 0;
}
}
TSUCHI_Result change_container_reader(uint64_t container, uint64_t store,
    ContainerReaderState& state, bool enabled, ContainerLookup* changes, uint32_t count) {
    namespace io = t::container_io;
    namespace m = t::container_map;
    if (!store || !count || !changes || !t::present(t::data::io_dispatcher) ||
        !t::bound(t::fn::lock_mutex) || !t::bound(t::fn::unlock_mutex) ||
        uint64_t(io::toc_bytes) > sizeof state.toc) return TSUCHI_UNSUPPORTED;
    // The caller has flushed loading and checked package-object lifetime. Do not
    // flush again here: that could run callbacks creating new package objects
    // after its guard. No game callback runs between that guard and these locks.
    const uint64_t dispatcher = game::global<uint64_t>(t::data::io_dispatcher);
    const uint64_t impl = game::read_or<uint64_t>(dispatcher + io::implementation);
    if (!impl) return TSUCHI_NOT_READY;
    const uint64_t backend = impl + io::backend;
    const uint64_t lock = backend + io::lock, package_lock = store + io::package_lock;
    if (!game::writable(lock, io::lock_bytes) || !game::writable(package_lock, io::lock_bytes)) return TSUCHI_NOT_READY;
    t::fn::lock_mutex(reinterpret_cast<void*>(lock));
    t::fn::lock_mutex(reinterpret_cast<void*>(package_lock));
    struct Unlock {
        uint64_t io, packages;
        ~Unlock() { t::fn::unlock_mutex(reinterpret_cast<void*>(packages)); t::fn::unlock_mutex(reinterpret_cast<void*>(io)); }
    } unlock{lock, package_lock};
    uint64_t reader = 0;
    const int32_t readers = game::read_or<int32_t>(backend + io::readers_count, -1);
    const uint64_t data = game::read_or<uint64_t>(backend + io::readers);
    if (readers < 0 || readers > 65536 || !game::readable(data, uint64_t(readers) * 8)) return TSUCHI_NOT_READY;
    for (int32_t i = 0; i < readers; ++i) {
        const uint64_t candidate = game::read_or<uint64_t>(data + uint64_t(i) * 8);
        if (candidate && game::read_or<uint64_t>(candidate + io::container_id, ~container) == container) {
            if (reader) return TSUCHI_BUSY; // duplicate IDs are ambiguous
            reader = candidate;
        }
    }
    if (!reader || (state.reader && state.reader != reader)) return TSUCHI_NOT_READY;
    const uint64_t toc = reader + io::toc;
    if (!game::writable(toc, io::toc_bytes)) return TSUCHI_NOT_READY;
    if (enabled == !state.detached) return TSUCHI_OK;
    // Preflight every write while both native locks are held. A foreign publisher
    // changing metadata causes refusal, never a partly retired archive.
    for (uint32_t i = 0; i < count; ++i) {
        auto& change = changes[i];
        change.slot = package_slot(store + io::package_map, change.package);
        if (!change.slot || !game::writable(change.slot, 8) ||
            game::read_or<uint64_t>(change.slot, ~change.expected) != change.expected) return TSUCHI_BUSY;
    }
    if (enabled) game::write_bytes(toc, state.toc, io::toc_bytes);
    else {
        if (!game::read_bytes(toc, state.toc, io::toc_bytes)) return TSUCHI_NOT_READY;
        // The exact empty-map state produced by FFileIoStoreReader's constructor.
        // Swapping the whole map preserves its sparse array, bits and hash chains
        // together. No reader is deleted or renumbered, and no cache key is reused.
        alignas(8) uint8_t empty[128]{};
        const int32_t first_free = -1, inline_bits = 128;
        memcpy(empty + m::first_free, &first_free, 4);
        memcpy(empty + m::bits_capacity, &inline_bits, 4);
        game::write_bytes(toc, empty, io::toc_bytes);
    }
    for (uint32_t i = 0; i < count; ++i) game::write(changes[i].slot, changes[i].replacement);
    state.reader = reader;
    state.detached = !enabled;
    return TSUCHI_OK;
}
}
