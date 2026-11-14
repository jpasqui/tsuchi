#include "native_hooks.h"

#include "game.h"
#include "hooks.h"
#include "native_codegen.h"
#include "native_routes.h"
#include "registry.h"
#include "tsuchi/hook.h"

namespace tsuchi {
namespace {
namespace t = game::table;
constexpr unsigned kTargets = kNativeHookTargets;
constexpr unsigned kPatches = 256;
static_assert(native_code::kGatewayBytes == kNativeGatewayBytes, "gateway slot size");
const Kind kPatch{"instruction_patch"};
CodeMemory memory{};
CodeArena arena{};
enum class Mode : uint8_t { function, inline_hook, call };
struct Target {
    uint64_t address, gateway, entry;
    uint32_t expected, branch;
    Mode mode;
    bool published;
};
Target targets[kTargets];
unsigned target_count = 0;
struct PatchWord {
    TSUCHI_Mod* mod;
    uint64_t id, address;
    uint32_t expected, replacement;
};
PatchWord patches[kPatches];
uint64_t patch_serial = 1ull << 63;

bool offset_valid(uint64_t offset, bool code_target = true) {
    const uint64_t base = game::main_base();
    return base && base <= ~uint64_t(0) - t::constant::code_size &&
           !(offset & 3) && offset < t::constant::code_size &&
           t::constant::code_size - offset >= 4 &&
           (!code_target || !(offset >= t::constant::stub_base && offset < t::constant::stub_end));
}
bool reserved(uint64_t offset) {
    for (const auto& site : t::code_sites) if (site.name && site.address == offset) return true;
    for (const auto& route : t::native_routes) if (route.name && route.address == offset) return true;
    // Conditional routes also own the following conditional branch.
    for (unsigned i = 0; i < t::conditional_site_count; ++i)
        if (t::conditional_sites[i].address == offset || t::conditional_sites[i].address + 4 == offset) return true;
    return false;
}
bool patched(uint64_t offset) {
    for (const auto& patch : patches) if (patch.id && patch.address == offset) return true;
    return false;
}
Target* target_at(uint64_t offset) {
    for (unsigned i = 0; i < target_count; ++i) if (targets[i].address == offset) return &targets[i];
    return nullptr;
}
uint32_t* writable_word(uint64_t offset) {
    const uint64_t address = game::main_base() + offset;
    if (!memory.alias || !memory.flush || !game::executable(address)) return nullptr;
    return static_cast<uint32_t*>(memory.alias(address, 4));
}
bool exchange(uint64_t offset, uint32_t expected, uint32_t replacement) {
    uint32_t* alias = writable_word(offset);
    if (!alias || !__atomic_compare_exchange_n(alias, &expected, replacement, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) return false;
    memory.flush(alias, game::main_base() + offset, 4);
    return true;
}
bool overlap(uint64_t first, uint64_t bytes, uint64_t other, uint64_t other_bytes) {
    return first < other + other_bytes && other < first + bytes;
}
bool admit_arena() {
    if (arena.rx) return true;
    if (!memory.gateway_arena) return false;
    CodeArena candidate{};
    if (!memory.gateway_arena(&candidate)) return false;
    const uint64_t rw = reinterpret_cast<uint64_t>(candidate.rw);
    const uint64_t main = game::main_base(), main_end = main + t::constant::code_size;
    if (!candidate.rx || !rw || candidate.bytes != kNativeArenaBytes ||
        ((candidate.rx | rw) & 4095) || candidate.bytes > ~uint64_t(0) - candidate.rx ||
        candidate.bytes > ~uint64_t(0) - rw ||
        overlap(candidate.rx, candidate.bytes, rw, candidate.bytes) ||
        overlap(candidate.rx, candidate.bytes, main, t::constant::code_size) ||
        overlap(rw, candidate.bytes, main, t::constant::code_size)) return false;
    const uint64_t last = candidate.rx + candidate.bytes - 4;
    uint32_t jump;
    // Check both directions, including replay's return just beyond the final
    // main instruction. B's positive and negative endpoints differ by a word.
    if (!native_code::branch(main, last, jump) || !native_code::branch(last, main, jump) ||
        !native_code::branch(main_end, candidate.rx, jump) ||
        !native_code::branch(candidate.rx, main_end, jump) ||
        !native_code::branch(candidate.rx + 4, main + t::stub::native_shared, jump, true) ||
        !native_code::branch(last - native_code::kGatewayBytes + 8,
                             main + t::stub::native_shared, jump, true)) return false;
    arena.rw = candidate.rw;
    arena.bytes = candidate.bytes;
    __atomic_store_n(&arena.rx, candidate.rx, __ATOMIC_RELEASE);
    return true;
}
void* gateway_alias(const Target& target) {
    if (!arena.rx || target.gateway < arena.rx ||
        target.gateway - arena.rx > arena.bytes - native_code::kGatewayBytes) return nullptr;
    return static_cast<uint8_t*>(arena.rw) + (target.gateway - arena.rx);
}
void inline_end(TSUCHI_InlineContext*) {}

TSUCHI_Result wrap(TSUCHI_Mod* mod, uint64_t offset, uint32_t expected, void* function,
                   uint64_t* id, void** original, Mode mode) {
    if (!mod_usable(mod) || !function || !id || !original || !offset_valid(offset)) return TSUCHI_INVALID;
    if (patch_found() != Patch::right) {
        mod_block(mod, patch_problem());
        return TSUCHI_NOT_READY;
    }
    if (mode == Mode::call && ((expected & 0xfc000000u) != 0x94000000u ||
        !offset_valid(offset + 4 * native_code::signed_bits(expected & 0x03ffffffu, 26)))) return TSUCHI_INVALID;
    if (mode == Mode::function || mode == Mode::call) {
        for (unsigned i = 0; i < t::code_site_count; ++i) {
            const auto& site = t::code_sites[i];
            if (site.address != offset) continue;
            if (bool(site.call) != (mode == Mode::call) || site.instruction != expected) return TSUCHI_BUSY;
            return hook_code_site(mod, i, function, id, original);
        }
    }
    if (reserved(offset) || patched(offset)) return TSUCHI_BUSY;
    const uint64_t main = game::main_base();
    Target* target = target_at(offset);
    bool created = false;
    if (target) {
        if (target->mode != mode || target->expected != expected ||
            game::read_or<uint32_t>(main + offset) != (target->published ? target->branch : expected)) return TSUCHI_BUSY;
    } else {
        uint32_t there;
        if (!game::read(main + offset, there)) return TSUCHI_NOT_READY;
        if (there != expected) return TSUCHI_BUSY;
        if (target_count >= kTargets) return TSUCHI_CAPACITY;
        // Refuse unsupported forms before requesting writable aliases or an
        // executable allocation. Address-dependent replay is checked only at
        // the admitted arena: a trial gateway at the target could incorrectly
        // refuse a valid branch at the edge of its original reach.
        native_code::Gateway code;
        const bool indirect_call = (expected & 0xfffffc1fu) == 0xd63f0000u;
        const bool invalid_vector_literal = (expected & 0x3b000000u) == 0x18000000u &&
                                            (expected & (1u << 26)) && (expected >> 30) == 3;
        if ((mode == Mode::function && (native_code::relative(expected) || indirect_call)) ||
            (mode == Mode::inline_hook && ((indirect_call && ((expected >> 5) & 31) == 30) ||
                                           invalid_vector_literal)))
            return TSUCHI_INVALID;
        if (!memory.gateway_arena || !writable_word(offset) || !admit_arena()) return TSUCHI_NOT_READY;
        const uint64_t gateway = arena.rx + native_code::kGatewayBytes * target_count;
        uint32_t jump;
        if (!native_code::branch(main + offset, gateway, jump, mode == Mode::call) ||
            !(mode == Mode::call ? native_code::call(code, 0) :
              mode == Mode::function ? native_code::function(code, main + offset, expected, gateway, 0) :
                                      native_code::inline_hook(code, main + offset, expected, gateway, main + t::stub::native_shared)))
            return TSUCHI_INVALID;
        target = &targets[target_count];
        const uint64_t entry = mode == Mode::call ? main + offset + 4 * native_code::signed_bits(expected & 0x03ffffffu, 26) :
            mode == Mode::function ? gateway + 16 : reinterpret_cast<uint64_t>(&inline_end);
        *target = {offset, gateway, entry, expected, jump, mode, false};
        created = true;
    }
    // Fill the caller's continuation under the hook engine's dispatch lock.
    // A published target may run on another thread as soon as that lock opens.
    const TSUCHI_Result result = hook_wrap(mod, reinterpret_cast<uint64_t>(&target->entry), 0, function, 0, id, original);
    if (result != TSUCHI_OK) {
        if (created) *target = {};
        return result;
    }
    const uint64_t hook = *id;
    // hook_wrap has published a permanent entry thunk. Keep its target slot
    // even if a later code publication loses to another writer; reusing that
    // storage for another address would change the thunk's stock continuation.
    if (created) __atomic_store_n(&target_count, target_count + 1, __ATOMIC_RELEASE);
    if (!target->published) {
        native_code::Gateway code;
        const bool emitted = mode == Mode::call ? native_code::call(code, target->entry) : mode == Mode::function ?
            native_code::function(code, main + offset, expected, target->gateway, target->entry) :
            native_code::inline_hook(code, main + offset, expected, target->gateway, main + t::stub::native_shared);
        void* alias = gateway_alias(*target);
        if (!emitted || !alias) {
            hook_remove(mod, hook);
            *id = 0; *original = nullptr;
            return TSUCHI_NOT_READY;
        }
        memcpy(alias, &code, sizeof code);
        memory.flush(alias, target->gateway, sizeof code);
        // Publish dispatch before the single instruction that can enter it.
        __atomic_store_n(&target->published, true, __ATOMIC_RELEASE);
        if (!exchange(offset, expected, target->branch)) {
            __atomic_store_n(&target->published, false, __ATOMIC_RELEASE);
            hook_remove(mod, hook);
            *id = 0; *original = nullptr;
            return TSUCHI_BUSY;
        }
    }
    return TSUCHI_OK;
}
TSUCHI_Result wrap_function(TSUCHI_Mod* mod, uint64_t at, uint32_t expected, void* fn, uint64_t* id, void** next) {
    return wrap(mod, at, expected, fn, id, next, Mode::function);
}
TSUCHI_Result wrap_inline(TSUCHI_Mod* mod, uint64_t at, uint32_t expected, void* fn, uint64_t* id, void** next) {
    return wrap(mod, at, expected, fn, id, next, Mode::inline_hook);
}
TSUCHI_Result wrap_call(TSUCHI_Mod* mod, uint64_t at, uint32_t expected, void* fn, uint64_t* id, void** next) {
    return wrap(mod, at, expected, fn, id, next, Mode::call);
}

void end_patch(void*, uint64_t id) {
    for (auto& patch : patches) if (patch.id == id) {
        exchange(patch.address, patch.replacement, patch.expected);
        patch = {};
        return;
    }
}
TSUCHI_Result patch_instruction(TSUCHI_Mod* mod, uint64_t at, uint32_t expected, uint32_t replacement, uint64_t* id) {
    if (!mod_usable(mod) || !id || !offset_valid(at) || expected == replacement) return TSUCHI_INVALID;
    if (reserved(at) || patched(at) || target_at(at)) return TSUCHI_BUSY;
    uint32_t there;
    if (!game::read(game::main_base() + at, there)) return TSUCHI_NOT_READY;
    if (there != expected) return TSUCHI_BUSY;
    if (!writable_word(at)) return TSUCHI_NOT_READY;
    PatchWord* slot = nullptr;
    for (auto& patch : patches) if (!patch.id) { slot = &patch; break; }
    if (!slot || patch_serial == ~uint64_t(0)) return TSUCHI_CAPACITY;
    const uint64_t serial = patch_serial++;
    const TSUCHI_Result owned = own(mod, kPatch, serial, end_patch, nullptr);
    if (owned != TSUCHI_OK) return owned;
    if (!exchange(at, expected, replacement)) { disown(mod, kPatch, serial); return TSUCHI_BUSY; }
    *slot = {mod, serial, at, expected, replacement};
    *id = serial;
    return TSUCHI_OK;
}
TSUCHI_Result remove_patch(TSUCHI_Mod* mod, uint64_t id) {
    for (auto& patch : patches) if (patch.id == id && patch.mod == mod) {
        if (!mod_usable(mod)) return TSUCHI_INVALID;
        if (game::read_or<uint32_t>(game::main_base() + patch.address) != patch.replacement) return TSUCHI_BUSY;
        if (!writable_word(patch.address)) return TSUCHI_NOT_READY;
        if (!ownership_update_allowed(mod, kPatch, id)) return TSUCHI_BUSY;
        if (!exchange(patch.address, patch.replacement, patch.expected)) return TSUCHI_BUSY;
        disown(mod, kPatch, id);
        patch = {};
        return TSUCHI_OK;
    }
    return TSUCHI_NOT_FOUND;
}
void dispatch(NativeFrame* frame, uint64_t returned) {
    const uint64_t rx = __atomic_load_n(&arena.rx, __ATOMIC_ACQUIRE);
    if (!rx || returned < rx || returned - rx >= kNativeArenaBytes) return;
    const uint64_t offset = returned - rx;
    if (offset % native_code::kGatewayBytes != 8) return;
    const unsigned index = unsigned(offset / native_code::kGatewayBytes);
    if (index >= __atomic_load_n(&target_count, __ATOMIC_ACQUIRE)) return;
    Target& target = targets[index];
    if (target.mode != Mode::inline_hook ||
        !__atomic_load_n(&target.published, __ATOMIC_ACQUIRE)) return;
    TSUCHI_InlineContext context{frame->x, frame->q, frame->sp, frame->nzcv, frame->fpcr, frame->fpsr};
    const uint64_t entry = __atomic_load_n(&target.entry, __ATOMIC_ACQUIRE);
    reinterpret_cast<void (*)(TSUCHI_InlineContext*)>(entry)(&context);
    frame->nzcv = context.nzcv & 0xf0000000ull;
    frame->fpcr = context.fpcr;
    frame->fpsr = context.fpsr;
}
TSUCHI_Result check_code(uint64_t offset, const void* expected, uint64_t bytes) {
    if (!expected || !offset_valid(offset, false) || bytes < 4 || bytes > 4096 || (bytes & 3) ||
        bytes > t::constant::code_size - offset) return TSUCHI_INVALID;
    const uint64_t at = game::main_base() + offset;
    if (!game::readable(at, bytes)) return TSUCHI_NOT_READY;
    return memcmp(reinterpret_cast<const void*>(at), expected, bytes) ? TSUCHI_BUSY : TSUCHI_OK;
}
const NativeHooks backend{wrap_function, wrap_inline, patch_instruction, remove_patch, check_code, wrap_call};
} // namespace

void native_hooks_start(CodeMemory next) {
    memory = next;
    set_native_hooks(&backend);
    set_native_dispatch(dispatch);
}

NativeHookStats native_hooks_stats() {
    NativeHookStats stats{};
    stats.target_limit = kTargets;
    stats.targets_assigned = __atomic_load_n(&target_count, __ATOMIC_ACQUIRE);
    for (unsigned i = 0; i < stats.targets_assigned; ++i)
        stats.targets_published += __atomic_load_n(&targets[i].published, __ATOMIC_ACQUIRE);
    for (const auto& patch : patches) stats.patches_active += patch.id != 0;
    if (memory.stats) memory.stats(&stats.code);
    return stats;
}

} // namespace tsuchi
