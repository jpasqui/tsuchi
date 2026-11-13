"""Turns the game table (one YAML file) into game_table.h.

The table is the only place an address or an offset is written down. Its parts:
`identity` (how to recognize this game), `data` (addresses of the game's
globals), `constants`, `structs` (sizes and field offsets), `functions`
(addresses with their signatures), `slots` (entries in the game's function
tables), `code_sites` (places in the game's code that a patch redirects
into the framework), `assets` (the paths of assets of the game's own that
the framework loads), `rows` (lists of names of rows in its tables), and `names` (what the game calls its own classes, stages
and levels, where the framework has to spell one).

A function the framework calls or hooks has its signature here, as `returns`
and `args`, so that the code names it and never casts by hand. That goes for
`functions`, and for the four other ways a function is reached: an indirect function
stored in `data`, an entry of
an object's own function table, which is a struct's field written as
`{at: 0x430, returns: bool, args: [pointer, u64]}`; a slot or a slot group;
and a code site. The generated header gives each a type that carries the
signature, and the helpers in game.h and hooks.h take the function from it.

An entry that only the checks' own mods read goes in game/fixtures.yaml instead. `load` adds
it when given that file, and the framework's header is generated without it.

Several builds of the game share one table. game/table.yaml has every name and signature and
the first build's values. A file game/builds/<key>.yaml has another build's values for the same
names, and may mark an entry `absent`. Every number becomes one cell of a list the framework
selects when it starts, read through an accessor that reads the same as a constant in code and
cannot be assigned. The few numbers the compiler needs are listed under `fixed` in the table,
and stay constants that have to be the same in every build. DESIGN.md, "Several builds of the
game", has the decision and the rules.
"""
import re
from pathlib import Path

import yaml

from . import stubs
from .schema import NAME, SchemaError

BANNER = '// Generated from {source}. Do not edit: change the table and run tools/generate.py.\n'
KEYWORDS = {'class', 'struct', 'union', 'enum', 'namespace', 'template', 'this', 'new', 'delete', 'default',
            'switch', 'case', 'float', 'double', 'int', 'long', 'short', 'char', 'bool', 'void', 'register',
            'operator', 'private', 'public', 'protected', 'virtual', 'static', 'const', 'volatile', 'export'}
C_TYPES = {'void': 'void', 'bool': 'bool', 'u8': 'uint8_t', 'i32': 'int32_t', 'u32': 'uint32_t', 'i64': 'int64_t', 'u64': 'uint64_t',
           'f32': 'float', 'f64': 'double', 'pointer': 'void*', 'const_pointer': 'const void*',
           'engine_text': '::tsuchi::game::EngineText', 'physics_pose': '::tsuchi::game::PhysicsPose',
           'engine_array': '::tsuchi::game::EngineArray',
           'text': 'const char*', 'text16': 'const char16_t*'}
SIGNED = {'returns', 'args'}
ABSENT = 'absent'                    # how a build file writes an entry that build does not have
ABSENT_VALUE = 0x0FFF0000FFFF0000    # the number of one: added to an address, it is an address nothing can read, and cut to 32 bits it is not 0
BY_VALUE = {'engine_text', 'physics_pose', 'engine_array'}  # returned by value: a missing function is null, not a stand-in
BUILD_KEY = re.compile(r'[a-z0-9][a-z0-9.-]{0,23}')
MOST_FAMILIES = 64


def identifier(name):
    return name + '_' if name in KEYWORDS else name


def number(where, value):
    if not isinstance(value, int) or isinstance(value, bool) or value < 0:
        raise SchemaError(f'{where}: expected a whole number, got {value!r}')
    return value


def names(where, table):
    table = table or {}
    for name in table:
        if not isinstance(name, str) or not NAME.match(name):
            raise SchemaError(f'{where}: bad name "{name}"')
    return table


def c_type(where, text):
    if not isinstance(text, str) or text not in C_TYPES:
        raise SchemaError(f'{where}: unknown type "{text}" (one of {", ".join(C_TYPES)})')
    return C_TYPES[text]


def signature(where, body):
    """The type of the function something leads to, when the table says what it returns and takes: `R (*)(A, B)`.
    None when it says neither."""
    if not SIGNED & set(body):
        return None
    args = body.get('args', [])
    if not isinstance(args, list):
        raise SchemaError(f'{where}: "args" is a list of types')
    return f'{c_type(where, body.get("returns", "void"))} (*)({", ".join(c_type(where, arg) for arg in args)})'


FIXTURE_GROUPS = ('data', 'constants', 'assets', 'structs', 'functions')


def with_fixtures(raw, path):
    """The table with the entries of a fixture table (game/fixtures.yaml) added. A fixture table
    adds names and never changes one the game table has."""
    extra = yaml.safe_load(Path(path).read_text(encoding='utf-8')) or {}
    merged = dict(raw)
    for group, entries in extra.items():
        if group not in FIXTURE_GROUPS:
            raise SchemaError(f'{path}: a fixture table has only {", ".join(FIXTURE_GROUPS)}, not "{group}"')
        combined = dict(raw.get(group) or {})
        for name, value in (entries or {}).items():
            if name in combined:
                raise SchemaError(f'{path}: {group}.{name} is already in the game table')
            combined[name] = value
        merged[group] = combined
    return merged


def merged_number(at, value):
    """A build's number for an entry the base table has. Every entry is stated: an address carried
    over from another build unnoticed is the one mistake this format exists to prevent."""
    if value == ABSENT:
        return ABSENT
    if isinstance(value, bool) or not isinstance(value, int) or value < 0:
        raise SchemaError(f'{at}: a build gives a whole number or "absent", not {value!r}')
    return value


def same_names(at, base, given):
    """Refuses a build that adds a name or leaves one out."""
    if not isinstance(given, dict):
        raise SchemaError(f'{at}: expected the entries by name')
    extra, missing = set(given) - set(base), set(base) - set(given)
    if extra:
        raise SchemaError(f'{at}.{sorted(extra)[0]}: a build changes values and never adds a name')
    if missing:
        raise SchemaError(f'{at}.{sorted(missing)[0]}: the build does not say; give the value or "absent"')


def over_function(at, entry, value):
    """An entry that keeps its signature from the base table and takes its number from a build."""
    return {**entry, 'at': merged_number(at, value)} if isinstance(entry, dict) else merged_number(at, value)


def with_build(raw, build, where):
    """The base table with a build file's values in place of its own. Names and signatures stay as
    they are; the result is checked by `load_raw` as the base table is."""
    if not isinstance(build, dict):
        raise SchemaError(f'{where}: a build file is a map')
    allowed = {'version', 'build_id', 'identity', 'data', 'constants', 'structs', 'functions', 'slots', 'tables',
               'slot_groups', 'code_sites', 'conditional_sites', 'native_routes', 'route_frames', 'names',
               'patch_space', 'patch_checked'}
    if set(build) - allowed:
        raise SchemaError(f'{where}: a build file has no "{sorted(set(build) - allowed)[0]}"')
    for key in ('version', 'build_id', 'identity'):
        if not build.get(key):
            raise SchemaError(f'{where}: missing "{key}"')
    # One span can be the same in another program, as the start of the code often is.
    if not isinstance(build['identity'], list) or len(build['identity']) < 2:
        raise SchemaError(f'{where}: identity lists at least two spans')
    fixed = fixed_entries(raw)
    merged = {key: value for key, value in raw.items()}
    merged['version'], merged['build_id'], merged['identity'] = str(build['version']), str(build['build_id']), build['identity']
    for group in ('data', 'constants'):
        base = raw.get(group) or {}
        given = dict(build.get(group) or {})
        for name in base:
            if f'{group}.{name}' in fixed:
                given.setdefault(name, base[name])
        same_names(f'{where}: {group}', base, given)
        merged[group] = {name: over_function(f'{where}: {group}.{name}', base[name], given[name]) for name in base}
    structs = {}
    given_structs = dict(build.get('structs') or {})
    for name, body in (raw.get('structs') or {}).items():
        if f'structs.{name}' in fixed:
            given_structs.setdefault(name, {key: body[key] for key in ('size', 'fields') if key in body})
        elif not body.get('fields') and 'size' not in body:
            given_structs.setdefault(name, {})
    same_names(f'{where}: structs', raw.get('structs') or {}, given_structs)
    for name, body in (raw.get('structs') or {}).items():
        at, said = f'{where}: structs.{name}', given_structs[name]
        if not isinstance(said, dict) or set(said) - {'size', 'fields'}:
            raise SchemaError(f'{at}: a build gives a struct\'s size and fields')
        if ('size' in body) != ('size' in said):
            raise SchemaError(f'{at}: the size is {"missing" if "size" in body else "not in the base table"}')
        fields = body.get('fields') or {}
        same_names(f'{at}.fields', fields, said.get('fields') or {})
        structs[name] = {**body, 'fields': {field: over_function(f'{at}.{field}', fields[field], said['fields'][field]) for field in fields}}
        if 'size' in body:
            structs[name]['size'] = merged_number(f'{at}.size', said['size'])
    merged['structs'] = structs
    base = raw.get('functions') or {}
    same_names(f'{where}: functions', base, build.get('functions') or {})
    merged['functions'] = {name: {**base[name], 'address': merged_number(f'{where}: functions.{name}', build['functions'][name])} for name in base}
    base = raw.get('slots') or {}
    same_names(f'{where}: slots', base, build.get('slots') or {})
    merged['slots'] = {}
    for name, body in base.items():
        said, at = build['slots'][name], f'{where}: slots.{name}'
        if said == ABSENT:
            merged['slots'][name] = {**body, 'table': ABSENT, 'offset': ABSENT, 'stock': ABSENT}
            continue
        if not isinstance(said, dict) or set(said) != {'table', 'offset', 'stock'}:
            raise SchemaError(f'{at}: a build gives table, offset and stock, or "absent"')
        merged['slots'][name] = {**body, **{key: merged_number(f'{at}.{key}', said[key]) for key in said}}
    base = raw.get('tables') or {}
    same_names(f'{where}: tables', base, build.get('tables') or {})
    merged['tables'] = {}
    for name, rows in base.items():
        said, at = build['tables'][name], f'{where}: tables.{name}'
        if not isinstance(said, list) or len(said) != len(rows):
            raise SchemaError(f'{at}: a build gives a list of the same length, {len(rows)}')
        merged['tables'][name] = [merged_number(at, value) for value in said]
    base = raw.get('slot_groups') or {}
    same_names(f'{where}: slot_groups', base, build.get('slot_groups') or {})
    merged['slot_groups'] = {}
    for name, body in base.items():
        said, at = build['slot_groups'][name], f'{where}: slot_groups.{name}'
        if said == ABSENT:
            merged['slot_groups'][name] = {**body, 'offset': ABSENT, 'stock': [ABSENT] * len(body['stock'])}
            continue
        if not isinstance(said, dict) or set(said) != {'offset', 'stock'} or not isinstance(said['stock'], list) or len(said['stock']) != len(body['stock']):
            raise SchemaError(f'{at}: a build gives offset and a stock list of the same length, {len(body["stock"])}, or "absent"')
        merged['slot_groups'][name] = {**body, 'offset': merged_number(at, said['offset']), 'stock': [merged_number(at, value) for value in said['stock']]}
    for group, numbers, optional in (('code_sites', {'address', 'instruction'}, {'call'}),
                                     ('conditional_sites', {'address', 'instruction', 'next_instruction', 'register', 'threshold'}, {'admit'}),
                                     ('native_routes', {'address', 'instruction'}, {'call', 'exits', 'resume'})):
        base = raw.get(group) or {}
        same_names(f'{where}: {group}', base, build.get(group) or {})
        merged[group] = {}
        for name, body in base.items():
            said, at = build[group][name], f'{where}: {group}.{name}'
            if said == ABSENT:
                merged[group][name] = {**body, 'absent': True}
                continue
            if group in ('native_routes', 'code_sites') and isinstance(said, dict) and set(said) == {'same_as'}:
                # The base's shape (a route's exits, a site's signature) stays; the place is the other entry's.
                merged[group][name] = {'same_as': said['same_as'],
                                       **{key: value for key, value in body.items() if key not in ('address', 'instruction', 'call', 'same_as')}}
                continue
            body = {key: value for key, value in body.items() if key != 'same_as'}
            if not isinstance(said, dict) or set(said) - numbers - optional or numbers - set(said):
                raise SchemaError(f'{at}: a build gives {", ".join(sorted(numbers))}, or "absent"')
            if 'exits' in body or 'exits' in said:
                same_names(f'{at}.exits', body.get('exits') or {}, said.get('exits') or {})
            merged[group][name] = {**body, **said}
    base = raw.get('route_frames') or {}
    same_names(f'{where}: route_frames', base, build.get('route_frames') or {})
    merged['route_frames'] = {}
    for name, fields in base.items():
        said, at = build['route_frames'][name], f'{where}: route_frames.{name}'
        if said == ABSENT:
            said = {field: ABSENT for field in fields}
        same_names(at, fields, said)
        merged['route_frames'][name] = {field: merged_number(f'{at}.{field}', said[field]) for field in fields}
    own_names = set(raw.get('build_names') or [])
    if set(build.get('names') or {}) != own_names:
        raise SchemaError(f'{where}: names gives exactly {sorted(own_names)}, the names the table lets a build change')
    merged['names'] = {**(raw.get('names') or {}), **(build.get('names') or {})}
    for key in ('patch_space', 'patch_checked'):
        merged.pop(key, None)
        if key in build:
            merged[key] = build[key]
    return merged


def fixed_entries(raw):
    """The entries the table says the compiler needs: `constants.x` and `structs.x`, each the same in every build."""
    fixed = raw.get('fixed') or {}
    if not isinstance(fixed, dict) or set(fixed) - {'constants', 'structs'}:
        raise SchemaError('fixed: lists constants and structs')
    out = set()
    for group, entries in fixed.items():
        for name in entries or []:
            if name not in (raw.get(group) or {}):
                raise SchemaError(f'fixed.{group}: {name} is not in the table')
            out.add(f'{group}.{name}')
    return out


def load(path, fixtures=None, build=None):
    """The table at `path`. With `fixtures`, the entries only the checks' own mods read are added. With
    `build`, the path of a build file, the table as that build has it."""
    raw = yaml.safe_load(Path(path).read_text(encoding='utf-8'))
    if fixtures is not None:
        raw = with_fixtures(raw, fixtures)
    if build is None:
        return load_raw(raw, path)
    return load_raw(with_build(raw, yaml.safe_load(Path(build).read_text(encoding='utf-8')), Path(build).name), path, Path(build).stem)


def load_builds(path, fixtures=None):
    """Every build the table at `path` knows of, the base table's first and then game/builds/ by name."""
    folder = Path(path).parent / 'builds'
    builds = [load(path, fixtures)]
    for file in sorted(folder.glob('*.yaml')) if folder.is_dir() else []:
        if not BUILD_KEY.fullmatch(file.stem):
            raise SchemaError(f'{file.name}: a build\'s key is lower-case letters, digits, "." and "-", at most 24')
        builds.append(load(path, fixtures, file))
    seen = {}
    for table in builds:
        # A mod's manifest can name a build by its version, so one that names 1.2.0 has to mean one program.
        for what, value in (('key', table['key']), ('build', table['build_id'].upper()), ('version', table['version'])):
            if seen.get(value, table['key']) != table['key']:
                raise SchemaError(f'build {table["key"]}: its {what} is the one {seen[value]} has')
            seen[value] = table['key']
    # The first build whose spans all match is selected, so no build's spans can be among another's.
    for at, table in enumerate(builds):
        for other in builds[at + 1:]:
            one, two = set(table['identity']), set(other['identity'])
            if one <= two or two <= one:
                raise SchemaError(f'build {other["key"]}: its identity cannot be told from that of {table["key"]}')
    return builds


def load_raw(raw, path, build_key=None):
    """The table in `raw`, checked. `build_key` says it is a build's. A build may hold absent entries; the base table
    only absent globals, structure fields and functions, for what only a later build of the game has."""
    for key in ('game', 'version'):
        if not raw.get(key):
            raise SchemaError(f'{path}: missing "{key}"')
    table = {'game': str(raw['game']), 'version': str(raw['version']),
             'data': {}, 'constants': {}, 'structs': {}, 'functions': {}, 'slots': {}}
    table['key'] = build_key or str(raw.get('key', raw['version']))
    table['absent'] = []
    table['fixed'] = fixed_entries(raw)
    table['build_names'] = list(raw.get('build_names') or [])
    if not BUILD_KEY.fullmatch(table['key']):
        raise SchemaError(f'{path}: a build\'s key is lower-case letters, digits, "." and "-", at most 24')

    def amount(at, value):
        """A number, or the number of an entry this build does not have."""
        if value != ABSENT:
            return number(at, value)
        # The first build can lack a global, a structure's field or a function a later build has, as the Switch 2
        # Edition's Dandori Academy. Everything else a later build has needs a change of format first.
        if build_key is None and not at.startswith(('data.', 'structs.', 'functions.')):
            raise SchemaError(f'{at}: only a build file can mark an entry absent, but for a global, a structure field or a function')
        if at.startswith('constants.') and at.split('.')[1] not in (raw.get('address_constants') or ()):
            raise SchemaError(f'{at} cannot be absent: only a constant under address_constants can')
        table['absent'].append(at)
        return ABSENT_VALUE

    table['data_signatures'] = {}
    for group in ('data', 'constants'):
        for name, value in names(group, raw.get(group)).items():
            at = f'{group}.{name}'
            if group == 'data' and isinstance(value, dict):
                if not set(value) <= {'at'} | SIGNED or not SIGNED & set(value):
                    raise SchemaError(f'{at}: an indirect function gives its address and signature')
                table['data_signatures'][name] = signature(at, value)
                value = value.get('at')
            table[group][name] = amount(at, value)
    for name, body in names('structs', raw.get('structs')).items():
        fields, signatures = {}, {}
        for field, offset in names(f'structs.{name}', body.get('fields')).items():
            at = f'structs.{name}.{field}'
            if isinstance(offset, dict):
                # An entry of a function table: where it is in the table, and what the function there returns and takes.
                if not set(offset) <= {'at'} | SIGNED or not SIGNED & set(offset):
                    raise SchemaError(f'{at}: an entry of a function table is written {{at: 0x10, returns: bool, args: [pointer]}}')
                signatures[field] = signature(at, offset)
                offset = offset.get('at')
            fields[field] = amount(at, offset)
        size = amount(f'structs.{name}.size', body['size']) if 'size' in body else None
        if size is not None and size != ABSENT_VALUE and any(offset >= size and offset != ABSENT_VALUE for offset in fields.values()):
            raise SchemaError(f'structs.{name}: a field lies outside the size')
        if 'size' in fields:
            raise SchemaError(f'structs.{name}: "size" is not a field name')
        table['structs'][name] = {'size': size, 'fields': fields, 'signatures': signatures}
    for name, body in names('functions', raw.get('functions')).items():
        at = f'functions.{name}'
        table['functions'][name] = {
            'address': amount(at, body.get('address')),
            'returns': c_type(at, body.get('returns', 'void')),
            'args': [c_type(at, arg) for arg in body.get('args') or []]}
    for name, body in names('slots', raw.get('slots')).items():
        at = f'slots.{name}'
        table['slots'][name] = {'table': amount(at, body.get('table')), 'offset': amount(at, body.get('offset')),
                                'stock': amount(at, body.get('stock')), 'signature': signature(at, body)}
    table['assets'] = {}
    for name, path in names('assets', raw.get('assets')).items():
        if not isinstance(path, str) or not re.fullmatch(r'/[\w./-]+', path):
            raise SchemaError(f'assets.{name}: the path of an asset, beginning with "/"')
        table['assets'][name] = path
    table['rows'] = {}
    for name, body in names('rows', raw.get('rows')).items():
        if not isinstance(body, list) or not body or not all(isinstance(row, str) and re.fullmatch(r'[\w.-]+', row) for row in body):
            raise SchemaError(f'rows.{name}: a list of the names of rows')
        table['rows'][name] = list(body)
    table['names'] = {}
    for name, text in names('names', raw.get('names')).items():
        if not isinstance(text, str) or not re.fullmatch(r'[\w/-]+', text):
            raise SchemaError(f'names.{name}: a name of the game\'s, in letters, digits, "_", "-" and "/"')
        table['names'][name] = text
    table['tables'] = {}
    for name, body in names('tables', raw.get('tables')).items():
        if not isinstance(body, list) or not body:
            raise SchemaError(f'tables.{name}: a list of addresses')
        # A class another build has and this one lacks leaves a gap in a list. The rest of the list stays good, and
        # what walks the list passes over a gap: only a list with nothing left in it is an entry that is gone.
        if build_key is not None and ABSENT in body and any(address != ABSENT for address in body):
            table['tables'][name] = [ABSENT_VALUE if address == ABSENT else number(f'tables.{name}', address) for address in body]
        else:
            table['tables'][name] = [amount(f'tables.{name}', address) for address in body]
    table['slot_groups'] = {}
    for name, body in names('slot_groups', raw.get('slot_groups')).items():
        at = f'slot_groups.{name}'
        if body.get('tables') not in table['tables']:
            raise SchemaError(f'{at}: "tables" names a list under tables')
        stock = body.get('stock')
        if not isinstance(stock, list) or not stock:
            raise SchemaError(f'{at}: "stock" lists the functions the game puts in the entry')
        table['slot_groups'][name] = {'tables': body['tables'], 'offset': amount(at, body.get('offset')),
                                      'stock': [amount(at, address) for address in stock], 'signature': signature(at, body)}
    table['build_id'] = str(raw.get('build_id', ''))
    table['patch_space'] = raw.get('patch_space')
    table['patch_checked'] = raw.get('patch_checked')
    table['identity'] = [(number('identity.offset', span.get('offset')), number('identity.hash', span.get('hash')))
                         for span in raw.get('identity') or []]
    table['code_sites'] = {}
    for name, body in names('code_sites', raw.get('code_sites')).items():
        at = f'code_sites.{name}'
        if body.get('absent'):
            if build_key is None:
                raise SchemaError(f'{at}: only a build file can mark an entry absent')
            table['absent'].append(at)
            table['code_sites'][name] = {'absent': True, 'address': ABSENT_VALUE, 'instruction': 0, 'call': False, 'signature': signature(at, body)}
            continue
        if 'same_as' in body:
            # A function this build reaches through another site's: that site sees it, and this one is not installed.
            other = (raw.get('code_sites') or {}).get(body['same_as'])
            if not isinstance(other, dict) or 'same_as' in other:
                raise SchemaError(f'{at}: same_as names another code site of this table, one with a place of its own')
            table['code_sites'][name] = {'absent': True, 'same_as': body['same_as'], 'address': ABSENT_VALUE, 'instruction': 0,
                                         'call': False, 'signature': signature(at, body)}
            continue
        site = {'address': number(at, body.get('address')), 'instruction': number(at, body.get('instruction')),
                'call': bool(body.get('call', False)), 'signature': signature(at, body)}
        if site['address'] % 4 or site['instruction'] >> 32:
            raise SchemaError(f'{at}: an address is a multiple of 4 and an instruction is 32 bits')
        if site['call']:
            try:
                site['target'] = stubs.call_target(site['address'], site['instruction'])
            except ValueError as error:
                raise SchemaError(f'{at}: {error}') from None
        elif stubs.is_position_dependent(site['instruction']) and stubs.moved_page(site['address'], site['instruction']) is None:
            raise SchemaError(f'{at}: the instruction there cannot be moved; pick the function entry or mark it `call`')
        table['code_sites'][name] = site
    table['conditional_sites'] = {}
    for name, body in names('conditional_sites', raw.get('conditional_sites')).items():
        at = f'conditional_sites.{name}'
        if isinstance(body, dict) and body.get('absent'):
            if build_key is None:
                raise SchemaError(f'{at}: only a build file can mark an entry absent')
            table['absent'].append(at)
            table['conditional_sites'][name] = {'absent': True, 'address': ABSENT_VALUE, 'instruction': 0, 'next_instruction': 0, 'register': 0, 'threshold': 0, 'admit': []}
            continue
        if not isinstance(body, dict) or set(body) != {'address', 'instruction', 'next_instruction', 'register', 'threshold', 'admit'}:
            raise SchemaError(f'{at}: give address, instruction, next_instruction, register, threshold and admit')
        site = {key: number(at + '.' + key, body[key]) for key in ('address', 'instruction', 'next_instruction', 'register', 'threshold')}
        admitted = body['admit']
        if (site['address'] % 4 or site['register'] > 30 or site['threshold'] > 4095 or
                site['instruction'] != (0x7100001F | (site['threshold'] << 10) | (site['register'] << 5))):
            raise SchemaError(f'{at}: instruction must be CMP w<register>, #<threshold> without a shift')
        if not isinstance(admitted, list) or not admitted or len(admitted) > 8:
            raise SchemaError(f'{at}: admit is a bounded list of one to eight values')
        admitted = [number(at + '.admit', value) for value in admitted]
        if len(set(admitted)) != len(admitted) or any(value < site['threshold'] or value >= 4095 for value in admitted):
            raise SchemaError(f'{at}: distinct admitted values range from the threshold through 4094')
        if site['next_instruction'] >> 32 or (site['next_instruction'] & 0xFF00001F) != 0x5400000A:
            raise SchemaError(f'{at}: the next instruction must be B.GE')
        site['admit'] = admitted
        table['conditional_sites'][name] = site
    table['native_routes'] = {}
    for name, body in names('native_routes', raw.get('native_routes')).items():
        at = f'native_routes.{name}'
        if isinstance(body, dict) and body.get('absent'):
            if build_key is None:
                raise SchemaError(f'{at}: only a build file can mark an entry absent')
            table['absent'].append(at)
            # Its place in the block stays, so that the block's layout has the same list in every build.
            table['native_routes'][name] = {'absent': True, 'address': ABSENT_VALUE, 'instruction': 0xD503201F, 'call': False,
                                            'has_resume': body.get('resume', True), 'exits': {key: 0 for key in body.get('exits') or {}}}
            continue
        if isinstance(body, dict) and 'same_as' in body:
            # A place the build's code has only once, where another route already is: that route serves both, and this
            # one is not installed. It is not missing from the build, so it is not listed absent.
            other = (raw.get('native_routes') or {}).get(body['same_as'])
            if set(body) - {'same_as', 'exits', 'resume'} or not isinstance(other, dict) or 'same_as' in other:
                raise SchemaError(f'{at}: same_as names another route of this table, one with a place of its own')
            table['native_routes'][name] = {'absent': True, 'same_as': body['same_as'], 'address': ABSENT_VALUE,
                                            'instruction': 0xD503201F, 'call': False, 'has_resume': body.get('resume', True),
                                            'exits': {key: 0 for key in body.get('exits') or {}}}
            continue
        if not isinstance(body, dict) or not set(body) <= {'address', 'instruction', 'call', 'exits', 'resume'}:
            raise SchemaError(f'{at}: a native route gives address, instruction, and optionally call and exits')
        site = {'address': number(at, body.get('address')), 'instruction': number(at, body.get('instruction'))}
        if site['address'] % 4 or site['instruction'] >> 32:
            raise SchemaError(f'{at}: an address is a multiple of 4 and an instruction is 32 bits')
        site['call'] = ((site['instruction'] & 0xFC000000) == 0x94000000 or
                        (site['instruction'] & 0xFFFFFC1F) == 0xD63F0000)
        if 'call' in body and (not isinstance(body['call'], bool) or body['call'] != site['call']):
            raise SchemaError(f'{at}: call must agree with the displaced BL or BLR instruction')
        if (site['instruction'] & 0xFFFFFC1F) == 0xD63F0000 and (site['instruction'] >> 5 & 31) == 30:
            raise SchemaError(f'{at}: BLR x30 cannot reuse LR for the native call target')
        if 'resume' in body and not isinstance(body['resume'], bool):
            raise SchemaError(f'{at}.resume: expected a boolean')
        site['has_resume'] = body.get('resume', True)
        site['exits'] = {}
        if 'exits' in body and not isinstance(body['exits'], dict):
            raise SchemaError(f'{at}.exits: a map of names to native code offsets')
        for exit_name, target in names(f'{at}.exits', body.get('exits')).items():
            target = number(f'{at}.exits.{exit_name}', target)
            if not target or target % 4:
                raise SchemaError(f'{at}.exits.{exit_name}: a nonzero multiple of 4')
            site['exits'][exit_name] = target
        table['native_routes'][name] = site
    exit_names = set()
    for route, site in table['native_routes'].items():
        for name in site['exits']:
            flat = identifier(route + '_' + name)
            if flat in exit_names:
                raise SchemaError(f'native_routes.{route}.exits.{name}: the generated exit name is already used')
            exit_names.add(flat)
    if table['native_routes'] or table['constants'].get('dynamic_hook_slots', 0):
        offset = table['constants'].get('native_jump_offset')
        if offset is None or offset < stubs.STATUS_SITE_JUMP + 8 or offset % 8 or offset > 32760:
            raise SchemaError('native routes need aligned constants.native_jump_offset after the ordinary hook entry')
    table['route_frames'] = {}
    for name, fields in names('route_frames', raw.get('route_frames')).items():
        table['route_frames'][name] = {field: amount(f'route_frames.{name}.{field}', value)
                                       for field, value in names(f'route_frames.{name}', fields).items()}
    check_route_frames(table, raw)
    addresses = set()
    for group in ('code_sites', 'native_routes', 'conditional_sites'):
        for name, site in table[group].items():
            if site.get('absent'):
                continue
            if site['address'] in addresses:
                raise SchemaError(f'{group}.{name}: a code address has more than one patch owner')
            addresses.add(site['address'])
    protected = {site['address'] + 4 for site in table['conditional_sites'].values() if not site.get('absent')}
    if addresses & protected:
        raise SchemaError('a patch site overlaps the protected B.GE after a conditional site')
    # What identifies a build is read with the patch already in the game's code, and a site's word is patched.
    for offset, _ in table['identity']:
        if any(offset - 3 <= word < offset + 16 for word in addresses | protected):
            raise SchemaError(f'identity: the span at {offset:#x} overlaps a patch site')
    if table['code_sites'] or table['native_routes'] or table['conditional_sites'] or table['constants'].get('dynamic_hook_slots', 0):
        for key in ('stub_base', 'stub_end'):
            if key not in table['constants']:
                raise SchemaError(f'code_sites need constants.{key}')
        if table['constants']['stub_base'] % (4 if table['patch_space'] is not None else 8) or table['constants']['stub_end'] % 4:
            raise SchemaError('stub_base is aligned to 8 bytes and stub_end to 4 bytes')
        if table['constants']['stub_end'] < table['constants']['stub_base']:
            raise SchemaError('stub_end comes before stub_base')
        if any(table['constants']['stub_base'] <= address < table['constants']['stub_end'] for address in protected):
            raise SchemaError('a protected B.GE overlaps the patch block')
        for group in ('code_sites', 'native_routes', 'conditional_sites'):
            for name, site in table[group].items():
                if not site.get('absent') and table['constants']['stub_base'] <= site['address'] < table['constants']['stub_end']:
                    raise SchemaError(f'{group}.{name}: a code site overlaps the patch block')
        check_patch_space(table, protected)
        try:
            stubs.place(table)
            stubs.assembly(table, 0)
        except ValueError as error:
            raise SchemaError(str(error)) from None
    code_size = table['constants'].get('code_size')
    if code_size is not None:
        # A span past the end of the code would have the framework read memory that is not there as it starts.
        for offset, _ in table['identity']:
            if offset + 16 > code_size:
                raise SchemaError(f'identity: the span at {offset:#x} lies outside the code, which ends at {code_size:#x}')
    seen = {}
    for group in ('data', 'functions'):
        for name, entry in table[group].items():
            address = entry if group == 'data' else entry['address']
            if address == ABSENT_VALUE:
                continue
            if address in seen:
                raise SchemaError(f'{group}.{name}: the address is already listed as {seen[address]}')
            seen[address] = f'{group}.{name}'
    return table


def check_route_frames(table, raw):
    """A handler indexes the registers it is given by these, so a register has to be one of x0 to x30 (or, for a field
    that can name either kind, v0 to v31 as 32 to 63), and an absent number leaves nothing to index with unless the
    route is gone too or its handler asks whether the number is there."""
    offsets = set(raw.get('route_frame_offsets') or [])
    either = set(raw.get('route_frame_either') or [])
    optional = set(raw.get('route_frame_optional') or [])
    for name, fields in table['route_frames'].items():
        route = table['native_routes'].get(name) or table['code_sites'].get(name)
        for field, value in fields.items():
            at = f'route_frames.{name}.{field}'
            if value == ABSENT_VALUE:
                if f'{name}.{field}' not in optional and (route is None or not route.get('absent')):
                    raise SchemaError(f'{at}: a route that stays needs its frame; mark the route absent too')
            elif f'{name}.{field}' in offsets:
                if value >= 0x1000 or value % 4:
                    raise SchemaError(f'{at}: an offset from the stack is a multiple of 4 below 0x1000')
            elif f'{name}.{field}' in either:
                if value > 63 or value == 31:
                    raise SchemaError(f'{at}: a register is x0 to x30 as 0 through 30, or v0 to v31 as 32 through 63')
            elif value > 30:
                raise SchemaError(f'{at}: a register is a number from 0 through 30')


def check_patch_space(table, protected):
    """Refuses a list of free places the table alone shows to be wrong. tools/patch_space.py tests them against the program."""
    space = table['patch_space']
    if space is None:
        return
    if not isinstance(space, list):
        raise SchemaError('patch_space: a list of [address, bytes]')
    constants, previous = table['constants'], 0
    taken = {site['address'] for group in ('code_sites', 'native_routes', 'conditional_sites')
             for site in table[group].values() if not site.get('absent')} | protected
    for place in space:
        if not isinstance(place, list) or len(place) != 2 or not all(type(value) is int for value in place):
            raise SchemaError('patch_space: a list of [address, bytes]')
        address, size = place
        at = f'patch_space: the place at {address:#x}'
        if address % 4 or size % 4 or size <= 0:
            raise SchemaError(f'{at}: an address and a size are multiples of 4')
        if address < previous:
            raise SchemaError(f'{at}: places are in order and do not overlap')
        previous = address + size
        code_size = constants.get('code_size')
        if code_size is not None and code_size != ABSENT_VALUE and address + size > code_size:
            raise SchemaError(f'{at}: it lies outside the code')
        if address < constants['stub_end'] and constants['stub_base'] < address + size:
            raise SchemaError(f'{at}: it overlaps the spare space after the code, which is used anyway')
        if any(address <= word < address + size for word in taken):
            raise SchemaError(f'{at}: it overlaps a patch site')
        # What identifies a build is read with the patch already in the game's code.
        if any(address < offset + 16 and offset < address + size for offset, _ in table['identity']):
            raise SchemaError(f'{at}: it overlaps an identity span')


def flatten(table):
    """{path: number} for every number of a build that can differ from another's, in a fixed order. Paths
    are the same for every build, since a build cannot add a name."""
    cells = {}
    fixed = table['fixed']
    for name, value in table['data'].items():
        cells[f'data.{name}'] = value
    for name, value in table['constants'].items():
        if f'constants.{name}' not in fixed:
            cells[f'constants.{name}'] = value
    for name, body in table['structs'].items():
        if f'structs.{name}' in fixed:
            continue
        if body['size'] is not None:
            cells[f'structs.{name}.size'] = body['size']
        for field, offset in body['fields'].items():
            cells[f'structs.{name}.{field}'] = offset
    for name, fields in table['route_frames'].items():
        for field, value in fields.items():
            cells[f'route_frames.{name}.{field}'] = value
    for name, f in table['functions'].items():
        cells[f'functions.{name}'] = f['address']
    for name, s in table['slots'].items():
        for key in ('table', 'offset', 'stock'):
            cells[f'slots.{name}.{key}'] = s[key]
    for name, addresses in table['tables'].items():
        for index, address in enumerate(addresses):
            cells[f'tables.{name}.{index}'] = address
    for name, group in table['slot_groups'].items():
        cells[f'slot_groups.{name}.offset'] = group['offset']
        for index, address in enumerate(group['stock']):
            cells[f'slot_groups.{name}.stock.{index}'] = address
    for name, s in table['code_sites'].items():
        cells[f'code_sites.{name}.address'] = s['address']
        cells[f'code_sites.{name}.instruction'] = s['instruction']
        cells[f'code_sites.{name}.call'] = int(s['call'])
        cells[f'code_sites.{name}.entry'] = s['entry']
        cells[f'code_sites.{name}.original'] = s['original']
    for name, s in table['conditional_sites'].items():
        for key in ('address', 'entry', 'instruction', 'next_instruction'):
            cells[f'conditional_sites.{name}.{key}'] = s[key]
    for name, s in table['native_routes'].items():
        cells[f'native_routes.{name}.address'] = s['address']
        cells[f'native_routes.{name}.instruction'] = s['instruction']
        cells[f'native_routes.{name}.call'] = int(s['call'])
        for key in ('entry', 'returned', 'original', 'resume'):
            cells[f'native_routes.{name}.{key}'] = s[key]
        for exit_name, entry in s['exit_entries'].items():
            cells[f'native_routes.{name}.exits.{exit_name}.entry'] = entry
            cells[f'native_routes.{name}.exits.{exit_name}.target'] = s['exits'][exit_name]
    cells['stub.patch_hash'] = stubs.patch_hash(table)
    for name, value in stubs.cells(table).items():
        cells[f'stub.{name}'] = value
    return cells


def compare_builds(builds, cells):
    """Refuses builds that cannot share one module."""
    first = builds[0]
    for table, flat in zip(builds[1:], cells[1:]):
        at = f'build {table["key"]}'
        if list(flat) != list(cells[0]):
            raise SchemaError(f'{at}: its entries differ from the base table\'s')
        for entry in first['fixed']:
            group, name = entry.split('.')
            if table[group][name] != first[group][name]:
                raise SchemaError(f'{at}: {entry} is fixed, and this build has another value; list it per build')
        if table['constants'].get('stub_base', 0) and not first['constants'].get('stub_base', 0):
            raise SchemaError(f'{at}: stub_base differs in kind from the base table')
        code_size = table['constants'].get('code_size')
        if code_size is not None and code_size != ABSENT_VALUE:
            for name, f in table['functions'].items():
                if f['address'] != ABSENT_VALUE and f['address'] >= code_size:
                    raise SchemaError(f'{at}: functions.{name} lies past the end of the code')
            for group in ('code_sites', 'native_routes', 'conditional_sites'):
                for name, site in table[group].items():
                    if not site.get('absent') and site['address'] >= code_size:
                        raise SchemaError(f'{at}: {group}.{name} lies past the end of the code')


# What in the framework's code stands for an entry: the namespace it is reached by, and its name.
def scan_key(path):
    """The (namespace, name) the framework's code spells an entry by."""
    parts = path.split('.')
    group = parts[0]
    if group == 'structs':
        return (parts[1], parts[2])
    if group == 'route_frames':
        return ('route_frame', parts[1])
    return ({'functions': 'fn', 'constants': 'constant', 'slots': 'slot', 'slot_groups': 'slot_group', 'code_sites': 'code_site',
             'native_routes': 'native_route', 'conditional_sites': 'conditional_site', 'tables': 'tables', 'data': 'data'}[group], parts[1])


def scan_uses(folder, table):
    """{(namespace, name): {file}} for what each file of the framework's code spells from the table."""
    structs = set(table['structs'])
    groups = {'data', 'constant', 'slot', 'slot_group', 'tables', 'fn', 'code_site', 'native_route', 'route_frame'}
    uses = {}
    for file in sorted(Path(folder).rglob('*')):
        if file.suffix not in ('.cpp', '.h'):
            continue
        text = re.sub(r'//[^\n]*', '', file.read_text(encoding='utf-8', errors='replace'))
        relative = file.relative_to(folder).as_posix()
        declared = [(match.start(), match.group(1), match.group(2)) for match in re.finditer(r'namespace\s+(\w+)\s*=\s*([\w:]+)\s*;', text)]
        aliases = {alias: target for _, alias, target in declared}
        roots = {'game::table', 'tsuchi::game::table', 'table'} | {alias for alias, target in aliases.items() if target.split('::')[-1] == 'table'}
        pattern = r'(?<![\w.>])(?:' + '|'.join(sorted(map(re.escape, roots), key=len, reverse=True)) + r')::(\w+)(?:::(\w+))?'
        # A name that is a keyword of C++ is spelled with an underscore after it (see identifier).
        spelled = lambda name: name[:-1] if name and name.endswith('_') and name[:-1] in KEYWORDS else name
        for match in re.finditer(pattern, text):
            uses.setdefault((spelled(match.group(1)), spelled(match.group(2))), set()).add(relative)
            if match.group(1) == 'most':
                uses.setdefault(('constant', match.group(2)), set()).add(relative)
        # A file can bind one alias again in another function, so each use belongs to the binding before it.
        for at, (start, alias, target) in enumerate(declared):
            tail = target.split('::')[-1]
            if tail in structs or tail in groups:
                following = [other for other, name, _ in declared[at + 1:] if name == alias]
                for match in re.finditer(r'(?<![\w:.>])' + alias + r'::(\w+)', text[start:following[0] if following else len(text)]):
                    uses.setdefault((tail, spelled(match.group(1))), set()).add(relative)
    return uses


def load_families(path, table):
    """The API families of game/families.yaml, with the entries of each worked out from the framework's code."""
    file = Path(path).parent / 'families.yaml'
    if not file.is_file():
        return None
    raw = yaml.safe_load(file.read_text(encoding='utf-8')) or {}
    if set(raw) - {'optional', 'mod_start'} != {'core', 'families'}:
        raise SchemaError('families.yaml: holds core, families, optional and mod_start')
    folder = Path(path).parents[1] / 'framework'
    uses = scan_uses(folder, table)
    families = {}
    owner = {}
    for name, body in names('families', raw['families']).items():
        if set(body) - {'services', 'calls', 'files', 'entries'}:
            raise SchemaError(f'families.{name}: a family has services, calls, files and entries')
        families[name] = {'services': list(body.get('services') or []), 'calls': list(body.get('calls') or []),
                          'entries': set(body.get('entries') or [])}
        for relative in body.get('files') or []:
            if relative in owner:
                raise SchemaError(f'families.{name}: {relative} already belongs to {owner[relative]}')
            if not (folder / relative).is_file():
                raise SchemaError(f'families.{name}: {relative} is not a file of the framework')
            owner[relative] = name
    core = raw['core'] or {}
    core_files = set(core.get('files') or [])
    used = {relative for files in uses.values() for relative in files}
    for relative in used - core_files - set(owner):
        raise SchemaError(f'families.yaml: {relative} reads the game table and is in no family and not in core')
    for relative in core_files & set(owner):
        raise SchemaError(f'families.yaml: {relative} is in core and in {owner[relative]}')
    if len(families) > MOST_FAMILIES:
        raise SchemaError(f'families.yaml: at most {MOST_FAMILIES} families')
    return {'names': list(families), 'families': families, 'owner': owner, 'core_files': core_files,
            'core_entries': set(core.get('entries') or []), 'optional': set(raw.get('optional') or []), 'mod_start': set(raw.get('mod_start') or []), 'uses': uses}


def absent_families(table, families, where):
    """The families the entries a build has marked absent take with them. An entry nothing can run without is refused."""
    out = {}
    for at in table['absent']:
        parts = at.split('.')
        if parts[0] == 'constants' and parts[1] in ('code_size', 'stub_base', 'stub_end', 'program_id'):
            raise SchemaError(f'{where}: {at} cannot be absent')
        key = scan_key(at)
        if not families:
            continue
        short = '.'.join(parts[:2])
        if at in families['mod_start']:
            raise SchemaError(f'{where}: {at} cannot be absent: no mod starts without it')
        if short in families['optional'] or at in families['optional']:
            continue
        if short in families['core_entries']:
            raise SchemaError(f'{where}: {at} cannot be absent: the framework does not run without it')
        # The framework keeps these in buffers of its own, where an offset that leads nowhere would not be refused by a read.
        if (parts[0] == 'structs' and (parts[1], 'most') in families['uses']) or (parts[0] == 'constants' and ('most', parts[1]) in families['uses']):
            raise SchemaError(f'{where}: {at} cannot be absent: the framework lays it out in a buffer of its own')
        files = set(families['uses'].get(key, ()))
        if at.split('.')[0] == 'tables':
            for group, body in table['slot_groups'].items():
                if body['tables'] == at.split('.')[1]:
                    files |= set(families['uses'].get(('slot_group', group), ()))
        core = sorted(files & families['core_files'])
        if core:
            raise SchemaError(f'{where}: {at} cannot be absent: {core[0]} needs it, and that file serves every family')
        for name, body in families['families'].items():
            if short in body['entries'] or any(families['owner'].get(file) == name for file in files):
                out.setdefault(name, []).append(at)
    return out


def gate_names():
    """The framework's gates name its families, which a table with none of its own still has to compile with. No build of
    such a table loses one."""
    file = Path(__file__).resolve().parents[2] / 'game' / 'families.yaml'
    return list(yaml.safe_load(file.read_text(encoding='utf-8'))['families']) if file.is_file() else []


def call_families(path, schema):
    """{service: {call or '*': [family's number]}} for the services' generated glue, from the table's families."""
    families = load_families(path, load(path))
    if not families:
        return {}
    services = {service.name: service for service in schema.services}
    out = {}
    for index, name in enumerate(families['names']):
        body = families['families'][name]
        for service in body['services']:
            if service not in services:
                raise SchemaError(f'families.{name}: no service "{service}"')
            out.setdefault(service, {}).setdefault('*', []).append(index)
        for call in body['calls']:
            service, _, function = call.partition('.')
            if service not in services or (function not in {fn.name for fn in services[service].functions} and
                                           function.removeprefix('on_') not in {event.name for event in services[service].events}):
                raise SchemaError(f'families.{name}: no call "{call}"')
            out.setdefault(service, {}).setdefault(function, []).append(index)
    return out


def generate(path, source=None):
    """{relative path: text} for the table at `path`, with each build game/builds/ holds."""
    builds = load_builds(path)
    table = builds[0]
    flat = [flatten(build) for build in builds]
    compare_builds(builds, flat)
    families = load_families(path, table)
    family_names = families['names'] if families else []
    absent = []
    for build in builds:
        lost = absent_families(build, families, f'build {build["key"]}')
        absent.append(lost)
    paths = ['none'] + list(flat[0])
    index = {entry: at for at, entry in enumerate(paths)}
    first = {'none': 0, **flat[0]}
    fixed = table['fixed']
    most_constants = {name: max(v for v in (build['constants'][name] for build in builds) if v != ABSENT_VALUE)
                      for name in table['constants']}

    def cell(entry):
        return f'{{{index[entry]}}}'

    out = [BANNER.format(source=source or Path(path).as_posix()), '#pragma once\n#include <stdint.h>\n\nnamespace tsuchi::game { struct EngineText; struct PhysicsPose; struct EngineArray; }\n\n',
           'namespace tsuchi::game::table {\n\n',
           f'constexpr const char* game = "{table["game"]}";\n\n']
    out.append('// How to tell a build of the game from any other: the 16 bytes at each offset hash (FNV-1a, 64-bit) to the value.\n'
               'struct Span {\n    uint64_t offset, hash;\n};\n')
    rows = ['    0x0, // nothing refers to this one; a list with no entries stands on it\n' if at == 0 else f'    {first[entry]:#x},\n' for at, entry in enumerate(paths)]
    out.append('// Every number that can differ between builds. It starts as the first build\'s, so that it needs no start-up\n'
               '// code, and select_build changes it once as the framework starts. Nothing else writes it.\n'
               f'constexpr unsigned cell_count = {len(paths)};\n'
               f'inline uint64_t cells[cell_count] = {{\n{"".join(rows)}}};\n')
    out.append(f'// The number of an entry a build does not have. Added to an address it gives an address nothing can read.\n'
               f'constexpr uint64_t absent_value = {ABSENT_VALUE:#x};\n')
    out.append('// A number of the selected build. It reads as the number, and cannot be assigned or be used where the\n'
               '// compiler needs a constant; `most` gives those places the largest value of any build.\n'
               'template <class T>\nstruct Cell {\n    unsigned index;\n    operator T() const { return T(cells[index]); }\n    const Cell* operator&() const = delete; // the number has no address of its own\n};\n'
               '// Whether the selected build has this entry.\ntemplate <class T>\ninline bool present(Cell<T> entry) { return cells[entry.index] != absent_value; }\n\n')
    for number_, build in enumerate(builds):
        spans = ''.join(f'    {{{offset:#x}, {value:#018x}}},\n' for offset, value in build['identity']) or '    {0, 0},\n'
        out.append(f'inline constexpr Span identity_{number_}[] = {{\n{spans}}};\n')
        changed = [(at, flat[number_][entry]) for at, entry in enumerate(paths) if at and flat[number_][entry] != first[entry]]
        if number_ == 0:
            # Going back to the first build sets every cell any build changes, so selecting is the same from anywhere.
            changed = [(at, first[entry]) for at, entry in enumerate(paths) if at and any(other[entry] != first[entry] for other in flat[1:])]
        if changed:
            out.append(f'inline constexpr uint16_t changed_cells_{number_}[] = {{{", ".join(str(at) for at, _ in changed)}}};\n'
                       f'inline constexpr uint64_t changed_values_{number_}[] = {{{", ".join(f"{value:#x}" for _, value in changed)}}};\n')
        build['changed_count'] = len(changed)
        own = [build['names'][name] for name in table['build_names']]
        listed = ', '.join(f'"{text}"' for text in own) or 'nullptr'
        out.append(f'inline constexpr const char* build_names_{number_}[] = {{{listed}}};\n')
    out.append('\n// A build of the game the framework knows of.\n'
               'struct Build {\n    const char* key;\n    const char* version;\n    const char* build_id;\n    const Span* identity;\n'
               '    unsigned identity_count;\n    uint64_t code_size;\n    uint64_t program_id;\n    const uint16_t* changed_cells;\n    const uint64_t* changed_values;\n'
               '    unsigned changed_count;\n    uint64_t absent_families;\n    const char* const* names;\n};\n')
    rows = []
    for number_, build in enumerate(builds):
        mask = 0
        for name in absent[number_]:
            mask |= 1 << family_names.index(name)
        cells_name = f'changed_cells_{number_}' if build['changed_count'] else 'nullptr'
        values_name = f'changed_values_{number_}' if build['changed_count'] else 'nullptr'
        rows.append(f'    {{"{build["key"]}", "{build["version"]}", "{build["build_id"].upper()}", identity_{number_}, {len(build["identity"])}, '
                    f'{build["constants"].get("code_size", 0):#x}, {build["constants"].get("program_id", 0):#x}, {cells_name}, {values_name}, {build["changed_count"]}, {mask:#x}, build_names_{number_}}},\n')
    out.append(f'inline constexpr Build builds[] = {{\n{"".join(rows)}}};\nconstexpr unsigned build_count = {len(builds)};\n'
               'inline unsigned selected_build = 0;\n'
               '// Puts a build\'s numbers in the cells. bind_functions comes after it.\n'
               'inline void select_build(unsigned build) {\n'
               '    const Build& first = builds[0];\n'
               '    for (unsigned i = 0; i < first.changed_count; ++i) cells[first.changed_cells[i]] = first.changed_values[i];\n'
               '    const Build& chosen = builds[build];\n'
               '    for (unsigned i = 0; build && i < chosen.changed_count; ++i) cells[chosen.changed_cells[i]] = chosen.changed_values[i];\n'
               '    selected_build = build;\n}\n'
               '// Which build a region of code is, given how to hash the 16 bytes at an offset into it: its place in `builds`, or\n'
               '// -1 for none. A build is looked at only when the region holds all of its code.\n'
               'template <class Hash>\ninline int find_build(uint64_t region_bytes, Hash hash) {\n'
               '    for (unsigned b = 0; b < build_count; ++b) {\n'
               '        if (!builds[b].identity_count || region_bytes < builds[b].code_size) continue;\n'
               '        bool match = true;\n'
               '        for (unsigned i = 0; match && i < builds[b].identity_count; ++i) match = hash(builds[b].identity[i].offset) == builds[b].identity[i].hash;\n'
               '        if (match) return int(b);\n    }\n    return -1;\n}\n'
               '// What the selected build says of itself. These read as text.\n'
               'struct SelectedVersion { operator const char*() const { return builds[selected_build].version; } };\n'
               'struct SelectedKey { operator const char*() const { return builds[selected_build].key; } };\n'
               'constexpr SelectedVersion version{};\nconstexpr SelectedKey build_key{};\n')
    out.append(f'// The API families of game/families.yaml. A bit of a build\'s absent_families stands for each.\n'
               f'constexpr unsigned family_count = {len(family_names)};\n'
               f'inline constexpr const char* families[] = {{{", ".join(f"{chr(34)}{name}{chr(34)}" for name in family_names) or "nullptr"}}};\n'
               'namespace family {\n' + ''.join(f'constexpr unsigned {identifier(name)} = {at};\n' for at, name in enumerate(family_names or gate_names())) + '}\n\n')
    out.append('// An indirect function kept in a global, with its signature.\n'
               'template <class F>\nstruct DataFunction {\n    using Function = F;\n    Cell<uint64_t> address;\n    operator uint64_t() const { return address; }\n};\n')
    out.append('// Addresses of the game\'s globals, from the start of its code.\nnamespace data {\n')
    for name in table['data']:
        typed = table['data_signatures'].get(name)
        out.append(f'constexpr DataFunction<{typed}> {identifier(name)}{{{cell(f"data.{name}")}}};\n' if typed
                   else f'constexpr Cell<uint64_t> {identifier(name)}{cell(f"data.{name}")};\n')
    out.append('}\n\nnamespace constant {\n')
    for name, value in table['constants'].items():
        out.append(f'constexpr uint64_t {identifier(name)} = {value:#x};\n' if f'constants.{name}' in fixed
                   else f'constexpr Cell<uint64_t> {identifier(name)}{cell(f"constants.{name}")};\n')
    out.append('}\n\n// The largest value of each constant in any build, for what the compiler has to size.\nnamespace most {\n')
    out += [f'constexpr uint64_t {identifier(name)} = {value:#x};\n' for name, value in most_constants.items()]
    out.append('}\n\n// Assets of the game\'s own, by their paths.\nnamespace asset {\n')
    out += [f'constexpr const char* {identifier(name)} = "{path}";\n' for name, path in table['assets'].items()]
    out.append('}\n\n// Rows of the game\'s own tables, by their names.\nnamespace row {\n')
    for name, rows in table['rows'].items():
        listed = ', '.join(f'"{row}"' for row in rows)
        out.append(f'constexpr const char* {identifier(name)}[] = {{{listed}}};\n')
    out.append('}\n\n// A name the table lets a build change. It reads as text.\n'
               'struct BuildName {\n    unsigned index;\n    operator const char*() const { return builds[selected_build].names[index]; }\n};\n')
    out.append('// What the game calls things of its own, where the framework has to spell one.\nnamespace name {\n')
    for name, text in table['names'].items():
        out.append(f'constexpr BuildName {identifier(name)}{{{table["build_names"].index(name)}}};\n' if name in table['build_names']
                   else f'constexpr const char* {identifier(name)} = "{text}";\n')
    out.append('}\n\n// An entry of an object\'s own function table whose function the table gives a signature: where the entry\n'
               '// is in the table, and as `F` the function it leads to. It stands for its offset wherever a number is asked\n'
               '// for. game::method (game.h) gives the function, to call.\n'
               'template <class F>\nstruct Entry {\n    using Function = F;\n    Cell<uint64_t> offset;\n    operator uint64_t() const { return offset; }\n};\n')
    out.append('\n// Field offsets, and sizes where known. A size\'s `most` is the largest in any build, for a buffer.\n')
    for name, body in table['structs'].items():
        is_fixed = f'structs.{name}' in fixed
        out.append(f'namespace {identifier(name)} {{\n')
        if body['size'] is not None:
            if is_fixed:
                out.append(f'constexpr uint32_t size = {body["size"]:#x};\n')
            else:
                out.append(f'constexpr Cell<uint64_t> size{cell(f"structs.{name}.size")};\n')
            largest = max((s for s in (build['structs'][name]['size'] for build in builds) if s != ABSENT_VALUE), default=0)
            out.append(f'constexpr uint32_t most = {largest:#x};\n')
        for field, offset in body['fields'].items():
            if is_fixed:
                if field in body['signatures']:
                    raise SchemaError(f'structs.{name}.{field}: a fixed struct has no function table entries')
                out.append(f'constexpr uint32_t {identifier(field)} = {offset:#x};\n')
            else:
                entry = f'structs.{name}.{field}'
                out.append(f'constexpr Entry<{body["signatures"][field]}> {identifier(field)}{{{cell(entry)}}};\n' if field in body['signatures']
                           else f'constexpr Cell<uint64_t> {identifier(field)}{cell(entry)};\n')
        out.append('}\n')
    out.append('\n// What a native route\'s handler finds where: register numbers and offsets from the stack, per build.\nnamespace route_frame {\n')
    for name, fields in table['route_frames'].items():
        out.append(f'namespace {identifier(name)} {{\n')
        out += [f'constexpr Cell<uint64_t> {identifier(field)}{cell(f"route_frames.{name}.{field}")};\n' for field in fields]
        out.append('}\n')
    out.append('}\n')
    out.append('\n// Entries in the game\'s function tables: where the entry is, and the function the game puts there.\n'
               '// A SlotOf is one whose function the table gives a signature, as `F`: hook_slot (hooks.h) takes a hook\n'
               '// of that type for it and no other.\n'
               'struct Slot {\n    Cell<uint64_t> table, offset, stock;\n};\n'
               'template <class F>\nstruct SlotOf : Slot { using Function = F; };\nnamespace slot {\n')
    for name, s in table['slots'].items():
        numbers = ', '.join(cell(f'slots.{name}.{key}') for key in ('table', 'offset', 'stock'))
        out.append(f'constexpr SlotOf<{s["signature"]}> {identifier(name)}{{{{{numbers}}}}};\n' if s['signature']
                   else f'constexpr Slot {identifier(name)}{{{numbers}}};\n')
    out.append('}\nstruct NamedSlot {\n    const char* name;\n    Slot slot;\n};\n')
    listed = ''.join(f'    {{"{name}", slot::{identifier(name)}}},\n' for name in table['slots']) or '    {nullptr, {{0}, {0}, {0}}},\n'
    out.append(f'constexpr NamedSlot slots[] = {{\n{listed}}};\nconstexpr unsigned slot_count = {len(table["slots"])};\n')
    out.append('\n// The same entry in many function tables: the tables, where the entry is in each, and the\n'
               '// functions the game puts there.\n'
               'struct SlotGroup {\n    const Cell<uint64_t>* tables;\n    unsigned table_count;\n    Cell<uint64_t> offset;\n'
               '    const Cell<uint64_t>* stock;\n    unsigned stock_count;\n};\n'
               'template <class F>\nstruct SlotGroupOf : SlotGroup { using Function = F; };\nnamespace tables {\n')
    for name, addresses in table['tables'].items():
        listed = ', '.join(cell(f'tables.{name}.{at}') for at in range(len(addresses)))
        out.append(f'constexpr Cell<uint64_t> {identifier(name)}[] = {{{listed}}};\n')
    out.append('}\nnamespace slot_group {\n')
    for name, group in table['slot_groups'].items():
        tables_name, count = identifier(group['tables']), len(table['tables'][group['tables']])
        numbers = f'tables::{tables_name}, {count}, {cell(f"slot_groups.{name}.offset")}, {identifier(name)}_stock, {len(group["stock"])}'
        listed = ', '.join(cell(f'slot_groups.{name}.stock.{at}') for at in range(len(group['stock'])))
        out.append(f'constexpr Cell<uint64_t> {identifier(name)}_stock[] = {{{listed}}};\n'
                   + (f'constexpr SlotGroupOf<{group["signature"]}> {identifier(name)}{{{{{numbers}}}}};\n' if group['signature']
                      else f'constexpr SlotGroup {identifier(name)}{{{numbers}}};\n'))
    out.append('}\n')
    out.append('\n// Places in the game\'s code that a patch redirects into the framework (see tools/gen/stubs.py):\n'
               '// where the place is, the instruction the game has there, where the patch sends it, and the\n'
               '// way back to the game.\n'
               'struct CodeSite {\n    const char* name;\n    Cell<uint64_t> address;\n    Cell<uint32_t> instruction;\n'
               '    Cell<bool> call;\n    Cell<uint64_t> entry, original;\n};\n')
    listed = ''.join('    {"' + name + '", ' + ', '.join(cell(f'code_sites.{name}.{key}') for key in ('address', 'instruction', 'call', 'entry', 'original')) + '},\n'
                     for name in table['code_sites'])
    listed = listed or '    {nullptr, {0}, {0}, {0}, {0}, {0}},\n'
    out.append(f'constexpr CodeSite code_sites[] = {{\n{listed}}};\n'
               f'constexpr unsigned code_site_count = {len(table["code_sites"])};\n')
    out.append('\n// Register-preserving comparison routes in the matching patch.\n'
               'struct ConditionalSite { Cell<uint64_t> address, entry; Cell<uint32_t> instruction, next_instruction; };\n')
    listed = ''.join('    {' + ', '.join(cell(f'conditional_sites.{name}.{key}') for key in ('address', 'entry', 'instruction', 'next_instruction')) + '},\n'
                     for name in table['conditional_sites']) or '    {{0}, {0}, {0}, {0}},\n'
    out.append(f'constexpr ConditionalSite conditional_sites[] = {{\n{listed}}};\n'
               f'constexpr unsigned conditional_site_count = {len(table["conditional_sites"])};\n')
    out.append('// Each one\'s place in that list. A SiteOf is one whose function the table gives a signature, as `F`:\n'
               '// hook_site (hooks.h) takes a hook of that type for it and no other. It stands for its place wherever a\n'
               '// number is asked for.\n'
               'template <class F>\nstruct SiteOf {\n    using Function = F;\n    unsigned number;\n    constexpr operator unsigned() const { return number; }\n};\n'
               'namespace code_site {\n')
    for at, (name, site) in enumerate(table['code_sites'].items()):
        out.append(f'constexpr SiteOf<{site["signature"]}> {identifier(name)}{{{at}}};\n' if site['signature']
                   else f'constexpr unsigned {identifier(name)} = {at};\n')
    out.append('}\n')
    out.append('\n// Interior native routes use full register frames, independently of mod hooks.\n'
               'struct NativeRoute {\n    const char* name;\n    Cell<uint64_t> address;\n    Cell<uint32_t> instruction;\n'
               '    Cell<bool> call;\n    Cell<uint64_t> entry, returned, original, resume;\n};\n')
    listed = ''.join('    {"' + name + '", ' + ', '.join(cell(f'native_routes.{name}.{key}') for key in ('address', 'instruction', 'call', 'entry', 'returned', 'original', 'resume')) + '},\n'
                     for name in table['native_routes']) or '    {nullptr, {0}, {0}, {0}, {0}, {0}, {0}, {0}},\n'
    out.append(f'constexpr NativeRoute native_routes[] = {{\n{listed}}};\n'
               f'constexpr unsigned native_route_count = {len(table["native_routes"])};\nnamespace native_route {{\n')
    for at, name in enumerate(table['native_routes']):
        out.append(f'constexpr unsigned {identifier(name)} = {at};\n')
    out.append('}\n')
    out.append(f'constexpr Cell<uint64_t> native_return{cell("stub.native_return")};\n')
    out.append('struct NativeExit { unsigned route; Cell<uint64_t> entry, target; };\n')
    exits = [(at, route, name) for at, (route, site) in enumerate(table['native_routes'].items()) for name in site['exits']]
    listed = ''.join(f'    {{{at}, {cell(f"native_routes.{route}.exits.{name}.entry")}, {cell(f"native_routes.{route}.exits.{name}.target")}}},\n'
                     for at, route, name in exits) or '    {0, {0}, {0}},\n'
    out.append(f'constexpr NativeExit native_exits[] = {{\n{listed}}};\n'
               f'constexpr unsigned native_exit_count = {len(exits)};\nnamespace native_exit {{\n')
    for _, route, name in exits:
        out.append(f'constexpr Cell<uint64_t> {identifier(route + "_" + name)}{cell(f"native_routes.{route}.exits.{name}.entry")};\n')
    out.append('}\n')
    out.append('\n// The patch\'s block. What its shared code expects of the framework\'s status block; and what the\n'
               '// framework expects of the block: where it is, the value it begins with, and after that the hash\n'
               '// of everything it was made from, which is the selected build\'s when the two were built together.\n'
               'namespace stub {\n'
               f'constexpr uint64_t status_magic = {stubs.STATUS_MAGIC:#x};\n'
               f'constexpr uint32_t status_site_jump = {stubs.STATUS_SITE_JUMP};\n'
               f'constexpr Cell<uint64_t> block{cell("stub.block")};\n'
               f'constexpr Cell<uint64_t> block_hash{cell("stub.block_hash")};\n'
               f'constexpr uint64_t patch_magic = {stubs.PATCH_MAGIC:#x};\n'
               f'constexpr Cell<uint64_t> patch_hash{cell("stub.patch_hash")};\n'
               f'constexpr Cell<uint64_t> dynamic_base{cell("stub.dynamic_base")};\n'
               f'constexpr unsigned dynamic_count = {table["constants"].get("dynamic_hook_slots", 0)};\n'
               f'constexpr Cell<uint64_t> native_shared{cell("stub.native_shared")};\n}}\n')
    out.append('\n// The game\'s functions. Null until bind_functions runs; a test may point them at its own.\nnamespace fn {\n')
    for name, f in table['functions'].items():
        out.append(f'inline {f["returns"]} (*{identifier(name)})({", ".join(f["args"]) or "void"}) = nullptr;\n')
    missing = [name for name in table['functions'] if any(build['functions'][name]['address'] == ABSENT_VALUE for build in builds)]
    out.append('}\n\n// What a function a build does not have is bound to: it does nothing and returns zero. `bound` says whether a\n'
               '// function can be called for what it does, which a check that it is not null cannot say of this stand-in.\n'
               '// One that returns a structure by value is left null instead, as this header only names those types.\n'
               'template <class R, class... A>\ninline R absent_function(A...) { return R(); }\n'
               'template <class R>\nstruct by_value { static constexpr bool value = false; };\n'
               + ''.join(f'template <>\nstruct by_value<{C_TYPES[kind]}> {{ static constexpr bool value = true; }};\n' for kind in sorted(BY_VALUE)) +
               'template <class R, class... A>\ninline bool bound(R (*function)(A...)) {\n'
               '    if constexpr (by_value<R>::value) return function != nullptr;\n'
               '    else return function && function != &absent_function<R, A...>;\n}\n')
    by_value_types = {C_TYPES[kind] for kind in BY_VALUE}
    out.append('\ninline void bind_functions(uint64_t main_base) {\n')
    for name, f in table['functions'].items():
        signature_ = f'{f["returns"]} (*)({", ".join(f["args"]) or "void"})'
        bound = f'reinterpret_cast<{signature_}>(main_base + cells[{index[f"functions.{name}"]}])'
        if name in missing:
            kinds = ', '.join([f['returns']] + f['args'])
            stand_in = 'nullptr' if f['returns'] in by_value_types else f'absent_function<{kinds}>'
            bound = f'cells[{index[f"functions.{name}"]}] == absent_value ? {stand_in} : {bound}'
        out.append(f'    fn::{identifier(name)} = {bound};\n')
    if not table['functions']:
        out.append('    (void)main_base;\n')
    out.append('}\n\n} // namespace tsuchi::game::table\n')
    return {'framework/game_table.h': ''.join(out)}
