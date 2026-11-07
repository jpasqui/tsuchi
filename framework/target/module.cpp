// The module's two entries: start-up, which the system's loader calls once
// before the game runs, and the per-frame function. It also keeps the status
// block (status.h) and starts the mods.
#include <stddef.h>
#include <string.h>

#include "actor_events.h"
#include "ordinary_lifecycle.h"
#include "actors.h"
#include "audio_parameters.h"
#include "added.h"
#include "clips.h"
#include "compiled_mods.h"
#include "containers.h"
#include "cost_clock.h"
#include "angles.h"
#include "families.h"
#include "flow.h"
#include "frame.h"
#include "game.h"
#include "hooks.h"
#include "host.h"
#include "motion.h"
#include "ground_contact.h"
#include "names.h"
#include "new_maps.h"
#include "native_routes.h"
#include "objects.h"
#include "pikmin_content.h"
#include "platform.h"
#include "presenter.h"
#include "reflect.h"
#include "reflection.h"
#include "registry.h"
#include "script_mods.h"
#include "sd_card.h"
namespace tsuchi_js { void register_script_mods(); }
extern "C" unsigned long long tsuchi_libc_ticks(); // script_library.cpp
#include "services.h"
#include "network_transport.h"
#include "https_jobs.h"
#include "hidden.h"
#include "life_events.h"
#include "materials.h"
#include "mods.h"
#include "mod_choices_game.h"
#include "ui_screens.h"
#include "ui_native.h"
#include "ui_widgets.h"
#include "ui_settings.h"
#include "ui_mod_list.h"
#include "ui_mod_list_native.h"
#include "suspend.h"
#include "switched.h"
#include "status.h"

extern "C" {
extern const unsigned char _start[];
extern const unsigned char tsuchi_hook_thunks[];
extern const unsigned char tsuchi_hook_thunks_end[];
uint64_t tsuchi_previous_tick = 0; // what tsuchi_engine_tick calls to continue the game's frame
void tsuchi_engine_tick();
void tsuchi_site_jump(); // where the patch's block enters the framework
void tsuchi_native_jump(); // where the patch's native routes enter it
void tsuchi_initialize();
void tsuchi_target_frame(float seconds);
}

namespace tsuchi {
// The thunks are in start.S, two instructions each.
uint32_t hook_thunk_count() { return uint32_t((tsuchi_hook_thunks_end - tsuchi_hook_thunks) / 8); }
uint64_t hook_thunk_address(uint32_t thunk) { return reinterpret_cast<uint64_t>(tsuchi_hook_thunks) + 8ull * thunk; }
} // namespace tsuchi

namespace tsuchi::target {
Status status;
uint64_t tick_entry = 0;
TSUCHI_Mod* self = nullptr;
bool mods_found = false;
bool mods_started = false;

// What mods write to their log, and what the framework writes of its own, goes to the system's debug output at
// once, and to the log's file in the save: a line each, with whose it is.
void log_line(const char* mod_id, const char* text) {
    char line[512];
    size_t used = 0;
    const auto add = [&](const char* piece) {
        for (; *piece && used + 1 < sizeof line; ++piece) line[used++] = *piece;
    };
    add("["), add(mod_id), add("] "), add(text);
    target::debug_output(line, used);
    log_file_line(mod_id, text);
}
} // namespace tsuchi::target

namespace {

using namespace tsuchi;
using namespace tsuchi::target;
namespace t = game::table;

// Memory for the framework's own tables: the engine's, which takes back what is
// released. The module starts before the engine has an allocator, and until then
// a fixed block serves, whose memory is not given back.
alignas(16) unsigned char arena[1u << 20];
size_t arena_used = 0;
void* module_allocate(size_t bytes) {
    if (void* block = game::engine_memory(bytes)) {
        ++status.memory_engine;
        return block;
    }
    bytes = (bytes + 15) & ~size_t(15);
    if (bytes > sizeof arena - arena_used) return nullptr;
    void* block = arena + arena_used;
    arena_used += bytes;
    status.memory_fixed = uint32_t(arena_used);
    return block;
}
void module_release(void* block) {
    const auto at = reinterpret_cast<uintptr_t>(block), fixed = reinterpret_cast<uintptr_t>(arena);
    if (!block || (at >= fixed && at < fixed + sizeof arena)) return;
    game::release_engine_memory(block);
    --status.memory_engine;
    ++status.memory_returned;
}

// Once the mods folder and saved selection can be read conclusively, mount the selected containers
// and prepare their early declarations. Busy storage and transient allocation failures retry on a
// later ordinary engine frame; neither publishes an unselected catalogue.
void find_mods_once() {
    if (mods_found) return;
    const TSUCHI_Result found = find_mods();
    if (found == TSUCHI_NOT_READY) return;
    status.mods_result = found;
    if (found == TSUCHI_NO_MEMORY) return;
    mods_found = true;
    presenter_settings(); // the framework's own settings are read from the same files
    if (found == TSUCHI_OK) {
        if (!family_absent(t::family::containers)) mount_mod_containers();
        prepare_mods();
    }
}

// What holds the mods back on this frame, or null when nothing does.
const char* why_mods_wait() {
    if (!mods_found) return "the mods folder cannot be read yet";
    if (const char* why = game::world_not_begun()) return why;
    if (game::busy_with_saves_or_scenes()) return "the game is busy with a save or a change of scene";
    return nullptr;
}

// Says in the log why the mods have not started, once for each reason, after long enough that it is no longer the
// ordinary wait at the title. The status block says `running` throughout: the framework is running, the mods are not.
constexpr uint32_t kFramesBeforeTellingWait = 1800;
void tell_mods_wait(const char* why) {
    static const char* told = nullptr;
    if (why == told || status.frames < kFramesBeforeTellingWait) return;
    told = why;
    char line[200] = "the mods have not started: ";
    copy_text(line + strlen(line), sizeof line - strlen(line), why);
    log_line("tsuchi", line);
}

// Mods are started once: at the first frame on which a world is up and the game is not busy
// with a save or a change of scene. Before that a mod would find no save to keep its settings
// in and nothing of the game to act on.
void start_mods_once() {
    static bool starting = false;
    if (starting || mods_started) return;
    if (const char* why = why_mods_wait()) {
        tell_mods_wait(why);
        return;
    }
    starting = true; // synchronous class loading can enter native callbacks
    ui_mod_list_native_prepare();
    start_mods();
    mods_started = true;
    starting = false;
    tell_containers_left_mounted();
    // Which of them run and which cannot: counted in the status block, and told in the log with the reasons.
    status.mods_found = mod_count();
    for (uint32_t i = 0; i < mod_count(); ++i) {
        status.mods_running += mod_at(i)->state == ModState::running;
        status.mods_blocked += mod_at(i)->state == ModState::blocked;
    }
    log_mods();
}

} // namespace

extern "C" void tsuchi_dialog_setup_entries(); // dialog_entry.cpp

// Whether this build of the game has what a family needs. A family that is gone does not start.
static bool serves(unsigned family) { return !family_absent(family); }

// Says in the log what this build of the game has none of. Their calls return TSUCHI_UNSUPPORTED.
static void tell_absent_families() {
    for (unsigned family = 0; family < t::family_count; ++family)
        if (family_absent(family)) {
            char line[160] = "this build of the game lacks what this family needs, and its calls are unsupported: ";
            copy_text(line + strlen(line), sizeof line - strlen(line), t::families[family]);
            log_line("tsuchi", line);
        }
}

extern "C" void tsuchi_initialize() {
    status.version = 64;
    status.size = sizeof status;
    status.module_base = reinterpret_cast<uint64_t>(_start);
    status.state = kStarting;
    if (!target::find_game(status.module_base)) {
        status.state = kNoGame;
        target::unknown_build_notice(status.module_base);
        return;
    }
    copy_text(status.game_version, sizeof status.game_version, t::version);
    copy_text(status.game_build, sizeof status.game_build, t::build_key);
    status.build_index = t::selected_build;
    status.families_absent = families_absent;
    status.game_base = game::main_base();
    target::new_frame();
    set_thread_probe(target::current_thread);
    // 20 microseconds: a lock is held for less, and the holder gets its core back in that time.
    set_thread_pause([] {
        register int64_t nanoseconds __asm__("x0") = 20000;
        asm volatile("svc 0x0B" : "+r"(nanoseconds) : : "x1", "x2", "x3", "x4", "x5", "x6", "x7", "memory");
    });
    set_allocator({module_allocate, module_release});
    t::bind_functions(status.game_base);
    const Patch patch = find_patch();
    // The route replays LDR x9,[x8] after this callback; the original null guard then
    // sees the entry from the same container whose package bytes win in IoStore.
    native_route(t::native_route::package_store_entry, [](NativeFrame& frame) {
        namespace r = t::route_frame::package_store_entry;
        refresh_mounted_package_entry(frame.x[r::slot], frame.x[r::entry]);
    });
    hooks_start();
    target::code_memory_start();
    reflect_start();
    actors_start();
    entities_start();
    components_start();
    collision_start();
    behavior_start();
    gimmicks_start();
    combat_start();
    game_start();
    players_start();
    camera_start();
    captains_start();
    pikmin_start();
    squad_start();
    creatures_start();
    assets_start();
    tables_start();
    text_start();
    hud_start();
    audio_start();
    world_start();
    anim_start();
    network_native_start();
    https::https_native_start();
    net_start();
    session_set_clock([] { return double(tsuchi_libc_ticks()) / 1000000.0; });
    session_start();
    mods_start();
    if (serves(t::family::mod_choices)) mod_choices_start();
    ui_start();
    declared_start();
    if (serves(t::family::maps)) maps_start();
    else add_manifest_key_off("maps", t::families[t::family::maps]);
    if (serves(t::family::content)) content_start();
    else add_manifest_key_off("content", t::families[t::family::content]);
    if (!serves(t::family::new_maps)) add_manifest_key_off("new_maps", t::families[t::family::new_maps]);
    content_service_start();
    if (serves(t::family::shaders)) shaders_start();
    else add_manifest_key_off("shaders", t::families[t::family::shaders]);
    tsuchi_lua::register_script_mods();
    tsuchi_js::register_script_mods();
    tsuchi_libc_ticks(); // scripts' clocks count from here
    cost_clock = [] { return uint64_t(tsuchi_libc_ticks()); }; // what the framework's own steps cost
    if (serves(t::family::compiled_mods)) register_compiled_mods();
    if (serves(t::family::story)) story_start();

    // Publish the status first, so a tool can read why start-up stopped.
    auto* words = reinterpret_cast<uint64_t*>(status.game_base + t::data::status_words);
    if (!game::writable(reinterpret_cast<uint64_t>(words), 16) || words[0] || words[1]) {
        status.state = kStatusWordsTaken;
        return;
    }
    status.magic = kMagic;
    // The patch's block comes into the framework through this, and only the block of the patch this build was
    // made with may: any other numbers the sites its own way. A block that finds nothing here sends every site
    // straight back to the game, as it does with no framework at all.
    status.site_jump = patch == Patch::right ? reinterpret_cast<uint64_t>(&tsuchi_site_jump) : 0;
    status.native_jump = patch == Patch::right ? reinterpret_cast<uint64_t>(&tsuchi_native_jump) : 0;
    for (uint32_t site = 0; site < t::code_site_count; ++site) status.sites_patched += code_site_patched(site);
    words[1] = reinterpret_cast<uint64_t>(&status);
    asm volatile("dmb ish" ::: "memory");
    words[0] = kMagic;

    // Hook the engine's per-frame entry. The framework's own context owns the hook.
    self = mod_create("tsuchi.self");
    const t::Slot& tick = t::slot::engine_tick;
    tick_entry = status.game_base + tick.table + tick.offset;
    uint64_t hook = 0;
    void* original = nullptr;
    if (const TSUCHI_Result hooked = hook_wrap(self, tick_entry, status.game_base + tick.stock, reinterpret_cast<void*>(&tsuchi_engine_tick), 0, &hook, &original);
        hooked != TSUCHI_OK) {
        status.state = kTickUnexpected;
        // Without the tick no mod gets a frame, so say which of hook_wrap's tests the slot failed.
        const bool readable = game::readable(tick_entry, sizeof(uint64_t)), writable = game::writable(tick_entry, sizeof(uint64_t));
        const uint64_t held = readable ? *reinterpret_cast<const uint64_t*>(tick_entry) : 0;
        char line[200] = "the engine's per-frame entry could not be hooked: result ";
        const auto add_number = [&](uint64_t value) {
            char digits[17];
            int used = 0;
            do digits[used++] = "0123456789abcdef"[value & 15], value >>= 4; while (value && used < 16);
            size_t at = strlen(line);
            while (used && at + 1 < sizeof line) line[at++] = digits[--used];
            line[at] = 0;
        };
        add_number(uint32_t(hooked));
        copy_text(line + strlen(line), sizeof line - strlen(line), readable ? ", readable" : ", not readable");
        copy_text(line + strlen(line), sizeof line - strlen(line), writable ? ", writable" : ", not writable");
        copy_text(line + strlen(line), sizeof line - strlen(line), held && game::executable(held) ? ", holds code" : ", holds no code");
        copy_text(line + strlen(line), sizeof line - strlen(line), held == status.game_base + tick.stock ? ", stock" : ", not stock");
        copy_text(line + strlen(line), sizeof line - strlen(line), ", slot +0x");
        add_number(tick_entry - status.game_base);
        log_line("tsuchi", line);
        return;
    }
    tsuchi_previous_tick = reinterpret_cast<uint64_t>(original);
    status.tick_was_stock = hook_entry_was_stock(tick_entry);
    if (serves(t::family::presenter)) presenter_start(self, [] { return uint64_t(tsuchi_libc_ticks()); }, [](uint32_t us) {
        register uint64_t nanoseconds __asm__("x0") = uint64_t(us) * 1000;
        asm volatile("svc 0x0B" : "+r"(nanoseconds) : : "x1", "x2", "x3", "x4", "x5", "x6", "x7", "memory");
    });

    if (serves(t::family::audio)) audio_parameters_start(self);
    if (serves(t::family::battle)) battle_start(self);
    if (serves(t::family::actor)) actor_events_start(self);
    if (serves(t::family::ground_contact)) ground_contact_start(self);
    if (serves(t::family::effects)) effects_start(self);
    if (serves(t::family::actor)) ordinary_lifecycle_start(self);
    if (serves(t::family::combat)) life_events_start(self);
    if (serves(t::family::save)) saves_start(self);
    if (serves(t::family::new_maps)) new_maps_start(self);
    if (serves(t::family::maps)) map_ai_start(self);
    if (serves(t::family::maps)) map_changes_start(self);
    if (serves(t::family::pikmin)) pikmin_content_start(self);
    if (serves(t::family::squad)) content_squad_start(self);
    if (serves(t::family::carry)) carry_start(self);
    if (serves(t::family::input)) input_start(self);
    if (serves(t::family::storage)) storage_start();
    if (serves(t::family::sd_card)) log_file_start();
    dev_folders_start();
    settings_start();
    presentation_start();
    if (serves(t::family::console_variables)) console_variables_start();
    // One entry for each register the request can be in, two instructions each.
    if (serves(t::family::dialog)) dialogs_start(self, reinterpret_cast<void*>(reinterpret_cast<uint64_t>(&tsuchi_dialog_setup_entries) + 8 * t::route_frame::dialog_setup_call::request));
    if (serves(t::family::ui)) ui_widgets_start(self);
    if (serves(t::family::ui) && ui_native_start(self)) {
        ui_settings_start(self);
        ui_mod_list_start(self);
    }
    set_log_sink(log_line);
    tell_absent_families();
#if TSUCHI_SELFCHECK
    if (serves(t::family::self_check)) selfcheck_start();
#endif
    status.state = kRunning;
}

// A call the framework refused because it came on another thread than the game's is a mistake in whatever made
// it, a mod or the framework itself. It is told in the debug output the first time, and each time the count doubles.
static void tell_off_thread_calls() {
    static uint32_t told = 0;
    const uint32_t refused = off_thread_calls();
    if (refused < (told ? told * 2 : 1)) return;
    told = refused;
    char count[12], line[96] = "[tsuchi] calls refused for coming on another thread than the game's: ";
    char* digit = count + sizeof count;
    *--digit = 0;
    uint32_t left = refused;
    do *--digit = char('0' + left % 10);
    while (left /= 10);
    copy_text(line + strlen(line), sizeof line - strlen(line), digit);
    target::debug_output(line, strlen(line));
}

extern "C" void tsuchi_target_frame(float seconds) {
    target::new_frame();
    ++status.frames;
    status.last_seconds = seconds;
    // The presenter says whether the world steps in this frame; a picture is told as the engine draws it.
    const bool step = presenter_frame(seconds);
    frame_begin();
    if (step) frame_step();
    if (!presenter_draws()) frame_picture(1.0f, seconds);
    tell_off_thread_calls();
    find_mods_once();
    start_mods_once();
#if TSUCHI_SELFCHECK
    selfcheck_frame();
#endif
    observe_status();
    presenter_frame_done();
}
