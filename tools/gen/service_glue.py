"""Writes the framework side of each service: the table mods call, built on the core.

For every service this gives a Backend struct for the game side to fill in, one
checked function per call, and one topic per event with its subscribe function.
The hand-written part of a service is then only its backend.
"""
from .c_header import BANNER, arguments, out_params, param, returns_texts
from .schema import pascal, snake_upper


# The framework's own code for reading the game is in namespace `game`, so the service of that
# name keeps its generated code in another.
RESERVED = {'game': 'game_service'}


def space(service):
    """The C++ namespace a service's generated code is in."""
    return RESERVED.get(service.name, service.name)


def backend_param(value):
    return param(value)


def native(service):
    """The functions compiled mods can call: the ones the service's table has."""
    return [fn for fn in service.functions if fn.native]


def absent_check(service, name, families):
    """The C++ line that refuses a call of a family the running build of the game cannot serve."""
    said = families.get(service.name, {})
    numbers = [*said.get('*', []), *said.get(name, [])]
    return [' || '.join(f'family_absent({number})' for number in numbers).join(('if (', ') return TSUCHI_UNSUPPORTED;'))] if numbers else []


def checks(fn, structs=None, absent=()):
    """C++ lines that refuse bad arguments before the backend sees them."""
    lines = ['if (!mod_usable(mod)) return TSUCHI_INVALID;']
    for p in fn.params:
        t = p.type
        if t.kind == 'handle' and not p.optional:
            lines.append(f'if (!{p.name}.id) return TSUCHI_INVALID;')
        elif t.kind in ('f32', 'vec3', 'rotation'):
            lines.append(f'if (!finite({p.name})) return TSUCHI_INVALID;')
        elif t.kind in ('text', 'pointer', 'value'):
            lines.append(f'if (!{p.name}) return TSUCHI_INVALID;')
        elif t.kind == 'struct':
            lines.append(f'if (!{p.name}) return TSUCHI_INVALID;')
            for field in structs[t.name].fields:  # every number in it is a number
                if field.type.kind == 'f32':
                    lines.append(f'if (!finite({p.name}->{field.name})) return TSUCHI_INVALID;')
                elif field.type.kind in ('vec3', 'rotation'):
                    lines.append(f'if (!finite(&{p.name}->{field.name})) return TSUCHI_INVALID;')
        elif t.kind == 'bytes':
            lines.append(f'if (!{p.name} && {p.name}_size) return TSUCHI_INVALID;')
        elif t.kind == 'enum' and t.open:  # an ID: whether it names anything is the backend's to say
            lines.append(f'if (!id_valid({p.name})) return TSUCHI_INVALID;')
        elif t.kind == 'enum':
            lines.append(f'if (!valid_{t.name}({p.name})) return TSUCHI_INVALID;')
        elif t.kind == 'flags':
            lines.append(f'if ({p.name} & ~all_{t.name}) return TSUCHI_INVALID;')
    if fn.returns:
        if fn.returns.type.textual or returns_texts(fn):
            lines.append('if (!buffer) return TSUCHI_INVALID;')
        elif fn.returns.type.kind in ('list', 'bytes'):
            lines.append('if (!buffer && capacity) return TSUCHI_INVALID;')
        lines.append('if (!out) return TSUCHI_INVALID;')
    lines += absent
    lines.append(f'if (!backend || !backend->{fn.name}) return TSUCHI_UNSUPPORTED;')
    return lines


def vocabularies(service):
    """The service's open enums that the game keeps as numbers: each gets a table of IDs and numbers."""
    return [enum for enum in service.enums if enum.numbered]


def service_header(service):
    table = 'TSUCHI_' + pascal(service.name) + 'Service'
    out = [BANNER.format(source=f'schema/{service.name}.yaml'), '#pragma once\n',
           '#include "ids.h"\n' if vocabularies(service) else '',
           f'#include "tsuchi/{service.name}.h"\n\nnamespace tsuchi::{space(service)} {{\n\n']
    for enum in vocabularies(service):
        out.append(f'// {enum.doc} The game keeps one as a number, and mods add to the set: `id_value` gives the number\n'
                   '// an ID stands for and `id_of` the ID of a number, both from ids.h.\n'
                   f'extern Vocabulary {enum.name}_vocabulary;\n\n')
    out += ['// What the game side provides. Arguments are already checked: the mod is usable, required\n'
            '// handles and pointers are not zero, numbers are finite, enums and flags hold known values,\n'
            '// and an ID is spelled as one. Whether an ID names anything is for the backend to say:\n'
            '// TSUCHI_NOT_FOUND when it does not. A function that records something against the mod is\n'
            '// given the mod.\n',
            'struct Backend {\n']
    for fn in native(service):
        params = (['TSUCHI_Mod* mod'] if fn.owns else []) + [backend_param(p) for p in fn.params] + out_params(fn, '* ')
        out.append(f'    TSUCHI_Result (*{fn.name})({", ".join(params) or "void"});\n')
    out.append('};\nvoid set_backend(const Backend* backend);\n\n')
    for event in service.events:
        struct = table[:-7] + pascal(event.name)
        out.append(f'// Delivers the event to every subscriber. `{event.name}_wanted` is false when nobody listens.\n')
        out.append(f'void emit_{event.name}(const {struct}& event);\nvoid emit_{event.name}_to(TSUCHI_Mod* recipient, const {struct}& event);\nbool {event.name}_wanted();\n')
    out.append(f'\nextern const {table} table;\n\n}} // namespace tsuchi::{space(service)}\n')
    return ''.join(out)


def service_source(service, families=None):
    families = families or {}
    table = 'TSUCHI_' + pascal(service.name) + 'Service'
    out = [BANNER.format(source=f'schema/{service.name}.yaml'),
           f'#include "{service.name}_service.h"\n\n#include "families.h"\n#include "ids.h"\n#include "registry.h"\n\n',
           f'namespace tsuchi::{space(service)} {{\nnamespace {{\n\nconst Backend* backend = nullptr;\n\n']
    for enum in vocabularies(service):
        rows = ', '.join(f'{{"{name}", {value}}}' for name, value, _ in enum.values)
        out.append(f'constexpr IdName {enum.name}_own[] = {{{rows}}};\n\n')
    for enum in service.enums:
        if enum.open:
            continue
        cases = ''.join(f'    case TSUCHI_{snake_upper(enum.name)}_{name.upper()}:\n' for name, _, _ in enum.values)
        out.append(f'[[maybe_unused]] bool valid_{enum.name}(uint32_t value) {{\n    switch (value) {{\n{cases}'
                   '        return true;\n    }\n    return false;\n}\n\n')
    for flags in service.flags:
        bits = ' | '.join(f'TSUCHI_{snake_upper(flags.name)}_{name.upper()}' for name, _, _ in flags.values)
        out.append(f'[[maybe_unused]] constexpr uint32_t all_{flags.name} = {bits};\n\n')
    for event in service.events:
        struct = table[:-7] + pascal(event.name)
        out.append(f'Topic {event.name}_topic{{"{service.name}.{event.name}", [](void (*function)(), void* user, const void* event) {{\n'
                   f'    reinterpret_cast<{struct}Fn>(function)(user, static_cast<const {struct}*>(event));\n}}}};\n\n')
    for fn in native(service):
        params = ['TSUCHI_Mod* mod'] + [backend_param(p) for p in fn.params] + out_params(fn, '* ')
        passed = (['mod'] if fn.owns else []) + [name for p in fn.params for name in arguments(p)]
        passed += [p.split()[-1] for p in out_params(fn, '* ')]
        body = '\n'.join(f'    {line}' for line in checks(fn, {s.name: s for s in service.structs}, absent_check(service, fn.name, families)))
        out.append(f'TSUCHI_Result {fn.name}({", ".join(params)}) {{\n{body}\n'
                   f'    return backend->{fn.name}({", ".join(passed)});\n}}\n\n')
    for event in service.events:
        struct = table[:-7] + pascal(event.name)
        refused = ''.join(f'    {line}\n' for line in absent_check(service, 'on_' + event.name, families))
        out.append(f'TSUCHI_Result on_{event.name}(TSUCHI_Mod* mod, {struct}Fn function, void* user, TSUCHI_Subscription* out) {{\n{refused}'
                   f'    return subscribe({event.name}_topic, mod, reinterpret_cast<void (*)()>(function), user, out);\n}}\n\n')
    out.append('} // namespace\n\n')
    for enum in vocabularies(service):
        out.append(f'Vocabulary {enum.name}_vocabulary{{"{service.name}.{enum.name}", {enum.name}_own, {len(enum.values)}, {enum.most}}};\n')
    out.append('void set_backend(const Backend* next) { backend = next; }\n\n')
    for event in service.events:
        struct = table[:-7] + pascal(event.name)
        out.append(f'void emit_{event.name}(const {struct}& event) {{ emit({event.name}_topic, &event); }}\n'
                   f'void emit_{event.name}_to(TSUCHI_Mod* recipient, const {struct}& event) {{ emit_to({event.name}_topic, recipient, &event); }}\n'
                   f'bool {event.name}_wanted() {{ return subscriber_count({event.name}_topic) != 0; }}\n')
    names = [member or 'nullptr' for member in service.table_order]
    out.append(f'\nconst {table} table{{sizeof({table}), 0, {", ".join(names)}}};\n\n}} // namespace tsuchi::{space(service)}\n')
    return ''.join(out)


def host_files(schema):
    header = (BANNER.format(source='schema/') + '#pragma once\n#include "tsuchi/tsuchi.h"\n\nnamespace tsuchi {\n\n'
              '// The table handed to every mod.\nextern const TSUCHI_Host host;\n\n'
              '// A result by the name the schema gives it.\nconst char* result_name(TSUCHI_Result result);\n\n} // namespace tsuchi\n')
    names = ''.join(f'    case TSUCHI_{name.upper()}: return "{name}";\n' for name, _ in schema.results)
    includes = ''.join(f'#include "{s.name}_service.h"\n' for s in schema.services)
    tables = ''.join(f', &{space(s)}::table' if s else ', nullptr' for s in schema.host_order)
    source = (BANNER.format(source='schema/') + f'#include "host.h"\n\n{includes}#include "registry.h"\n\n'
              f'namespace tsuchi {{\n\nconst TSUCHI_Host host{{TSUCHI_API_VERSION, sizeof(TSUCHI_Host), cancel{tables}}};\n\n'
              f'const char* result_name(TSUCHI_Result result) {{\n    switch (result) {{\n{names}    }}\n    return "unknown";\n}}\n\n'
              '} // namespace tsuchi\n')
    return {'framework/host.h': header, 'framework/host.cpp': source}


def generate(schema):
    files = host_files(schema)
    for service in schema.services:
        files[f'framework/{service.name}_service.h'] = service_header(service)
        files[f'framework/{service.name}_service.cpp'] = service_source(service, schema.call_families)
    return files
