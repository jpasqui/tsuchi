#include "platform.h"

#include "select_build.h"
#include "game.h"

namespace tsuchi::target {
namespace {

// What the system reports about the region an address is in.
struct MemoryInfo {
    uint64_t base, size;
    uint32_t state, attribute, permission, ipc_count, device_count, padding;
};
static_assert(sizeof(MemoryInfo) == 40, "the system's memory query result");
enum : uint32_t { kRead = 1, kWrite = 2, kExecute = 4 };

uint64_t query(MemoryInfo* info, uint64_t address) {
    register uint64_t result asm("x0") = reinterpret_cast<uint64_t>(info);
    register uint64_t at asm("x2") = address;
    asm volatile("svc #6" : "+r"(result) : "r"(at) : "x1", "x3", "memory");
    return result;
}

uint64_t game_base = 0;

// Readable regions the system has reported this frame. A query is a system
// call and services read thousands of fields a frame, so answers are reused
// until new_frame(). The list is one thread's: the thread that last called
// new_frame(), which is the game's. A read on any other thread, as in a hook
// the game calls from a worker, asks the system every time and leaves the
// list alone.
struct Region {
    uint64_t start, end;
};
Region known[16];
unsigned next_known = 0;
uint64_t known_thread = 0;

bool mapped(uint64_t address, uint64_t bytes, uint32_t need, bool reuse) {
    if (!address || bytes > ~uint64_t(0) - address) return false;
    const uint64_t end = address + bytes;
    reuse = reuse && current_thread() == known_thread;
    if (reuse) {
        for (const Region& region : known)
            if (address >= region.start && end <= region.end) return true;
    }
    while (address < end) {
        MemoryInfo info{};
        if (query(&info, address) || (info.permission & need) != need) return false;
        if (info.base > address || info.size > ~uint64_t(0) - info.base) return false;
        const uint64_t next = info.base + info.size;
        if (next <= address) return false;
        if (reuse) known[next_known++ % 16] = {info.base, next};
        address = next;
    }
    return true;
}

uint64_t hash_16_bytes(uint64_t address) {
    uint64_t hash = 0xCBF29CE484222325ull;
    for (unsigned i = 0; i < 16; ++i) hash = (hash ^ *reinterpret_cast<const unsigned char*>(address + i)) * 0x100000001B3ull;
    return hash;
}

} // namespace

bool find_game(uint64_t own_base) {
    namespace t = game::table;
    uint64_t address = 0;
    for (unsigned region = 0; region < 4096; ++region) {
        MemoryInfo info{};
        if (query(&info, address) || !info.size) break;
        const uint64_t end = info.base + info.size;
        if (end <= address) break;
        if (info.permission == (kRead | kExecute) && info.base != own_base) {
            const int build = t::find_build(info.size, [&](uint64_t offset) { return hash_16_bytes(info.base + offset); });
            if (build >= 0) {
                select_build(unsigned(build));
                game_base = info.base;
                return true;
            }
        }
        address = end;
    }
    return false;
}

bool query_region(uint64_t address, uint64_t& base, uint64_t& size, uint32_t& permission) {
    MemoryInfo info{};
    if (query(&info, address)) return false;
    base = info.base, size = info.size, permission = info.permission;
    return true;
}

uint64_t current_thread() {
    uint64_t own;
    asm volatile("mrs %0, tpidrro_el0" : "=r"(own));
    return own;
}

void new_frame() {
    known_thread = current_thread();
    for (Region& region : known) region = {};
}

bool executable(uint64_t address) { return mapped(address, 4, kRead | kExecute, false); }

void debug_output(const char* text, uint64_t length) {
    register uint64_t at asm("x0") = reinterpret_cast<uint64_t>(text);
    register uint64_t size asm("x1") = length;
    asm volatile("svc #0x27" : "+r"(at) : "r"(size) : "x2", "x3", "memory");
}

bool process_regions(uint64_t heap_alias_aslr[3]) {
    // GetInfo of this process: 4 is where the heap region starts, 2 the alias region, 12 the ASLR region.
    const uint32_t kinds[3] = {4, 2, 12};
    for (unsigned i = 0; i < 3; ++i) {
        register uint64_t result asm("x0") = 0;
        register uint64_t value asm("x1") = kinds[i];
        register uint64_t handle asm("x2") = 0xffff8001u; // this process
        register uint64_t sub asm("x3") = 0;
        asm volatile("svc #0x29" : "+r"(result), "+r"(value) : "r"(handle), "r"(sub) : "memory");
        if (result) return false;
        heap_alias_aslr[i] = value;
    }
    return true;
}

} // namespace tsuchi::target

namespace tsuchi::game {

uint64_t main_base() { return target::game_base; }
bool executable(uint64_t address) { return target::executable(address); }
bool readable(uint64_t address, uint64_t bytes) { return target::mapped(address, bytes, target::kRead, true); }
// Writes ask the system every time: permissions can change within a frame.
bool writable(uint64_t address, uint64_t bytes) { return target::mapped(address, bytes, target::kRead | target::kWrite, false); }

} // namespace tsuchi::game
