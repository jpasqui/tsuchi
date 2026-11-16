// The course screen can use an owned, already-loaded schedule without asking
// Nintendo to refresh that schedule. Account and membership queries stay native.
#include <string.h>
#include "tsuchi/hook.h"
#include "academy_internal.h"
#include "game.h"
#include "frame.h"
#include "hooks.h"
#include "mods.h"
#include "objects.h"
#include "registry.h"

namespace tsuchi {
namespace {
namespace t = game::table;
bool ready = false;
bool noted = false;
uint64_t course_presenter = 0;
uint64_t selected_course = 0;
uint32_t local_depth = 0;
void (*next_course_details)(void*, const void*, const void*, bool, bool) = nullptr;
bool (*next_course_refresh)(void*) = nullptr;
void (*next_preflight)(void*, const void*, bool) = nullptr;
void (*next_schedule_refresh)(void*, const void*, const void*) = nullptr;
void (*next_provider_decision)(TSUCHI_InlineContext*) = nullptr;
void (*next_admission)(TSUCHI_InlineContext*) = nullptr;

void course_details(void* presenter, const void* category, const void* course, bool first, bool second) {
    next_course_details(presenter, category, course, first, second);
    if (on_game_thread()) {
        course_presenter = game::to_handle(reinterpret_cast<uint64_t>(presenter));
        selected_course = reinterpret_cast<uint64_t>(course);
    }
}

bool local_course() {
    if (!on_game_thread() || !ready || !academy_uploads_blocked() || !academy_mod_schedule()) return false;
    const uint64_t manager = game::global<uint64_t>(t::data::academy_schedule);
    // The frontend can open before any gameplay course has been loaded. Its
    // exact callbacks and live presenter qualify each request below; the last
    // gameplay course is not the frontend selection.
    uint8_t valid = 0;
    if (!manager || !game::read(manager + t::academy_schedule::valid, valid) || valid != 1 || !t::bound(t::fn::academy_status)) return false;
    const int32_t state = t::fn::academy_status(reinterpret_cast<void*>(manager + t::academy_schedule::interface));
    // Nintendo's connection probe can temporarily report unavailable while
    // the owned schedule remains loaded. Native course/date/pass admission
    // still decides whether an attempt can start.
    return state == int32_t(t::constant::academy_status_open) || state == int32_t(t::constant::academy_status_unavailable);
}

bool course_refresh(void* presenter) {
    const bool local = local_course();
    if (local) ++local_depth;
    const bool result = next_course_refresh(presenter);
    if (local) --local_depth;
    return result;
}

bool live_presenter(uint64_t presenter) {
    const uint64_t widget = presenter ? game::read_or<uint64_t>(presenter + t::academy_local_presenter::widget) : 0;
    return widget && game::to_handle(widget) && t::bound(t::fn::ui_widget_in_viewport) &&
        t::fn::ui_widget_in_viewport(reinterpret_cast<void*>(widget));
}

void provider_decision(TSUCHI_InlineContext* registers) {
    next_provider_decision(registers);
    if (!registers || !registers->registers || !local_course()) return;
    auto* x = static_cast<uint64_t*>(registers->registers);
    if (!live_presenter(x[19])) return;
    // This exact decision in the native ready continuation selects a Nintendo
    // connection job for retakes. The supplied schedule uses its existing
    // offline launch branch. Native confirmation, admission and pass use stay
    // intact; no account, presenter or save field is changed by this adapter.
    x[8] &= ~uint64_t(1);
    mod_note(kFrameworkLogId, "Academy launch uses the supplied schedule provider");
}

void admission(TSUCHI_InlineContext* registers) {
    next_admission(registers);
    if (!registers || !registers->registers || !local_course()) return;
    auto* x = static_cast<uint64_t*>(registers->registers);
    const uint64_t action = game::global<uint64_t>(t::data::academy_local_launch_action);
    if (!(x[0] & 1) || !live_presenter(x[19]) || !action || !t::bound(t::fn::academy_local_launch_ready)) return;
    t::fn::academy_local_launch_ready(reinterpret_cast<void*>(x[19]), action);
    x[0] &= ~uint64_t(1);
    mod_note(kFrameworkLogId, "Academy accepted retake uses the supplied schedule");
}

void preflight(void* ui, const void* callback, bool flags) {
    const uint64_t outer = reinterpret_cast<uint64_t>(callback);
    const uint64_t inner = outer ? game::read_or<uint64_t>(outer + t::academy_local_callback::target) : 0;
    const uint64_t table = inner ? game::read_or<uint64_t>(inner) : 0;
    const auto invoke = inner ? game::method(inner, t::academy_local_ready_callback_table::invoke) : nullptr;
    const uint64_t presenter = inner ? game::read_or<uint64_t>(inner + t::academy_local_callback::presenter) : 0;
    const uint64_t action = game::global<uint64_t>(t::data::academy_local_launch_action);
    const bool local = local_course();
    const bool launch = !flags && local && presenter &&
        table == game::main_base() + t::data::academy_local_launch_callback_table &&
        reinterpret_cast<uint64_t>(invoke) == game::main_base() + t::data::academy_local_launch_callback_invoke;
    const bool retry = !flags && local && presenter &&
        table == game::main_base() + t::data::academy_local_retry_callback_table &&
        reinterpret_cast<uint64_t>(invoke) == game::main_base() + t::data::academy_local_retry_callback_invoke;
    const bool visible = (launch || retry) && live_presenter(presenter);
    if (retry && visible && t::bound(t::fn::academy_local_retry_ready)) {
        // This second native launch callback refreshes the schedule, then its
        // reply calls this presenter continuation. Our schedule is already
        // validated. No connectivity/download job or busy counter is needed.
        t::fn::academy_local_retry_ready(reinterpret_cast<void*>(presenter));
        mod_note(kFrameworkLogId, "Academy exam retry uses the supplied schedule");
        return;
    }
    if (launch && visible && action && t::bound(t::fn::academy_local_launch_ready)) {
        // This callback only refreshes Nintendo's schedule twice, then chooses
        // this native action from Nintendo's connection flag. Our validated
        // schedule is already loaded. Use this call's live captured presenter;
        // the remembered course-details presenter can belong to a prior world.
        // Continue the same native action directly,
        // without allocating the connectivity job or changing account state.
        t::fn::academy_local_launch_ready(reinterpret_cast<void*>(presenter), action);
        mod_note(kFrameworkLogId, "Academy exam launch uses the supplied schedule");
        return;
    }
    uint64_t target = 1;
    // Empty connectivity probes also run later from the visible course's
    // update, after the original refresh call has returned. They have no
    // result consumer; the mod's schedule/board requests already have replies.
    if (!flags && local && callback &&
        (local_depth || live_presenter(game::from_handle(course_presenter))) &&
        game::read(reinterpret_cast<uint64_t>(callback) + t::academy_local_callback::target, target) && !target) {
        mod_note(kFrameworkLogId, "Academy loaded schedule skipped an empty Nintendo connectivity probe");
        return;
    }
    next_preflight(ui, callback, flags);
}

void schedule_refresh(void* manager, const void* callback, const void* options) {
    const uint64_t outer = reinterpret_cast<uint64_t>(callback);
    const uint64_t inner = outer ? game::read_or<uint64_t>(outer + t::academy_local_callback::target) : 0;
    const uint64_t table = inner ? game::read_or<uint64_t>(inner) : 0;
    const auto invoke = inner ? game::method(inner, t::academy_local_callback_table::invoke) : nullptr;
    const uint64_t size = t::academy_local_reply::size;
    if (on_game_thread() && local_depth && local_course() && table == game::main_base() + t::data::academy_local_callback_table &&
        reinterpret_cast<uint64_t>(invoke) == game::main_base() + t::data::academy_local_callback_invoke &&
        size <= 64 && uint64_t(t::academy_local_reply::state) + sizeof(uint32_t) <= size &&
        uint64_t(t::academy_local_reply::owned_data) + 16 <= size) {
        // This exact callback consumes only the refresh status and its optional
        // owned message. The supplied schedule has already passed validation and
        // reached the native manager; no saved result or online response is made.
        alignas(8) uint8_t reply[64]{};
        const uint32_t success = uint32_t(t::constant::academy_local_refresh_success);
        memcpy(reply + t::academy_local_reply::state, &success, sizeof success);
        invoke(reinterpret_cast<void*>(inner), reply);
        if (!noted) {
            noted = true;
            mod_note(kFrameworkLogId, "Academy refreshed from the supplied exam schedule");
        }
        return;
    }
    next_schedule_refresh(manager, callback, options);
}
}

TSUCHI_Result academy_open_board(TSUCHI_Mod* mod, uint32_t course) {
    TSUCHI_AcademyBoardState board{};
    const auto owned = academy_board_state(mod, course, &board);
    if (owned != TSUCHI_OK) return owned;
    if (!local_course()) return TSUCHI_NOT_READY;
    if (!academy_course_matches(course, selected_course)) return TSUCHI_NOT_READY;
    const uint64_t presenter = game::from_handle(course_presenter);
    const uint64_t widget = presenter ? game::read_or<uint64_t>(presenter + t::academy_local_presenter::widget) : 0;
    uint8_t phase = 0, busy = 0;
    if (!widget || !game::to_handle(widget) || !t::bound(t::fn::ui_widget_in_viewport) || !t::bound(t::fn::ui_get_visibility) ||
        !t::fn::ui_widget_in_viewport(reinterpret_cast<void*>(widget))) return TSUCHI_NOT_READY;
    const uint32_t visibility = t::fn::ui_get_visibility(reinterpret_cast<void*>(widget));
    if (visibility != 0 && visibility != 3 && visibility != 4) return TSUCHI_NOT_READY;
    if (!game::read(presenter + t::academy_local_presenter::phase, phase) ||
        !game::read(presenter + t::academy_local_presenter::busy, busy)) return TSUCHI_NOT_READY;
    if (phase != t::constant::academy_local_idle_phase || busy) return TSUCHI_BUSY;
    const auto guard = academy_block_uploads();
    if (guard != TSUCHI_OK) return guard;
    // The game calls its refresh from this same phase, and the refresh moves the screen on itself once it starts.
    return course_refresh(reinterpret_cast<void*>(presenter)) ? TSUCHI_OK : TSUCHI_NOT_READY;
}

void academy_local_hooks_start(TSUCHI_Mod* self) {
    const auto* native = native_hooks_backend();
    if (!native || !native->function) return;
    const auto hook = [&](uint64_t address, auto replacement, auto& next) {
        if (address == t::absent_value) return false;
        uint64_t id = 0;
        void* original = nullptr;
        const auto result = native->function(self, address, uint32_t(t::constant::academy_hook_pac),
            reinterpret_cast<void*>(replacement), &id, &original);
        if (result == TSUCHI_OK) next = reinterpret_cast<decltype(replacement)>(original);
        return result == TSUCHI_OK;
    };
    const bool course = hook(t::data::academy_local_course_refresh, course_refresh, next_course_refresh);
    const bool details = hook(t::data::academy_local_course_details, course_details, next_course_details);
    const bool network = hook(t::data::academy_local_preflight, preflight, next_preflight);
    const bool schedule = hook(t::data::academy_local_schedule_refresh, schedule_refresh, next_schedule_refresh);
    bool provider = false;
    if (native->inline_hook && t::data::academy_local_provider_gate != t::absent_value) {
        uint64_t id = 0;
        void* original = nullptr;
        provider = native->inline_hook(self, t::data::academy_local_provider_gate,
            uint32_t(t::constant::academy_local_provider_instruction), reinterpret_cast<void*>(provider_decision), &id, &original) == TSUCHI_OK;
        if (provider) next_provider_decision = reinterpret_cast<void (*)(TSUCHI_InlineContext*)>(original);
    }
    bool admitted = false;
    if (native->inline_hook && t::data::academy_local_admission_gate != t::absent_value) {
        uint64_t id = 0;
        void* original = nullptr;
        admitted = native->inline_hook(self, t::data::academy_local_admission_gate,
            uint32_t(t::constant::academy_local_admission_instruction), reinterpret_cast<void*>(admission), &id, &original) == TSUCHI_OK;
        if (admitted) next_admission = reinterpret_cast<void (*)(TSUCHI_InlineContext*)>(original);
    }
    ready = course && details && network && schedule && provider && admitted;
}
}
