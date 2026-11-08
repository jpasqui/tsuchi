// Checks of dynamic hooks, the base of script hooks, against a made-up game with host thunks that can really be
// called. The host platform here has gateways for the two shapes the checks use: six whole numbers in and one
// out, and the engine update's (pointer, float, bool). Exits with the number of failures.
#include <array>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <thread>
#include <utility>
#include <vector>

#include "fake_game.h"
#include "callable_thunks.h"
#include "dynamic_hooks.h"
#include "hooks.h"
#include "registry.h"

namespace tsuchi {
// The frame tick isn't part of this program; the checks call dynamic_hooks_frame themselves.
bool each_engine_frame(void (*)()) { return true; }
} // namespace tsuchi

namespace {

using tsuchi::CallFrame;
using tsuchi::CallType;
using tsuchi::DynamicClient;
using tsuchi::DynamicKind;
using tsuchi::DynamicOriginal;
using tsuchi::DynamicSite;
using tsuchi::DynamicTarget;
using tsuchi::InlineRegisters;
using tsuchi::Signature;
using tsuchi::WatchRecord;

int failures = 0;
#define CHECK(condition)                                                     \
    do {                                                                     \
        if (!(condition)) {                                                  \
            std::printf("%s:%d: %s\n", __FILE__, __LINE__, #condition);      \
            ++failures;                                                      \
        }                                                                    \
    } while (0)

// ---- Threads: the probe gives each thread its own number ----

std::atomic<uint64_t> next_thread{100};
uint64_t thread_number() {
    thread_local uint64_t number = next_thread++;
    return number;
}

// ---- The host platform ----

enum class Shape { integers, tick };
Shape shape = Shape::integers;

template <unsigned N>
uint64_t integer_gateway(uint64_t a, uint64_t b, uint64_t c, uint64_t d, uint64_t e, uint64_t f) {
    CallFrame frame{{a, b, c, d, e, f, 0, 0}, {}};
    tsuchi_dynamic_dispatch(N, &frame);
    return frame.x[0];
}
uint64_t float_bits(float value) {
    uint32_t bits;
    std::memcpy(&bits, &value, sizeof bits);
    return bits;
}
float bits_float(uint64_t bits) {
    const uint32_t low = uint32_t(bits);
    float value;
    std::memcpy(&value, &low, sizeof value);
    return value;
}
template <unsigned N>
void tick_gateway(void* engine, float seconds, bool idle) {
    CallFrame frame{{reinterpret_cast<uint64_t>(engine), uint64_t(idle), 0, 0, 0, 0, 0, 0}, {float_bits(seconds)}};
    tsuchi_dynamic_dispatch(N, &frame);
}
template <size_t... I>
std::array<uint64_t, sizeof...(I)> integer_gateways(std::index_sequence<I...>) {
    return {reinterpret_cast<uint64_t>(&integer_gateway<I>)...};
}
template <size_t... I>
std::array<uint64_t, sizeof...(I)> tick_gateways(std::index_sequence<I...>) {
    return {reinterpret_cast<uint64_t>(&tick_gateway<I>)...};
}
const auto all_integer = integer_gateways(std::make_index_sequence<tsuchi::kDynamicGateways>());
const auto all_tick = tick_gateways(std::make_index_sequence<tsuchi::kDynamicGateways>());

uint64_t host_gateway(uint32_t number, bool) {
    if (number >= tsuchi::kDynamicGateways) return 0;
    return shape == Shape::integers ? all_integer[number] : all_tick[number];
}
void host_call(uint64_t target, CallFrame* frame, bool) {
    if (shape == Shape::integers) {
        frame->x[0] = thunks::call(target, frame->x[0], frame->x[1], frame->x[2], frame->x[3], frame->x[4], frame->x[5]);
    } else {
        reinterpret_cast<thunks::EngineTick>(target)(reinterpret_cast<void*>(frame->x[0]), bits_float(frame->f[0]), frame->x[1] != 0);
    }
}

// ---- A made-up native backend: a function or an instruction gets an entry of its own, as the real one does ----

struct NativeTarget {
    uint64_t offset, entry;
};
std::vector<NativeTarget> native_targets;
uint64_t native_entry(uint64_t offset, uint64_t stock) {
    for (const NativeTarget& target : native_targets)
        if (target.offset == offset) return target.entry;
    const uint64_t entry = fake::memory(8);
    fake::put<uint64_t>(entry, stock);
    native_targets.push_back({offset, entry});
    return entry;
}
uint64_t function_stock = 0, inline_stock = 0;
TSUCHI_Result native_function(TSUCHI_Mod* mod, uint64_t offset, uint32_t expected, void* fn, uint64_t* id, void** next) {
    if (expected != 0xD10203FF) return TSUCHI_BUSY;
    return tsuchi::hook_wrap(mod, native_entry(offset, function_stock), 0, fn, 0, id, next);
}
TSUCHI_Result native_inline(TSUCHI_Mod* mod, uint64_t offset, uint32_t expected, void* fn, uint64_t* id, void** next) {
    if (expected != 0x1A9F97E8) return TSUCHI_BUSY;
    return tsuchi::hook_wrap(mod, native_entry(offset, inline_stock), 0, fn, 0, id, next);
}
TSUCHI_Result native_patch(TSUCHI_Mod*, uint64_t, uint32_t, uint32_t, uint64_t*) { return TSUCHI_NOT_READY; }
TSUCHI_Result native_remove(TSUCHI_Mod*, uint64_t) { return TSUCHI_NOT_FOUND; }
TSUCHI_Result native_check(uint64_t, const void*, uint64_t) { return TSUCHI_OK; }
const tsuchi::NativeHooks native{native_function, native_inline, native_patch, native_remove, native_check};

// ---- The game's functions ----

std::atomic<int> stock_calls{0};
uint64_t last_args[6];
uint64_t stock(uint64_t a, uint64_t b, uint64_t c, uint64_t d, uint64_t e, uint64_t f) {
    ++stock_calls;
    const uint64_t args[6] = {a, b, c, d, e, f};
    std::memcpy(last_args, args, sizeof args);
    return a + b;
}
std::atomic<bool> hold_stock{false}, stock_entered{false};
uint64_t holding_stock(uint64_t a, uint64_t b, uint64_t, uint64_t, uint64_t, uint64_t) {
    stock_entered = true;
    while (hold_stock) std::this_thread::yield();
    return a + b;
}
// The game's function calls itself through its own entry, ten levels deep.
uint64_t recursive_entry = 0;
uint64_t recursive_stock(uint64_t level, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t) {
    if (level >= 10) return level;
    return thunks::call(fake::get<uint64_t>(recursive_entry), level + 1, 0, 0, 0, 0, 0);
}
uint64_t text_stock(uint64_t text, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t) {
    // The game's function changes the text it was given.
    reinterpret_cast<uint16_t*>(text)[0] = u'X';
    return 7;
}
float tick_seconds = 0;
bool tick_idle = false;
void tick_stock(void*, float seconds, bool idle) {
    tick_seconds = seconds;
    tick_idle = idle;
}
TSUCHI_InlineContext* inline_seen = nullptr;
uint64_t inline_end_calls = 0;
uint64_t inline_end(uint64_t context, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t) {
    ++inline_end_calls;
    inline_seen = reinterpret_cast<TSUCHI_InlineContext*>(context);
    return 0;
}

// A compiled hook from another mod, outermost, that calls the original and checks that mod calls work there.
TSUCHI_Mod* other_mod = nullptr;
void* compiled_next = nullptr;
bool compiled_could_call = false;
uint64_t compiled(uint64_t a, uint64_t b, uint64_t c, uint64_t d, uint64_t e, uint64_t f) {
    compiled_could_call = tsuchi::mod_usable(other_mod);
    return thunks::call(reinterpret_cast<uint64_t>(compiled_next), a, b, c, d, e, f) + 1000;
}

// ---- A client, as a script runtime would supply ----

struct Client {
    TSUCHI_Mod* mod = nullptr;
    enum class Mode { add_ten, fail_first, recurse, reenter, change_float, inline_flags, inline_fail } mode = Mode::add_ten;
    bool ready = true;
    int calls = 0, releases = 0;
    bool mod_calls_refused = false;
    uint64_t entry = 0; // for recurse
    std::vector<WatchRecord> records;
};
bool client_call(void* user, CallFrame& frame, const DynamicOriginal& original) {
    auto* client = static_cast<Client*>(user);
    ++client->calls;
    client->mod_calls_refused = !tsuchi::mod_usable(client->mod);
    switch (client->mode) {
    case Client::Mode::add_ten: {
        CallFrame call = frame;
        tsuchi::set_frame_argument(call, *original.signature, 0, tsuchi::frame_argument(frame, *original.signature, 0) + 10);
        tsuchi::call_with_frame(original.target, call, original.signature->result);
        tsuchi::set_frame_result(frame, original.signature->result, tsuchi::frame_result(call, original.signature->result));
        return true;
    }
    case Client::Mode::fail_first: return false;
    case Client::Mode::recurse:
        tsuchi::call_with_frame(original.target, frame, original.signature->result);
        return true;
    case Client::Mode::reenter:
        // The hooked function is reached from inside the client, not through the original, as a runtime's own
        // allocator might reach a hooked allocator. That call goes straight to the original.
        thunks::call(fake::get<uint64_t>(client->entry), 1, 2, 0, 0, 0, 0);
        tsuchi::call_with_frame(original.target, frame, original.signature->result);
        return true;
    case Client::Mode::change_float: {
        const float seconds = bits_float(tsuchi::frame_argument(frame, *original.signature, 1));
        tsuchi::set_frame_argument(frame, *original.signature, 1, float_bits(seconds * 2));
        tsuchi::call_with_frame(original.target, frame, original.signature->result);
        return true;
    }
    default: return false;
    }
}
bool client_inline(void* user, InlineRegisters& registers) {
    auto* client = static_cast<Client*>(user);
    ++client->calls;
    registers.x[8] = 0;
    registers.nzcv &= ~(1ull << 29); // C clear: "hi" is false
    return client->mode == Client::Mode::inline_flags;
}
void client_record(void* user, const WatchRecord& record) { static_cast<Client*>(user)->records.push_back(record); }
bool client_ready(void* user) { return static_cast<Client*>(user)->ready; }
void client_release(void* user) { ++static_cast<Client*>(user)->releases; }
DynamicClient client_for() {
    DynamicClient client;
    client.call = client_call;
    client.inline_call = client_inline;
    client.record = client_record;
    client.ready = client_ready;
    client.release = client_release;
    return client;
}

uint64_t offset_of(uint64_t address) { return address - fake::main_image(); }
uint64_t new_entry(uint64_t (*holds)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t)) {
    const uint64_t entry = fake::memory(8);
    fake::put<uint64_t>(entry, reinterpret_cast<uint64_t>(holds));
    return entry;
}
uint64_t call_entry(uint64_t entry, uint64_t a, uint64_t b) { return thunks::call(fake::get<uint64_t>(entry), a, b, 3, 4, 5, 6); }

Signature parsed(const char* text) {
    Signature signature;
    CHECK(tsuchi::parse_signature(text, signature) == TSUCHI_OK);
    return signature;
}

// ---- The checks ----

void signatures() {
    Signature signature;
    CHECK(tsuchi::parse_signature("bool(pointer, i32)", signature) == TSUCHI_OK);
    CHECK(signature.result == CallType::boolean && signature.count == 2 && signature.reg[0] == 0 && signature.reg[1] == 1);
    CHECK(tsuchi::parse_signature(" void ( u64 , f32, u64, f32 ) ", signature) == TSUCHI_OK);
    CHECK(signature.count == 4 && signature.reg[0] == 0 && signature.reg[1] == 0 && signature.reg[2] == 1 && signature.reg[3] == 1);
    CHECK(tsuchi::parse_signature("void()", signature) == TSUCHI_OK && signature.count == 0);
    for (const char* bad : {"", "bool", "bool(", "bool(x)", "text16()", "bool(void)", "bool(u64,)", "bool(u64) x", "(u64)"})
        CHECK(tsuchi::parse_signature(bad, signature) == TSUCHI_INVALID);
    CHECK(tsuchi::parse_signature("void(u64,u64,u64,u64,u64,u64,u64,u64,u64)", signature) == TSUCHI_UNSUPPORTED);
    CHECK(tsuchi::parse_signature("void(f32,f32,f32,f32,f32,f32,f32,f32,f32)", signature) == TSUCHI_UNSUPPORTED);
    CHECK(tsuchi::parse_signature("void(u64,u64,u64,u64,u64,u64,u64,u64,f32)", signature) == TSUCHI_OK);
    // Bits keep the meaning of their type.
    signature = parsed("void(bool, i32, u32, f32)");
    CallFrame frame{{0x1234500, 0xFFFFFFFFull | (7ull << 40), 0xFFFFFFFFull | (7ull << 40)}, {0xAAAAAAAA3F800000ull}};
    CHECK(tsuchi::frame_argument(frame, signature, 0) == 0);
    CHECK(tsuchi::frame_argument(frame, signature, 1) == UINT64_MAX);
    CHECK(tsuchi::frame_argument(frame, signature, 2) == 0xFFFFFFFFull);
    CHECK(tsuchi::frame_argument(frame, signature, 3) == 0x3F800000ull);
}

void wraps_share_chains_with_compiled_hooks(TSUCHI_Mod* mod) {
    const uint64_t entry = new_entry(stock);
    Client client;
    client.mod = mod;
    uint64_t id = 0;
    const Signature signature = parsed("u64(u64, u64, u64, u64, u64, u64)");
    const DynamicTarget target{DynamicSite::entry, offset_of(entry), offset_of(reinterpret_cast<uint64_t>(stock))};
    CHECK(tsuchi::dynamic_hook(mod, DynamicKind::wrap, target, signature, client_for(), &client, &id) == TSUCHI_OK);
    // A compiled hook of another mod goes outside it.
    uint64_t compiled_id = 0;
    CHECK(tsuchi::hook_wrap(other_mod, entry, 0, reinterpret_cast<void*>(compiled), 0, &compiled_id, &compiled_next) == TSUCHI_OK);
    stock_calls = 0;
    CHECK(call_entry(entry, 1, 2) == 1013); // (1 + 10) + 2, plus the compiled hook's 1000
    CHECK(stock_calls == 1 && last_args[0] == 11 && last_args[1] == 2 && last_args[5] == 6);
    CHECK(client.calls == 1 && client.mod_calls_refused);
    CHECK(compiled_could_call);
    // The barrier lifts for the original, and comes back for the rest of the client.
    CHECK(!tsuchi::in_dynamic_hook());

    // A client that fails before it has a result: the original runs with the game's arguments.
    client.mode = Client::Mode::fail_first;
    const auto failed = tsuchi::dynamic_stats().failed;
    CHECK(call_entry(entry, 1, 2) == 1003 && last_args[0] == 1);
    CHECK(tsuchi::dynamic_stats().failed == failed + 1);

    // Not ready: straight to the original.
    client.mode = Client::Mode::add_ten;
    client.ready = false;
    const int calls = client.calls;
    CHECK(call_entry(entry, 1, 2) == 1003 && client.calls == calls);
    client.ready = true;

    // A call on another thread never reaches the client.
    const auto off = tsuchi::dynamic_stats().off_thread;
    uint64_t from_thread = 0;
    std::thread([&] { from_thread = call_entry(entry, 1, 2); }).join();
    CHECK(from_thread == 1003 && client.calls == calls);
    CHECK(tsuchi::dynamic_stats().off_thread == off + 1);

    // Reached from inside the client, not through the original: straight to the original, and counted.
    client.mode = Client::Mode::reenter;
    client.entry = entry;
    const auto refused = tsuchi::dynamic_stats().not_ready;
    client.calls = 0;
    call_entry(entry, 1, 2);
    CHECK(client.calls == 1);
    CHECK(tsuchi::dynamic_stats().not_ready == refused + 1);
    client.mode = Client::Mode::add_ten;

    // Removal: new calls skip it; the client hears release once nothing calls it.
    CHECK(tsuchi::dynamic_remove(mod, id) == TSUCHI_OK);
    CHECK(tsuchi::dynamic_remove(mod, id) == TSUCHI_NOT_FOUND);
    client.calls = 0;
    CHECK(call_entry(entry, 1, 2) == 1003 && client.calls == 0);
    tsuchi::dynamic_hooks_frame();
    CHECK(client.releases == 1);
    tsuchi::hook_remove(other_mod, compiled_id);
}

void recursion_stops_at_the_depth_limit(TSUCHI_Mod* mod) {
    recursive_entry = new_entry(recursive_stock);
    fake::code().push_back(reinterpret_cast<uint64_t>(recursive_stock));
    Client client;
    client.mod = mod;
    client.mode = Client::Mode::recurse;
    uint64_t id = 0;
    CHECK(tsuchi::dynamic_hook(mod, DynamicKind::wrap, DynamicTarget{DynamicSite::entry, offset_of(recursive_entry), 0},
                               parsed("u64(u64)"), client_for(), &client, &id) == TSUCHI_OK);
    const auto deep = tsuchi::dynamic_stats().depth;
    CHECK(thunks::call(fake::get<uint64_t>(recursive_entry), 0) == 10);
    // Levels 0 to 7 run the client; levels 8 to 10 go to the original.
    CHECK(client.calls == int(tsuchi::kDynamicDepth));
    CHECK(tsuchi::dynamic_stats().depth == deep + 3);
    tsuchi::dynamic_remove(mod, id);
    tsuchi::dynamic_hooks_frame();
}

void a_stock_check_refuses_a_changed_entry(TSUCHI_Mod* mod) {
    const uint64_t entry = new_entry(holding_stock);
    Client client;
    uint64_t id = 0;
    const DynamicTarget target{DynamicSite::entry, offset_of(entry), offset_of(reinterpret_cast<uint64_t>(stock))};
    CHECK(tsuchi::dynamic_hook(mod, DynamicKind::wrap, target, parsed("u64(u64)"), client_for(), &client, &id) == TSUCHI_BUSY);
    CHECK(tsuchi::dynamic_stats().gateways_used == 0);
    CHECK(client.releases == 0);
}

void removal_waits_for_running_calls(TSUCHI_Mod* mod) {
    const uint64_t entry = new_entry(holding_stock);
    Client client;
    client.mod = mod;
    uint64_t id = 0;
    const DynamicTarget target{DynamicSite::entry, offset_of(entry), 0};
    CHECK(tsuchi::dynamic_hook(mod, DynamicKind::wrap, target, parsed("u64(u64, u64)"), client_for(), &client, &id) == TSUCHI_OK);
    hold_stock = true;
    stock_entered = false;
    uint64_t result = 0;
    // A call on another thread is inside the gateway, on its way to the original, as the hook is removed.
    std::thread caller([&] { result = call_entry(entry, 4, 5); });
    while (!stock_entered) std::this_thread::yield();
    CHECK(tsuchi::dynamic_remove(mod, id) == TSUCHI_OK);
    tsuchi::dynamic_hooks_frame();
    CHECK(client.releases == 0);
    CHECK(tsuchi::dynamic_stats().gateways_used == 1);
    hold_stock = false;
    caller.join();
    CHECK(result == 9);
    tsuchi::dynamic_hooks_frame();
    CHECK(client.releases == 1);
    CHECK(tsuchi::dynamic_stats().gateways_used == 0);
}

void watches(TSUCHI_Mod* mod) {
    const uint64_t entry = new_entry(stock);
    Client client;
    uint64_t id = 0;
    const DynamicTarget target{DynamicSite::entry, offset_of(entry), 0};
    CHECK(tsuchi::dynamic_hook(mod, DynamicKind::watch, target, parsed("u64(u64, u64, u64)"), client_for(), &client, &id) == TSUCHI_OK);
    // Four threads, 50 calls each: all fit. Every record holds one call's arguments, never a mix.
    auto run = [&](int per_thread) {
        std::vector<std::thread> threads;
        for (uint64_t t = 1; t <= 4; ++t)
            threads.emplace_back([&, t] {
                for (uint64_t i = 0; i < uint64_t(per_thread); ++i) call_entry(entry, t * 1000 + i, t);
            });
        for (auto& thread : threads) thread.join();
    };
    run(50);
    CHECK(client.records.empty()); // nothing until the next frame
    tsuchi::dynamic_hooks_frame();
    CHECK(client.records.size() == 200);
    bool whole = true;
    for (const WatchRecord& record : client.records)
        whole = whole && record.args[0] / 1000 == record.args[1] && record.args[2] == 3 &&
                record.result == record.args[0] + record.args[1] && !record.game_thread && !record.dropped;
    CHECK(whole);
    // 400 calls between two frames: 256 records, and the rest counted on the next one sent.
    client.records.clear();
    run(100);
    tsuchi::dynamic_hooks_frame();
    CHECK(client.records.size() == tsuchi::kWatchRecords);
    uint64_t dropped = 0;
    for (const WatchRecord& record : client.records) dropped += record.dropped;
    CHECK(dropped == 400 - tsuchi::kWatchRecords);
    CHECK(tsuchi::dynamic_remove(mod, id) == TSUCHI_OK);
    tsuchi::dynamic_hooks_frame();
    CHECK(client.releases == 1);

    // text16 is copied before the game's function changes it.
    const uint64_t text_entry = new_entry(text_stock);
    Client texts;
    CHECK(tsuchi::dynamic_hook(mod, DynamicKind::watch, DynamicTarget{DynamicSite::entry, offset_of(text_entry), 0},
                               parsed("u64(text16)"), client_for(), &texts, &id) == TSUCHI_OK);
    const uint64_t buffer = fake::memory(16);
    const char16_t saved[] = u"save0";
    std::memcpy(reinterpret_cast<void*>(buffer), saved, sizeof saved);
    thunks::call(fake::get<uint64_t>(text_entry), buffer);
    tsuchi::dynamic_hooks_frame();
    CHECK(texts.records.size() == 1);
    if (texts.records.size() == 1) {
        const WatchRecord& record = texts.records[0];
        CHECK(record.text_units[0] == 5 && record.text[record.text_start[0]] == u's' && record.result == 7);
        CHECK(record.game_thread);
    }
    CHECK(reinterpret_cast<uint16_t*>(buffer)[0] == u'X');
    tsuchi::dynamic_remove(mod, id);
    tsuchi::dynamic_hooks_frame();
}

void floats(TSUCHI_Mod* mod) {
    shape = Shape::tick;
    thunks::engine_ticks = true;
    const uint64_t entry = fake::memory(8);
    fake::put<uint64_t>(entry, reinterpret_cast<uint64_t>(tick_stock));
    Client client;
    client.mode = Client::Mode::change_float;
    uint64_t id = 0;
    CHECK(tsuchi::dynamic_hook(mod, DynamicKind::wrap, DynamicTarget{DynamicSite::entry, offset_of(entry), 0},
                               parsed("void(pointer, f32, bool)"), client_for(), &client, &id) == TSUCHI_OK);
    thunks::engine_ticks = false;
    reinterpret_cast<thunks::EngineTick>(fake::get<uint64_t>(entry))(nullptr, 0.25f, true);
    CHECK(tick_seconds == 0.5f && tick_idle && client.calls == 1);
    tsuchi::dynamic_remove(mod, id);
    tsuchi::dynamic_hooks_frame();
    shape = Shape::integers;
}

void native_targets_need_the_backend(TSUCHI_Mod* mod) {
    Client client;
    uint64_t id = 0;
    tsuchi::set_native_hooks(nullptr);
    CHECK(tsuchi::dynamic_hook(mod, DynamicKind::wrap, DynamicTarget{DynamicSite::function, 0x1AE5FC0, 0xD10203FF},
                               parsed("u64(u64, u64)"), client_for(), &client, &id) == TSUCHI_NOT_READY);
    CHECK(tsuchi::dynamic_hook(mod, DynamicKind::inline_hook, DynamicTarget{DynamicSite::instruction, 0x156B264, 0x1A9F97E8},
                               Signature{}, client_for(), &client, &id) == TSUCHI_NOT_READY);
    tsuchi::set_native_hooks(&native);

    // A function by its offset.
    function_stock = reinterpret_cast<uint64_t>(stock);
    CHECK(tsuchi::dynamic_hook(mod, DynamicKind::wrap, DynamicTarget{DynamicSite::function, 0x1AE5FC0, 0xD10203FF},
                               parsed("u64(u64, u64)"), client_for(), &client, &id) == TSUCHI_OK);
    CHECK(call_entry(native_entry(0x1AE5FC0, 0), 1, 2) == 13);
    tsuchi::dynamic_remove(mod, id);
    CHECK(tsuchi::dynamic_hook(mod, DynamicKind::wrap, DynamicTarget{DynamicSite::function, 0x1AE5FC0, 0x12345678},
                               parsed("u64(u64, u64)"), client_for(), &client, &id) == TSUCHI_BUSY);

    // An instruction: the client changes x8 and the flags; the rest of the chain runs once.
    inline_stock = reinterpret_cast<uint64_t>(inline_end);
    Client inlined;
    inlined.mode = Client::Mode::inline_flags;
    CHECK(tsuchi::dynamic_hook(mod, DynamicKind::inline_hook, DynamicTarget{DynamicSite::instruction, 0x156B264, 0x1A9F97E8},
                               Signature{}, client_for(), &inlined, &id) == TSUCHI_OK);
    uint64_t registers[31] = {};
    registers[8] = 77;
    TSUCHI_InlineContext context{registers, nullptr, 0, 0x20000000ull | 0x5, 0, 0};
    const uint64_t inline_entry = native_entry(0x156B264, 0);
    inline_end_calls = 0;
    thunks::call(fake::get<uint64_t>(inline_entry), reinterpret_cast<uint64_t>(&context));
    CHECK(inline_end_calls == 1 && inline_seen == &context);
    CHECK(registers[8] == 0 && context.nzcv == 0);
    // A client that fails: its changes are dropped, and the chain still runs once.
    inlined.mode = Client::Mode::inline_fail;
    registers[8] = 77;
    context.nzcv = 0x20000000ull;
    thunks::call(fake::get<uint64_t>(inline_entry), reinterpret_cast<uint64_t>(&context));
    CHECK(inline_end_calls == 2 && registers[8] == 77 && context.nzcv == 0x20000000ull);
    tsuchi::dynamic_remove(mod, id);
    tsuchi::dynamic_hooks_frame();
    // Kinds and sites have to agree.
    CHECK(tsuchi::dynamic_hook(mod, DynamicKind::inline_hook, DynamicTarget{DynamicSite::entry, 0, 0}, Signature{},
                               client_for(), &inlined, &id) == TSUCHI_INVALID);
}

void stopping_ends_every_hook() {
    TSUCHI_Mod* mod = tsuchi::mod_create("dynamic.stop");
    const uint64_t entry = new_entry(stock);
    Client client;
    client.mod = mod;
    uint64_t id = 0;
    CHECK(tsuchi::dynamic_hook(mod, DynamicKind::wrap, DynamicTarget{DynamicSite::entry, offset_of(entry), 0},
                               parsed("u64(u64, u64)"), client_for(), &client, &id) == TSUCHI_OK);
    CHECK(call_entry(entry, 1, 2) == 13);
    tsuchi::mod_stop(mod);
    CHECK(call_entry(entry, 1, 2) == 3 && client.calls == 1);
    tsuchi::dynamic_hooks_frame();
    CHECK(client.releases == 1);
}

// Some frames below the caller, as a hook is below the frame tick.
__attribute__((noinline)) uint64_t room_below(int levels) {
    volatile char pad[512];
    pad[0] = char(levels);
    return levels ? room_below(levels - 1) + pad[0] * 0 : tsuchi::stack_room();
}

void frames() {
    const uint64_t before = tsuchi::engine_frame_number();
    tsuchi::dynamic_hooks_frame();
    CHECK(tsuchi::engine_frame_number() == before + 1);
    const uint64_t shallow = room_below(1), deep = room_below(8);
    CHECK(shallow > 0 && shallow < tsuchi::kGameStackBytes);
    CHECK(deep < shallow);
}

} // namespace

int main() {
    fake::start();
    for (auto pointer : thunks::all) fake::code().push_back(reinterpret_cast<uint64_t>(pointer));
    for (auto pointer : thunks::all_engine_ticks) fake::code().push_back(reinterpret_cast<uint64_t>(pointer));
    for (uint64_t pointer : all_integer) fake::code().push_back(pointer);
    for (uint64_t pointer : all_tick) fake::code().push_back(pointer);
    for (auto pointer : {reinterpret_cast<uint64_t>(stock), reinterpret_cast<uint64_t>(holding_stock),
                         reinterpret_cast<uint64_t>(text_stock), reinterpret_cast<uint64_t>(tick_stock),
                         reinterpret_cast<uint64_t>(inline_end), reinterpret_cast<uint64_t>(compiled)})
        fake::code().push_back(pointer);
    tsuchi::set_dynamic_platform(tsuchi::DynamicPlatform{tsuchi::kDynamicGateways, host_gateway, host_call});
    tsuchi::set_thread_probe(thread_number);
    TSUCHI_Mod* mod = tsuchi::mod_create("dynamic.hooks");
    other_mod = tsuchi::mod_create("dynamic.other");
    signatures();

    // Before the game's thread is known, no client runs.
    {
        const uint64_t entry = new_entry(stock);
        Client client;
        uint64_t id = 0;
        CHECK(tsuchi::dynamic_hook(mod, DynamicKind::wrap, DynamicTarget{DynamicSite::entry, offset_of(entry), 0},
                                   parsed("u64(u64, u64)"), client_for(), &client, &id) == TSUCHI_OK);
        CHECK(id >= (uint64_t(1) << 62) && id < (uint64_t(1) << 63));
        CHECK(call_entry(entry, 1, 2) == 3 && client.calls == 0);
        tsuchi::dynamic_remove(mod, id);
        tsuchi::dynamic_hooks_frame();
    }
    tsuchi::note_game_thread();

    wraps_share_chains_with_compiled_hooks(mod);
    recursion_stops_at_the_depth_limit(mod);
    a_stock_check_refuses_a_changed_entry(mod);
    removal_waits_for_running_calls(mod);
    watches(mod);
    floats(mod);
    native_targets_need_the_backend(mod);
    stopping_ends_every_hook();
    frames();
    tsuchi::mod_stop(mod);
    tsuchi::mod_stop(other_mod);
    if (failures) std::printf("%d failures\n", failures);
    else std::puts("dynamic hooks passed");
    return failures;
}
