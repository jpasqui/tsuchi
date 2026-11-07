/* Holds named tables, changes and restores their fields, and copies one row through the public API. */
#include <string.h>
#include "tsuchi/tsuchi.h"
#include "tsuchi_check.h"

static const TSUCHI_Host *host;
static TSUCHI_Mod *self;
static TSUCHI_Subscription listening;
static TSUCHI_Object table, references, replacement;
static char targets[3072];
static const char *names[8];
static unsigned steps;
static const char *const every[] = {"held", "number", "restored", "fields", "reference", "text", "row", "refused", "released"};

static bool ready(TSUCHI_Result result) { return result != TSUCHI_BUSY && result != TSUCHI_NOT_READY; }

static bool run(void) {
    const TSUCHI_DatatableService *api = host->datatable;
    TSUCHI_Result loaded = table.id ? TSUCHI_OK : host->asset->load(self, names[0], &table);
    TSUCHI_Result referred = references.id ? TSUCHI_OK : host->asset->load(self, names[4], &references);
    TSUCHI_Result replacing = replacement.id ? TSUCHI_OK : host->asset->load(self, names[7], &replacement);
    if (!ready(loaded) || !ready(referred) || !ready(replacing)) {
        for (unsigned i = 0; i < sizeof every / sizeof *every; ++i) tsuchi_check_waiting(every[i], "the named assets: busy or not_ready");
        return false;
    }
    TSUCHI_Value value = {0}, wanted = {0};
    uint64_t needed = 0;
    char path[1024] = {0}, name[256] = {0}, before[1024] = {0}, after[1024] = {0};
    double number = 0;

    tsuchi_check_begin("held");
    tsuchi_expect_result(loaded, TSUCHI_OK, "loading the table");
    tsuchi_expect_result(referred, TSUCHI_OK, "loading the table of references");
    tsuchi_expect_result(replacing, TSUCHI_OK, "loading the asset to give as a reference");
    tsuchi_expect_result(host->reflection->path(self, table, path, sizeof path, &needed), TSUCHI_OK, "the loaded table's path");
    tsuchi_expect_text(path, names[0], "the loaded table's path");
    tsuchi_check_end();

    tsuchi_check_begin("number");
    tsuchi_expect_result(api->get(self, table, names[1], names[2], &value), TSUCHI_OK, "reading the number");
    tsuchi_expect_number(value.kind, TSUCHI_VALUE_NUMBER, "the field's kind");
    number = value.as.number;
    wanted.kind = TSUCHI_VALUE_NUMBER, wanted.as.number = number + 123;
    tsuchi_expect_result(api->override(self, table, names[1], names[2], &wanted), TSUCHI_OK, "changing the number");
    tsuchi_expect_result(api->get(self, table, names[1], names[2], &value), TSUCHI_OK, "reading it back");
    tsuchi_expect_near(value.as.number, number + 123, 0.0001, "the number after the change");
    tsuchi_check_end();

    tsuchi_check_begin("restored");
    tsuchi_expect_result(api->restore(self, table, names[1], names[2]), TSUCHI_OK, "restoring the number");
    tsuchi_expect_result(api->restore(self, table, names[1], names[2]), TSUCHI_NOT_FOUND, "restoring it twice");
    tsuchi_expect_result(api->get(self, table, names[1], names[2], &value), TSUCHI_OK, "reading it afterward");
    tsuchi_expect_near(value.as.number, number, 0.0001, "the number before the change");
    tsuchi_check_end();

    tsuchi_check_begin("fields");
    unsigned count = 0;
    bool has_number = false;
    TSUCHI_Result listed = TSUCHI_NOT_FOUND;
    for (; count < 1024; ++count) {
        listed = api->field_at(self, table, names[1], "", count, name, sizeof name, &needed);
        if (listed != TSUCHI_OK) break;
        has_number = has_number || !strcmp(name, names[2]);
    }
    tsuchi_expect_result(listed, TSUCHI_NOT_FOUND, "the position past the last field");
    tsuchi_expect(count >= 3 && has_number, "at least three fields are listed, including the number");
    tsuchi_check_end();

    tsuchi_check_begin("reference");
    tsuchi_expect_result(api->get_text(self, references, names[5], names[6], before, sizeof before, &needed), TSUCHI_OK, "reading the reference's original path");
    tsuchi_expect(before[0] == '/', "the reference holds an asset path");
    tsuchi_expect_result(api->override_text(self, references, names[5], names[6], names[7]), TSUCHI_OK, "changing the path");
    tsuchi_expect_result(api->get_text(self, references, names[5], names[6], after, sizeof after, &needed), TSUCHI_OK, "reading the changed path");
    tsuchi_expect_text(after, names[7], "the changed path");
    wanted.kind = TSUCHI_VALUE_OBJECT, wanted.as.object = replacement;
    const TSUCHI_Result assigned = api->override(self, references, names[5], names[6], &wanted);
    tsuchi_expect(assigned == TSUCHI_OK || assigned == TSUCHI_INVALID, "giving an object either sets its path or refuses an object of the wrong kind");
    tsuchi_expect_result(api->get_text(self, references, names[5], names[6], after, sizeof after, &needed), TSUCHI_OK, "reading after the object was given");
    tsuchi_expect_text(after, names[7], "the assigned or unchanged reference");
    tsuchi_expect_result(api->restore(self, references, names[5], names[6]), TSUCHI_OK, "restoring the reference");
    tsuchi_expect_result(api->get_text(self, references, names[5], names[6], after, sizeof after, &needed), TSUCHI_OK, "reading the restored reference");
    tsuchi_expect_text(after, before, "the reference before the change");
    tsuchi_check_end();

    tsuchi_check_begin("text");
    tsuchi_expect_result(api->get_text(self, table, names[1], names[3], before, sizeof before, &needed), TSUCHI_OK, "reading the text field");
    tsuchi_expect_result(api->override_text(self, table, names[1], names[3], "TsuchiCheck"), TSUCHI_OK, "changing the text field");
    tsuchi_expect_result(api->get_text(self, table, names[1], names[3], after, sizeof after, &needed), TSUCHI_OK, "reading the changed text");
    tsuchi_expect_text(after, "TsuchiCheck", "the changed text");
    tsuchi_expect_result(api->restore(self, table, names[1], names[3]), TSUCHI_OK, "restoring the text field");
    tsuchi_expect_result(api->get_text(self, table, names[1], names[3], after, sizeof after, &needed), TSUCHI_OK, "reading the restored text");
    tsuchi_expect_text(after, before, "the text before the change");
    tsuchi_check_end();

    tsuchi_check_begin("row");
    const bool had_row = api->get(self, table, "TsuchiModCheckRow", names[2], &value) == TSUCHI_OK;
    const TSUCHI_Result added = api->add_row(self, table, "TsuchiModCheckRow", names[1]);
    tsuchi_expect_result(added, had_row ? TSUCHI_INVALID : TSUCHI_OK, "copying a row under a new name");
    tsuchi_expect_result(api->get(self, table, "TsuchiModCheckRow", names[2], &value), TSUCHI_OK, "reading the copy's number");
    tsuchi_expect_near(value.as.number, number, 0.0001, "the copy's number");
    tsuchi_expect_result(api->add_row(self, table, "TsuchiModCheckRow", names[1]), TSUCHI_INVALID, "copying under an existing name");
    tsuchi_expect_result(api->add_row(self, table, "TsuchiOther", "TsuchiNoSuchRow"), TSUCHI_NOT_FOUND, "copying a row that does not exist");
    tsuchi_check_end_with(had_row ? "kept" : "fresh");

    tsuchi_check_begin("refused");
    tsuchi_expect_result(api->get(self, table, "TsuchiNoSuchRow", names[2], &value), TSUCHI_NOT_FOUND, "reading an absent row");
    tsuchi_expect_result(api->get(self, table, names[1], "TsuchiNoSuchField", &value), TSUCHI_NOT_FOUND, "reading an absent field");
    tsuchi_expect_result(api->get_text(self, table, names[1], names[2], after, sizeof after, &needed), TSUCHI_INVALID, "reading a number as text");
    tsuchi_check_end();

    tsuchi_check_begin("released");
    tsuchi_expect_result(host->asset->release(self, replacement), TSUCHI_OK, "releasing the replacement asset");
    tsuchi_expect_result(host->asset->release(self, references), TSUCHI_OK, "releasing the references table");
    tsuchi_expect_result(host->asset->release(self, table), TSUCHI_OK, "releasing the table");
    tsuchi_check_end();
    return true;
}

static void on_step(void *user, const TSUCHI_TimeStep *event) {
    (void)user, (void)event;
    if (++steps % 30 == 0 && run()) host->cancel(self, listening);
}

TSUCHI_Result tsuchi_mod_start(const TSUCHI_Host *given, TSUCHI_Mod *mod) {
    host = given, self = mod, steps = 0;
    table.id = references.id = replacement.id = 0;
    tsuchi_check_start(host, self);
    for (unsigned i = 0; i < sizeof every / sizeof *every; ++i) tsuchi_check_plan(every[i]);
    if (tsuchi_check_lines("targets.txt", targets, sizeof targets, names, 8) != 8) {
        for (unsigned i = 0; i < sizeof every / sizeof *every; ++i) {
            tsuchi_check_begin(every[i]);
            tsuchi_expect(false, "targets.txt must name both tables, rows, fields and a replacement asset in eight lines");
            tsuchi_check_end();
        }
        return TSUCHI_OK;
    }
    if (run()) return TSUCHI_OK;
    return host->time->on_step(self, on_step, 0, &listening);
}
