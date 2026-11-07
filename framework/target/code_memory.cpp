// Writable aliases of main code and an owned near-main gateway arena. Existing
// RX mappings keep their permissions. Handles and mappings are created on demand.
#include "platform.h"
#include "game.h"
#include "native_hooks.h"
#include "native_codegen.h"
#include "reflect.h"
#include <string.h>

#if defined(TSUCHI_NATIVE_CODE_ACCESS)
extern "C" __attribute__((used)) const uint32_t tsuchi_native_code_access = 1;
#endif

namespace tsuchi::target {
#if defined(TSUCHI_NATIVE_CODE_ACCESS)
namespace {
constexpr uint64_t kPage = 4096;
constexpr uint32_t kSelf = 0xffff8001u;
uint32_t last_result = 0;
struct MemoryInfo {
    uint64_t base, size;
    uint32_t state, attribute, permission, ipc_count, device_count, padding;
};
static_assert(sizeof(MemoryInfo) == 40, "SVC memory information");

bool query(uint64_t address, MemoryInfo& info) {
    register uint64_t r0 asm("x0") = reinterpret_cast<uint64_t>(&info);
    register uint64_t r2 asm("x2") = address;
    asm volatile("svc #6" : "+r"(r0) : "r"(r2) : "x1", "x3", "memory");
    last_result = uint32_t(r0);
    return r0 == 0;
}
bool information(uint32_t kind, uint32_t handle, uint64_t& value) {
    register uint64_t r0 asm("x0") = 0;
    register uint64_t r1 asm("x1") = kind;
    register uint64_t r2 asm("x2") = handle;
    register uint64_t r3 asm("x3") = 0;
    asm volatile("svc #0x29" : "+r"(r0), "+r"(r1) : "r"(r2), "r"(r3) : "memory");
    last_result = uint32_t(r0);
    value = r1;
    return r0 == 0;
}
void close_handle(uint32_t handle) {
    if (!handle) return;
    register uint64_t r0 asm("x0") = handle;
    asm volatile("svc #0x16" : "+r"(r0) : : "memory");
}
uint32_t* ipc_buffer() {
    uint64_t address;
    asm volatile("mrs %0, tpidrro_el0" : "=r"(address));
    return reinterpret_cast<uint32_t*>(address);
}

uint32_t process = 0, server = 0;
bool process_attempted = false;
alignas(4096) uint8_t receiver_stack[16384];
[[noreturn]] void receive_handle(void*) {
    memset(ipc_buffer(), 0, 0x100);
    register uint64_t r0 asm("x0") = 0;
    register uint64_t r1 asm("x1") = reinterpret_cast<uint64_t>(&server);
    register uint64_t r2 asm("x2") = 1;
    register uint64_t r3 asm("x3") = 0;
    register uint64_t r4 asm("x4") = 2000000000;
    asm volatile("svc #0x43" : "+r"(r0), "+r"(r1) : "r"(r2), "r"(r3), "r"(r4) : "memory");
    if (!r0) {
        const uint32_t* message = ipc_buffer();
        if ((message[1] & 0x80000000u) && message[2] == 2 && message[3] && message[3] != kSelf)
            __atomic_store_n(&process, message[3], __ATOMIC_RELEASE);
    }
    close_handle(server); // closing the session wakes the sender
    asm volatile("svc #0x0a" : : : "memory");
    __builtin_unreachable();
}

// A pseudo process handle can't be passed to MapProcessMemory. A self IPC
// session converts it to a real handle when the kernel extension isn't present.
uint32_t process_handle() {
    uint32_t handle = __atomic_load_n(&process, __ATOMIC_ACQUIRE);
    if (handle || process_attempted) return handle;
    process_attempted = true;
    uint64_t value = 0;
    if (information(65001, 0, value) && value && value <= 0xffffffffu && value != kSelf) {
        process = uint32_t(value);
        return process;
    }
    register uint64_t r0 asm("x0") = 0;
    register uint64_t r1 asm("x1") = 0;
    register uint64_t r2 asm("x2") = 0;
    register uint64_t r3 asm("x3") = 0;
    asm volatile("svc #0x40" : "+r"(r0), "+r"(r1), "+r"(r2) : "r"(r3) : "memory");
    last_result = uint32_t(r0);
    if (r0) return 0;
    server = uint32_t(r1);
    const uint32_t client = uint32_t(r2);
    r0 = 0;
    r1 = reinterpret_cast<uint64_t>(&receive_handle);
    r2 = 0;
    r3 = reinterpret_cast<uint64_t>(receiver_stack + sizeof receiver_stack);
    register uint64_t r4 asm("x4") = 0x2c;
    register uint64_t r5 asm("x5") = uint64_t(-2);
    asm volatile("svc #8" : "+r"(r0), "+r"(r1) : "r"(r2), "r"(r3), "r"(r4), "r"(r5) : "memory");
    last_result = uint32_t(r0);
    if (r0) { close_handle(server); close_handle(client); return 0; }
    const uint32_t thread = uint32_t(r1);
    r0 = thread;
    asm volatile("svc #9" : "+r"(r0) : : "memory");
    last_result = uint32_t(r0);
    if (r0) { close_handle(thread); close_handle(server); close_handle(client); return 0; }
    uint8_t saved[0x100];
    memcpy(saved, ipc_buffer(), sizeof saved);
    memset(ipc_buffer(), 0, sizeof saved);
    const uint32_t message[] = {0, 0x80000000u, 2, kSelf};
    memcpy(ipc_buffer(), message, sizeof message);
    r0 = client;
    asm volatile("svc #0x21" : "+r"(r0) : : "memory");
    last_result = uint32_t(r0);
    memcpy(ipc_buffer(), saved, sizeof saved);
    close_handle(client);
    r0 = 0;
    r1 = reinterpret_cast<uint64_t>(&thread);
    r2 = 1;
    r3 = 2000000000;
    asm volatile("svc #0x18" : "+r"(r0), "+r"(r1) : "r"(r2), "r"(r3) : "memory");
    last_result = uint32_t(r0);
    // The stack remains alive even if the wait was refused or timed out.
    close_handle(thread);
    return __atomic_load_n(&process, __ATOMIC_ACQUIRE);
}

struct Alias { uint64_t original, writable; };
Alias aliases[512];
unsigned alias_count = 0;
uint64_t search = 0, search_end = 0;
struct Region { uint64_t start, end; };
Region excluded[2]{};
Region address_space{};
// Guards remain unoccupied by this backend. A successful mapping, rather than
// a memory query, claims the pages; other allocators may still race with setup.
Region arena_windows[2]{};
CodeArena arena{};
void* arena_allocation = nullptr;
uint64_t arena_source = 0;
bool code_mapped = false, alias_mapped = false;
bool arena_setup = false;
CodeMemoryStats diagnostic{};

void phase(CodeMemoryPhase value) {
    diagnostic.phase = uint32_t(value);
    last_result = 0;
}

bool region(uint32_t kind, Region& out) {
    uint64_t start, size;
    if (!information(kind, kSelf, start) || !information(kind + 1, kSelf, size) ||
        ((start | size) & (kPage - 1)) || size > ~uint64_t(0) - start) return false;
    out = {start, start + size};
    return true;
}

bool placement_start() {
    Region aslr, heap, alias;
    if (!region(12, aslr) || aslr.start == aslr.end ||
        !region(4, heap) || !region(2, alias)) return false;
    excluded[0] = heap;
    excluded[1] = alias;
    address_space = aslr;
    search = aslr.start;
    search_end = aslr.end;
    return true;
}

bool checked_query(uint64_t at, MemoryInfo& info, unsigned& queries) {
    if (queries == 4096) { last_result = 0; return false; }
    ++queries;
    if (!query(at, info)) return false;
    return info.size && info.base <= at && !((info.base | info.size) & (kPage - 1)) &&
           info.size <= ~uint64_t(0) - info.base && info.base + info.size > at;
}

bool overlaps(uint64_t at, uint64_t bytes, const Region& region) {
    return region.start != region.end && at < region.end && region.start < at + bytes;
}

// Finds one wholly free window without entering reserved regions. The caller
// owns the search cursor and a shared query budget, so mapping retries are bounded.
bool free_window(uint64_t& cursor, uint64_t limit, uint64_t bytes, unsigned& queries) {
    while (cursor < limit && bytes <= limit - cursor && queries < 4096) {
        bool skipped = false;
        for (const Region& reserved : excluded) if (overlaps(cursor, bytes, reserved)) {
            cursor = reserved.end;
            skipped = true;
            break;
        }
        if (skipped) continue;
        for (const Region& reserved : arena_windows) if (overlaps(cursor, bytes, reserved)) {
            cursor = reserved.end;
            skipped = true;
            break;
        }
        if (skipped) continue;
        MemoryInfo info{};
        if (!checked_query(cursor, info, queries)) return false;
        const uint64_t end = info.base + info.size;
        if ((info.state & 0xff) == 0 && bytes <= end - cursor) return true;
        cursor = end;
    }
    last_result = 0;
    return false;
}

uint32_t map_process(uint64_t destination, uint32_t handle, uint64_t source, uint64_t bytes, bool unmap = false) {
    register uint64_t r0 asm("x0") = destination;
    register uint64_t r1 asm("x1") = handle;
    register uint64_t r2 asm("x2") = source;
    register uint64_t r3 asm("x3") = bytes;
    if (unmap) asm volatile("svc #0x75" : "+r"(r0) : "r"(r1), "r"(r2), "r"(r3) : "memory");
    else asm volatile("svc #0x74" : "+r"(r0) : "r"(r1), "r"(r2), "r"(r3) : "memory");
    return last_result = uint32_t(r0);
}

uint32_t map_code(uint32_t handle, uint64_t destination, uint64_t source, uint64_t bytes, bool unmap = false) {
    register uint64_t r0 asm("x0") = handle;
    register uint64_t r1 asm("x1") = destination;
    register uint64_t r2 asm("x2") = source;
    register uint64_t r3 asm("x3") = bytes;
    if (unmap) asm volatile("svc #0x78" : "+r"(r0) : "r"(r1), "r"(r2), "r"(r3) : "memory");
    else asm volatile("svc #0x77" : "+r"(r0) : "r"(r1), "r"(r2), "r"(r3) : "memory");
    return last_result = uint32_t(r0);
}

uint32_t set_rx(uint32_t handle, uint64_t address, uint64_t bytes) {
    register uint64_t r0 asm("x0") = handle;
    register uint64_t r1 asm("x1") = address;
    register uint64_t r2 asm("x2") = bytes;
    register uint64_t r3 asm("x3") = 5;
    asm volatile("svc #0x73" : "+r"(r0) : "r"(r1), "r"(r2), "r"(r3) : "memory");
    return last_result = uint32_t(r0);
}

void* alias_code(uint64_t address, uint64_t bytes) {
    const uint64_t base = game::main_base(), offset = address - base;
    if (!base || address < base || !bytes || offset >= game::table::constant::code_size ||
        bytes > game::table::constant::code_size - offset || !game::executable(address)) return nullptr;
    const uint64_t page = address & ~(kPage - 1), within = address - page;
    if (bytes > kPage - within) return nullptr;
    for (unsigned i = 0; i < alias_count; ++i)
        if (aliases[i].original == page) return reinterpret_cast<void*>(aliases[i].writable + within);
    if (alias_count == sizeof aliases / sizeof *aliases) return nullptr;
    const uint32_t handle = process_handle();
    if (!handle) return nullptr;
    if (!search_end && !placement_start()) return nullptr;
    unsigned queries = 0;
    for (unsigned attempts = 0; attempts < 16 && free_window(search, search_end, kPage, queries); ++attempts) {
        const uint64_t candidate = search;
        search += kPage;
        if (!map_process(candidate, handle, page, kPage)) {
            aliases[alias_count++] = {page, candidate};
            return reinterpret_cast<void*>(candidate + within);
        }
    }
    return nullptr;
}

bool mapped_range(uint64_t at, uint64_t bytes, uint32_t type, uint32_t permission, unsigned& queries) {
    const uint64_t end = at + bytes;
    while (at < end) {
        MemoryInfo info{};
        if (!checked_query(at, info, queries)) return false;
        const uint32_t state = info.state & 0xff;
        if ((state != type && !(type == 8 && state == 9)) || info.permission != permission ||
            info.attribute || info.ipc_count || info.device_count) return false;
        at = info.base + info.size;
    }
    return true;
}

bool near_window(Region& window) {
    const uint64_t main = game::main_base(), size = game::table::constant::code_size;
    constexpr uint64_t reach = (1ull << 27) - 4;
    constexpr uint64_t bytes = kNativeArenaBytes + 2 * kPage;
    if (!main || (main & 3) || size < 4 || (size & 3) || size > ~uint64_t(0) - main ||
        game::table::stub::native_shared >= size || address_space.end - address_space.start < bytes) return false;
    const uint64_t last = main + size;
    uint64_t low = last > reach ? last - reach : 0;
    if (low < address_space.start + kPage) low = address_space.start + kPage;
    uint64_t high = main > ~uint64_t(0) - reach ? ~uint64_t(0) : main + reach;
    if (high < kNativeArenaBytes - 4) return false;
    high -= kNativeArenaBytes - 4;
    const uint64_t available = address_space.end - kNativeArenaBytes - kPage;
    if (high > available) high = available;
    if (low > high) return false;
    low = (low + kPage - 1) & ~(kPage - 1);
    high &= ~(kPage - 1);
    if (low > high) return false;
    window = {low - kPage, high + kNativeArenaBytes + kPage};
    return true;
}

bool reachable(uint64_t rx) {
    const uint64_t main = game::main_base(), end = main + game::table::constant::code_size;
    const uint64_t last = rx + kNativeArenaBytes - 4;
    const uint64_t shared = main + game::table::stub::native_shared;
    uint32_t word;
    return native_code::branch(main, last, word) && native_code::branch(end - 4, rx, word) &&
           native_code::branch(rx, end, word) && native_code::branch(last, main, word) &&
           native_code::branch(rx + 4, shared, word, true) && native_code::branch(last, shared, word, true);
}

bool refuse_arena(uint32_t handle) {
    diagnostic.result = last_result;
    if (alias_mapped) {
        const uint32_t result = map_process(reinterpret_cast<uint64_t>(arena.rw), handle, arena.rx, kNativeArenaBytes, true);
        if (result) {
            diagnostic.phase = uint32_t(CodeMemoryPhase::rollback_alias);
            diagnostic.rollback_result = result;
            diagnostic.arena_state = 3;
            return false;
        }
        alias_mapped = false;
        arena.rw = nullptr;
        arena_windows[1] = {};
    }
    if (code_mapped) {
        const uint32_t result = map_code(handle, arena.rx, arena_source, kNativeArenaBytes, true);
        if (result) {
            diagnostic.phase = uint32_t(CodeMemoryPhase::rollback_code);
            diagnostic.rollback_result = result;
            diagnostic.arena_state = 3;
            return false;
        }
        code_mapped = false;
    }
    if (arena_allocation) game::release_engine_memory(arena_allocation);
    arena_allocation = nullptr;
    arena_source = 0;
    arena = {};
    arena_windows[0] = {};
    arena_windows[1] = {};
    diagnostic.arena_state = 2;
    return false;
}

void clean_data(void* alias, uint64_t bytes);
void flush_code(void* alias, uint64_t executable, uint64_t bytes);

bool gateway_arena(CodeArena* out) {
    if (!out) return false;
    *out = {};
    if (diagnostic.arena_state == 1) { *out = arena; return true; }
    if (diagnostic.arena_state == 3) return false;
    // Allocator hooks may reenter on the game thread, including during rollback.
    // A nested request leaves the outer attempt's ownership and diagnostics alone.
    if (arena_setup) return false;
    struct Setup {
        Setup() { arena_setup = true; }
        ~Setup() { arena_setup = false; }
    } setup;
    diagnostic = {};
    phase(CodeMemoryPhase::handle);
    const uint32_t handle = process_handle();
    if (!handle) return refuse_arena(0);
    phase(CodeMemoryPhase::allocate);
    constexpr uint64_t allocation_bytes = kNativeArenaBytes + kPage - 1;
    arena_allocation = game::engine_memory(allocation_bytes);
    if (!arena_allocation) return refuse_arena(handle);
    const uint64_t allocation = reinterpret_cast<uint64_t>(arena_allocation);
    phase(CodeMemoryPhase::backing);
    if (allocation_bytes > ~uint64_t(0) - allocation) return refuse_arena(handle);
    arena_source = (allocation + kPage - 1) & ~(kPage - 1);
    unsigned queries = 0, attempts = 0;
    if (!mapped_range(arena_source, kNativeArenaBytes, 5, 3, queries)) return refuse_arena(handle);
    memset(reinterpret_cast<void*>(arena_source), 0, kNativeArenaBytes);
    clean_data(reinterpret_cast<void*>(arena_source), kNativeArenaBytes);
    phase(CodeMemoryPhase::placement);
    if (!search_end && !placement_start()) return refuse_arena(handle);
    Region near{};
    if (!near_window(near)) return refuse_arena(handle);
    uint64_t cursor = near.start;
    constexpr uint64_t window_bytes = kNativeArenaBytes + 2 * kPage;
    while (attempts < 16) {
        phase(CodeMemoryPhase::placement);
        if (!free_window(cursor, near.end, window_bytes, queries)) return refuse_arena(handle);
        const uint64_t rx = cursor + kPage;
        if (!reachable(rx)) return refuse_arena(handle);
        phase(CodeMemoryPhase::map_code);
        ++attempts;
        if (!map_code(handle, rx, arena_source, kNativeArenaBytes)) {
            arena = {rx, nullptr, kNativeArenaBytes};
            code_mapped = true;
            arena_windows[0] = {cursor, cursor + window_bytes};
            break;
        }
        cursor += kPage;
    }
    if (!code_mapped) return refuse_arena(handle);
    phase(CodeMemoryPhase::set_rx);
    if (set_rx(handle, arena.rx, kNativeArenaBytes)) return refuse_arena(handle);
    cursor = address_space.start;
    phase(CodeMemoryPhase::map_alias);
    while (attempts < 16) {
        if (!free_window(cursor, address_space.end, window_bytes, queries)) return refuse_arena(handle);
        const uint64_t rw = cursor + kPage;
        ++attempts;
        if (!map_process(rw, handle, arena.rx, kNativeArenaBytes)) {
            arena.rw = reinterpret_cast<void*>(rw);
            alias_mapped = true;
            arena_windows[1] = {cursor, cursor + window_bytes};
            break;
        }
        cursor += kPage;
    }
    if (!alias_mapped) return refuse_arena(handle);
    phase(CodeMemoryPhase::verify_rx);
    if (!mapped_range(arena.rx, kNativeArenaBytes, 8, 5, queries)) return refuse_arena(handle);
    phase(CodeMemoryPhase::verify_rw);
    if (!mapped_range(reinterpret_cast<uint64_t>(arena.rw), kNativeArenaBytes, 15, 3, queries)) return refuse_arena(handle);
    auto* writable = static_cast<volatile uint32_t*>(arena.rw);
    const auto* executable = reinterpret_cast<const volatile uint32_t*>(arena.rx);
    if (*writable || *executable) return refuse_arena(handle);
    *writable = 0xd503201fu;
    flush_code(arena.rw, arena.rx, 4);
    const bool mirrored = *executable == 0xd503201fu;
    *writable = 0;
    flush_code(arena.rw, arena.rx, 4);
    if (!mirrored) return refuse_arena(handle);
    flush_code(arena.rw, arena.rx, kNativeArenaBytes);
    diagnostic.arena_state = 1;
    diagnostic.result = 0;
    phase(CodeMemoryPhase::ready);
    *out = arena;
    return true;
}

void memory_stats(CodeMemoryStats* out) {
    if (!out) return;
    *out = diagnostic;
    out->alias_count = alias_count;
    out->alias_limit = sizeof aliases / sizeof *aliases;
    out->arena_rx = arena.rx;
    out->arena_rw = reinterpret_cast<uint64_t>(arena.rw);
    out->arena_bytes = arena.bytes;
}

void clean_data(void* alias, uint64_t bytes) {
    uint64_t ctr;
    asm volatile("mrs %0, ctr_el0" : "=r"(ctr));
    const uint64_t data_line = 4ull << ((ctr >> 16) & 15);
    const uint64_t writable = reinterpret_cast<uint64_t>(alias);
    for (uint64_t at = writable & ~(data_line - 1); at < writable + bytes; at += data_line)
        asm volatile("dc cvau, %0" : : "r"(at) : "memory");
    asm volatile("dsb ish" : : : "memory");
}

void flush_code(void* alias, uint64_t executable, uint64_t bytes) {
    clean_data(alias, bytes);
    uint64_t ctr;
    asm volatile("mrs %0, ctr_el0" : "=r"(ctr));
    const uint64_t instruction_line = 4ull << (ctr & 15);
    for (uint64_t at = executable & ~(instruction_line - 1); at < executable + bytes; at += instruction_line)
        asm volatile("ic ivau, %0" : : "r"(at) : "memory");
    asm volatile("dsb ish\nisb" : : : "memory");
}
} // namespace

void code_memory_start() { native_hooks_start({alias_code, flush_code, gateway_arena, memory_stats}); }
#else
namespace {
void disabled_stats(CodeMemoryStats* out) {
    if (out) *out = {0, 512, 4, uint32_t(CodeMemoryPhase::disabled), 0, 0, 0, 0, 0};
}
}
void code_memory_start() { native_hooks_start({nullptr, nullptr, nullptr, disabled_stats}); }
#endif

} // namespace tsuchi::target
