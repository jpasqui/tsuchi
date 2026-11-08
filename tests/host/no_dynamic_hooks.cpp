// No game or native gateways in the language-only host fixtures.
#include "dynamic_hooks.h"
namespace tsuchi {
TSUCHI_Result parse_signature(const char*, Signature&) { return TSUCHI_NOT_READY; }
uint32_t read_text16(uint64_t, uint16_t*, uint32_t) { return 0; }
uint64_t frame_argument(const CallFrame&, const Signature&, uint32_t) { return 0; }
void set_frame_argument(CallFrame&, const Signature&, uint32_t, uint64_t) {}
uint64_t frame_result(const CallFrame&, CallType) { return 0; }
void set_frame_result(CallFrame&, CallType, uint64_t) {}
void call_with_frame(uint64_t, CallFrame&, CallType) {}
TSUCHI_Result dynamic_hook(TSUCHI_Mod*, DynamicKind, const DynamicTarget&, const Signature&, const DynamicClient&, void*, uint64_t*) { return TSUCHI_NOT_READY; }
TSUCHI_Result dynamic_remove(TSUCHI_Mod*, uint64_t) { return TSUCHI_NOT_FOUND; }
uint64_t stack_room() { return UINT64_MAX; }
uint64_t engine_frame_number() { return 0; }
bool each_dynamic_frame(void (*)()) { return true; }
}
