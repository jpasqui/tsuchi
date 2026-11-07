// Native Pikmin save storage. Lifecycle callers repair after load/construction,
// and clear added rows after the game's fixed-color reset.
#pragma once
#include <stdint.h>
#include "tsuchi/core.h"
namespace tsuchi {
// Grow only, retaining larger arrays and unknown saved colors. New structs are
// made by the engine. No buffer is borrowed or restored when a mod stops.
// A failure may follow successful growth of earlier fields. Repair is retryable.
TSUCHI_Result pikmin_save_summary(uint64_t type, uint64_t base, bool clear_custom = false);
TSUCHI_Result pikmin_save_result(uint64_t type, uint64_t base, bool clear_custom = false);
TSUCHI_Result pikmin_save_story(uint64_t story, bool clear_custom = false);
// Clear only the four native follower counters for colors 19 and 21 in the
// current summary. No growth or other save fields change. Writes are retryable.
TSUCHI_Result pikmin_save_clear_follow(uint64_t story);
// No hooks are installed here. Frame repair alone cannot protect load readers.
}
