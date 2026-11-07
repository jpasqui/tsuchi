"""Writes the Lua bindings: one file per service, and services_lua.cpp."""
import json

from .c_header import BANNER, c_type, returns_texts
from .c_header import arguments as passed_as
from .schema import pascal, snake_upper

INTEGERS = {'i32', 'u32', 'i64', 'u64'}


def literal(value, t):
    if t.kind == 'u64':
        return str(int(value)) + 'ULL'
    if t.kind == 'f32':
        return repr(float(value)) + 'f'
    if t.kind == 'bool':
        return 'true' if value else 'false'
    return str(int(value))


def read(value, index):
    """C++ that reads Lua argument `index` into a local named after the value."""
    t, name = value.type, value.name
    if t.kind == 'handle':
        optional = 'true' if value.optional else 'false'
        return f'const {c_type(t)} {name}{{tsuchi_lua::check_handle(L, {index}, "{t.name}", {optional})}};'
    if t.kind == 'f32':
        call = f'luaL_optnumber(L, {index}, {literal(value.default, t)})' if value.has_default else f'luaL_checknumber(L, {index})'
        return f'const float {name} = static_cast<float>({call});'
    if t.kind == 'u64':
        check = f'tsuchi_lua::check_u64(L, {index})'
        call = f'(lua_isnoneornil(L, {index}) ? {literal(value.default, t)} : {check})' if value.has_default else check
        return f'const uint64_t {name} = {call};'
    if t.kind in INTEGERS:
        low, high = {'i32': ('-0x80000000LL', '0x7FFFFFFFLL'), 'u32': ('0', '0xFFFFFFFFLL'),
                     'i64': ('LUA_MININTEGER', 'LUA_MAXINTEGER')}[t.kind]
        check = f'tsuchi_lua::check_integer(L, {index}, {low}, {high})'  # a number that does not fit is refused, not cut short
        call = f'(lua_isnoneornil(L, {index}) ? {literal(value.default, t)} : {check})' if value.has_default else check
        return f'const {c_type(t)} {name} = static_cast<{c_type(t)}>({call});'
    if t.kind == 'bool':
        if value.has_default:
            return f'const bool {name} = lua_isnoneornil(L, {index}) ? {literal(value.default, t)} : lua_toboolean(L, {index}) != 0;'
        return f'luaL_checkany(L, {index});\n    const bool {name} = lua_toboolean(L, {index}) != 0;'
    if t.kind == 'vec3':
        return f'const TSUCHI_Vec3 {name} = tsuchi_lua::check_vec3(L, {index});'
    if t.kind == 'rotation':
        return f'const TSUCHI_Rotation {name} = tsuchi_lua::check_rotation(L, {index});'
    if t.textual:  # text, and an ID: whether an ID names anything is told by the call's result, not raised
        if value.has_default:
            return f'const char* {name} = luaL_optstring(L, {index}, {json.dumps(str(value.default))});'
        return f'const char* {name} = luaL_checkstring(L, {index});'
    if t.kind == 'bytes':
        return f'size_t {name}_size = 0;\n    const char* {name} = luaL_checklstring(L, {index}, &{name}_size);'
    if t.kind == 'enum':
        default = f'"{value.default}"' if value.has_default else 'nullptr'
        return f'const TSUCHI_{t.name} {name} = {t.name}_values[luaL_checkoption(L, {index}, {default}, {t.name}_names)];'
    if t.kind == 'flags':
        return f'const TSUCHI_{t.name} {name} = tsuchi_lua::check_flags(L, {index}, {t.name}_names, {t.name}_values);'
    if t.kind == 'struct':
        return f'const TSUCHI_{t.name} {name} = read_{t.name}(L, {index});'
    raise ValueError(f'no Lua reader for {t.text}')


def push(t, expression):
    """C++ that pushes a value of type `t` onto the Lua stack."""
    if t.kind == 'handle':
        return f'tsuchi_lua::push_handle(L, "{t.name}", {expression}.id);'
    if t.kind == 'f32':
        return f'lua_pushnumber(L, {expression});'
    if t.kind == 'u64':
        return f'tsuchi_lua::push_u64(L, {expression});'
    if t.kind in INTEGERS:
        return f'lua_pushinteger(L, static_cast<lua_Integer>({expression}));'
    if t.kind == 'bool':
        return f'lua_pushboolean(L, {expression});'
    if t.textual:
        return f'lua_pushstring(L, {expression});'
    if t.kind == 'vec3':
        return f'tsuchi_lua::push_vec3(L, {expression});'
    if t.kind == 'rotation':
        return f'tsuchi_lua::push_rotation(L, {expression});'
    if t.kind == 'enum':
        return f'push_{t.name}(L, {expression});'
    if t.kind == 'flags':
        return f'tsuchi_lua::push_flags(L, {expression}, {t.name}_names, {t.name}_values);'
    if t.kind == 'struct':
        return f'push_{t.name}(L, {expression});'
    raise ValueError(f'no Lua writer for {t.text}')


def deprecated(service, name, mark):
    """C++ that tells a script's mod, once, that what it called is deprecated. The call goes on as before."""
    return f'tsuchi_lua::deprecated(L, "{service.name}.{name}", {mark.deprecated}, "{mark.use}");'


def told(service, event):
    return f'\n    {deprecated(service, "on_" + event.name, event.mark)}' if event.mark.deprecated else ''


def fields_table(fields, source):
    lines = [f'    lua_createtable(L, 0, {len(fields)});']
    for f in fields:
        lines.append(f'    {push(f.type, f"{source}{f.name}")}')
        lines.append(f'    lua_setfield(L, -2, "{f.name}");')
    return '\n'.join(lines)


def service_binding(service):
    table = 'TSUCHI_' + pascal(service.name)
    out = [BANNER.format(source=f'schema/{service.name}.yaml'), '#include "tsuchi_lua.h"\n\nnamespace {\n\n']
    closed = [enum for enum in service.enums if not enum.open]  # an open enum is an ID, a string as it is
    for item in closed + service.flags:
        names = ', '.join(f'"{name}"' for name, _, _ in item.values)
        values = ', '.join(f'TSUCHI_{snake_upper(item.name)}_{name.upper()}' for name, _, _ in item.values)
        out.append(f'const char* const {item.name}_names[] = {{{names}, nullptr}};\n')
        out.append(f'const uint32_t {item.name}_values[] = {{{values}}};\n')
    for enum in closed:
        out.append(f'''[[maybe_unused]] void push_{enum.name}(lua_State* L, uint32_t value) {{
    for (int i = 0; {enum.name}_names[i]; ++i) {{
        if ({enum.name}_values[i] != value) continue;
        lua_pushstring(L, {enum.name}_names[i]);
        return;
    }}
    tsuchi_lua::push_unnamed(L, value); // a value this version has no name for: still a string, "#" and the number
}}
''')
    for struct in service.structs:
        out.append(f'[[maybe_unused]] void push_{struct.name}(lua_State* L, const TSUCHI_{struct.name}& value) {{\n'
                   f'{fields_table(struct.fields, "value.")}\n}}\n')
    # A struct a function takes is read from a table with the same fields.
    taken = {p.type.name for fn in service.functions for p in fn.params if p.type.kind == 'struct'}
    for struct in service.structs:
        if struct.name not in taken:
            continue
        lines = [f'[[maybe_unused]] TSUCHI_{struct.name} read_{struct.name}(lua_State* L, int index) {{',
                 '    luaL_checktype(L, index, LUA_TTABLE);', f'    TSUCHI_{struct.name} out{{}};']
        for f in struct.fields:
            lines += [f'    lua_getfield(L, index, "{f.name}");', '    {',
                      '        ' + read(f, 'lua_gettop(L)').replace('\n    ', '\n        '),
                      f'        out.{f.name} = {f.name};', '    }', '    lua_pop(L, 1);']
        out.append('\n'.join(lines + ['    return out;', '}']) + '\n')
    out.append('\n')
    for fn in service.functions:
        if not service.scripted(fn):
            continue
        if service.custom(fn):  # written by hand in runtime/lua
            first = f'{deprecated(service, fn.name, fn.mark)} ' if fn.mark.deprecated else ''
            out.append(f'}} // namespace\nint tsuchi_lua_{service.name}_{fn.name}(lua_State* L);\nnamespace {{\n'
                       f'int l_{fn.name}(lua_State* L) {{ {first}return tsuchi_lua_{service.name}_{fn.name}(L); }}\n\n')
            continue
        body = [f'int l_{fn.name}(lua_State* L) {{']
        if fn.mark.deprecated:
            body.append(f'    {deprecated(service, fn.name, fn.mark)}')
        body += [f'    {read(p, i + 1)}' for i, p in enumerate(fn.params)]
        arguments = ['tsuchi_lua::mod(L)'] + [('&' if p.type.kind in ('vec3', 'rotation', 'struct') else '') + name for p in fn.params for name in passed_as(p)]
        if fn.returns and fn.returns.type.textual:
            call = f'tsuchi_lua::host(L)->{service.name}->{fn.name}({", ".join(arguments)}'
            body += ['    char small[256];', '    uint64_t needed = 0;',
                     f'    TSUCHI_Result result = {call}, small, sizeof small, &needed);',
                     '    const char* text = small;',
                     '    if (result == TSUCHI_CAPACITY) { // a buffer of the size it asked for, owned by the script state',
                     '        char* big = static_cast<char*>(lua_newuserdatauv(L, static_cast<size_t>(needed), 0));',
                     f'        result = {call}, big, needed, &needed);', '        text = big;', '    }',
                     '    if (result != TSUCHI_OK) return tsuchi_lua::fail(L, result);',
                     '    lua_pushlstring(L, text, static_cast<size_t>(needed ? needed - 1 : 0));', '    return 1;\n}\n']
            out.append('\n'.join(body) + '\n')
            continue
        if returns_texts(fn):
            call = f'tsuchi_lua::host(L)->{service.name}->{fn.name}({", ".join(arguments)}'
            body += ['    char small[1024];', '    uint64_t needed = 0;',
                     f'    TSUCHI_Result result = {call}, small, sizeof small, &needed);',
                     '    const char* texts = small;',
                     '    if (result == TSUCHI_CAPACITY) { // a buffer of the size it asked for, owned by the script state',
                     '        char* big = static_cast<char*>(lua_newuserdatauv(L, static_cast<size_t>(needed), 0));',
                     f'        result = {call}, big, needed, &needed);', '        texts = big;', '    }',
                     '    if (result != TSUCHI_OK) return tsuchi_lua::fail(L, result);',
                     '    lua_newtable(L);',
                     '    lua_Integer count = 0;',
                     '    for (const char* text = texts; *text; text += strlen(text) + 1) { // one after another, to an empty one',
                     '        lua_pushstring(L, text);', '        lua_rawseti(L, -2, ++count);', '    }', '    return 1;\n}\n']
            out.append('\n'.join(body) + '\n')
            continue
        if fn.returns and fn.returns.type.kind == 'bytes':
            # A script says how many bytes it wants at most, as one more argument.
            most = len(fn.params) + 1
            call = f'tsuchi_lua::host(L)->{service.name}->{fn.name}({", ".join(arguments)}'
            body += [f'    const lua_Integer most = luaL_checkinteger(L, {most});',
                     f'    luaL_argcheck(L, most >= 0, {most}, "a count of bytes is not below zero");',
                     '    void* buffer = lua_newuserdatauv(L, most ? static_cast<size_t>(most) : 1, 0); // owned by the script state',
                     '    uint64_t got = 0;',
                     f'    const TSUCHI_Result result = {call}, buffer, static_cast<uint64_t>(most), &got);',
                     '    if (result != TSUCHI_OK) return tsuchi_lua::fail(L, result);',
                     '    lua_pushlstring(L, static_cast<const char*>(buffer), static_cast<size_t>(got));', '    return 1;\n}\n']
            out.append('\n'.join(body) + '\n')
            continue
        if fn.returns and fn.returns.type.kind == 'list':
            element = fn.returns.type.element
            call = f'tsuchi_lua::host(L)->{service.name}->{fn.name}({", ".join(arguments)}'
            body += [f'    {c_type(element)} small[64];', '    uint64_t count = 0;',
                     f'    TSUCHI_Result result = {call}, small, 64, &count);',
                     f'    const {c_type(element)}* items = small;',
                     '    if (result == TSUCHI_CAPACITY) { // a buffer of the size it asked for, owned by the script state',
                     f'        auto* big = static_cast<{c_type(element)}*>(lua_newuserdatauv(L, static_cast<size_t>(count) * sizeof small[0], 0));',
                     f'        result = {call}, big, count, &count);', '        items = big;', '    }',
                     '    if (result != TSUCHI_OK) return tsuchi_lua::fail(L, result);',
                     '    lua_createtable(L, static_cast<int>(count), 0);',
                     '    for (uint64_t i = 0; i < count; ++i) {',
                     '        ' + push(element, 'items[i]'),
                     '        lua_rawseti(L, -2, static_cast<lua_Integer>(i) + 1);', '    }', '    return 1;\n}\n']
            out.append('\n'.join(body) + '\n')
            continue
        if fn.returns:
            body.append(f'    {c_type(fn.returns.type)} out{{}};')
            arguments.append('&out')
        body.append(f'    const TSUCHI_Result result = tsuchi_lua::host(L)->{service.name}->{fn.name}({", ".join(arguments)});')
        body.append('    if (result != TSUCHI_OK) return tsuchi_lua::fail(L, result);')
        if fn.returns and fn.returns.none_field:  # nothing there: nil, and no table
            body.append(f'    if (!out.{fn.returns.none_field}.id) {{')
            body.append('        lua_pushnil(L);\n        return 1;\n    }')
        body.append(f'    {push(fn.returns.type, "out")}' if fn.returns else '    lua_pushboolean(L, 1);')
        body.append('    return 1;\n}\n')
        out.append('\n'.join(body) + '\n')
    for event in service.events:
        struct = table + pascal(event.name)
        out.append(f'''int c_{event.name}(lua_State* L) {{
    const auto* event = static_cast<const {struct}*>(lua_touserdata(L, 1));
{fields_table(event.fields, "event->")}
    lua_call(L, 1, 0);
    return 0;
}}

void e_{event.name}(void* user, const {struct}* event) {{
    tsuchi_lua::deliver(static_cast<tsuchi_lua::Listener*>(user), c_{event.name}, event);
}}

int l_on_{event.name}(lua_State* L) {{{told(service, event)}
    tsuchi_lua::Listener* listener = tsuchi_lua::listen(L, 1);
    TSUCHI_Subscription subscription{{}};
    const TSUCHI_Result result = tsuchi_lua::host(L)->{service.name}->on_{event.name}(tsuchi_lua::mod(L), e_{event.name}, listener, &subscription);
    return tsuchi_lua::subscribed(L, listener, result, subscription);
}}

''')
    names = [fn.name for fn in service.functions if service.scripted(fn)] + [f'on_{event.name}' for event in service.events]
    rows = ''.join(f'    {{"{name}", l_{name}}},\n' for name in names)
    out.append(f'const luaL_Reg functions[] = {{\n{rows}    {{nullptr, nullptr}},\n}};\n\n}} // namespace\n\n')
    out.append(f'void tsuchi_lua_open_{service.name}(lua_State* L) {{\n    luaL_newlib(L, functions);\n'
               f'    lua_setglobal(L, "{service.name}");\n}}\n')
    return ''.join(out)


def services_binding(schema):
    out = [BANNER.format(source='schema/'), '#include "tsuchi_lua.h"\n\n']
    services = [s for s in schema.services if s.has_bindings]
    out += [f'void tsuchi_lua_open_{s.name}(lua_State* L);\n' for s in services]
    out.append('\nnamespace tsuchi_lua {\n\nconst char* result_name(TSUCHI_Result result) {\n    switch (result) {\n')
    out += [f'    case TSUCHI_{name.upper()}: return "{name}";\n' for name, _ in schema.results]
    out.append('    }\n    return "unknown";\n}\n\nvoid open_services(lua_State* L) {\n')
    out += [f'    tsuchi_lua_open_{s.name}(L);\n' for s in services]
    out.append('}\n\n} // namespace tsuchi_lua\n')
    return ''.join(out)


def generate(schema):
    files = {'lua/services_lua.cpp': services_binding(schema)}
    for service in schema.services:
        if service.has_bindings:
            files[f'lua/{service.name}_lua.cpp'] = service_binding(service)
    return files
