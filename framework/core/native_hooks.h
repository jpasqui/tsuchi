// Owned native hooks and one-word code patches. The platform supplies writable
// aliases; the core validates targets, conflicts, replay, ownership, and dispatch.
#pragma once
#include <stdint.h>

namespace tsuchi {

constexpr uint32_t kNativeHookTargets = 256;
constexpr uint64_t kNativeGatewayBytes = 64;
constexpr uint64_t kNativeArenaBytes = kNativeHookTargets * kNativeGatewayBytes;

struct CodeArena {
    uint64_t rx;
    void* rw;
    uint64_t bytes;
};

enum class CodeMemoryPhase : uint32_t {
    idle, disabled, handle, allocate, backing, placement, map_code, set_rx,
    map_alias, verify_rx, verify_rw, rollback_alias, rollback_code, ready,
};
// Internal diagnostics. Arena state: 0 untried, 1 ready, 2 refused with clean
// rollback, 3 retained after failed rollback, 4 disabled. Results are kernel
// result words; a zero result with refusal denotes a local validation failure.
struct CodeMemoryStats {
    uint32_t alias_count, alias_limit, arena_state, phase, result, rollback_result;
    uint64_t arena_rx, arena_rw, arena_bytes;
};

struct CodeMemory {
    // Maps an existing RX range as writable without changing its executable
    // mapping. A successful alias remains valid for the process's lifetime.
    void* (*alias)(uint64_t address, uint64_t bytes);
    // Publishes alias stores to instruction fetch at the executable address.
    void (*flush)(void* alias, uint64_t address, uint64_t bytes);
    // Lazily admits exactly one process-lifetime 16 KiB RX/RW gateway arena.
    // Refusal must leave no target instruction modified. Failed rollback keeps
    // backing memory owned and makes subsequent arena requests fail closed.
    bool (*gateway_arena)(CodeArena*) = nullptr;
    void (*stats)(CodeMemoryStats*) = nullptr;
};

struct NativeHookStats {
    uint32_t target_limit, targets_assigned, targets_published, patches_active;
    CodeMemoryStats code;
};

// Called once before mods start. A missing backend makes writes fail closed.
void native_hooks_start(CodeMemory memory);
// Snapshot on the game thread; no allocations or mappings are admitted here.
NativeHookStats native_hooks_stats();

} // namespace tsuchi
