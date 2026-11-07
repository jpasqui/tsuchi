/* A compiled mod that refuses to start. It listens for frames first: what it made has to be undone before its
 * code is unloaded, or the next frame would call code that is no longer there. */
#include "tsuchi/tsuchi.h"

static void on_step(void *user, const TSUCHI_TimeStep *event) {
    (void)user, (void)event;
}

TSUCHI_Result tsuchi_mod_start(const TSUCHI_Host *host, TSUCHI_Mod *mod) {
    TSUCHI_Subscription subscription;
    if (host->time->on_step(mod, on_step, 0, &subscription) != TSUCHI_OK) return TSUCHI_INVALID;
    host->mod->log(mod, "refuser ran");
    return TSUCHI_NOT_READY;
}
