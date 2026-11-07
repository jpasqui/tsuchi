/* The check of `storage`: a piece is kept, read back, listed, replaced by a shorter one, and forgotten; a name
 * that leads out of the mod's data is refused; and one piece is left in the save, which the next run of the check
 * on the same save has to find. */
#include <string.h>

#include "tsuchi/tsuchi.h"
#include "tsuchi_check.h"

static const TSUCHI_Host *host;
static TSUCHI_Mod *self;
static TSUCHI_Subscription listening;
static int steps;

static const char first[] = "kept by the check", second[] = "again", stays[] = "left by the check";
static const char *const every[] = {"left", "kept", "listed", "replaced", "refused", "removed"};

/* Whether a name is among the pieces the mod keeps. */
static bool listed(const char *wanted, TSUCHI_Result *result) {
    char names[512];
    uint64_t needed = 0;
    *result = host->storage->list(self, names, sizeof names, &needed);
    if (*result != TSUCHI_OK) return false;
    for (const char *name = names; *name; name += strlen(name) + 1)
        if (!strcmp(name, wanted)) return true;
    return false;
}

/* Makes the checks. False when the save cannot be reached yet: they are made on a later step. */
static bool run(void) {
    const TSUCHI_StorageService *kept = host->storage;
    char text[64];
    uint64_t size = 0, got = 0;
    bool there = false;
    TSUCHI_Result result = kept->size(self, "check/stays.txt", &size), list_result;
    if (result == TSUCHI_BUSY || result == TSUCHI_NOT_READY) {
        for (size_t i = 0; i < sizeof every / sizeof *every; ++i) tsuchi_check_waiting(every[i], result == TSUCHI_BUSY ? "the save: busy" : "the save: not_ready");
        return false;
    }

    /* What the run before this one left. On a save that has never seen the check there is nothing. */
    const bool left = result == TSUCHI_OK;
    tsuchi_check_begin("left");
    if (left) {
        tsuchi_expect_number((int64_t)size, sizeof stays - 1, "the size of what the last run left");
        tsuchi_expect_result(kept->read(self, "check/stays.txt", 0, text, sizeof text, &got), TSUCHI_OK, "reading what the last run left");
        if (tsuchi_expect_number((int64_t)got, sizeof stays - 1, "the bytes read of it")) tsuchi_expect_bytes(text, stays, sizeof stays - 1, "what the last run left");
    } else {
        tsuchi_expect_result(result, TSUCHI_NOT_FOUND, "the size of a piece that was never kept");
    }
    tsuchi_expect_result(kept->write(self, "check/stays.txt", stays, sizeof stays - 1), TSUCHI_OK, "keeping a piece for the next run");
    tsuchi_check_end_with(left ? "kept" : "fresh");

    tsuchi_check_begin("kept");
    tsuchi_expect_result(kept->write(self, "check/hello.txt", first, sizeof first - 1), TSUCHI_OK, "keeping a piece");
    tsuchi_expect_result(kept->exists(self, "check/hello.txt", &there), TSUCHI_OK, "asking whether it is there");
    tsuchi_expect(there, "the piece is there");
    tsuchi_expect_result(kept->size(self, "check/hello.txt", &size), TSUCHI_OK, "its size");
    tsuchi_expect_number((int64_t)size, sizeof first - 1, "its size");
    tsuchi_expect_result(kept->read(self, "check/hello.txt", 0, text, sizeof text, &got), TSUCHI_OK, "reading it");
    if (tsuchi_expect_number((int64_t)got, sizeof first - 1, "the bytes read")) tsuchi_expect_bytes(text, first, sizeof first - 1, "what was read");
    tsuchi_check_end();

    tsuchi_check_begin("listed");
    tsuchi_expect(listed("check/hello.txt", &list_result), "the piece is among those listed");
    tsuchi_expect_result(list_result, TSUCHI_OK, "listing the pieces");
    tsuchi_expect(listed("check/stays.txt", &list_result), "the piece left for the next run is listed too");
    tsuchi_check_end();

    /* A shorter one in its place leaves nothing of the longer, and a part is read from within. */
    tsuchi_check_begin("replaced");
    tsuchi_expect_result(kept->write(self, "check/hello.txt", second, sizeof second - 1), TSUCHI_OK, "keeping a shorter piece under the name");
    tsuchi_expect_result(kept->size(self, "check/hello.txt", &size), TSUCHI_OK, "its size");
    tsuchi_expect_number((int64_t)size, sizeof second - 1, "its size");
    tsuchi_expect_result(kept->read(self, "check/hello.txt", 1, text, 3, &got), TSUCHI_OK, "reading three bytes from the second on");
    if (tsuchi_expect_number((int64_t)got, 3, "the bytes read")) tsuchi_expect_bytes(text, "gai", 3, "what was read");
    tsuchi_check_end();

    tsuchi_check_begin("refused");
    tsuchi_expect_result(kept->write(self, "../escape", "x", 1), TSUCHI_INVALID, "a name that leads out of the mod's data");
    tsuchi_expect_result(kept->read(self, "check/absent", 0, text, 4, &got), TSUCHI_NOT_FOUND, "reading a piece that was never kept");
    tsuchi_check_end();

    tsuchi_check_begin("removed");
    tsuchi_expect_result(kept->remove(self, "check/hello.txt"), TSUCHI_OK, "forgetting the piece");
    tsuchi_expect_result(kept->size(self, "check/hello.txt", &size), TSUCHI_NOT_FOUND, "the size of a piece that was forgotten");
    tsuchi_expect_result(kept->exists(self, "check/hello.txt", &there), TSUCHI_OK, "asking whether it is there");
    tsuchi_expect(!there, "the piece is not there");
    tsuchi_expect_result(kept->remove(self, "check/hello.txt"), TSUCHI_NOT_FOUND, "forgetting it again");
    tsuchi_expect(!listed("check/hello.txt", &list_result), "the piece is not listed any more");
    tsuchi_expect_result(list_result, TSUCHI_OK, "listing the pieces");
    tsuchi_check_end();
    return true;
}

static void on_step(void *user, const TSUCHI_TimeStep *event) {
    (void)user, (void)event;
    if (++steps % 30 == 0 && run()) host->cancel(self, listening);
}

TSUCHI_Result tsuchi_mod_start(const TSUCHI_Host *given, TSUCHI_Mod *mod) {
    host = given, self = mod, steps = 0;
    tsuchi_check_start(host, mod);
    for (size_t i = 0; i < sizeof every / sizeof *every; ++i) tsuchi_check_plan(every[i]);
    if (run()) return TSUCHI_OK;
    return host->time->on_step(mod, on_step, 0, &listening);
}
