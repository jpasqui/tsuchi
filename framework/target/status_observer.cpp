// Copies retained acceptance counters without running a check or changing the game.
#include "holds.h"
#include "map_navigation.h"
#include "materials.h"
#include "shader_materials.h"
#include "mods.h"
#include "new_maps.h"
#include "services.h"
#include "status.h"
#include <stddef.h>

// The target's C formatter is supplied by runtime/lua/libc/libc.c.
extern "C" int snprintf(char*, size_t, const char*, ...);

namespace tsuchi::target {

void observe_status() {
    if (status.frames < 2 || !(status.frames & 31)) {
        status.native_hooks = native_hooks_stats();
        status.hook_budgets = hook_stats();
        static uint32_t reported_arena = 0;
        const auto& code = status.native_hooks.code;
        if (code.arena_state >= 1 && code.arena_state <= 3 && code.arena_state != reported_arena) {
            reported_arena = code.arena_state;
            const char* const states[] = {"untried", "ready", "refused", "retained"};
            const char* const phases[] = {"idle", "disabled", "handle", "allocate", "backing", "placement", "map_code", "set_rx",
                                          "map_alias", "verify_rx", "verify_rw", "rollback_alias", "rollback_code", "ready"};
            char line[320];
            snprintf(line, sizeof line, "native hook arena %s phase=%s result=%#x rollback=%#x rx=%llx rw=%llx bytes=%llu targets=%u/%u aliases=%u/%u thunks=%u/%u frames=%u peak=%u",
                     states[code.arena_state], code.phase < sizeof phases / sizeof *phases ? phases[code.phase] : "unknown",
                     code.result, code.rollback_result, static_cast<unsigned long long>(code.arena_rx),
                     static_cast<unsigned long long>(code.arena_rw), static_cast<unsigned long long>(code.arena_bytes),
                     status.native_hooks.targets_assigned, status.native_hooks.target_limit, code.alias_count, code.alias_limit,
                     status.hook_budgets.thunks_used, status.hook_budgets.thunks_limit,
                     status.hook_budgets.frames_active, status.hook_budgets.frames_peak);
            log_line("tsuchi", line);
        }
    }
    status.mods_found = mod_count();
    status.mods_running = status.mods_blocked = 0;
    for (uint32_t i = 0; i < status.mods_found; ++i) {
        status.mods_running += mod_at(i)->state == ModState::running;
        status.mods_blocked += mod_at(i)->state == ModState::blocked;
    }
    const MapCounts maps = map_counts();
    const NavigationCounts navigation = map_navigation_counts();
    status.map_parts = map_parts(), status.map_levels_asked = maps.levels_asked, status.map_levels_refused = maps.levels_refused;
    status.map_entries_taken = maps.entries_taken, status.map_entries_refused = maps.entries_refused;
    status.map_removed = maps.removed, status.map_moved = maps.moved, status.map_values_refused = maps.values_refused;
    status.nav_meshes = navigation.navmeshes, status.nav_removed = navigation.removed, status.nav_added = navigation.added, status.nav_refused = navigation.refused;
    status.newmap_menus_listed = new_map_counts().menus_listed;
    status.map_values_set = maps.values_set;
    const ShaderCounts shaders = shader_counts();
    const MaterialHoldCounts holds = material_hold_counts();
    const CostTotal passes = hold_pass_cost();
    status.shader_level_requests = shaders.level_requests, status.shader_copies_asked = shaders.copies_asked;
    status.shader_copies_ready = shaders.copies_ready, status.shader_stand_ins = shaders.stand_ins, status.shader_failed = shaders.failed;
    status.shader_searches = shaders.material_searches, status.shader_parts_asked = shaders.parts_asked, status.shader_slots_set = shaders.slots_set;
    status.hold_kept = holds.kept, status.hold_changed = holds.changed, status.hold_set = holds.set;
    status.shader_steps = shaders.steps.runs, status.shader_step_us = shaders.steps.total_us, status.shader_step_longest_us = shaders.steps.longest_us;
    status.hold_passes = passes.runs, status.hold_pass_us = passes.total_us, status.hold_pass_longest_us = passes.longest_us;
    status.shader_full_walks = shaders.full_walks;
}

} // namespace tsuchi::target
