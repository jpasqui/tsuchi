#include "game.h"
#include "hooks.h"
#include "host.h"
#include "native_hooks.h"
#include "native_routes.h"
#include "registry.h"
#include <array>
#include <cstdio>
#include <cstring>
#include <utility>

namespace {
struct alignas(4096) Memory {
    uint8_t image[0x4000], rx[0x4000], rw[0x4000];
} storage{};
auto& image = storage.image;
int failures = 0, alias_calls = 0, arena_calls = 0, flushes = 0, called_a = 0, called_b = 0, called_last = 0;
bool aliases_enabled = true;
bool continuation_ready_at_publication = false;
const char* scenario = "";
unsigned supplied_thunks = 64;
uint64_t foreign_thread = 1;
TSUCHI_Hook first{}, second{}, other{}, function_hook{};
TSUCHI_Hook inflight{};
TSUCHI_Mod* inflight_mod = nullptr;
#define CHECK(x) do { if (!(x)) { std::printf("line %d: %s\n", __LINE__, #x); ++failures; } } while (0)
uint64_t base() { return reinterpret_cast<uint64_t>(image); }
uint64_t arena_rx() { return reinterpret_cast<uint64_t>(storage.rx); }
uint32_t gateway_word(unsigned slot, unsigned offset = 0) {
    uint32_t value;
    memcpy(&value, storage.rw + slot * 64 + offset, 4);
    return value;
}
uint64_t thread() { return foreign_thread; }
uint32_t word(uint64_t offset) { uint32_t value; memcpy(&value, image + offset, 4); return value; }
void put(uint64_t offset, uint32_t value) { memcpy(image + offset, &value, 4); }
void* alias(uint64_t at, uint64_t bytes) {
    ++alias_calls;
    CHECK(at >= base() && at < base() + sizeof image);
    if (!strcmp(scenario, "late-alias-refused") && alias_calls > 1) return nullptr;
    return aliases_enabled && at >= base() && bytes <= sizeof image && at - base() <= sizeof image - bytes ? reinterpret_cast<void*>(at) : nullptr;
}
bool gateway_arena(tsuchi::CodeArena* out) {
    ++arena_calls;
    *out = {arena_rx(), storage.rw, sizeof storage.rw};
    if (!strcmp(scenario, "arena-refused")) return false;
    if (!strcmp(scenario, "zero-rx")) out->rx = 0;
    if (!strcmp(scenario, "zero-rw")) out->rw = nullptr;
    if (!strcmp(scenario, "unaligned-rx")) ++out->rx;
    if (!strcmp(scenario, "unaligned-rw")) out->rw = storage.rw + 1;
    if (!strcmp(scenario, "wrong-size")) out->bytes -= 4096;
    if (!strcmp(scenario, "overflow-rx")) out->rx = ~uint64_t(4095);
    if (!strcmp(scenario, "overflow-rw")) out->rw = reinterpret_cast<void*>(~uint64_t(4095));
    if (!strcmp(scenario, "overlap-rx-main")) out->rx = base();
    if (!strcmp(scenario, "overlap-rw-main")) out->rw = image;
    if (!strcmp(scenario, "overlap-rx-rw")) out->rw = storage.rx;
    if (!strcmp(scenario, "unreachable")) out->rx = base() + (1ull << 28);
    if (!strcmp(scenario, "branch-endpoint")) out->rx = base() + sizeof image - (1ull << 27);
    if (!strcmp(scenario, "backward-branch")) out->rx = base() - sizeof storage.rx;
    return true;
}
void memory_stats(tsuchi::CodeMemoryStats* out) {
    out->alias_count = alias_calls;
    out->alias_limit = 768;
    out->arena_state = arena_calls ? 1 : 0;
}
void flush(void*, uint64_t at, uint64_t bytes) {
    ++flushes;
    if (at == arena_rx() && !strcmp(scenario, "publication-race")) put(0x1000, 0x52800000);
    if (at == base() + 0x1000 && bytes == 4 && first.original && first.id.id) {
        continuation_ready_at_publication = first.original && first.id.id;
    }
}
void a(TSUCHI_InlineContext* context) {
    ++called_a;
    static_cast<uint64_t*>(context->registers)[0] += 1;
    static_cast<uint64_t*>(context->vectors)[0] = 91;
    context->sp = 0; // read-only input; it cannot change the machine frame.
    context->nzcv = ~uint64_t(0);
    reinterpret_cast<void (*)(TSUCHI_InlineContext*)>(first.original)(context);
}
void b(TSUCHI_InlineContext* context) {
    ++called_b;
    static_cast<uint64_t*>(context->registers)[0] *= 2;
    reinterpret_cast<void (*)(TSUCHI_InlineContext*)>(second.original)(context);
}
void no_op(TSUCHI_InlineContext*) {}
void last_target(TSUCHI_InlineContext* context) {
    ++called_last;
    static_cast<uint64_t*>(context->registers)[0] += 43;
}
void remove_self(TSUCHI_InlineContext* context) {
    CHECK(tsuchi::host.hook->remove(inflight_mod, inflight.id) == TSUCHI_OK);
    reinterpret_cast<void (*)(TSUCHI_InlineContext*)>(inflight.original)(context);
}
template<unsigned N> void thunk(TSUCHI_InlineContext* context) {
    uint64_t stack = 0;
    const auto selected = tsuchi_hook_enter(N, reinterpret_cast<uint64_t>(&stack), 1);
    reinterpret_cast<void (*)(TSUCHI_InlineContext*)>(selected.target)(context);
    if (selected.tracked) CHECK(tsuchi_hook_leave(reinterpret_cast<uint64_t>(&stack)) == 1);
}
template<size_t... I> auto make_thunks(std::index_sequence<I...>) {
    return std::array<void (*)(TSUCHI_InlineContext*), sizeof...(I)>{&thunk<I>...};
}
const auto thunks = make_thunks(std::make_index_sequence<1024>());
void invoke(unsigned slot, tsuchi::NativeFrame& frame) {
    tsuchi_native_dispatch(&frame, arena_rx() + 64 * slot + 8);
}
}
namespace tsuchi::game {
uint64_t main_base() { return base(); }
bool readable(uint64_t address, uint64_t bytes) { return address && bytes < 0x100000 && address <= ~uint64_t(0) - bytes; }
bool writable(uint64_t address, uint64_t bytes) { return readable(address, bytes); }
bool executable(uint64_t address) { return address != 0; }
}
namespace tsuchi {
uint32_t hook_thunk_count() { return supplied_thunks; }
uint64_t hook_thunk_address(uint32_t n) { return reinterpret_cast<uint64_t>(thunks[n]); }
}
int main(int argc, char** argv) {
    namespace t = tsuchi::game::table;
    tsuchi::set_thread_probe(thread); tsuchi::note_game_thread(); tsuchi::hooks_start();
    if (argc > 1) scenario = argv[1];
    tsuchi::CodeMemory backend{alias, flush, gateway_arena, memory_stats};
    if (!strcmp(scenario, "missing-arena")) backend.gateway_arena = nullptr;
    if (!strcmp(scenario, "missing-alias")) backend.alias = nullptr;
    if (!strcmp(scenario, "missing-flush")) backend.flush = nullptr;
    tsuchi::native_hooks_start(backend);
    auto* owner = tsuchi::mod_create("native.one");
    auto* another = tsuchi::mod_create("native.two");
    auto* no_patch = tsuchi::mod_create("native.missing");
    put(0x1000, 0xd503201f); put(0x1100, 0xd503201f); put(0x1200, 0xd503201f); put(0x1300, 0xd503201f);
    CHECK(tsuchi::host.hook->wrap_inline(no_patch, 0x1000, 0xd503201f, reinterpret_cast<void*>(a), &first) == TSUCHI_NOT_READY);
    CHECK(tsuchi::mod_block_reason(no_patch) && !tsuchi::owned_count(no_patch));
    tsuchi::mod_stop(no_patch);
    const uint64_t mark[] = {t::stub::patch_magic, t::stub::patch_hash};
    memcpy(image + t::stub::block, mark, sizeof mark);
    CHECK(tsuchi::find_patch() == tsuchi::Patch::right);
    auto* api = tsuchi::host.hook;
    if (*scenario) {
        if (!strcmp(scenario, "backward-branch")) {
            put(0x1000, 0x16000000); // B at the negative limit of its original reach.
            CHECK(api->wrap_inline(owner, 0x1000, 0x16000000, reinterpret_cast<void*>(no_op), &first) == TSUCHI_OK);
            CHECK(tsuchi::native_hooks_stats().targets_published == 1 && arena_calls == 1);
            std::printf("native edge relocation in external arena: %s\n", failures ? "FAIL" : "PASS");
            return failures;
        }
        if (!strcmp(scenario, "hook-capacity")) supplied_thunks = 0;
        const bool publication = !strcmp(scenario, "publication-race") || !strcmp(scenario, "late-alias-refused");
        const auto result = api->wrap_inline(owner, 0x1000, 0xd503201f, reinterpret_cast<void*>(no_op), &first);
        CHECK(result == (publication ? TSUCHI_BUSY : !strcmp(scenario, "hook-capacity") ? TSUCHI_CAPACITY : TSUCHI_NOT_READY));
        auto stats = tsuchi::native_hooks_stats();
        CHECK(stats.target_limit == 256 && stats.targets_assigned == unsigned(publication));
        CHECK(!stats.targets_published && !stats.patches_active && !tsuchi::owned_count(owner));
        CHECK(stats.code.alias_count == unsigned(alias_calls) && stats.code.alias_limit == 768);
        CHECK(word(0x1000) == (!strcmp(scenario, "publication-race") ? 0x52800000u : 0xd503201fu));
        if (!publication) {
            for (uint8_t value : storage.rw) CHECK(value == 0);
            CHECK(!flushes);
        } else {
            CHECK(gateway_word(0) != 0 && flushes == 1);
            scenario = "";
            put(0x1000, 0xd503201f);
            CHECK(api->wrap_inline(another, 0x1000, 0xd503201f, reinterpret_cast<void*>(no_op), &other) == TSUCHI_OK);
            stats = tsuchi::native_hooks_stats();
            CHECK(stats.targets_assigned == 1 && stats.targets_published == 1 && arena_calls == 1);
            tsuchi::NativeFrame frame{}; frame.x[0] = 31; invoke(0, frame);
            CHECK(frame.x[0] == 31);
            tsuchi::mod_stop(another);
        }
        if (!strncmp(argv[1], "missing-", 8)) CHECK(!arena_calls && !alias_calls);
        std::printf("native arena admission and failed publication: %s\n", failures ? "FAIL" : "PASS");
        return failures;
    }
    CHECK(TSUCHI_HAS(api, wrap_function) && TSUCHI_HAS(api, wrap_inline) && TSUCHI_HAS(api, patch_instruction));
    const uint32_t expected_pair[] = {0xd503201f, 0};
    CHECK(api->check_code(owner, 0x1000, expected_pair, sizeof expected_pair) == TSUCHI_OK);
    const uint32_t empty_gateway[] = {0, 0};
    CHECK(api->check_code(owner, t::stub::dynamic_base, empty_gateway, sizeof empty_gateway) == TSUCHI_OK);
    CHECK(api->check_code(owner, 0x1000, expected_pair, 3) == TSUCHI_INVALID);
    CHECK(api->check_code(owner, 0x3ffc, expected_pair, sizeof expected_pair) == TSUCHI_INVALID);
    put(0x1004, 1);
    CHECK(api->check_code(owner, 0x1000, expected_pair, sizeof expected_pair) == TSUCHI_BUSY);
    put(0x1004, 0);
    auto* named = tsuchi::mod_create("native.named");
    TSUCHI_Hook named_hook{};
    const auto& site = t::code_sites[0];
    put(site.address, 0x14000000u | (uint32_t((site.entry - site.address) >> 2) & 0x3ffffff));
    CHECK(api->wrap_function(named, site.address, site.instruction, reinterpret_cast<void*>(no_op), &named_hook) == TSUCHI_OK);
    CHECK(!alias_calls && named_hook.original);
    tsuchi::mod_stop(named);
    CHECK(api->wrap_inline(owner, 0x1001, 0xd503201f, reinterpret_cast<void*>(a), &first) == TSUCHI_INVALID);
    CHECK(api->wrap_inline(owner, ~uint64_t(0) - 3, 0, reinterpret_cast<void*>(a), &first) == TSUCHI_INVALID);
    CHECK(api->wrap_inline(owner, t::constant::stub_base, 0, reinterpret_cast<void*>(a), &first) == TSUCHI_INVALID);
    CHECK(api->wrap_inline(owner, t::code_sites[0].address, 0, reinterpret_cast<void*>(a), &first) == TSUCHI_BUSY);
    CHECK(api->wrap_inline(owner, t::native_routes[0].address, 0, reinterpret_cast<void*>(a), &first) == TSUCHI_BUSY);
    CHECK(api->wrap_inline(owner, 0x1000, 0, reinterpret_cast<void*>(a), &first) == TSUCHI_BUSY && !alias_calls && !arena_calls);
    put(0x1500, 0x10000000); // ADR requires relocation at a function entry.
    CHECK(api->wrap_function(owner, 0x1500, 0x10000000, reinterpret_cast<void*>(a), &first) == TSUCHI_INVALID && !alias_calls && !arena_calls);
    for (uint32_t unsupported : {0xd63f03c0u, 0xdc000000u}) {
        put(0x1504, unsupported);
        CHECK(api->wrap_inline(owner, 0x1504, unsupported, reinterpret_cast<void*>(a), &first) == TSUCHI_INVALID && !alias_calls && !arena_calls);
    }
    aliases_enabled = false;
    CHECK(api->wrap_inline(owner, 0x1000, 0xd503201f, reinterpret_cast<void*>(a), &first) == TSUCHI_NOT_READY);
    CHECK(!tsuchi::owned_count(owner) && word(0x1000) == 0xd503201f);
    aliases_enabled = true;
    CHECK(api->wrap_inline(owner, 0x1000, 0xd503201f, reinterpret_cast<void*>(a), &first) == TSUCHI_OK);
    CHECK(continuation_ready_at_publication);
    CHECK(api->wrap_inline(another, 0x1000, 0xd503201f, reinterpret_cast<void*>(b), &second) == TSUCHI_OK);
    const auto instruction = word(0x1000);
    CHECK(instruction != 0xd503201f && first.id.id != second.id.id && first.original && second.original);
    tsuchi::NativeFrame frame{}; frame.x[0] = 3; frame.sp = 0x1234; frame.resume = 0x5678;
    invoke(0, frame);
    CHECK(frame.x[0] == 7 && called_a == 1 && called_b == 1 && frame.q[0][0] == 91);
    CHECK(frame.sp == 0x1234 && frame.resume == 0x5678 && frame.nzcv == 0xf0000000);
    CHECK(api->wrap_function(owner, 0x1000, 0xd503201f, reinterpret_cast<void*>(a), &function_hook) == TSUCHI_BUSY);
    TSUCHI_HookId patch{};
    CHECK(api->patch_instruction(owner, 0x1000, instruction, 0, &patch) == TSUCHI_BUSY);
    CHECK(api->remove(owner, second.id) == TSUCHI_NOT_FOUND);
    CHECK(api->remove(owner, first.id) == TSUCHI_OK);
    frame.x[0] = 3; invoke(0, frame);
    CHECK(frame.x[0] == 6 && called_a == 1 && called_b == 2);
    tsuchi::mod_stop(another);
    frame.x[0] = 3; invoke(0, frame);
    CHECK(frame.x[0] == 3 && called_b == 2 && word(0x1000) == instruction);
    CHECK(api->wrap_inline(owner, 0x1000, 0xd503201f, reinterpret_cast<void*>(a), &first) == TSUCHI_OK);
    CHECK(api->wrap_inline(owner, 0x1100, 0xd503201f, reinterpret_cast<void*>(no_op), &other) == TSUCHI_OK);
    CHECK(api->wrap_function(owner, 0x1200, 0xd503201f, reinterpret_cast<void*>(no_op), &function_hook) == TSUCHI_OK);
    CHECK(gateway_word(2) == 0x58000050 && gateway_word(2, 16) == 0xd503201f);
    CHECK(api->wrap_inline(owner, 0x1300, 0xd503201f, reinterpret_cast<void*>(no_op), &other) == TSUCHI_OK);
    put(0x1400, 0xd503201f);
    CHECK(tsuchi::native_hooks_stats().targets_assigned == 4 && arena_calls == 1);
    inflight_mod = tsuchi::mod_create("native.inflight-removal");
    CHECK(api->wrap_inline(inflight_mod, 0x1000, 0xd503201f, reinterpret_cast<void*>(remove_self), &inflight) == TSUCHI_OK);
    unsigned retired = unsigned(thunks.size());
    for (unsigned i = 0; i < thunks.size(); ++i)
        if (reinterpret_cast<void*>(thunks[i]) == inflight.original) retired = i;
    CHECK(retired < thunks.size() && tsuchi_hook_target(retired) != 0);
    frame.x[0] = 3; invoke(0, frame);
    CHECK(frame.x[0] == 4 && !tsuchi::owned_count(inflight_mod) && tsuchi_hook_target(retired) == 0);
    tsuchi::mod_stop(inflight_mod);
    // Dispatch accepts only a published inline gateway's returned instruction.
    const int calls = called_a;
    for (uint64_t returned : {arena_rx() - 56, arena_rx(), arena_rx() + 9,
                              arena_rx() + 2 * 64 + 8, arena_rx() + 4 * 64 + 8,
                              arena_rx() + tsuchi::kNativeArenaBytes + 8})
        tsuchi_native_dispatch(&frame, returned);
    CHECK(called_a == calls);
    put(0x1600, 0xd503201f);
    CHECK(api->patch_instruction(owner, 0x1600, 0xd503201f, 0x91000400, &patch) == TSUCHI_OK);
    CHECK(tsuchi::native_hooks_stats().patches_active == 1);
    CHECK(word(0x1600) == 0x91000400);
    auto* third = tsuchi::mod_create("native.three");
    TSUCHI_HookId duplicate{};
    CHECK(api->patch_instruction(third, 0x1600, 0x91000400, 0, &duplicate) == TSUCHI_BUSY);
    CHECK(api->remove(third, patch) == TSUCHI_NOT_FOUND);
    put(0x1600, 0x52800000); // foreign changes must not be overwritten by removal.
    CHECK(api->remove(owner, patch) == TSUCHI_BUSY && word(0x1600) == 0x52800000);
    put(0x1600, 0x91000400);
    CHECK(api->remove(owner, patch) == TSUCHI_OK && word(0x1600) == 0xd503201f);
    CHECK(api->patch_instruction(owner, 0x1600, 0xd503201f, 0x91000400, &patch) == TSUCHI_OK);
    tsuchi::mod_stop(owner);
    CHECK(word(0x1600) == 0xd503201f);
    CHECK(api->patch_instruction(third, 0x1600, 0xd503201f, 0x91000400, &patch) == TSUCHI_OK);
    put(0x1600, 0x52800000);
    tsuchi::mod_stop(third);
    CHECK(word(0x1600) == 0x52800000);
    auto* restarted = tsuchi::mod_create("native.one");
    CHECK(api->wrap_inline(restarted, 0x1000, 0xd503201f, reinterpret_cast<void*>(a), &first) == TSUCHI_OK);
    foreign_thread = 2;
    CHECK(api->wrap_inline(restarted, 0x1000, 0xd503201f, reinterpret_cast<void*>(a), &other) == TSUCHI_INVALID);
    foreign_thread = 1;
    CHECK(api->wrap_inline(owner, 0x1000, 0xd503201f, reinterpret_cast<void*>(a), &other) == TSUCHI_INVALID);
    tsuchi::mod_stop(restarted);
    // Failed startup follows the same mod_stop path as the compiled loader.
    // Inactive owners are skipped by dispatch even if cleanup leaks a record,
    // so stock output alone cannot prove reclamation. Check the retired
    // original thunk is actually unassigned, then exceed the supplied budget
    // through repeated failed starts on the same permanent target.
    for (unsigned attempt = 0; attempt < 128; ++attempt) {
        auto* failed = tsuchi::mod_create("native.failed-startup");
        TSUCHI_Hook armed{}, refused{};
        CHECK(api->wrap_inline(failed, 0x1100, 0xd503201f, reinterpret_cast<void*>(no_op), &armed) == TSUCHI_OK);
        CHECK(armed.original && armed.id.id && tsuchi::owned_count(failed) == 1);
        CHECK(api->wrap_inline(failed, 0x1000, 0xd503201b, reinterpret_cast<void*>(no_op), &refused) == TSUCHI_BUSY);
        unsigned original = unsigned(thunks.size());
        for (unsigned i = 0; i < thunks.size(); ++i)
            if (reinterpret_cast<void*>(thunks[i]) == armed.original) original = i;
        CHECK(original < thunks.size() && tsuchi_hook_target(original) != 0);
        tsuchi::mod_stop(failed);
        CHECK(tsuchi_hook_target(original) == 0);
    }
    // Permanent targets use a shared arena and one entry thunk each. Increase
    // the stand-in's supplied budget after the 64-thunk reclamation test so
    // capacity here measures the target pool rather than continuation thunks.
    supplied_thunks = 1024;
    auto* capacity_owner = tsuchi::mod_create("native.capacity");
    for (unsigned slot = 4; slot < tsuchi::kNativeHookTargets; ++slot) {
        const uint64_t offset = 0x1800 + 4 * slot;
        put(offset, 0xd503201f);
        auto handler = slot + 1 == tsuchi::kNativeHookTargets ? last_target : no_op;
        CHECK(api->wrap_inline(capacity_owner, offset, 0xd503201f, reinterpret_cast<void*>(handler), &other) == TSUCHI_OK);
        CHECK(word(offset) != 0xd503201f);
    }
    frame.x[0] = 9;
    invoke(tsuchi::kNativeHookTargets - 1, frame);
    CHECK(called_last == 1 && frame.x[0] == 52);
    for (uint64_t returned : {arena_rx() + tsuchi::kNativeArenaBytes - 60,
                              arena_rx() + tsuchi::kNativeArenaBytes - 55,
                              arena_rx() + tsuchi::kNativeArenaBytes + 8})
        tsuchi_native_dispatch(&frame, returned);
    CHECK(called_last == 1 && frame.x[0] == 52);
    const uint64_t refused_at = 0x2000;
    put(refused_at, 0xd503201f);
    const int aliases_before_capacity = alias_calls;
    CHECK(api->wrap_inline(capacity_owner, refused_at, 0xd503201f, reinterpret_cast<void*>(no_op), &other) == TSUCHI_CAPACITY);
    CHECK(word(refused_at) == 0xd503201f && alias_calls == aliases_before_capacity);
    auto stats = tsuchi::native_hooks_stats();
    CHECK(stats.target_limit == 256 && stats.targets_assigned == 256 && stats.targets_published == 256);
    CHECK(!stats.patches_active && arena_calls == 1);
    tsuchi::mod_stop(capacity_owner);
    auto* shared_owner = tsuchi::mod_create("native.shared-after-capacity");
    CHECK(api->wrap_inline(shared_owner, 0x1000, 0xd503201f, reinterpret_cast<void*>(no_op), &other) == TSUCHI_OK);
    CHECK(tsuchi::native_hooks_stats().targets_assigned == 256);
    tsuchi::mod_stop(shared_owner);
    for (unsigned i = 0; i < t::stub::dynamic_count * 64; ++i)
        CHECK(image[t::stub::dynamic_base + i] == 0);
    CHECK(flushes > 4);
    std::printf("native ownership, instruction checks, chaining, restart, and conflicts: %s\n", failures ? "FAIL" : "PASS");
    return failures;
}
