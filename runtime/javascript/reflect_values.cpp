// Plain reflection values share the compiled service's validation and ownership.
#include "tsuchi_js.h"
#include "script_writes.h"
#include <stdio.h>
namespace {
using tsuchi_js::Value;
using tsuchi_js::String;
using tsuchi_js::Buffer;
using tsuchi_js::host;
using tsuchi_js::mod;
struct Target {
    JSContext* ctx; TSUCHI_Object object{}; TSUCHI_PreparedCall call{}; const char* row = nullptr;
    const TSUCHI_ReflectionService& api() const { return *host(ctx)->reflection; }
    const TSUCHI_DatatableService& tables() const { return *host(ctx)->datatable; }
    TSUCHI_Result get(const char* p, TSUCHI_Value* v) const { return row ? tables().get(mod(ctx), object, row, p, v) : call.id ? api().call_get(mod(ctx), call, p, v) : api().get(mod(ctx), object, p, v); }
    TSUCHI_Result set(const char* p, const TSUCHI_Value& v) const { return row ? tables().override(mod(ctx), object, row, p, &v) : call.id ? api().call_set(mod(ctx), call, p, &v) : api().set(mod(ctx), object, p, &v); }
    TSUCHI_Result get_text(const char* p, char* b, uint64_t c, uint64_t* n) const { return row ? tables().get_text(mod(ctx), object, row, p, b, c, n) : call.id ? api().call_get_text(mod(ctx), call, p, b, c, n) : api().get_text(mod(ctx), object, p, b, c, n); }
    TSUCHI_Result set_text(const char* p, const char* t) const { return row ? tables().override_text(mod(ctx), object, row, p, t) : call.id ? api().call_set_text(mod(ctx), call, p, t) : api().set_text(mod(ctx), object, p, t); }
    TSUCHI_Result resize(const char* p, uint32_t n) const { return row ? TSUCHI_INVALID : call.id ? api().call_resize(mod(ctx), call, p, n) : api().resize(mod(ctx), object, p, n); }
    TSUCHI_Result field_at(const char* p, uint32_t i, char* b, uint64_t c, uint64_t* n) const { return row ? tables().field_at(mod(ctx), object, row, p, i, b, c, n) : call.id ? api().call_field_at(mod(ctx), call, p, i, b, c, n) : api().field_at(mod(ctx), object, p, i, b, c, n); }
};
struct Path {
    char text[4096]; size_t length = 0;
    bool set(const char* p) { if (!p) return false; length = strlen(p); if (length >= sizeof text) return false; memcpy(text, p, length + 1); return true; }
    bool field(const char* name) { int n = snprintf(text + length, sizeof text - length, "%s%s", length ? "." : "", name); if (n < 0 || size_t(n) >= sizeof text - length) return false; length += size_t(n); return true; }
    bool element(uint32_t i) { int n = snprintf(text + length, sizeof text - length, "[%u]", i); if (n < 0 || size_t(n) >= sizeof text - length) return false; length += size_t(n); return true; }
    void back_to(size_t n) { text[length = n] = 0; }
};
template<class Read>
const char* read_name(Buffer& buffer, char* small, size_t cap, Read read) {
    uint64_t n = 0; TSUCHI_Result r = read(small, cap, &n);
    if (r != TSUCHI_CAPACITY) return r == TSUCHI_OK ? small : nullptr;
    if (!buffer.resize(n)) return nullptr;
    return read(static_cast<char*>(buffer.data), n, &n) == TSUCHI_OK ? static_cast<char*>(buffer.data) : nullptr;
}
JSValue object(JSContext* ctx, uint64_t id) {
    if (!id) return JS_NULL;
    TSUCHI_Object type{}; bool yes = false;
    if (host(ctx)->reflection->find_class(mod(ctx), "Actor", &type) == TSUCHI_OK && host(ctx)->reflection->is_a(mod(ctx), {id}, type, &yes) == TSUCHI_OK && yes) return tsuchi_js::push_handle(ctx, "actor", id);
    if (host(ctx)->reflection->find_class(mod(ctx), "ActorComponent", &type) == TSUCHI_OK && host(ctx)->reflection->is_a(mod(ctx), {id}, type, &yes) == TSUCHI_OK && yes) return tsuchi_js::push_handle(ctx, "component", id);
    return tsuchi_js::push_handle(ctx, "object", id);
}
TSUCHI_Result get(const Target& t, Path& p, unsigned depth, JSValue* out) {
    JSContext* ctx = t.ctx; TSUCHI_Value v{};
    TSUCHI_Result r = t.get(p.text, &v); if (r != TSUCHI_OK) return r;
    if (depth > 16) return TSUCHI_UNSUPPORTED;
    const size_t here = p.length;
    switch (v.kind) {
    case TSUCHI_VALUE_NONE: *out = JS_NULL; break;
    case TSUCHI_VALUE_BOOLEAN: *out = JS_NewBool(ctx, v.as.boolean); break;
    case TSUCHI_VALUE_INTEGER: *out = JS_NewBigInt64(ctx, v.as.integer); break;
    case TSUCHI_VALUE_NUMBER: *out = JS_NewFloat64(ctx, v.as.number); break;
    case TSUCHI_VALUE_OBJECT: *out = object(ctx, v.as.object.id); break;
    case TSUCHI_VALUE_TEXT: {
        Buffer text(ctx); if (!text.resize(v.count)) return TSUCHI_NO_MEMORY;
        uint64_t n = 0; r = t.get_text(p.text, static_cast<char*>(text.data), v.count, &n);
        if (r != TSUCHI_OK) return r;
        *out = JS_NewStringLen(ctx, static_cast<char*>(text.data), n ? size_t(n - 1) : 0); break;
    }
    case TSUCHI_VALUE_STRUCT: {
        Value record(ctx, JS_NewObject(ctx)); char small[256]; Buffer name_buffer(ctx);
        for (uint32_t i = 0;; ++i) {
            const char* name = read_name(name_buffer, small, sizeof small, [&](char* b, uint64_t c, uint64_t* n) { return t.field_at(p.text, i, b, c, n); });
            if (!name) break;
            JSValue member = JS_UNDEFINED;
            if (p.field(name) && get(t, p, depth + 1, &member) == TSUCHI_OK) JS_SetPropertyStr(ctx, record, name, member);
            p.back_to(here);
            if (JS_HasException(ctx)) return TSUCHI_NO_MEMORY;
        }
        *out = record.take(); break;
    }
    case TSUCHI_VALUE_ARRAY: {
        Value array(ctx, JS_NewArray(ctx));
        for (uint32_t i = 0; i < v.count; ++i) {
            JSValue member = JS_UNDEFINED;
            r = p.element(i) ? get(t, p, depth + 1, &member) : TSUCHI_INVALID; p.back_to(here);
            if (r != TSUCHI_OK) return r;
            if (JS_SetPropertyUint32(ctx, array, i, member) < 0) return TSUCHI_NO_MEMORY;
        }
        *out = array.take(); break;
    }
    default: return TSUCHI_UNSUPPORTED;
    }
    return JS_IsException(*out) ? TSUCHI_NO_MEMORY : TSUCHI_OK;
}
bool numeric_key(const char* name) {
    if (!*name) return false;
    for (const char* p = name; *p; ++p) if (*p < '0' || *p > '9') return false;
    return true;
}
struct Properties {
    JSContext* ctx; JSPropertyEnum* entries = nullptr; uint32_t count = 0;
    explicit Properties(JSContext* c) : ctx(c) {}
    ~Properties() { for (uint32_t i = 0; i < count; ++i) JS_FreeAtom(ctx, entries[i].atom); js_free(ctx, entries); }
    bool read(JSValueConst v) { return JS_GetOwnPropertyNames(ctx, &entries, &count, v, JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) == 0; }
};
using tsuchi::ScriptWrite;
using tsuchi::ScriptWriteKind;
using tsuchi::ScriptWriteTree;
// Getters and conversions run while copying input, before any native writes.
TSUCHI_Result materialize(JSContext* ctx, JSValueConst input, unsigned depth,
                          ScriptWriteTree& tree, ScriptWrite** out, void** ancestors, unsigned max_depth = 16) {
    if (JS_HasException(ctx) || JS_IsException(input)) return TSUCHI_INVALID;
    if (depth > max_depth) return TSUCHI_UNSUPPORTED;
    ScriptWrite* node = tree.add(ScriptWriteKind::value);
    if (!node) return TSUCHI_NO_MEMORY;
    *out = node;
    TSUCHI_Value& v = node->value;
    if (tsuchi_js::absent(input)) { v.kind = TSUCHI_VALUE_NONE; return TSUCHI_OK; }
    if (JS_IsBool(input)) { v.kind = TSUCHI_VALUE_BOOLEAN; v.as.boolean = JS_ToBool(ctx, input) != 0; return TSUCHI_OK; }
    if (JS_IsBigInt(ctx, input)) { v.kind = TSUCHI_VALUE_INTEGER; v.as.integer = tsuchi_js::i64(ctx, input); return JS_HasException(ctx) ? TSUCHI_INVALID : TSUCHI_OK; }
    if (JS_IsNumber(input)) {
        double n = 0;
        if (JS_ToFloat64(ctx, &n, input) < 0) return TSUCHI_INVALID;
        if (n >= -9007199254740991.0 && n <= 9007199254740991.0 && static_cast<double>(static_cast<int64_t>(n)) == n) { v.kind = TSUCHI_VALUE_INTEGER; v.as.integer = static_cast<int64_t>(n); }
        else { v.kind = TSUCHI_VALUE_NUMBER; v.as.number = n; }
        return TSUCHI_OK;
    }
    if (JS_IsString(input)) {
        String text(ctx, input, nullptr, true);
        if (!text.text) return TSUCHI_NO_MEMORY;
        if (strlen(text.text) != text.size) return TSUCHI_INVALID;
        node->kind = ScriptWriteKind::text;
        return node->text.set(text) ? TSUCHI_OK : TSUCHI_NO_MEMORY;
    }
    if (tsuchi_js::to_handle(ctx, input, &v.as.object.id)) { v.kind = TSUCHI_VALUE_OBJECT; return TSUCHI_OK; }
    if (!JS_IsObject(input)) return TSUCHI_INVALID;
    void* identity = JS_VALUE_GET_PTR(input);
    for (unsigned i = 0; i < depth; ++i) if (ancestors[i] == identity) return TSUCHI_UNSUPPORTED;
    ancestors[depth] = identity;
    const int array = JS_IsArray(ctx, input);
    if (array < 0 || JS_HasException(ctx)) return TSUCHI_INVALID;
    node->kind = array ? ScriptWriteKind::array : ScriptWriteKind::structure;
    if (array) {
        Value length(ctx, JS_GetPropertyStr(ctx, input, "length")); double size = 0;
        if (JS_IsException(length) || JS_ToFloat64(ctx, &size, length) < 0 || JS_HasException(ctx)) return TSUCHI_INVALID;
        if (size > tsuchi::kMaxScriptArrayCount) return TSUCHI_CAPACITY;
        if (!(size >= 0)) return TSUCHI_INVALID;
        const uint32_t count = static_cast<uint32_t>(size);
        if (count != size) return TSUCHI_INVALID;
        for (uint32_t i = 0; i < count; ++i) {
            Value member(ctx, JS_GetPropertyUint32(ctx, input, i));
            ScriptWrite* child = nullptr;
            TSUCHI_Result r = materialize(ctx, member, depth + 1, tree, &child, ancestors, max_depth);
            if (r != TSUCHI_OK) return r;
            if (!node->children.add(child)) return TSUCHI_NO_MEMORY;
        }
    } else {
        Properties keys(ctx); if (!keys.read(input)) return TSUCHI_NO_MEMORY;
        for (uint32_t i = 0; i < keys.count; ++i) {
            Value key(ctx, JS_AtomToString(ctx, keys.entries[i].atom));
            String name(ctx, key, nullptr, true);
            if (!name.text) return TSUCHI_NO_MEMORY;
            if (strlen(name.text) != name.size || numeric_key(name)) return TSUCHI_INVALID;
            Value member(ctx, JS_GetProperty(ctx, input, keys.entries[i].atom));
            ScriptWrite* child = nullptr;
            TSUCHI_Result r = materialize(ctx, member, depth + 1, tree, &child, ancestors, max_depth);
            if (r != TSUCHI_OK) return r;
            if (!child->name.set(name) || !node->children.add(child)) return TSUCHI_NO_MEMORY;
        }
    }
    return JS_HasException(ctx) ? TSUCHI_INVALID : TSUCHI_OK;
}
TSUCHI_Result apply(const Target& t, const char* path, const ScriptWrite& node) {
    if (node.kind == ScriptWriteKind::value) return t.set(path, node.value);
    if (node.kind == ScriptWriteKind::text) return t.set_text(path, node.text.get());
    return tsuchi::apply_script_write(mod(t.ctx), {t.object, t.call, t.row}, path, node);
}
TSUCHI_Result set(const Target& t, Path& p, JSValueConst input, unsigned depth) {
    ScriptWriteTree tree;
    void* ancestors[17]{};
    TSUCHI_Result r = materialize(t.ctx, input, depth, tree, &tree.root, ancestors);
    return r == TSUCHI_OK ? apply(t, p.text, *tree.root) : r;
}

JSValue finish(JSContext* ctx, TSUCHI_Result r, JSValue out) {
    if (JS_HasException(ctx)) { JS_FreeValue(ctx, out); return JS_EXCEPTION; }
    if (r == TSUCHI_OK) return out;
    JS_FreeValue(ctx, out); return tsuchi_js::fail(ctx, r);
}
JSValue access(JSContext* ctx, int argc, JSValueConst* argv, bool table, bool write) {
    const uint64_t id = tsuchi_js::handle(ctx, tsuchi_js::arg(argc, argv, 0), "object", false);
    String row(ctx, table ? tsuchi_js::arg(argc, argv, 1) : JS_UNDEFINED, "");
    String name(ctx, tsuchi_js::arg(argc, argv, table ? 2 : 1));
    if (JS_HasException(ctx)) return JS_EXCEPTION;
    Target t{ctx, {id}, {}, table ? row.text : nullptr}; Path path;
    if (!path.set(name)) return tsuchi_js::fail(ctx, TSUCHI_INVALID);
    JSValue out = JS_TRUE;
    TSUCHI_Result r;
    if (write) {
        const int i = table ? 3 : 2;
        if (argc <= i) return JS_ThrowTypeError(ctx, "expected a value");
        r = set(t, path, argv[i], 0);
    } else r = get(t, path, 0, &out);
    return finish(ctx, r, out);
}
}
JSValue tsuchi_js_reflection_get(JSContext* c, int n, JSValueConst* a) { return access(c, n, a, false, false); }
JSValue tsuchi_js_reflection_set(JSContext* c, int n, JSValueConst* a) { return access(c, n, a, false, true); }
namespace {
bool held_scalar(JSContext* ctx, JSValueConst from, TSUCHI_Value& value) {
    if (JS_IsBool(from)) { value.kind = TSUCHI_VALUE_BOOLEAN; value.as.boolean = JS_ToBool(ctx, from); }
    else if (JS_IsBigInt(ctx, from)) { value.kind = TSUCHI_VALUE_INTEGER; value.as.integer = tsuchi_js::i64(ctx, from); }
    else if (JS_IsNumber(from)) { value.kind = TSUCHI_VALUE_NUMBER; value.as.number = tsuchi_js::number(ctx, from); }
    else return false;
    return !JS_HasException(ctx);
}
}
JSValue tsuchi_js_reflection_hold_field(JSContext* ctx, int argc, JSValueConst* argv) {
    const TSUCHI_Object object{tsuchi_js::handle(ctx, tsuchi_js::arg(argc, argv, 0), "object", false)};
    String path(ctx, tsuchi_js::arg(argc, argv, 1)), identity(ctx, tsuchi_js::arg(argc, argv, 3));
    if (JS_HasException(ctx)) return JS_EXCEPTION;
    TSUCHI_Value value{};
    if (!held_scalar(ctx, tsuchi_js::arg(argc, argv, 2), value))
        return JS_HasException(ctx) ? JS_EXCEPTION : tsuchi_js::fail(ctx, TSUCHI_INVALID);
    TSUCHI_FieldHold hold{};
    const auto result = tsuchi_js::host(ctx)->reflection->hold_field(tsuchi_js::mod(ctx), object, path, &value, identity, &hold);
    return result == TSUCHI_OK ? tsuchi_js::push_handle(ctx, "field_hold", hold.id) : tsuchi_js::fail(ctx, result);
}
JSValue tsuchi_js_reflection_update_field(JSContext* ctx, int argc, JSValueConst* argv) {
    const TSUCHI_FieldHold hold{tsuchi_js::handle(ctx, tsuchi_js::arg(argc, argv, 0), "field_hold", false)};
    if (JS_HasException(ctx)) return JS_EXCEPTION;
    TSUCHI_Value value{};
    if (!held_scalar(ctx, tsuchi_js::arg(argc, argv, 1), value))
        return JS_HasException(ctx) ? JS_EXCEPTION : tsuchi_js::fail(ctx, TSUCHI_INVALID);
    const auto result = tsuchi_js::host(ctx)->reflection->update_field(tsuchi_js::mod(ctx), hold, &value);
    return result == TSUCHI_OK ? JS_TRUE : tsuchi_js::fail(ctx, result);
}

namespace {
bool setting_scalar(JSContext* ctx, JSValueConst from, TSUCHI_Value& value) {
    if (!held_scalar(ctx, from, value)) return false;
    if (value.kind == TSUCHI_VALUE_NUMBER && value.as.number >= -9007199254740991.0 && value.as.number <= 9007199254740991.0) {
        const int64_t integer = int64_t(value.as.number);
        if (double(integer) == value.as.number) { value.kind = TSUCHI_VALUE_INTEGER; value.as.integer = integer; }
    }
    return true;
}
}
JSValue tsuchi_js_settings_hold(JSContext* ctx, int argc, JSValueConst* argv) {
    String key(ctx, tsuchi_js::arg(argc, argv, 0));
    if (JS_HasException(ctx)) return JS_EXCEPTION;
    TSUCHI_Value value{};
    if (!setting_scalar(ctx, tsuchi_js::arg(argc, argv, 1), value))
        return JS_HasException(ctx) ? JS_EXCEPTION : tsuchi_js::fail(ctx, TSUCHI_INVALID);
    TSUCHI_SettingHold hold{};
    const auto result = tsuchi_js::host(ctx)->settings->hold(tsuchi_js::mod(ctx), key, &value, &hold);
    return result == TSUCHI_OK ? tsuchi_js::push_handle(ctx, "setting_hold", hold.id) : tsuchi_js::fail(ctx, result);
}
JSValue tsuchi_js_settings_update_hold(JSContext* ctx, int argc, JSValueConst* argv) {
    const TSUCHI_SettingHold hold{tsuchi_js::handle(ctx, tsuchi_js::arg(argc, argv, 0), "setting_hold", false)};
    if (JS_HasException(ctx)) return JS_EXCEPTION;
    TSUCHI_Value value{};
    if (!setting_scalar(ctx, tsuchi_js::arg(argc, argv, 1), value))
        return JS_HasException(ctx) ? JS_EXCEPTION : tsuchi_js::fail(ctx, TSUCHI_INVALID);
    const auto result = tsuchi_js::host(ctx)->settings->update_hold(tsuchi_js::mod(ctx), hold, &value);
    return result == TSUCHI_OK ? JS_TRUE : tsuchi_js::fail(ctx, result);
}
JSValue tsuchi_js_datatable_get(JSContext* c, int n, JSValueConst* a) { return access(c, n, a, true, false); }
JSValue tsuchi_js_datatable_override(JSContext* c, int n, JSValueConst* a) { return access(c, n, a, true, true); }
JSValue tsuchi_js_reflection_call(JSContext* ctx, int argc, JSValueConst* argv) {
    const uint64_t id = tsuchi_js::handle(ctx, tsuchi_js::arg(argc, argv, 0), "object", false);
    String name(ctx, tsuchi_js::arg(argc, argv, 1)); JSValueConst args = tsuchi_js::arg(argc, argv, 2);
    if (JS_HasException(ctx)) return JS_EXCEPTION;
    if (!tsuchi_js::absent(args) && !JS_IsObject(args)) return JS_ThrowTypeError(ctx, "expected an object of arguments");
    ScriptWriteTree arguments;
    void* ancestors[18]{};
    TSUCHI_Result r = TSUCHI_OK;
    if (!tsuchi_js::absent(args)) {
        r = materialize(ctx, args, 0, arguments, &arguments.root, ancestors, 17);
        if (r == TSUCHI_OK && arguments.root->kind != ScriptWriteKind::structure) r = TSUCHI_INVALID;
        if (r != TSUCHI_OK) return finish(ctx, r, JS_UNDEFINED);
    }
    Target t{ctx, {id}, {}, nullptr}; auto& api = t.api(); r = api.begin_call(mod(ctx), t.object, name, &t.call);
    if (r != TSUCHI_OK) return tsuchi_js::fail(ctx, r);
    // Always end the prepared call, including a getter, conversion, or allocation failure.
    struct End { const Target& t; ~End() { t.api().end_call(mod(t.ctx), t.call); } } end{t};
    Path path;
    if (arguments.root) {
        for (uint32_t i = 0; i < arguments.root->children.count && r == TSUCHI_OK; ++i) {
            const ScriptWrite& child = *arguments.root->children[i];
            r = path.set(child.name.get()) ? apply(t, path.text, child) : TSUCHI_INVALID;
        }
    }
    if (r == TSUCHI_OK) r = api.invoke(mod(ctx), t.call);
    if (r != TSUCHI_OK) return finish(ctx, r, JS_UNDEFINED);
    JSValue returned = JS_NULL; path.set("ReturnValue"); r = get(t, path, 0, &returned);
    if (r == TSUCHI_NOT_FOUND) r = TSUCHI_OK;
    Value result(ctx, returned); Value outputs(ctx, JS_NewObject(ctx)); Buffer names(ctx); char small[256];
    for (uint32_t i = 0; r == TSUCHI_OK; ++i) {
        const char* key = read_name(names, small, sizeof small, [&](char* b, uint64_t c, uint64_t* n) { return api.call_output_at(mod(ctx), t.call, i, b, c, n); });
        if (!key) break;
        if (!strcmp(key, "ReturnValue")) continue;
        JSValue value = JS_UNDEFINED;
        if (path.set(key) && get(t, path, 0, &value) == TSUCHI_OK) JS_SetPropertyStr(ctx, outputs, key, value);
        if (JS_HasException(ctx)) return JS_EXCEPTION;
    }
    if (r != TSUCHI_OK) return finish(ctx, r, JS_UNDEFINED);
    Value tuple(ctx, JS_NewArray(ctx)); JS_SetPropertyUint32(ctx, tuple, 0, result.take()); JS_SetPropertyUint32(ctx, tuple, 1, outputs.take()); return tuple.take();
}
