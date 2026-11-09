#include "dynamic_hooks.h"

#include <string.h>

#include "frame.h"
#include "game.h"
#include "hooks.h"
#include "registry.h"

namespace tsuchi {
namespace {

const Kind kDynamic{"dynamic hook"};

// A watch's records, between the calls that fill them on any thread and the game thread that sends them. Each
// cell's sequence says what it holds: equal to a ticket, it's free for that ticket's writer; one past it, it's
// published for the reader; the reader then sets it a lap ahead, for the writer of the next lap.
struct Ring {
    struct Cell {
        uint64_t sequence;
        WatchRecord record;
    };
    Cell cells[kWatchRecords];
    uint64_t write; // the next ticket
    uint64_t read;  // the next ticket to send; the game thread's alone
    uint32_t dropped;
};

// Every member has an initializer, so the table below is constant-initialized: the module has no start-up code
// for globals (tools/build_module.py).
struct Record {
    uint64_t id = 0;       // 0 marks a free gateway
    uint64_t engine = 0;   // the hook engine's id for it
    uint64_t original = 0; // where the hook continues; written by the hook engine as it puts the hook on
    TSUCHI_Mod* mod = nullptr;
    DynamicKind kind = DynamicKind::wrap;
    bool live = false;   // read on any thread
    bool native = false; // put on through the native backend
    Signature signature;
    DynamicClient client;
    void* user = nullptr;
    Ring* ring = nullptr;
    uint32_t depth = 0; // client calls running; game thread only
};

Record records[kDynamicGateways];
uint64_t next_id = uint64_t(1) << 62; // native hooks use the low range; instruction patches use bit 63
DynamicPlatform platform{};
uint64_t frames_begun = 0;
uint64_t frame_stack = 0; // an address near the top of the game thread's stack, from the last engine frame

struct Counters {
    uint32_t calls = 0, off_thread = 0, depth = 0, not_ready = 0, failed = 0, dropped = 0;
} counters;
void count(uint32_t& counter) { __atomic_fetch_add(&counter, 1, __ATOMIC_RELAXED); }

// ---- Signatures -----------------------------------------------------------

struct TypeName {
    const char* name;
    CallType type;
};
const TypeName kTypes[] = {{"void", CallType::void_},   {"bool", CallType::boolean},  {"i32", CallType::i32},
                           {"u32", CallType::u32},      {"u64", CallType::u64},       {"pointer", CallType::pointer},
                           {"const_pointer", CallType::const_pointer}, {"f32", CallType::f32},
                           {"text16", CallType::text16}};

const char* skip_space(const char* at) {
    while (*at == ' ' || *at == '\t') ++at;
    return at;
}
size_t word_length(const char* at) {
    size_t length = 0;
    while ((at[length] >= 'a' && at[length] <= 'z') || (at[length] >= '0' && at[length] <= '9') || at[length] == '_') ++length;
    return length;
}
bool type_named(const char* at, size_t length, CallType& out) {
    for (const TypeName& type : kTypes)
        if (strlen(type.name) == length && !memcmp(type.name, at, length)) {
            out = type.type;
            return true;
        }
    return false;
}

uint64_t normalized(CallType type, uint64_t bits) {
    switch (type) {
    case CallType::void_: return 0;
    case CallType::boolean: return (bits & 0xff) != 0;
    case CallType::i32: return uint64_t(int64_t(int32_t(uint32_t(bits))));
    case CallType::u32:
    case CallType::f32: return bits & 0xffffffffull;
    default: return bits;
    }
}

// ---- Gateways -------------------------------------------------------------

using InlineHandler = void (*)(TSUCHI_InlineContext*);
void dispatch_inline(uint32_t gateway, TSUCHI_InlineContext* context);
template <unsigned N>
void inline_gateway(TSUCHI_InlineContext* context) {
    dispatch_inline(N, context);
}
InlineHandler inline_gateways[kDynamicGateways];
template <unsigned N>
struct FillInline {
    static void fill() {
        FillInline<N - 1>::fill();
        inline_gateways[N - 1] = &inline_gateway<N - 1>;
    }
};
template <>
struct FillInline<0> {
    static void fill() {}
};

#if defined(__aarch64__)
} // namespace
} // namespace tsuchi
// start.S: two instructions per gateway, and the call that hands a frame's registers to a function.
extern "C" char tsuchi_dynamic_gateways[], tsuchi_dynamic_gateways_end[];
extern "C" void tsuchi_dynamic_call(uint64_t target, tsuchi::CallFrame* frame);
namespace tsuchi {
namespace {
uint64_t arm_gateway(uint32_t number, bool) {
    const uint64_t first = reinterpret_cast<uint64_t>(tsuchi_dynamic_gateways);
    const uint64_t count = (reinterpret_cast<uint64_t>(tsuchi_dynamic_gateways_end) - first) / 8;
    return number < count && number < kDynamicGateways ? first + uint64_t(number) * 8 : 0;
}
void arm_call(uint64_t target, CallFrame* frame, bool) { tsuchi_dynamic_call(target, frame); }
#endif

bool gateways_ready = false;
void prepare_gateways() {
    if (gateways_ready) return;
    FillInline<kDynamicGateways>::fill();
#if defined(__aarch64__)
    if (!platform.gateway) platform = DynamicPlatform{kDynamicGateways, arm_gateway, arm_call};
#endif
    gateways_ready = true;
}

// ---- Dispatch -------------------------------------------------------------

// True when the hook's client may run for this call. Otherwise the call goes to the original, and is counted.
bool client_may_run(Record& record) {
    if (!known_game_thread_token()) {
        count(counters.off_thread);
        return false;
    }
    // A call that reaches a hook while a client runs, other than through its original, comes from inside the
    // client's own runtime, such as its allocator. The runtime can't be entered again from there.
    if (in_hook_client() || !__atomic_load_n(&record.live, __ATOMIC_ACQUIRE) || !mod_running(record.mod)) {
        count(counters.not_ready);
        return false;
    }
    if (record.depth >= kDynamicDepth) {
        count(counters.depth);
        return false;
    }
    if (record.client.ready && !record.client.ready(record.user)) {
        count(counters.not_ready);
        return false;
    }
    return true;
}

bool push(Ring& ring, const WatchRecord& record) {
    uint64_t ticket = __atomic_load_n(&ring.write, __ATOMIC_RELAXED);
    for (;;) {
        Ring::Cell& cell = ring.cells[ticket % kWatchRecords];
        const uint64_t sequence = __atomic_load_n(&cell.sequence, __ATOMIC_ACQUIRE);
        const int64_t ahead = int64_t(sequence - ticket);
        if (ahead == 0) {
            if (__atomic_compare_exchange_n(&ring.write, &ticket, ticket + 1, true, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {
                cell.record = record;
                __atomic_store_n(&cell.sequence, ticket + 1, __ATOMIC_RELEASE);
                return true;
            }
            // `ticket` now holds the write count another writer left.
        } else if (ahead < 0) {
            // The reader hasn't sent this cell's record from the lap before: the ring is full.
            __atomic_fetch_add(&ring.dropped, 1, __ATOMIC_RELAXED);
            count(counters.dropped);
            return false;
        } else {
            ticket = __atomic_load_n(&ring.write, __ATOMIC_RELAXED);
        }
    }
}

// Copies a text16 argument into the record's buffer, up to its terminator or the buffer's end.
void copy_text(WatchRecord& record, uint32_t argument, uint64_t address, uint16_t& used) {
    record.text_start[argument] = used;
    const uint32_t units = read_text16(address, record.text + used, kWatchTextUnits - used);
    used = uint16_t(used + units);
    record.text_units[argument] = uint16_t(units);
}

void (*frame_work[4])() = {};

void call_original(uint64_t target, CallFrame& frame, CallType result);

void watch_call(Record& record, CallFrame& frame, uint64_t original) {
    WatchRecord watched{};
    const Signature& signature = record.signature;
    uint16_t used = 0;
    for (uint32_t i = 0; i < signature.count; ++i) {
        watched.args[i] = frame_argument(frame, signature, i);
        // The game's function may free or change the text, so it's copied before the call.
        if (signature.args[i] == CallType::text16) copy_text(watched, i, watched.args[i], used);
    }
    call_original(original, frame, signature.result);
    watched.result = frame_result(frame, signature.result);
    watched.game_thread = known_game_thread_token() != 0;
    if (record.ring) push(*record.ring, watched);
}

void dispatch_inline(uint32_t gateway, TSUCHI_InlineContext* context) {
    Record& record = records[gateway];
    const auto next = reinterpret_cast<InlineHandler>(__atomic_load_n(&record.original, __ATOMIC_ACQUIRE));
    count(counters.calls);
    if (client_may_run(record)) {
        InlineRegisters registers;
        memcpy(registers.x, context->registers, sizeof registers.x);
        registers.nzcv = context->nzcv & 0xf0000000ull;
        ++record.depth;
        hook_client_enter();
        const bool done = record.client.inline_call(record.user, registers);
        hook_client_leave();
        --record.depth;
        if (done) {
            memcpy(context->registers, registers.x, sizeof registers.x);
            context->nzcv = registers.nzcv & 0xf0000000ull;
        } else {
            count(counters.failed);
        }
    }
    // The rest of the chain, exactly once, and then the displaced instruction.
    if (next) next(context);
}

// ---- Ending ---------------------------------------------------------------

void end_dynamic(void*, uint64_t id) {
    for (Record& record : records) {
        if (record.id != id || !record.live) continue;
        __atomic_store_n(&record.live, false, __ATOMIC_RELEASE);
        if (!record.engine) return;
        // As a mod stops, its hook-engine records are released before this one, being newer, and these find
        // nothing left to remove.
        const NativeHooks* native = native_hooks_backend();
        TSUCHI_Result removed = TSUCHI_NOT_FOUND;
        if (record.native && native) removed = native->remove(record.mod, record.engine);
        if (removed == TSUCHI_NOT_FOUND) hook_remove(record.mod, record.engine);
        return;
    }
}

void drop_record(Record& record) {
    if (record.client.release) record.client.release(record.user);
    release(record.ring);
    record = Record{};
}

// Frees each ended hook that nothing will call again.
void reclaim() {
    for (Record& record : records)
        if (record.id && !__atomic_load_n(&record.live, __ATOMIC_ACQUIRE) && !record.depth && hook_retired(record.engine)) drop_record(record);
}

void send_records(Record& record) {
    Ring& ring = *record.ring;
    for (uint32_t sent = 0; sent < kWatchRecords; ++sent) {
        Ring::Cell& cell = ring.cells[ring.read % kWatchRecords];
        if (__atomic_load_n(&cell.sequence, __ATOMIC_ACQUIRE) != ring.read + 1) return;
        WatchRecord watched = cell.record;
        __atomic_store_n(&cell.sequence, ring.read + kWatchRecords, __ATOMIC_RELEASE);
        ++ring.read;
        watched.dropped = __atomic_exchange_n(&ring.dropped, 0, __ATOMIC_RELAXED);
        if (__atomic_load_n(&record.live, __ATOMIC_ACQUIRE) && mod_running(record.mod)) {
            ++record.depth; // a callback can remove this watch and install another hook
            record.client.record(record.user, watched);
            --record.depth;
        }
        if (!__atomic_load_n(&record.live, __ATOMIC_ACQUIRE)) return;
    }
}

} // namespace

bool is_float(CallType type) { return type == CallType::f32; }

uint32_t read_text16(uint64_t address, uint16_t* out, uint32_t capacity) {
    uint32_t units = 0;
    while (units < capacity && address) {
        const uint64_t at = address + uint64_t(units) * 2;
        // A page that can't be read ends the text.
        if (at < address || !game::readable(at, 2)) break;
        uint16_t unit;
        __builtin_memcpy(&unit, reinterpret_cast<const void*>(at), sizeof unit);
        if (!unit) break;
        out[units++] = unit;
    }
    return units;
}

bool each_dynamic_frame(void (*work)()) {
    for (auto& slot : frame_work) {
        if (slot == work) return true;
        if (!slot) {
            slot = work;
            return true;
        }
    }
    return false;
}

TSUCHI_Result parse_signature(const char* text, Signature& out) {
    out = Signature{};
    if (!text) return TSUCHI_INVALID;
    const char* at = skip_space(text);
    size_t length = word_length(at);
    CallType type;
    if (!length || !type_named(at, length, type) || type == CallType::text16) return TSUCHI_INVALID;
    out.result = type;
    at = skip_space(at + length);
    if (*at != '(') return TSUCHI_INVALID;
    at = skip_space(at + 1);
    uint8_t integers = 0, floats = 0;
    bool stack = false;
    if (*at != ')') {
        for (;;) {
            length = word_length(at);
            if (!length || !type_named(at, length, type) || type == CallType::void_) return TSUCHI_INVALID;
            uint8_t& next = is_float(type) ? floats : integers;
            if (next >= kRegisterArguments) {
                stack = true;
            } else {
                out.args[out.count] = type;
                out.reg[out.count] = next++;
                ++out.count;
            }
            at = skip_space(at + length);
            if (*at == ')') break;
            if (*at != ',') return TSUCHI_INVALID;
            at = skip_space(at + 1);
        }
    }
    if (*skip_space(at + 1)) return TSUCHI_INVALID;
    return stack ? TSUCHI_UNSUPPORTED : TSUCHI_OK;
}

uint64_t frame_argument(const CallFrame& frame, const Signature& signature, uint32_t index) {
    if (index >= signature.count) return 0;
    const CallType type = signature.args[index];
    const uint8_t reg = signature.reg[index];
    return normalized(type, is_float(type) ? frame.q[reg][0] : frame.x[reg]);
}

void set_frame_argument(CallFrame& frame, const Signature& signature, uint32_t index, uint64_t bits) {
    if (index >= signature.count) return;
    const CallType type = signature.args[index];
    const uint8_t reg = signature.reg[index];
    (is_float(type) ? frame.q[reg][0] : frame.x[reg]) = normalized(type, bits);
}

uint64_t frame_result(const CallFrame& frame, CallType type) {
    return normalized(type, is_float(type) ? frame.q[0][0] : frame.x[0]);
}

void set_frame_result(CallFrame& frame, CallType type, uint64_t bits) {
    if (type == CallType::void_) return;
    (is_float(type) ? frame.q[0][0] : frame.x[0]) = normalized(type, bits);
}

// A client's `original`: the barrier its call raised is lifted while the game's function runs, so other mods'
// hooks it reaches work as usual.
void call_with_frame(uint64_t target, CallFrame& frame, CallType result) {
    if (!target || !platform.call) return;
    const bool game_thread = known_game_thread_token() != 0;
    const uint32_t barrier = game_thread ? hook_client_suspend() : 0;
    platform.call(target, &frame, is_float(result));
    if (game_thread) hook_client_restore(barrier);
}

// The original when no client runs for this call. A barrier stays as it was: the call may come from inside a
// script runtime, such as its allocator, and must not let a hook further in enter that runtime.
namespace {
void call_original(uint64_t target, CallFrame& frame, CallType result) {
    if (target && platform.call) platform.call(target, &frame, is_float(result));
}
} // namespace

TSUCHI_Result dynamic_hook(TSUCHI_Mod* mod, DynamicKind kind, const DynamicTarget& target, const Signature& signature,
                           const DynamicClient& client, void* user, uint64_t* id) {
    if (!mod_usable(mod) || !id) return TSUCHI_INVALID;
    if ((kind == DynamicKind::inline_hook) != (target.site == DynamicSite::instruction)) return TSUCHI_INVALID;
    if ((kind == DynamicKind::wrap && !client.call) || (kind == DynamicKind::inline_hook && !client.inline_call) ||
        (kind == DynamicKind::watch && !client.record)) return TSUCHI_INVALID;
    const uint64_t main = game::main_base();
    if (!main) return TSUCHI_NOT_READY;
    prepare_gateways();
    reclaim();
    uint32_t number = 0;
    while (number < kDynamicGateways && records[number].id) ++number;
    if (number >= kDynamicGateways) return TSUCHI_CAPACITY;
    uint64_t gateway = 0;
    if (kind == DynamicKind::inline_hook) {
        gateway = reinterpret_cast<uint64_t>(inline_gateways[number]);
    } else {
        if (!platform.gateway || !platform.call) return TSUCHI_NOT_READY;
        if (number >= platform.gateways) return TSUCHI_CAPACITY;
        gateway = platform.gateway(number, is_float(signature.result));
        if (!gateway) return TSUCHI_NOT_READY;
    }
    const NativeHooks* native = native_hooks_backend();
    if (target.site != DynamicSite::entry && !native) return TSUCHI_NOT_READY;
    Ring* ring = nullptr;
    if (kind == DynamicKind::watch) {
        uint32_t watches = 0;
        for (const Record& other : records) watches += other.id && other.mod == mod && other.kind == DynamicKind::watch;
        if (watches >= kWatchesPerMod) return TSUCHI_CAPACITY;
        ring = static_cast<Ring*>(allocate(sizeof(Ring)));
        if (!ring) return TSUCHI_NO_MEMORY;
        memset(ring, 0, sizeof *ring);
        for (uint32_t i = 0; i < kWatchRecords; ++i) ring->cells[i].sequence = i;
    }
    // Owned before the hook engine's own record of the hook, so that record is released first as the mod stops.
    if (next_id >= (uint64_t(1) << 63)) { release(ring); return TSUCHI_CAPACITY; }
    const uint64_t serial = next_id;
    const TSUCHI_Result owned = own(mod, kDynamic, serial, end_dynamic, nullptr);
    if (owned != TSUCHI_OK) {
        release(ring);
        return owned;
    }
    ++next_id;
    // Filled in before the hook can be reached: the hook engine publishes it as soon as it's on.
    Record& record = records[number];
    record = Record{};
    record.id = serial;
    record.mod = mod;
    record.kind = kind;
    record.native = target.site != DynamicSite::entry;
    if (kind != DynamicKind::inline_hook) record.signature = signature;
    record.client = client;
    record.user = user;
    record.ring = ring;
    __atomic_store_n(&record.live, true, __ATOMIC_RELEASE);
    void** original = reinterpret_cast<void**>(&record.original);
    uint64_t engine = 0;
    TSUCHI_Result result = TSUCHI_INVALID;
    switch (target.site) {
    case DynamicSite::entry:
        result = hook_wrap(mod, main + target.offset, target.check ? main + target.check : 0, reinterpret_cast<void*>(gateway),
                           target.check ? kHookNeedsStock : 0, &engine, original);
        break;
    case DynamicSite::function:
        result = native->function(mod, target.offset, uint32_t(target.check), reinterpret_cast<void*>(gateway), &engine, original);
        break;
    case DynamicSite::instruction:
        result = native->inline_hook(mod, target.offset, uint32_t(target.check), reinterpret_cast<void*>(gateway), &engine, original);
        break;
    }
    if (result != TSUCHI_OK) {
        disown(mod, kDynamic, serial);
        record.client.release = nullptr; // the caller keeps what it gave when the hook isn't on
        drop_record(record);
        return result;
    }
    record.engine = engine;
    *id = serial;
    return TSUCHI_OK;
}

TSUCHI_Result dynamic_remove(TSUCHI_Mod* mod, uint64_t id) {
    if (!mod_usable(mod)) return TSUCHI_INVALID;
    if (disown(mod, kDynamic, id) != TSUCHI_OK) return TSUCHI_NOT_FOUND;
    end_dynamic(nullptr, id);
    return TSUCHI_OK;
}

bool in_dynamic_hook() { return in_hook_client(); }

uint64_t stack_room() {
    if (!frame_stack) return UINT64_MAX;
    const uint64_t here = reinterpret_cast<uint64_t>(__builtin_frame_address(0));
    if (here >= frame_stack) return kGameStackBytes;
    // The engine's frames above the tick take a few kilobytes. 32 KB is assumed, so the room is never overstated.
    const uint64_t used = frame_stack - here + 0x8000;
    return used < kGameStackBytes ? kGameStackBytes - used : 0;
}

uint64_t engine_frame_number() { return frames_begun; }

DynamicStats dynamic_stats() {
    DynamicStats stats{};
    for (const Record& record : records) stats.gateways_used += record.id != 0;
    stats.calls = __atomic_load_n(&counters.calls, __ATOMIC_RELAXED);
    stats.off_thread = __atomic_load_n(&counters.off_thread, __ATOMIC_RELAXED);
    stats.depth = __atomic_load_n(&counters.depth, __ATOMIC_RELAXED);
    stats.not_ready = __atomic_load_n(&counters.not_ready, __ATOMIC_RELAXED);
    stats.failed = __atomic_load_n(&counters.failed, __ATOMIC_RELAXED);
    stats.dropped = __atomic_load_n(&counters.dropped, __ATOMIC_RELAXED);
    return stats;
}

void dynamic_hooks_frame() {
    ++frames_begun;
    frame_stack = reinterpret_cast<uint64_t>(__builtin_frame_address(0));
    for (Record& record : records)
        if (record.id && record.ring) send_records(record);
    reclaim();
    for (auto work : frame_work)
        if (work) work();
}

void dynamic_hooks_start() {
    prepare_gateways();
    set_dynamic_remove(dynamic_remove);
    each_engine_frame(dynamic_hooks_frame);
}

void set_dynamic_platform(const DynamicPlatform& next) {
    platform = next;
    gateways_ready = false;
    prepare_gateways();
}

} // namespace tsuchi

extern "C" void tsuchi_dynamic_dispatch(uint32_t gateway, tsuchi::CallFrame* frame) {
    using namespace tsuchi;
    if (gateway >= kDynamicGateways || !frame) return;
    Record& record = records[gateway];
    const uint64_t original = __atomic_load_n(&record.original, __ATOMIC_ACQUIRE);
    if (!original) return; // the hook engine reaches a gateway only once it has stored the original
    count(counters.calls);
    const CallType result = record.signature.result;
    if (record.kind == DynamicKind::watch) {
        watch_call(record, *frame, original);
        return;
    }
    if (!client_may_run(record)) {
        call_original(original, *frame, result);
        return;
    }
    const CallFrame game = *frame;
    ++record.depth;
    hook_client_enter();
    const bool done = record.client.call(record.user, *frame, DynamicOriginal{original, &record.signature});
    hook_client_leave();
    --record.depth;
    if (!done) {
        count(counters.failed);
        *frame = game;
        call_original(original, *frame, result);
    }
}
