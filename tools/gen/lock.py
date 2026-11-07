"""The order of everything a compiled mod reaches by its place.

A compiled mod calls the framework through tables of function pointers, so what matters to a mod
that is already built is where each thing is, not what it is called. `schema/abi.lock` records
those places: the services in the order the host table lists them, and the members of each
service's table in order. The generators lay the tables out in that order, whatever order the
schema files have.

Until the framework is released the lock follows the schema: `python tools/lock.py` writes it
anew. Once `released` is true it only grows. A new service or member goes at the end of its
list, nothing moves, and what the schema no longer has keeps its place as a gap.

The lock also records, under `marks`, the version that added a service or a member and the
version that deprecated it, for those that say so. Once released, the version that added a thing
is a fact about frameworks people have: it does not change, and a thing released without one
does not gain one. A table's members are in the order they were added, so a mod can tell what a
framework has from how long its table is.
"""
import json

from .schema import SchemaError

NAME = 'abi.lock'


def natural(service):
    """A table's members as the schema has them: the functions compiled mods get, then the events."""
    return [fn.name for fn in service.functions if fn.native] + ['on_' + event.name for event in service.events]


def marks(schema):
    """{name: {'since': version, 'deprecated': version}} for the services and table members that say either."""
    out = {}

    def note(name, mark):
        said = {key: value for key, value in (('since', mark.since), ('deprecated', mark.deprecated)) if value}
        if said:
            out[name] = said

    for service in sorted(schema.services, key=lambda s: s.name):
        note(service.name, service.mark)
        for fn in service.functions:
            if fn.native:
                note(f'{service.name}.{fn.name}', fn.mark)
        for event in service.events:
            note(f'{service.name}.on_{event.name}', event.mark)
    return out


def read(root):
    path = root / NAME
    if not path.is_file():
        return None
    return json.loads(path.read_text(encoding='utf-8'))


def fresh(schema):
    lock = {'released': False, 'api_version': schema.api_version,
            'services': [s.name for s in sorted(schema.services, key=lambda s: s.name)],
            'tables': {s.name: natural(s) for s in sorted(schema.services, key=lambda s: s.name)}}
    said = marks(schema)
    if said:  # a lock of a schema that says nothing of versions has no `marks`
        lock['marks'] = said
    return lock


def grown(schema, lock):
    """The lock with what the schema has added put at the ends. Nothing in it moves.
    The release number people see, `release_version`, is kept as it was."""
    out = growing(schema, lock)
    if lock and lock.get('release_version') is not None:
        out = {**{key: out[key] for key in ('released', 'api_version')}, 'release_version': lock['release_version'],
               **{key: value for key, value in out.items() if key not in ('released', 'api_version')}}
    return out


def growing(schema, lock):
    if not lock or not lock.get('released'):
        return fresh(schema)
    out = {'released': True, 'api_version': schema.api_version, 'services': list(lock['services']),
           'tables': {name: list(members) for name, members in lock['tables'].items()}}
    for service in sorted(schema.services, key=lambda s: s.name):
        if service.name not in out['tables']:
            out['services'].append(service.name)
            out['tables'][service.name] = []
        out['tables'][service.name] += [m for m in natural(service) if m not in out['tables'][service.name]]
    # What was released keeps the version it was released as added in, and the version it was deprecated in.
    before, now, have = lock.get('marks', {}), marks(schema), marks_possible(schema)
    released = set(lock['services']) | {f'{name}.{member}' for name, members in lock['tables'].items() for member in members}
    for name in sorted(released & have):
        was, said = before.get(name, {}), now.get(name, {})
        if was.get('since') != said.get('since'):
            raise SchemaError(f'{name}: released as {added(was)}, and the schema says {added(said)}; that does not change')
        if was.get('deprecated') and said.get('deprecated') != was['deprecated']:
            raise SchemaError(f'{name}: released as deprecated in version {was["deprecated"]}; that does not change')
    # What is gone keeps its record with its place.
    kept = {name: mark for name, mark in before.items() if name not in have}
    if kept or now:
        out['marks'] = dict(sorted({**kept, **now}.items()))
    return out


def added(said):
    return f'added in version {said["since"]}' if said.get('since') else 'there from the first release'


def marks_possible(schema):
    """Every name the schema has that the lock could hold a mark for."""
    return {service.name for service in schema.services} | {f'{service.name}.{member}' for service in schema.services
                                                            for member in natural(service)}


def write(root, lock):
    (root / NAME).write_text(json.dumps(lock, indent=1) + '\n', encoding='utf-8', newline='\n')


def apply(schema, lock):
    """Puts the schema's services in the lock's order and gives each service its table's order.

    A name the lock has and the schema does not is a gap: `None` in the order. A name the schema
    has and the lock does not is an error, since its place has not been given out yet.
    """
    if lock is None:
        lock = fresh(schema)
    by_name = {s.name: s for s in schema.services}
    for service in schema.services:
        if service.name not in lock['tables']:
            raise SchemaError(f'{service.name}: the service is not in schema/{NAME}; run python tools/lock.py')
        have = natural(service)
        order = lock['tables'][service.name]
        missing = [m for m in have if m not in order]
        if missing:
            raise SchemaError(f'{service.name}.{missing[0]}: not in schema/{NAME}; run python tools/lock.py')
        gone = [m for m in order if m not in have]
        if gone and not lock.get('released'):
            raise SchemaError(f'{service.name}.{gone[0]}: in schema/{NAME} and not in the schema; run python tools/lock.py')
        service.table_order = [m if m in have else None for m in order]
    gone = [name for name in lock['services'] if name not in by_name]
    if gone and not lock.get('released'):
        raise SchemaError(f'{gone[0]}: in schema/{NAME} and not in the schema; run python tools/lock.py')
    if sorted(n for n in lock['services'] if n in by_name) != sorted(by_name):
        raise SchemaError(f'schema/{NAME} does not list every service once; run python tools/lock.py')
    schema.host_order = [by_name.get(name) for name in lock['services']]
    # The versions the lock records are the schema's own, for everything the schema still has.
    possible, said = marks_possible(schema), marks(schema)
    recorded = {name: mark for name, mark in lock.get('marks', {}).items() if name in possible}
    for name in sorted(set(recorded) | set(said)):
        if recorded.get(name) != said.get(name):
            raise SchemaError(f'{name}: schema/{NAME} and the schema do not agree on its version; run python tools/lock.py')
    if lock.get('released'):
        in_order(schema)
    return schema


def in_order(schema):
    """A released table has its members in the order they were added, and the host table its services:
    a framework has everything up to the length of its table, so nothing older may sit behind something newer."""
    newest, after = 0, ''
    for service in schema.host_order:
        if not service:
            continue
        if service.mark.since < newest:
            raise SchemaError(f'{service.name}: its place in schema/{NAME} is after {after}, which was added in version '
                              f'{newest}; a framework older than that would have neither')
        newest, after = service.mark.since, service.name
    for service in schema.services:
        members = {fn.name: fn for fn in service.functions}
        members.update({'on_' + event.name: event for event in service.events})
        newest, after = 0, ''
        for name in service.table_order:
            if not name:
                continue
            since = service.since(members[name])
            if since < newest:
                raise SchemaError(f'{service.name}.{name}: its place in schema/{NAME} is after {after}, which was added in '
                                  f'version {newest}; a framework older than that would have neither')
            newest, after = since, name
