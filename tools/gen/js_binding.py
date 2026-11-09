"""Generates JavaScript bindings from the same model as Lua."""
import dataclasses
import json
from .c_header import BANNER, c_type, returns_texts, arguments as passed_as
from .schema import pascal, waited_field
from .lua_binding import literal


def read(value, e):
    t, n = value.type, value.name
    if t.textual:
        default = ', ' + json.dumps(value.default) if value.has_default else ''
        return f'tsuchi_js::String {n}(ctx, {e}{default});'
    if t.kind == 'bytes':
        return f'size_t {n}_size = 0;\n    const auto* {n} = tsuchi_js::bytes(ctx, {e}, &{n}_size);'
    calls = {'f32': 'number', 'u64': 'u64', 'i64': 'i64', 'bool': 'boolean', 'vec3': 'vec3', 'rotation': 'rotation'}
    if t.kind in calls:
        call = f'tsuchi_js::{calls[t.kind]}(ctx, {e})'
    elif t.kind in ('u32', 'i32'):
        lo, hi = ('0', 'UINT32_MAX') if t.kind == 'u32' else ('INT32_MIN', 'INT32_MAX')
        call = f'tsuchi_js::integer(ctx, {e}, {lo}, {hi})'
    elif t.kind == 'handle':
        return f'const {c_type(t)} {n}{{tsuchi_js::handle(ctx, {e}, "{t.name}", {str(value.optional).lower()})}};'
    elif t.kind in ('flags', 'enum'):
        call = f'tsuchi_js::{"flags" if t.kind == "flags" else "enumeration"}(ctx, {e}, {t.name}_names, {t.name}_values)'
    elif t.kind == 'struct':
        call = f'read_{t.name}(ctx, {e})'
    else:
        raise ValueError(t.text)
    return f'const {c_type(t)} {n} = static_cast<{c_type(t)}>({call});'


def default_value(v):
    t = v.type
    if t.kind == 'u64': return f'JS_NewBigUint64(ctx, {literal(v.default, t)})'
    if t.kind == 'i64': return f'JS_NewBigInt64(ctx, {literal(v.default, t)})'
    if t.kind == 'flags': return 'JS_NewArray(ctx)'
    if t.kind == 'bool': return 'JS_TRUE' if v.default else 'JS_FALSE'
    if t.kind == 'enum': return f'JS_NewString(ctx, {json.dumps(v.default)})'
    if t.kind == 'f32': return f'JS_NewFloat64(ctx, {literal(v.default, t)})'
    return f'JS_NewInt64(ctx, {literal(v.default, t)})'


def push(t, e):
    if t.textual: return f'JS_NewString(ctx, {e})'
    if t.kind == 'handle': return f'tsuchi_js::push_handle(ctx, "{t.name}", {e}.id)'
    if t.kind == 'u64': return f'JS_NewBigUint64(ctx, {e})'
    if t.kind == 'i64': return f'JS_NewBigInt64(ctx, {e})'
    if t.kind in ('u32', 'i32'): return f'JS_NewInt64(ctx, {e})'
    if t.kind == 'f32': return f'JS_NewFloat64(ctx, {e})'
    if t.kind == 'bool': return f'JS_NewBool(ctx, {e})'
    if t.kind in ('vec3', 'rotation'): return f'tsuchi_js::push_{t.kind}(ctx, {e})'
    if t.kind in ('enum', 'flags'): return f'tsuchi_js::push_{"enum" if t.kind == "enum" else "flags"}(ctx, {e}, {t.name}_names, {t.name}_values)'
    if t.kind == 'struct': return f'push_{t.name}(ctx, {e})'
    raise ValueError(t.text)


def messages(fn):
    cases = ''.join(f'    case TSUCHI_{name.upper()}: return {json.dumps(doc)};\n' for name, doc in fn.fails.items())
    return f'[](TSUCHI_Result r) -> const char* {{ switch (r) {{\n{cases}    default: return nullptr; }} }}(result)'


def fields(fields, source):
    return '\n'.join(f'    if (JS_SetPropertyStr(ctx, out, "{f.name}", {push(f.type, source + f.name)}) < 0) return JS_EXCEPTION;' for f in fields)


def service_binding(service):
    name = service.name
    out = [BANNER.format(source=f'schema/{name}.yaml'), '#include "tsuchi_js.h"\nnamespace {\n']
    for item in [e for e in service.enums if not e.open] + service.flags:
        out += [f'const char* const {item.name}_names[] = {{' + ', '.join(json.dumps(v) for v, _, _ in item.values) + ', nullptr};\n',
                f'const uint32_t {item.name}_values[] = {{' + ', '.join(str(v) + 'u' for _, v, _ in item.values) + '};\n']
    scripted_structs = [struct for struct in service.structs if struct.only != 'native']
    for struct in scripted_structs:
        out.append(f'[[maybe_unused]] JSValue push_{struct.name}(JSContext* ctx, const TSUCHI_{struct.name}& value) {{\n'
                   f'    tsuchi_js::Value out(ctx, JS_NewObject(ctx));\n{fields(struct.fields, "value.")}\n    return out.take();\n}}\n')
    taken = {p.type.name for fn in service.functions if service.scripted(fn) for p in fn.params if p.type.kind == 'struct'}
    for struct in scripted_structs:
        if struct.name not in taken: continue
        out.append(f'[[maybe_unused]] TSUCHI_{struct.name} read_{struct.name}(JSContext* ctx, JSValueConst record) {{\n'
                   f'    TSUCHI_{struct.name} out{{}};\n')
        for f in struct.fields:
            out.append(f'    {{ tsuchi_js::Value field(ctx, JS_GetPropertyStr(ctx, record, "{f.name}"));\n'
                       f'    {read(f, "field")}\n    out.{f.name} = {f.name}; }}\n')
        out.append('    return out;\n}\n')
    waited = [e for e in service.events if any(fn.waits == e.name and service.scripted(fn) for fn in service.functions)]
    for event in waited:
        struct = 'TSUCHI_' + pascal(name) + pascal(event.name)
        field = waited_field(next(fn for fn in service.functions if fn.waits == event.name), event)
        out.append(f'''void w_{event.name}(void* user, const {struct}* event);
TSUCHI_Result s_{event.name}(TSUCHI_Mod* mod, const TSUCHI_Host* host, void* user, TSUCHI_Subscription* subscription) {{
    return host->{name}->on_{event.name}(mod, w_{event.name}, user, subscription);
}}
const tsuchi_js::Waiter {event.name}_waiter{{s_{event.name}, "{field.type.name}", "{field.name}"}};
void w_{event.name}(void* user, const {struct}* event) {{
    JSContext* ctx = tsuchi_js::waiting(user, {event.name}_waiter, event->{field.name}.id);
    if (!ctx) return;
    tsuchi_js::Value out(ctx, JS_NewObject(ctx));
''')
        for f in event.fields:
            out.append(f'    JS_SetPropertyStr(ctx, out, "{f.name}", {push(f.type, "event->" + f.name)});\n')
        out.append(f'    tsuchi_js::settle(ctx, {event.name}_waiter, event->{field.name}.id, out.take());\n}}\n')
    for fn in service.functions:
        if not service.scripted(fn): continue
        sig = f'JSValue j_{fn.name}(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)'
        if service.custom(fn):
            out.append(f'}}\nJSValue tsuchi_js_{name}_{fn.name}(JSContext*, int, JSValueConst*);\nnamespace {{\n')
            body = [sig + ' {']
            if fn.mark.deprecated:
                body.append(f'    tsuchi_js::deprecated(ctx, "{name}.{fn.name}", {fn.mark.deprecated}, "{fn.mark.use}");')
            body += [f'    JSValue returned = tsuchi_js_{name}_{fn.name}(ctx, argc, argv);',
                     '    if (!JS_IsException(returned)) return returned;',
                     '    tsuchi_js::Value error(ctx, JS_GetException(ctx));',
                     '    tsuchi_js::Value code(ctx, JS_GetPropertyStr(ctx, error, "name"));',
                     '    tsuchi_js::String text(ctx, code);',
                     '    if (!JS_HasException(ctx)) {']
            for code, message in fn.fails.items():
                body.append(f'        if (!strcmp(text, "{code}")) JS_SetPropertyStr(ctx, error, "message", JS_NewString(ctx, {json.dumps(message)}));')
            body += ['    }', '    return JS_Throw(ctx, error.take());', '}']
            out.append('\n'.join(body) + '\n')
            continue
        body = [sig + ' {']
        if not fn.params: body.append('    (void)argc; (void)argv;')
        if fn.mark.deprecated: body.append(f'    tsuchi_js::deprecated(ctx, "{name}.{fn.name}", {fn.mark.deprecated}, "{fn.mark.use}");')
        for i, p in enumerate(fn.params):
            e = f'tsuchi_js::arg(argc, argv, {i})'
            if p.has_default and not p.type.textual:
                body.append(f'    tsuchi_js::Value default_{p.name}(ctx, tsuchi_js::absent({e}) ? {default_value(p)} : JS_DupValue(ctx, {e}));')
                p = dataclasses.replace(p, has_default=False)
                e = f'default_{p.name}'
            body.append('    ' + read(p, e))
            body.append('    if (JS_HasException(ctx)) return JS_EXCEPTION;')
        arguments = ['tsuchi_js::mod(ctx)'] + [('&' if p.type.kind in ('vec3', 'rotation', 'struct') else '') + n for p in fn.params for n in passed_as(p)]
        call = f'tsuchi_js::host(ctx)->{name}->{fn.name}({", ".join(arguments)}'
        if fn.returns and (fn.returns.type.textual or returns_texts(fn) or fn.returns.type.kind == 'list'):
            texts = returns_texts(fn)
            listed = fn.returns.type.kind == 'list' and not texts
            typ = c_type(fn.returns.type.element) if listed else 'char'
            width = 64 if listed else 1024
            body += [f'    {typ} small[{width}];', '    uint64_t count = 0;', f'    TSUCHI_Result result = {call}, small, {width}, &count);',
                     '    tsuchi_js::Buffer big(ctx);', f'    const {typ}* items = small;',
                     '    if (result == TSUCHI_CAPACITY) {', '        if (!big.resize(count, sizeof small[0])) return JS_EXCEPTION;',
                     f'        result = {call}, static_cast<{typ}*>(big.data), count, &count);', f'        items = static_cast<const {typ}*>(big.data);', '    }',
                     f'    if (result != TSUCHI_OK) return tsuchi_js::fail(ctx, result, {messages(fn)});']
            if listed:
                body += ['    tsuchi_js::Value out(ctx, JS_NewArray(ctx));', '    for (uint64_t i = 0; i < count; ++i) {',
                         f'        if (JS_SetPropertyUint32(ctx, out, static_cast<uint32_t>(i), {push(fn.returns.type.element, "items[i]")}) < 0) return JS_EXCEPTION;', '    }', '    return out.take();']
            elif texts:
                body += ['    tsuchi_js::Value out(ctx, JS_NewArray(ctx));', '    uint32_t i = 0;',
                         '    for (const char* text = items; *text; text += strlen(text) + 1)',
                         '        if (JS_SetPropertyUint32(ctx, out, i++, JS_NewString(ctx, text)) < 0) return JS_EXCEPTION;', '    return out.take();']
            else: body.append('    return JS_NewStringLen(ctx, items, static_cast<size_t>(count ? count - 1 : 0));')
        elif fn.returns and fn.returns.type.kind == 'bytes':
            body += [f'    const uint64_t most = tsuchi_js::u64(ctx, tsuchi_js::arg(argc, argv, {len(fn.params)}));',
                     '    if (JS_HasException(ctx)) return JS_EXCEPTION;', '    tsuchi_js::Buffer buffer(ctx);', '    if (!buffer.resize(most)) return JS_EXCEPTION;',
                     '    uint64_t got = 0;', f'    TSUCHI_Result result = {call}, buffer.data, most, &got);',
                     f'    if (result != TSUCHI_OK) return tsuchi_js::fail(ctx, result, {messages(fn)});',
                     '    return tsuchi_js::push_bytes(ctx, buffer.data, static_cast<size_t>(got));']
        else:
            if fn.returns:
                body.append(f'    {c_type(fn.returns.type)} out{{}};')
                arguments.append('&out')
            body += [f'    const TSUCHI_Result result = tsuchi_js::host(ctx)->{name}->{fn.name}({", ".join(arguments)});',
                     f'    if (result != TSUCHI_OK) return tsuchi_js::fail(ctx, result, {messages(fn)});']
            if fn.returns and fn.returns.none_field: body.append(f'    if (!out.{fn.returns.none_field}.id) return JS_NULL;')
            value = push(fn.returns.type, 'out') if fn.returns else 'JS_TRUE'
            if fn.waits == 'result': value = f'tsuchi_js::resolved(ctx, {value})'
            elif fn.waits: value = f'tsuchi_js::wait_for(ctx, {fn.waits}_waiter, out.id)'
            body.append(f'    return {value};')
        out.append('\n'.join(body) + '\n}\n')
    for event in service.events:
        struct = 'TSUCHI_' + pascal(name) + pascal(event.name)
        out.append(f'''void e_{event.name}(void* user, const {struct}* event) {{
    auto* listener = static_cast<tsuchi_js::Listener*>(user);
    if (!tsuchi_js::listening(listener)) return;
    JSContext* ctx = tsuchi_js::listener_context(listener);
    tsuchi_js::Value out(ctx, JS_NewObject(ctx));
''')
        for f in event.fields:
            out.append(f'    JS_SetPropertyStr(ctx, out, "{f.name}", {push(f.type, "event->" + f.name)});\n')
        out.append('    tsuchi_js::deliver(listener, out.take());\n}\n')
    if service.events:
        out.append('JSValue j_on(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {\n'
                   '    tsuchi_js::String name(ctx, tsuchi_js::arg(argc, argv, 0));\n'
                   '    if (JS_HasException(ctx)) return JS_EXCEPTION;\n')
        for event in service.events:
            out.append(f'    if (!strcmp(name, "{event.name}")) {{\n')
            if event.mark.deprecated: out.append(f'        tsuchi_js::deprecated(ctx, "{name}.{event.name}", {event.mark.deprecated}, "{event.mark.use}");\n')
            out.append(f'''        auto* listener = tsuchi_js::listen(ctx, tsuchi_js::arg(argc, argv, 1));
        if (!listener) return JS_EXCEPTION;
        TSUCHI_Subscription subscription{{}};
        const TSUCHI_Result result = tsuchi_js::host(ctx)->{name}->on_{event.name}(tsuchi_js::mod(ctx), e_{event.name}, listener, &subscription);
        return tsuchi_js::subscribed(ctx, listener, result, subscription);
    }}
''')
        out.append('    return JS_ThrowTypeError(ctx, "unknown event");\n}\n')
    out.append('}\n' + f'void tsuchi_js_open_{name}(JSContext* ctx) {{\n    tsuchi_js::Value service(ctx, JS_NewObject(ctx));\n')
    for fn in service.functions:
        if service.scripted(fn): out.append(f'    JS_SetPropertyStr(ctx, service, "{fn.name}", JS_NewCFunction(ctx, j_{fn.name}, "{fn.name}", {len(fn.params)}));\n')
    if service.events: out.append('    JS_SetPropertyStr(ctx, service, "on", JS_NewCFunction(ctx, j_on, "on", 2));\n')
    out.append(f'    tsuchi_js::Value global(ctx, JS_GetGlobalObject(ctx));\n    JS_SetPropertyStr(ctx, global, "{name}", service.take());\n}}\n')
    return ''.join(out)


def services_binding(schema):
    services = [s for s in schema.services if s.has_bindings]
    out = [BANNER.format(source='schema/'), '#include "tsuchi_js.h"\n']
    out += [f'void tsuchi_js_open_{s.name}(JSContext*);\n' for s in services]
    out.append('namespace tsuchi_js {\nconst char* result_name(TSUCHI_Result r) { switch (r) {\n')
    out += [f'case TSUCHI_{name.upper()}: return "{name}";\n' for name, _ in schema.results]
    out.append('} return "unknown"; }\nconst char* result_message(TSUCHI_Result r) { switch (r) {\n')
    out += [f'case TSUCHI_{name.upper()}: return {json.dumps(description)};\n' for name, description in schema.results]
    out.append('} return "unknown result"; }\nvoid open_services(JSContext* ctx) {\n')
    out += [f'tsuchi_js_open_{s.name}(ctx);\n' for s in services]
    out.append('}\n}\n')
    return ''.join(out)


def generate(schema):
    files = {'javascript/services_js.cpp': services_binding(schema)}
    for s in schema.services:
        if s.has_bindings: files[f'javascript/{s.name}_js.cpp'] = service_binding(s)
    return files