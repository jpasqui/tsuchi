#include "engine_hooks.h"

#include <string.h>

#include "frame.h"
#include "game.h"
#include "hooks.h"
#include "names.h"
#include "objects.h"
#include "reflect.h"
#include "reflection.h"
#include "registry.h"

namespace tsuchi {
namespace {

namespace t = game::table;
using Native = void (*)(void* object, void* frame, void* result);

const Kind kEngine{"engine function hook"};

// A function whose entry holds a thunk. Every member has an initializer, so the table is constant-initialized:
// the module has no start-up code for globals (tools/build_module.py).
struct Site {
    uint64_t target = 0;   // the function whose entry holds the thunk
    uint64_t handle = 0;   // its handle, which stops resolving when the engine lets go of it
    uint64_t original = 0; // what the entry held; read on any thread
    uint32_t inside = 0;   // calls in the thunk, on any thread
};
struct Hook {
    uint64_t id = 0; // 0 marks a free record
    TSUCHI_Mod* mod = nullptr;
    uint32_t site = 0;
    uint64_t function = 0; // the function the path named
    int64_t entry = -1;    // its entry point into the event graph, or -1 when the thunk is on its own entry
    bool live = false;
    EngineClient client;
    void* user = nullptr;
    uint32_t depth = 0; // calls using it; game thread only
};

Site sites[kEngineSites];
Hook hooks[kEngineHooks];
uint64_t next_id = uint64_t(3) << 61; // dynamic hooks count up from 1 << 62; instruction patches use bit 63

struct Counters {
    uint32_t calls = 0, off_thread = 0, script_arguments = 0, not_ready = 0, depth = 0, failed = 0, script_calls = 0;
} counters;
void count(uint32_t& counter) { __atomic_fetch_add(&counter, 1, __ATOMIC_RELAXED); }

bool same_ignoring_case(const char* a, const char* b) {
    for (;; ++a, ++b) {
        char x = *a, y = *b;
        if (x >= 'A' && x <= 'Z') x = char(x + 32);
        if (y >= 'A' && y <= 'Z') y = char(y + 32);
        if (x != y) return false;
        if (!x) return true;
    }
}

// A C++ function that takes a value of any type reads that type from the calling Blueprint's script code as it
// runs: the engine marks such a function CustomThunk, and nothing in the loaded function says so. Its reflected
// arguments are placeholders, so a hook's thunk, which lays the arguments out by them, would write past them. The
// engine's own are refused by path: three libraries whole, and three single functions.
constexpr const char* kTypedByCaller[] = {
    "/Script/Engine.KismetArrayLibrary:",
    "/Script/Engine.BlueprintMapLibrary:",
    "/Script/Engine.BlueprintSetLibrary:",
    "/Script/Engine.KismetSystemLibrary:SetStructurePropertyByName",
    "/Script/Engine.KismetSystemLibrary:SetCollisionProfileNameProperty",
    "/Script/Engine.DataTableFunctionLibrary:GetDataTableRowFromName",
};
bool typed_by_caller(const char* path) {
    for (const char* entry : kTypedByCaller) {
        const char* a = path;
        const char* b = entry;
        for (; *b; ++a, ++b) {
            char x = *a, y = *b;
            if (x >= 'A' && x <= 'Z') x = char(x + 32);
            if (y >= 'A' && y <= 'Z') y = char(y + 32);
            if (x != y) break;
        }
        // A library's entry ends at the colon and covers every function after it; a function's has to end with the path.
        if (!*b && (b[-1] == ':' || !*a)) return true;
    }
    return false;
}

uint64_t flags_of(uint64_t property) { return game::read_or<uint64_t>(property + t::property::flags); }
bool is_argument(uint64_t property) { return flags_of(property) & t::constant::property_param; }
bool is_result(uint64_t property) { return (flags_of(property) & t::constant::property_return) != 0; }

// ---- Thunks ---------------------------------------------------------------

void dispatch(uint32_t site, void* object, void* frame, void* result);
template <unsigned N>
void thunk(void* object, void* frame, void* result) {
    dispatch(N, object, frame, result);
}
Native thunks[kEngineSites];
template <unsigned N>
struct FillThunks {
    static void fill() {
        FillThunks<N - 1>::fill();
        thunks[N - 1] = &thunk<N - 1>;
    }
};
template <>
struct FillThunks<0> {
    static void fill() {}
};
bool thunks_ready = false;
void prepare_thunks() {
    if (thunks_ready) return;
    FillThunks<kEngineSites>::fill();
    thunks_ready = true;
}
uint64_t thunk_address(uint32_t site) { return reinterpret_cast<uint64_t>(thunks[site]); }

// ---- Calls ----------------------------------------------------------------

bool may_run(Hook& hook, uint32_t site, int32_t entry) {
    if (!hook.id || hook.site != site || !hook.live || !mod_running(hook.mod)) return false;
    if (hook.entry >= 0 && hook.entry != entry) return false; // another event's entry point
    if (hook.depth >= kEngineDepth) {
        count(counters.depth);
        return false;
    }
    if (hook.client.ready && !hook.client.ready(hook.user)) {
        count(counters.not_ready);
        return false;
    }
    return true;
}

// A frame's own fields, which the engine hands over as valid memory, read and written directly.
uint64_t frame_word(uint64_t frame, uint64_t offset) {
    uint64_t value;
    __builtin_memcpy(&value, reinterpret_cast<const void*>(frame + offset), sizeof value);
    return value;
}
void set_frame_word(uint64_t frame, uint64_t offset, uint64_t value) {
    __builtin_memcpy(reinterpret_cast<void*>(frame + offset), &value, sizeof value);
}

bool script_calls_present() {
    return t::present(t::data::script_natives) && t::present(t::script_frame::code) && t::present(t::script_frame::most_recent_property) &&
           t::present(t::script_frame::most_recent_address) && t::present(t::script_frame::property_chain);
}

// What FFrame::Step does: runs the opcode at the frame's code, which moves the code past its expression, and
// puts the expression's value at `result` when it isn't null.
void step(uint64_t frame, uint64_t result) {
    const uint64_t code = frame_word(frame, t::script_frame::code);
    const uint8_t opcode = *reinterpret_cast<const uint8_t*>(code);
    set_frame_word(frame, t::script_frame::code, code + 1);
    const uint64_t handler = frame_word(game::main_base() + t::data::script_natives, uint64_t(opcode) * sizeof(uint64_t));
    reinterpret_cast<Native>(handler)(reinterpret_cast<void*>(frame_word(frame, t::script_frame::object)), reinterpret_cast<void*>(frame),
                                      reinterpret_cast<void*>(result));
}

// A Blueprint's call of a C++ function: the caller's frame, whose code is at the call's arguments, as script code.
// They are run here into memory laid out as the function's, the way a C++ function's generated code runs them, so
// the handlers see them and the function then reads them from there.
struct ScriptCall {
    bool active = false;
    uint64_t memory = 0, records = 0, out_parms = 0;
    uint64_t property = 0, address = 0; // what the last argument's expression named, which the caller's frame keeps
};

// Runs the call's arguments, as execCallMulticastDelegate does: an argument with a value into its place, and one the
// function writes by its reference, which goes in the list of those, the way the engine's own calls list them. Then
// moves past EX_EndFunctionParms.
bool evaluate(uint64_t function, uint64_t frame, ScriptCall& script) {
    if (game::make_arguments(function, script.memory) != TSUCHI_OK) return false;
    uint32_t writes = 0;
    for (uint32_t i = 0;; ++i) {
        const uint64_t property = game::field_at(function, i, false);
        if (!property || !is_argument(property)) break;
        writes += !is_result(property) && (flags_of(property) & t::constant::property_out);
    }
    if (writes) {
        script.records = reinterpret_cast<uint64_t>(game::engine_memory(uint64_t(writes) * t::out_parm::size));
        if (!script.records) {
            game::destroy_arguments(function, script.memory);
            return false;
        }
    }
    uint64_t* link = &script.out_parms;
    uint32_t written = 0;
    for (uint32_t i = 0;; ++i) {
        const uint64_t property = game::field_at(function, i, false);
        if (!property || !is_argument(property)) break;
        if (is_result(property)) continue;
        const uint64_t code = frame_word(frame, t::script_frame::code);
        if (!code || *reinterpret_cast<const uint8_t*>(code) == t::constant::script_end_parms) break;
        const uint64_t place = script.memory + uint64_t(int64_t(game::read_or<int32_t>(property + t::property::offset)));
        if (flags_of(property) & t::constant::property_out) {
            set_frame_word(frame, t::script_frame::most_recent_address, 0);
            step(frame, 0);
            const uint64_t address = frame_word(frame, t::script_frame::most_recent_address);
            const uint64_t record = script.records + uint64_t(written++) * t::out_parm::size;
            set_frame_word(record, t::out_parm::property, property);
            set_frame_word(record, t::out_parm::address, address ? address : place);
            set_frame_word(record, t::out_parm::next, 0);
            *link = record;
            link = reinterpret_cast<uint64_t*>(record + t::out_parm::next);
        } else {
            step(frame, place);
        }
    }
    // What P_FINISH does: past EX_EndFunctionParms.
    if (const uint64_t code = frame_word(frame, t::script_frame::code)) set_frame_word(frame, t::script_frame::code, code + 1);
    script.property = frame_word(frame, t::script_frame::most_recent_property);
    script.address = frame_word(frame, t::script_frame::most_recent_address);
    script.active = true;
    return true;
}

void finish(uint64_t function, ScriptCall& script) {
    if (!script.active) return;
    game::destroy_arguments(function, script.memory);
    if (script.records) game::release_engine_memory(reinterpret_cast<void*>(script.records));
}

// Calls the function with the caller's frame laid out as ProcessEvent lays one out: no code left, the arguments in
// its locals, the list of those it writes, and its own fields to read them by. Puts the caller's frame back after.
void call_with_arguments(Native original, uint64_t function, void* object, uint64_t frame, void* result, const ScriptCall& script) {
    const uint64_t code = frame_word(frame, t::script_frame::code), locals = frame_word(frame, t::script_frame::locals),
                   out_parms = frame_word(frame, t::script_frame::out_parms), chain = frame_word(frame, t::script_frame::property_chain),
                   node = frame_word(frame, t::script_frame::node);
    set_frame_word(frame, t::script_frame::code, 0);
    set_frame_word(frame, t::script_frame::locals, script.memory);
    set_frame_word(frame, t::script_frame::out_parms, script.out_parms);
    set_frame_word(frame, t::script_frame::property_chain, game::read_or<uint64_t>(function + t::struct_type::properties));
    set_frame_word(frame, t::script_frame::node, function);
    if (original) original(object, reinterpret_cast<void*>(frame), result);
    set_frame_word(frame, t::script_frame::code, code);
    set_frame_word(frame, t::script_frame::locals, locals);
    set_frame_word(frame, t::script_frame::out_parms, out_parms);
    set_frame_word(frame, t::script_frame::property_chain, chain);
    set_frame_word(frame, t::script_frame::node, node);
    set_frame_word(frame, t::script_frame::most_recent_property, script.property);
    set_frame_word(frame, t::script_frame::most_recent_address, script.address);
}

// The hooks that run for this call, newest first, or none.
uint32_t choose(uint32_t site, uint64_t frame, void* object, uint32_t* chosen, EngineCall& call, ScriptCall& script) {
    if (!known_game_thread_token()) {
        count(counters.off_thread);
        return 0;
    }
    // A call that reaches a hook while a script hook's function runs comes from inside that script's runtime.
    if (in_hook_client()) {
        count(counters.not_ready);
        return 0;
    }
    const uint64_t target = sites[site].target;
    // The function's own frame comes from ProcessEvent. A Blueprint that calls a C++ function hands over its own,
    // with the arguments still to be run.
    const bool from_script = game::read_or<uint64_t>(frame + t::script_frame::node) != target;
    if (from_script && (!script_calls_present() || !game::read_or<uint64_t>(frame + t::script_frame::code))) {
        count(counters.script_arguments);
        return 0;
    }
    const uint64_t locals = from_script ? 0 : game::read_or<uint64_t>(frame + t::script_frame::locals);
    // A call into an event graph has one argument, the entry point, at the start of its locals. A Blueprint's call
    // of a C++ function is no event's.
    const int32_t entry = from_script ? INT32_MIN : game::read_or<int32_t>(locals, -1);
    uint32_t found = 0;
    for (uint32_t i = 0; i < kEngineHooks; ++i) {
        if (!may_run(hooks[i], site, entry)) continue;
        if (found == kEngineRunning) {
            count(counters.not_ready);
            continue;
        }
        // Newest first: ids grow as hooks are put on.
        uint32_t at = found++;
        while (at && hooks[chosen[at - 1]].id < hooks[i].id) {
            chosen[at] = chosen[at - 1];
            --at;
        }
        chosen[at] = i;
    }
    if (!found) return 0;
    if (from_script) {
        if (!evaluate(target, frame, script)) {
            count(counters.not_ready);
            return 0;
        }
        count(counters.script_calls);
        call.object = game::to_handle(reinterpret_cast<uint64_t>(object));
        call.locals = script.memory;
        call.out_parms = script.out_parms;
        return found;
    }
    call.object = game::to_handle(game::read_or<uint64_t>(frame + t::script_frame::object));
    call.locals = locals;
    call.out_parms = game::read_or<uint64_t>(frame + t::script_frame::out_parms);
    return found;
}

void dispatch(uint32_t site, void* object, void* frame, void* result) {
    Site& record = sites[site];
    __atomic_fetch_add(&record.inside, 1, __ATOMIC_ACQ_REL);
    const auto original = reinterpret_cast<Native>(__atomic_load_n(&record.original, __ATOMIC_ACQUIRE));
    count(counters.calls);
    uint32_t chosen[kEngineRunning];
    EngineCall call;
    ScriptCall script;
    const uint32_t running = choose(site, reinterpret_cast<uint64_t>(frame), object, chosen, call, script);
    call.result = reinterpret_cast<uint64_t>(result);
    for (uint32_t i = 0; i < running; ++i) ++hooks[chosen[i]].depth;
    for (uint32_t i = 0; i < running; ++i) {
        Hook& hook = hooks[chosen[i]];
        if (!hook.live || !hook.client.before) continue;
        call.function = hook.function; // an event's own, where the thunk is on its event graph's
        hook_client_enter();
        const bool done = hook.client.before(hook.user, call);
        hook_client_leave();
        if (!done) count(counters.failed);
    }
    if (script.active) call_with_arguments(original, record.target, object, reinterpret_cast<uint64_t>(frame), result, script);
    else if (original) original(object, frame, result);
    call.finished = true;
    // After the call, oldest first, so the newest hook's `after` has the last word.
    for (uint32_t i = running; i--;) {
        Hook& hook = hooks[chosen[i]];
        if (!hook.live || !hook.client.after || !mod_running(hook.mod)) continue;
        call.function = hook.function;
        hook_client_enter();
        const bool done = hook.client.after(hook.user, call);
        hook_client_leave();
        if (!done) count(counters.failed);
    }
    for (uint32_t i = 0; i < running; ++i) --hooks[chosen[i]].depth;
    finish(record.target, script);
    __atomic_fetch_sub(&record.inside, 1, __ATOMIC_ACQ_REL);
}

// ---- Values ---------------------------------------------------------------

// Where the argument a path begins with is: in the frame's locals; for one the function writes, where the
// frame's list of those says; for the result, where the caller wants it. `base` gets that place less the
// argument's offset, so the reflect reader, which adds the offset again, starts there.
TSUCHI_Result base_of(const EngineCall& call, const char* path, uint64_t& base) {
    if (!path) return TSUCHI_INVALID;
    char name[game::kNameBytes];
    size_t length = 0;
    for (; path[length] && path[length] != '.' && path[length] != '['; ++length) {
        if (length + 1 >= sizeof name) return TSUCHI_INVALID;
        name[length] = path[length];
    }
    name[length] = 0;
    if (!length) return TSUCHI_INVALID;
    for (uint32_t i = 0;; ++i) {
        const uint64_t property = game::field_at(call.function, i, false);
        if (!property) return TSUCHI_NOT_FOUND;
        if (!is_argument(property)) continue;
        char text[game::kNameBytes];
        if (!game::name_text(game::field_name(property), text, sizeof text) || !same_ignoring_case(text, name)) continue;
        const int32_t offset = game::read_or<int32_t>(property + t::property::offset);
        uint64_t at = call.locals + uint64_t(int64_t(offset));
        if (is_result(property)) {
            if (!call.finished) return TSUCHI_NOT_FOUND;
            if (call.result) at = call.result;
        } else if (flags_of(property) & t::constant::property_out) {
            uint64_t record = call.out_parms;
            for (uint32_t guard = 0; record && guard < 256; ++guard, record = game::read_or<uint64_t>(record + t::out_parm::next))
                if (game::read_or<uint64_t>(record + t::out_parm::property) == property) {
                    at = game::read_or<uint64_t>(record + t::out_parm::address, at);
                    break;
                }
        }
        base = at - uint64_t(int64_t(offset));
        return TSUCHI_OK;
    }
}

void end_hook(void*, uint64_t id) {
    for (Hook& hook : hooks)
        if (hook.id == id) hook.live = false;
}

void drop(Hook& hook) {
    if (hook.client.release) hook.client.release(hook.user);
    hook = Hook{};
}

void reclaim() {
    for (Hook& hook : hooks)
        if (hook.id && !hook.live && !hook.depth) drop(hook);
}

// A site for the function: the one whose thunk its entry holds, or a new one whose thunk it gets.
TSUCHI_Result site_for(uint64_t target, uint32_t& out) {
    const uint64_t entry = target + t::function::func;
    uint64_t current = 0;
    if (!game::read(entry, current) || !current) return TSUCHI_NOT_READY;
    for (uint32_t i = 0; i < kEngineSites; ++i)
        if (sites[i].target == target && current == thunk_address(i)) {
            sites[i].handle = game::to_handle(target);
            out = i;
            return TSUCHI_OK;
        }
    if (!game::writable(entry, sizeof current)) return TSUCHI_NOT_READY;
    // A free site, or one whose function the engine has let go of, with no call in its thunk and no hook left.
    uint32_t free = kEngineSites;
    for (uint32_t i = 0; i < kEngineSites && free == kEngineSites; ++i) {
        const Site& site = sites[i];
        if (!site.target) { free = i; break; }
        if (game::from_handle(site.handle) == site.target || __atomic_load_n(&site.inside, __ATOMIC_ACQUIRE)) continue;
        bool used = false;
        for (const Hook& hook : hooks) used |= hook.id && hook.site == i;
        if (!used) free = i;
    }
    if (free == kEngineSites) return TSUCHI_CAPACITY;
    Site& site = sites[free];
    site.target = target;
    site.handle = game::to_handle(target);
    __atomic_store_n(&site.original, current, __ATOMIC_RELEASE);
    // Filled in before the thunk can be reached.
    __atomic_store_n(reinterpret_cast<uint64_t*>(entry), thunk_address(free), __ATOMIC_RELEASE);
    out = free;
    return TSUCHI_OK;
}

game::ClassRef function_class{t::name::function_class};
game::ClassRef actor_class{"Actor"};
game::ClassRef component_class{"ActorComponent"};

struct Search {
    const char* path;
    const char* name;
    uint64_t found;
};

bool engine_hooks_present() {
    return t::present(t::function::func) && t::present(t::function::event_graph) && t::present(t::function::event_graph_entry) &&
           t::present(t::script_frame::node) && t::present(t::script_frame::object) && t::present(t::script_frame::locals) &&
           t::present(t::script_frame::out_parms) && t::present(t::out_parm::property) && t::present(t::out_parm::address) &&
           t::present(t::out_parm::next);
}

} // namespace

TSUCHI_Result engine_read(const EngineCall& call, const char* path, TSUCHI_Value* out) {
    uint64_t base = 0;
    const TSUCHI_Result result = base_of(call, path, base);
    return result == TSUCHI_OK ? read_at(call.function, base, path, out) : result;
}

TSUCHI_Result engine_text(const EngineCall& call, const char* path, char* buffer, uint64_t capacity, uint64_t* needed) {
    uint64_t base = 0;
    const TSUCHI_Result result = base_of(call, path, base);
    return result == TSUCHI_OK ? text_at(call.function, base, path, buffer, capacity, needed) : result;
}

TSUCHI_Result engine_field_at(const EngineCall& call, const char* path, uint32_t index, char* buffer, uint64_t capacity, uint64_t* needed) {
    if (!path) return TSUCHI_INVALID;
    if (*path) {
        uint64_t base = 0;
        const TSUCHI_Result result = base_of(call, path, base);
        return result == TSUCHI_OK ? field_name_at(call.function, base, path, index, true, buffer, capacity, needed) : result;
    }
    for (uint32_t i = 0;; ++i) {
        const uint64_t property = game::field_at(call.function, i, false);
        if (!property) return TSUCHI_NOT_FOUND;
        if (is_argument(property) && !is_result(property) && !index--) return game::name_into(game::field_name(property), buffer, capacity, needed);
    }
}

TSUCHI_Result engine_result_name(const EngineCall& call, char* buffer, uint64_t capacity, uint64_t* needed) {
    for (uint32_t i = 0;; ++i) {
        const uint64_t property = game::field_at(call.function, i, false);
        if (!property) return TSUCHI_NOT_FOUND;
        if (is_argument(property) && is_result(property)) return game::name_into(game::field_name(property), buffer, capacity, needed);
    }
}

TSUCHI_Result engine_write(const EngineCall& call, const EngineWrite* writes, uint32_t count) {
    if (!count) return TSUCHI_OK;
    if (!writes) return TSUCHI_INVALID;
    game::WritePlan plan;
    for (uint32_t i = 0; i < count; ++i) {
        uint64_t base = 0;
        TSUCHI_Result result = writes[i].value ? base_of(call, writes[i].path, base) : TSUCHI_INVALID;
        if (result == TSUCHI_OK) result = plan.prepare(call.function, base, writes[i].path, *writes[i].value);
        if (result != TSUCHI_OK) return result;
    }
    plan.commit();
    return TSUCHI_OK;
}

const char* engine_object_kind(uint64_t handle) {
    const uint64_t object = game::from_handle(handle);
    if (!object) return "object";
    if (game::is_a(object, actor_class)) return "actor";
    if (game::is_a(object, component_class)) return "component";
    return "object";
}

uint64_t find_function_path(const char* path) {
    if (!path || !*path || strlen(path) >= 4096) return 0;
    const char* name = path;
    for (const char* at = path; *at; ++at)
        if (*at == ':' || *at == '.' || *at == '/') name = at + 1;
    if (!*name || name == path) return 0;
    if (!game::find_class(function_class)) return 0;
    Search search{path, name, 0};
    game::each_object(
        [](void* context, uint64_t object) {
            auto& wanted = *static_cast<Search*>(context);
            char text[game::kNameBytes];
            if (!game::name_text(game::name_of(object), text, sizeof text) || !same_ignoring_case(text, wanted.name)) return true;
            if (!game::is_a(object, function_class)) return true;
            char full[4096];
            uint64_t needed = 0;
            if (game::object_path(object, full, sizeof full, &needed) != TSUCHI_OK || !same_ignoring_case(full, wanted.path)) return true;
            wanted.found = object;
            return false;
        },
        &search);
    return search.found;
}

TSUCHI_Result engine_hook(TSUCHI_Mod* mod, const char* path, const EngineClient& client, void* user, uint64_t* id) {
    if (!mod_usable(mod) || !path || !id || (!client.before && !client.after)) return TSUCHI_INVALID;
    if (!engine_hooks_present()) return TSUCHI_UNSUPPORTED;
    prepare_thunks();
    reclaim();
    const uint64_t function = find_function_path(path);
    if (!function) return TSUCHI_NOT_FOUND;
    if (typed_by_caller(path)) return TSUCHI_UNSUPPORTED;
    uint64_t target = function;
    int64_t entry = -1;
    if (const uint64_t graph = game::read_or<uint64_t>(function + t::function::event_graph)) {
        target = graph;
        entry = game::read_or<int32_t>(function + t::function::event_graph_entry);
        if (entry < 0 || !game::live(target)) return TSUCHI_NOT_READY;
    }
    Hook* hook = nullptr;
    for (Hook& candidate : hooks)
        if (!candidate.id) {
            hook = &candidate;
            break;
        }
    if (!hook) return TSUCHI_CAPACITY;
    uint32_t site = 0;
    TSUCHI_Result result = site_for(target, site);
    if (result != TSUCHI_OK) return result;
    const uint64_t serial = next_id;
    result = own(mod, kEngine, serial, end_hook, nullptr);
    if (result != TSUCHI_OK) return result;
    ++next_id;
    *hook = Hook{};
    hook->id = serial;
    hook->mod = mod;
    hook->site = site;
    hook->function = function;
    hook->entry = entry;
    hook->client = client;
    hook->user = user;
    hook->live = true;
    *id = serial;
    return TSUCHI_OK;
}

TSUCHI_Result engine_hook_remove(TSUCHI_Mod* mod, uint64_t id) {
    if (!mod_usable(mod)) return TSUCHI_INVALID;
    if (disown(mod, kEngine, id) != TSUCHI_OK) return TSUCHI_NOT_FOUND;
    end_hook(nullptr, id);
    return TSUCHI_OK;
}

EngineStats engine_stats() {
    EngineStats stats{};
    for (const Site& site : sites) stats.sites_used += site.target != 0;
    for (const Hook& hook : hooks) stats.hooks_used += hook.id != 0;
    stats.calls = __atomic_load_n(&counters.calls, __ATOMIC_RELAXED);
    stats.off_thread = __atomic_load_n(&counters.off_thread, __ATOMIC_RELAXED);
    stats.script_arguments = __atomic_load_n(&counters.script_arguments, __ATOMIC_RELAXED);
    stats.not_ready = __atomic_load_n(&counters.not_ready, __ATOMIC_RELAXED);
    stats.depth = __atomic_load_n(&counters.depth, __ATOMIC_RELAXED);
    stats.failed = __atomic_load_n(&counters.failed, __ATOMIC_RELAXED);
    stats.script_calls = __atomic_load_n(&counters.script_calls, __ATOMIC_RELAXED);
    return stats;
}

void engine_hooks_frame() {
    reclaim();
    for (Hook& hook : hooks)
        if (hook.id && hook.live && hook.client.report) hook.client.report(hook.user);
}

void engine_hooks_start() {
    prepare_thunks();
    set_engine_remove(engine_hook_remove);
    each_engine_frame(engine_hooks_frame);
}

} // namespace tsuchi
