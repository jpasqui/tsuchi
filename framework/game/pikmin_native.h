// Native reflected containers shared by added Pikmin admission and presentation.
#pragma once
#include <stdint.h>
#include "tsuchi/core.h"
namespace tsuchi {
TSUCHI_Result native_map_put(uint64_t object, const char* path, const void* key, uint32_t key_bytes,
                            const void* value, uint32_t value_bytes);
TSUCHI_Result native_map_get(uint64_t object, const char* path, const void* key, uint32_t key_bytes,
                            void* value, uint32_t value_bytes);
TSUCHI_Result native_pikmin_enum(uint64_t object);
}
