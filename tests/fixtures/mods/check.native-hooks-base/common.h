/* Pure-math live probes. Only our noncanonical marker on the startup thread may
 * change registers. Nothing here reads actors, edits assets or writes a save. */
#ifndef NATIVE_HOOK_CHECK_COMMON_H
#define NATIVE_HOOK_CHECK_COMMON_H
#include "tsuchi_check.h"
#include "native_hook_targets.h"

#define NH_MAGIC UINT64_C(0xfedcba9876543210)
#define NH_REDIRECT UINT64_C(1)
#define NH_REMOVE_BASE UINT64_C(2)
typedef void (*NH_Handler)(TSUCHI_InlineContext *);
typedef float (*NH_Math)(float, uint64_t, uint64_t, uint64_t);
typedef struct NH_MemoryInfo {
    uint64_t base, size;
    uint32_t state, attribute, permission, ipc_count, device_count, padding;
} NH_MemoryInfo;
static struct {
    const TSUCHI_Host *host;
    TSUCHI_Mod *self;
    uint64_t main, thread;
    unsigned calls[2];
} nh;

#ifdef NH_HOST_TEST
uint64_t nh_host_query(NH_MemoryInfo *, uint64_t);
uint64_t nh_host_thread(void);
float nh_host_call(uint64_t, unsigned, float, uint64_t, uint64_t, uint64_t);
static inline uint64_t nh_query(NH_MemoryInfo *info, uint64_t address) { return nh_host_query(info, address); }
static inline uint64_t nh_thread(void) { return nh_host_thread(); }
#else
static inline uint64_t nh_query(NH_MemoryInfo *info, uint64_t address) {
    register uint64_t result __asm__("x0") = (uint64_t)(uintptr_t)info;
    register uint64_t at __asm__("x2") = address;
    __asm__ volatile("svc #6" : "+r"(result) : "r"(at) : "x1", "x3", "memory");
    return result;
}
static inline uint64_t nh_thread(void) {
    uint64_t thread;
    __asm__ volatile("mrs %0, tpidrro_el0" : "=r"(thread));
    return thread;
}
#endif

static inline uint64_t nh_hash(uint64_t address) {
    uint64_t hash = UINT64_C(0xcbf29ce484222325);
    const volatile unsigned char *bytes = (const volatile unsigned char *)(uintptr_t)address;
    for (unsigned i = 0; i < 16; ++i) hash = (hash ^ bytes[i]) * UINT64_C(0x100000001b3);
    return hash;
}

/* The same bounded RX-region/identity test as the runtime's find_game. The SDK
 * intentionally has no raw game-base service; this helper belongs to the check. */
static inline uint64_t nh_find_main(void) {
    uint64_t address = 0;
    for (unsigned region = 0; region < 4096; ++region) {
        NH_MemoryInfo info = {0};
        if (nh_query(&info, address) || !info.size || info.base > address || info.size > UINT64_MAX - info.base) break;
        const uint64_t end = info.base + info.size;
        if (end <= address) break;
        if (info.permission == 5 && info.size >= nh_code_size) {
            bool match = true;
            for (unsigned i = 0; match && i < sizeof nh_identity / sizeof *nh_identity; ++i)
                match = nh_hash(info.base + nh_identity[i].offset) == nh_identity[i].hash;
            if (match) return info.base;
        }
        address = end;
    }
    return 0;
}

static inline bool nh_prepare(const TSUCHI_Host *host, TSUCHI_Mod *self) {
    nh.host = host; nh.self = self; nh.thread = nh_thread();
    if (!TSUCHI_HAS(host, hook) || !TSUCHI_HAS(host->hook, wrap_inline) ||
        !TSUCHI_HAS(host->hook, check_code) || !TSUCHI_HAS(host->hook, remove)) return false;
    nh.main = nh_find_main();
    return nh.main != 0;
}

static inline uint64_t nh_site(unsigned target) { return nh_targets[target].entry + nh_branch_offset; }
static inline uint32_t nh_instruction(unsigned target) { return nh_targets[target].words[3]; }
static inline bool nh_arena_layout(void) {
#ifdef NH_HOST_TEST
    /* Host control-flow checks do not qualify the game's memory mappings. */
    return true;
#else
    uint64_t first = 0;
    bool good = true;
    for (unsigned i = 0; i < 2; ++i) {
        const uint64_t site = nh.main + nh_site(i);
        const uint32_t word = *(const volatile uint32_t *)(uintptr_t)site;
        if (!tsuchi_expect((word & UINT32_C(0xfc000000)) == UINT32_C(0x14000000), "the installed target uses one direct branch")) return false;
        int64_t units = word & UINT32_C(0x03ffffff);
        if (units & (INT64_C(1) << 25)) units -= INT64_C(1) << 26;
        const int64_t delta = units * 4;
        if (!tsuchi_expect(delta < 0 ? site >= (uint64_t)-delta : site <= UINT64_MAX - (uint64_t)delta, "gateway branch arithmetic stays in range")) return false;
        const uint64_t gateway = delta < 0 ? site - (uint64_t)-delta : site + (uint64_t)delta;
        NH_MemoryInfo info = {0};
        if (!tsuchi_expect(!nh_query(&info, gateway), "query the installed gateway mapping")) return false;
        const bool bounded = info.size == UINT64_C(0x4000) && info.base <= UINT64_MAX - info.size;
        good = tsuchi_expect(bounded && info.permission == 5 && !info.attribute, "gateway arena is exactly 16 KiB RX without writable permission") && good;
        good = tsuchi_expect(bounded && (info.base + info.size <= nh.main || info.base >= nh.main + nh_code_size), "gateway arena is outside main text") && good;
        if (!i) first = gateway;
        good = tsuchi_expect(gateway == info.base + i * 64 && gateway == first + i * 64, "both targets use adjacent slots in one arena") && good;
    }
    return good;
#endif
}
static inline TSUCHI_Result nh_install(unsigned target, NH_Handler handler, TSUCHI_Hook *hook) {
    return nh.host->hook->wrap_inline(nh.self, nh_site(target), nh_instruction(target), (void *)handler, hook);
}
static inline bool nh_remove(TSUCHI_Hook hooks[2]) {
    bool good = true;
    for (unsigned i = 0; i < 2; ++i) if (hooks[i].id.id) {
        const TSUCHI_Result result = nh.host->hook->remove(nh.self, hooks[i].id);
        if (result == TSUCHI_OK) hooks[i].id.id = 0;
        else good = false;
        /* The continuation can still be in flight, including in our callback. */
    }
    return good;
}
static inline bool nh_probe(const TSUCHI_InlineContext *context) {
    return nh_thread() == nh.thread && ((const uint64_t *)context->registers)[0] == NH_MAGIC;
}
static inline float nh_float(const TSUCHI_InlineContext *context) {
    float value;
    memcpy(&value, context->vectors, sizeof value);
    return value;
}
static inline void nh_set_float(TSUCHI_InlineContext *context, float value) {
    memcpy(context->vectors, &value, sizeof value);
}
static inline float nh_result(float value, uint64_t marker, uint64_t payload, uint64_t mode) {
    (void)mode;
    return marker == NH_MAGIC ? value + (float)payload : -10000.0f;
}
static inline float nh_call(unsigned target, float value, uint64_t marker, uint64_t payload, uint64_t mode) {
#ifdef NH_HOST_TEST
    return nh_host_call(nh.main, target, value, marker, payload, mode);
#else
    return ((NH_Math)(uintptr_t)(nh.main + nh_targets[target].entry))(value, marker, payload, mode);
#endif
}
static inline bool nh_stock(unsigned target, float value, float wanted) {
    return tsuchi_expect_near(nh_call(target, value, NH_MAGIC, 0, NH_REDIRECT), wanted, 0.00001,
                             "no handler changes the guarded stock math call");
}
static inline void nh_ordinary(void) {
    const unsigned before[2] = {nh.calls[0], nh.calls[1]};
    tsuchi_expect_near(nh_call(0, 0.25f, 0, 3, NH_REDIRECT), 0.9689124217, 0.00001, "ordinary cosine unchanged");
    tsuchi_expect_near(nh_call(1, 0.25f, 0, 3, NH_REDIRECT), 0.2474039593, 0.00001, "ordinary sine unchanged");
    tsuchi_expect(nh.calls[0] == before[0] && nh.calls[1] == before[1], "ordinary calls do not enter the probe logic");
}
static inline void nh_plan(const char *const *names, unsigned count) {
    for (unsigned i = 0; i < count; ++i) tsuchi_check_plan(names[i]);
}
#endif
