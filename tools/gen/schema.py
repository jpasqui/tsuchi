"""Loads and checks the API schema (schema/*.yaml). Every generator reads this model."""
import re
from dataclasses import dataclass, field, replace
from pathlib import Path

import yaml

SCALARS = {'bool', 'i32', 'u32', 'i64', 'u64', 'f32', 'vec3', 'rotation', 'text', 'bytes', 'pointer', 'value', 'callback'}
SIDES = {'', 'native', 'script'}
LAYERS = {'core', 'engine', 'game'}
LIFETIMES = {'process', 'save', 'world', 'match', 'session'}
NAME = re.compile(r'^[a-z][a-z0-9]*(_[a-z0-9]+)*$')
TYPE_NAME = re.compile(r'^[A-Z][A-Za-z0-9]*$')
WRAPPED = re.compile(r'^(handle|enum|flags|struct)<([A-Za-z_][A-Za-z0-9_]*)>$')
MIN_USERS = 2
# An ID, as framework/core/ids.h reads one: a mod's ID is 1 to 47 bytes, and so is the name after its colon.
ID_PART = r'[a-z0-9](?:[a-z0-9._-]{0,45}[a-z0-9])?'
ID = re.compile(rf'^(?:{ID_PART}:{ID_PART}|[A-Za-z0-9._-]{{1,95}}|#(?:0|[1-9][0-9]{{0,9}}))$')


class SchemaError(Exception):
    pass


@dataclass
class Type:
    """A schema type: a scalar ('f32'), or a kind with a name ('handle', 'actor')."""
    kind: str
    name: str = ''
    element: object = None  # a list's element type
    open: bool = False      # an enum mods can add to: it crosses as an ID, which is text
    home: str = ''          # the service an open enum is described in, when that is another one

    @property
    def textual(self):
        """True for what crosses as text: text itself, and the ID of an open enum."""
        return self.kind == 'text' or (self.kind == 'enum' and self.open)

    @property
    def text(self):
        if self.element:
            return f'list<{self.element.text}>'
        return f'{self.kind}<{self.name}>' if self.name else self.kind


@dataclass(frozen=True)
class Mark:
    """When a thing came into the API, and when it stopped being the thing to use. See DESIGN.md, "Versions"."""
    since: int = 0       # the version of the framework that added it; 0 for what the first release has
    deprecated: int = 0  # the version from which something else is to be used; 0 while it is the thing to use
    use: str = ''        # what to use in its place, as a mod names it: `service.member`, or the bare name of a type, value or field
    use_home: str = ''   # the service the replacement is described in, when that is another one

    @property
    def added(self):
        """The sentence a header and a reference page end an entry with, or nothing."""
        return f'Added in version {self.since}.' if self.since else ''

    @property
    def retired(self):
        """The sentence that follows it for a deprecated thing, or nothing."""
        return f'Deprecated in version {self.deprecated}: use `{self.use}`.' if self.deprecated else ''

    @property
    def notes(self):
        """Both sentences, each with a space in front, to go after a description."""
        return ''.join(' ' + sentence for sentence in (self.added, self.retired) if sentence)


NO_MARK = Mark()


@dataclass
class Value:
    """A parameter, a struct field or an event field."""
    name: str
    type: Type
    doc: str
    optional: bool = False
    default: object = None
    has_default: bool = False
    none: str = ''  # for what a function returns: when there is nothing to return, which is no failure
    none_field: str = ''  # for a struct that can be none: its first field, a handle, which is then empty
    mark: Mark = NO_MARK  # a field can be deprecated; it has no `since`, since a struct never grows


@dataclass
class Function:
    name: str
    doc: str
    params: list
    returns: Value
    fails: dict
    users: list
    owns: bool = False  # the call records something against the mod, so its backend is told which mod
    only: str = ''      # 'native' or 'script' when the function exists for one kind of mod only
    mark: Mark = NO_MARK
    completes: str = ''  # in place of users: the member of its service whose family it completes
    # How a script waits for it, which changes no table: '' for a plain return, 'result' when a JavaScript
    # call gives a Promise for the result, or an event of the same service, which the Promise resolves with
    # once that event names the handle the call returned.
    waits: str = ''
    returns_outputs: bool = False  # a script also gets the arguments the function wrote, by name
    # The call works inside a hook callback: it never calls into the engine or a script runtime, so the barrier a
    # hook raises against other mod calls doesn't apply to it.
    in_hooks: bool = False

    @property
    def native(self):
        return self.only != 'script'

    @property
    def uses_values(self):
        return any(v.type.kind == 'value' for v in self.params + ([self.returns] if self.returns else []))


@dataclass
class Event:
    name: str
    doc: str
    fields: list
    users: list
    mark: Mark = NO_MARK
    completes: str = ''


@dataclass
class Named:
    """An enum, a flag set or a struct."""
    name: str
    doc: str
    values: list = field(default_factory=list)   # enums, flags: (name, value, doc)
    fields: list = field(default_factory=list)   # structs: Value
    open: bool = False  # enums: mods can add to the set, so it crosses as IDs; closed, the game's code decides it
    most: int = None    # open enums the game keeps as numbers: the largest number the game can hold for one
    mark: Mark = NO_MARK
    marks: dict = field(default_factory=dict)    # enums, flags: a value's name to its Mark, for the values that have one
    classified: bool = False  # documentation: framework categories use other rather than native-number placeholders
    only: str = ''  # structs: 'native' for one only compiled mods see, such as one with a pointer in it

    def value_mark(self, name):
        return self.marks.get(name, NO_MARK)

    @property
    def numbered(self):
        """True for an open enum the game keeps as numbers, which the framework maps IDs to and from."""
        return self.open and self.most is not None


@dataclass
class Service:
    name: str
    layer: str
    acts_on: str
    lifetime: str
    brief: str
    description: str
    enums: list
    flags: list
    structs: list
    functions: list
    events: list
    native_only: bool = False  # for compiled mods only: no script bindings
    table_order: list = field(default_factory=list)  # its table's members by place, from the lock; None is a gap
    mark: Mark = NO_MARK
    tutorials: list = field(default_factory=list)  # documentation: (title, site path)

    def since(self, item, value=''):
        """The version that added a function, an event or a type of this service, or a value of one: its own
        `since`, or failing that the `since` of what it is in. 0 for what the first release has."""
        own = item.value_mark(value).since if value else 0
        return own or item.mark.since or self.mark.since

    def scripted(self, fn):
        """True when scripts can call the function."""
        return not self.native_only and fn.only != 'native'

    def custom(self, fn):
        """True when the function's script binding is written by hand: it has no native form, or it
        passes plain values, which a script gives and gets as whatever they are."""
        return self.scripted(fn) and (fn.only == 'script' or fn.uses_values)

    @property
    def has_bindings(self):
        return not self.native_only and (self.events or any(self.scripted(fn) for fn in self.functions))


@dataclass
class Schema:
    api_version: int
    results: list     # (name, doc), ok first
    handles: list     # (name, doc)
    services: list
    object_handles: set = field(default_factory=set)
    host_order: list = field(default_factory=list)  # the services by their place in the host table; None is a gap
    call_families: dict = field(default_factory=dict)  # by service, what game/families.yaml disables: game_table.call_families


def _need(where, table, key):
    if not isinstance(table, dict) or key not in table or table[key] in (None, ''):
        raise SchemaError(f'{where}: missing "{key}"')
    return table[key]


def _name(where, text, pattern=NAME):
    if not isinstance(text, str) or not pattern.fullmatch(text):
        raise SchemaError(f'{where}: bad name "{text}"')
    return text


# Reserved identifiers are refused in every API member category. Globals depend on
# where the name appears: services must not replace a script global, and argument
# and field names must not hide Lua globals. A function such as asset.load is fine.
# Lua 5.4: https://www.lua.org/manual/5.4/manual.html#3.1 and #6
LUA_RESERVED = set("""
    and break do else elseif end false for function goto if in local nil not or
    repeat return then true until while
""".split())
LUA_GLOBALS = set("""
    _G _VERSION assert collectgarbage dofile error getmetatable ipairs load loadfile
    next pairs pcall print rawequal rawget rawlen rawset require select setmetatable
    tonumber tostring type warn xpcall coroutine debug io math os package string table utf8
""".split())
# ECMAScript ReservedWord, strict-only reserved words, and global-object properties.
# Contextual words and binding-only restrictions (arguments/eval) are not keywords.
# https://tc39.es/ecma262/multipage/ecmascript-language-lexical-grammar.html#sec-keywords-and-reserved-words
# https://tc39.es/ecma262/multipage/global-object.html (including legacy escape/unescape)
JS_RESERVED = set("""
    await break case catch class const continue debugger default delete do else enum
    export extends false finally for function if import in instanceof new null return
    super switch this throw true try typeof var void while with yield let static
    implements interface package private protected public
""".split())
JS_GLOBALS = set("""
    globalThis Infinity NaN undefined eval isFinite isNaN parseFloat parseInt
    decodeURI decodeURIComponent encodeURI encodeURIComponent escape unescape
    AggregateError Array ArrayBuffer AsyncDisposableStack BigInt BigInt64Array
    BigUint64Array Boolean DataView Date DisposableStack Error EvalError
    FinalizationRegistry Float16Array Float32Array Float64Array Function Int8Array
    Int16Array Int32Array Iterator Map Number Object Promise Proxy RangeError
    ReferenceError RegExp Set SharedArrayBuffer String SuppressedError Symbol
    SyntaxError TypeError Uint8Array Uint8ClampedArray Uint16Array Uint32Array
    URIError WeakMap WeakRef WeakSet Atomics JSON Math Reflect
""".split())
# C11/C23 and C++ keywords, including the alternative operator spellings.
# https://www.open-std.org/jtc1/sc22/wg14/www/docs/n3096.pdf section 6.4.1
# https://eel.is/c++draft/lex.key
# Ordinary library functions (time, remove, clock) are not language keywords.
C_RESERVED = set("""
    auto break case char const continue default do double else enum extern float for
    goto if inline int long register restrict return short signed sizeof static
    struct switch typedef union unsigned void volatile while alignas alignof bool
    constexpr false nullptr static_assert thread_local true typeof typeof_unqual
    _Alignas _Alignof _Atomic _BitInt _Bool _Complex _Decimal32 _Decimal64 _Decimal128
    _Generic _Imaginary _Noreturn _Static_assert _Thread_local
""".split())
CPP_RESERVED = set("""
    alignas alignof asm auto bool break case catch char char8_t char16_t char32_t class
    concept const consteval constexpr constinit const_cast continue contract_assert
    co_await co_return co_yield decltype default delete do double dynamic_cast else
    enum explicit export extern false float for friend goto if import inline int long
    module mutable namespace new noexcept nullptr operator private protected public
    register reinterpret_cast requires return short signed sizeof static static_assert
    static_cast struct switch template this thread_local throw true try typedef typeid
    typename union unsigned using virtual void volatile wchar_t while and and_eq
    bitand bitor compl not not_eq or or_eq xor xor_eq
""".split())
RESERVED = LUA_RESERVED | JS_RESERVED | C_RESERVED | CPP_RESERVED


def _member(where, text):
    if _name(where, text) in RESERVED:
        raise SchemaError(f'{where}: "{text}" is a reserved word in Lua, JavaScript, C or C++')
    return text


def _service_name(where, text):
    if _member(where, text) in LUA_GLOBALS | JS_GLOBALS:
        raise SchemaError(f'{where}: "{text}" is a built-in global in Lua or JavaScript')
    return text


def _value_name(where, text):
    if _member(where, text) in LUA_GLOBALS:
        raise SchemaError(f'{where}: "{text}" is a built-in global in Lua')
    return text


def _doc(where, table):
    text = str(_need(where, table, 'doc')).strip()
    if not text.endswith(('.', '`')):
        raise SchemaError(f'{where}: the description must be a full sentence')
    return text


def _type(where, text):
    if text in SCALARS:
        return Type(text)
    if str(text).startswith('list<') and str(text).endswith('>'):
        element = _type(where, str(text)[5:-1])
        if element.kind not in ('handle', 'text', 'vec3'):
            raise SchemaError(f'{where}: a list holds handles, texts, or vectors')
        return Type('list', element=element)
    match = WRAPPED.match(str(text))
    if not match:
        raise SchemaError(f'{where}: unknown type "{text}"')
    return Type(match.group(1), match.group(2))


def _whole(value):
    return isinstance(value, int) and not isinstance(value, bool)


def _mark(where, table, version, grows=True):
    """What a thing says of when it was added and when it was deprecated. `version` is the framework's own.
    `grows` is false for a field of a struct or an event, which has no `since`."""
    if not isinstance(table, dict) or ('since' not in table and 'deprecated' not in table):
        return NO_MARK
    since = table.get('since', 0)
    if 'since' in table:
        if not grows:
            raise SchemaError(f'{where}: a field has no "since": a struct or an event never grows, and more to tell means a '
                              'new function or event with a struct of its own (DESIGN.md, "Versions")')
        if not _whole(since) or since < 2:
            raise SchemaError(f'{where}: "since" is the version of the framework that added it, a whole number from 2 up: '
                              'what the first release has says nothing')
        if since > version:
            raise SchemaError(f'{where}: added in version {since}, and the framework is at version {version} '
                              '(core.yaml, "api_version")')
    if 'deprecated' not in table:
        return Mark(since)
    said = table['deprecated']
    if not isinstance(said, dict) or set(said) != {'in', 'use'} or not isinstance(said['use'], str) or not said['use']:
        raise SchemaError(f'{where}: "deprecated" says the version and what to use in its place: {{in: 3, use: other_name}}')
    if not _whole(said['in']) or said['in'] <= max(since, 1):
        raise SchemaError(f'{where}: "deprecated" names a version after the one that added it, and the first release is version 1')
    if said['in'] > version:
        raise SchemaError(f'{where}: deprecated in version {said["in"]}, and the framework is at version {version} '
                          '(core.yaml, "api_version")')
    return Mark(since, said['in'], said['use'])


def _value(where, table, version=0):
    name = _value_name(where, _need(where, table, 'name'))
    where = f'{where}.{name}'
    return Value(name, _type(where, _need(where, table, 'type')), _doc(where, table),
                 bool(table.get('optional', False)), table.get('default'), 'default' in table,
                 mark=_mark(where, table, version, grows=False))


def _users(where, table):
    if 'completes' in table:
        # Checked once the whole service is read: see _service.
        if table.get('users') or not isinstance(table['completes'], str):
            raise SchemaError(f'{where}: a typed call names its users, or with "completes" the one member whose family it completes')
        return []
    users = table.get('users') or []
    if len(set(users)) < MIN_USERS:
        raise SchemaError(f'{where}: a typed call names at least {MIN_USERS} users (DESIGN.md, "When a call gets a typed form")')
    return list(users)


def _named(where, table, sort, version):
    """The enums, flag sets or structs of a service. `sort` is 'enum', 'flags' or 'struct'."""
    out = []
    for name, body in (table or {}).items():
        at = f'{where}.{_name(where, name, TYPE_NAME)}'
        item = Named(name, _doc(at, body), mark=_mark(at, body, version))
        if 'classified' in body:
            if sort != 'enum' or not isinstance(body['classified'], bool):
                raise SchemaError(f'{at}: "classified" is true or false for an enum')
            item.classified = body['classified']
        if sort == 'struct':
            item.fields = [_value(at, row, version) for row in _need(at, body, 'fields')]
            item.only = body.get('only') or ''
            if item.only not in ('', 'native'):
                raise SchemaError(f'{at}: "only" is "native" for a struct')
            out.append(item)
            continue
        if sort == 'enum':
            # Every enum says which it is. See DESIGN.md, "IDs and vocabularies".
            if not isinstance(body.get('open'), bool):
                raise SchemaError(f'{at}: an enum says whether it is open: "open: true" where mods can add to the set, '
                                  '"open: false" where the game decides it')
            item.open = body['open']
        elif 'open' in body:
            raise SchemaError(f'{at}: a set of flags is always closed')
        # A closed set lists its values. An open one lists the game's own where the game keeps numbers,
        # and nothing where the game's own names are the IDs.
        rows = (body.get('values') or []) if item.open else _need(at, body, 'values')
        seen, names = set(), set()
        for row in rows:
            value_name = _member(at, _need(at, row, 'name'))
            value = _need(at, row, 'value') if 'value' in row else None
            if not isinstance(value, int) or value < 0 or value > 0xFFFFFFFF or value in seen or value_name in names:
                raise SchemaError(f'{at}.{value_name}: values are distinct whole numbers with distinct names')
            seen.add(value)
            names.add(value_name)
            item.values.append((value_name, value, _doc(f'{at}.{value_name}', row)))
            mark = _mark(f'{at}.{value_name}', row, version)
            if mark != NO_MARK:
                item.marks[value_name] = mark
        if item.open and rows:
            item.most = body.get('most')
            if not isinstance(item.most, int) or isinstance(item.most, bool) or not max(seen) <= item.most <= 0xFFFFFFFF:
                raise SchemaError(f'{at}: an open enum with values says the largest number the game can hold for one: "most"')
        elif 'most' in body:
            raise SchemaError(f'{at}: "most" is for an open enum with values')
        if item.classified and (item.open or 'other' not in names):
            raise SchemaError(f'{at}: a classified enum is closed and includes "other"')
        out.append(item)
    return out


def _service(path, version):
    raw = yaml.safe_load(path.read_text(encoding='utf-8'))
    name = _service_name(str(path), _need(str(path), raw, 'service'))
    if name != path.stem:
        raise SchemaError(f'{path}: the file is named after its service')
    service = Service(
        name=name,
        layer=_need(name, raw, 'layer'),
        acts_on=_need(name, raw, 'acts_on'),
        lifetime=_need(name, raw, 'lifetime'),
        brief=str(_need(name, raw, 'brief')).strip(),
        description=str(_need(name, raw, 'description')).strip(),
        enums=_named(f'{name}.enums', raw.get('enums'), 'enum', version),
        flags=_named(f'{name}.flags', raw.get('flags'), 'flags', version),
        structs=_named(f'{name}.structs', raw.get('structs'), 'struct', version),
        functions=[], events=[], native_only=bool(raw.get('native_only', False)), mark=_mark(name, raw, version))
    tutorials = raw.get('tutorials', [])
    if not isinstance(tutorials, list):
        raise SchemaError(f'{name}: "tutorials" is a list of title and path pairs')
    for tutorial in tutorials:
        if not isinstance(tutorial, dict) or set(tutorial) != {'title', 'path'} or not isinstance(tutorial['title'], str) or not tutorial['title'] or not isinstance(tutorial['path'], str) or not tutorial['path'].startswith('/'):
            raise SchemaError(f'{name}: each tutorial has a title and a site path')
        service.tutorials.append((tutorial['title'], tutorial['path']))
    if service.layer not in LAYERS:
        raise SchemaError(f'{name}: layer is one of {sorted(LAYERS)}')
    if service.lifetime not in LIFETIMES:
        raise SchemaError(f'{name}: lifetime is one of {sorted(LIFETIMES)}')
    if len(service.brief) > 200:
        raise SchemaError(f'{name}: the brief description is 200 characters at most (STYLE.md)')
    for row in raw.get('functions') or []:
        at = f'{name}.{_member(name, _need(name, row, "name"))}'
        returns = None
        if row.get('returns'):
            returns = Value('out', _type(at, _need(at, row['returns'], 'type')), _doc(f'{at}.returns', row['returns']))
            if 'none' in row['returns']:
                returns.none = _doc(f'{at}.returns', {'doc': row['returns']['none']})
                first = next((s.fields[0] for s in service.structs if returns.type.kind == 'struct' and s.name == returns.type.name), None)
                if returns.type.kind != 'handle' and (not first or first.type.kind != 'handle'):
                    raise SchemaError(f'{at}: "none" is for a handle, or a struct whose first field is one')
                returns.none_field = first.name if first else ''
        only = row.get('only') or ''
        if only not in SIDES or (only == 'script' and service.native_only):
            raise SchemaError(f'{at}: "only" is "native" or "script"')
        waits = row.get('waits', '')
        if waits is False or waits == 'result' or not isinstance(waits, (str, bool)):
            raise SchemaError(f'{at}: "waits" is true, or the name of the event of this service that answers the call')
        if waits is True:
            waits = 'result'
        if 'returns_outputs' in row and not isinstance(row['returns_outputs'], bool):
            raise SchemaError(f'{at}: "returns_outputs" is true or false')
        if 'in_hooks' in row and not isinstance(row['in_hooks'], bool):
            raise SchemaError(f'{at}: "in_hooks" is true or false')
        service.functions.append(Function(
            row['name'], _doc(at, row), [_value(at, p) for p in row.get('params') or []],
            returns, dict(row.get('fails') or {}), _users(at, row), bool(row.get('owns', False)), only,
            mark=_mark(at, row, version), completes=row.get('completes', ''),
            waits=waits, returns_outputs=bool(row.get('returns_outputs', False)), in_hooks=bool(row.get('in_hooks', False))))
    for row in raw.get('events') or []:
        at = f'{name}.{_member(name, _need(name, row, "name"))}'
        service.events.append(Event(row['name'], _doc(at, row),
                                    [_value(at, f, version) for f in _need(at, row, 'fields')], _users(at, row),
                                    mark=_mark(at, row, version), completes=row.get('completes', '')))
    # A member with no users of its own completes a family that has them: it names one member of that family.
    members = {member.name: member for member in service.functions + service.events}
    for member in members.values():
        if 'completes' in next(row for row in (raw.get('functions') or []) + (raw.get('events') or []) if row['name'] == member.name):
            family = members.get(member.completes)
            if not family or family is member or len(set(family.users)) < MIN_USERS:
                raise SchemaError(f'{name}.{member.name}: "completes" names a function or an event of the same service that has '
                                  f'{MIN_USERS} users of its own (DESIGN.md, "When a call gets a typed form")')
    return service


def _check_marks(services):
    """What has to hold of `since` and `deprecated` once every service is read: nothing is older than what
    it is in or what it uses, and whatever is deprecated names something that is there to be used."""
    by_name = {service.name: service for service in services}
    types = {item.name: (service, item) for service in services for item in service.enums + service.flags + service.structs}

    def older(where, since, than, what):
        if since < than:
            raise SchemaError(f'{where}: it is in the API from version {max(since, 1)}, before {what}, which was added in '
                              f'version {than}: say "since: {than}" or later')

    def replaced(where, mark, found, shown, home=''):
        """The mark with its replacement spelled as a mod names it. `found` is the replacement's own mark and
        the version that added it, or None when nothing has the name."""
        if found is None:
            raise SchemaError(f'{where}: deprecated in favor of "{mark.use}", and there is no such thing')
        other, since = found
        if other.deprecated:
            raise SchemaError(f'{where}: deprecated in favor of "{mark.use}", which is deprecated itself')
        if since > mark.deprecated:
            raise SchemaError(f'{where}: deprecated in version {mark.deprecated} in favor of "{mark.use}", which was only '
                              f'added in version {since}')
        return replace(mark, use=shown, use_home=home)

    for service in services:
        if service.mark.deprecated:
            other = by_name.get(service.mark.use)
            found = (other.mark, other.mark.since) if other and other is not service else None
            service.mark = replaced(service.name, service.mark, found, service.mark.use, service.mark.use)
        groups = [(item, 'enums') for item in service.enums] + [(item, 'flags') for item in service.flags]
        for item, group in groups + [(item, 'structs') for item in service.structs]:
            at = f'{service.name}.{group}.{item.name}'
            if item.mark.since:
                older(at, item.mark.since, service.mark.since, f'the service {service.name}')
            if item.mark.deprecated:
                home, other = types.get(item.mark.use, (None, None))
                found = (other.mark, home.since(other)) if other and other is not item else None
                item.mark = replaced(at, item.mark, found, item.mark.use, home.name if home and home is not service else '')
            for name, mark in list(item.marks.items()):
                if mark.since:
                    older(f'{at}.{name}', mark.since, service.since(item), item.name)
                if mark.deprecated:
                    there = mark.use != name and any(mark.use == value[0] for value in item.values)
                    found = (item.value_mark(mark.use), service.since(item, mark.use)) if there else None
                    item.marks[name] = replaced(f'{at}.{name}', mark, found, mark.use)
        for owner in service.structs + service.events:
            at = f'{service.name}.{"" if isinstance(owner, Event) else "structs."}{owner.name}'
            fields = {f.name: f for f in owner.fields}
            for f in owner.fields:
                if f.mark.deprecated:
                    other = fields.get(f.mark.use)
                    found = (other.mark, 0) if other and other is not f else None
                    f.mark = replaced(f'{at}.{f.name}', f.mark, found, f.mark.use)
        for member in service.functions + service.events:
            at = f'{service.name}.{member.name}'
            if member.mark.since:
                older(at, member.mark.since, service.mark.since, f'the service {service.name}')
            # What a function takes and gives, and what an event carries, was there when it was.
            values = member.fields if isinstance(member, Event) else member.params + ([member.returns] if member.returns else [])
            for value in values:
                t = value.type.element or value.type
                if t.name in types and t.kind != 'handle':
                    home, used = types[t.name]
                    older(at, service.since(member), home.since(used), t.name)
            if member.mark.deprecated:
                home_name, _, name = member.mark.use.rpartition('.')
                home = by_name.get(home_name) if home_name else service
                other = next((m for m in home.functions + home.events if m.name == name), None) if home else None
                found = (other.mark, home.since(other)) if other and other is not member else None
                member.mark = replaced(at, member.mark, found, f'{home.name}.{name}' if home else '',
                                       home.name if home and home is not service else '')


def _check_references(service, handles, shared):
    """`shared` is every open enum by name, with the service it is described in: any service may use one."""
    enums = {e.name: e for e in service.enums}
    enums.update({name: enum for name, (_, enum) in shared.items()})
    known = {'handle': set(handles), 'enum': set(enums),
             'flags': {f.name for f in service.flags}, 'struct': {s.name for s in service.structs}}
    structs = {s.name: s for s in service.structs}

    def check(where, value, allow_struct, native=False, in_function=False):
        t = value.type.element or value.type
        if value.type.element and value is not returned:
            raise SchemaError(f'{where}.{value.name}: a list is only returned')
        if t.name and t.name not in known[t.kind]:
            raise SchemaError(f'{where}.{value.name}: unknown {t.kind} "{t.name}"')
        if t.kind == 'enum':
            t.open = enums[t.name].open
            t.home = shared[t.name][0] if t.name in shared and shared[t.name][0] != service.name else ''
        if t.kind == 'pointer' and not native:
            raise SchemaError(f'{where}.{value.name}: "pointer" is for compiled mods: mark the function "only: native"')
        if t.kind == 'value' and not (native or in_function):
            raise SchemaError(f'{where}.{value.name}: a plain value is passed to a function or returned by one')
        if t.kind == 'callback' and (not in_function or value is returned):
            raise SchemaError(f'{where}.{value.name}: a callback is passed to a function')
        if t.kind == 'struct' and not allow_struct:
            raise SchemaError(f'{where}.{value.name}: a struct is passed to a function or returned by one')
        if t.kind == 'bytes' and not in_function:
            raise SchemaError(f'{where}.{value.name}: bytes are passed to a function or returned by one')
        if t.kind == 'bytes' and value.has_default:
            raise SchemaError(f'{where}.{value.name}: bytes have no default')
        if value.optional and t.kind not in ('handle', 'value'):
            raise SchemaError(f'{where}.{value.name}: only a handle or a plain value is optional; give other types a default')
        if value.has_default and t.kind == 'enum':
            # A closed enum's default is one of its values. So is an open one's when the schema lists the
            # game's own; one with no list takes any plain name, which is the game's own name for a thing.
            listed = {v[0] for v in enums[t.name].values}
            if (value.default not in listed) if listed else not (isinstance(value.default, str) and ID.match(value.default)):
                raise SchemaError(f'{where}.{value.name}: the default is not a value of {t.name}')

    returned = None
    if service.acts_on not in handles and service.acts_on != 'game':
        raise SchemaError(f'{service.name}: acts_on names a handle kind, or "game" for the game as a whole')
    for struct in service.structs:
        for f in struct.fields:
            check(f'{service.name}.{struct.name}', f, False, service.native_only or struct.only == 'native')
    for fn in service.functions:
        at = f'{service.name}.{fn.name}'
        for code in fn.fails:
            if code not in RESULT_NAMES or code == 'ok':
                raise SchemaError(f'{at}: unknown result "{code}"')
        defaulted = False
        native = service.native_only or fn.only == 'native'
        for p in fn.params:
            check(at, p, True, native, True)
            if p.type.kind == 'callback' and fn.only != 'script':
                raise SchemaError(f'{at}.{p.name}: a callback is for a function whose script binding is written by hand: mark it "only: script"')
            if p.type.kind == 'struct' and structs[p.type.name].only == 'native' and not native:
                raise SchemaError(f'{at}.{p.name}: {p.type.name} is for compiled mods: mark the function "only: native"')
            if p.type.kind == 'struct' and any(f.type.kind == 'enum' and f.type.open for f in structs[p.type.name].fields):
                raise SchemaError(f'{at}.{p.name}: a struct with an ID in it is only returned; pass the ID as an argument of its own')
            # One rule for an ID the framework does not know, in every service. See DESIGN.md, "What is not known".
            if p.type.kind == 'enum' and p.type.open and not {'invalid', 'not_found'} <= set(fn.fails):
                raise SchemaError(f'{at}.{p.name}: a function that takes an ID fails with "invalid" for text that is not '
                                  'one and with "not_found" for one that names nothing, and says so')
            if defaulted and not (p.has_default or p.optional):
                raise SchemaError(f'{at}.{p.name}: parameters that can be left out come last')
            defaulted = defaulted or p.has_default or p.optional
        if fn.returns:
            returned = fn.returns
            check(at, fn.returns, True, native, True)
            returned = None
            if (fn.returns.type.textual or fn.returns.type.kind in ('list', 'bytes')) and any(p.name in ('buffer', 'capacity') for p in fn.params):
                raise SchemaError(f'{at}: a function that returns text, bytes or a list has its own "buffer" and "capacity" in C')
        if fn.waits and not service.scripted(fn):
            raise SchemaError(f'{at}: "waits" is for a function scripts can call')
        if fn.waits and fn.waits != 'result':
            event = next((ev for ev in service.events if ev.name == fn.waits), None)
            kind = fn.returns.type if fn.returns else None
            if not event or not kind or kind.kind != 'handle' or not waited_field(fn, event):
                raise SchemaError(f'{at}: "waits" names an event of {service.name} with a field of the handle the function returns')
        if fn.returns_outputs and not service.custom(fn):
            raise SchemaError(f'{at}: "returns_outputs" is for a function whose script binding is written by hand')
    for ev in service.events:
        for f in ev.fields:
            check(f'{service.name}.{ev.name}', f, False, service.native_only)


def waited_field(fn, event):
    """The field of the event that names the handle a waiting function returned, or None."""
    kind = fn.returns.type
    return next((f for f in event.fields if f.type.kind == 'handle' and f.type.name == kind.name), None)


RESULT_NAMES = set()


def load(root):
    """Reads every file under `root` (the schema folder) and returns a checked Schema."""
    root = Path(root)
    core = yaml.safe_load((root / 'core.yaml').read_text(encoding='utf-8'))
    results = [(_member('core.results', r['name']), _doc(f'core.results.{r["name"]}', r)) for r in core['results']]
    if results[0][0] != 'ok':
        raise SchemaError('core.results: "ok" comes first')
    RESULT_NAMES.clear()
    RESULT_NAMES.update(name for name, _ in results)
    handles = [(_member('core.handles', h['name']), _doc(f'core.handles.{h["name"]}', h)) for h in core['handles']]
    names = [name for name, _ in handles]
    for handle in core["handles"]:
        if not isinstance(handle.get("object", False), bool):
            raise SchemaError(f'core.handles.{handle["name"]}: "object" is true or false')
    object_handles = {h["name"] for h in core["handles"] if h.get("object", False)}
    version = core['api_version']
    if not _whole(version) or version < 0:
        raise SchemaError('core.api_version: the version of the framework, a whole number')
    services = [_service(path, version) for path in sorted(root.glob('*.yaml')) if path.name != 'core.yaml']
    # A service's own table of functions takes its name, and so does each kind of handle.
    seen = {pascal(service.name) + "Service": f'the service {service.name}' for service in services}
    seen.update({pascal(name): f'the handle {name}' for name in names})
    for service in services:
        for item in service.enums + service.flags + service.structs:
            if item.name in seen:
                raise SchemaError(f'{service.name}.{item.name}: the name is already used by {seen[item.name]}')
            seen[item.name] = service.name
    # An open enum is text wherever it goes, so any service can take or give one. A closed enum is
    # numbers with names, which only its own service's header has.
    shared = {enum.name: (service.name, enum) for service in services for enum in service.enums if enum.open}
    for service in services:
        _check_references(service, names, shared)
    _check_marks(services)
    from . import lock  # here: the lock module reads this one
    return lock.apply(Schema(version, results, handles, services, object_handles=object_handles), lock.read(root))


def pascal(name):
    return ''.join(part.capitalize() for part in name.split('_'))


def snake_upper(name):
    """AttackKind -> ATTACK_KIND; attack_kind -> ATTACK_KIND."""
    return re.sub(r'(?<=[a-z0-9])(?=[A-Z])', '_', name).upper()
