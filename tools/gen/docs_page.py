"""Writes one reference page per service, laid out as STYLE.md describes."""
import json
import re

from .c_header import out_params, param
from .schema import pascal, waited_field

WORDS = {'f32': 'number', 'i32': 'integer', 'u32': 'integer', 'i64': 'integer', 'u64': 'integer', 'bool': 'boolean',
         'vec3': 'vector', 'rotation': 'rotation', 'text': 'text', 'bytes': 'bytes', 'pointer': 'pointer', 'value': 'value',
         'callback': 'function'}
NOTE = '{/* Generated from schema/%s.yaml. Do not edit: change the schema and run tools/generate.py. */}\n'


# What every enum's entry says about the set it is. DESIGN.md, "IDs and vocabularies", has the rules.
OPEN_SET = ('This set is open, so mods can add to it. A value is an ID, which is text in Lua, JavaScript, and C. A value that a '
            'mod adds is `<mod id>:<name>`, and one of the game\'s own is a plain name. Text that isn\'t a valid ID fails '
            'with `invalid`, and an ID that doesn\'t refer to anything fails with `not_found`. In C, an ID is at most '
            '`TSUCHI_ID_SIZE` bytes. ')
OPEN_NUMBERED = ('The game\'s own values are listed here. A value that Tsuchi has no name for is returned as `#` followed '
                 'by the game\'s number for it, like `#8`, and you can pass it back in the same format.')
OPEN_NAMED = 'The game\'s own values use the game\'s names for them.'
CLOSED_SET = ('This set is closed, so mods can\'t add to it. A value from the game that isn\'t listed is returned as `#` '
              'followed by its number in Lua and JavaScript, like `#12`, and as the number itself in C. You can only pass the listed values.')


def prose(text):
    """Schema text as MDX: braces and angle brackets outside code spans are escaped."""
    parts = re.split(r'(`[^`]*`)', ' '.join(text.split()))
    return ''.join(p if p.startswith('`') else re.sub(r'([{}<>])', r'\\\1', p) for p in parts)


def paragraphs(text):
    """A paragraph's lines are joined into one. A table keeps its lines, as each one is a row."""
    def one(paragraph):
        lines = [line.strip() for line in paragraph.strip().split('\n')]
        if all(line.startswith('|') for line in lines):
            return '\n'.join(prose(line) for line in lines)
        return prose(paragraph)
    return '\n\n'.join(one(p) for p in text.strip().split('\n\n'))


def type_text(t):
    if t.kind in WORDS:
        return WORDS[t.kind]
    if t.kind == 'handle':
        return f'{t.name} handle'
    if t.kind == 'list':
        return f'list of {type_text(t.element)}s'
    if t.home:  # an open enum another service describes
        return f'[`{t.name}`](/reference/{t.home}/#{t.name.lower()})'
    return f'[`{t.name}`](#{t.name.lower()})'


def lua_default(value):
    if value.type.kind in ('enum', 'text'):
        return f'"{value.default}"'
    if value.type.kind == 'flags':
        return '{}'
    return str(value.default).lower() if isinstance(value.default, bool) else str(value.default)


def lua_signature(service, fn):
    params = [p.name + (f' = {lua_default(p)}' if p.has_default else '') for p in fn.params]
    if fn.returns and fn.returns.type.kind == 'bytes':
        params.append('most')  # how many bytes the script wants at most
    return f'{service.name}.{fn.name}({", ".join(params)})'


def c_signature(fn):
    params = ['TSUCHI_Mod *mod'] + [param(p) for p in fn.params] + out_params(fn)
    return f'TSUCHI_Result {fn.name}({", ".join(params)});'


def tabs(lua, c, javascript=None):
    if lua is None:
        return '```c\n' + c + '\n```\n'
    entries = [('Lua', 'lua', lua)]
    if javascript is not None: entries.append(('JavaScript', 'javascript', javascript))
    if c is not None: entries.append(('C', 'c', c))
    return '<Tabs syncKey="language">\n' + ''.join(
        f'<TabItem label="{label}">\n\n```{language}\n{code}\n```\n\n</TabItem>\n'
        for label, language, code in entries) + '</Tabs>\n'


def js_signature(service, fn):
    params = [p.name for p in fn.params]
    if fn.returns and fn.returns.type.kind == 'bytes': params.append('most')
    prefix = 'await ' if fn.waits else ''
    return prefix + f'{service.name}.{fn.name}({", ".join(params)})'


def versions(mark, since=0, anchor=None):
    """What an entry ends with: the version that added the thing, when that is not the first release, and for a
    deprecated thing the version that said so and what to use in its place. `since` is the version that added
    it, which may be its service's. `anchor` turns a replacement's name into where its entry is on its page."""
    out = [f'Added in version {since or mark.since}.'] if since or mark.since else []
    if mark.deprecated:
        place = (f'/reference/{mark.use_home}/' if mark.use_home else '') + (f'#{anchor(mark.use)}' if anchor else '')
        out.append(f'Deprecated in version {mark.deprecated}: use ' + (f'[`{mark.use}`]({place}).' if place else f'`{mark.use}`.'))
    return ' '.join(out)


def member_anchor(use):
    """A function or an event is named `service.member`, and its entry's heading is the member."""
    return use.rpartition('.')[2]


def value_table(values, first='Name'):
    rows = [f'| {first} | Type | Description |', '| --- | --- | --- |']
    for v in values:
        doc = prose(v.doc) + (' Can be `nil` in Lua or `null` in JavaScript.' if v.optional else '') + (' ' + versions(v.mark) if v.mark.deprecated else '')
        rows.append(f'| `{v.name}` | {type_text(v.type)} | {doc} |')
    return '\n'.join(rows) + '\n'


def service_page(service):
    table = 'TSUCHI_' + pascal(service.name)
    # The description is written as JSON, which YAML reads as it stands: a brief may hold a colon or a quote.
    out = ['---\n', f'title: {service.name}\n', f'description: {json.dumps(service.brief, ensure_ascii=False)}\n', '---\n\n',
           "import { Tabs, TabItem } from '@astrojs/starlight/components';\n\n",
           NOTE % service.name, '\n',
           f'{prose(service.brief)}\n\n## Description\n\n{paragraphs(service.description)}\n\n']
    if service.mark.since or service.mark.deprecated:
        out.append(versions(service.mark, anchor=lambda use: '') + '\n\n')
    if service.native_only:
        out.append('This service is only for compiled mods. You can\'t use it from Lua or JavaScript.\n\n')
    out.append('## Tutorials\n\n')
    for title, path in service.tutorials or [('Developer Guides', '/develop/')]:
        out.append(f'- [{title}]({path})\n')
    out.append('\n')
    if service.functions:
        out.append('## Functions\n\n| Function | Does |\n| --- | --- |\n')
        for fn in service.functions:
            first = prose(fn.doc.strip().split('\n\n')[0].split('. ')[0].rstrip('.') + '.')
            out.append(f'| [`{fn.name}`](#{fn.name}) | {first}{" Deprecated." if fn.mark.deprecated else ""} |\n')
        out.append('\n')
    if service.events:
        out.append('## Events\n\n| Event | Emitted |\n| --- | --- |\n')
        for event in service.events:
            when = prose(event.doc.strip().split('\n\n')[0].split('. ')[0].rstrip('.') + '.').removeprefix('Emitted ')
            out.append(f'| [`{event.name}`](#{event.name}) | {when[0].upper()}{when[1:]}{" Deprecated." if event.mark.deprecated else ""} |\n')
        out.append('\n')
    if service.enums or service.flags or service.structs:
        out.append('## Types\n\n| Type | Is |\n| --- | --- |\n')
        for item in service.structs + service.enums + service.flags:
            out.append(f'| [`{item.name}`](#{item.name.lower()}) | {prose(item.doc)}{" Deprecated." if item.mark.deprecated else ""} |\n')
        out.append('\n')

    if service.functions:
        out.append('## Function descriptions\n\n')
    for fn in service.functions:
        lua = lua_signature(service, fn) if service.scripted(fn) else None
        out.append(f'### {fn.name}\n\n{tabs(lua, c_signature(fn) if fn.native else None, js_signature(service, fn) if service.scripted(fn) else None)}\n{paragraphs(fn.doc)}\n\n')
        if not service.native_only and not service.scripted(fn):
            out.append('This function is only for compiled mods.\n\n')
        if not fn.native:
            out.append('This function is only for script mods.\n\n')
        if service.scripted(fn):
            out.append('In JavaScript, failures throw an `Error` whose `name` is the result code and whose `message` is the failure description.\n\n')
            if fn.waits == 'result':
                out.append('In JavaScript, this returns a Promise. Use `await` to get the result. The call finishes before '
                           'it returns, so the Promise is already resolved.\n\n')
            elif fn.waits:
                field = waited_field(fn, next(e for e in service.events if e.name == fn.waits))
                out.append(f'In JavaScript, this returns a Promise. Use `await` to get the `{fn.waits}` event for this call. '
                           f'The Promise also carries the handle as `{field.name}`.\n\n')
            if fn.returns_outputs:
                out.append('In JavaScript, this returns `[result, outputs]`, with `null` when there is no result.\n\n')
        if fn.params:
            out.append(value_table(fn.params, 'Argument') + '\n')
        for code, when in fn.fails.items():
            out.append(f'Fails with `{code}`: {prose(when)}\n\n')
        if fn.returns:
            what = type_text(fn.returns.type)
            detail = '' if fn.name.startswith(('get_', 'set_')) else ' ' + prose(fn.returns.doc)
            article = '' if what in ('text', 'bytes') else 'an ' if what[0] in 'aeiou' else 'a '
            language = 'In Lua and C, returns' if fn.waits and fn.waits != 'result' else 'Returns'
            out.append(f'{language} {article}{what}.{detail}\n\n')
            if fn.returns.none:
                empty = 'an empty handle' if fn.returns.type.kind == 'handle' else 'a zeroed struct'
                out.append(f'This can return nothing, which isn\'t a failure. {prose(fn.returns.none)} '
                           f'In that case, you get only `nil` in Lua, `null` in JavaScript, and {empty} with `ok` in C.\n\n')
            texts = fn.returns.type.kind == 'list' and fn.returns.type.element.kind == 'text'
            if texts and fn.native:
                out.append('In C, the texts are copied into `buffer` one after another as UTF-8, each followed by a null '
                           'terminator, with one more null terminator after the last text. `out` receives the total size in '
                           'bytes. If the buffer is too small, the call fails with `capacity`, copies nothing, and still '
                           'sets the size.\n\n')
            if what == 'bytes':
                out.append('Lua receives a byte string, and JavaScript receives a `Uint8Array`.\n\n')
                out.append('In Lua and JavaScript, you pass one more argument, `most`, which is the maximum number of bytes that you want. '
                           + ('In C, up to `capacity` bytes are copied into `buffer`, and `out` receives the number of bytes '
                              'that were copied.' if fn.native else '')
                           + '\n\n')
            if fn.returns.type.kind == 'list' and fn.native and not texts:
                out.append('In C, the list is copied into `buffer`, which has room for `capacity` elements, and `out` '
                           'receives the number of elements. If the buffer is too small, the call fails with `capacity`, '
                           'copies nothing, and still sets the count.\n\n')
            if fn.returns.type.textual and fn.native:
                out.append('In C, the text is copied into `buffer` as UTF-8 with a null terminator, and `out` receives '
                           'its size in bytes. If the buffer is too small, the call fails with `capacity`, copies nothing, '
                           'and still sets the size.\n\n')
        if service.since(fn) or fn.mark.deprecated:
            out.append(versions(fn.mark, service.since(fn), member_anchor) + '\n\n')

    if service.events:
        out.append('## Event descriptions\n\n')
    for event in service.events:
        lua = f'{service.name}.on_{event.name}(function(event) ... end)'
        c = f'void callback(void *user, const {table}{pascal(event.name)} *event);'
        javascript = f'{service.name}.on("{event.name}", (event) => {{ /* ... */ }});'
        out.append(f'### {event.name}\n\n{tabs(lua, c, javascript)}\n{prose(event.doc)}\n\n{value_table(event.fields, "Field")}\n')
        out.append('The subscription ends when you cancel it or when your mod stops.\n\n')
        if service.since(event) or event.mark.deprecated:
            out.append(versions(event.mark, service.since(event), member_anchor) + '\n\n')

    if service.enums or service.flags or service.structs:
        out.append('## Type descriptions\n\n')
    for struct in service.structs:
        out.append(f'### {struct.name}\n\n{prose(struct.doc)}\n\n{value_table(struct.fields, "Field")}\n')
        if service.since(struct) or struct.mark.deprecated:
            out.append(versions(struct.mark, service.since(struct), str.lower) + '\n\n')
    for item in service.enums + service.flags:
        out.append(f'### {item.name}\n\n{prose(item.doc)}\n\n')
        if item.open:
            out.append(OPEN_SET + (OPEN_NUMBERED if item.values else OPEN_NAMED) + '\n\n')
        elif item in service.enums:
            if item.classified:
                out.append('Tsuchi sorts the game\'s values into these categories, so mods can\'t add to them. A value '
                           'that fits none of the categories is returned as `other`.\n\n')
            else:
                out.append(CLOSED_SET + '\n\n')
        if item.values:
            out.append('| Value | Meaning |\n| --- | --- |\n')
            for name, _, doc in item.values:
                mark = item.value_mark(name)
                out.append(f'| `{name}` | {prose(doc)}{" " + versions(mark) if mark.since or mark.deprecated else ""} |\n')
            out.append('\n')
        if service.since(item) or item.mark.deprecated:
            out.append(versions(item.mark, service.since(item), str.lower) + '\n\n')
    return ''.join(out).rstrip() + '\n'


def generate(schema):
    return {f'docs/reference/{service.name}.mdx': service_page(service) for service in schema.services}
