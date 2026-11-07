#if defined(NH_LIVE_CHECK) || defined(NH_HOST_TEST)
#include "../common.h"
static TSUCHI_Hook hooks[2];
static bool removed;
static const char *const names[] = {"setup", "registers", "ordinary", "self_remove"};

static void handle(unsigned target, TSUCHI_InlineContext *context) {
    const NH_Handler next = (NH_Handler)hooks[target].original;
    if (nh_probe(context)) {
        ++nh.calls[target];
        uint64_t *x = (uint64_t *)context->registers;
        nh_set_float(context, nh_float(context) + 1.0f);
        x[1] = x[1] * 10 + 1;
        if (x[2] & NH_REDIRECT) x[17] = (uint64_t)(uintptr_t)&nh_result;
        if ((x[2] & NH_REMOVE_BASE) && !removed) {
            removed = true;
            tsuchi_check_begin("self_remove");
            tsuchi_expect(nh_remove(hooks), "remove both owned hooks while the first callback is running");
            tsuchi_check_end();
        }
    }
    next(context);
}
static void cosine(TSUCHI_InlineContext *context) { handle(0, context); }
static void sine(TSUCHI_InlineContext *context) { handle(1, context); }

TSUCHI_Result tsuchi_mod_start(const TSUCHI_Host *host, TSUCHI_Mod *self) {
    tsuchi_check_start(host, self); nh_plan(names, sizeof names / sizeof *names);
    tsuchi_check_begin("setup");
    if (!tsuchi_expect(nh_prepare(host, self), "native-hook API and identified readable main code")) {
        tsuchi_check_end(); return TSUCHI_NOT_READY;
    }
    bool ready = true;
    for (unsigned i = 0; i < 2; ++i)
        ready = tsuchi_expect_result(host->hook->check_code(self, nh_targets[i].entry, nh_targets[i].words, sizeof nh_targets[i].words),
                                    TSUCHI_OK, "all four original import-veneer instructions") && ready;
    if (ready) ready = tsuchi_expect_result(nh_install(0, cosine, &hooks[0]), TSUCHI_OK, "install first inline target");
    if (ready) ready = tsuchi_expect_result(nh_install(1, sine, &hooks[1]), TSUCHI_OK, "install second inline target");
    if (ready) ready = nh_arena_layout();
    tsuchi_check_end();
    if (!ready) return TSUCHI_NOT_READY;
    tsuchi_check_begin("registers");
    for (unsigned i = 0; i < 2; ++i) {
        tsuchi_expect_near(nh_call(i, 0.25f, NH_MAGIC, 0, NH_REDIRECT), 2.25, 0,
                          "q0 input and x1 payload reach the redirected BR x17 callee");
        tsuchi_expect_number(nh.calls[i], 1, "one inline callback for the target");
        const float ordinary = nh_call(i, 1.25f, 0, 0, 0);
        tsuchi_expect_near(nh_call(i, 0.25f, NH_MAGIC, 0, 0), ordinary, 0.00001,
                          "modified q0 reaches the genuine math function through displaced BR x17");
    }
    tsuchi_check_end();
    tsuchi_check_begin("ordinary"); nh_ordinary(); tsuchi_check_end();
    tsuchi_check_waiting("self_remove", "the separate chain mod's removal request");
    return TSUCHI_OK;
}
#else
#include "tsuchi/tsuchi.h"
TSUCHI_Result tsuchi_mod_start(const TSUCHI_Host *host, TSUCHI_Mod *self) {
    (void)host; (void)self; return TSUCHI_UNSUPPORTED;
}
#endif
