/* Paired with main.js to check script-first startup and one shared mod context. */
#include "tsuchi/tsuchi.h"
static const TSUCHI_Host *host;
static TSUCHI_Mod *self;
static unsigned frames;
static void step(void *user, const TSUCHI_TimeStep *event) {
    (void)user; (void)event;
    if (++frames == 30) host->mod->log(self, "compiled frames 30");
}
TSUCHI_Result tsuchi_mod_start(const TSUCHI_Host *given, TSUCHI_Mod *mod) {
    TSUCHI_Subscription subscription;
    host = given; self = mod; frames = 0;
    host->mod->log(mod, "compiled started");
    return host->time->on_step(mod, step, 0, &subscription);
}
