#if defined(NH_LIVE_CHECK) || defined(NH_HOST_TEST)
#include "../../check.native-hooks-base/common.h"
/* A removed callback may still be finishing on a game worker. Every install
 * generation keeps its own immutable continuations and distinct wrappers. */
static TSUCHI_Hook hooks[3][2];
static uint64_t observed_payload;
static float observed_value;
static const char *const names[] = {"setup", "chain_order", "ordinary", "base_removed", "remove", "restart", "startup_cleanup_armed"};

static void handle(unsigned generation, unsigned target, TSUCHI_InlineContext *context) {
    const NH_Handler next = (NH_Handler)hooks[generation][target].original;
    const bool probe = nh_probe(context);
    if (probe) {
        ++nh.calls[target];
        uint64_t *x = (uint64_t *)context->registers;
        nh_set_float(context, nh_float(context) * 2.0f);
        x[1] = x[1] * 10 + 2;
        if (x[2] & NH_REDIRECT) x[17] = (uint64_t)(uintptr_t)&nh_result;
    }
    next(context);
    if (probe) {
        observed_payload = ((const uint64_t *)context->registers)[1];
        observed_value = nh_float(context);
    }
}
static void cosine_initial(TSUCHI_InlineContext *context) { handle(0, 0, context); }
static void sine_initial(TSUCHI_InlineContext *context) { handle(0, 1, context); }
static void cosine_restart(TSUCHI_InlineContext *context) { handle(1, 0, context); }
static void sine_restart(TSUCHI_InlineContext *context) { handle(1, 1, context); }
static void cosine_armed(TSUCHI_InlineContext *context) { handle(2, 0, context); }
static void sine_armed(TSUCHI_InlineContext *context) { handle(2, 1, context); }
static bool install(unsigned generation) {
    const NH_Handler callbacks[3][2] = {{cosine_initial, sine_initial}, {cosine_restart, sine_restart}, {cosine_armed, sine_armed}};
    return tsuchi_expect_result(nh_install(0, callbacks[generation][0], &hooks[generation][0]), TSUCHI_OK, "join cosine target") &&
           tsuchi_expect_result(nh_install(1, callbacks[generation][1], &hooks[generation][1]), TSUCHI_OK, "join sine target");
}

TSUCHI_Result tsuchi_mod_start(const TSUCHI_Host *host, TSUCHI_Mod *self) {
    tsuchi_check_start(host, self); nh_plan(names, sizeof names / sizeof *names);
    tsuchi_check_begin("setup");
    if (!tsuchi_expect(nh_prepare(host, self), "native-hook API and identified main code")) {
        tsuchi_check_end(); return TSUCHI_NOT_READY;
    }
    bool ready = true;
    for (unsigned i = 0; i < 2; ++i) {
        ready = tsuchi_expect_result(host->hook->check_code(self, nh_targets[i].entry, nh_targets[i].words, 12), TSUCHI_OK, "the preceding veneer instructions stay original") && ready;
        ready = tsuchi_expect_result(host->hook->check_code(self, nh_site(i), &nh_targets[i].words[3], 4), TSUCHI_BUSY, "read-only checks see the published branch") && ready;
    }
    if (ready) ready = install(0);
    tsuchi_check_end();
    if (!ready) return TSUCHI_NOT_READY;
    tsuchi_check_begin("chain_order");
    for (unsigned i = 0; i < 2; ++i) {
        tsuchi_expect_near(nh_call(i, 0.25f, NH_MAGIC, 0, NH_REDIRECT), 22.5, 0, "B then A restores both integer and vector arguments");
        tsuchi_expect_number(observed_payload, 21, "B's original continued into A exactly once");
        tsuchi_expect_near(observed_value, 1.5, 0, "B doubled before A added");
        tsuchi_expect_number(nh.calls[i], 1, "one B callback on each shared target");
        const float wanted = nh_call(i, 1.5f, 0, 0, 0);
        tsuchi_expect_near(nh_call(i, 0.25f, NH_MAGIC, 0, 0), wanted, 0.00001, "shared handlers continue into the genuine math function");
    }
    tsuchi_check_end();
    tsuchi_check_begin("ordinary"); nh_ordinary(); tsuchi_check_end();
    tsuchi_check_begin("base_removed");
    tsuchi_expect_near(nh_call(0, 0.25f, NH_MAGIC, 0, NH_REDIRECT | NH_REMOVE_BASE), 22.5, 0, "the removed in-flight A handler finishes through its continuation");
    for (unsigned i = 0; i < 2; ++i)
        tsuchi_expect_near(nh_call(i, 0.25f, NH_MAGIC, 0, NH_REDIRECT), 2.5, 0, "B remains on both targets after A removes itself");
    tsuchi_check_end();
    tsuchi_check_begin("remove"); tsuchi_expect(nh_remove(hooks[0]), "remove B's two handlers");
    nh_stock(0, 0.25f, 0.9689124217f); nh_stock(1, 0.25f, 0.2474039593f); tsuchi_check_end();
    tsuchi_check_begin("restart");
    if (install(1)) for (unsigned i = 0; i < 2; ++i)
        tsuchi_expect_near(nh_call(i, 0.25f, NH_MAGIC, 0, NH_REDIRECT), 2.5, 0, "reinstall uses the same target and a fresh continuation");
    tsuchi_expect(nh_remove(hooks[1]), "remove the reinstalled handlers"); tsuchi_check_end();
    tsuchi_check_begin("startup_cleanup_armed");
    const TSUCHI_Result first = nh_install(0, cosine_armed, &hooks[2][0]);
    tsuchi_expect_result(first, TSUCHI_OK, "one handler belongs to this still-starting mod");
    const TSUCHI_Result second = host->hook->wrap_inline(self, nh_site(1), nh_instruction(1) ^ 4, (void *)sine_armed, &hooks[2][1]);
    tsuchi_expect_result(second, TSUCHI_BUSY, "changed original instruction refuses the second hook");
    tsuchi_check_end_with("returning not_ready with the first hook owned; the observer must prove handler inactivity after startup failure");
    /* Deliberately no manual remove. The ordinary compiled-mod loader must stop
     * this mod and release its first hook before starting the observer. */
    return TSUCHI_NOT_READY;
}
#else
#include "tsuchi/tsuchi.h"
TSUCHI_Result tsuchi_mod_start(const TSUCHI_Host *host, TSUCHI_Mod *self) {
    (void)host; (void)self; return TSUCHI_UNSUPPORTED;
}
#endif
