// The native score job reports failure through its result object when uploads
// are blocked. It never enters the NPLN client on that path.
#include <string.h>
#include "academy_internal.h"
#include "families.h"
#include "frame.h"
#include "game.h"
#include "hooks.h"
#include "objects.h"
#include "mods.h"
#include "reflect.h"
#include "academy_service.h"
#include "tsuchi/hook.h"

namespace tsuchi {
namespace {
namespace t = game::table;

uint32_t blocked = 0, in_flight = 0;
bool installed = false;
void (*next_upload)(void*) = nullptr;
void (*next_request)(void*, const void*, uint32_t, int32_t, const void*) = nullptr;
uint32_t worker_patch = 0, request_patch = 0;
bool (*next_score_task)(void*) = nullptr;
uint32_t task_patch = 0;
struct LocalTask { LocalTask* previous; uint64_t task; bool completed = false; };
LocalTask* local_task = nullptr;
void (*next_provider_gate[6])(TSUCHI_InlineContext*){};
uint32_t provider_patches[6]{};
const Kind kScoreSubmissions{"academy_score_submissions"};
struct ScoreRoute { TSUCHI_Mod* owner = nullptr; uint64_t generation = 0; };
ScoreRoute score_routes[72];
struct Submission {
    Submission* previous;
    TSUCHI_Mod* owner;
    uint64_t id, save, generation;
    uint32_t course;
    bool answered = false, succeeded = false;
};
Submission* active_submission = nullptr;
uint64_t next_submission = 1;
uint32_t submission_depth = 0;
TSUCHI_Mod* native_owner = nullptr;
bool native_pending = false, native_registered = false;

uint64_t active_save() {
    const auto instance = game::global<uint64_t>(t::data::game_instance);
    return instance ? game::read_or<uint64_t>(instance + t::game_instance::academy_save) : 0;
}

void start_native_hooks() {
    if (!native_pending || !game::engine_allocator_ready()) return;
    academy_hooks_start(native_owner); // code aliases may not have been available during startup
    if (!installed) return;
    native_pending = false;
    academy_local_hooks_start(native_owner);
    academy_event_hooks_start(native_owner);
    academy_board_hooks_start(native_owner);
}

// This thread counts itself and then reads the block; the game thread sets the block and then reads the count.
// Both orders are sequentially consistent, so one of the two always sees the other.
void upload(void* job) {
    __atomic_add_fetch(&in_flight, 1u, __ATOMIC_SEQ_CST);
    if (!__atomic_load_n(&blocked, __ATOMIC_SEQ_CST) && next_upload) {
        next_upload(job);
    } else {
        const uint64_t result = game::read_or<uint64_t>(reinterpret_cast<uint64_t>(job) + t::academy_upload_job::result);
        const auto fail = result ? game::method(result, t::academy_upload_result_table::failed) : nullptr;
        if (fail) fail(reinterpret_cast<void*>(result), uint32_t(t::constant::academy_blocked_result), uint32_t(t::constant::academy_blocked_detail));
        // An unreadable result cannot be completed, but must never fall through
        // to the network operation after a modification.
    }
    __atomic_sub_fetch(&in_flight, 1u, __ATOMIC_SEQ_CST);
}

uint64_t upload_site() { return reinterpret_cast<uint64_t>(t::fn::academy_upload_stock) - game::main_base(); }

bool intercepts_owned() {
    const uint64_t sites[] = {t::data::academy_points_session_gate, t::data::academy_points_provider_gate,
        t::data::academy_points_blocked_gate, t::data::academy_time_session_gate,
        t::data::academy_time_provider_gate, t::data::academy_time_blocked_gate};
    for (unsigned i = 0; i < 6; ++i)
        if (!provider_patches[i] || game::read_or<uint32_t>(game::main_base() + sites[i]) != provider_patches[i]) return false;
    return installed && worker_patch && request_patch && task_patch &&
        game::read_or<uint32_t>(game::main_base() + t::data::academy_score_task_update) == task_patch &&
        game::read_or<uint32_t>(reinterpret_cast<uint64_t>(t::fn::academy_upload_stock)) == worker_patch &&
        game::read_or<uint32_t>(game::main_base() + t::data::academy_score_request) == request_patch;
}

uint64_t owned_exam_context() {
    if (!on_game_thread() || !academy_uploads_blocked() || !intercepts_owned() || !active_save()) return 0;
    const uint64_t context = game::global<uint64_t>(t::data::game_context);
    const uint8_t course = context ? game::read_or<uint8_t>(context + t::academy_context::course, 255) : 255;
    return course < 72 && mod_usable(score_routes[course].owner) && t::bound(t::fn::academy_is_exam) &&
        t::fn::academy_is_exam(reinterpret_cast<void*>(context)) ? context : 0;
}

template <unsigned Index>
void provider_gate(TSUCHI_InlineContext* registers) {
    next_provider_gate[Index](registers);
    if (!registers || !registers->registers) return;
    auto* x = static_cast<uint64_t*>(registers->registers);
    const uint64_t context = owned_exam_context();
    if (!context || x[Index < 3 ? 8 : 9] != context ||
        game::read_or<uint8_t>(context + t::academy_context::submission_mode) != 1) return;
    // These exact branches guard Nintendo's availability and refusal state.
    // The selected exam has an owned provider instead. Adapt only this decision
    // in the register frame; manager/account state is neither written nor faked.
    if constexpr (Index % 3 == 2) x[10] &= ~uint64_t(1);
    else registers->nzcv |= uint64_t(1) << 30; // the native B.NE sees equality
}

// Bounded integers for startup diagnostics. Submission serialization belongs to mods.
void number(char*& at, int64_t value) {
    uint64_t magnitude = value < 0 ? uint64_t(-(value + 1)) + 1 : uint64_t(value);
    if (value < 0) *at++ = '-';
    char digits[20]; unsigned count = 0;
    do { digits[count++] = char('0' + magnitude % 10); magnitude /= 10; } while (magnitude);
    while (count) *at++ = digits[--count];
}
void literal(char*& at, const char* text) { while (*text) *at++ = *text++; }

bool submission_valid(const Submission& request) {
    const ScoreRoute& route = score_routes[request.course];
    return mod_usable(request.owner) && route.owner == request.owner &&
        route.generation == request.generation && active_save() == request.save;
}

// The callback remains on this native call's stack. Only the owning mod hears
// the request, on the engine thread; no native object is retained across frames.
void score_request(void* interface, const void* callback, uint32_t key, int32_t score, const void* options) {
    __atomic_add_fetch(&in_flight, 1u, __ATOMIC_SEQ_CST);
    if (!academy_uploads_blocked()) {
        next_request(interface, callback, key, score, options);
        __atomic_sub_fetch(&in_flight, 1u, __ATOMIC_SEQ_CST);
        return;
    }
    const uint64_t target = callback ? game::read_or<uint64_t>(reinterpret_cast<uint64_t>(callback) + t::academy_local_callback::target) : 0;
    const auto invoke = target ? game::method(target, t::academy_local_callback_table::invoke) : nullptr;
    bool succeeded = false;
    uint8_t course = 255;
    int32_t selected = -1;
    const auto context = game::global<uint64_t>(t::data::game_context);
    const auto manager = game::global<uint64_t>(t::data::academy_schedule);
    const auto save = active_save();
    if (on_game_thread() && invoke && context && manager && save &&
        reinterpret_cast<uint64_t>(interface) == manager + t::academy_schedule::interface &&
        game::read(context + t::academy_context::course, course) && course < 72 &&
        game::read(context + t::academy_context::board_key, selected) && uint32_t(selected) == key) {
        const ScoreRoute route = score_routes[course];
        if (route.owner && mod_usable(route.owner) && next_submission && submission_depth < 16) {
            Submission request{active_submission, route.owner, next_submission++, save, route.generation, course};
            active_submission = &request;
            ++submission_depth;
            academy::emit_score_submission_requested_to(route.owner, {{request.id}, course, key, score});
            active_submission = request.previous;
            --submission_depth;
            succeeded = request.answered && request.succeeded && submission_valid(request);
        }
    }
    const uint64_t completion_task = target ? game::read_or<uint64_t>(target + t::academy_local_callback::presenter) : 0;
    if (local_task && completion_task == local_task->task &&
        game::read_or<uint64_t>(target) == game::main_base() + t::data::academy_score_response_table &&
        reinterpret_cast<uint64_t>(invoke) == game::main_base() + t::data::academy_score_response_invoke) {
        // This local task did not allocate Nintendo's connectivity/busy job.
        // Complete its native response phase from the actual writer outcome,
        // without consuming another operation's busy-window counter.
        local_task->completed = game::write(completion_task + t::academy_score_task::phase, uint32_t(succeeded ? 2 : 3));
    } else if (invoke) {
        alignas(8) uint8_t reply[48]{};
        const uint32_t state = succeeded ? 1 : uint32_t(t::constant::academy_blocked_result);
        memcpy(reply, &state, sizeof state);
        invoke(reinterpret_cast<void*>(target), reply);
    }
    __atomic_sub_fetch(&in_flight, 1u, __ATOMIC_SEQ_CST);
}

bool score_task(void* task) {
    const uint64_t context = owned_exam_context();
    const uint64_t address = reinterpret_cast<uint64_t>(task);
    if (!context || !address || game::read_or<uint8_t>(context + t::academy_context::submission_mode) != 1 ||
        game::read_or<uint32_t>(address + t::academy_score_task::phase, UINT32_MAX) != 0)
        return next_score_task(task);
    // The stock phase-zero update starts the score preflight plus Nintendo's
    // auxiliary profile uploads, then writes phase 1 after returning. A local
    // synchronous completion there would be overwritten and wait forever.
    // Invoke the same native score/key continuation for this task directly;
    // later updates use the original phase-2 success / phase-3 failure paths.
    LocalTask pending{local_task, address};
    local_task = &pending;
    alignas(8) uint64_t callback[2]{game::main_base() + t::data::academy_score_ready_table, address};
    const auto invoke = reinterpret_cast<void (*)(void*)>(game::main_base() + t::data::academy_score_ready_invoke);
    invoke(callback);
    local_task = pending.previous;
    if (!pending.completed) game::write(address + t::academy_score_task::phase, uint32_t(3));
    return false;
}

void end_score_submissions(void*, uint64_t key) {
    if (key && key <= 72) {
        score_routes[key - 1].owner = nullptr;
        ++score_routes[key - 1].generation;
    }
}
}

bool academy_uploads_blocked() { return __atomic_load_n(&blocked, __ATOMIC_SEQ_CST) != 0; }

void academy_prepare_score_submission() {
    const uint64_t context = owned_exam_context();
    // Native course launch initializes this per-attempt provider choice from
    // Nintendo's connection. A held route supplies that provider locally. The
    // next native course launch initializes it again, including practice.
    if (context && game::write(context + t::academy_context::submission_mode, uint8_t(1)))
        mod_note(kFrameworkLogId, "Academy exam uses the mod's native score provider");
}

TSUCHI_Result academy_block_uploads() {
    if (!intercepts_owned()) return TSUCHI_NOT_READY;
    // Scores and queued jobs can outlive the mod. Once a change is made behind the block, uploads remain
    // blocked until process exit, including after a schedule or course is restored.
    if (__atomic_exchange_n(&blocked, 1u, __ATOMIC_SEQ_CST)) return TSUCHI_OK; // every upload since took the refusal
    if (!__atomic_load_n(&in_flight, __ATOMIC_SEQ_CST)) return TSUCHI_OK;
    // An upload that began before the block is still running. The caller changes nothing, so no block is kept.
    __atomic_store_n(&blocked, 0u, __ATOMIC_SEQ_CST);
    return TSUCHI_BUSY;
}

void academy_unblock_uploads() { __atomic_store_n(&blocked, 0u, __ATOMIC_SEQ_CST); }

TSUCHI_Result academy_hold_score_submissions(TSUCHI_Mod* mod, uint32_t course) {
    if (!mod_usable(mod) || course >= 72) return TSUCHI_INVALID;
    ScoreRoute& route = score_routes[course];
    if (route.owner && (route.owner != mod || !ownership_update_allowed(mod, kScoreSubmissions, course + 1))) return TSUCHI_BUSY;
    TSUCHI_Result result = reserve_owned(mod, route.owner ? 0 : 1);
    const bool was_blocked = academy_uploads_blocked();
    if (result == TSUCHI_OK) result = academy_block_uploads();
    if (result != TSUCHI_OK) return result;
    result = own(mod, kScoreSubmissions, course + 1, end_score_submissions, nullptr, true);
    if (result != TSUCHI_OK) { if (!was_blocked) academy_unblock_uploads(); return result; }
    if (!route.owner) ++route.generation;
    route.owner = mod;
    return TSUCHI_OK;
}

TSUCHI_Result academy_release_score_submissions(TSUCHI_Mod* mod, uint32_t course) {
    if (!mod_usable(mod) || course >= 72) return TSUCHI_INVALID;
    if (!score_routes[course].owner) return TSUCHI_NOT_FOUND;
    if (score_routes[course].owner != mod) return TSUCHI_DENIED;
    if (!ownership_update_allowed(mod, kScoreSubmissions, course + 1)) return TSUCHI_BUSY;
    const auto result = disown(mod, kScoreSubmissions, course + 1);
    if (result == TSUCHI_OK) end_score_submissions(nullptr, course + 1);
    return result;
}

TSUCHI_Result academy_complete_score_submission(TSUCHI_Mod* mod, TSUCHI_AcademySubmission id, bool succeeded) {
    if (!mod_usable(mod) || !id.id) return TSUCHI_INVALID;
    for (Submission* request = active_submission; request; request = request->previous) {
        if (request->id != id.id) continue;
        if (request->owner != mod) return TSUCHI_DENIED;
        if (request->answered) return TSUCHI_BUSY;
        if (!submission_valid(*request)) return TSUCHI_NOT_READY;
        request->answered = true;
        request->succeeded = succeeded;
        return TSUCHI_OK;
    }
    return TSUCHI_NOT_FOUND;
}

TSUCHI_Result academy_before_table_change(uint64_t table) {
    if (family_absent(t::family::academy)) return TSUCHI_OK;
    char path[256];
    uint64_t needed = 0;
    const TSUCHI_Result read = game::object_path(table, path, sizeof path, &needed);
    if (read == TSUCHI_CAPACITY) return TSUCHI_OK;
    if (read != TSUCHI_OK) return TSUCHI_NOT_READY;
    if (strcmp(path, "/Game/Carrot4/UI/OutGame/Dandori/DataTable/DT_DDKTestCourseInfo.DT_DDKTestCourseInfo") &&
        strcmp(path, "/Game/Carrot4/UI/OutGame/Dandori/DataTable/DT_DDKTrainingCourseInfo.DT_DDKTrainingCourseInfo")) return TSUCHI_OK;
    return academy_block_uploads();
}

void academy_hooks_start(TSUCHI_Mod* self) {
    if (!native_registered) {
        native_owner = self;
        native_registered = native_pending = each_engine_frame(start_native_hooks);
    }
    if (installed) return;
    const auto* native = native_hooks_backend();
    if (!native || !native->function || !t::bound(t::fn::academy_upload_stock) ||
        uint64_t(t::data::academy_score_request) == t::absent_value) return;
    uint64_t id = 0;
    void* original = nullptr;
    TSUCHI_Result worker_result = TSUCHI_OK, request_result = TSUCHI_OK;
    if (!next_upload && (worker_result = native->function(self, upload_site(), uint32_t(t::constant::academy_hook_pac),
                                       reinterpret_cast<void*>(upload), &id, &original)) == TSUCHI_OK) {
        next_upload = reinterpret_cast<void (*)(void*)>(original);
        worker_patch = game::read_or<uint32_t>(reinterpret_cast<uint64_t>(t::fn::academy_upload_stock));
    }
    if (!next_request && (request_result = native->function(self, t::data::academy_score_request, uint32_t(t::constant::academy_hook_pac),
                                        reinterpret_cast<void*>(score_request), &id, &original)) == TSUCHI_OK) {
        next_request = reinterpret_cast<decltype(next_request)>(original);
        request_patch = game::read_or<uint32_t>(game::main_base() + t::data::academy_score_request);
    }
    if (!next_score_task && t::data::academy_score_task_update != t::absent_value &&
        native->function(self, t::data::academy_score_task_update, uint32_t(t::constant::academy_hook_pac),
                         reinterpret_cast<void*>(score_task), &id, &original) == TSUCHI_OK) {
        next_score_task = reinterpret_cast<decltype(next_score_task)>(original);
        task_patch = game::read_or<uint32_t>(game::main_base() + t::data::academy_score_task_update);
    }
    const uint64_t sites[] = {t::data::academy_points_session_gate, t::data::academy_points_provider_gate,
        t::data::academy_points_blocked_gate, t::data::academy_time_session_gate,
        t::data::academy_time_provider_gate, t::data::academy_time_blocked_gate};
    const uint32_t words[] = {uint32_t(t::constant::academy_points_session_instruction), uint32_t(t::constant::academy_points_provider_instruction),
        uint32_t(t::constant::academy_points_blocked_instruction), uint32_t(t::constant::academy_time_session_instruction),
        uint32_t(t::constant::academy_time_provider_instruction), uint32_t(t::constant::academy_time_blocked_instruction)};
    void (*handlers[])(TSUCHI_InlineContext*) = {provider_gate<0>, provider_gate<1>, provider_gate<2>, provider_gate<3>, provider_gate<4>, provider_gate<5>};
    bool providers = native->inline_hook != nullptr;
    for (unsigned i = 0; i < 6; ++i) {
        if (!next_provider_gate[i] && native->inline_hook && sites[i] != t::absent_value &&
            native->inline_hook(self, sites[i], words[i], reinterpret_cast<void*>(handlers[i]), &id, &original) == TSUCHI_OK) {
            next_provider_gate[i] = reinterpret_cast<void (*)(TSUCHI_InlineContext*)>(original);
            provider_patches[i] = game::read_or<uint32_t>(game::main_base() + sites[i]);
        }
        providers = providers && next_provider_gate[i];
    }
    installed = next_upload && next_request && next_score_task && providers;
    if (!installed) {
        char note[96]; char* at = note;
        literal(at, "Academy score interception unavailable: worker="); number(at, worker_result);
        literal(at, ", request="); number(at, request_result); *at = 0;
        mod_note(kFrameworkLogId, note);
    }
}
}
