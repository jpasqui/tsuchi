/* Reads, replaces and restores text through the API, including a new key that keeps its last text. */
#include <string.h>
#include "tsuchi/tsuchi.h"
#include "tsuchi_check.h"

static const TSUCHI_Host *host;
static TSUCHI_Mod *self;
static TSUCHI_Subscription listening;
static char targets[512], before[4096], now[4096];
static const char *names[3];
static unsigned steps;
static const char *const every[] = {"read", "refused", "replaced", "again", "restored", "new", "left"};

static bool run(void) {
    const TSUCHI_TextService *text = host->text;
    uint64_t needed = 0;
    const TSUCHI_Result ready = text->get(self, names[0], names[1], before, sizeof before, &needed);
    if (ready == TSUCHI_NOT_READY) {
        for (unsigned i = 0; i < sizeof every / sizeof *every; ++i) tsuchi_check_waiting(every[i], "the game's text: not_ready");
        return false;
    }
    tsuchi_check_begin("read");
    tsuchi_expect_result(ready, TSUCHI_OK, "reading the named text");
    tsuchi_expect(before[0] != 0, "the named text is not empty");
    tsuchi_expect_number((int64_t)needed, (int64_t)strlen(before) + 1, "the bytes needed with the terminator");
    tsuchi_check_end();

    tsuchi_check_begin("refused");
    tsuchi_expect_result(text->get(self, names[0], "TsuchiAbsentText", now, sizeof now, &needed), TSUCHI_NOT_FOUND, "reading a key the game does not have");
    tsuchi_expect_result(text->get(self, "", names[1], now, sizeof now, &needed), TSUCHI_INVALID, "an empty namespace");
    tsuchi_check_end();

    tsuchi_check_begin("replaced");
    tsuchi_expect_result(text->override(self, names[0], names[1], "Tsuchi Check Name"), TSUCHI_OK, "replacing the text");
    tsuchi_expect_result(text->get(self, names[0], names[1], now, sizeof now, &needed), TSUCHI_OK, "reading the replacement");
    tsuchi_expect_text(now, "Tsuchi Check Name", "the replacement");
    tsuchi_check_end();

    tsuchi_check_begin("again");
    tsuchi_expect_result(text->override(self, names[0], names[1], "Another"), TSUCHI_OK, "replacing it again");
    tsuchi_expect_result(text->get(self, names[0], names[1], now, sizeof now, &needed), TSUCHI_OK, "reading the second replacement");
    tsuchi_expect_text(now, "Another", "the second replacement");
    tsuchi_check_end();

    tsuchi_check_begin("restored");
    tsuchi_expect_result(text->restore(self, names[0], names[1]), TSUCHI_OK, "restoring the text");
    tsuchi_expect_result(text->restore(self, names[0], names[1]), TSUCHI_NOT_FOUND, "restoring it twice");
    tsuchi_expect_result(text->get(self, names[0], names[1], now, sizeof now, &needed), TSUCHI_OK, "reading the restored text");
    tsuchi_expect_text(now, before, "what the text held before");
    tsuchi_check_end();

    tsuchi_check_begin("new");
    tsuchi_expect_result(text->override(self, names[0], names[2], "Hello there"), TSUCHI_OK, "giving text to a new key");
    tsuchi_expect_result(text->get(self, names[0], names[2], now, sizeof now, &needed), TSUCHI_OK, "reading the new key");
    tsuchi_expect_text(now, "Hello there", "the new key's text");
    tsuchi_check_end();

    tsuchi_check_begin("left");
    tsuchi_expect_result(text->restore(self, names[0], names[2]), TSUCHI_OK, "ending the replacement of the new key");
    tsuchi_expect_result(text->get(self, names[0], names[2], now, sizeof now, &needed), TSUCHI_OK, "reading the new key afterward");
    tsuchi_expect_text(now, "Hello there", "a new key keeps its last text");
    tsuchi_check_end();
    return true;
}

static void on_step(void *user, const TSUCHI_TimeStep *event) {
    (void)user, (void)event;
    if (++steps % 30 == 0 && run()) host->cancel(self, listening);
}

TSUCHI_Result tsuchi_mod_start(const TSUCHI_Host *given, TSUCHI_Mod *mod) {
    host = given, self = mod, steps = 0;
    tsuchi_check_start(host, self);
    for (unsigned i = 0; i < sizeof every / sizeof *every; ++i) tsuchi_check_plan(every[i]);
    if (tsuchi_check_lines("targets.txt", targets, sizeof targets, names, 3) != 3) {
        for (unsigned i = 0; i < sizeof every / sizeof *every; ++i) {
            tsuchi_check_begin(every[i]);
            tsuchi_expect(false, "targets.txt must name the namespace, key and new key, one per line");
            tsuchi_check_end();
        }
        return TSUCHI_OK;
    }
    if (run()) return TSUCHI_OK;
    return host->time->on_step(self, on_step, 0, &listening);
}
