"""Writes the C headers: core.h, one header per service, and tsuchi.h."""
import textwrap

from .schema import pascal, snake_upper

BANNER = '/* Generated from {source}. Do not edit: change the schema and run tools/generate.py. */\n'
SCALAR_C = {'bool': 'bool', 'i32': 'int32_t', 'u32': 'uint32_t', 'i64': 'int64_t', 'u64': 'uint64_t', 'f32': 'float',
            'text': 'const char*', 'pointer': 'void*', 'value': 'TSUCHI_Value'}


def comment(text, indent=''):
    lines = []
    for paragraph in text.strip().split('\n\n'):
        lines += textwrap.wrap(' '.join(paragraph.split()), 92 - len(indent)) + ['']
    lines = lines[:-1]
    if len(lines) == 1:
        return f'{indent}/* {lines[0]} */\n'
    return f'{indent}/* ' + f'\n{indent} * '.join(lines).replace(' * \n', ' *\n') + ' */\n'


def c_type(t):
    """The C type of a value passed by value."""
    if t.kind in SCALAR_C:
        return SCALAR_C[t.kind]
    if t.kind == 'enum' and t.open:  # an ID, which is text
        return 'const char*'
    if t.kind == 'vec3':
        return 'TSUCHI_Vec3'
    if t.kind == 'rotation':
        return 'TSUCHI_Rotation'
    return 'TSUCHI_' + pascal(t.name) if t.kind == 'handle' else 'TSUCHI_' + t.name


def param(value):
    """A parameter declaration. Vectors and structs go in by pointer and bytes with their count; everything else by value."""
    if value.type.kind in ('vec3', 'rotation', 'value', 'struct'):
        return f'const {c_type(value.type)} *{value.name}'
    if value.type.kind == 'bytes':
        return f'const void *{value.name}, uint64_t {value.name}_size'
    return f'{c_type(value.type)} {value.name}'


def arguments(value):
    """The names a parameter is passed on by: one, or two for bytes."""
    return [value.name, value.name + '_size'] if value.type.kind == 'bytes' else [value.name]


def field(value):
    """A struct's field. An ID is kept in the struct itself, in room for the longest one."""
    if value.type.kind == 'enum' and value.type.open:
        return f'char {value.name}[TSUCHI_ID_SIZE]'
    return f'{c_type(value.type)} {value.name}'


def warns(mark):
    """What makes a compiler warn where a mod uses a deprecated thing, to go after its name."""
    return f' TSUCHI_DEPRECATED("use {mark.use}")' if mark.deprecated else ''


HAS_NOTE = ' A mod that also runs on a framework older than that asks TSUCHI_HAS before it uses this.'


def versions(mark, since):
    """The paragraphs a table member's description ends with: when it was added, and what to use in its place.
    `since` is the version that added it, which may be its service's."""
    out = f'\n\nAdded in version {since}.{HAS_NOTE}' if since else ''
    return out + (f'\n\n{mark.retired}' if mark.deprecated else '')


TEXT_NOTE = ('The text is copied into `buffer` as UTF-8 with a terminator, and `out` gets the bytes it takes, '
             'terminator included.')
TEXT_CAPACITY = 'The buffer is too small. Nothing is copied, and `out` still gets the size.'
LIST_NOTE = ('The list is copied into `buffer`, which has room for `capacity` of them, and `out` gets how many there '
             'are. A `capacity` of 0 with no buffer asks only how many.')
LIST_CAPACITY = 'The buffer is too small. Nothing is copied, and `out` still gets how many there are.'
TEXTS_NOTE = ('The texts are copied into `buffer` one after another as UTF-8, each with its terminator, and one more '
              'terminator follows the last. `out` gets the bytes all of that takes.')
OPEN_NOTE = ('Mods add to this set, so a value is an ID: text, in room for TSUCHI_ID_SIZE bytes at most.')
BYTES_NOTE = ('Up to `capacity` bytes are copied into `buffer`, and `out` gets how many were.')


def returns_texts(fn):
    """True when the function returns a list of texts."""
    return bool(fn.returns) and fn.returns.type.kind == 'list' and fn.returns.type.element.kind == 'text'


def out_params(fn, star=' *'):
    """The parameters a function's result comes back through. Text needs a buffer and its size."""
    if not fn.returns:
        return []
    if fn.returns.type.textual or returns_texts(fn):
        return [f'char{star}buffer', 'uint64_t capacity', f'uint64_t{star}out']
    if fn.returns.type.kind == 'bytes':
        return [f'void{star}buffer', 'uint64_t capacity', f'uint64_t{star}out']
    if fn.returns.type.kind == 'list':
        return [f'{c_type(fn.returns.type.element)}{star}buffer', 'uint64_t capacity', f'uint64_t{star}out']
    return [f'{c_type(fn.returns.type)}{star}out']


def core_header(schema):
    out = [BANNER.format(source='schema/core.yaml'),
           '#ifndef TSUCHI_CORE_H\n#define TSUCHI_CORE_H\n#include <stdbool.h>\n#include <stdint.h>\n',
           '#ifdef __cplusplus\nextern "C" {\n#endif\n\n',
           f'#define TSUCHI_API_VERSION {schema.api_version}u\n\n',
           '/* True when the table has the member and it is offered. Every table begins with how many bytes of it\n'
           '   the framework that made it has, and a later framework only ever adds to the end of a table. A mod\n'
           '   asks this before it uses anything added after the version of the framework it says it needs. It\n'
           '   works on the host table too: TSUCHI_HAS(host, actor). */\n'
           '#define TSUCHI_HAS(table, member) \\\n'
           '    ((table) != 0 && (const char *)&(table)->member + sizeof((table)->member) <= (const char *)(table) + (table)->table_size && \\\n'
           '     (table)->member != 0)\n\n',
           '/* Marks what a later version of the framework wants no longer used, with what to use in its place, so\n'
           '   that the compiler warns where a mod uses it. It still works. A build that defines TSUCHI_DEPRECATED\n'
           '   itself, as nothing, gets no such warnings. */\n'
           '#ifndef TSUCHI_DEPRECATED\n#define TSUCHI_DEPRECATED(text) __attribute__((deprecated(text)))\n#endif\n\n',
           comment('What a call reports. Anything but TSUCHI_OK means nothing changed.'),
           'typedef enum TSUCHI_Result {\n']
    for index, (name, doc) in enumerate(schema.results):
        out.append(f'    TSUCHI_{name.upper()} = {index}, /* {doc} */\n')
    out.append('} TSUCHI_Result;\n\n')
    out.append(comment('The calling mod. The framework hands one to each mod when it loads; '
                       'every call takes it, and everything the mod makes or holds is tied to it.'))
    out.append('typedef struct TSUCHI_Mod TSUCHI_Mod;\n\n')
    out.append(comment('A subscription to an event. It ends with cancel, or when the mod stops.'))
    out.append('typedef struct TSUCHI_Subscription { uint64_t id; } TSUCHI_Subscription;\n\n')
    out.append(comment('Room for the longest ID and its terminator. An ID names one of a set of kinds that mods can add to: '
                       '`<mod id>:<name>` for a thing a mod adds, a plain name for one of the game\'s own, and `#` and a number '
                       'for a number of the game\'s that has no name. Text that is not spelled as an ID fails with '
                       'TSUCHI_INVALID, and an ID that names nothing with TSUCHI_NOT_FOUND.'))
    out.append('#define TSUCHI_ID_SIZE 96\n\n')
    out.append('typedef struct TSUCHI_Vec3 { float x, y, z; } TSUCHI_Vec3;\n\n')
    out.append(comment('Which way something faces, in degrees: `pitch` tilts it up, `yaw` turns it about the vertical, and '
                       '`roll` tips it about the way it faces. With all three zero it faces along +X.'))
    out.append('typedef struct TSUCHI_Rotation { float pitch, yaw, roll; } TSUCHI_Rotation;\n\n')
    for name, doc in schema.handles:
        out.append(comment(f'{doc} Zero is no {name}.'))
        out.append(f'typedef struct TSUCHI_{pascal(name)} {{ uint64_t id; }} TSUCHI_{pascal(name)};\n\n')
    for name, _ in schema.handles:
        if name != 'object' and name in schema.object_handles:
            article = 'An' if name[0] in 'aeiou' else 'A'
            out.append(comment(f'{article} {name} is an object: the same handle, for calls that take any object.'))
            out.append(f'static inline TSUCHI_Object tsuchi_{name}_as_object(TSUCHI_{pascal(name)} {name}) {{\n'
                       f'    TSUCHI_Object object = {{{name}.id}};\n    return object;\n}}\n\n')
    out.append(comment('One value of a kind decided at run time, as reflection reads and writes them. `count` is the '
                       'length of an array, or the bytes a text takes.'))
    out.append('typedef enum TSUCHI_ValueKind {\n    TSUCHI_VALUE_NONE = 0, /* nothing: an empty object reference */\n'
               '    TSUCHI_VALUE_BOOLEAN = 1,\n    TSUCHI_VALUE_INTEGER = 2,\n    TSUCHI_VALUE_NUMBER = 3,\n'
               '    TSUCHI_VALUE_OBJECT = 4,\n    TSUCHI_VALUE_TEXT = 5,   /* read it with the text call */\n'
               '    TSUCHI_VALUE_STRUCT = 6, /* read its fields by path */\n    TSUCHI_VALUE_ARRAY = 7,  /* read its elements by path */\n'
               '} TSUCHI_ValueKind;\n'
               'typedef struct TSUCHI_Value {\n    uint32_t kind; /* TSUCHI_ValueKind */\n    uint32_t count;\n'
               '    union {\n        bool boolean;\n        int64_t integer;\n        double number;\n        TSUCHI_Object object;\n    } as;\n'
               '} TSUCHI_Value;\n\n')
    out.append('#ifdef __cplusplus\n}\n#endif\n#endif\n')
    return ''.join(out)


def service_header(service):
    guard = f'TSUCHI_{service.name.upper()}_H'
    table = 'TSUCHI_' + pascal(service.name) + 'Service'
    out = [BANNER.format(source=f'schema/{service.name}.yaml'),
           f'#ifndef {guard}\n#define {guard}\n#include "core.h"\n',
           '#ifdef __cplusplus\nextern "C" {\n#endif\n\n']
    for group in (service.enums, service.flags):
        for item in group:
            if item.open:
                # Mods add to this set, so it has no numbers here: a value is an ID, which is text.
                out.append(comment(f'{item.doc} {OPEN_NOTE}' + (' These are the game\'s own.' if item.values else '') + item.mark.notes))
                for name, _, doc in item.values:
                    out.append(f'#define TSUCHI_{snake_upper(item.name)}_{name.upper()} "{name}" /* {doc}{item.value_mark(name).notes} */\n')
                out.append('\n')
                continue
            out.append(comment(item.doc + item.mark.notes))
            out.append(f'typedef uint32_t TSUCHI_{item.name};\nenum {{\n')
            for name, value, doc in item.values:
                mark = item.value_mark(name)
                out.append(f'    TSUCHI_{snake_upper(item.name)}_{name.upper()}{warns(mark)} = {value}u, /* {doc}{mark.notes} */\n')
            out.append('};\n\n')
    for struct in service.structs:
        out.append(comment(struct.doc + struct.mark.notes))
        out.append(f'typedef struct TSUCHI_{struct.name} {{\n')
        for f in struct.fields:
            out.append(f'    {field(f)}{warns(f.mark)}; /* {f.doc}{f.mark.notes} */\n')
        out.append(f'}} TSUCHI_{struct.name};\n\n')
    for event in service.events:
        name = table[:-7] + pascal(event.name)
        out.append(comment(event.doc + event.mark.notes))
        out.append(f'typedef struct {name} {{\n')
        for f in event.fields:
            note = ' Zero when there is none.' if f.optional else ''
            out.append(f'    {c_type(f.type)} {f.name}{warns(f.mark)}; /* {f.doc}{note}{f.mark.notes} */\n')
        out.append(f'}} {name};\n')
        out.append(f'typedef void (*{name}Fn)(void *user, const {name} *event);\n\n')
    out.append(comment(f'{service.brief}{service.mark.notes}'))
    out.append(f'typedef struct {table} {{\n')
    out.append(comment('How many bytes of this table the framework that made it has. A mod built against a newer header '
                       'asks with TSUCHI_HAS before it calls a member an older framework may not have.', '    '))
    out.append('    uint32_t table_size;\n    uint32_t table_reserved;\n')
    members = {}
    for fn in service.functions:
        if not fn.native:
            continue
        doc = fn.doc
        texts = returns_texts(fn)
        text = fn.returns and fn.returns.type.textual
        listed = fn.returns and fn.returns.type.kind == 'list' and not texts
        if text:
            doc += '\n\n' + TEXT_NOTE
        if listed:
            doc += '\n\n' + LIST_NOTE
        if texts:
            doc += '\n\n' + TEXTS_NOTE
        if fn.returns and fn.returns.type.kind == 'bytes':
            doc += '\n\n' + BYTES_NOTE
        for p in fn.params:
            if p.optional:
                doc += f'\n\n`{p.name}` may be zero.'
        if fn.returns and fn.returns.none:
            empty = f'`out->{fn.returns.none_field}`' if fn.returns.none_field else '`out`'
            doc += f'\n\nTSUCHI_OK with {empty} zero: {fn.returns.none}'
        for code, when in fn.fails.items():
            doc += f'\n\nTSUCHI_{code.upper()}: {when}'
        if text or texts:
            doc += f'\n\nTSUCHI_CAPACITY: {TEXT_CAPACITY}'
        if listed:
            doc += f'\n\nTSUCHI_CAPACITY: {LIST_CAPACITY}'
        doc += versions(fn.mark, service.since(fn))
        params = ['TSUCHI_Mod *mod'] + [param(p) for p in fn.params] + out_params(fn)
        members[fn.name] = comment(doc, '    ') + f'    TSUCHI_Result (*{fn.name})({", ".join(params)}){warns(fn.mark)};\n'
    for event in service.events:
        name = table[:-7] + pascal(event.name)
        doc = f'{event.doc} The subscription ends with cancel or when the mod stops.' + versions(event.mark, service.since(event))
        members['on_' + event.name] = (comment(doc, '    ')
                                       + f'    TSUCHI_Result (*on_{event.name})(TSUCHI_Mod *mod, {name}Fn fn, void *user, TSUCHI_Subscription *out){warns(event.mark)};\n')
    # By place, as schema/abi.lock has it: a mod that is already built finds each member where it was.
    for place, member in enumerate(service.table_order):
        out.append(members[member] if member else f'    void *gone_{place}; /* what was here is no longer offered */\n')
    out.append(f'}} {table};\n\n#ifdef __cplusplus\n}}\n#endif\n#endif\n')
    return ''.join(out)


def host_header(schema):
    out = [BANNER.format(source='schema/'), '#ifndef TSUCHI_H\n#define TSUCHI_H\n#include "core.h"\n']
    out += [f'#include "{s.name}.h"\n' for s in schema.services]
    out.append('#ifdef __cplusplus\nextern "C" {\n#endif\n\n')
    out.append(comment('Everything the framework offers a mod. `api_version` is TSUCHI_API_VERSION of the '
                       'framework that is running.'))
    out.append('typedef struct TSUCHI_Host {\n    uint32_t api_version;\n'
               '    uint32_t table_size; /* how many bytes of this table the framework that made it has */\n')
    out.append(comment('Ends a subscription. TSUCHI_NOT_FOUND when it has already ended.', '    '))
    out.append('    TSUCHI_Result (*cancel)(TSUCHI_Mod *mod, TSUCHI_Subscription subscription);\n')
    for place, s in enumerate(schema.host_order):
        note = f' /*{s.mark.notes} */' if s and s.mark.notes else ''
        out.append(f'    const TSUCHI_{pascal(s.name)}Service *{s.name};{note}\n' if s else f'    const void *gone_{place};\n')
    out.append('} TSUCHI_Host;\n\n')
    out.append(comment('Where a compiled mod starts. A compiled mod is a file named `mod.nro` in the mod\'s folder, built with '
                       'the SDK. The framework loads it when the mod starts and calls this function of it once, with '
                       'everything the framework offers and the mod\'s own context; both stay good until the mod stops. '
                       'Anything but TSUCHI_OK blocks the mod, and what it made is undone. The code stays loaded for as long as '
                       'the game runs: if the mod is stopped and started again, this function is called again, with the '
                       'mod\'s global variables as they were left. The mod defines the function; TSUCHI_EXPORT lets the '
                       'framework find it.'))
    out.append('#define TSUCHI_EXPORT __attribute__((visibility("default")))\n'
               'TSUCHI_EXPORT TSUCHI_Result tsuchi_mod_start(const TSUCHI_Host *host, TSUCHI_Mod *mod);\n\n'
               '#ifdef __cplusplus\n}\n#endif\n#endif\n')
    return ''.join(out)


def generate(schema):
    """{relative path: text} for every header."""
    files = {'include/tsuchi/core.h': core_header(schema), 'include/tsuchi/tsuchi.h': host_header(schema)}
    for service in schema.services:
        files[f'include/tsuchi/{service.name}.h'] = service_header(service)
    return files
