/* Reads inherited fields and calls the engine through the public, scoped reflection API. */
#include "tsuchi/tsuchi.h"
#include "tsuchi_check.h"

static const TSUCHI_Host *host;
static TSUCHI_Mod *self;
static TSUCHI_Subscription listener;
static TSUCHI_Object actor_class, scene_class, actor, part;
static const char *const names[] = {"classes", "names", "fields", "read", "written", "call", "results", "ended", "refused"};

static bool ready(void) {
    TSUCHI_Actor captain = {0};
    if (host->player->captain(self, 0, &captain) != TSUCHI_OK || !captain.id) return false;
    actor.id = captain.id;
    if (host->reflection->find_class(self, "GActor", &actor_class) != TSUCHI_OK ||
        host->reflection->find_class(self, "SceneComponent", &scene_class) != TSUCHI_OK) return false;
    TSUCHI_Actor actors[4096];
    uint64_t actor_count = 0;
    if (host->entity->list(self, actor_class, actors, 4096, &actor_count) != TSUCHI_OK) return false;
    for (uint64_t a = 0; a < actor_count; ++a) {
        TSUCHI_Component parts[256];
        uint64_t count = 0;
        if (host->component->list(self, actors[a], parts, 256, &count) != TSUCHI_OK) continue;
        for (uint64_t i = 0; i < count; ++i) {
            const TSUCHI_Object object = {parts[i].id};
            bool scene = false;
            TSUCHI_Value visible = {0};
            if (host->reflection->is_a(self, object, scene_class, &scene) == TSUCHI_OK && scene &&
                host->reflection->get(self, object, "bVisible", &visible) == TSUCHI_OK && visible.kind == TSUCHI_VALUE_BOOLEAN) {
                part = object;
                return true;
            }
        }
    }
    return false;
}

static void run(void) {
    const TSUCHI_ReflectionService *api = host->reflection;
    TSUCHI_Object actual = {0};
    bool belongs = false;
    tsuchi_check_begin("classes");
    tsuchi_expect_result(api->class_of(self, actor, &actual), TSUCHI_OK, "the actor's actual class");
    tsuchi_expect(actual.id != 0, "the actor has a class");
    tsuchi_expect_result(api->is_a(self, actor, actual, &belongs), TSUCHI_OK, "membership in its actual class");
    tsuchi_expect(belongs, "the actor belongs to its actual class");
    tsuchi_expect_result(api->is_a(self, actor, actor_class, &belongs), TSUCHI_OK, "membership in the base actor class");
    tsuchi_expect(belongs, "the actor inherits from GActor");
    tsuchi_check_end();

    char text[256] = "";
    uint64_t needed = 0;
    tsuchi_check_begin("names");
    tsuchi_expect_result(api->name(self, actor, text, sizeof text, &needed), TSUCHI_OK, "the actor's name");
    tsuchi_expect(text[0] != 0 && needed == strlen(text) + 1, "the name has its terminator in the reported size");
    tsuchi_expect_result(api->name(self, actor, text, 1, &needed), TSUCHI_CAPACITY, "asking how much room its name needs");
    tsuchi_check_end();

    tsuchi_check_begin("fields");
    bool found = false, ended = false;
    for (uint32_t i = 0; i < 4096; ++i) {
        const TSUCHI_Result result = api->field_at(self, part, "", i, text, sizeof text, &needed);
        if (result == TSUCHI_NOT_FOUND) { ended = true; break; }
        tsuchi_expect_result(result, TSUCHI_OK, "enumerating a reflected field");
        if (result != TSUCHI_OK) break;
        if (strcmp(text, "bVisible") == 0) found = true;
    }
    tsuchi_expect(found && ended, "enumeration found the inherited visibility field and its end");
    tsuchi_check_end();

    TSUCHI_Value before = {0}, again = {0};
    tsuchi_check_begin("read");
    tsuchi_expect_result(api->get(self, part, "bVisible", &before), TSUCHI_OK, "reading the visibility field");
    tsuchi_expect(before.kind == TSUCHI_VALUE_BOOLEAN, "the field is a boolean");
    tsuchi_check_end();
    tsuchi_check_begin("written");
    tsuchi_expect_result(api->set(self, part, "bVisible", &before), TSUCHI_OK, "writing back the same visibility");
    tsuchi_expect_result(api->get(self, part, "bVisible", &again), TSUCHI_OK, "reading back visibility");
    tsuchi_expect(again.kind == before.kind && again.as.boolean == before.as.boolean, "the field kept its value");
    tsuchi_check_end();

    TSUCHI_PreparedCall call = {0};
    tsuchi_check_begin("call");
    const TSUCHI_Result prepared = api->begin_call(self, actor, "K2_GetActorLocation", &call);
    tsuchi_expect_result(prepared, TSUCHI_OK, "preparing the engine's location call");
    tsuchi_expect(call.id != 0, "the prepared call has an ID");
    tsuchi_expect_result(api->call_field_at(self, call, "", 0, text, sizeof text, &needed), TSUCHI_OK, "the first result or argument");
    tsuchi_expect(strcmp(text, "ReturnValue") == 0, "the location is its return value");
    tsuchi_expect_result(api->call_output_at(self, call, 0, text, sizeof text, &needed), TSUCHI_OK, "the first output");
    tsuchi_expect(strcmp(text, "ReturnValue") == 0, "the return value is an output");
    tsuchi_expect_result(api->call_output_at(self, call, 1, text, sizeof text, &needed), TSUCHI_NOT_FOUND, "there is just one output");
    tsuchi_expect_result(api->invoke(self, call), TSUCHI_OK, "invoking the engine call");
    tsuchi_check_end();

    TSUCHI_Pose transform;
    memset(&transform, 0, sizeof transform);
    tsuchi_check_begin("results");
    tsuchi_expect_result(host->entity->pose(self, (TSUCHI_Actor){actor.id}, &transform), TSUCHI_OK, "the public actor's transform");
    const char *const paths[] = {"ReturnValue.X", "ReturnValue.Y", "ReturnValue.Z"};
    const double coordinates[] = {transform.location.x, transform.location.y, transform.location.z};
    for (unsigned i = 0; i < 3; ++i) {
        TSUCHI_Value value = {0};
        tsuchi_expect_result(api->call_get(self, call, paths[i], &value), TSUCHI_OK, "reading a result by field path");
        tsuchi_expect(value.kind == TSUCHI_VALUE_NUMBER, "the coordinate is a number");
        tsuchi_expect_near(value.as.number, coordinates[i], 0.01, "the engine and actors service agree");
    }
    tsuchi_expect_result(api->invoke(self, call), TSUCHI_OK, "invoking the same prepared call again");
    tsuchi_check_end();
    tsuchi_check_begin("ended");
    tsuchi_expect_result(api->end_call(self, call), TSUCHI_OK, "ending the prepared call");
    tsuchi_expect_result(api->end_call(self, call), TSUCHI_NOT_FOUND, "ending it twice");
    tsuchi_expect_result(api->call_get(self, call, "ReturnValue.X", &again), TSUCHI_NOT_FOUND, "a result of an ended call");
    tsuchi_check_end();

    TSUCHI_PreparedCall missing = {0};
    tsuchi_check_begin("refused");
    tsuchi_expect_result(api->find_class(self, "TsuchiMissingClass", &actual), TSUCHI_NOT_FOUND, "an absent class");
    tsuchi_expect_result(api->get(self, part, "TsuchiMissingField", &again), TSUCHI_NOT_FOUND, "an absent field");
    tsuchi_expect_result(api->get(self, part, "bVisible[", &again), TSUCHI_INVALID, "a malformed path");
    tsuchi_expect_result(api->begin_call(self, actor, "TsuchiMissingCall", &missing), TSUCHI_NOT_FOUND, "an absent function");
    tsuchi_check_end();
}

static void on_step(void *user, const TSUCHI_TimeStep *event) {
    (void)user, (void)event;
    if (!ready()) return;
    host->cancel(self, listener);
    run();
}

TSUCHI_Result tsuchi_mod_start(const TSUCHI_Host *given, TSUCHI_Mod *mod) {
    host = given, self = mod;
    tsuchi_check_start(host, self);
    for (unsigned i = 0; i < sizeof names / sizeof *names; ++i) tsuchi_check_plan(names[i]);
    if (ready()) run();
    else {
        const TSUCHI_Result watched = host->time->on_step(self, on_step, 0, &listener);
        for (unsigned i = 0; i < sizeof names / sizeof *names; ++i) {
            if (watched == TSUCHI_OK) tsuchi_check_waiting(names[i], "a captain and a scene part in the world");
            else { tsuchi_check_begin(names[i]); tsuchi_expect_result(watched, TSUCHI_OK, "watching for the world"); tsuchi_check_end(); }
        }
    }
    return TSUCHI_OK;
}
