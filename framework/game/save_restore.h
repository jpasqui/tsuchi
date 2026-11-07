#pragma once
#include <stdint.h>

#include "tsuchi/save.h"

namespace tsuchi {
// `save.restore`: the saved copy (an object handle) into the game's current save of its kind.
TSUCHI_Result restore_save(uint64_t copy_handle);
}
