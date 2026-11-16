#include <thread>
#include <string>
#include "fake_game.h"
#include "../../framework/game/academy_online.cpp"
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "line %d: %s\n", __LINE__, #x); std::exit(1); } } while (0)
namespace {
using namespace tsuchi;
namespace t = game::table;
bool allocator_ready = false, refuse_hook = false, make_busy = false;
void (*frames[4])(){}; unsigned frame_count = 0;
unsigned event_starts = 0, board_starts = 0, forwarded = 0, failures = 0, replies = 0;
uint64_t nested_callback = 0;
uint32_t reply_state = 0;
TSUCHI_Result during_upload = TSUCHI_OK;
TSUCHI_Mod *test_owner = nullptr, *other = nullptr;
uint64_t instance = 0, original_save = 0;
uint32_t delivered = 0, observed = 0;
std::vector<int32_t> seen_scores;
TSUCHI_AcademySubmission last_request{};
int mode = 0;
bool in_handler = false;
bool native_exam = true;
uint32_t score_continuations = 0;
unsigned stock_tasks = 0;
void score_ready(void*);
void (*nested_submit)(uint32_t, int32_t) = nullptr;
const char* table_path = "Other.Table";
void (*wrapped)(void*) = nullptr;
void (*request_hook)(void*, const void*, uint32_t, int32_t, const void*) = nullptr;
bool (*task_hook)(void*) = nullptr;
void unexpected_native_response(void*, void*) { CHECK(false); }
void score_ready(void* callback) {
    ++score_continuations;
    const uint64_t task = static_cast<uint64_t*>(callback)[1];
    const auto outer = fake::memory(0x40), target = fake::memory(0x20);
    fake::put<uint64_t>(outer + t::academy_local_callback::target, target);
    fake::put<uint64_t>(target, game::main_base() + t::data::academy_score_response_table);
    fake::put<uint64_t>(target + t::academy_local_callback::presenter, task);
    const auto manager = game::global<uint64_t>(t::data::academy_schedule);
    request_hook(reinterpret_cast<void*>(manager + t::academy_schedule::interface), reinterpret_cast<void*>(outer), 987, 77, nullptr);
}
bool stock_task(void* task) {
    ++stock_tasks;
    auto at = reinterpret_cast<uint64_t>(task) + t::academy_score_task::phase;
    if (fake::get<uint32_t>(at) == 2) return true;
    fake::put<uint32_t>(at, 1); // the phase-zero setup would overwrite an immediate completion
    return false;
}
void (*provider_hooks[6])(TSUCHI_InlineContext*){};
void inline_next(TSUCHI_InlineContext*) {}
TSUCHI_Result inline_provider(TSUCHI_Mod*, uint64_t at, uint32_t, void* handler, uint64_t* id, void** original) {
    if (refuse_hook) return TSUCHI_BUSY;
    CHECK(at >= 0x1200 && at < 0x1260 && (at & 15) == 0);
    provider_hooks[(at - 0x1200) / 16] = reinterpret_cast<void (*)(TSUCHI_InlineContext*)>(handler);
    *original = reinterpret_cast<void*>(inline_next);
    fake::put<uint32_t>(game::main_base() + at, 0x14000001); *id = at;
    return TSUCHI_OK;
}
void stock_upload(void*) { ++forwarded; if (make_busy) during_upload = academy_block_uploads(); }
void stock_request(void*, const void*, uint32_t, int32_t, const void*) { ++forwarded; }
void fail_result(void*, uint32_t result, uint32_t detail) {
    CHECK(result == t::constant::academy_blocked_result && detail == t::constant::academy_blocked_detail); ++failures;
}
void reply(void*, void* response) { CHECK(!in_handler); ++replies; memcpy(&reply_state, response, 4); }
void submission(void*, const TSUCHI_AcademyScoreSubmissionRequested* event) {
    ++delivered; last_request = event->request; seen_scores.push_back(event->score);
    CHECK(event->course == 4 && event->board_key == 987);
    CHECK(academy_complete_score_submission(other, event->request, true) == TSUCHI_DENIED);
    const bool before = in_handler; in_handler = true;
    if (mode == 0 || mode == 1) {
        CHECK(academy_complete_score_submission(test_owner, event->request, mode == 0) == TSUCHI_OK);
        CHECK(academy_complete_score_submission(test_owner, event->request, true) == TSUCHI_BUSY);
    } else if (mode == 3) {
        CHECK(academy_complete_score_submission(test_owner, event->request, true) == TSUCHI_OK);
        CHECK(academy_release_score_submissions(test_owner, 4) == TSUCHI_OK);
        CHECK(academy_hold_score_submissions(test_owner, 4) == TSUCHI_OK); // a new generation cannot borrow this success
    } else if (mode == 4) {
        CHECK(academy_complete_score_submission(test_owner, event->request, true) == TSUCHI_OK);
        fake::put<uint64_t>(instance + t::game_instance::academy_save, original_save + 8);
    } else if (mode == 5) {
        CHECK(academy_complete_score_submission(test_owner, event->request, true) == TSUCHI_OK);
        mod_stop(test_owner);
    } else if (mode == 6) {
        auto outer = event->request;
        mode = 0; in_handler = false;
        nested_submit(987, 23);
        in_handler = true; mode = 6;
        CHECK(last_request.id != outer.id);
        CHECK(academy_complete_score_submission(test_owner, last_request, true) == TSUCHI_NOT_FOUND);
        CHECK(academy_complete_score_submission(test_owner, outer, true) == TSUCHI_OK);
    }
    in_handler = before;
}
void observer(void*, const TSUCHI_AcademyScoreSubmissionRequested*) { ++observed; }
TSUCHI_Result hook(TSUCHI_Mod*, uint64_t at, uint32_t expected, void* function, uint64_t* id, void** original) {
    CHECK(expected == t::constant::academy_hook_pac);
    if (refuse_hook) return TSUCHI_BUSY;
    if (at == 0x1000) { wrapped = reinterpret_cast<decltype(wrapped)>(function); *original = reinterpret_cast<void*>(stock_upload); }
    else if (at == uint64_t(t::data::academy_score_task_update)) { task_hook = reinterpret_cast<decltype(task_hook)>(function); *original = reinterpret_cast<void*>(stock_task); }
    else { CHECK(at == uint64_t(t::data::academy_score_request)); request_hook = reinterpret_cast<decltype(request_hook)>(function); *original = reinterpret_cast<void*>(stock_request); }
    fake::put<uint32_t>(game::main_base() + at, 0x14000001); *id = at; return TSUCHI_OK;
}
const NativeHooks native{hook, inline_provider, nullptr, nullptr, nullptr};
}
namespace tsuchi {
void mod_note(const char*, const char*) {}
bool each_engine_frame(void (*work)()) { frames[frame_count++] = work; return true; }
void academy_event_hooks_start(TSUCHI_Mod*) { ++event_starts; }
void academy_board_hooks_start(TSUCHI_Mod*) { ++board_starts; }
void academy_local_hooks_start(TSUCHI_Mod*) {}
const NativeHooks* native_hooks_backend() { return &native; }
namespace game {
bool engine_allocator_ready() { return allocator_ready; }
TSUCHI_Result object_path(uint64_t, char* buffer, uint64_t capacity, uint64_t* needed) {
    *needed = strlen(table_path) + 1; if (*needed > capacity) return TSUCHI_CAPACITY;
    memcpy(buffer, table_path, size_t(*needed)); return TSUCHI_OK;
}
}
}
int main() {
    using namespace tsuchi; fake::start();
    t::cells[t::academy_upload_job::result.index] = 8;
    t::cells[t::academy_upload_result_table::failed.offset.index] = 24;
    t::cells[t::academy_local_callback::target.index] = 32;
    t::cells[t::academy_local_callback_table::invoke.offset.index] = 48;
    t::cells[t::data::academy_score_request.index] = 0x1100;
    t::cells[t::data::academy_score_task_update.index] = 0x1300;
    t::cells[t::academy_score_task::phase.index] = 4;
    t::cells[t::academy_local_callback::presenter.index] = 8;
    t::cells[t::data::academy_schedule.index] = 0x300;
    t::cells[t::data::game_context.index] = 0x308;
    t::cells[t::data::game_instance.index] = 0x310;
    t::cells[t::academy_context::course.index] = 0;
    t::cells[t::academy_context::board_key.index] = 8;
    t::cells[t::academy_schedule::interface.index] = 0x10;
    t::cells[t::academy_context::submission_mode.index] = 0x18;
    const auto gates = {t::data::academy_points_session_gate, t::data::academy_points_provider_gate,
        t::data::academy_points_blocked_gate, t::data::academy_time_session_gate,
        t::data::academy_time_provider_gate, t::data::academy_time_blocked_gate};
    unsigned gate = 0;
    for (auto cell : gates) t::cells[cell.index] = 0x1200 + 16 * gate++;
    t::cells[t::game_instance::academy_save.index] = 0x20;
    t::fn::academy_upload_stock = reinterpret_cast<decltype(t::fn::academy_upload_stock)>(game::main_base() + 0x1000);
    CHECK(academy_block_uploads() == TSUCHI_NOT_READY);
    refuse_hook = true; academy_hooks_start(nullptr); CHECK(academy_block_uploads() == TSUCHI_NOT_READY);
    refuse_hook = false; academy_hooks_start(nullptr); CHECK(wrapped && request_hook);
    frames[0](); CHECK(!event_starts); allocator_ready = true; frames[0](); frames[0]();
    CHECK(event_starts == 1 && board_starts == 1);
    uint64_t job = fake::memory(0x40), result = fake::memory(0x40), methods = fake::memory(0x40);
    fake::put<uint64_t>(job + t::academy_upload_job::result, result); fake::put<uint64_t>(result, methods);
    fake::put<uint64_t>(methods + t::academy_upload_result_table::failed, reinterpret_cast<uint64_t>(fail_result));
    fake::code().push_back(reinterpret_cast<uint64_t>(fail_result));
    wrapped(reinterpret_cast<void*>(job)); CHECK(forwarded == 1);
    make_busy = true; wrapped(reinterpret_cast<void*>(job)); CHECK(during_upload == TSUCHI_BUSY && !academy_uploads_blocked()); make_busy = false;
    request_hook(nullptr, nullptr, 0, 0, nullptr); CHECK(forwarded == 3);
    test_owner = mod_create("academy-json-results"); other = mod_create("academy-community-board");
    CHECK(academy_hold_score_submissions(test_owner, 72) == TSUCHI_INVALID);
    CHECK(academy_hold_score_submissions(test_owner, 4) == TSUCHI_OK);
    CHECK(academy_hold_score_submissions(other, 4) == TSUCHI_BUSY);
    CHECK(academy_release_score_submissions(other, 4) == TSUCHI_DENIED);
    TSUCHI_Subscription listener{}, other_listener{};
    CHECK(academy::table.on_score_submission_requested(test_owner, submission, nullptr, &listener) == TSUCHI_OK);
    CHECK(academy::table.on_score_submission_requested(other, observer, nullptr, &other_listener) == TSUCHI_OK);
    std::thread worker([&] { wrapped(reinterpret_cast<void*>(job)); }); worker.join(); CHECK(failures == 1 && forwarded == 3);
    const auto manager = fake::memory(0x80), context = fake::memory(0x40);
    instance = fake::memory(0x40); original_save = fake::memory(0x40);
    fake::put<uint64_t>(game::main_base() + t::data::academy_schedule, manager);
    fake::put<uint64_t>(game::main_base() + t::data::game_context, context);
    fake::put<uint64_t>(game::main_base() + t::data::game_instance, instance);
    fake::put<uint64_t>(instance + t::game_instance::academy_save, original_save);
    fake::put<uint8_t>(context + t::academy_context::course, 4); fake::put<int32_t>(context + t::academy_context::board_key, 987);
    auto callback = fake::memory(0x40), target = fake::memory(0x40), vtable = fake::memory(0x50);
    fake::put<uint64_t>(callback + t::academy_local_callback::target, target); fake::put<uint64_t>(target, vtable);
    fake::put<uint64_t>(vtable + t::academy_local_callback_table::invoke, reinterpret_cast<uint64_t>(reply)); fake::code().push_back(reinterpret_cast<uint64_t>(reply));
    auto submit = [&](uint32_t key, int32_t score) { request_hook(reinterpret_cast<void*>(manager + t::academy_schedule::interface), reinterpret_cast<void*>(callback), key, score, nullptr); };
    nested_submit = +[](uint32_t key, int32_t score) {
        const auto manager = game::global<uint64_t>(t::data::academy_schedule);
        // This test's callback stays live on main's stack through the nested request.
        request_hook(reinterpret_cast<void*>(manager + t::academy_schedule::interface), reinterpret_cast<void*>(nested_callback), key, score, nullptr);
    };
    nested_callback = callback;
    // Only the actual exam's exact connectivity continuation can reach the
    // mod-owned score request without a Nintendo connection job.
    t::fn::academy_is_exam = +[](void*) { return native_exam; };
    native_exam = false;
    academy_prepare_score_submission();
    CHECK(fake::get<uint8_t>(context + t::academy_context::submission_mode) == 0);
    native_exam = true;
    uint8_t manager_before[0x80]; memcpy(manager_before, reinterpret_cast<void*>(manager), sizeof manager_before);
    academy_prepare_score_submission();
    CHECK(fake::get<uint8_t>(context + t::academy_context::submission_mode) == 1);
    CHECK(!memcmp(manager_before, reinterpret_cast<void*>(manager), sizeof manager_before));
    for (unsigned i = 0; i < 6; ++i) {
        CHECK(provider_hooks[i]);
        uint64_t x[31]{}; x[8] = x[9] = context; x[10] = 0x81;
        TSUCHI_InlineContext registers{x, nullptr, 0, 0, 0, 0};
        provider_hooks[i](&registers);
        CHECK(i % 3 == 2 ? (x[10] == 0x80 && registers.nzcv == 0) : (x[10] == 0x81 && registers.nzcv == (uint64_t(1) << 30)));
        CHECK(!memcmp(manager_before, reinterpret_cast<void*>(manager), sizeof manager_before));
        native_exam = false; registers.nzcv = 0; x[10] = 0x81;
        provider_hooks[i](&registers);
        CHECK(x[10] == 0x81 && registers.nzcv == 0); // practice/training and unowned courses retain stock decisions
        native_exam = true; fake::put<uint8_t>(context + t::academy_context::course, 5);
        provider_hooks[i](&registers); CHECK(x[10] == 0x81 && registers.nzcv == 0);
        fake::put<uint8_t>(context + t::academy_context::course, 4);
    }
    const auto ready_methods = fake::memory(0x50), response_methods = fake::memory(0x50), task = fake::memory(0x20);
    fake::code().push_back(reinterpret_cast<uint64_t>(score_ready));
    fake::code().push_back(reinterpret_cast<uint64_t>(unexpected_native_response));
    fake::put<uint64_t>(response_methods + t::academy_local_callback_table::invoke, reinterpret_cast<uint64_t>(unexpected_native_response));
    t::cells[t::data::academy_score_ready_table.index] = ready_methods - game::main_base();
    t::cells[t::data::academy_score_ready_invoke.index] = reinterpret_cast<uint64_t>(score_ready) - game::main_base();
    t::cells[t::data::academy_score_response_table.index] = response_methods - game::main_base();
    t::cells[t::data::academy_score_response_invoke.index] = reinterpret_cast<uint64_t>(unexpected_native_response) - game::main_base();
    CHECK(task_hook && !task_hook(reinterpret_cast<void*>(task)));
    CHECK(score_continuations == 1 && stock_tasks == 0 && fake::get<uint32_t>(task + 4) == 2);
    CHECK(task_hook(reinterpret_cast<void*>(task)) && stock_tasks == 1); // native success exit remains native
    mode = 1; fake::put<uint32_t>(task + 4, 0);
    CHECK(!task_hook(reinterpret_cast<void*>(task)) && fake::get<uint32_t>(task + 4) == 3);
    mode = 2; fake::put<uint32_t>(task + 4, 0);
    CHECK(!task_hook(reinterpret_cast<void*>(task)) && fake::get<uint32_t>(task + 4) == 3);
    CHECK(!local_task && !memcmp(manager_before, reinterpret_cast<void*>(manager), sizeof manager_before));
    const auto before_task = score_continuations;
    native_exam = false;
    fake::put<uint32_t>(task + 4, 0);
    CHECK(!task_hook(reinterpret_cast<void*>(task)) && fake::get<uint32_t>(task + 4) == 1 && score_continuations == before_task);
    native_exam = true;
    fake::put<uint8_t>(context + t::academy_context::course, 5);
    fake::put<uint32_t>(task + 4, 0);
    CHECK(!task_hook(reinterpret_cast<void*>(task)) && fake::get<uint32_t>(task + 4) == 1 && score_continuations == before_task);
    fake::put<uint8_t>(context + t::academy_context::course, 4);
    mode = 0; delivered = 0; seen_scores.clear();
    submit(987, -123); CHECK(replies == 1 && reply_state == 1 && delivered == 1 && observed == 0 && seen_scores.back() == -123);
    CHECK(academy_complete_score_submission(test_owner, last_request, true) == TSUCHI_NOT_FOUND);
    mode = 1; submit(987, INT32_MIN); CHECK(reply_state != 1 && delivered == 2 && forwarded == 3 && seen_scores.back() == INT32_MIN);
    mode = 2; submit(987, 0); CHECK(reply_state != 1 && delivered == 3); // unanswered defaults to failure
    submit(999, 12); CHECK(delivered == 3 && reply_state != 1);
    mode = 3; submit(987, 12); CHECK(reply_state != 1); // release and rehold invalidated completion
    mode = 4; submit(987, 13); CHECK(reply_state != 1);
    fake::put<uint64_t>(instance + t::game_instance::academy_save, original_save);
    mode = 6; submit(987, 14); CHECK(reply_state == 1 && !active_submission && submission_depth == 0);
    CHECK(academy_release_score_submissions(test_owner, 4) == TSUCHI_OK);
    const auto before_release = delivered; submit(987, 1); CHECK(delivered == before_release && reply_state != 1 && academy_uploads_blocked());
    CHECK(academy_hold_score_submissions(test_owner, 4) == TSUCHI_OK);
    mode = 5; submit(987, 1); CHECK(reply_state != 1 && !mod_running(test_owner));
    const auto before_stop = delivered; submit(987, 1); CHECK(delivered == before_stop && reply_state != 1);
    fake::put<uint32_t>(game::main_base() + 0x1000, 0); CHECK(academy_block_uploads() == TSUCHI_NOT_READY);
    fake::unmap(result); wrapped(reinterpret_cast<void*>(job)); CHECK(failures == 1 && forwarded == 3);
    std::puts("academy upload guard checks passed");
}
