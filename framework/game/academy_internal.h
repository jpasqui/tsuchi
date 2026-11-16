#pragma once
#include <stdint.h>
#include "tsuchi/core.h"
#include "tsuchi/academy.h"

namespace tsuchi {
bool academy_mod_schedule();
void academy_deliver(uint64_t manager);
bool academy_delivery_ready();
void academy_backend_start();
bool academy_course_matches(uint32_t course, uint64_t address);
void academy_hooks_start(TSUCHI_Mod* self);
void academy_event_hooks_start(TSUCHI_Mod* self);
void academy_board_hooks_start(TSUCHI_Mod* self);
void academy_local_hooks_start(TSUCHI_Mod* self);
TSUCHI_Result academy_hold_board(TSUCHI_Mod* mod, uint32_t course, const char* json);
TSUCHI_Result academy_release_board(TSUCHI_Mod* mod, uint32_t course);
TSUCHI_Result academy_board_state(TSUCHI_Mod* mod, uint32_t course, TSUCHI_AcademyBoardState* out);
TSUCHI_Result academy_open_board(TSUCHI_Mod* mod, uint32_t course);
TSUCHI_Result academy_hold_score_submissions(TSUCHI_Mod* mod, uint32_t course);
TSUCHI_Result academy_release_score_submissions(TSUCHI_Mod* mod, uint32_t course);
TSUCHI_Result academy_complete_score_submission(TSUCHI_Mod* mod, TSUCHI_AcademySubmission request, bool succeeded);
// Blocks Nintendo score uploads before an Academy change. TSUCHI_BUSY, with no block kept, while an upload
// that began earlier is still running.
TSUCHI_Result academy_block_uploads();
bool academy_uploads_blocked();
void academy_prepare_score_submission();
// Gives back a block the caller was the first to take, when its own change then failed and changed nothing.
void academy_unblock_uploads();
TSUCHI_Result academy_before_table_change(uint64_t table);
TSUCHI_Result academy_hold_schedule(TSUCHI_Mod* mod, const char* json);
TSUCHI_Result academy_release_schedule(TSUCHI_Mod* mod);
TSUCHI_Result academy_hold_unlocked_periods(TSUCHI_Mod* mod, TSUCHI_FieldHold* out);
}
