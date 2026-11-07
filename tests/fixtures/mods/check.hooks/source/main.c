/* Wraps the game's update through its table entry and its patched function, continuing with its own ABI. */
#include "tsuchi/tsuchi.h"
#include "tsuchi_check.h"

typedef void (*EngineTick)(void *, float, bool);
static const TSUCHI_Host *host;
static TSUCHI_Mod *self;
static TSUCHI_Hook slot, site, address;
static TSUCHI_Subscription listener;
static uint32_t slot_calls, site_calls, address_calls, steps, slot_ended, site_ended, address_ended;

static void count_slot(void *engine, float seconds, bool idle) {
    ++slot_calls;
    ((EngineTick)slot.original)(engine, seconds, idle);
}
static void count_site(void *engine, float seconds, bool idle) {
    ++site_calls;
    ((EngineTick)site.original)(engine, seconds, idle);
}
static void count_address(void *engine, float seconds, bool idle) {
    ++address_calls;
    ((EngineTick)address.original)(engine, seconds, idle);
}
static bool hex_number(const char **text, uint64_t *value) {
    const char *at = *text;
    while (*at == ' ' || *at == '\r' || *at == '\n') ++at;
    if (at[0] == '0' && at[1] == 'x') at += 2;
    unsigned digits = 0;
    *value = 0;
    while (*at) {
        unsigned digit;
        if (*at >= '0' && *at <= '9') digit = (unsigned)(*at - '0');
        else if (*at >= 'a' && *at <= 'f') digit = (unsigned)(*at - 'a' + 10);
        else if (*at >= 'A' && *at <= 'F') digit = (unsigned)(*at - 'A' + 10);
        else break;
        if (++digits > 16) return false;
        *value = (*value << 4) | digit;
        ++at;
    }
    *text = at;
    return digits != 0;
}
static void on_step(void *user, const TSUCHI_TimeStep *event) {
    (void)user, (void)event;
    if (++steps == 3) {
        tsuchi_check_begin("heard");
        tsuchi_expect(slot_calls > 0, "the table entry reached its wrapper");
        tsuchi_expect(address_calls > 0, "the address entry reached its wrapper");
        tsuchi_expect(site_calls > 0, "the patched function reached its wrapper");
        tsuchi_check_end();
        tsuchi_check_begin("removed");
        tsuchi_expect_result(host->hook->remove(self, slot.id), TSUCHI_OK, "ending the table wrapper");
        tsuchi_expect_result(host->hook->remove(self, slot.id), TSUCHI_NOT_FOUND, "ending it twice");
        tsuchi_expect_result(host->hook->remove(self, site.id), TSUCHI_OK, "ending the function wrapper");
        tsuchi_expect_result(host->hook->remove(self, site.id), TSUCHI_NOT_FOUND, "ending it twice");
        tsuchi_check_end();
        tsuchi_check_begin("address_removed");
        tsuchi_expect_result(host->hook->remove(self, address.id), TSUCHI_OK, "ending the address wrapper");
        tsuchi_expect_result(host->hook->remove(self, address.id), TSUCHI_NOT_FOUND, "ending it twice");
        tsuchi_check_end();
        slot_ended = slot_calls, site_ended = site_calls, address_ended = address_calls;
    } else if (steps == 6) {
        tsuchi_check_begin("quiet");
        tsuchi_expect_number(slot_calls, slot_ended, "table calls after removal");
        tsuchi_expect_number(address_calls, address_ended, "address calls after removal");
        tsuchi_expect_number(site_calls, site_ended, "function calls after removal");
        tsuchi_expect_result(host->cancel(self, listener), TSUCHI_OK, "ending the step listener");
        tsuchi_check_end();
    }
}

TSUCHI_Result tsuchi_mod_start(const TSUCHI_Host *given, TSUCHI_Mod *mod) {
    host = given, self = mod;
    memset(&slot, 0, sizeof slot), memset(&site, 0, sizeof site), memset(&address, 0, sizeof address);
    slot_calls = site_calls = address_calls = steps = slot_ended = site_ended = address_ended = 0;
    tsuchi_check_start(host, self);
    const char *const names[] = {"slot", "site", "heard", "removed", "quiet", "refused", "address", "address_removed"};
    for (unsigned i = 0; i < sizeof names / sizeof *names; ++i) tsuchi_check_plan(names[i]);
    const TSUCHI_Result slotted = host->hook->wrap_slot(self, "engine_tick", (void *)count_slot, &slot);
    tsuchi_check_begin("slot");
    tsuchi_expect_result(slotted, TSUCHI_OK, "wrapping the engine's table entry");
    tsuchi_expect(slot.id.id != 0 && slot.original != 0, "the wrapper has an ID and an original to continue");
    tsuchi_check_end();
    const TSUCHI_Result sited = host->hook->wrap_site(self, "engine_tick_function", (void *)count_site, &site);
    tsuchi_check_begin("site");
    tsuchi_expect_result(sited, TSUCHI_OK, "wrapping the engine's patched function");
    tsuchi_expect(site.id.id != 0 && site.original != 0, "the wrapper has an ID and an original to continue");
    tsuchi_check_end();
    char targets[96] = {0};
    uint64_t got = 0, entry = 0, stock = 0;
    const TSUCHI_Result read = host->file->read(self, "targets.txt", 0, targets, sizeof targets - 1, &got);
    const char *at = targets;
    const bool target_ok = read == TSUCHI_OK && hex_number(&at, &entry) && hex_number(&at, &stock) && entry != 0;
    const TSUCHI_Result addressed = target_ok ? host->hook->wrap_address(self, entry, stock, (void *)count_address, &address) : TSUCHI_INVALID;
    tsuchi_check_begin("address");
    tsuchi_expect_result(read, TSUCHI_OK, "reading the entry and stock offsets from the game table");
    tsuchi_expect(target_ok, "two hexadecimal offsets identify the existing engine entry");
    tsuchi_expect_result(addressed, TSUCHI_OK, "wrapping the same entry by address");
    tsuchi_expect(address.id.id != 0 && address.original != 0, "the address wrapper can continue its chain");
    tsuchi_check_end();
    tsuchi_check_begin("refused");
    TSUCHI_Hook missing = {0};
    tsuchi_expect_result(host->hook->wrap_slot(self, "tsuchi_missing_slot", (void *)count_slot, &missing), TSUCHI_NOT_FOUND, "a table entry nobody named");
    tsuchi_expect_result(host->hook->wrap_site(self, "tsuchi_missing_site", (void *)count_site, &missing), TSUCHI_NOT_FOUND, "a function nobody named");
    tsuchi_check_end();
    TSUCHI_Result listened = TSUCHI_NOT_READY;
    if (slotted == TSUCHI_OK && sited == TSUCHI_OK && addressed == TSUCHI_OK) listened = host->time->on_step(self, on_step, 0, &listener);
    if (listened == TSUCHI_OK) {
        tsuchi_check_waiting("heard", "three world steps");
        tsuchi_check_waiting("removed", "the wrappers to have run");
        tsuchi_check_waiting("quiet", "three steps after removal");
        tsuchi_check_waiting("address_removed", "the address wrapper to have run");
    } else {
        for (unsigned i = 2; i < 5; ++i) {
            tsuchi_check_begin(names[i]);
            tsuchi_expect_result(listened, TSUCHI_OK, "both wrappers and their step listener");
            tsuchi_check_end();
        }
        tsuchi_check_begin("address_removed");
        tsuchi_expect_result(listened, TSUCHI_OK, "the address wrapper and step listener");
        tsuchi_check_end();
        if (addressed == TSUCHI_OK) host->hook->remove(self, address.id);
        if (slotted == TSUCHI_OK) host->hook->remove(self, slot.id);
        if (sited == TSUCHI_OK) host->hook->remove(self, site.id);
    }
    return TSUCHI_OK;
}
