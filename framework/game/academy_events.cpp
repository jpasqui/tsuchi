// Native hooks record observations. Script callbacks run on the next engine frame.
#include "academy_internal.h"
#include "academy_service.h"
#include "frame.h"
#include "game.h"
#include "hooks.h"
#include "mods.h"
#include "objects.h"
#include "registry.h"
#include "tsuchi/hook.h"

namespace tsuchi {
namespace {
namespace t = game::table;

void (*next_menu_update)(void*, float) = nullptr;
void (*next_exam_finish)(TSUCHI_InlineContext*) = nullptr;
void (*next_points_start)(TSUCHI_InlineContext*) = nullptr;
void (*next_time_start)(TSUCHI_InlineContext*) = nullptr;
uint64_t menu_widget = 0, visible_widget = 0;
struct Attempt {
    uint64_t save;
    bool started;
    TSUCHI_AcademyExamStarted start;
    TSUCHI_AcademyExamFinished finish;
};
Attempt attempts[16];
uint32_t attempt_count = 0;
const char* finish_issue = nullptr;

uint64_t active_save() {
    const uint64_t instance = game::global<uint64_t>(t::data::game_instance);
    return instance ? game::read_or<uint64_t>(instance + t::game_instance::academy_save) : 0;
}

// The site is on the path that counts an exam attempt, past the game's own check of the practice flag its exam
// predicate reads. A practice or training result takes another path and never comes here.
void exam_finish(TSUCHI_InlineContext* registers) {
    next_exam_finish(registers);
    if (!on_game_thread() || !registers || !registers->registers || attempt_count == 16) return;
    const auto* x = static_cast<const uint64_t*>(registers->registers);
    const uint64_t context = game::global<uint64_t>(t::data::game_context);
    if (!context) return;
    const uint64_t report = x[21];
    TSUCHI_AcademyExamFinished event{};
    uint8_t course = 0, cleared = 0, rule = 0;
    if (!game::read(context + t::academy_context::course, course) || course > 71 ||
        !game::read(context + t::game_context::rule, rule) ||
        !game::read(report + t::academy_end_report::cleared, cleared)) {
        finish_issue = "Academy finish report could not be read";
        return;
    }
    event.timed = rule == uint8_t(t::constant::academy_time_rule);
    if (event.timed) {
        // ExtraCave's report supplies the remaining clock here. SpecialCave's
        // points report does not initialize this field; the native result
        // processor only reads it on the timed branch as well.
        if (!game::read(report + t::academy_end_report::elapsed, event.elapsed) ||
            !(event.elapsed >= 0) || !finite(event.elapsed)) {
            finish_issue = "Academy timed finish report has an invalid clock";
            return;
        }
        event.score = event.elapsed >= 2147483648.0f ? INT32_MAX : int32_t(event.elapsed);
    } else if (rule != uint8_t(t::constant::academy_points_rule) ||
               !game::read(report + t::academy_end_report::score, event.score)) {
        finish_issue = "Academy points finish report could not be read";
        return;
    }
    event.course = course;
    event.medal = uint32_t(x[11]);
    if (event.medal > 5) {
        finish_issue = "Academy finish report has an invalid medal";
        return;
    }
    // The native result path compares the outcome byte with 1. Other outcome
    // codes describe attempts that did not clear; this field is not a bool.
    event.cleared = cleared == 1;
    const uint64_t save = active_save();
    if (save) attempts[attempt_count++] = {save, false, {}, event};
    else finish_issue = "Academy finish report has no active save";
}

void exam_start(bool timed) {
    if (!on_game_thread() || attempt_count == 16 || !t::bound(t::fn::academy_is_exam)) return;
    const uint64_t context = game::global<uint64_t>(t::data::game_context);
    uint8_t course = 0, rule = 0;
    if (!context || !game::read(context + t::academy_context::course, course) || course > 71 ||
        !game::read(context + t::game_context::rule, rule) ||
        rule != uint8_t(timed ? t::constant::academy_time_rule : t::constant::academy_points_rule) ||
        !t::fn::academy_is_exam(reinterpret_cast<void*>(context))) return;
    const uint64_t save = active_save();
    if (save) {
        academy_prepare_score_submission();
        attempts[attempt_count++] = {save, true, {course, timed}, {}};
    }
}

void points_start(TSUCHI_InlineContext* registers) {
    next_points_start(registers);
    exam_start(false);
}

void time_start(TSUCHI_InlineContext* registers) {
    next_time_start(registers);
    exam_start(true);
}

void menu_update(void* state, float seconds) {
    next_menu_update(state, seconds);
    if (!on_game_thread()) return;
    const uint64_t parent = game::read_or<uint64_t>(reinterpret_cast<uint64_t>(state) + t::academy_menu_state::parent);
    const uint64_t widget = parent ? game::read_or<uint64_t>(parent + t::academy_menu_presenter::widget) : 0;
    menu_widget = game::to_handle(widget);
}

void observe_events() {
    if (finish_issue) {
        mod_note(kFrameworkLogId, finish_issue);
        finish_issue = nullptr;
    }
    Attempt batch[16];
    const uint32_t count = attempt_count;
    for (uint32_t i = 0; i < count; ++i) batch[i] = attempts[i];
    attempt_count = 0;
    const uint64_t widget = game::from_handle(menu_widget);
    uint64_t visible = 0;
    if (widget && t::bound(t::fn::ui_widget_in_viewport) && t::bound(t::fn::ui_get_visibility) &&
        t::fn::ui_widget_in_viewport(reinterpret_cast<void*>(widget))) {
        const uint32_t visibility = t::fn::ui_get_visibility(reinterpret_cast<void*>(widget));
        // ESlateVisibility: Visible, HitTestInvisible, SelfHitTestInvisible.
        if (visibility == 0 || visibility == 3 || visibility == 4) visible = menu_widget;
    }
    const bool opened = visible && visible != visible_widget;
    visible_widget = visible;
    if (opened) academy::emit_opened({{visible}});
    for (uint32_t i = 0; i < count; ++i) {
        if (active_save() != batch[i].save) continue;
        if (batch[i].started) academy::emit_exam_started(batch[i].start);
        else academy::emit_exam_finished(batch[i].finish);
    }
}
}

void academy_event_hooks_start(TSUCHI_Mod* self) {
    bool installed = false;
    if (t::bound(t::fn::academy_menu_update_stock)) {
        uint64_t id = 0;
        void* original = nullptr;
        if (hook_wrap(self, game::main_base() + t::data::academy_menu_update_entry,
                  reinterpret_cast<uint64_t>(t::fn::academy_menu_update_stock),
                  reinterpret_cast<void*>(menu_update), kHookFirst | kHookNeedsStock, &id, &original) == TSUCHI_OK) {
            next_menu_update = reinterpret_cast<void (*)(void*, float)>(original);
            installed = true;
        }
    }
    const auto* native = native_hooks_backend();
    if (native && native->inline_hook && uint64_t(t::data::academy_exam_finish_site) != t::absent_value) {
        uint64_t id = 0;
        void* original = nullptr;
        if (native->inline_hook(self, t::data::academy_exam_finish_site, uint32_t(t::constant::academy_exam_finish_instruction),
                                reinterpret_cast<void*>(exam_finish), &id, &original) == TSUCHI_OK) {
            next_exam_finish = reinterpret_cast<void (*)(TSUCHI_InlineContext*)>(original);
            installed = true;
        }
    }
    if (native && native->inline_hook && t::bound(t::fn::academy_is_exam)) {
        const auto start_hook = [&](uint64_t site, void (*handler)(TSUCHI_InlineContext*),
                                    void (**next)(TSUCHI_InlineContext*)) {
            if (site == t::absent_value) return;
            uint64_t id = 0;
            void* original = nullptr;
            if (native->inline_hook(self, site, uint32_t(t::constant::academy_exam_start_instruction),
                                    reinterpret_cast<void*>(handler), &id, &original) == TSUCHI_OK) {
                *next = reinterpret_cast<void (*)(TSUCHI_InlineContext*)>(original);
                installed = true;
            }
        };
        start_hook(t::data::academy_exam_points_start_site, points_start, &next_points_start);
        start_hook(t::data::academy_exam_time_start_site, time_start, &next_time_start);
    }
    if (installed) each_engine_frame(observe_events);
}
}
