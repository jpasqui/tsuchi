// No engine function hooks in host fixtures that have no made-up engine: hook.engine_function is not ready.
#include "engine_hooks.h"
namespace tsuchi {
TSUCHI_Result engine_read(const EngineCall&, const char*, TSUCHI_Value*) { return TSUCHI_NOT_FOUND; }
TSUCHI_Result engine_text(const EngineCall&, const char*, char*, uint64_t, uint64_t*) { return TSUCHI_NOT_FOUND; }
TSUCHI_Result engine_field_at(const EngineCall&, const char*, uint32_t, char*, uint64_t, uint64_t*) { return TSUCHI_NOT_FOUND; }
TSUCHI_Result engine_result_name(const EngineCall&, char*, uint64_t, uint64_t*) { return TSUCHI_NOT_FOUND; }
TSUCHI_Result engine_write(const EngineCall&, const EngineWrite*, uint32_t) { return TSUCHI_NOT_READY; }
const char* engine_object_kind(uint64_t) { return "object"; }
TSUCHI_Result engine_hook(TSUCHI_Mod*, const char*, const EngineClient&, void*, uint64_t*) { return TSUCHI_NOT_READY; }
TSUCHI_Result engine_hook_remove(TSUCHI_Mod*, uint64_t) { return TSUCHI_NOT_FOUND; }
}
