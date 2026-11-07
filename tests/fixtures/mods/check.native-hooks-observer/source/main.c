#if defined(NH_LIVE_CHECK) || defined(NH_HOST_TEST)
#include "../../check.native-hooks-base/common.h"
static TSUCHI_Hook hooks[2];
static const char *const names[] = {"setup", "startup_cleanup", "wrong_bytes", "reuse", "ordinary", "cleanup"};
static void handle(unsigned target, TSUCHI_InlineContext *context) {
    if (nh_probe(context)) {
        ++nh.calls[target];
        uint64_t *x = (uint64_t *)context->registers;
        nh_set_float(context, nh_float(context) + 1.0f);
        x[1] += 1;
        if (x[2] & NH_REDIRECT) x[17] = (uint64_t)(uintptr_t)&nh_result;
    }
    ((NH_Handler)hooks[target].original)(context);
}
static void cosine(TSUCHI_InlineContext *context) { handle(0, context); }
static void sine(TSUCHI_InlineContext *context) { handle(1, context); }

TSUCHI_Result tsuchi_mod_start(const TSUCHI_Host *host, TSUCHI_Mod *self) {
    tsuchi_check_start(host, self); nh_plan(names, sizeof names / sizeof *names);
    tsuchi_check_begin("setup");
    bool ready = tsuchi_expect(nh_prepare(host, self), "native-hook API and identified main code");
    if (ready) for (unsigned i = 0; i < 2; ++i)
        ready = tsuchi_expect_result(host->hook->check_code(self, nh_targets[i].entry, nh_targets[i].words, 12),
                                    TSUCHI_OK, "all preceding veneer instructions remain original before any probe") && ready;
    tsuchi_check_end(); if (!ready) return TSUCHI_NOT_READY;
    tsuchi_check_begin("startup_cleanup");
    char version[32]; uint64_t length = 0;
    tsuchi_expect_result(host->mod->version_of(self, "check.native-hooks-base", version, sizeof version, &length), TSUCHI_OK, "base mod remains loaded after removing its hooks");
    tsuchi_expect_result(host->mod->version_of(self, "check.native-hooks-chain", version, sizeof version, &length), TSUCHI_NOT_FOUND, "ordinary loader rejected and stopped the chain mod");
    nh_stock(0, 0.25f, 0.9689124217f); nh_stock(1, 0.25f, 0.2474039593f);
    tsuchi_check_end_with("failed-startup handler is inactive; requires the armed receipt and loader not_ready record, and does not measure hook storage reclamation");
    tsuchi_check_begin("wrong_bytes");
    for (unsigned i = 0; i < 2; ++i) {
        TSUCHI_Hook refused = {0};
        tsuchi_expect_result(host->hook->wrap_inline(self, nh_site(i), nh_instruction(i) ^ 4, (void *)cosine, &refused), TSUCHI_BUSY, "wrong original is refused even for a previously assigned target");
        tsuchi_expect(!refused.id.id && !refused.original, "rejected installation publishes no handle or continuation");
        tsuchi_expect_result(host->hook->check_code(self, nh_targets[i].entry, nh_targets[i].words, sizeof nh_targets[i].words), TSUCHI_BUSY, "inactive gateways still occupy the instruction until process exit");
    }
    nh_stock(0, 0.25f, 0.9689124217f); nh_stock(1, 0.25f, 0.2474039593f); tsuchi_check_end();
    tsuchi_check_begin("reuse");
    bool installed = tsuchi_expect_result(nh_install(0, cosine, &hooks[0]), TSUCHI_OK, "new owner reuses cosine target");
    installed = tsuchi_expect_result(nh_install(1, sine, &hooks[1]), TSUCHI_OK, "new owner reuses sine target") && installed;
    if (installed) for (unsigned i = 0; i < 2; ++i)
        tsuchi_expect_near(nh_call(i, 0.25f, NH_MAGIC, 0, NH_REDIRECT), 2.25, 0, "new owner's restored arguments reach its own helper");
    tsuchi_check_end();
    tsuchi_check_begin("ordinary"); nh_ordinary(); tsuchi_check_end();
    tsuchi_check_begin("cleanup"); tsuchi_expect(nh_remove(hooks), "remove all observer-owned handlers");
    nh_stock(0, 0.25f, 0.9689124217f); nh_stock(1, 0.25f, 0.2474039593f); tsuchi_check_end();
    return TSUCHI_OK;
}
#else
#include "tsuchi/tsuchi.h"
TSUCHI_Result tsuchi_mod_start(const TSUCHI_Host *host, TSUCHI_Mod *self) {
    (void)host; (void)self; return TSUCHI_UNSUPPORTED;
}
#endif
