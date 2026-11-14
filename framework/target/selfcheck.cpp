// The framework's own checks. They run in the game, in a build made for checking it: each
// takes a part of the framework through what it is for and writes what happened into the
// status block. They spawn and kill things, travel, and write to the save, so they are
// compiled only with TSUCHI_SELFCHECK, never into the build people run.
#if TSUCHI_SELFCHECK
#include <stddef.h>
#include <string.h>
#include "actor_events.h"
#include "actors.h"
#include "added.h"
#include "compiled_mods.h"
#include "files.h"
#include "angles.h"
#include "flow.h"
#include "frame.h"
#include "game.h"
#include "hooks.h"
#include "host.h"
#include "ids.h"
#include "names.h"
#include "objects.h"
#include "pikmin_service.h"
#include "people.h"
#include "battle_service.h"
#include "gimmicks_service.h"
#include "platform.h"
#include "reflect.h"
#include "reflection.h"
#include "registry.h"
#include "script_mods.h"
#include "services.h"
#include "hidden.h"
#include "life_events.h"
#include "materials.h"
#include "mods.h"
#include "suspend.h"
#include "status.h"
#include "world_follow_check.h"

namespace {

using namespace tsuchi;
using namespace tsuchi::target;
namespace t = game::table;

// The port services are reached through the same generated ABI in the check build.
static_assert(sizeof(TSUCHI_BattleService) >= sizeof(void*) * 20, "battle service family");
static_assert(sizeof(TSUCHI_GimmicksService) >= sizeof(void*) * 10, "gimmick service family");

void heard_frame(void*, const TSUCHI_TimeStep*) { ++status.frames_heard; }
void heard_spawned(void*, const TSUCHI_EntitySpawned*) { ++status.events_heard_spawned; }
void heard_destroyed(void*, const TSUCHI_EntityDestroyed* event) {
    ++status.events_heard_destroyed;
    copy_text(status.events_last_left, sizeof status.events_last_left, event->kind);
}

struct Tally {
    game::ClassRef classes{"Class"}, actors{"Actor"};
    uint64_t first_actor = 0;
};

void check_base() {
    TSUCHI_NetworkStatus network{};
    TSUCHI_NetAddress loopback{};
    if (host.net->status(self, &network) != TSUCHI_OK ||
        host.net->resolve(self, "127.0.0.1", 47700, &loopback) != TSUCHI_OK ||
        loopback.ipv4 != 0x7f000001 || loopback.port != 47700)
        host.mod->log(self, "net numeric-address selfcheck failed");
    Tally tally;
    status.object_count = game::global<int32_t>(t::data::object_count);
    status.live_objects = status.classes = status.actors = status.handles_tried = status.handles_ok = 0;
    game::each_object(
        [](void* context, uint64_t object) {
            auto& tally = *static_cast<Tally*>(context);
            ++status.live_objects;
            if (game::is_a(object, tally.classes)) ++status.classes;
            if (game::is_a(object, tally.actors)) {
                if (!tally.first_actor) tally.first_actor = object;
                // A handle for a sample only: making one numbers the object.
                if (++status.actors <= 32) {
                    ++status.handles_tried;
                    if (game::from_handle(game::to_handle(object)) == object) ++status.handles_ok;
                }
            }
            return true;
        },
        &tally);
    const uint64_t root = game::find_class(tally.classes);
    status.root_handle = game::to_handle(root);
    if (!game::name_text(game::name_of(root), status.root_name, sizeof status.root_name)) status.root_name[0] = 0;
    if (!tally.first_actor || !game::name_text(game::name_of(tally.first_actor), status.actor_name, sizeof status.actor_name))
        status.actor_name[0] = 0;
    ++status.checks;
}

// The second hook on the per-frame entry. It has the engine function's own
// shape, counts, and continues.
using EngineTick = void (*)(void* engine, float seconds, bool idle);
TSUCHI_Mod* hook_tester = nullptr;
TSUCHI_Hook test_hook{};
void counting_tick(void* engine, float seconds, bool idle) {
    ++status.hook_test_calls;
    reinterpret_cast<EngineTick>(test_hook.original)(engine, seconds, idle);
}

// The same on the per-frame function itself, reached through the patch.
TSUCHI_Mod* site_tester = nullptr;
TSUCHI_Hook site_hook{};
void counting_function(void* engine, float seconds, bool idle) {
    ++status.site_test_calls;
    reinterpret_cast<EngineTick>(site_hook.original)(engine, seconds, idle);
}

void check_hooks() {
    if (status.frames == kHookTestStart) {
        hook_tester = mod_create("tsuchi.hook-check");
        status.hook_test_result = host.hook->wrap_slot(hook_tester, "engine_tick", reinterpret_cast<void*>(&counting_tick), &test_hook);
        status.hook_test = status.hook_test_result == TSUCHI_OK ? 1 : 3;
    } else if (status.frames == kHookTestStart + kHookTestFrames && status.hook_test == 1) {
        mod_stop(hook_tester);
        status.hook_test = 2;
    }
    status.tick_hooks = hook_count(tick_entry);

    if (status.frames == kSiteTestStart) {
        site_tester = mod_create("tsuchi.site-check");
        status.site_test_result =
            host.hook->wrap_site(site_tester, "engine_tick_function", reinterpret_cast<void*>(&counting_function), &site_hook);
        status.site_test = status.site_test_result == TSUCHI_OK ? 1 : 3;
    } else if (status.frames == kSiteTestStart + kHookTestFrames && status.site_test == 1) {
        mod_stop(site_tester);
        status.site_test = 2;
    }
}

// ---- The check of reflection ------------------------------------------------

bool near(float a, float b, float tolerance) {
    const float difference = a - b;
    return difference < tolerance && difference > -tolerance;
}

// Remembers the first result that was not TSUCHI_OK, for calls that should work.
TSUCHI_Result expect(TSUCHI_Result result) {
    if (result != TSUCHI_OK && !status.reflect_result) status.reflect_result = result;
    return result;
}

TSUCHI_Value number_value(double number) {
    TSUCHI_Value value{};
    value.kind = TSUCHI_VALUE_NUMBER;
    value.as.number = number;
    return value;
}
TSUCHI_Value integer_value(int64_t integer) {
    TSUCHI_Value value{};
    value.kind = TSUCHI_VALUE_INTEGER;
    value.as.integer = integer;
    return value;
}
TSUCHI_Value boolean_value(bool boolean) {
    TSUCHI_Value value{};
    value.kind = TSUCHI_VALUE_BOOLEAN;
    value.as.boolean = boolean;
    return value;
}

bool field(TSUCHI_Object object, const char* path, uint32_t kind, TSUCHI_Value& out) {
    return expect(host.reflection->get(self, object, path, &out)) == TSUCHI_OK && out.kind == kind;
}
bool field_number(TSUCHI_Object object, const char* path, float& out) {
    TSUCHI_Value value{};
    if (!field(object, path, TSUCHI_VALUE_NUMBER, value)) return false;
    out = float(value.as.number);
    return true;
}
bool result(TSUCHI_PreparedCall call, const char* path, uint32_t kind, TSUCHI_Value& out) {
    return expect(host.reflection->call_get(self, call, path, &out)) == TSUCHI_OK && out.kind == kind;
}
bool result_number(TSUCHI_PreparedCall call, const char* path, float& out) {
    TSUCHI_Value value{};
    if (!result(call, path, TSUCHI_VALUE_NUMBER, value)) return false;
    out = float(value.as.number);
    return true;
}
bool argument(TSUCHI_PreparedCall call, const char* path, TSUCHI_Value value) { return expect(host.reflection->call_set(self, call, path, &value)) == TSUCHI_OK; }

// Begins a call and runs it at once: for functions without arguments. The
// call stays open for its results.
bool run(TSUCHI_Object object, const char* function, TSUCHI_PreparedCall& call) {
    call = {};
    return expect(host.reflection->begin_call(self, object, function, &call)) == TSUCHI_OK && expect(host.reflection->invoke(self, call)) == TSUCHI_OK;
}
void end(TSUCHI_PreparedCall call) {
    if (call.id) host.reflection->end_call(self, call);
}

bool location_of(TSUCHI_Object actor, float out[3]) {
    TSUCHI_PreparedCall call{};
    const bool ok = run(actor, "K2_GetActorLocation", call) && result_number(call, "ReturnValue.X", out[0]) &&
                    result_number(call, "ReturnValue.Y", out[1]) && result_number(call, "ReturnValue.Z", out[2]);
    end(call);
    return ok;
}

// Two actors that are in a level and whose place is their own, not relative
// to something they are attached to.
struct Pick {
    game::ClassRef actors{"Actor"}, levels{"Level"};
    TSUCHI_Object found[2]{};
    uint32_t count = 0;
};
bool pick_actor(void* context, uint64_t object) {
    auto& pick = *static_cast<Pick*>(context);
    if (!game::is_a(object, pick.actors) || !game::is_a(game::outer_of(object), pick.levels)) return true;
    const TSUCHI_Object actor{game::to_handle(object)};
    TSUCHI_Value value{};
    if (!actor.id || host.reflection->get(self, actor, "RootComponent", &value) != TSUCHI_OK || value.kind != TSUCHI_VALUE_OBJECT) return true;
    if (host.reflection->get(self, actor, "RootComponent.AttachParent", &value) != TSUCHI_OK || value.kind != TSUCHI_VALUE_NONE) return true;
    pick.found[pick.count++] = actor;
    return pick.count < 2;
}

void check_reflect() {
    const TSUCHI_ReflectionService& api = *host.reflection;
    status.reflect_test = 1;
    const uint32_t owned = owned_count(self);
    Pick pick;
    game::each_object(pick_actor, &pick);
    if (pick.count < 2) {
        status.reflect_test = 3;
        return;
    }
    const TSUCHI_Object a = pick.found[0], b = pick.found[1];
    TSUCHI_Value value{}, other{};
    TSUCHI_PreparedCall call{};
    uint64_t needed = 0;
    char name[128];
    const auto begin = [](uint32_t step) { status.reflect_step = step; };
    const auto pass = [](uint32_t step, bool ok) { status.reflect_passed |= uint32_t(ok) << step; };

    begin(0); // classes and names
    TSUCHI_Object actor_class{}, type{};
    bool yes = false;
    bool ok = expect(api.find_class(self, "Actor", &actor_class)) == TSUCHI_OK && expect(api.class_of(self, a, &type)) == TSUCHI_OK &&
              type.id && expect(api.is_a(self, a, actor_class, &yes)) == TSUCHI_OK && yes &&
              expect(api.name(self, a, name, sizeof name, &needed)) == TSUCHI_OK && needed > 1;
    copy_text(status.reflect_actor, sizeof status.reflect_actor, ok ? name : "");
    if (api.name(self, b, name, sizeof name, &needed) == TSUCHI_OK) copy_text(status.reflect_other, sizeof status.reflect_other, name);
    pass(0, ok);

    begin(1); // the kinds of value a field can be
    float interval = 0;
    ok = field_number(a, "PrimaryActorTick.TickInterval", interval) && field(a, "RootComponent.RelativeLocation", TSUCHI_VALUE_STRUCT, value) &&
         field(a, "Tags", TSUCHI_VALUE_ARRAY, value);
    status.reflect_tick_interval = interval;
    pass(1, ok);

    begin(2); // through a reference to another object, into a struct
    TSUCHI_Value root{};
    ok = field(a, "RootComponent", TSUCHI_VALUE_OBJECT, root) && field_number(a, "RootComponent.RelativeLocation.X", status.reflect_field_location[0]) &&
         field_number(a, "RootComponent.RelativeLocation.Y", status.reflect_field_location[1]) &&
         field_number(a, "RootComponent.RelativeLocation.Z", status.reflect_field_location[2]);
    pass(2, ok);

    begin(3); // a function that returns a struct
    ok = location_of(a, status.reflect_call_location) && location_of(b, status.reflect_other_call_location);
    pass(3, ok);
    begin(4); // and it agrees with the fields, for both actors
    ok = ok && field_number(b, "RootComponent.RelativeLocation.X", status.reflect_other_field_location[0]) &&
         field_number(b, "RootComponent.RelativeLocation.Y", status.reflect_other_field_location[1]) &&
         field_number(b, "RootComponent.RelativeLocation.Z", status.reflect_other_field_location[2]);
    for (int axis = 0; axis < 3; ++axis)
        ok = ok && near(status.reflect_call_location[axis], status.reflect_field_location[axis], 0.01f) &&
             near(status.reflect_other_call_location[axis], status.reflect_other_field_location[axis], 0.01f);
    pass(4, ok);

    begin(5); // a function that returns an object
    ok = run(a, "K2_GetRootComponent", call) && result(call, "ReturnValue", TSUCHI_VALUE_OBJECT, value) && value.as.object.id == root.as.object.id;
    end(call);
    pass(5, ok);

    begin(6); // a function that takes an object
    const float* there = status.reflect_other_call_location;
    other = TSUCHI_Value{};
    other.kind = TSUCHI_VALUE_OBJECT;
    other.as.object = b;
    call = {};
    ok = expect(api.begin_call(self, a, "GetDistanceTo", &call)) == TSUCHI_OK && argument(call, "OtherActor", other) &&
         expect(api.invoke(self, call)) == TSUCHI_OK && result_number(call, "ReturnValue", status.reflect_distance);
    end(call);
    for (int axis = 0; axis < 3; ++axis) {
        const float difference = there[axis] - status.reflect_call_location[axis];
        status.reflect_distance_squared += difference * difference;
    }
    pass(6, ok && near(status.reflect_distance * status.reflect_distance, status.reflect_distance_squared,
                       1.0f + status.reflect_distance_squared * 0.001f));

    begin(7); // what a function changes shows in the field
    float now = -1;
    TSUCHI_PreparedCall setter{};
    ok = expect(api.begin_call(self, a, "SetActorTickInterval", &setter)) == TSUCHI_OK && argument(setter, "TickInterval", number_value(interval + 0.25)) &&
         expect(api.invoke(self, setter)) == TSUCHI_OK && field_number(a, "PrimaryActorTick.TickInterval", now) && near(now, interval + 0.25f, 0.0001f);
    pass(7, ok);

    begin(8); // what is written to the field shows in a function
    value = number_value(interval + 0.5);
    ok = expect(api.set(self, a, "PrimaryActorTick.TickInterval", &value)) == TSUCHI_OK && run(a, "GetActorTickInterval", call) &&
         result_number(call, "ReturnValue", now) && near(now, interval + 0.5f, 0.0001f);
    end(call);
    // Put back through the function, which also resets the game's own timing for it.
    const bool restored = setter.id && argument(setter, "TickInterval", number_value(interval)) && expect(api.invoke(self, setter)) == TSUCHI_OK &&
                          field_number(a, "PrimaryActorTick.TickInterval", now) && near(now, interval, 0.0001f);
    end(setter);
    pass(8, ok && restored);

    begin(9); // yes-or-no values kept as bits
    TSUCHI_Value before{}, flipped{};
    ok = field(a, "bCanBeDamaged", TSUCHI_VALUE_BOOLEAN, before);
    if (ok) {
        value = boolean_value(!before.as.boolean);
        ok = expect(api.set(self, a, "bCanBeDamaged", &value)) == TSUCHI_OK && field(a, "bCanBeDamaged", TSUCHI_VALUE_BOOLEAN, flipped) &&
             flipped.as.boolean != before.as.boolean;
        ok = expect(api.set(self, a, "bCanBeDamaged", &before)) == TSUCHI_OK && field(a, "bCanBeDamaged", TSUCHI_VALUE_BOOLEAN, flipped) &&
             flipped.as.boolean == before.as.boolean && ok;
    }
    ok = ok && field(a, "bActorEnableCollision", TSUCHI_VALUE_BOOLEAN, before) && run(a, "GetActorEnableCollision", call) &&
         result(call, "ReturnValue", TSUCHI_VALUE_BOOLEAN, value) && value.as.boolean == before.as.boolean;
    end(call);
    pass(9, ok);

    begin(10); // functions that belong to a class
    TSUCHI_Object math{};
    call = {};
    ok = expect(api.find_class(self, "KismetMathLibrary", &math)) == TSUCHI_OK && expect(api.begin_call(self, math, "Abs", &call)) == TSUCHI_OK &&
         argument(call, "A", number_value(-2.5)) && expect(api.invoke(self, call)) == TSUCHI_OK && result_number(call, "ReturnValue", status.reflect_abs);
    end(call);
    call = {};
    ok = ok && expect(api.begin_call(self, math, "Add_IntInt", &call)) == TSUCHI_OK && argument(call, "A", integer_value(40)) &&
         argument(call, "B", integer_value(2)) && expect(api.invoke(self, call)) == TSUCHI_OK && result(call, "ReturnValue", TSUCHI_VALUE_INTEGER, value);
    end(call);
    status.reflect_sum = ok ? int32_t(value.as.integer) : -1;
    pass(10, ok && status.reflect_abs == 2.5f && status.reflect_sum == 42);

    begin(11); // a value from a list of named ones, as a field and from a function
    ok = field(a, "Role", TSUCHI_VALUE_INTEGER, before) && run(a, "GetLocalRole", call) && result(call, "ReturnValue", TSUCHI_VALUE_INTEGER, value) &&
         value.as.integer == before.as.integer;
    end(call);
    status.reflect_role = ok ? int32_t(before.as.integer) : -1;
    pass(11, ok);

    begin(12); // a function that returns a string
    TSUCHI_Object system{};
    char text[128] = "";
    other = TSUCHI_Value{};
    other.kind = TSUCHI_VALUE_OBJECT;
    other.as.object = a;
    call = {};
    ok = expect(api.find_class(self, "KismetSystemLibrary", &system)) == TSUCHI_OK && expect(api.begin_call(self, system, "GetObjectName", &call)) == TSUCHI_OK &&
         argument(call, "Object", other) && expect(api.invoke(self, call)) == TSUCHI_OK && result(call, "ReturnValue", TSUCHI_VALUE_TEXT, value) &&
         expect(api.call_get_text(self, call, "ReturnValue", text, sizeof text, &needed)) == TSUCHI_OK && needed == value.count &&
         expect(api.name(self, a, name, sizeof name, &needed)) == TSUCHI_OK && !strcmp(text, name);
    end(call);
    copy_text(status.reflect_text, sizeof status.reflect_text, text);
    pass(12, ok);

    begin(13); // a field that holds a name
    text[0] = 0;
    ok = field(a, "NetDriverName", TSUCHI_VALUE_TEXT, value) && expect(api.get_text(self, a, "NetDriverName", text, sizeof text, &needed)) == TSUCHI_OK &&
         needed == value.count && needed == strlen(text) + 1 && needed > 1;
    copy_text(status.reflect_name_field, sizeof status.reflect_name_field, text);
    pass(13, ok);

    begin(14); // an array made longer, a name written into it, and the game's own code finding it there
    TSUCHI_Value tags{};
    TSUCHI_PreparedCall has{};
    ok = field(a, "Tags", TSUCHI_VALUE_ARRAY, tags);
    const uint32_t length = tags.count;
    char slot[24] = "Tags[";
    size_t end_of = 5;
    char digits[10];
    uint32_t digit_count = 0, rest = length;
    do {
        digits[digit_count++] = char('0' + rest % 10);
        rest /= 10;
    } while (rest);
    while (digit_count) slot[end_of++] = digits[--digit_count];
    slot[end_of++] = ']';
    slot[end_of] = 0;
    text[0] = 0;
    ok = ok && expect(api.resize(self, a, "Tags", length + 2)) == TSUCHI_OK && expect(api.set_text(self, a, slot, "TsuchiCheck")) == TSUCHI_OK &&
         expect(api.get_text(self, a, slot, text, sizeof text, &needed)) == TSUCHI_OK && !strcmp(text, "TsuchiCheck") &&
         expect(api.begin_call(self, a, "ActorHasTag", &has)) == TSUCHI_OK && expect(api.call_set_text(self, has, "Tag", "TsuchiCheck")) == TSUCHI_OK &&
         expect(api.invoke(self, has)) == TSUCHI_OK && result(has, "ReturnValue", TSUCHI_VALUE_BOOLEAN, value) && value.as.boolean;
    // Back to the length it had, and the game no longer finds it.
    const bool shrunk = expect(api.resize(self, a, "Tags", length)) == TSUCHI_OK && field(a, "Tags", TSUCHI_VALUE_ARRAY, tags) && tags.count == length &&
                        has.id && expect(api.invoke(self, has)) == TSUCHI_OK && result(has, "ReturnValue", TSUCHI_VALUE_BOOLEAN, value) && !value.as.boolean;
    end(has);
    pass(14, ok && shrunk);

    begin(15); // strings as arguments
    TSUCHI_Object strings{};
    call = {};
    ok = expect(api.find_class(self, "KismetStringLibrary", &strings)) == TSUCHI_OK && expect(api.begin_call(self, strings, "Len", &call)) == TSUCHI_OK &&
         expect(api.call_set_text(self, call, "S", "h\xC3\xA9llo")) == TSUCHI_OK && expect(api.invoke(self, call)) == TSUCHI_OK &&
         result(call, "ReturnValue", TSUCHI_VALUE_INTEGER, value);
    end(call);
    status.reflect_length = ok ? int32_t(value.as.integer) : -1;
    call = {};
    text[0] = 0;
    ok = ok && expect(api.begin_call(self, strings, "Concat_StrStr", &call)) == TSUCHI_OK && expect(api.call_set_text(self, call, "A", "Pik")) == TSUCHI_OK &&
         expect(api.call_set_text(self, call, "B", "min")) == TSUCHI_OK && expect(api.invoke(self, call)) == TSUCHI_OK &&
         expect(api.call_get_text(self, call, "ReturnValue", text, sizeof text, &needed)) == TSUCHI_OK && !strcmp(text, "Pikmin");
    end(call);
    pass(15, ok && status.reflect_length == 5);

    begin(16); // lists of strings and of structs as arguments, made longer and shorter
    call = {};
    text[0] = 0;
    ok = expect(api.begin_call(self, strings, "JoinStringArray", &call)) == TSUCHI_OK && expect(api.call_resize(self, call, "SourceArray", 3)) == TSUCHI_OK &&
         expect(api.call_set_text(self, call, "SourceArray[0]", "red")) == TSUCHI_OK && expect(api.call_set_text(self, call, "SourceArray[1]", "blue")) == TSUCHI_OK &&
         expect(api.call_set_text(self, call, "SourceArray[2]", "yellow")) == TSUCHI_OK && expect(api.call_resize(self, call, "SourceArray", 2)) == TSUCHI_OK &&
         expect(api.call_set_text(self, call, "Separator", ", ")) == TSUCHI_OK && expect(api.invoke(self, call)) == TSUCHI_OK &&
         expect(api.call_get_text(self, call, "ReturnValue", text, sizeof text, &needed)) == TSUCHI_OK && !strcmp(text, "red, blue");
    end(call);
    copy_text(status.reflect_joined, sizeof status.reflect_joined, text);
    call = {};
    float height = 0;
    ok = ok && expect(api.begin_call(self, math, "GetVectorArrayAverage", &call)) == TSUCHI_OK && expect(api.call_resize(self, call, "Vectors", 2)) == TSUCHI_OK &&
         argument(call, "Vectors[0].X", number_value(2)) && argument(call, "Vectors[1].X", number_value(4)) &&
         argument(call, "Vectors[1].Z", number_value(10)) && expect(api.invoke(self, call)) == TSUCHI_OK &&
         result_number(call, "ReturnValue.X", status.reflect_average) && result_number(call, "ReturnValue.Z", height);
    end(call);
    pass(16, ok && status.reflect_average == 3.0f && height == 5.0f);

    begin(17); // lists of fields, and of what a function gives back
    const char* const axes[] = {"X", "Y", "Z"};
    ok = true;
    for (uint32_t axis = 0; axis < 3; ++axis)
        ok = ok && expect(api.field_at(self, a, "RootComponent.RelativeLocation", axis, text, sizeof text, &needed)) == TSUCHI_OK && !strcmp(text, axes[axis]);
    ok = ok && api.field_at(self, a, "RootComponent.RelativeLocation", 3, text, sizeof text, &needed) == TSUCHI_NOT_FOUND &&
         expect(api.field_at(self, a, "", 0, text, sizeof text, &needed)) == TSUCHI_OK && needed > 1;
    call = {};
    ok = ok && expect(api.begin_call(self, a, "GetActorBounds", &call)) == TSUCHI_OK &&
         expect(api.call_field_at(self, call, "", 0, text, sizeof text, &needed)) == TSUCHI_OK && !strcmp(text, "bOnlyCollidingComponents") &&
         expect(api.call_output_at(self, call, 0, text, sizeof text, &needed)) == TSUCHI_OK && !strcmp(text, "Origin") &&
         expect(api.call_output_at(self, call, 1, text, sizeof text, &needed)) == TSUCHI_OK && !strcmp(text, "BoxExtent") &&
         api.call_output_at(self, call, 2, text, sizeof text, &needed) == TSUCHI_NOT_FOUND;
    end(call);
    pass(17, ok);

    begin(18); // what should be refused is, with the result that says why
    value = boolean_value(true);
    call = {};
    pass(18, api.get(self, a, "NoSuchField", &other) == TSUCHI_NOT_FOUND && api.begin_call(self, a, "NoSuchFunction", &call) == TSUCHI_NOT_FOUND &&
                 api.set(self, a, "PrimaryActorTick.TickInterval", &value) == TSUCHI_INVALID && api.get(self, a, "Tags[", &other) == TSUCHI_INVALID &&
                 api.get(self, a, "Tags[100000]", &other) == TSUCHI_NOT_FOUND);

    begin(19); // nothing is left behind
    status.reflect_left_open = owned_count(self) - owned;
    pass(19, status.reflect_left_open == 0);
    status.reflect_test = 2;
}

// ---- The check of the objects service ---------------------------------------

void check_objects() {
    const TSUCHI_EntityService& api = *host.entity;
    status.objects_test = 1;
    status.objects_passed = status.objects_placed = status.objects_compared = status.objects_differed = 0;
    status.objects_result = 0;
    const auto need = [](TSUCHI_Result result) {
        if (result != TSUCHI_OK && !status.objects_result) status.objects_result = result;
        return result == TSUCHI_OK;
    };
    const auto pass = [](uint32_t step, bool ok) { status.objects_passed |= uint32_t(ok) << step; };

    // How many there are, then all of them.
    static TSUCHI_Actor actors[4096];
    uint64_t count = 0;
    const TSUCHI_Result counted = api.list(self, {}, nullptr, 0, &count);
    pass(0, counted == TSUCHI_OK || counted == TSUCHI_CAPACITY); // a world is loaded
    status.objects_count = uint32_t(count);
    if ((counted != TSUCHI_OK && counted != TSUCHI_CAPACITY) || !count || count > 4096) {
        status.objects_result = counted;
        status.objects_test = 2;
        return;
    }
    uint64_t listed = 0;
    bool ok = need(api.list(self, {}, actors, 4096, &listed)) && listed == count;
    pass(1, ok);
    if (!ok) {
        status.objects_test = 2;
        return;
    }

    // What the first one is.
    const TSUCHI_Actor first = actors[0];
    char kind[64] = "";
    uint64_t needed = 0;
    bool alive = false;
    ok = need(api.kind(self, first, kind, sizeof kind, &needed)) && needed > 1 && need(host.actor->alive(self, first, &alive)) && alive;
    copy_text(status.objects_kind, sizeof status.objects_kind, kind);
    pass(2, ok);
    uint64_t placement = 0;
    ok = need(api.team(self, first, &status.objects_team)) && need(api.placement(self, first, &placement));
    pass(3, ok);

    // Where it is; and put where it already is, it is still there.
    TSUCHI_Pose at{}, after{};
    ok = need(api.pose(self, first, &at));
    status.objects_location[0] = at.location.x, status.objects_location[1] = at.location.y, status.objects_location[2] = at.location.z;
    status.objects_rotation[0] = at.rotation.pitch, status.objects_rotation[1] = at.rotation.yaw, status.objects_rotation[2] = at.rotation.roll;
    pass(4, ok);
    ok = ok && need(api.set_pose(self, first, &at)) && need(api.pose(self, first, &after)) &&
         near(after.location.x, at.location.x, 0.5f) && near(after.location.y, at.location.y, 0.5f) && near(after.location.z, at.location.z, 0.5f) &&
         near(after.rotation.pitch, at.rotation.pitch, 0.5f) && near(after.rotation.yaw, at.rotation.yaw, 0.5f) && near(after.rotation.roll, at.rotation.roll, 0.5f);
    pass(5, ok);

    // Every actor is of the game's actor class, so asking by that class gives them all.
    TSUCHI_Object actor_class{};
    uint64_t by_class = 0;
    const TSUCHI_Result classed = need(host.reflection->find_class(self, "GActor", &actor_class)) ? api.list(self, actor_class, nullptr, 0, &by_class)
                                                                                           : TSUCHI_NOT_FOUND;
    pass(6, (classed == TSUCHI_OK || classed == TSUCHI_CAPACITY) && by_class == count);

    // One a level placed is found again by its number.
    TSUCHI_Actor placed{}, found{};
    uint64_t number = 0;
    for (uint64_t i = 0; i < count; ++i) {
        if (api.placement(self, actors[i], &placement) != TSUCHI_OK || !placement) continue;
        if (!status.objects_placed++) placed = actors[i], number = placement;
    }
    pass(7, placed.id && need(api.find_placed(self, number, &found)) && found.id == placed.id);

    // Every actor's place, against what the game's own function says of the same actor.
    for (uint64_t i = 0; i < count; ++i) {
        TSUCHI_Pose here{};
        float game_says[3];
        const TSUCHI_Object object = tsuchi_actor_as_object(actors[i]);
        TSUCHI_PreparedCall call{};
        if (api.pose(self, actors[i], &here) != TSUCHI_OK || host.reflection->begin_call(self, object, "K2_GetActorLocation", &call) != TSUCHI_OK) continue;
        const bool read = host.reflection->invoke(self, call) == TSUCHI_OK && result_number(call, "ReturnValue.X", game_says[0]) &&
                          result_number(call, "ReturnValue.Y", game_says[1]) && result_number(call, "ReturnValue.Z", game_says[2]);
        host.reflection->end_call(self, call);
        if (!read) continue;
        ++status.objects_compared;
        status.objects_differed += !(near(here.location.x, game_says[0], 0.01f) && near(here.location.y, game_says[1], 0.01f) &&
                                     near(here.location.z, game_says[2], 0.01f));
    }
    pass(8, status.objects_compared && !status.objects_differed); // an actor with no place in the world is not compared

    // What the list has against what was seen entering and leaving.
    uint32_t seen = 0;
    for (uint64_t i = 0; i < count; ++i) seen += actor_seen_in_play(game::from_handle(actors[i].id));
    status.events_unseen = uint32_t(count) - seen;
    status.events_unlisted = actor_event_counts().in_play - seen;
    pass(9, !status.events_unseen);

    // Notes for whoever reads the block: the kinds in play, and anything seen entering that the list lacks.
    size_t used = 0;
    const auto note = [&](const char* text) {
        for (; *text && used + 1 < sizeof status.notes; ++text) status.notes[used++] = *text;
        status.notes[used] = 0;
    };
    const auto note_number = [&](uint32_t value) {
        char digits[12];
        int length = 0;
        do {
            digits[length++] = char('0' + value % 10);
            value /= 10;
        } while (value);
        while (length && used + 1 < sizeof status.notes) status.notes[used++] = digits[--length];
        status.notes[used] = 0;
    };
    static game::Name kinds[256];
    static uint32_t kind_counts[256];
    uint32_t kind_total = 0;
    for (uint64_t i = 0; i < count; ++i) {
        const game::Name kind_name = game::actor_kind(game::from_handle(actors[i].id));
        uint32_t at = 0;
        while (at < kind_total && (kinds[at].index != kind_name.index || kinds[at].number != kind_name.number)) ++at;
        if (at == kind_total) {
            if (kind_total == 256) continue;
            kinds[kind_total] = kind_name, kind_counts[kind_total++] = 0;
        }
        ++kind_counts[at];
    }
    for (uint32_t i = 0; i < kind_total; ++i) {
        char text_of[64];
        if (!game::name_text(kinds[i], text_of, sizeof text_of)) continue;
        note(text_of), note("="), note_number(kind_counts[i]), note(" ");
    }
    struct Stray {
        const TSUCHI_Actor* listed;
        uint64_t count;
        decltype(note)* note;
    } stray{actors, count, &note};
    game::each_object(
        [](void* context, uint64_t object) {
            auto& stray = *static_cast<Stray*>(context);
            if (!actor_seen_in_play(object)) return true;
            const uint64_t handle = game::to_handle(object);
            for (uint64_t i = 0; i < stray.count; ++i)
                if (stray.listed[i].id == handle) return true;
            char text_of[64];
            (*stray.note)("| unlisted ");
            if (game::name_text(game::name_of(object), text_of, sizeof text_of)) (*stray.note)(text_of);
            (*stray.note)(" kind ");
            if (game::name_text(game::actor_kind(object), text_of, sizeof text_of)) (*stray.note)(text_of);
            (*stray.note)(" class ");
            if (game::name_text(game::name_of(game::class_of(object)), text_of, sizeof text_of)) (*stray.note)(text_of);
            (*stray.note)(" ");
            return true;
        },
        &stray);
    status.objects_test = 2;
}

// ---- Native allocation and pooled-lifetime checks ------------------------------
// check.spawn covers the public spawn, placement, removal and event assertions.
// These actors also supply the remaining native checks below. Retired pass bits
// stay zero; list counts, owned allocations and the retained pooled object remain.

struct SpawnCheck {
    TSUCHI_Actor made[2]{};
    TSUCHI_Actor pooled{};
    uint64_t pooled_object = 0;
    TSUCHI_Vec3 asked[2]{};
    uint64_t listed_before = 0;
    uint32_t owned_before = 0, put_away = 0, destroyed = 0, asked_frame = 0, removal_frame = 0;
    bool verified = false;
    uint32_t suspend_frame = 0, owned_at_suspend = 0, messages_before = 0, control_frame = 0;
    uint32_t body_frame = 0, owned_at_body = 0;
    uint32_t parts_frame = 0, owned_at_parts = 0;
    uint32_t game_frame = 0, changes_at_hold = 0, title_frame = 0;
    TSUCHI_PauseHold pause_hold{};
    uint32_t captains_frame = 0, whistles_before = 0;
    bool whistle_heard = false;
    TSUCHI_Vec3 aim{}, thrown_from{};
    TSUCHI_Actor thrown{};
    uint32_t herd_frame = 0, wild_frame = 0, hud_frame = 0, day_frame = 0;
    uint32_t day_area = 0, day_alive_before = 0;
    TSUCHI_Actor day_victim{};
    TSUCHI_Actor prey{};
    uint64_t listed_pikmin = 0, led = 0;
    TSUCHI_Actor herd_one{}, herd_new[3]{};
    TSUCHI_Actor captain{}, dog{};
    TSUCHI_Vec3 held_at{};
    TSUCHI_Component part{}, bone_part{};
    TSUCHI_Object own_material{}, other_material{};
    uint32_t paint_slot = 0;
    uint32_t owned_at_paint = 0;
    uint32_t owned_at_added = 0;
    TSUCHI_Vec3 control_at{}, resumed_at{};
    TSUCHI_Vec3 suspended_at{};
    uint32_t moved_frame = 0;
} spawn_check;

bool spawn_need(TSUCHI_Result result) {
    if (result != TSUCHI_OK && !status.spawn_result) status.spawn_result = result;
    return result == TSUCHI_OK;
}
void spawn_pass(uint32_t step, bool ok) { status.spawn_passed |= uint32_t(ok) << step; }
uint64_t actors_listed() {
    uint64_t count = 0;
    host.entity->list(self, {}, nullptr, 0, &count);
    return count;
}

// ---- The check of the behavior service ------------------------------------------

TSUCHI_Subscription behavior_subscription{};
TSUCHI_Actor behavior_creature{}, avatar_actor{};
void behavior_pass(uint32_t step, bool ok) { status.behavior_passed |= uint32_t(ok) << step; }

// What an actor is doing, as much of it as fits the record.
bool doing(TSUCHI_Actor actor, char* out, size_t capacity) {
    static char text[4096];
    uint64_t needed = 0;
    const TSUCHI_Result result = host.behavior->action(self, actor, text, sizeof text, &needed);
    if (result != TSUCHI_OK) {
        if (!status.behavior_result) status.behavior_result = result;
        return false;
    }
    copy_text(out, capacity, text);
    return true;
}

void heard_changed(void*, const TSUCHI_BehaviorChanged* event) {
    ++status.behavior_events;
    static char now[4096];
    uint64_t needed = 0;
    if (host.behavior->action(self, event->actor, now, sizeof now, &needed) == TSUCHI_OK && strcmp(now, event->action)) ++status.behavior_mismatch;
    if (behavior_watched() > status.behavior_watched) status.behavior_watched = behavior_watched();
    if (event->actor.id != behavior_creature.id) return;
    ++status.behavior_own;
    // Added to the trail while there is room for all of it.
    const size_t used = strlen(status.behavior_trail), more = strlen(event->action);
    if (used + more + 2 > sizeof status.behavior_trail) return;
    memcpy(status.behavior_trail + used, event->action, more);
    status.behavior_trail[used + more] = ' ', status.behavior_trail[used + more + 1] = 0;
}

// Pikmin deaths are listened for over the same stretch.
TSUCHI_Subscription died_subscription{};
TSUCHI_CombatPikminDied herd_death{};
TSUCHI_Actor herd_victim{};
void heard_died(void*, const TSUCHI_CombatPikminDied* event) {
    ++status.died_heard;
    if (herd_victim.id && event->pikmin.id == herd_victim.id) herd_death = *event;
}

// Finds an actor of a kind among those in play.
bool first_of_kind(const char* wanted, TSUCHI_Actor& out) {
    static TSUCHI_Actor actors[4096];
    uint64_t count = 0;
    if (host.entity->list(self, {}, actors, 4096, &count) != TSUCHI_OK) return false;
    for (uint64_t i = 0; i < count; ++i) {
        char kind[64];
        uint64_t needed = 0;
        if (host.entity->kind(self, actors[i], kind, sizeof kind, &needed) != TSUCHI_OK || strcmp(kind, wanted)) continue;
        out = actors[i];
        return true;
    }
    return false;
}

void spawn_begin() {
    const TSUCHI_EntityService& api = *host.entity;
    SpawnCheck& c = spawn_check;
    // Listening for what actors do starts here and ends just before the removal.
    behavior_pass(0, first_of_kind("AVATAR", avatar_actor) && doing(avatar_actor, status.behavior_avatar, sizeof status.behavior_avatar) &&
                         status.behavior_avatar[0]);
    behavior_pass(2, host.behavior->on_changed(self, heard_changed, nullptr, &behavior_subscription) == TSUCHI_OK &&
                         host.combat->on_pikmin_died(self, heard_died, nullptr, &died_subscription) == TSUCHI_OK);
    c.listed_before = actors_listed();
    c.owned_before = owned_count(self);
    c.put_away = actor_event_counts().put_away, c.destroyed = actor_event_counts().destroyed;
    const char* const kinds[2] = {"CRACKPOT", "CHAPPY"};
    for (int i = 0; i < 2; ++i) {
        TSUCHI_Actor original{};
        TSUCHI_Object type{};
        TSUCHI_Pose at{};
        const bool ok = first_of_kind(kinds[i], original) && spawn_need(host.reflection->class_of(self, tsuchi_actor_as_object(original), &type)) &&
                        spawn_need(api.pose(self, original, &at));
        if (!ok) continue;
        c.asked[i] = {at.location.x + 150, at.location.y, at.location.z + 20}; // beside the one the level has
        spawn_need(api.spawn(self, type, &c.asked[i], &at.rotation, -1, false, &c.made[i]));
    }

    // A Pikmin of the level's own, for the pool: a copy made by the spawner is made new and is not
    // in any pool, as a run showed, so only one the game put out itself will do.
    TSUCHI_Actor pikmin{};
    if (first_of_kind("PIKMINPHOTON", pikmin)) c.pooled = pikmin;
    c.pooled_object = game::from_handle(c.pooled.id);
    spawn_pass(10, c.pooled_object != 0);
    status.pool_made = c.pooled_object && (game::read_or<uint64_t>(c.pooled_object + t::game_actor::pool) ||
                                           game::read_or<uint64_t>(c.pooled_object + t::game_actor::kind_pool));
    c.asked_frame = status.frames;
    status.spawn_test = 1;
}

void spawn_verify() {
    const TSUCHI_EntityService& api = *host.entity;
    SpawnCheck& c = spawn_check;
    const char* const kinds[2] = {"CRACKPOT", "CHAPPY"};
    bool there = true;
    for (int i = 0; i < 2; ++i) {
        char kind[64] = "";
        uint64_t needed = 0;
        bool alive = false;
        there = there && c.made[i].id && host.actor->alive(self, c.made[i], &alive) == TSUCHI_OK && alive &&
                api.kind(self, c.made[i], kind, sizeof kind, &needed) == TSUCHI_OK && !strcmp(kind, kinds[i]);
    }
    behavior_creature = c.made[1];
    behavior_pass(1, doing(c.made[1], status.behavior_creature, sizeof status.behavior_creature));
    spawn_pass(3, there && actors_listed() == c.listed_before + 2 && owned_count(self) == c.owned_before + 2);
    c.verified = true;
    c.moved_frame = status.frames;
    // Read-only setup shared by the remaining captain, squad and trace checks.
    host.player->captain(self, 0, &c.captain);
    host.player->dog(self, 0, &c.dog);
}

void spawn_remove() {
    const TSUCHI_EntityService& api = *host.entity;
    SpawnCheck& c = spawn_check;
    if (c.pooled.id && spawn_need(api.destroy(self, c.pooled))) --c.listed_before;
    for (int i = 0; i < 2; ++i)
        if (c.made[i].id) spawn_need(api.destroy(self, c.made[i]));
    c.removal_frame = status.frames;
    status.spawn_test = 2;
}

void added_pass(uint32_t step, bool ok);

void spawn_finish() {
    SpawnCheck& c = spawn_check;
    bool gone = true;
    for (int i = 0; i < 2; ++i) {
        bool alive = true;
        gone = gone && c.made[i].id && host.actor->alive(self, c.made[i], &alive) == TSUCHI_OK && !alive;
    }
    bool pooled_alive = true;
    const bool pooled_gone = c.pooled.id && host.actor->alive(self, c.pooled, &pooled_alive) == TSUCHI_OK && !pooled_alive;
    gone = gone && (pooled_gone || !c.pooled.id);
    const bool late = status.frames >= c.removal_frame + 300;
    if (!gone && !late) return; // give the game's own removal its time
    status.spawn_frames = status.frames - c.removal_frame;
    status.spawn_left_pool = actor_event_counts().put_away - c.put_away;
    status.spawn_left_destroyed = actor_event_counts().destroyed - c.destroyed;
    // The level goes on while the checks run, so the count is the same give or take what came and went by itself.
    const uint64_t listed = actors_listed();
    spawn_pass(7, gone && listed + 3 >= c.listed_before && listed <= c.listed_before + 3);
    behavior_pass(5, behavior_watched() == 0); // nobody listening: nobody watched
    // The part left on the creature went with it. That it is no longer held against the framework
    // shows in the count of what the framework owns, below.
    added_pass(7, c.bone_part.id && !game::from_handle(c.bone_part.id) && added_count() == 0);
    spawn_pass(9, owned_count(self) == c.owned_before);
    // The pooled one: its handle is over, and the object is still there, put away.
    status.pool_object_lives = pooled_gone && game::live(c.pooled_object) && game::to_handle(c.pooled_object) != c.pooled.id;
    spawn_pass(11, pooled_gone);
    spawn_pass(12, status.pool_object_lives && status.spawn_left_pool >= 1);
    status.spawn_test = 3;
}

// ---- The check of suspending -------------------------------------------------

bool suspend_need(TSUCHI_Result result) {
    if (result != TSUCHI_OK && !status.suspend_result) status.suspend_result = result;
    return result == TSUCHI_OK;
}
void suspend_pass(uint32_t step, bool ok) { status.suspend_passed |= uint32_t(ok) << step; }

// Asks the engine whether something is being updated: one of its own functions, by name.
bool ticking(uint64_t object, const char* function) {
    const TSUCHI_Object target{game::to_handle(object)};
    TSUCHI_PreparedCall call{};
    TSUCHI_Value value{};
    const bool on = target.id && host.reflection->begin_call(self, target, function, &call) == TSUCHI_OK && host.reflection->invoke(self, call) == TSUCHI_OK &&
                    host.reflection->call_get(self, call, "ReturnValue", &value) == TSUCHI_OK && value.kind == TSUCHI_VALUE_BOOLEAN && value.as.boolean;
    end(call);
    return on;
}
float moved_since(TSUCHI_Actor actor, const TSUCHI_Vec3& from) {
    TSUCHI_Pose at{};
    if (host.entity->pose(self, actor, &at) != TSUCHI_OK) return -1;
    const float dx = at.location.x - from.x, dy = at.location.y - from.y, dz = at.location.z - from.z;
    return math::square_root(dx * dx + dy * dy + dz * dz);
}
TSUCHI_Vec3 place_of(TSUCHI_Actor actor) {
    TSUCHI_Pose at{};
    host.entity->pose(self, actor, &at);
    return at.location;
}

uint32_t switches_of(uint64_t actor) {
    const uint64_t brain = game::ai_component(actor), mover = game::movement_component(actor);
    return uint32_t(ticking(actor, "IsActorTickEnabled")) | uint32_t(brain && ticking(brain, "IsComponentTickEnabled")) << 1 |
           uint32_t(mover && ticking(mover, "IsComponentTickEnabled")) << 2;
}

// Notes on the suspended creature and its body.
size_t suspend_used = 0;
void suspend_note(const char* text) {
    for (; *text && suspend_used + 1 < sizeof status.suspend_notes; ++text) status.suspend_notes[suspend_used++] = *text;
    status.suspend_notes[suspend_used] = 0;
}
void suspend_number(long long whole) {
    char digits[24];
    int length = 0;
    const bool negative = whole < 0;
    if (negative) whole = -whole;
    do {
        digits[length++] = char('0' + whole % 10);
        whole /= 10;
    } while (whole);
    if (negative) suspend_note("-");
    while (length && suspend_used + 1 < sizeof status.suspend_notes) status.suspend_notes[suspend_used++] = digits[--length];
    status.suspend_notes[suspend_used] = 0;
}
// What a part says of itself: its class, whether the engine may ever update it, and what asking the engine returns.
void describe_part(const char* label, uint64_t object, const char* tick_field, const char* function) {
    suspend_note(label), suspend_note("=");
    if (!object) return suspend_note("none; ");
    char name[64];
    if (game::name_text(game::name_of(game::class_of(object)), name, sizeof name)) suspend_note(name);
    const TSUCHI_Object target{game::to_handle(object)};
    TSUCHI_Value value{};
    char path[96];
    const char* const fields[] = {"bCanEverTick", "bStartWithTickEnabled"};
    for (const char* field : fields) {
        size_t n = 0;
        for (const char* s = tick_field; *s; ++s) path[n++] = *s;
        path[n++] = '.';
        for (const char* s = field; *s; ++s) path[n++] = *s;
        path[n] = 0;
        const TSUCHI_Result result = host.reflection->get(self, target, path, &value);
        suspend_note(" "), suspend_note(field + 1), suspend_note(":");
        suspend_number(result == TSUCHI_OK ? (value.kind == TSUCHI_VALUE_BOOLEAN ? value.as.boolean : -2) : -100 - int(result));
    }
    TSUCHI_PreparedCall call{};
    const TSUCHI_Result found = host.reflection->begin_call(self, target, function, &call);
    const TSUCHI_Result ran = found == TSUCHI_OK ? host.reflection->invoke(self, call) : found;
    const TSUCHI_Result read = ran == TSUCHI_OK ? host.reflection->call_get(self, call, "ReturnValue", &value) : ran;
    suspend_note(" asked:"), suspend_number(read == TSUCHI_OK ? (value.kind == TSUCHI_VALUE_BOOLEAN ? value.as.boolean : -2) : -100 - int(read));
    end(call);
    suspend_note("; ");
}
void describe_creature(const char* when, uint64_t address) {
    suspend_note(when), suspend_note(" ");
    describe_part("actor", address, "PrimaryActorTick", "IsActorTickEnabled");
    describe_part("brain", game::ai_component(address), "PrimaryComponentTick", "IsComponentTickEnabled");
    describe_part("mover", game::movement_component(address), "PrimaryComponentTick", "IsComponentTickEnabled");
}

void suspend_begin() {
    const TSUCHI_EntityService& api = *host.entity;
    SpawnCheck& c = spawn_check;
    const uint64_t address = game::from_handle(c.made[1].id);
    suspend_used = 0;
    describe_creature("before", address);
    TSUCHI_Pose at{};
    TSUCHI_EntityParts parts = 0;
    status.suspend_control = moved_since(c.made[1], c.control_at);
    c.owned_at_suspend = owned_count(self);
    c.messages_before = suspend_counts().messages;
    status.suspend_before = address ? switches_of(address) : 0;
    api.pose(self, c.made[1], &at);
    c.suspended_at = at.location;
    const bool ok = suspend_need(api.hold_suspended(self, c.made[1], TSUCHI_ENTITY_PARTS_LOGIC | TSUCHI_ENTITY_PARTS_MOVEMENT)) &&
                    suspend_need(api.suspended(self, c.made[1], &parts)) && parts == (TSUCHI_ENTITY_PARTS_LOGIC | TSUCHI_ENTITY_PARTS_MOVEMENT);
    suspend_pass(0, ok);
    c.suspend_frame = status.frames;
    status.suspend_test = 1;
}

void suspend_end() {
    const TSUCHI_EntityService& api = *host.entity;
    SpawnCheck& c = spawn_check;
    const uint64_t address = game::from_handle(c.made[1].id);
    TSUCHI_Pose at{};
    TSUCHI_EntityParts parts = 99;
    status.suspend_during = address ? switches_of(address) : 0xFF;
    describe_creature("| during", address);
    suspend_pass(1, !(status.suspend_during & 6));
    suspend_pass(2, !(status.suspend_during & 1));
    if (api.pose(self, c.made[1], &at) == TSUCHI_OK) {
        const float dx = at.location.x - c.suspended_at.x, dy = at.location.y - c.suspended_at.y, dz = at.location.z - c.suspended_at.z;
        status.suspend_drift = math::square_root(dx * dx + dy * dy + dz * dz);
    }
    status.suspend_messages = suspend_counts().messages - c.messages_before;
    c.resumed_at = place_of(c.made[1]);
    const bool ok = suspend_need(api.release_suspended(self, c.made[1], TSUCHI_ENTITY_PARTS_LOGIC | TSUCHI_ENTITY_PARTS_MOVEMENT)) &&
                    suspend_need(api.suspended(self, c.made[1], &parts)) && parts == 0;
    suspend_pass(3, ok);
    c.suspend_frame = status.frames;
    status.suspend_test = 2;
}

void suspend_after() {
    SpawnCheck& c = spawn_check;
    const uint64_t address = game::from_handle(c.made[1].id);
    status.suspend_after = address ? switches_of(address) : 0xFF;
    status.suspend_resumed = moved_since(c.made[1], c.resumed_at);
    suspend_pass(4, (status.suspend_after & 6) == (status.suspend_before & 6));
    suspend_pass(5, (status.suspend_after & 1) == (status.suspend_before & 1));
    suspend_pass(6, owned_count(self) == c.owned_at_suspend);
    // It walks when left to itself, stops when suspended, and walks again when given back.
    suspend_pass(7, status.suspend_control > 100 && status.suspend_drift < status.suspend_control / 4 && status.suspend_resumed > 100);
    c.moved_frame = status.frames; // the removal waits a little after this
    status.suspend_test = 3;
}

// ---- The check of suspending physics and solidity -----------------------------

bool body_need(TSUCHI_Result result) {
    if (result != TSUCHI_OK && !status.body_result) status.body_result = result;
    return result == TSUCHI_OK;
}
void body_pass(uint32_t step, bool ok) { status.body_passed |= uint32_t(ok) << step; }

// Asks the engine about a part, by name: a yes or no, or a small number. -1 when it cannot be asked.
int asked_of(uint64_t part, const char* function) {
    const TSUCHI_Object target{game::to_handle(part)};
    TSUCHI_PreparedCall call{};
    TSUCHI_Value value{};
    int answer = -1;
    if (target.id && host.reflection->begin_call(self, target, function, &call) == TSUCHI_OK && host.reflection->invoke(self, call) == TSUCHI_OK &&
        host.reflection->call_get(self, call, "ReturnValue", &value) == TSUCHI_OK)
        answer = value.kind == TSUCHI_VALUE_BOOLEAN ? int(value.as.boolean) : value.kind == TSUCHI_VALUE_INTEGER ? int(value.as.integer) : -1;
    end(call);
    return answer;
}

constexpr TSUCHI_EntityParts kEverything = TSUCHI_ENTITY_PARTS_LOGIC | TSUCHI_ENTITY_PARTS_MOVEMENT | TSUCHI_ENTITY_PARTS_PHYSICS | TSUCHI_ENTITY_PARTS_COLLISION;

// check.body covers direct body reads, blocked launch, hovering and falling.
// This native hold remains only to compare the framework's allocation count.
void body_begin() {
    SpawnCheck& c = spawn_check;
    c.owned_at_body = owned_count(self);
    body_need(host.entity->hold_suspended(self, c.made[1], kEverything));
    c.body_frame = status.frames;
    status.body_test = 1;
}

void body_release() {
    SpawnCheck& c = spawn_check;
    body_need(host.entity->release_suspended(self, c.made[1], kEverything));
    c.body_frame = status.frames;
    status.body_test = 2;
}

struct MostParts {
    uint64_t actor;
    uint32_t parts;
};
bool more_parts(void* context, uint64_t actor) {
    auto& most = *static_cast<MostParts*>(context);
    const uint32_t parts = game::actor_primitives(actor, nullptr, 0);
    if (parts > most.parts) most = MostParts{actor, parts};
    return true;
}
void count_blocking(void* context, uint64_t component) { *static_cast<uint32_t*>(context) += game::blocks(game::collision_of(component)); }
uint32_t blocking_parts(uint64_t actor) {
    uint32_t count = 0;
    game::each_primitive(actor, count_blocking, &count);
    return count;
}

// However many parts an actor has, its solidity can be suspended and given back.
void check_most_parts() {
    MostParts most{};
    game::each_actor(more_parts, &most);
    status.most_parts = most.parts;
    if (!most.actor) return;
    game::name_text(game::actor_kind(most.actor), status.most_kind, sizeof status.most_kind);
    const TSUCHI_Actor actor{game::to_handle(most.actor)};
    const uint32_t before = blocking_parts(most.actor);
    status.most_result = host.entity->hold_suspended(self, actor, TSUCHI_ENTITY_PARTS_COLLISION);
    const uint32_t during = blocking_parts(most.actor);
    if (status.most_result == TSUCHI_OK) status.most_result = host.entity->release_suspended(self, actor, TSUCHI_ENTITY_PARTS_COLLISION);
    const uint32_t after = blocking_parts(most.actor);
    status.most_blocking = before | during << 10 | after << 20;
    body_pass(8, status.most_result == TSUCHI_OK && !during && after == before);
}

void body_finish() {
    SpawnCheck& c = spawn_check;
    body_pass(7, owned_count(self) == c.owned_at_body);
    check_most_parts();
    c.moved_frame = status.frames; // the removal waits a little after this
    status.body_test = 3;
}

// ---- The remaining pause check and native run finish ------------------------------

bool game_need(TSUCHI_Result result) {
    if (result != TSUCHI_OK && !status.game_result) status.game_result = result;
    return result == TSUCHI_OK;
}
void game_pass(uint32_t step, bool ok) { status.game_passed |= uint32_t(ok) << step; }
// Where the game is, as much of each text as fits the record.
bool stage_text(char* out, size_t capacity) {
    static char text[1024];
    uint64_t needed = 0;
    if (!game_need(host.game->stage(self, text, sizeof text, &needed))) return false;
    copy_text(out, capacity, text);
    return true;
}

// The SDK checks cover pause kinds, control, events and random state. This residual
// still observes the creature's movement and autonomous behavior during a real hold.
void game_begin() {
    const TSUCHI_GameService& api = *host.game;
    SpawnCheck& c = spawn_check;
    TSUCHI_Mode mode = TSUCHI_MODE_NONE;
    static char world[256];
    uint64_t needed = 0;
    // Context retained for the native check and tools observing its loaded world.
    api.mode(self, &mode), api.rule(self, &status.game_rule);
    stage_text(status.game_stage_level, sizeof status.game_stage_level);
    status.game_mode_level = mode;
    if (api.world(self, world, sizeof world, &needed) == TSUCHI_OK) copy_text(status.game_world, sizeof status.game_world, world);
    game_need(api.hold_pause(self, TSUCHI_PAUSE_TIMER | TSUCHI_PAUSE_ACTORS, &c.pause_hold));
    c.held_at = place_of(c.made[1]);
    c.changes_at_hold = status.behavior_events;
    c.game_frame = status.frames;
    status.game_test = 1;
}

void game_release() {
    SpawnCheck& c = spawn_check;
    const TSUCHI_Vec3 now = place_of(c.made[1]);
    const float dx = now.x - c.held_at.x, dy = now.y - c.held_at.y;
    status.game_moved_held = dx * dx + dy * dy;
    status.game_changes_held = status.behavior_events - c.changes_at_hold;
    game_pass(6, c.pause_hold.id && status.game_moved_held < 1 && status.game_changes_held <= 1); // one change may have been on its way
    game_need(host.game->release_pause(self, c.pause_hold));
    c.moved_frame = status.frames; // the next check waits a little after this
    status.game_test = 2;
}

// At the very end: back to the title screen.
void game_leave() {
    // Existing finish step for the remaining native checks, without SDK event grading.
    game_need(host.game->return_to_title(self));
    spawn_check.title_frame = status.frames;
    status.game_test = 3;
}

// Each frame after that, until the game is there.
void game_arriving() {
    TSUCHI_Mode mode = TSUCHI_MODE_NONE;
    if (host.game->mode(self, &mode) != TSUCHI_OK || mode != TSUCHI_MODE_TITLE) return;
    status.game_title_frames = status.frames - spawn_check.title_frame;
    status.game_test = 4;
}

// ---- The check of the captains service -----------------------------------------------

bool captains_need(TSUCHI_Result result) {
    if (result != TSUCHI_OK && !status.captains_result) status.captains_result = result;
    return result == TSUCHI_OK;
}
void captains_pass(uint32_t step, bool ok) { status.captains_passed |= uint32_t(ok) << step; }
void captains_next(uint32_t step) {
    spawn_check.captains_frame = status.frames;
    status.captains_test = step;
}
// How far apart across the ground, by Newton's method: the module has no math library.
float flat_distance(const TSUCHI_Vec3& a, const TSUCHI_Vec3& b) {
    const float squared = (a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y);
    if (!(squared > 0)) return 0;
    float root = squared > 1 ? squared / 2 : 1;
    for (int i = 0; i < 40; ++i) root = (root + squared / root) / 2;
    return root;
}

void captains_step() {
    const TSUCHI_CaptainService& api = *host.captain;
    SpawnCheck& c = spawn_check;
    const uint32_t waited = status.frames - c.captains_frame;
    TSUCHI_Whistle sound{};
    TSUCHI_Vec3 point{};
    TSUCHI_Pose at{};
    TSUCHI_Actor mount{};
    switch (status.captains_test) {
    case 0: // the whistle and the cursor read, and the cursor held three meters ahead
        captains_pass(0, captains_need(api.whistle(self, c.captain, &sound)) && !sound.active && api.whistle(self, c.dog, &sound) == TSUCHI_NOT_FOUND);
        c.whistles_before = sound.count;
        if (captains_need(api.cursor(self, c.captain, &point)) && captains_need(host.entity->pose(self, c.captain, &at))) {
            c.aim = {at.location.x + 300, at.location.y, at.location.z};
            captains_pass(1, captains_need(api.hold_cursor(self, c.captain, &c.aim)));
        }
        captains_next(1);
        break;
    case 1: // ten frames on the cursor is still there; the whistle is held
        if (waited < 10) break;
        if (api.cursor(self, c.captain, &point) == TSUCHI_OK) status.captains_cursor_off = flat_distance(point, c.aim);
        captains_pass(2, api.cursor(self, c.captain, &point) == TSUCHI_OK && status.captains_cursor_off < 5);
        captains_pass(3, captains_need(api.hold_whistle(self, c.captain)));
        c.whistle_heard = false;
        captains_next(2);
        break;
    case 2: // twenty frames of whistling, at the cursor
        if (api.whistle(self, c.captain, &sound) == TSUCHI_OK) {
            c.whistle_heard = c.whistle_heard || sound.active;
            if (waited == 2) status.captains_radius[0] = sound.radius;
        }
        if (waited < 20) break;
        status.captains_radius[1] = sound.radius;
        status.captains_whistles = sound.count - c.whistles_before;
        captains_pass(4, c.whistle_heard && sound.count > c.whistles_before && flat_distance(sound.center, c.aim) < 100);
        captains_pass(5, captains_need(api.release_whistle(self, c.captain)) && api.release_whistle(self, c.captain) == TSUCHI_NOT_FOUND);
        captains_next(3);
        break;
    case 3: // quiet again; then the same on a captain whose decisions are suspended
        if (waited < 5) break;
        captains_pass(6, api.whistle(self, c.captain, &sound) == TSUCHI_OK && !sound.active);
        c.whistles_before = sound.count;
        captains_need(host.entity->hold_suspended(self, c.captain, TSUCHI_ENTITY_PARTS_LOGIC));
        captains_need(api.hold_whistle(self, c.captain));
        captains_next(4);
        break;
    case 4:
        if (waited == 2 && api.whistle(self, c.captain, &sound) == TSUCHI_OK) status.captains_radius[2] = sound.radius;
        if (waited < 20) break;
        if (api.whistle(self, c.captain, &sound) == TSUCHI_OK) {
            status.captains_radius[3] = sound.radius;
            status.captains_whistles_suspended = sound.count - c.whistles_before;
        }
        captains_pass(7, sound.active && status.captains_whistles_suspended == 1 && status.captains_radius[3] > status.captains_radius[2]);
        captains_need(api.release_whistle(self, c.captain));
        captains_need(api.release_cursor(self, c.captain));
        captains_need(host.entity->release_suspended(self, c.captain, TSUCHI_ENTITY_PARTS_LOGIC));
        captains_next(5);
        break;
    case 5: // the dog is brought beside the captain
        if (waited < 10) break;
        if (captains_need(host.entity->pose(self, c.captain, &at))) {
            const TSUCHI_Vec3 beside{at.location.x + 70, at.location.y, at.location.z + 20};
            captains_need(host.entity->warp(self, c.dog, &beside, &at.rotation));
        }
        captains_next(6);
        break;
    case 6: // and the captain is asked to get on
        if (waited < 45) break;
        status.captains_ride = api.ride(self, c.captain);
        captains_pass(8, status.captains_ride == TSUCHI_OK && api.ride(self, c.captain) == TSUCHI_BUSY);
        captains_next(status.captains_ride == TSUCHI_OK ? 7 : 9);
        break;
    case 7: // on, within five seconds; then asked to get off
        if (!(api.riding(self, c.captain, &mount) == TSUCHI_OK && mount.id == c.dog.id) && waited < 150) break;
        status.captains_ride_frames = waited;
        captains_pass(9, mount.id == c.dog.id && captains_need(api.dismount(self, c.captain)));
        captains_next(8);
        break;
    case 8: { // off, and not seated again two seconds later
        const bool on = api.riding(self, c.captain, &mount) == TSUCHI_OK && mount.id;
        if (!status.captains_off_frames) {
            if (!on) status.captains_off_frames = waited ? waited : 1;
            else if (waited < 150) break;
        }
        if (status.captains_off_frames && waited < status.captains_off_frames + 60) break;
        captains_pass(10, status.captains_off_frames != 0 && !on);
        captains_next(9);
        break;
    }
    case 9: { // a throw, of the first glow Pikmin that follows the captain
        static TSUCHI_Actor actors[4096];
        uint64_t count = 0;
        if (host.entity->list(self, {}, actors, 4096, &count) != TSUCHI_OK) count = 0;
        for (uint64_t i = 0; i < count && !c.thrown.id; ++i) {
            char kind[64];
            uint64_t needed = 0;
            if (host.entity->kind(self, actors[i], kind, sizeof kind, &needed) != TSUCHI_OK || strcmp(kind, "PIKMINPHOTON")) continue;
            ++status.captains_pikmin;
            status.captains_throw = api.throw_pikmin(self, c.captain, actors[i]);
            if (status.captains_throw == TSUCHI_OK && host.entity->pose(self, actors[i], &at) == TSUCHI_OK) c.thrown = actors[i], c.thrown_from = at.location;
        }
        if (!status.captains_pikmin) status.captains_throw = -1000; // there was none to ask
        captains_next(10);
        break;
    }
    case 10: // two seconds later it is somewhere else
        if (waited < 60) break;
        if (c.thrown.id && host.entity->pose(self, c.thrown, &at) == TSUCHI_OK) status.captains_thrown = flat_distance(at.location, c.thrown_from);
        captains_pass(11, status.captains_thrown > 50);
        c.moved_frame = status.frames; // the next check waits a little after this
        captains_next(11);
        break;
    }
}

// ---- The check of the pikmin and squad services ----------------------------------------

bool herd_need(TSUCHI_Result result) {
    if (result != TSUCHI_OK && !status.herd_result) status.herd_result = result;
    return result == TSUCHI_OK;
}
void herd_pass(uint32_t step, bool ok) { status.herd_passed |= uint32_t(ok) << step; }
void herd_next(uint32_t step) {
    spawn_check.herd_frame = status.frames;
    status.herd_test = step;
}
TSUCHI_Actor herd_before[1024];

void herd_step() {
    const TSUCHI_PikminService& flock = *host.pikmin;
    const TSUCHI_SquadService& band = *host.squad;
    SpawnCheck& c = spawn_check;
    const uint32_t waited = status.frames - c.herd_frame;
    static TSUCHI_Actor listed[1024];
    uint64_t count = 0;
    TSUCHI_PikminInfo info{};
    TSUCHI_Actor leader{};
    switch (status.herd_test) {
    case 0: { // every Pikmin described; those that follow the captain; one told to leave
        bool ok = herd_need(flock.list(self, listed, 1024, &count)) && count > 0;
        status.herd_count = uint32_t(count);
        for (uint64_t i = 0; ok && i < count; ++i) ok = flock.info(self, listed[i], &info) == TSUCHI_OK;
        if (ok && flock.info(self, listed[0], &info) == TSUCHI_OK) {
            // The type is told as an ID. The status keeps the game's number for it, as it always has.
            uint32_t type = 0;
            if (id_value(pikmin::PikminType_vocabulary, info.kind, &type) == TSUCHI_OK) status.herd_type = type;
            status.herd_leaf = info.leaf;
        }
        herd_pass(0, ok);
        ok = herd_need(band.members(self, c.captain, herd_before, 1024, &count)) && count > 0;
        status.herd_members = uint32_t(count);
        for (uint64_t i = 0; ok && i < count; ++i) ok = band.leader(self, herd_before[i], &leader) == TSUCHI_OK && leader.id == c.captain.id;
        herd_pass(1, ok && band.members(self, herd_before[0], listed, 1024, &count) == TSUCHI_NOT_FOUND);
        c.herd_one = herd_before[0];
        herd_pass(2, herd_need(band.leave(self, c.herd_one)));
        herd_next(1);
        break;
    }
    case 1: // it follows nobody; told to join, it follows the captain at once; three new ones are asked for
        if (!(band.leader(self, c.herd_one, &leader) == TSUCHI_OK && !leader.id) && waited < 60) break;
        status.herd_leave_frames = waited;
        herd_pass(3, band.leader(self, c.herd_one, &leader) == TSUCHI_OK && !leader.id);
        herd_pass(4, herd_need(band.join(self, c.herd_one, c.captain)) && band.leader(self, c.herd_one, &leader) == TSUCHI_OK && leader.id == c.captain.id);
        if (flock.list(self, listed, 1024, &count) == TSUCHI_OK) c.listed_pikmin = count;
        if (band.members(self, c.captain, herd_before, 1024, &count) == TSUCHI_OK) c.led = count;
        herd_pass(5, herd_need(flock.add(self, c.captain, TSUCHI_PIKMIN_TYPE_GLOW, TSUCHI_LEAF_FLOWER, 3)));
        herd_next(2);
        break;
    case 2: { // ten frames on there are three more, following the captain, flowered and counted; the squad is dismissed
        if (waited < 10) break;
        if (flock.list(self, listed, 1024, &count) == TSUCHI_OK) status.herd_added = uint32_t(count - c.listed_pikmin);
        uint32_t fresh = 0;
        bool as_asked = true;
        if (band.members(self, c.captain, listed, 1024, &count) == TSUCHI_OK) {
            for (uint64_t i = 0; i < count; ++i) {
                bool old = false;
                for (uint64_t j = 0; j < c.led && !old; ++j) old = herd_before[j].id == listed[i].id;
                if (old) continue;
                as_asked = as_asked && flock.info(self, listed[i], &info) == TSUCHI_OK && !strcmp(info.kind, TSUCHI_PIKMIN_TYPE_GLOW) && info.leaf == TSUCHI_LEAF_FLOWER && info.in_army;
                if (fresh < 3) c.herd_new[fresh] = listed[i];
                ++fresh;
            }
        }
        herd_pass(6, status.herd_added == 3 && fresh == 3 && as_asked);
        // One of the new ones is sent to work on the creature the checks spawned.
        status.herd_work = c.herd_new[1].id ? flock.work_on(self, c.herd_new[1], c.made[1]) : -1000;
        herd_next(7);
        break;
    }
    case 7: { // a second later it is doing something other than following; then the squad is dismissed
        if (waited < 30) break;
        static char doing_now[4096];
        uint64_t needed = 0;
        if (host.behavior->action(self, c.herd_new[1], doing_now, sizeof doing_now, &needed) == TSUCHI_OK) copy_text(status.herd_work_doing, sizeof status.herd_work_doing, doing_now);
        status.herd_work_led = band.leader(self, c.herd_new[1], &leader) == TSUCHI_OK && leader.id == c.captain.id;
        herd_pass(12, status.herd_work == TSUCHI_OK && status.herd_work_doing[0] && strcmp(status.herd_work_doing, "Follow") &&
                          flock.work_on(self, c.captain, c.made[1]) == TSUCHI_NOT_FOUND);
        TSUCHI_Life life{};
        if (host.combat->life(self, c.dog, &life) == TSUCHI_OK) status.herd_dog_life = life.life;
        if (host.behavior->action(self, c.dog, doing_now, sizeof doing_now, &needed) == TSUCHI_OK) copy_text(status.herd_dog_state, sizeof status.herd_dog_state, doing_now);
        herd_pass(7, herd_need(band.dismiss(self, c.captain)));
        herd_next(3);
        break;
    }
    case 3: { // nobody follows the captain, within ten seconds; one is told to follow the dog
        if (waited < 45) break;
        if (band.members(self, c.captain, listed, 1024, &count) != TSUCHI_OK) count = 1024;
        if (waited == 45) { // how it stands a second and a half on, and what a straggler is doing
            status.herd_left = uint32_t(count);
            if (count && count < 1024) {
                static char doing_now[4096];
                uint64_t needed = 0;
                status.herd_straggler = listed[0].id == c.herd_one.id ? 1 : 3;
                for (const TSUCHI_Actor& fresh : c.herd_new)
                    if (fresh.id == listed[0].id) status.herd_straggler = 2;
                if (host.behavior->action(self, listed[0], doing_now, sizeof doing_now, &needed) == TSUCHI_OK) copy_text(status.herd_doing, sizeof status.herd_doing, doing_now);
            }
        }
        if (count && waited == 150) { // still followed: asked once more
            status.herd_redismissed = 1;
            band.dismiss(self, c.captain);
        }
        if (count && waited < 300) break;
        status.herd_left_frames = waited;
        herd_pass(8, count == 0);
        status.herd_dog_idle_join = band.join(self, c.herd_one, c.dog); // the dog only follows its captain: recorded, whichever way it goes
        herd_next(4);
        break;
    }
    case 4: // what came of that; then the player steers the dog
        if (waited < 10) break;
        status.herd_dog_idle_led = band.leader(self, c.herd_one, &leader) == TSUCHI_OK && leader.id == c.dog.id;
        host.player->set_character(self, 0, c.dog, false);
        herd_next(8);
        break;
    case 8: { // the dog leads now: a Pikmin told to join it
        TSUCHI_Actor steered{};
        if (!(host.player->character(self, 0, &steered) == TSUCHI_OK && steered.id == c.dog.id) && waited < 150) break;
        status.herd_dog_join = steered.id == c.dog.id ? band.join(self, c.herd_one, c.dog) : -1000;
        herd_next(9);
        break;
    }
    case 9: // follows it; the player goes back to the captain
        if (waited < 10) break;
        status.herd_dog_led = band.leader(self, c.herd_one, &leader) == TSUCHI_OK && leader.id == c.dog.id;
        {
            static char doing_now[4096];
            uint64_t needed = 0;
            if (host.behavior->action(self, c.herd_one, doing_now, sizeof doing_now, &needed) == TSUCHI_OK) copy_text(status.herd_dog_doing, sizeof status.herd_dog_doing, doing_now);
        }
        herd_pass(9, status.herd_dog_join == TSUCHI_OK && status.herd_dog_led && band.members(self, c.dog, listed, 1024, &count) == TSUCHI_OK && count >= 1);
        host.player->set_character(self, 0, c.captain, false);
        herd_next(10);
        break;
    case 10: { // one of the new ones is killed, drowned, by nobody
        TSUCHI_Actor steered{};
        if (!(host.player->character(self, 0, &steered) == TSUCHI_OK && steered.id == c.captain.id) && waited < 150) break;
        herd_victim = c.herd_new[0];
        herd_pass(10, herd_victim.id && herd_need(host.combat->kill_pikmin(self, herd_victim, TSUCHI_DEATH_CAUSE_DROWNED, TSUCHI_Actor{})));
        herd_next(5);
        break;
    }
    case 5: // told as a death of that cause; it cannot be killed twice; the other two are taken away quietly
        if (herd_death.pikmin.id != herd_victim.id && waited < 90) break;
        status.herd_frames = waited;
        status.herd_died_cause = uint32_t(herd_death.cause);
        herd_pass(11, herd_victim.id && herd_death.pikmin.id == herd_victim.id && herd_death.cause == TSUCHI_DEATH_CAUSE_DROWNED && herd_death.attacker.id == 0 &&
                          host.combat->kill_pikmin(self, herd_victim, TSUCHI_DEATH_CAUSE_DROWNED, TSUCHI_Actor{}) == TSUCHI_NOT_FOUND);
        for (int i = 1; i < 3; ++i)
            if (c.herd_new[i].id) host.entity->destroy(self, c.herd_new[i]);
        c.moved_frame = status.frames; // the next check waits a little after this
        herd_next(6);
        break;
    }
}

// ---- The check of the creatures service ------------------------------------------------

bool wild_need(TSUCHI_Result result) {
    if (result != TSUCHI_OK && !status.wild_result) status.wild_result = result;
    return result == TSUCHI_OK;
}
void wild_pass(uint32_t step, bool ok) { status.wild_passed |= uint32_t(ok) << step; }
void wild_next(uint32_t step) {
    spawn_check.wild_frame = status.frames;
    status.wild_test = step;
}

void wild_step() {
    const TSUCHI_CreatureService& api = *host.creature;
    SpawnCheck& c = spawn_check;
    const uint32_t waited = status.frames - c.wild_frame;
    static TSUCHI_Actor listed[2048];
    uint64_t count = 0, needed = 0;
    bool down = false;
    switch (status.wild_test) {
    case 0: { // none defeated yet; the one the checks spawned is among them, and the captain, the dog and a Pikmin are not
        bool ok = wild_need(api.list(self, listed, 2048, &count)) && count > 0, among = false;
        status.wild_count = uint32_t(count);
        char wanted[64] = "", kind[64] = "";
        host.entity->kind(self, c.made[1], wanted, sizeof wanted, &needed);
        for (uint64_t i = 0; ok && i < count; ++i) {
            ok = api.defeated(self, listed[i], &down) == TSUCHI_OK;
            status.wild_defeated += down;
            among = among || listed[i].id == c.made[1].id;
            if (listed[i].id == c.made[1].id || down) continue;
            // The one to hit: another of the spawned one's kind if there is one, else any.
            const bool same = host.entity->kind(self, listed[i], kind, sizeof kind, &needed) == TSUCHI_OK && !strcmp(kind, wanted);
            if (!c.prey.id || (same && strcmp(status.wild_kind, wanted))) c.prey = listed[i], copy_text(status.wild_kind, sizeof status.wild_kind, kind);
        }
        wild_pass(0, ok && among);
        wild_pass(1, api.defeated(self, c.captain, &down) == TSUCHI_NOT_FOUND && api.defeated(self, c.dog, &down) == TSUCHI_NOT_FOUND &&
                         api.defeated(self, c.herd_one, &down) == TSUCHI_NOT_FOUND);
        wild_pass(2, api.defeated(self, c.made[1], &down) == TSUCHI_OK && !down);
        // A blow far beyond its life, in the captain's name.
        wild_pass(3, c.prey.id && wild_need(host.combat->damage(self, c.prey, 100000, TSUCHI_ATTACK_KIND_NORMAL, c.captain)));
        wild_next(c.prey.id ? 1 : 3);
        break;
    }
    case 1: // it reads as defeated
        if (!(api.defeated(self, c.prey, &down) == TSUCHI_OK && down) && waited < 300) break;
        status.wild_frames = waited;
        wild_pass(4, api.defeated(self, c.prey, &down) == TSUCHI_OK && down);
        wild_next(2);
        break;
    case 2: // two seconds on: what is listed, and whether it still is
        if (waited < 60) break;
        if (api.list(self, listed, 2048, &count) != TSUCHI_OK) count = 0;
        status.wild_after = uint32_t(count);
        for (uint64_t i = 0; i < count; ++i) status.wild_still |= listed[i].id == c.prey.id;
        c.moved_frame = status.frames; // the next check waits a little after this
        wild_next(3);
        break;
    }
}

// ---- The check of mods written in Lua ---------------------------------------------------

// What mods write to the log. The checks' own script mod says what it found by what it prints, one thing to a line.
void heard_log(const char* mod_id, const char* text) {
    log_line(mod_id, text); // where every line goes in the build people run
    if (!strcmp(mod_id, "check.compiled-refuses") && !strcmp(text, "refuser ran")) status.compiled_passed |= 1u << 4;
    if (!strcmp(mod_id, "check.compiled")) {
        copy_text(status.compiled_line, sizeof status.compiled_line, text);
        const char* const whole[] = {"compiled started check.compiled 1.0.0 with 1.4.2", "compiled memory 42 zeros", "compiled frames 30"};
        for (uint32_t i = 0; i < 3; ++i)
            if (!strcmp(text, whole[i])) status.compiled_passed |= 1u << i;
        if (strlen(text) > 16 && !memcmp(text, "compiled actors ", 16)) status.compiled_passed |= 1u << 3; // once a level is up
    }
    if (strcmp(mod_id, "check.script")) return;
    ++status.script_lines;
    copy_text(status.script_line, sizeof status.script_line, text);
    const char* const whole[] = {"started check.script 1.0.0 with 1.4.2", "helper 42", "language same", "caught this", "frames 30"};
    for (uint32_t i = 0; i < 5; ++i)
        if (!strcmp(text, whole[i])) status.script_passed |= 1u << i;
    if (strlen(text) > 7 && !memcmp(text, "actors ", 7)) status.script_passed |= 1u << 5; // once a level is up
    if (strlen(text) > 8 && !memcmp(text, "differs ", 8)) copy_text(status.script_note, sizeof status.script_note, text);
}

// ---- The check of the mods that are installed, and of a mod's own files -------------------

void mods_pass(uint32_t step, bool ok) { status.mods_passed |= uint32_t(ok) << step; }

// The checks' own mods are found in the mods folder, read, put in order and started; then two of them are spoken
// for through the services, each with its own context.
void mods_check() {
    const TSUCHI_ModService& about = *host.mod;
    const TSUCHI_FileService& folder = *host.file;
    // Use the same snapshot as production; repeated frames do not accumulate counts.
    observe_status();
    const ModRecord* library = find_mod("check.library"), *app = find_mod("check.app"), *broken = find_mod("check.broken");
    const ModRecord* lonely = find_mod("check.lonely"), *bare = find_mod("check.bare");
    mods_pass(0, status.mods_result == TSUCHI_OK && library && app && broken && lonely && bare && status.mods_running + status.mods_blocked == status.mods_found);
    // The one that is its manifest alone: what it declares of the game's data was done as it started.
    const ModRecord* declared = find_mod("check.declared");
    TSUCHI_Object sheet{};
    TSUCHI_Value life{};
    static char said[64];
    uint64_t said_bytes = 0;
    const bool as_declared = declared && declared->state == ModState::running &&
                             host.asset->load(self, "/Game/Carrot4/Core/GActor/DT_TekiParameter.DT_TekiParameter", &sheet) == TSUCHI_OK &&
                             host.datatable->get(self, sheet, "KOCHAPPY", "MaxLife", &life) == TSUCHI_OK && life.as.number == 77 &&
                             host.text->get(self, "GenseiName", "TsuchiDeclared", said, sizeof said, &said_bytes) == TSUCHI_OK && !strcmp(said, "Declared by a manifest");
    if (sheet.id) host.asset->release(self, sheet);
    if (declared && declared->state == ModState::blocked) copy_text(status.script_note, sizeof status.script_note, declared->problem.get());
    mods_pass(7, as_declared);
    // The two that are scripts: one ran, and has printed what it prints as it starts; the other failed at its third line.
    const ModRecord* script = find_mod("check.script"), *failing = find_mod("check.script-broken");
    if (failing) copy_text(status.script_problem, sizeof status.script_problem, failing->problem.get());
    mods_pass(6, script && failing && script->state == ModState::running && failing->state == ModState::blocked && !failing->context &&
                     !strcmp(failing->problem.get(), "main.lua: check.script-broken/main.lua:3: on purpose") && (status.script_passed & 0xF) == 0xF);
    // The three that are compiled: one was loaded and started; one's start refused; one's file the system's loader
    // would not take, with the reason it gave kept.
    const ModRecord* compiled = find_mod("check.compiled"), *refuser = find_mod("check.compiled-refuses"), *rejected = find_mod("check.compiled-rejected");
    status.compiled_loaded = compiled_counts().loaded, status.compiled_started = compiled_counts().started, status.compiled_refused = compiled_counts().refused;
    if (refuser) copy_text(status.compiled_problem, sizeof status.compiled_problem, refuser->problem.get());
    if (rejected && strcmp(rejected->problem.get(), "mod.nro: the system's loader would not take it")) copy_text(status.compiled_problem, sizeof status.compiled_problem, rejected->problem.get());
    if (compiled && compiled->state != ModState::running) copy_text(status.compiled_problem, sizeof status.compiled_problem, compiled->problem.get());
    status.compiled_passed |= uint32_t(compiled && refuser && rejected && compiled->state == ModState::running && refuser->state == ModState::blocked && !refuser->context &&
                                       !strcmp(refuser->problem.get(), "mod.nro: its start answered not_ready") && rejected->state == ModState::blocked &&
                                       !strcmp(rejected->problem.get(), "mod.nro: the system's loader would not take it") && status.compiled_refused != 0 &&
                                       status.compiled_loaded >= 2 && status.compiled_started >= 2 && (status.compiled_passed & 0x13) == 0x13) // these two, and the check mods that are compiled
                              << 5;
    status.mods_test = 1;
    if (!library || !app || !broken || !lonely || !bare) return;
    mods_pass(1, library->state == ModState::running && app->state == ModState::running && app->order > library->order && mod_usable(library->context) &&
                     mod_usable(app->context) && !strcmp(library->name.get(), "Check library") && library->parts[0] == 1 && library->parts[1] == 4 && library->parts[2] == 2);
    copy_text(status.mods_problem, sizeof status.mods_problem, broken->problem.get());
    mods_pass(2, broken->state == ModState::blocked && strlen(broken->problem.get()) > 28 && !memcmp(broken->problem.get(), "mod.json, line 4, column 1: ", 28) && lonely->state == ModState::blocked &&
                     !strcmp(lonely->problem.get(), "it needs check.absent, which is not installed") && bare->state == ModState::blocked &&
                     !strcmp(bare->problem.get(), "it has no mod.json") && !broken->context && !lonely->context);
    if (library->state != ModState::running || app->state != ModState::running) return;
    TSUCHI_Mod* const mine = app->context;
    TSUCHI_Mod* const theirs = library->context;
    char text[64], other[64];
    uint64_t needed = 0, size = 0;
    const uint32_t lines = log_lines();
    uint32_t running = ~0u;
    mods_pass(3, about.id(mine, text, sizeof text, &needed) == TSUCHI_OK && !strcmp(text, "check.app") && about.name(mine, text, sizeof text, &needed) == TSUCHI_OK &&
                     !strcmp(text, "Check app") && about.version(mine, text, sizeof text, &needed) == TSUCHI_OK && !strcmp(text, "2.0.0") &&
                     about.version_of(mine, "check.library", other, sizeof other, &needed) == TSUCHI_OK && !strcmp(other, "1.4.2") &&
                     about.version_of(mine, "check.lonely", other, sizeof other, &needed) == TSUCHI_NOT_FOUND && about.name(self, text, sizeof text, &needed) == TSUCHI_NOT_FOUND &&
                     about.log(mine, "the check of mods ran") == TSUCHI_OK && log_lines() == lines + 1 &&
                     about.framework(mine, &running) == TSUCHI_OK && running == TSUCHI_API_VERSION && running == host.api_version);
    status.mods_log_lines = log_lines();
    // The library's own files: a text, and 256 bytes that count up from 0.
    uint8_t bytes[16] = {};
    bool there = false, missing = true, right = folder.size(theirs, "data/hello.txt", &size) == TSUCHI_OK && size == 26;
    right = right && folder.read(theirs, "data/hello.txt", 0, text, 5, &size) == TSUCHI_OK && size == 5 && !memcmp(text, "hello", 5);
    right = right && folder.size(theirs, "data/deep/numbers.bin", &size) == TSUCHI_OK && size == 256;
    right = right && folder.read(theirs, "data/deep/numbers.bin", 100, bytes, 8, &size) == TSUCHI_OK && size == 8 && bytes[0] == 100 && bytes[7] == 107;
    right = right && folder.read(theirs, "data/deep/numbers.bin", 250, bytes, 16, &size) == TSUCHI_OK && size == 6 && bytes[5] == 255;
    right = right && folder.exists(theirs, "mod.json", &there) == TSUCHI_OK && there && folder.exists(theirs, "data/nothing.txt", &missing) == TSUCHI_OK && !missing;
    mods_pass(4, right);
    // What its folder holds, and no way out of it: not for it, and not into it for another.
    char listed[128];
    bool root_right = folder.list(theirs, "", listed, sizeof listed, &needed) == TSUCHI_OK;
    if (root_right) {
        bool manifest = false, data = false;
        uint32_t count = 0;
        size_t at = 0;
        for (const char* name = listed; *name; name += strlen(name) + 1, ++count) {
            manifest = manifest || !strcmp(name, "mod.json"), data = data || !strcmp(name, "data/");
            const size_t length = strlen(name);
            if (at + length + 2 < sizeof status.mods_listed) memcpy(status.mods_listed + at, name, length), status.mods_listed[at + length] = ' ', at += length + 1;
        }
        root_right = manifest && data && count == 2;
    }
    mods_pass(5, root_right && folder.list(theirs, "data/deep", listed, sizeof listed, &needed) == TSUCHI_OK && !strcmp(listed, "numbers.bin") &&
                     folder.list(theirs, "nowhere", listed, sizeof listed, &needed) == TSUCHI_NOT_FOUND &&
                     folder.size(theirs, "../check.app/mod.json", &size) == TSUCHI_INVALID && folder.size(mine, "data/hello.txt", &size) == TSUCHI_NOT_FOUND &&
                     folder.size(mine, "mod.json", &size) == TSUCHI_OK && size > 0);
}

// ---- The check of a slot's files, and of a stand-in --------------------------------------

void files_pass(uint32_t step, bool ok) { status.files_passed |= uint32_t(ok) << step; }

struct FilesCheck {
    uint32_t owned_before, held_before;
} files_check;

// At the title screen: the files of the slot in use are listed and read whole, and given back as a stand-in for
// the same slot. The game then loads the level from the stand-in when the level is asked for.
void files_at_title() {
    const TSUCHI_SaveService& api = *host.save;
    ++status.files_tries;
    char small[1024];
    uint64_t needed = 0, none = 0;
    TSUCHI_Result result = api.slot(self, &status.files_slot);
    const uint32_t slot = status.files_slot;
    if (result == TSUCHI_OK) result = api.list(self, slot, small, sizeof small, &needed);
    char* paths = small;
    if (result == TSUCHI_CAPACITY) { // more than the first buffer holds: one of the size it asked for
        paths = static_cast<char*>(allocate(needed));
        result = paths ? api.list(self, slot, paths, needed, &needed) : TSUCHI_NO_MEMORY;
    }
    status.files_result = result;
    if (result == TSUCHI_OK) {
        files_check.owned_before = owned_count(self);
        bool whole = true, given = true, parts = true;
        for (const char* path = paths; *path; path += strlen(path) + 1) {
            uint64_t size = 0, got = 0;
            if (!status.files_count) copy_text(status.files_first, sizeof status.files_first, path);
            ++status.files_count;
            if (api.size(self, slot, path, &size) != TSUCHI_OK) {
                whole = false;
                continue;
            }
            auto* bytes = static_cast<uint8_t*>(allocate(size ? size : 1));
            if (!bytes) {
                whole = false;
                continue;
            }
            whole = whole && api.read(self, slot, path, 0, bytes, size, &got) == TSUCHI_OK && got == size;
            status.files_bytes += uint32_t(got);
            if (size > status.files_largest) status.files_largest = uint32_t(size);
            if (size >= 8) { // a part from within is the same bytes; at the end there are fewer, and past it none
                uint8_t part[4];
                parts = parts && api.read(self, slot, path, size - 6, part, 4, &got) == TSUCHI_OK && got == 4 && !memcmp(part, bytes + size - 6, 4);
                parts = parts && api.read(self, slot, path, size - 2, part, 4, &got) == TSUCHI_OK && got == 2 && !memcmp(part, bytes + size - 2, 2);
                parts = parts && api.read(self, slot, path, size, part, 4, &got) == TSUCHI_OK && got == 0;
            }
            given = given && api.add_stand_in_file(self, slot, path, bytes, size) == TSUCHI_OK;
            release(bytes);
        }
        files_pass(0, status.files_count >= 1);
        files_pass(1, whole && status.files_bytes > 0);
        files_pass(2, parts && api.size(self, slot, "tsuchi/no-such-file", &none) == TSUCHI_NOT_FOUND && api.size(self, slot, "../config.sav", &none) == TSUCHI_INVALID &&
                          api.list(self, 99, small, sizeof small, &none) == TSUCHI_INVALID);
        files_pass(3, given && api.use_stand_in(self) == TSUCHI_OK);
        files_check.held_before = save_counts().held_back;
    }
    if (paths != small) release(paths);
    if (result != TSUCHI_BUSY) status.files_test = result == TSUCHI_OK ? 1 : 9; // busy: tried again in a moment
}

// With the level up: the game has read the slot from the stand-in, which is ended.
void files_in_level() {
    const SaveCounts counts = save_counts();
    status.files_reads = counts.stand_in_reads;
    status.files_held = counts.held_back - files_check.held_before;
    files_pass(4, status.files_reads >= 1);
    files_pass(5, host.save->end_stand_in(self) == TSUCHI_OK && owned_count(self) == files_check.owned_before && host.save->end_stand_in(self) == TSUCHI_NOT_FOUND);
    status.files_test = 2;
}

// ---- The check of the dialogs service ---------------------------------------------------

void talk_pass(uint32_t step, bool ok) { status.talk_passed |= uint32_t(ok) << step; }

struct TalkCheck {
    TSUCHI_Dialog asked[5];
    bool answered[5], accepted[5];
    uint32_t frame;
    TSUCHI_Subscription listening;
} talk_check;

// What is asked, in order, and the answer whoever runs the check gives each.
constexpr struct {
    bool question;
    const char* text;
    bool accept;
} kTalks[5] = {
    {false, "Tsuchi message check", true},
    {true, "Tsuchi question check: accept this one", true},
    {true, "Tsuchi question check: back out of this one", false},
    {true, "Tsuchi first of two asked for at once", true},
    {false, "Tsuchi second of two asked for at once", true},
};

void heard_answer(void*, const TSUCHI_DialogAnswered* event) {
    ++status.talk_heard;
    for (uint32_t i = 0; i < 5; ++i) {
        if (talk_check.asked[i].id != event->dialog.id || talk_check.answered[i]) continue;
        talk_check.answered[i] = true, talk_check.accepted[i] = event->accepted;
        if (event->accepted) status.talk_accepted |= 1u << i;
    }
}

bool talk_ask(uint32_t which) {
    const TSUCHI_Result result = kTalks[which].question ? host.dialog->confirm(self, kTalks[which].text, &talk_check.asked[which])
                                                     : host.dialog->message(self, kTalks[which].text, &talk_check.asked[which]);
    if (result != TSUCHI_OK && status.talk_result == 0) status.talk_result = result;
    return result == TSUCHI_OK;
}

// The module only asks and listens: the buttons are pressed by whoever runs the check, which sees in
// `talk_test` what is on the screen.
void talk_step() {
    TalkCheck& k = talk_check;
    SpawnCheck& c = spawn_check;
    const uint32_t waited = status.frames - k.frame;
    switch (status.talk_test) {
    case 0: {
        TSUCHI_Dialog none{};
        const bool listening = host.dialog->on_answered(self, heard_answer, nullptr, &k.listening) == TSUCHI_OK;
        talk_pass(0, listening && talk_ask(0) && host.dialog->confirm(self, "\xff", &none) == TSUCHI_INVALID && dialogs_waiting() == 1);
        k.frame = status.frames;
        status.talk_test = 1;
        break;
    }
    case 1:
    case 2:
    case 3:
    case 4:
    case 5: {
        const uint32_t which = status.talk_test - 1;
        if (!k.answered[which] && waited < 1800) break;
        status.talk_frames[which] = waited;
        talk_pass(status.talk_test, k.answered[which] && k.accepted[which] == kTalks[which].accept);
        // The next is asked for when the one before is answered, but for the last two, which are asked for together.
        if (which < 2) talk_ask(which + 1);
        if (which == 2) talk_pass(7, talk_ask(3) && talk_ask(4) && dialogs_waiting() == 2 && k.asked[4].id != k.asked[3].id);
        k.frame = status.frames;
        ++status.talk_test;
        break;
    }
    case 6: // nothing is kept of any of them
        if (waited < 60) break;
        status.talk_waiting = dialogs_waiting();
        talk_pass(6, status.talk_waiting == 0 && status.talk_heard == 5 && status.talk_result == 0 && host.cancel(self, k.listening) == TSUCHI_OK);
        c.moved_frame = status.frames + 60; // the next check waits until the last dialog has gone from the screen
        status.talk_test = 7;
        break;
    }
}

// ---- The check of the hud and audio services -------------------------------------------

void hud_pass(uint32_t step, bool ok) { status.hud_passed |= uint32_t(ok) << step; }

// The banner's clock, read past the service: where the game keeps how long its announcements stay.
uint64_t banner_clock_now() {
    namespace g = game::table;
    const uint64_t framework = game::game_framework();
    const uint64_t ui = framework ? game::read_or<uint64_t>(framework + g::game_framework::ui) : 0;
    const uint64_t announcer = ui ? game::read_or<uint64_t>(ui + g::ui_manager::announcer) : 0;
    const uint64_t widget = announcer ? game::read_or<uint64_t>(announcer + g::announcer::banner) : 0;
    return widget ? game::read_or<uint64_t>(widget + g::banner_widget::clock) : 0;
}

void hud_step() {
    namespace g = game::table;
    SpawnCheck& c = spawn_check;
    const uint32_t waited = status.frames - c.hud_frame;
    switch (status.hud_test) {
    case 0: // a banner for six seconds, and a sound of the captain's
        if (const uint64_t clock = banner_clock_now()) status.hud_wait_before = game::read_or<float>(clock + g::announce_widget::wait_time);
        status.hud_result = host.hud->banner(self, "Tsuchi banner check", 6);
        hud_pass(0, status.hud_result == TSUCHI_OK && host.hud->banner(self, "Tsuchi banner check", 0) == TSUCHI_INVALID);
        status.audio_play = host.audio->play_on(self, c.captain, "Whistle_Charge");
        status.audio_missing = host.audio->play_on(self, c.captain, "TsuchiNoSuchSound");
        hud_pass(1, status.audio_play == TSUCHI_OK && status.audio_missing == TSUCHI_NOT_FOUND && host.audio->play_on(self, c.captain, "") == TSUCHI_INVALID);
        c.hud_frame = status.frames;
        status.hud_test = 1;
        break;
    case 1: { // up a second later; the game's own time for its announcements is back once it has gone
        const uint64_t clock = banner_clock_now();
        if (waited == 30 && clock) {
            status.hud_state = uint32_t(game::read_or<int32_t>(clock + g::announce_widget::state));
            hud_pass(2, status.hud_state == 1 || status.hud_state == 2);
        }
        if (waited < 300) break;
        if (clock) status.hud_wait_after = game::read_or<float>(clock + g::announce_widget::wait_time);
        hud_pass(3, clock && status.hud_wait_after != 6 && (status.hud_wait_before == 0 || status.hud_wait_after == status.hud_wait_before) &&
                        game::read_or<int32_t>(clock + g::announce_widget::state) == 0);
        c.moved_frame = status.frames; // the next check waits a little after this
        status.hud_test = 2;
        break;
    }
    }
}

// ---- The check of the story service ----------------------------------------------------

bool story_need(TSUCHI_Result result);
void story_pass(uint32_t step, bool ok);

// The table of treasures, found among the loaded tables: rows that are worth Sparklium and have no life.
game::ClassRef data_table_class{"DataTable"};
struct TreasureSearch {
    TSUCHI_Object table{};
    char row[64] = "";
};
bool look_for_treasures(void* context, uint64_t object) {
    namespace g = game::table;
    auto& search = *static_cast<TreasureSearch*>(context);
    if (!game::is_a(object, data_table_class)) return true;
    ++status.story_treasure_tables;
    const uint64_t rows = game::read_or<uint64_t>(object + g::data_table::rows);
    const TSUCHI_Object table{game::to_handle(object)};
    for (uint32_t slot = 0; rows && slot < 8; ++slot) {
        TSUCHI_Value worth{}, life{};
        uint32_t count = 0;
        const game::Name name = game::read_or<game::Name>(rows + slot * g::table_row_entry::size + g::table_row_entry::name);
        if (!game::name_text(name, search.row, sizeof search.row)) continue;
        if (host.datatable->get(self, table, search.row, "Kira", &worth) != TSUCHI_OK || worth.kind != TSUCHI_VALUE_INTEGER) break;
        if (host.datatable->get(self, table, search.row, "MaxLife", &life) == TSUCHI_OK) break; // a creature's row
        if (host.story->treasure_count(self, search.row, &count) != TSUCHI_OK) continue;     // the story knows no treasure of that name
        status.story_treasure_worth = int32_t(worth.as.integer);
        search.table = table;
        return false;
    }
    return true;
}

void treasure_check() {
    namespace g = game::table;
    TreasureSearch found;
    game::each_object(look_for_treasures, &found);
    if (!found.table.id) return;
    static char path[1024];
    uint64_t needed = 0;
    copy_text(status.story_treasure_name, sizeof status.story_treasure_name, found.row);
    if (host.reflection->path(self, found.table, path, sizeof path, &needed) == TSUCHI_OK) copy_text(status.story_treasure_table, sizeof status.story_treasure_table, path);
    const uint64_t instance = game::global<uint64_t>(g::data::game_instance);
    const uint64_t story = instance ? game::read_or<uint64_t>(instance + g::game_instance::story_save) : 0;
    const int32_t before = story ? game::read_or<int32_t>(story + g::story_save::sparklium) : 0;
    uint32_t count = 0;
    story_pass(5, story_need(host.story->treasure_count(self, found.row, &status.story_treasure_before)) &&
                      host.story->treasure_count(self, "TsuchiNoSuchTreasure", &count) == TSUCHI_NOT_FOUND);
    status.story_treasure = host.story->give_treasure(self, found.row, false);
    story_pass(6, status.story_treasure == TSUCHI_OK && story_need(host.story->treasure_count(self, found.row, &status.story_treasure_after)) &&
                      status.story_treasure_after == status.story_treasure_before + 1);
    status.story_sparklium_gain = story ? game::read_or<int32_t>(story + g::story_save::sparklium) - before : 0;
    story_pass(7, status.story_sparklium_gain == status.story_treasure_worth && host.story->give_treasure(self, "TsuchiNoSuchTreasure", false) == TSUCHI_NOT_FOUND);
}

bool story_need(TSUCHI_Result result) {
    if (result != TSUCHI_OK && !status.story_result) status.story_result = result;
    return result == TSUCHI_OK;
}
void story_pass(uint32_t step, bool ok) { status.story_passed |= uint32_t(ok) << step; }

void story_check() {
    const TSUCHI_StoryService& api = *host.story;
    SpawnCheck& c = spawn_check;
    // What the dialogs before this left the game in.
    game::paused_kinds(status.talk_paused);
    status.talk_busy = game::busy_with_saves_or_scenes();
    stage_text(status.talk_stage, sizeof status.talk_stage);
    // At night the game takes no travel, and above ground there is no cave to leave.
    status.story_travel = api.travel(self, "Area001_P", -1);
    status.story_leave = api.leave_cave(self);
    story_pass(4, status.story_travel == TSUCHI_NOT_READY && status.story_leave == TSUCHI_NOT_READY && api.travel(self, "Area1000_P", -1) == TSUCHI_NOT_FOUND &&
                      api.travel(self, "no such area", -1) == TSUCHI_INVALID);
    treasure_check();
    // The save slot in use, and a hold on saves taken and let go.
    const uint32_t owned = owned_count(self);
    status.saves_passed |= uint32_t(host.save->slot(self, &status.saves_slot) == TSUCHI_OK && status.saves_slot <= 5);
    status.saves_hold = host.save->hold(self);
    status.saves_passed |= uint32_t(status.saves_hold == TSUCHI_OK && host.save->hold(self) == TSUCHI_OK && owned_count(self) == owned + 1 &&
                                    host.save->release(self) == TSUCHI_OK && host.save->release(self) == TSUCHI_NOT_FOUND && owned_count(self) == owned)
                           << 1;
    status.saves_test = 1;
    c.moved_frame = status.frames; // the next check waits a little after this
    status.story_test = 1;
}

// ---- The retired Pikmin death check --------------------------------------------
// check.death observes its own initialized victim, cause, kind and credited actor.
// The phase marker keeps the native sequence's existing layout and ordering.
void caught_begin() {
    spawn_check.moved_frame = status.frames;
    status.caught_test = 2;
}

// ---- The check of the components service ---------------------------------------

bool parts_need(TSUCHI_Result result) {
    if (result != TSUCHI_OK && !status.parts_result) status.parts_result = result;
    return result == TSUCHI_OK;
}
void parts_pass(uint32_t step, bool ok) { status.parts_passed |= uint32_t(ok) << step; }

void parts_begin() {
    const TSUCHI_ComponentService& api = *host.component;
    const TSUCHI_ReflectionService& reflection = *host.reflection;
    SpawnCheck& c = spawn_check;
    c.owned_at_parts = owned_count(self);
    // Its parts: how many, then each of them.
    static TSUCHI_Component parts[256];
    uint64_t count = 0, again = 0;
    const TSUCHI_Result asked = api.list(self, c.made[1], nullptr, 0, &count);
    bool ok = asked == TSUCHI_CAPACITY && count > 0 && count <= 256 && parts_need(api.list(self, c.made[1], parts, 256, &again)) && again == count;
    parts_pass(0, ok);
    status.parts_count = uint32_t(count);
    // Each is the creature's own. Its mesh is the one to hide, or failing that the first with a place in the world.
    TSUCHI_Object scene{}, skeletal{};
    reflection.find_class(self, "SceneComponent", &scene), reflection.find_class(self, "SkeletalMeshComponent", &skeletal);
    bool all_owned = ok, mesh = false;
    for (uint64_t i = 0; ok && i < again; ++i) {
        TSUCHI_Actor whose{};
        all_owned = all_owned && api.owner(self, parts[i], &whose) == TSUCHI_OK && whose.id == c.made[1].id;
        const TSUCHI_Object object = tsuchi_component_as_object(parts[i]);
        bool is = false;
        if (scene.id && reflection.is_a(self, object, scene, &is) == TSUCHI_OK && is) {
            ++status.parts_scene;
            if (!c.part.id) c.part = parts[i];
        }
        if (!mesh && skeletal.id && reflection.is_a(self, object, skeletal, &is) == TSUCHI_OK && is) c.part = parts[i], mesh = true;
    }
    parts_pass(1, all_owned);
    // Found again by its name.
    uint64_t needed = 0;
    TSUCHI_Component found{};
    ok = ok && c.part.id && parts_need(reflection.name(self, tsuchi_component_as_object(c.part), status.parts_name, sizeof status.parts_name, &needed)) &&
         parts_need(api.find(self, c.made[1], status.parts_name, &found)) && found.id == c.part.id;
    parts_pass(2, ok);
    // Hidden, with what is attached below it.
    const uint64_t address = game::from_handle(c.part.id);
    const int before = address ? asked_of(address, "IsVisible") : -1;
    bool held = false;
    ok = ok && parts_need(api.hold_hidden(self, c.part, true)) && parts_need(api.hidden(self, c.part, &held)) && held;
    parts_pass(3, ok);
    const int during = address ? asked_of(address, "IsVisible") : -1;
    status.parts_visible = uint32_t(before == 1) | uint32_t(during == 1) << 1;
    status.parts_hidden = hidden_counts().parts;
    parts_pass(4, before == 1 && during == 0);
    c.parts_frame = status.frames;
    status.parts_test = 1;
}

// ---- A material held in a slot --------------------------------------------------

uint64_t object_from(TSUCHI_Object object, const char* function);

bool paint_need(TSUCHI_Result result) {
    if (result != TSUCHI_OK && !status.paint_result) status.paint_result = result;
    return result == TSUCHI_OK;
}
void paint_pass(uint32_t step, bool ok) { status.paint_passed |= uint32_t(ok) << step; }

// What the engine says a slot of a mesh is drawn with: a handle, or 0.
uint64_t engine_material(TSUCHI_Component part, int slot) {
    TSUCHI_PreparedCall call{};
    uint64_t answer = 0;
    TSUCHI_Value value{};
    if (host.reflection->begin_call(self, tsuchi_component_as_object(part), "GetMaterial", &call) == TSUCHI_OK && argument(call, "ElementIndex", integer_value(slot)) &&
        host.reflection->invoke(self, call) == TSUCHI_OK && host.reflection->call_get(self, call, "ReturnValue", &value) == TSUCHI_OK && value.kind == TSUCHI_VALUE_OBJECT)
        answer = value.as.object.id;
    end(call);
    return answer;
}

// The first material on another actor's mesh that is not `own`.
TSUCHI_Object material_elsewhere(TSUCHI_Actor not_of, TSUCHI_Object own) {
    static TSUCHI_Actor actors[1024];
    static TSUCHI_Component parts[256];
    TSUCHI_Object meshes{}, found{};
    uint64_t count = 0;
    if (host.reflection->find_class(self, "SkeletalMeshComponent", &meshes) != TSUCHI_OK || host.entity->list(self, {}, actors, 1024, &count) != TSUCHI_OK) return found;
    for (uint64_t i = 0; i < count && !found.id; ++i) {
        uint64_t part_count = 0;
        if (actors[i].id == not_of.id || host.component->list(self, actors[i], parts, 256, &part_count) != TSUCHI_OK) continue;
        for (uint64_t j = 0; j < part_count && !found.id; ++j) {
            bool is = false;
            if (host.reflection->is_a(self, tsuchi_component_as_object(parts[j]), meshes, &is) != TSUCHI_OK || !is) continue;
            for (uint32_t slot = 0; slot < 16 && !found.id; ++slot) {
                TSUCHI_Object material{};
                const TSUCHI_Result read = host.component->material(self, parts[j], slot, &material);
                if (read == TSUCHI_INVALID) break; // past its last slot
                if (read == TSUCHI_OK && material.id != own.id) found = material;
            }
        }
    }
    return found;
}

void paint_begin() {
    const TSUCHI_ComponentService& api = *host.component;
    SpawnCheck& c = spawn_check;
    c.owned_at_paint = owned_count(self);
    const uint64_t address = game::from_handle(c.part.id);
    const int slots = address ? asked_of(address, "GetNumMaterials") : -1;
    status.paint_slots = slots > 0 ? uint32_t(slots) : 0;
    // The first slot the engine draws with something. An empty slot has nothing to read.
    status.paint_slot = c.paint_slot = 99;
    bool empties_agree = true;
    for (int slot = 0; slot < slots && c.paint_slot == 99; ++slot) {
        TSUCHI_Object nothing{};
        if (engine_material(c.part, slot)) status.paint_slot = c.paint_slot = uint32_t(slot);
        else empties_agree = empties_agree && api.material(self, c.part, uint32_t(slot), &nothing) == TSUCHI_NOT_FOUND;
    }
    uint64_t needed = 0;
    bool ok = c.paint_slot != 99 && paint_need(api.material(self, c.part, c.paint_slot, &c.own_material)) &&
              c.own_material.id == engine_material(c.part, int(c.paint_slot));
    // Names for the record: as much of each as fits.
    char name[game::kNameBytes];
    if (ok && host.reflection->name(self, c.own_material, name, sizeof name, &needed) == TSUCHI_OK) copy_text(status.paint_own, sizeof status.paint_own, name);
    TSUCHI_Object beyond{};
    paint_pass(0, ok && empties_agree && api.material(self, c.part, uint32_t(slots), &beyond) == TSUCHI_INVALID);
    c.other_material = material_elsewhere(c.made[1], c.own_material);
    ok = ok && c.other_material.id;
    if (ok && host.reflection->name(self, c.other_material, name, sizeof name, &needed) == TSUCHI_OK) copy_text(status.paint_with, sizeof status.paint_with, name);
    paint_pass(1, ok);
    ok = ok && paint_need(api.hold_material(self, c.part, c.paint_slot, c.other_material));
    paint_pass(2, ok && engine_material(c.part, int(c.paint_slot)) == c.other_material.id && material_holds() == 1 && owned_count(self) == c.owned_at_paint + 1);
    // A part that is no mesh has no slots to hold.
    TSUCHI_Component root{object_from(tsuchi_actor_as_object(c.made[1]), "K2_GetRootComponent")};
    paint_pass(5, root.id && root.id != c.part.id && api.hold_material(self, root, 0, c.other_material) == TSUCHI_UNSUPPORTED);
}

void paint_end() {
    const TSUCHI_ComponentService& api = *host.component;
    SpawnCheck& c = spawn_check;
    const int slot = int(c.paint_slot);
    // Still there after all those frames, whatever the game did meanwhile.
    paint_pass(3, c.other_material.id && engine_material(c.part, slot) == c.other_material.id && material_holds() == 1);
    const bool ok = paint_need(api.release_material(self, c.part, c.paint_slot));
    paint_pass(4, ok && engine_material(c.part, slot) == c.own_material.id && material_holds() == 0 && owned_count(self) == c.owned_at_paint);
}

void parts_release() {
    const TSUCHI_ComponentService& api = *host.component;
    SpawnCheck& c = spawn_check;
    const uint64_t address = game::from_handle(c.part.id);
    // Still hidden after all those frames, whatever the game did meanwhile.
    const int still = address ? asked_of(address, "IsVisible") : -1;
    status.parts_visible |= uint32_t(still == 1) << 2;
    status.parts_frames = status.frames - c.parts_frame;
    parts_pass(5, still == 0 && status.parts_hidden > 0 && hidden_counts().parts == status.parts_hidden);
    bool held = true;
    const bool ok = parts_need(api.release_hidden(self, c.part)) && parts_need(api.hidden(self, c.part, &held)) && !held;
    const int after = address ? asked_of(address, "IsVisible") : -1;
    status.parts_visible |= uint32_t(after == 1) << 3;
    parts_pass(6, ok && after == 1 && hidden_counts().parts == 0);
    parts_pass(7, owned_count(self) == c.owned_at_parts);
    paint_begin();
    c.parts_frame = status.frames;
    status.parts_test = 2;
}

void parts_restore() {
    paint_end();
    spawn_check.parts_frame = status.frames;
    status.parts_test = 3;
}

// ---- Parts added to the creature ------------------------------------------------

bool added_need(TSUCHI_Result result) {
    if (result != TSUCHI_OK && !status.added_result) status.added_result = result;
    return result == TSUCHI_OK;
}
void added_pass(uint32_t step, bool ok) { status.added_passed |= uint32_t(ok) << step; }

// Asks the engine for an object: what a function of `object` gives back. 0 when it gives none.
uint64_t object_from(TSUCHI_Object object, const char* function) {
    TSUCHI_PreparedCall call{};
    TSUCHI_Value value{};
    uint64_t answer = 0;
    if (host.reflection->begin_call(self, object, function, &call) == TSUCHI_OK && host.reflection->invoke(self, call) == TSUCHI_OK &&
        host.reflection->call_get(self, call, "ReturnValue", &value) == TSUCHI_OK && value.kind == TSUCHI_VALUE_OBJECT)
        answer = value.as.object.id;
    end(call);
    return answer;
}

// Where the engine says a part is, or where a bone of it is when one is named.
bool where_is(TSUCHI_Component part, const char* bone, TSUCHI_Vec3& out) {
    TSUCHI_PreparedCall call{};
    bool ok = host.reflection->begin_call(self, tsuchi_component_as_object(part), bone ? "GetSocketLocation" : "K2_GetComponentLocation", &call) == TSUCHI_OK &&
              (!bone || host.reflection->call_set_text(self, call, "InSocketName", bone) == TSUCHI_OK) && host.reflection->invoke(self, call) == TSUCHI_OK;
    ok = ok && result_number(call, "ReturnValue.X", out.x) && result_number(call, "ReturnValue.Y", out.y) && result_number(call, "ReturnValue.Z", out.z);
    end(call);
    return ok;
}

void added_check() {
    const TSUCHI_ComponentService& api = *host.component;
    const TSUCHI_ReflectionService& reflection = *host.reflection;
    SpawnCheck& c = spawn_check;
    c.owned_at_added = owned_count(self);
    uint64_t before = 0, after = 0, needed = 0;
    api.list(self, c.made[1], nullptr, 0, &before);
    // A bone of its mesh, as the engine lists them.
    TSUCHI_Object plain_class{};
    TSUCHI_PreparedCall call{};
    bool ok = c.part.id && reflection.find_class(self, "SceneComponent", &plain_class) == TSUCHI_OK &&
              reflection.begin_call(self, tsuchi_component_as_object(c.part), "GetAllSocketNames", &call) == TSUCHI_OK && reflection.invoke(self, call) == TSUCHI_OK &&
              reflection.call_get_text(self, call, "ReturnValue[1]", status.added_socket, sizeof status.added_socket, &needed) == TSUCHI_OK;
    end(call);
    added_pass(0, ok && status.added_socket[0]);
    // One under its root.
    TSUCHI_Component plain{}, none{};
    TSUCHI_Actor whose{};
    ok = ok && added_need(api.add(self, c.made[1], plain_class, none, "", &plain)) && added_need(api.owner(self, plain, &whose)) && whose.id == c.made[1].id;
    const uint64_t root_part = object_from(tsuchi_actor_as_object(c.made[1]), "K2_GetRootComponent");
    added_pass(1, ok && root_part && object_from(tsuchi_component_as_object(plain), "GetAttachParent") == root_part);
    // One at a bone of its mesh: attached there, and exactly where the bone is.
    ok = ok && added_need(api.add(self, c.made[1], plain_class, c.part, status.added_socket, &c.bone_part));
    char attached_at[32] = "";
    call = {};
    const bool named = ok && reflection.begin_call(self, tsuchi_component_as_object(c.bone_part), "GetAttachSocketName", &call) == TSUCHI_OK &&
                       reflection.invoke(self, call) == TSUCHI_OK && reflection.call_get_text(self, call, "ReturnValue", attached_at, sizeof attached_at, &needed) == TSUCHI_OK;
    end(call);
    added_pass(2, named && !strcmp(attached_at, status.added_socket) && object_from(tsuchi_component_as_object(c.bone_part), "GetAttachParent") == c.part.id);
    TSUCHI_Vec3 part_at{}, bone_at{};
    status.added_miss = -1;
    if (ok && where_is(c.bone_part, nullptr, part_at) && where_is(c.part, status.added_socket, bone_at)) {
        const float dx = part_at.x - bone_at.x, dy = part_at.y - bone_at.y, dz = part_at.z - bone_at.z;
        status.added_miss = dx * dx + dy * dy + dz * dz;
    }
    added_pass(3, status.added_miss >= 0 && status.added_miss < 1);
    api.list(self, c.made[1], nullptr, 0, &after);
    status.added_listed = uint32_t(after);
    added_pass(4, ok && after == before + 2 && added_count() == 2 && owned_count(self) == c.owned_at_added + 2);
    // The first is removed again. The creature's own mesh is not the framework's to remove.
    ok = ok && added_need(api.remove(self, plain));
    api.list(self, c.made[1], nullptr, 0, &after);
    added_pass(5, ok && !game::from_handle(plain.id) && after == before + 1 && added_count() == 1);
    added_pass(6, api.remove(self, c.part) == TSUCHI_NOT_FOUND && game::from_handle(c.part.id) != 0);
}

void parts_finish() {
    added_check();
    // The creature did things of its own accord all this while, and each was what it was doing when told.
    behavior_pass(3, status.behavior_own >= 1 && status.behavior_watched > 0);
    behavior_pass(4, status.behavior_mismatch * 20 <= status.behavior_events);
    if (host.cancel(self, behavior_subscription) == TSUCHI_OK) --spawn_check.owned_before; // one thing fewer is the framework's from here
    if (host.cancel(self, died_subscription) == TSUCHI_OK) --spawn_check.owned_before;
    spawn_check.moved_frame = status.frames; // the removal waits a little after this
    status.parts_test = 4;
}

// ---- The check of a trace --------------------------------

void clip_pass(uint32_t step, bool ok) { status.clip_passed |= uint32_t(ok) << step; }
bool clip_need(TSUCHI_Result result) {
    if (result != TSUCHI_OK && !status.clip_result) status.clip_result = result;
    return result == TSUCHI_OK;
}

// A line through the captain, from above to below, traced by the engine's own function for it through `reflection`:
// it hits the ground the captain stands on, and tells what it hit through a reference the engine keeps weakly.
void trace_check() {
    const TSUCHI_ReflectionService& reflection = *host.reflection;
    const TSUCHI_Vec3 at = place_of(spawn_check.captain);
    status.trace_from = at.z;
    TSUCHI_Object system{};
    TSUCHI_PreparedCall call{};
    TSUCHI_Value context{}, value{};
    context.kind = TSUCHI_VALUE_OBJECT;
    context.as.object = tsuchi_actor_as_object(spawn_check.captain);
    bool ok = clip_need(reflection.find_class(self, "KismetSystemLibrary", &system)) && clip_need(reflection.begin_call(self, system, "LineTraceSingle", &call));
    ok = ok && clip_need(reflection.call_set(self, call, "WorldContextObject", &context));
    const struct {
        const char* path;
        TSUCHI_Value value;
    } arguments[] = {
        {"Start.X", number_value(at.x)}, {"Start.Y", number_value(at.y)}, {"Start.Z", number_value(at.z + 300)},
        {"End.X", number_value(at.x)},   {"End.Y", number_value(at.y)},   {"End.Z", number_value(at.z - 1000)},
        {"TraceChannel", integer_value(0)}, {"bTraceComplex", boolean_value(false)}, {"bIgnoreSelf", boolean_value(true)},
    };
    for (const auto& argument : arguments) ok = ok && clip_need(reflection.call_set(self, call, argument.path, &argument.value));
    ok = ok && clip_need(reflection.invoke(self, call));
    if (ok && reflection.call_get(self, call, "ReturnValue", &value) == TSUCHI_OK && value.kind == TSUCHI_VALUE_BOOLEAN) status.trace_hit = value.as.boolean;
    if (ok && reflection.call_get(self, call, "OutHit.Location.Z", &value) == TSUCHI_OK && value.kind == TSUCHI_VALUE_NUMBER) status.trace_z = float(value.as.number);
    if (ok && reflection.call_get(self, call, "OutHit.Distance", &value) == TSUCHI_OK && value.kind == TSUCHI_VALUE_NUMBER) status.trace_distance = float(value.as.number);
    bool named = false;
    uint64_t needed = 0;
    if (ok && reflection.call_get(self, call, "OutHit.Actor", &value) == TSUCHI_OK && value.kind == TSUCHI_VALUE_OBJECT)
        named = reflection.name(self, value.as.object, status.trace_actor, sizeof status.trace_actor, &needed) == TSUCHI_OK;
    end(call);
    // Something under the captain, between where the line began and a little below the captain's middle.
    clip_pass(6, ok && status.trace_hit && status.trace_z < at.z + 300 && status.trace_z > at.z - 200 && near(status.trace_distance, at.z + 300 - status.trace_z, 1.0f));
    clip_pass(7, named && status.trace_actor[0]);
}

// ---- A level by day: travel ------------------------------------------------------------

// True when the level has a clock and the clock says night: the level the other checks are written for.
bool level_is_night() {
    namespace g = game::table;
    const uint64_t framework = game::game_framework();
    const uint64_t clock = framework ? game::read_or<uint64_t>(framework + g::game_framework::clock) : 0;
    return clock && game::read_or<uint8_t>(clock + g::clock::night);
}

void day_pass(uint32_t step, bool ok) { status.day_passed |= uint32_t(ok) << step; }

// How many creatures of a kind are in the level and not defeated.
uint32_t undefeated_of(const char* wanted) {
    static TSUCHI_Actor listed[2048];
    uint64_t count = 0, needed = 0;
    uint32_t found = 0;
    if (host.creature->list(self, listed, 2048, &count) != TSUCHI_OK) return 0;
    for (uint64_t i = 0; i < count; ++i) {
        char kind[64] = "";
        bool down = true;
        if (host.entity->kind(self, listed[i], kind, sizeof kind, &needed) == TSUCHI_OK && !strcmp(kind, wanted) && host.creature->defeated(self, listed[i], &down) == TSUCHI_OK && !down)
            ++found;
    }
    return found;
}

void day_step() {
    namespace g = game::table;
    SpawnCheck& c = spawn_check;
    const uint32_t waited = status.frames - c.day_frame;
    static char world[256];
    uint64_t needed = 0;
    switch (status.day_test) {
    case 1: { // ask, every second, until the game takes it or a minute has passed
        if (waited % 30) break;
        if (!status.day_tries) {
            static char stages[1024];
            if (host.game->world(self, world, sizeof world, &needed) == TSUCHI_OK) copy_text(status.day_from, sizeof status.day_from, world);
            if (host.game->stage(self, stages, sizeof stages, &needed) == TSUCHI_OK) copy_text(status.day_stage, sizeof status.day_stage, stages);
            c.day_area = strcmp(status.day_from, "Area001_P") ? 1 : 2;
            day_pass(0, host.story->day(self, &status.day_day) == TSUCHI_OK && status.day_day >= 1);
            day_pass(4, host.save->hold(self) == TSUCHI_OK); // the game saves on the journey: held back from here
        }
        const char area[] = {'A', 'r', 'e', 'a', '0', '0', char('0' + c.day_area), '_', 'P', 0}; // an area is named by its world's ID
        status.day_travel = host.story->travel(self, area, -1);
        if (!status.day_tries++) status.day_first = status.day_travel;
        if (status.day_travel == TSUCHI_OK) {
            day_pass(1, host.story->travel(self, area, -1) == TSUCHI_BUSY); // asked once: a second request waits for the journey
            c.day_frame = status.frames;
            status.day_test = 2;
        } else if (waited >= 1800) {
            c.day_frame = status.frames;
            status.day_test = 4;
        }
        break;
    }
    case 2: { // the other world, begun, with the player in control
        bool control = false;
        const bool there = host.game->world(self, world, sizeof world, &needed) == TSUCHI_OK && strcmp(world, status.day_from) &&
                           host.game->has_control(self, &control) == TSUCHI_OK && control;
        if (!there && waited < 5400) break;
        status.day_frames = waited;
        copy_text(status.day_to, sizeof status.day_to, there ? world : "");
        const char wanted[] = {'A', 'r', 'e', 'a', '0', '0', char('0' + c.day_area), '_', 'P', 0};
        day_pass(2, there && !strcmp(world, wanted));
        c.day_frame = status.frames;
        status.day_test = 3;
        break;
    }
    case 3: // five seconds there: the day is the same, and the clock runs
        if (waited < 150) break;
        {
            uint32_t day = 0;
            day_pass(3, host.story->day(self, &day) == TSUCHI_OK && day == status.day_day && host.story->clock(self, &status.day_time) == TSUCHI_OK && status.day_time >= 0 &&
                            status.day_time <= 1);
        }
        {
            const SaveCounts counts = save_counts();
            status.day_saves_held = counts.held_back, status.day_saves_passed = counts.passed;
            copy_text(status.day_saves_last, sizeof status.day_saves_last, counts.last);
            day_pass(5, counts.held_back >= 1 && host.save->release(self) == TSUCHI_OK);
        }
        c.day_frame = status.frames;
        status.day_test = 6;
        break;
    case 6: { // the level's creatures near where the player landed, and one more made beside the captain
        static TSUCHI_Actor listed[2048];
        uint64_t count = 0, needed = 0;
        if (host.creature->list(self, listed, 2048, &count) != TSUCHI_OK) count = 0;
        status.day_wild_count = uint32_t(count);
        for (uint64_t i = 0; i < count; ++i) {
            const uint64_t actor = game::from_handle(listed[i].id);
            status.day_wild_dormant += actor && game::read_or<uint8_t>(actor + g::game_actor::rest) != 0;
        }
        // A Dwarf Bulborb, its class loaded by path: by day none may be in play near the landing site.
        TSUCHI_Actor captain{};
        TSUCHI_Pose at{};
        TSUCHI_Object type{};
        if (host.asset->load(self, "/Game/Carrot4/Placeables/Teki/GKochappy.GKochappy_C", &type) == TSUCHI_OK && host.player->captain(self, 0, &captain) == TSUCHI_OK &&
            host.entity->pose(self, captain, &at) == TSUCHI_OK) {
            const TSUCHI_Vec3 near_by{at.location.x + 400, at.location.y, at.location.z + 50};
            if (host.entity->spawn(self, type, &near_by, &at.rotation, -1, false, &c.day_victim) != TSUCHI_OK) c.day_victim = TSUCHI_Actor{};
        }
        if (c.day_victim.id) host.entity->kind(self, c.day_victim, status.day_wild_kind, sizeof status.day_wild_kind, &needed);
        c.day_frame = status.frames;
        status.day_test = c.day_victim.id ? 7 : 4;
        break;
    }
    case 7: { // three seconds on it is up; a blow far beyond its life, in the captain's name
        if (waited < 90) break;
        const uint64_t actor = game::from_handle(c.day_victim.id);
        TSUCHI_Actor captain{};
        status.day_wild_awake = actor && game::read_or<uint8_t>(actor + g::game_actor::rest) == 0;
        bool down = true;
        bool lying = true;
        day_pass(10, host.creature->defeated(self, c.day_victim, &down) == TSUCHI_OK && !down && host.creature->carcass(self, c.day_victim, &lying) == TSUCHI_OK &&
                         !lying); // the new one is a creature, not defeated and no carcass
        c.day_alive_before = undefeated_of(status.day_wild_kind);
        host.player->captain(self, 0, &captain);
        status.day_wild_hit = host.combat->damage(self, c.day_victim, 100000, TSUCHI_ATTACK_KIND_NORMAL, captain);
        day_pass(6, status.day_wild_awake && status.day_wild_hit == TSUCHI_OK);
        c.day_frame = status.frames;
        status.day_test = 8;
        break;
    }
    case 8: { // it reads as defeated
        bool down = false;
        if (!(host.creature->defeated(self, c.day_victim, &down) == TSUCHI_OK && down) && waited < 300) break;
        status.day_wild_defeat_frames = waited;
        day_pass(7, down);
        c.day_frame = status.frames;
        status.day_test = 9;
        break;
    }
    case 9: { // by day a defeated creature stays as a carcass: the game marks it one
        bool carcass = false;
        if (host.creature->carcass(self, c.day_victim, &carcass) != TSUCHI_OK) carcass = false;
        if (!carcass && waited < 900) break;
        status.day_wild_carcass_frames = carcass ? (waited ? waited : 1) : 0;
        static TSUCHI_Actor listed[2048];
        uint64_t count = 0;
        if (host.creature->list(self, listed, 2048, &count) != TSUCHI_OK) count = 0;
        for (uint64_t i = 0; i < count; ++i) status.day_wild_listed |= listed[i].id == c.day_victim.id;
        day_pass(8, carcass && status.day_wild_listed);
        // And it is brought back.
        status.day_wild_revive = host.creature->revive(self, c.day_victim);
        c.day_frame = status.frames;
        status.day_test = 10;
        break;
    }
    case 10: { // one more of its kind is up and about
        const bool back = undefeated_of(status.day_wild_kind) > c.day_alive_before - 1;
        if (!back && waited < 900) break;
        bool alive = false;
        status.day_wild_revive_frames = back ? (waited ? waited : 1) : 0;
        status.day_wild_old_alive = host.actor->alive(self, c.day_victim, &alive) == TSUCHI_OK && alive;
        day_pass(9, status.day_wild_revive == TSUCHI_OK && back);
        c.day_frame = status.frames;
        status.day_test = 4;
        break;
    }
    case 4: { // back to the title, asked until the game takes it
        TSUCHI_Mode mode = TSUCHI_MODE_NONE;
        if (host.game->mode(self, &mode) == TSUCHI_OK && mode == TSUCHI_MODE_TITLE) {
            status.game_test = 4; // what the run waits for
            status.day_test = 5;
        } else if (waited % 30 == 0) {
            host.game->return_to_title(self);
        }
        break;
    }
    }
}

// ---- Which checks a run wants ---------------------------------------------------------------

// A run can name the checks it wants in `rom:/tsuchi/checks.txt`, one to a line. With no such file
// every check runs. The checks made in a level come one after another, on the same creature, each
// beginning when the one before it is over. So one that is not wanted is marked as over, with no
// step of it passed, and the next begins.
char wanted_text[512];
bool wanted_read = false, wanted_some = false;

bool wanted(const char* name) {
    if (!wanted_read) {
        char* data = nullptr;
        uint64_t size = 0;
        const TSUCHI_Result read = game::read_whole_file("rom:/tsuchi/checks.txt", data, size);
        if (read == TSUCHI_NOT_READY) return true;
        wanted_read = true;
        if (read == TSUCHI_OK) {
            size_t used = 0;
            wanted_text[used++] = '\n';
            for (uint64_t i = 0; i < size && used + 2 < sizeof wanted_text; ++i)
                if (data[i] != '\r' && data[i] != ' ') wanted_text[used++] = data[i];
            wanted_text[used++] = '\n', wanted_text[used] = 0;
            wanted_some = true;
            release(data);
        }
    }
    if (!wanted_some) return true;
    const size_t length = strlen(name);
    for (const char* at = wanted_text + 1; *at; ++at) // a name begins a line and ends it
        if (at[-1] == '\n' && !memcmp(at, name, length) && at[length] == '\n') return true;
    return false;
}

bool world_effects_selected() {
    // This game-only proof runs only when the check selection names it.
    return wanted("world-effects") && wanted_some;
}

void world_effects_step() {
    if (world_effects_selected()) tsuchi::target::world_follow_check_step();
}

struct LevelCheck {
    const char* name;
    uint32_t Status::* test; // how far the check has come
    uint32_t over;           // and what that reads when it is over
};
constexpr LevelCheck kLevelChecks[] = {
    {"suspend", &Status::suspend_test, 3}, {"body", &Status::body_test, 3},       {"caught", &Status::caught_test, 2},
    {"game", &Status::game_test, 2},       {"captains", &Status::captains_test, 11}, {"herd", &Status::herd_test, 6},
    {"wild", &Status::wild_test, 3},       {"hud", &Status::hud_test, 2},         {"talk", &Status::talk_test, 7},
    {"story", &Status::story_test, 1},     {"parts", &Status::parts_test, 4},     {"clip", &Status::clip_test, 4},
};

// Marks as over each check that is next in line and not wanted, up to the first that is wanted or under way.
void skip_unwanted() {
    if (status.spawn_test != 1 || !spawn_check.verified) return;
    for (const LevelCheck& check : kLevelChecks) {
        uint32_t& test = status.*check.test;
        if (test == check.over) continue;
        if (test != 0 || wanted(check.name)) return;
        test = check.over;
    }
}

void check_spawning() {
    SpawnCheck& c = spawn_check;
    if (status.day_test) return day_step();
    if (status.objects_level_frame && status.spawn_test == 0 && status.frames >= status.objects_level_frame + 900 && !level_is_night()) {
        c.day_frame = status.frames;
        status.day_test = 1;
        return;
    }
    if (status.spawn_test == 3 && status.game_test == 2 && status.frames >= c.removal_frame + 90) return game_leave();
    if (status.game_test == 3) return game_arriving();
    if (!status.objects_level_frame || status.spawn_test == 3) return;
    skip_unwanted();
    if (status.spawn_test == 0 && status.frames >= status.objects_level_frame + 900) spawn_begin();
    else if (status.spawn_test == 1 && !c.verified && status.frames >= c.asked_frame + 45) spawn_verify();
    else if (status.spawn_test == 1 && c.verified && status.suspend_test == 0 && !c.control_frame && status.frames >= c.moved_frame + 60) {
        c.control_at = place_of(c.made[1]); // from here: what it does left to itself
        c.control_frame = status.frames;
    } else if (status.spawn_test == 1 && c.verified && status.suspend_test == 0 && c.control_frame && status.frames >= c.control_frame + 150) suspend_begin();
    else if (status.suspend_test == 1 && status.frames >= c.suspend_frame + 150) suspend_end();
    else if (status.suspend_test == 1 && (status.frames - c.suspend_frame) % 30 == 0) {
        TSUCHI_Pose at{};
        if (host.entity->pose(self, c.made[1], &at) == TSUCHI_OK) {
            suspend_note("d"), suspend_number(static_cast<long long>(at.location.x - c.suspended_at.x));
            suspend_note(","), suspend_number(static_cast<long long>(at.location.y - c.suspended_at.y));
            suspend_note(","), suspend_number(static_cast<long long>(at.location.z - c.suspended_at.z)), suspend_note(" ");
        }
    }
    else if (status.suspend_test == 2 && status.frames >= c.suspend_frame + 150) suspend_after();
    else if (status.spawn_test == 1 && status.suspend_test == 3 && status.body_test == 0 && status.frames >= c.moved_frame + 15) body_begin();
    else if (status.body_test == 1 && status.frames >= c.body_frame + 45) body_release();
    else if (status.body_test == 2 && status.frames >= c.body_frame + 90) body_finish();
    else if (status.spawn_test == 1 && status.body_test == 3 && status.caught_test == 0 && status.frames >= c.moved_frame + 15) caught_begin();
    else if (status.spawn_test == 1 && status.caught_test == 2 && status.game_test == 0 && status.frames >= c.moved_frame + 30) game_begin();
    else if (status.game_test == 1 && status.frames >= c.game_frame + 120) game_release();
    else if (status.spawn_test == 1 && status.game_test == 2 && status.captains_test == 0 && status.frames >= c.moved_frame + 30) captains_step();
    else if (status.game_test == 2 && status.captains_test > 0 && status.captains_test < 11) captains_step();
    else if (status.captains_test == 11 && status.herd_test == 0 && status.frames >= c.moved_frame + 30) herd_step();
    else if (status.captains_test == 11 && status.herd_test != 0 && status.herd_test != 6) herd_step();
    else if (status.herd_test == 6 && status.wild_test == 0 && status.frames >= c.moved_frame + 30) wild_step();
    else if (status.herd_test == 6 && status.wild_test > 0 && status.wild_test < 3) wild_step();
    else if (status.wild_test == 3 && status.hud_test == 0 && status.frames >= c.moved_frame + 30) hud_step();
    else if (status.wild_test == 3 && status.hud_test == 1) hud_step();
    else if (status.hud_test == 2 && status.talk_test == 0 && status.frames >= c.moved_frame + 30) talk_step();
    else if (status.talk_test >= 1 && status.talk_test < 7) talk_step();
    else if (status.talk_test == 7 && status.story_test == 0 && status.frames >= c.moved_frame + 30) story_check();
    else if (status.spawn_test == 1 && status.story_test == 1 && status.parts_test == 0 && status.frames >= c.moved_frame + 60) parts_begin();
    else if (status.parts_test == 1 && status.frames >= c.parts_frame + 240) parts_release();
    else if (status.parts_test == 2 && status.frames >= c.parts_frame + 240) parts_restore();
    else if (status.parts_test == 3 && status.frames >= c.parts_frame + 150) parts_finish();
    else if (status.spawn_test == 1 && status.parts_test == 4 && status.clip_test == 0 && status.frames >= c.moved_frame + 15) {
        trace_check();
        status.clip_test = 4;
        c.moved_frame = status.frames;
    }
    else if (status.spawn_test == 1 && status.clip_test == 4 && status.frames >= c.moved_frame + 15) spawn_remove();
    else if (status.spawn_test == 2 && status.frames > c.removal_frame + 2) spawn_finish();
}

// Counts the actors in play now and then. The check runs once on the title
// screen, where there is next to nothing, and once more when a level is up.
void watch_actors() {
    static uint32_t settled = 0;
    if (status.frames >= 1340 && mods_started && !status.mods_test) mods_check();
    // A little later, and again while the game is busy with its saves, until whoever runs the check goes on to a level.
    if (status.frames >= 1360 && status.frames <= 1395 && status.frames % 5 == 0 && !status.objects_level_frame && !status.files_test) files_at_title();
    if (status.objects_level_frame && status.files_test == 1) files_in_level();
    if (status.frames % 15) return;
    const ActorEventCounts& events = actor_event_counts();
    status.events_tables = events.tables, status.events_pool_sites = events.pool_sites, status.events_entered = events.entered;
    status.events_destroyed = events.destroyed, status.events_put_away = events.put_away, status.events_in_play = events.in_play;
    uint64_t count = 0;
    const TSUCHI_Result result = host.entity->list(self, {}, nullptr, 0, &count);
    status.actors_now = result == TSUCHI_OK || result == TSUCHI_CAPACITY ? uint32_t(count) : 0;
    // Asked of the engine by name, with any actor in play as the way to the world.
    static TSUCHI_Actor everyone[4096];
    uint64_t listed = 0;
    TSUCHI_Object statics{};
    const TSUCHI_Actor any = host.entity->list(self, {}, everyone, 4096, &listed) == TSUCHI_OK && listed ? everyone[0] : TSUCHI_Actor{};
    if (any.id && host.reflection->find_class(self, "GameplayStatics", &statics) == TSUCHI_OK) {
        TSUCHI_Value context{}, answer{};
        context.kind = TSUCHI_VALUE_OBJECT;
        context.as.object = tsuchi_actor_as_object(any);
        TSUCHI_PreparedCall call{};
        if (host.reflection->begin_call(self, statics, "IsGamePaused", &call) == TSUCHI_OK && host.reflection->call_set(self, call, "WorldContextObject", &context) == TSUCHI_OK &&
            host.reflection->invoke(self, call) == TSUCHI_OK && host.reflection->call_get(self, call, "ReturnValue", &answer) == TSUCHI_OK)
            status.game_paused = answer.kind == TSUCHI_VALUE_BOOLEAN && answer.as.boolean;
        end(call);
        call = {};
        if (host.reflection->begin_call(self, statics, "GetTimeSeconds", &call) == TSUCHI_OK && host.reflection->call_set(self, call, "WorldContextObject", &context) == TSUCHI_OK &&
            host.reflection->invoke(self, call) == TSUCHI_OK && host.reflection->call_get(self, call, "ReturnValue", &answer) == TSUCHI_OK && answer.kind == TSUCHI_VALUE_NUMBER)
            status.world_seconds = float(answer.as.number);
        end(call);
    }
    settled = status.actors_now >= kLevelActors ? settled + 1 : 0;
    if (settled != kLevelSettleSamples || status.objects_level_frame) return;
    status.objects_title_passed = status.objects_passed;
    status.objects_title_count = status.objects_count;
    check_objects();
    status.objects_level_frame = status.frames;
}


} // namespace

namespace tsuchi::target {

void selfcheck_start() {
    // Check the public screen lifetime without taking the player's menu.
    TSUCHI_Screen screen{};
    const TSUCHI_Result created = host.ui->create(self, "UI selfcheck", &screen);
    const TSUCHI_Result destroyed = created == TSUCHI_OK ? host.ui->destroy(self, screen) : created;
    mod_note(kFrameworkLogId, destroyed == TSUCHI_OK ? "UI create and destroy passed" : "UI create and destroy failed");
    // These world services are exercised by the isolated effects/navigation mod.
    // Refuse a missing table here before that mod can call through it.
    if (!host.effects || !host.world) {
        mod_block(self, "The effects or world service is unavailable.");
        return;
    }
    each_frame(world_effects_step);
    // Subscribers of the framework's own, so every frame and every actor goes through the
    // same path a mod's would.
    TSUCHI_Subscription subscription{};
    host.time->on_step(self, heard_frame, nullptr, &subscription);
    host.entity->on_spawned(self, heard_spawned, nullptr, &subscription);
    host.entity->on_destroyed(self, heard_destroyed, nullptr, &subscription);
    set_log_sink(heard_log);
}

// A queued declaration must expose its outcome and undo its own text on retract.
static void check_content_port() {
    static TSUCHI_ContentDeclaration declaration{};
    static bool finished = false;
    if (finished || status.frames < 120) return;
    if (!declaration.id) {
        const auto result = host.content->add(self, "{\"text\":{\"TsuchiContentProbe\":{\"temporary\":\"ready\"}}}", &declaration);
        if (result != TSUCHI_OK) { host.mod->log(self, "content admission refused"); finished = true; }
        return;
    }
    TSUCHI_DeclarationState state = TSUCHI_DECLARATION_STATE_QUEUED;
    if (host.content->state(self, declaration, &state) != TSUCHI_OK || state == TSUCHI_DECLARATION_STATE_QUEUED) return;
    const auto ended = host.content->retract(self, declaration);
    host.mod->log(self, state == TSUCHI_DECLARATION_STATE_READY && ended == TSUCHI_OK ? "content admission and retract passed" : "content native admission failed");
    uint64_t carrying = 0;
    const auto listed = host.carry->list(self, nullptr, 0, &carrying);
    if (listed == TSUCHI_OK || listed == TSUCHI_CAPACITY) host.mod->log(self, "carry list available");
    finished = true;
}

void selfcheck_frame() {
    // Reads alone: action and screenshot checks live in the explicit camera-input proof mod.
    static bool camera_input_read = false;
    if (!camera_input_read && game::player_captain(0)) {
        TSUCHI_View view{}; TSUCHI_PadState pad{}; bool held = false;
        if (host.camera->view(self, 0, &view) == TSUCHI_OK && host.input->pad(self, 0, &pad) == TSUCHI_OK &&
            host.input->key(self, 4, &held) == TSUCHI_UNSUPPORTED) {
            camera_input_read = true;
            mod_note("tsuchi.self", "camera/input read checks passed");
        }
    }
    check_content_port();
    if (status.frames == kCheckFrames || status.frames == kCheckAgainFrames) check_base();
    check_hooks();
    if (status.frames == kReflectTestFrame) check_reflect();
    if (status.frames == kObjectsTestFrame && !status.objects_level_frame) check_objects();
    if (status.frames > kObjectsTestFrame) watch_actors();
    if (!world_effects_selected()) check_spawning();
}

} // namespace tsuchi::target
#endif
