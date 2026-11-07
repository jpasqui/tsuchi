// JavaScript mod entry and registry-owned runtime lifetime.
#include "tsuchi_js.h"
#include "host.h"
#include "mods.h"
#include "registry.h"
namespace tsuchi_js {
namespace {
const tsuchi::Kind kScript{"javascript.state"};
void close(void* context, uint64_t) { destroy(static_cast<State*>(context)); }
TSUCHI_Result start(TSUCHI_Mod* mod, tsuchi::Text& problem) {
    State* s = create(mod, &tsuchi::host);
    if (!s) return TSUCHI_NO_MEMORY;
    const TSUCHI_Result owned = tsuchi::own(mod, kScript, 0, close, s);
    if (owned != TSUCHI_OK) { destroy(s); return owned; }
    JSContext* ctx = context(s);
    uint64_t n = 0, got = 0;
    TSUCHI_Result result = tsuchi::host.file->size(mod, "main.js", &n);
    Buffer source(ctx);
    if (result == TSUCHI_OK && (n == UINT64_MAX || !source.resize(n + 1))) result = TSUCHI_NO_MEMORY;
    if (result == TSUCHI_OK) result = tsuchi::host.file->read(mod, "main.js", 0, source.data, n, &got);
    if (result != TSUCHI_OK || got != n) { problem.set(result_name(result == TSUCHI_OK ? TSUCHI_IO : result)); return result == TSUCHI_OK ? TSUCHI_IO : result; }
    static_cast<char*>(source.data)[n] = 0;
    if (!JS_HasException(ctx) && evaluate(ctx, static_cast<char*>(source.data), static_cast<size_t>(n), "main.js", true)) return TSUCHI_OK;
    Value error(ctx, JS_GetException(ctx)); const char* text = JS_ToCString(ctx, error);
    problem.set(text ? text : "script error"); if (text) JS_FreeCString(ctx, text);
    return TSUCHI_INVALID;
}
// Compiles the mod's main.js as a module in a runtime of its own, with the modules it imports, without running any
// of them, as a restart does before it stops the mod. The running mod's context reads the imported files.
TSUCHI_Result check(const char* id, tsuchi::Text& problem) {
    char* text = nullptr;
    uint64_t n = 0;
    const TSUCHI_Result read = tsuchi::read_mod_file(id, "main.js", text, n);
    if (read != TSUCHI_OK) { problem.set(read == TSUCHI_NO_MEMORY ? "main.js: there is no memory to read it" : "main.js: it could not be read"); return read; }
    const tsuchi::ModRecord* record = tsuchi::find_mod(id);
    State* s = create(record ? record->context : nullptr, &tsuchi::host);
    if (!s) { tsuchi::release(text); problem.set("main.js: there is no memory to compile it"); return TSUCHI_NO_MEMORY; }
    JSContext* ctx = context(s);
    TSUCHI_Result result = TSUCHI_OK;
    {
        Buffer source(ctx); // QuickJS reads up to a terminator after the text
        if (n == UINT64_MAX || !source.resize(n + 1)) result = TSUCHI_NO_MEMORY;
        else {
            if (n) memcpy(source.data, text, static_cast<size_t>(n));
            static_cast<char*>(source.data)[n] = 0;
            Value compiled(ctx, JS_Eval(ctx, static_cast<char*>(source.data), static_cast<size_t>(n), "main.js", JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY));
            if (JS_IsException(compiled)) {
                Value error(ctx, JS_GetException(ctx));
                Value stack(ctx, JS_GetPropertyStr(ctx, error, "stack"));
                const char* said = JS_ToCString(ctx, error);
                const char* trace = JS_IsString(stack) ? JS_ToCString(ctx, stack) : nullptr;
                // The message, and where it is: the stack's first place, such as "main.js:3:7".
                const char* at = trace ? strstr(trace, "main.js:") : nullptr;
                size_t where = 0;
                while (at && at[where] && at[where] != '\n' && at[where] != ')') ++where;
                const char* message = said ? said : "main.js: it does not compile";
                const size_t length = strlen(message);
                if (!problem.reserve(length + where + 5)) result = TSUCHI_NO_MEMORY;
                else {
                    char* end = problem.data;
                    memcpy(end, message, length), end += length;
                    if (where) memcpy(end, " at ", 4), memcpy(end + 4, at, where), end += 4 + where;
                    *end = 0;
                }
                if (trace) JS_FreeCString(ctx, trace);
                if (said) JS_FreeCString(ctx, said);
                if (result == TSUCHI_OK) result = TSUCHI_INVALID;
            }
        }
    }
    tsuchi::release(text);
    destroy(s);
    if (result == TSUCHI_NO_MEMORY) problem.set("main.js: there is no memory to compile it");
    return result;
}
}
void register_script_mods() { tsuchi::add_mod_runtime({"main.js", start, check}); }
}