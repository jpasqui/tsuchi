/* A public SDK hook runs inside Tsuchi's AI-values wrapper, directly before native BabyAI start.
 * Addresses and layout come from targets.txt, generated from the active game table. */
#include <stdatomic.h>
#include "tsuchi/tsuchi.h"
#include "tsuchi_check.h"

typedef void (*AiStart)(void *);
static const TSUCHI_Host *host;
static TSUCHI_Mod *self;
static TSUCHI_Hook observer;
static TSUCHI_Subscription listener;
static uint64_t targets[12];
static atomic_uint calls;
static atomic_bool complete;
static uint32_t steps;
static bool reported, census_done;
static char target_class[128];
static atomic_uint native_starts;
static uint32_t starts_logged;
static struct {
    uint64_t part, owner, native_owner, placement, current_ai;
    atomic_bool complete;
} starts[32];
static struct {
    uint64_t part, owner, native_owner, placement, current_ai, actions_before, actions_after, patrol_reference;
    float before[5], after[5];
    uint8_t patrol_before, patrol_after;
} sample;
static const float wanted[5] = {11.25f, -22.5f, 33.75f, 87.5f, 731.25f};

static uint64_t pointer_at(uint64_t address) {
    return *(const volatile uint64_t *)(uintptr_t)address;
}
static void values_at(uint64_t part, float out[5]) {
    for (unsigned i = 0; i < 5; ++i)
        out[i] = *(const volatile float *)(uintptr_t)(part + targets[6] + sizeof(float) * i);
}
static bool values_match(const float values[5]) {
    for (unsigned i = 0; i < 5; ++i) if (values[i] != wanted[i]) return false;
    return true;
}
static void before_native(void *part_pointer) {
    const uint64_t part = (uint64_t)(uintptr_t)part_pointer;
    const uint64_t owner = pointer_at(part + targets[2]);
    const uint64_t native_owner = pointer_at(part + targets[3]);
    const uint64_t placement = owner ? pointer_at(owner + targets[5]) : 0;
    const uint32_t index = atomic_fetch_add_explicit(&native_starts, 1, memory_order_relaxed);
    if (index < 32) {
        starts[index].part = part, starts[index].owner = owner, starts[index].native_owner = native_owner;
        starts[index].placement = placement;
        starts[index].current_ai = owner ? pointer_at(owner + targets[4]) : 0;
    }
    const bool record = placement == targets[9] && atomic_fetch_add_explicit(&calls, 1, memory_order_relaxed) == 0;
    if (record) {
        sample.part = part, sample.owner = owner, sample.native_owner = native_owner;
        sample.placement = placement, sample.current_ai = pointer_at(owner + targets[4]);
        sample.actions_before = pointer_at(part + targets[8]);
        values_at(part, sample.before);
        sample.patrol_before = *(const volatile uint8_t *)(uintptr_t)(part + targets[7]);
    }
    /* Installed before the declaring mod, this continuation reaches the native start.
     * No framework call is made on this hook's thread. The next world step reports the copy. */
    ((AiStart)observer.original)(part_pointer);
    if (index < 32) atomic_store_explicit(&starts[index].complete, true, memory_order_release);
    if (record) {
        sample.actions_after = pointer_at(part + targets[8]);
        const uint64_t context = sample.actions_after ? pointer_at(sample.actions_after + targets[10]) : 0;
        sample.patrol_reference = context ? pointer_at(context + targets[11]) : 0;
        values_at(part, sample.after);
        sample.patrol_after = *(const volatile uint8_t *)(uintptr_t)(part + targets[7]);
        atomic_store_explicit(&complete, true, memory_order_release);
    }
}
static bool next_hex(const char **text, uint64_t *value) {
    const char *at = *text;
    while (*at == ' ' || *at == '\r' || *at == '\n') ++at;
    if (at[0] == '0' && at[1] == 'x') at += 2;
    unsigned digits = 0;
    *value = 0;
    for (;;) {
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
/* Diagnostics are bounded, and SDK calls only occur on the world-step thread. */
static void diagnose(void) {
    const uint32_t count = atomic_load_explicit(&native_starts, memory_order_relaxed);
    while (starts_logged < count && starts_logged < 32) {
        const uint32_t index = starts_logged;
        if (!atomic_load_explicit(&starts[index].complete, memory_order_acquire)) break;
        char line[TSUCHI_CHECK_LINE] = "AI native start placement=";
        tsuchi_check_add_number(line, (int64_t)starts[index].placement);
        tsuchi_check_add(line, " part="), tsuchi_check_add_number(line, (int64_t)starts[index].part);
        tsuchi_check_add(line, " owner="), tsuchi_check_add_number(line, (int64_t)starts[index].owner);
        tsuchi_check_add(line, " native_owner="), tsuchi_check_add_number(line, (int64_t)starts[index].native_owner);
        tsuchi_check_add(line, " current_ai="), tsuchi_check_add_number(line, (int64_t)starts[index].current_ai);
        host->mod->log(self, line);
        ++starts_logged;
    }
    if (census_done) return;
    bool control = false;
    if (host->game->has_control(self, &control) != TSUCHI_OK || !control) return;
    census_done = true;
    TSUCHI_Object of_class = {0};
    TSUCHI_Actor actors[128];
    uint64_t count_actors = 0;
    const TSUCHI_Result found = host->reflection->find_class(self, target_class, &of_class);
    const TSUCHI_Result listed = found == TSUCHI_OK ? host->entity->list(self, of_class, actors, 128, &count_actors) : found;
    char line[TSUCHI_CHECK_LINE] = "AI census class=";
    tsuchi_check_add(line, target_class);
    tsuchi_check_add(line, " result="), tsuchi_check_add_number(line, listed);
    tsuchi_check_add(line, " actors="), tsuchi_check_add_number(line, count_actors);
    tsuchi_check_add(line, " native_starts="), tsuchi_check_add_number(line, count);
    host->mod->log(self, line);
    if (listed != TSUCHI_OK) return;
    for (uint64_t i = 0; i < count_actors; ++i) {
        uint64_t placement = 0;
        const TSUCHI_Result result = host->entity->placement(self, actors[i], &placement);
        char entry[TSUCHI_CHECK_LINE] = "AI census placement=";
        tsuchi_check_add_number(entry, (int64_t)placement);
        tsuchi_check_add(entry, " actor_handle="), tsuchi_check_add_number(entry, (int64_t)actors[i].id);
        tsuchi_check_add(entry, " result="), tsuchi_check_add_number(entry, result);
        host->mod->log(self, entry);
    }
}
static void on_step(void *user, const TSUCHI_TimeStep *event) {
    (void)user, (void)event;
    diagnose();
    if (reported) return;
    ++steps;
    if (!atomic_load_explicit(&complete, memory_order_acquire)) {
        if (steps < 18000) return;
        reported = true;
        tsuchi_check_begin("observed");
        tsuchi_expect(false, "no completed native start for the targeted placement before the deadline");
        tsuchi_check_end();
        host->cancel(self, listener);
        return;
    }
    const uint32_t observed = atomic_load_explicit(&calls, memory_order_relaxed);
    reported = true;
    char line[TSUCHI_CHECK_LINE] = "AI start placement=";
    tsuchi_check_add_number(line, (int64_t)sample.placement);
    tsuchi_check_add(line, " calls="), tsuchi_check_add_number(line, observed);
    tsuchi_check_add(line, " before=");
    for (unsigned i = 0; i < 5; ++i) {
        if (i) tsuchi_check_add(line, ",");
        tsuchi_check_add_real(line, sample.before[i]);
    }
    tsuchi_check_add(line, " patrol="), tsuchi_check_add_number(line, sample.patrol_before);
    tsuchi_check_add(line, " native_actions_changed=");
    tsuchi_check_add_number(line, sample.actions_after != 0 && sample.actions_after != sample.actions_before);
    host->mod->log(self, line);
    tsuchi_check_begin("observed");
    tsuchi_expect(observed > 0, "the targeted placed larva entered native AI start before the deadline");
    tsuchi_check_end();
    tsuchi_check_begin("identity");
    tsuchi_expect(sample.part && sample.owner && sample.native_owner == sample.owner,
                  "the inherited and native owner pointers identify the same actor");
    tsuchi_expect(sample.current_ai == sample.part && sample.placement == targets[9],
                  "this is the targeted actor's current AI component");
    tsuchi_check_end();
    tsuchi_check_begin("values_before_start");
    tsuchi_expect(values_match(sample.before) && sample.patrol_before == 0,
                  "all five changed territory numbers and the patrol value precede original native start");
    tsuchi_check_end();
    tsuchi_check_begin("native_started");
    tsuchi_expect(sample.actions_after != 0 && sample.actions_after != sample.actions_before &&
                  sample.patrol_reference == sample.part + targets[7],
                  "original native start built a new tree that refers to the observed BabyAI parameters");
    tsuchi_check_end();
    tsuchi_check_begin("kept_values");
    tsuchi_expect(values_match(sample.after) && sample.patrol_after == 0,
                  "native start returned with the supplied territory and patrol values");
    tsuchi_check_end();
    host->cancel(self, listener);
}

TSUCHI_Result tsuchi_mod_start(const TSUCHI_Host *given, TSUCHI_Mod *mod) {
    host = given, self = mod;
    memset(&observer, 0, sizeof observer), memset(&sample, 0, sizeof sample);
    atomic_store(&calls, 0), atomic_store(&complete, false);
    steps = 0, reported = false, census_done = false, starts_logged = 0;
    atomic_store(&native_starts, 0);
    for (unsigned i = 0; i < 32; ++i) atomic_store(&starts[i].complete, false);
    memset(target_class, 0, sizeof target_class);
    tsuchi_check_start(host, self);
    const char *const names[] = {"installed", "identity", "values_before_start", "native_started", "kept_values", "observed"};
    for (unsigned i = 0; i < sizeof names / sizeof *names; ++i) tsuchi_check_plan(names[i]);
    char text[256] = {0};
    uint64_t got = 0;
    const TSUCHI_Result read = host->file->read(self, "targets.txt", 0, text, sizeof text - 1, &got);
    const TSUCHI_Result class_read = host->file->read(self, "class.txt", 0, target_class, sizeof target_class - 1, &got);
    const char *at = text;
    bool good = read == TSUCHI_OK && class_read == TSUCHI_OK && target_class[0];
    for (unsigned i = 0; i < 12 && good; ++i) good = next_hex(&at, &targets[i]);
    TSUCHI_Result wrapped = good ? host->hook->wrap_address(self, targets[0], targets[1],
                                                         (void *)before_native, &observer) : TSUCHI_INVALID;
    TSUCHI_Result listened = wrapped == TSUCHI_OK ? host->time->on_step(self, on_step, 0, &listener) : wrapped;
    tsuchi_check_begin("installed");
    tsuchi_expect_result(read, TSUCHI_OK, "reading table-derived targets");
    tsuchi_expect(good && targets[0] && targets[9], "twelve offsets name the entry, layout, and placement");
    tsuchi_expect_result(wrapped, TSUCHI_OK, "wrapping BabyAI before the dependent mod declares its values");
    tsuchi_expect(observer.id.id && observer.original, "the hook has an original to continue");
    tsuchi_expect_result(listened, TSUCHI_OK, "listening for the copied native observation");
    tsuchi_check_end();
    for (unsigned i = 1; i < sizeof names / sizeof *names; ++i)
        tsuchi_check_waiting(names[i], "the targeted larva's native AI start");
    return listened;
}
