"""Import and switch external mod files before launching the game.

The install root is one merged emulator overlay, or a console SD root. No command
launches the game. Export save:/tsuchi/external-requests.json and pass it to apply.
"""
import argparse
from contextlib import contextmanager
import copy
from functools import wraps
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import re
import stat
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
HEX = re.compile(r'[0-9a-f]{64}\Z')
ID = re.compile(r'external\.[a-z0-9](?:[a-z0-9._-]*[a-z0-9])?\Z')
PRIVATE = '.tsuchi-external'
MAX_MODS = 256
MAX_REGISTRY_BYTES = 3 * 1024 * 1024
MAX_JOURNAL_BYTES = 8 * 1024 * 1024


class Refused(ValueError):
    """The manager preserved files because an operation was not safe."""


def canonical(value):
    return (json.dumps(value, ensure_ascii=False, sort_keys=True, separators=(',', ':')) + '\n').encode('utf-8')


def identity(build=None):
    """What a manager says of the build of the game it manages: the first in the table unless `build` names one."""
    from gen import game_table
    from package_release import title_of
    builds = game_table.load_builds(ROOT / 'game/table.yaml')
    table = next((each for each in builds if each['key'] == build), None) if build else builds[0]
    if table is None:
        raise Refused('the game table has no build ' + build)
    found = {'title': title_of(table), 'version': table['version'], 'build': table['build_id'].upper(), 'project': table['names']['project']}
    # A registry written before there were several builds has no key, and the first build keeps it that way.
    return found if table is builds[0] else {**found, 'key': table['key']}


def _plain(text, label, limit=160):
    if not isinstance(text, str) or not text or len(text.encode('utf-8')) > limit or any(ord(c) < 32 or 127 <= ord(c) <= 159 for c in text):
        raise Refused(label + ' must be nonempty text without control characters')
    return text


def _id(value):
    if not isinstance(value, str) or len(value) > 47 or not ID.fullmatch(value):
        raise Refused('mod ID must begin external. and use 1–47 lowercase letters, digits, dots, underscores, or hyphens')
    return value


def _relative(value):
    if not isinstance(value, str) or not value or '\\' in value or ':' in value:
        raise Refused('unsafe relative path')
    parts = value.split('/')
    if any(not p or p in ('.', '..') or p[-1] in (' ', '.') or
           any(ord(c) < 32 or c in '<>"|?*' for c in p) or
           re.fullmatch(r'(con|prn|aux|nul|com[1-9]|lpt[1-9])(?:\..*)?', p, re.I) for p in parts):
        raise Refused('unsafe relative path: ' + value)
    if PurePosixPath(value).is_absolute():
        raise Refused('absolute package paths are forbidden')
    return parts


def _no_link(path):
    try:
        info = path.lstat()
    except FileNotFoundError:
        return None
    if stat.S_ISLNK(info.st_mode) or getattr(info, 'st_file_attributes', 0) & 0x400:
        raise Refused('symlink or reparse point: ' + str(path))
    return info


def _safe(path):
    """Check every existing component, including the root's ancestors."""
    path = Path(os.path.abspath(path))
    for part in [*reversed(path.parents), path]:
        _no_link(part)
    return path


def _child(root, relative):
    parts = _relative(relative)
    here = _safe(root)
    for part in parts:
        if here.exists():
            if not here.is_dir():
                raise Refused('destination parent is not a directory: ' + str(here))
            matches = [p.name for p in here.iterdir() if p.name.casefold() == part.casefold()]
            if matches and matches != [part]:
                raise Refused('case-folded path collision: ' + str(here / part))
        here /= part
        _no_link(here)
    return here


def digest(path):
    info = _safe(path).stat()
    if not stat.S_ISREG(info.st_mode):
        raise Refused('expected a regular file: ' + str(path))
    value = hashlib.sha256()
    with path.open('rb') as source:
        while block := source.read(1024 * 1024):
            value.update(block)
    return {'sha256': value.hexdigest(), 'bytes': info.st_size}


def _json(path, limit=8 * 1024 * 1024):
    _safe(path)
    if path.stat().st_size > limit:
        raise Refused('manager JSON exceeds its size limit')
    def unique(pairs):
        result = {}
        for key, value in pairs:
            if key in result:
                raise Refused('duplicate JSON key: ' + key)
            result[key] = value
        return result
    try:
        return json.loads(path.read_text(encoding='utf-8'), object_pairs_hook=unique)
    except (UnicodeError, json.JSONDecodeError) as error:
        raise Refused('invalid JSON: ' + str(path)) from error


def _write(path, value):
    _safe(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    handle, temporary = tempfile.mkstemp(prefix=path.name + '.', suffix='.pending', dir=path.parent)
    pending = Path(temporary)
    with os.fdopen(handle, 'wb') as out:
        out.write(canonical(value))
        out.flush()
        os.fsync(out.fileno())
    os.replace(pending, path)
    if os.name != 'nt':
        directory = os.open(path.parent, os.O_RDONLY)
        try:
            os.fsync(directory)
        finally:
            os.close(directory)


def _locked(function):
    @wraps(function)
    def call(self, *args, **kwargs):
        with self._lock():
            return function(self, *args, **kwargs)
    return call


def _can_move():
    if os.name == 'nt':
        return True
    if sys.platform.startswith('linux'):
        import ctypes
        return getattr(ctypes.CDLL(None), 'renameat2', None) is not None
    return False


def _move(source, destination):
    if os.name == 'nt':
        os.rename(source, destination)
        return
    # rename(2) can replace an intervening empty foreign directory. Linux's
    # no-replace variant preserves it, as Windows rename does.
    if sys.platform.startswith('linux'):
        import ctypes
        libc = ctypes.CDLL(None, use_errno=True)
        rename = getattr(libc, 'renameat2', None)
        if rename is not None:
            if rename(-100, os.fsencode(source), -100, os.fsencode(destination), 1) != 0:
                number = ctypes.get_errno()
                raise OSError(number, os.strerror(number), str(destination))
            return
    raise Refused('this host has no supported no-replace move; use Windows or Linux with renameat2')


class ExternalMods:
    def __init__(self, root, layout='emulator', game=None, tsuchi_slot=9):
        if layout not in ('emulator', 'console'):
            raise Refused('layout must be emulator or console')
        if type(tsuchi_slot) is not int or not 0 <= tsuchi_slot <= 9:
            raise Refused('Tsuchi slot must be an integer from 0 through 9')
        self.root, self.layout, self.game = _safe(root), layout, game or identity()
        self.tsuchi_slot = tsuchi_slot
        if not self.root.is_dir():
            raise Refused('install root must be an existing directory')
        # Each build keeps its own registry: a package's patches and cheats are for one build's code.
        own = PRIVATE + ('/' + self.game['key'] if 'key' in self.game else '')
        self.own_private = own
        self.private = _child(self.root, own)
        self.registry_path = _child(self.root, own + '/registry.json')
        self.journal_path = _child(self.root, own + '/journal.json')
        self.romfs = ('romfs' if layout == 'emulator' else
                      'atmosphere/contents/' + self.game['title'] + '/romfs')
        # So is its catalog: two builds of one title share a romfs folder, and the framework reads the selected build's file.
        self.catalog_path = _child(self.root, self.romfs + '/tsuchi/external-mods' + ('-' + self.game['key'] if 'key' in self.game else '') + '.json')
        self._lock_depth = 0

    @contextmanager
    def _lock(self):
        if self._lock_depth:
            self._lock_depth += 1
            try:
                yield
            finally:
                self._lock_depth -= 1
            return
        lock = _child(self.root, self.own_private + '/manager.lock')
        lock.parent.mkdir(parents=True, exist_ok=True)
        with lock.open('a+b') as opened:
            opened.seek(0, os.SEEK_END)
            if not opened.tell():
                opened.write(b'\0')
                opened.flush()
            opened.seek(0)
            try:
                if os.name == 'nt':
                    import msvcrt
                    msvcrt.locking(opened.fileno(), msvcrt.LK_NBLCK, 1)
                else:
                    import fcntl
                    fcntl.flock(opened.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
            except OSError as error:
                raise Refused('another manager owns this install; wait for it to finish') from error
            self._lock_depth = 1
            try:
                yield
            finally:
                self._lock_depth = 0
                opened.seek(0)
                if os.name == 'nt':
                    msvcrt.locking(opened.fileno(), msvcrt.LK_UNLCK, 1)
                else:
                    fcntl.flock(opened.fileno(), fcntl.LOCK_UN)

    def _empty(self):
        return {'format': 1, 'generation': 0, 'layout': self.layout, 'game': self.game,
                'tsuchi_slot': self.tsuchi_slot, 'mods': []}

    @staticmethod
    def _same_registry(left, right):
        # A missing slot in a format-1 registry or journal means the historical 9.
        return dict(left, tsuchi_slot=left.get('tsuchi_slot', 9)) == \
            dict(right, tsuchi_slot=right.get('tsuchi_slot', 9))

    def _destination(self, mod_id, logical):
        parts = _relative(logical)
        top, name = parts[0].casefold(), '/'.join(parts[1:])
        if len(parts) < 2:
            raise Refused('package paths must begin romfs/, exefs/, or ips/')
        if top == 'romfs':
            if parts[0] != 'romfs' or parts[1].casefold() == 'tsuchi':
                raise Refused("Tsuchi's RomFS directory is protected")
            return self.romfs + '/' + name
        if top == 'exefs':
            if parts[0] != 'exefs' or len(parts) != 2:
                raise Refused('ExeFS files must be directly below exefs/')
            if re.fullmatch(r'subsdk[0-9]', name) and int(name[-1]) != self.tsuchi_slot:
                prefix = ('exefs' if self.layout == 'emulator' else
                          'atmosphere/contents/' + self.game['title'] + '/exefs')
                return prefix + '/' + name
            if name.endswith('.pchtxt') and self.layout == 'emulator' and name != self.game.get('key', self.game['version']) + '.pchtxt':
                return 'exefs/' + name
            raise Refused('unsupported or protected ExeFS file: ' + logical)
        if top == 'ips' and parts[0] == 'ips' and len(parts) == 2 and self.layout == 'console':
            if name != self.game['build'] + '.ips':
                raise Refused('IPS filename must match game build ' + self.game['build'])
            return 'atmosphere/exefs_patches/' + mod_id + '/' + name
        raise Refused('unsupported package file for ' + self.layout + ': ' + logical)

    def _registry(self, value=None):
        if value is None:
            value = _json(self.registry_path) if self.registry_path.exists() else self._empty()
        required = {'format', 'generation', 'layout', 'game', 'mods'}
        if not isinstance(value, dict) or not required <= set(value) or set(value) - required - {'tsuchi_slot', 'cheats'} or \
                type(value['format']) is not int or value['format'] != 1 or value['layout'] != self.layout or value['game'] != self.game:
            raise Refused('registry layout or game identity does not match this install')
        slot = value.get('tsuchi_slot', 9)
        if type(slot) is not int or slot != self.tsuchi_slot:
            raise Refused('registry Tsuchi slot differs; keep the selected slot or relocate the installation deliberately')
        if type(value['generation']) is not int or not 0 <= value['generation'] < 2**63:
            raise Refused('invalid registry generation')
        mods = value['mods']
        if not isinstance(mods, list) or len(mods) > MAX_MODS:
            raise Refused('registry exceeds 256 mods')
        ids, paths = set(), set()
        for mod in mods:
            if not isinstance(mod, dict) or set(mod) != {'id', 'name', 'version', 'enabled', 'files'}:
                raise Refused('invalid package registry fields')
            mod_id = _id(mod['id'])
            if mod_id in ids or type(mod['enabled']) is not bool:
                raise Refused('duplicate ID or invalid enabled state')
            ids.add(mod_id)
            _plain(mod['name'], 'name', 128)
            _plain(mod['version'], 'version', 64)
            if not isinstance(mod['files'], dict) or not mod['files']:
                raise Refused('package inventory is empty')
            for logical, item in mod['files'].items():
                active = self._destination(mod_id, logical).casefold()
                if active in paths:
                    raise Refused('packages share destination: ' + logical)
                paths.add(active)
                if not isinstance(item, dict) or set(item) != {'sha256', 'bytes'} or \
                        not isinstance(item['sha256'], str) or not HEX.fullmatch(item['sha256']) or \
                        type(item['bytes']) is not int or item['bytes'] < 0:
                    raise Refused('invalid immutable file inventory')
        if [m['id'] for m in mods] != sorted(ids):
            raise Refused('registry mods must be sorted by ID')
        if 'cheats' in value:
            cheats = value['cheats']
            if not isinstance(cheats, dict) or set(cheats) != {'id', 'version', 'source', 'codes', 'enabled'}:
                raise Refused('invalid cheat-set registry fields')
            prefix = _id(cheats['id'])
            if len(prefix) > 42:
                raise Refused('cheat ID prefix must be at most 42 characters')
            _plain(cheats['version'], 'version', 64)
            source = cheats['source']
            if not isinstance(source, dict) or set(source) != {'bytes', 'sha256'} or type(source['bytes']) is not int or \
                    not 0 < source['bytes'] < 65536 or not isinstance(source['sha256'], str) or not HEX.fullmatch(source['sha256']):
                raise Refused('invalid immutable cheat source inventory')
            codes, enabled = cheats['codes'], cheats['enabled']
            if not isinstance(codes, list) or not 1 <= len(codes) <= 127 or len(mods) + len(codes) > MAX_MODS:
                raise Refused('cheat set or combined catalog exceeds its row limit')
            for index, name in enumerate(codes):
                _plain(name, 'cheat name', 63)
                if prefix + f'.c{index:03}' in ids:
                    raise Refused('cheat row ID collides with an external package')
            if len(set(codes)) != len(codes) or not isinstance(enabled, list) or \
                    any(type(index) is not int or not 0 <= index < len(codes) for index in enabled) or enabled != sorted(set(enabled)):
                raise Refused('invalid cheat names or selected indexes')
        if any('/'.join(parts[:count]) in paths for path in paths
               for parts in [path.split('/')] for count in range(1, len(parts))):
            raise Refused('packages share destination through a file/directory prefix')
        if len(canonical(value)) > MAX_REGISTRY_BYTES:
            raise Refused('registry exceeds its 3 MiB recoverable metadata limit')
        return value

    def _cheat_paths(self):
        prefix = '' if self.layout == 'emulator' else 'atmosphere/contents/' + self.game['title'] + '/'
        paths = {'cheat': _child(self.root, prefix + 'cheats/' + self.game['build'][:16] + '.txt')}
        if self.layout == 'console':
            paths['toggles'] = _child(self.root, prefix + 'cheats/toggles.txt')
        return paths

    def _cheat_source(self):
        return _child(self.root, PRIVATE + '/cheats/source.txt')

    def _cheat_parsed(self, registry):
        if 'cheats' not in registry:
            return None
        import external_cheats
        cheats = registry['cheats']
        source = self._cheat_source()
        if not self._matches(source, cheats['source']):
            raise Refused('immutable cheat source is missing or changed')
        parsed = external_cheats.parse(source.read_bytes())
        if [code['name'] for code in parsed['codes']] != cheats['codes']:
            raise Refused('cheat registry names do not match the immutable source')
        return parsed

    def _cheat_data(self, registry):
        if 'cheats' not in registry:
            return {}
        import external_cheats
        parsed = self._cheat_parsed(registry)
        cheats = registry['cheats']
        outputs = external_cheats.render(parsed, set(cheats['enabled']))
        return {key: data for key, data in outputs.items() if key in self._cheat_paths()}

    @staticmethod
    def _inventory(data):
        return {'bytes': len(data), 'sha256': hashlib.sha256(data).hexdigest()}

    def _cheat_problem(self, registry):
        if 'cheats' not in registry:
            return ''
        try:
            cheats = registry['cheats']
            for index in range(len(cheats['codes'])):
                self._native_collision(cheats['id'] + f'.c{index:03}')
            outputs = self._cheat_data(registry)
            for key, path in self._cheat_paths().items():
                if key in outputs:
                    if not self._matches(path, self._inventory(outputs[key])):
                        return 'Generated cheat output is missing or changed: ' + key
                elif path.exists():
                    return 'Unmanaged cheat output already occupies destination: ' + key
        except (ValueError, OSError) as error:
            return str(error)
        return ''

    def _patch_problems(self, registry, incoming=None):
        import external_patch_checks as patches
        problems, named, owners = {}, {}, {}
        for mod in registry['mods']:
            for logical, item in mod['files'].items():
                if not (logical.startswith('exefs/') and logical.endswith('.pchtxt') or
                        logical.startswith('ips/') and logical.endswith('.ips')):
                    continue
                key = mod['id'] + ': ' + logical
                try:
                    if incoming and mod['id'] in incoming:
                        source = _child(incoming[mod['id']], logical)
                    else:
                        retained, active = self._paths(mod, logical)
                        source = active if self._matches(active, item) else retained
                    if not self._matches(source, item):
                        continue  # The ordinary ownership check reports this conflict.
                    if source.stat().st_size > patches.MAX_INPUT_BYTES:
                        raise ValueError('patch exceeds 8 MiB')
                    writes = patches.decode_patch(source.read_bytes(), Path(logical).suffix, self.game['build'])
                    patches.validate_fixed_ranges(writes, ROOT / 'game/table.yaml', self.game.get('key'))
                    named.setdefault(mod['id'], []).extend(writes)
                    owners[mod['id']] = mod['id']
                except (ValueError, OSError) as error:
                    problems[mod['id']] = 'Static patch conflict: ' + str(error)
        try:
            patches.validate_patch_conflicts(named)
        except ValueError as error:
            for mod_id in owners.values():
                problems[mod_id] = 'Static patch conflict: ' + str(error)
        return problems

    def _paths(self, mod, logical):
        return (_child(self.root, PRIVATE + '/payload/' + mod['id'] + '/' + logical),
                _child(self.root, self._destination(mod['id'], logical)))

    @staticmethod
    def _matches(path, item):
        return path.exists() and digest(path) == item

    def _problem(self, mod):
        try:
            self._native_collision(mod['id'])
            for logical, item in mod['files'].items():
                retained, active = self._paths(mod, logical)
                source, other = (active, retained) if mod['enabled'] else (retained, active)
                if not self._matches(source, item):
                    return 'Owned file is missing or changed: ' + logical
                if other.exists():
                    return 'Another file occupies the inactive destination: ' + logical
        except (OSError, Refused) as error:
            return str(error)
        return ''

    @staticmethod
    def _bounded_problem(problem):
        return problem.encode('utf-8')[:512].decode('utf-8', errors='ignore').translate(
            {c: ord(' ') for c in range(160) if c < 32 or 127 <= c <= 159})

    def inspect(self, _completed=False):
        registry = self._registry()
        recovering = self.journal_path.exists() and not _completed
        patches = self._patch_problems(registry)
        mods = []
        for mod in registry['mods']:
            problem = self._bounded_problem('Run manager recover before launching the game' if recovering else
                                            self._problem(mod) or patches.get(mod['id'], ''))
            roots = {p.split('/')[0] for p in mod['files']}
            kind = 'romfs' if roots == {'romfs'} else 'exefs' if 'romfs' not in roots else 'mixed'
            mods.append({key: mod[key] for key in ('id', 'name', 'version')} | {
                'kind': kind, 'state': 'recovery' if recovering else 'conflict' if problem else
                'enabled' if mod['enabled'] else 'disabled', 'problem': problem, 'manageable': not bool(problem)})
        if 'cheats' in registry:
            cheats = registry['cheats']
            fault = 'Run manager recover before launching the game' if recovering else self._cheat_problem(registry)
            shared = ''
            if not fault:
                parsed = self._cheat_parsed(registry)
                if parsed['master']:
                    shared = 'Required shared code: ' + parsed['master']['name']
            for index, name in enumerate(cheats['codes']):
                mods.append({'id': cheats['id'] + f'.c{index:03}', 'name': name, 'version': cheats['version'],
                             'kind': 'cheats', 'state': 'recovery' if recovering else 'conflict' if fault else
                             'enabled' if index in cheats['enabled'] else 'disabled',
                             'problem': self._bounded_problem(fault or shared), 'manageable': not bool(fault)})
        mods.sort(key=lambda mod: mod['id'])
        body = {'format': 1, 'backend': 'companion', 'mods': mods}
        result = {'format': 1, 'revision': hashlib.sha256(canonical({'registry': registry, 'observation': body})).hexdigest(),
                  'backend': 'companion', 'mods': mods}
        if len(canonical(result)) > 1024 * 1024:
            raise Refused('catalog exceeds runtime 1 MiB limit')
        return result

    @_locked
    def publish(self):
        catalog = self.inspect()
        _write(self.catalog_path, catalog)
        return catalog

    def _ready(self):
        if self.journal_path.exists():
            raise Refused('unfinished operation: run recover before import or apply')
        registry = self._registry()
        for mod in registry['mods']:
            if problem := self._problem(mod):
                raise Refused(problem)
        if problems := self._patch_problems(registry):
            raise Refused(next(iter(problems.values())))
        if problem := self._cheat_problem(registry):
            raise Refused(problem)
        return registry

    def _walk(self, folder):
        folder = _safe(folder)
        if not folder.is_dir():
            raise Refused('package source must be a directory')
        files, folded = {}, set()
        for parent, dirs, names in os.walk(folder, followlinks=False):
            for name in dirs + names:
                path = Path(parent) / name
                relative = path.relative_to(folder).as_posix()
                _relative(relative)
                _no_link(path)
                if relative.casefold() in folded:
                    raise Refused('case-folded package path collision: ' + relative)
                folded.add(relative.casefold())
            for name in names:
                path = Path(parent) / name
                files[path.relative_to(folder).as_posix()] = digest(path)
        if not files:
            raise Refused('package contains no files')
        return dict(sorted(files.items()))

    def _native_collision(self, mod_id):
        if _child(self.root, self.romfs + '/tsuchi/mods/' + mod_id).exists():
            raise Refused('ID already belongs to an installed Tsuchi mod: ' + mod_id)

    def _validate_content(self, path, logical):
        if logical.startswith('exefs/') and logical.endswith('.pchtxt'):
            if path.stat().st_size > 8 * 1024 * 1024:
                raise Refused('PCHTXT exceeds 8 MiB')
            text = path.read_text(encoding='utf-8-sig')
            expected = self.game['build'].ljust(64, '0')
            headers = re.findall(r'^@nsobid-([0-9a-fA-F]{32}|[0-9a-fA-F]{64})\s*$', text, re.M)
            if len(headers) != 1 or headers[0].upper().ljust(64, '0') != expected:
                raise Refused('PCHTXT must declare this game build: ' + self.game['build'])
        elif logical.startswith('exefs/'):
            with path.open('rb') as source:
                if source.read(4) != b'NSO0':
                    raise Refused('subsdk file must be an NSO module')
        elif logical.startswith('ips/'):
            with path.open('rb') as source:
                if source.read(5) not in (b'PATCH', b'IPS32'):
                    raise Refused('IPS header is invalid')

    @staticmethod
    def _copy(source, destination, expected):
        _safe(source)
        _safe(destination)
        destination.parent.mkdir(parents=True, exist_ok=True)
        hashed, size = hashlib.sha256(), 0
        with source.open('rb') as src, destination.open('xb') as dst:
            while block := src.read(1024 * 1024):
                dst.write(block)
                hashed.update(block)
                size += len(block)
            dst.flush()
            os.fsync(dst.fileno())
        if {'sha256': hashed.hexdigest(), 'bytes': size} != expected:
            raise Refused('source changed during import; retained incomplete payload for inspection')

    @_locked
    def import_package(self, source, mod_id, name, version='1.0.0', enable=False, kind=None):
        _id(mod_id)
        _plain(name, 'name', 128)
        _plain(version, 'version', 64)
        if kind not in (None, 'assets', 'cheats'):
            raise Refused('kind must be assets or cheats, or omitted for external files')
        registry = self._ready()
        existing_ids = {m['id'] for m in registry['mods']}
        if 'cheats' in registry:
            existing_ids.update(registry['cheats']['id'] + f'.c{index:03}'
                                for index in range(len(registry['cheats']['codes'])))
        if len(registry['mods']) >= MAX_MODS or mod_id in existing_ids:
            raise Refused('ID already imported, or the 256-mod limit was reached')
        self._native_collision(mod_id)
        source = _safe(source)
        # Keep imports independent from the active tree and retained payload.
        if source == self.root or self.root in source.parents or source in self.root.parents:
            raise Refused('package source and install root must be separate trees')
        files = self._walk(source)
        if kind == 'cheats':
            return self._import_cheats(source, mod_id, version, files, enable, registry)
        if kind == 'assets':
            return self._import_assets(source, mod_id, name, version, files, enable)
        mod = {'id': mod_id, 'name': name, 'version': version, 'enabled': False, 'files': files}
        changed = copy.deepcopy(registry)
        changed['generation'] += 1
        changed['mods'].append(mod)
        changed['mods'].sort(key=lambda m: m['id'])
        self._registry(changed)  # Includes all disabled packages' destination reservations.
        journal = {'format': 1, 'before': registry, 'after': changed, 'moves': []}
        if len(canonical(journal)) > MAX_JOURNAL_BYTES:
            raise Refused('journal exceeds its 8 MiB recovery limit')
        payload = _child(self.root, PRIVATE + '/payload/' + mod_id)
        if payload.exists():
            raise Refused('retained payload already exists; preserve it and inspect the earlier import')
        for logical in files:
            self._validate_content(source / logical, logical)
            _, active = self._paths(mod, logical)
            if active.exists():
                raise Refused('foreign file already occupies destination: ' + logical)
        if problems := self._patch_problems(changed, {mod_id: source}):
            raise Refused(next(iter(problems.values())))
        # All paths and conflicts were checked before creating any payload.
        for logical, item in files.items():
            retained, _ = self._paths(mod, logical)
            self._copy(source / logical, retained, item)
        self._preflight(journal)
        _write(self.journal_path, journal)
        result = self._finish(journal)
        return self.set_enabled(mod_id, True) if enable else result

    def _import_cheats(self, source, prefix, version, files, enable, registry):
        import external_cheats
        if 'cheats' in registry:
            raise Refused('one cheat source is already imported for this installation')
        if len(prefix) > 42:
            raise Refused('cheat ID prefix must be at most 42 characters')
        if enable:
            raise Refused('cheat imports start off; select individual cheat rows after import')
        logical = 'cheats/' + self.game['build'][:16] + '.txt'
        if set(files) != {logical}:
            raise Refused('cheat import requires exactly ' + logical + ' and no other files')
        if not 0 < files[logical]['bytes'] < 65536:
            raise Refused('cheat source must be below 64 KiB')
        parsed = external_cheats.parse((source / logical).read_bytes())
        changed = copy.deepcopy(registry)
        changed['generation'] += 1
        changed['cheats'] = {'id': prefix, 'version': version, 'source': files[logical],
                             'codes': [code['name'] for code in parsed['codes']], 'enabled': []}
        self._registry(changed)
        for index in range(len(parsed['codes'])):
            self._native_collision(prefix + f'.c{index:03}')
        for path in self._cheat_paths().values():
            if path.exists():
                raise Refused('unmanaged cheat or toggle file already occupies destination: ' + str(path))
        destination = self._cheat_source()
        if destination.exists():
            raise Refused('an earlier cheat source remains private; preserve and inspect it before retrying')
        journal = {'format': 1, 'before': registry, 'after': changed, 'moves': []}
        if len(canonical(journal)) > MAX_JOURNAL_BYTES:
            raise Refused('journal exceeds its 8 MiB recovery limit')
        self._copy(source / logical, destination, files[logical])
        self._preflight(journal)
        _write(self.journal_path, journal)
        return self._finish(journal)

    def _operations(self, before, after):
        if before == after:
            return []
        if after['generation'] != before['generation'] + 1:
            raise Refused('journal generation did not advance once')
        old_cheats, new_cheats = before.get('cheats'), after.get('cheats')
        if old_cheats:
            if not new_cheats or {k: v for k, v in old_cheats.items() if k != 'enabled'} != \
                    {k: v for k, v in new_cheats.items() if k != 'enabled'}:
                raise Refused('journal changed the immutable cheat source or names')
        elif new_cheats and new_cheats['enabled']:
            raise Refused('a newly imported cheat set must start off')
        result = []
        old_by_id = {m['id']: m for m in before['mods']}
        new_by_id = {m['id']: m for m in after['mods']}
        if not old_by_id.keys() <= new_by_id.keys():
            raise Refused('journal removed packages')
        added = new_by_id.keys() - old_by_id.keys()
        if len(added) > 1 or any(new_by_id[key]['enabled'] for key in added):
            raise Refused('journal may import one disabled package')
        for old in before['mods']:
            new = new_by_id[old['id']]
            expected = copy.deepcopy(old)
            expected['enabled'] = new['enabled']
            if expected != new:
                raise Refused('journal changed immutable package inventory')
            if old['enabled'] != new['enabled']:
                result.extend({'id': old['id'], 'path': logical, 'enabled': new['enabled']}
                              for logical in sorted(old['files']))
        if added and (result or old_cheats != new_cheats):
            raise Refused('an import journal cannot also change existing activation')
        return result

    def _generated(self, before, after):
        old, new = self._cheat_data(before), self._cheat_data(after)
        changed = {key for key in old.keys() | new.keys() if old.get(key) != new.get(key)}
        if not changed:
            return None
        return {'before': {key: self._inventory(old[key]) for key in sorted(changed) if key in old},
                'after': {key: self._inventory(new[key]) for key in sorted(changed) if key in new}}

    def _generated_paths(self, journal, key):
        if key not in self._cheat_paths():
            raise Refused('unknown generated cheat output')
        directory = PRIVATE + '/cheats/operation/' + str(journal['after']['generation'])
        return (self._cheat_paths()[key], _child(self.root, directory + '/' + key + '.new'),
                _child(self.root, directory + '/' + key + '.old'))

    def _stage_generated(self, journal):
        outputs = self._cheat_data(journal['after'])
        for key in journal.get('generated', {}).get('after', {}):
            _, staging, _ = self._generated_paths(journal, key)
            # An interruption before the journal leaves a private orphan. Do not
            # silently adopt or overwrite it on a later operation.
            if staging.exists():
                raise Refused('earlier unjournaled cheat staging remains; preserve and inspect it: ' + str(staging))
            staging.parent.mkdir(parents=True, exist_ok=True)
            with staging.open('xb') as stream:
                stream.write(outputs[key])
                stream.flush()
                os.fsync(stream.fileno())

    def _generated_preflight(self, journal, recovery=False, preparing=False):
        generated = journal.get('generated')
        if not generated:
            return
        committed = self._same_registry(self._registry(), journal['after'])
        for key in sorted(generated['before'].keys() | generated['after'].keys()):
            active, staged, retired = self._generated_paths(journal, key)
            old, new = generated['before'].get(key), generated['after'].get(key)
            active_old = bool(old and self._matches(active, old))
            active_new = bool(new and self._matches(active, new))
            staged_ok = bool(new and self._matches(staged, new))
            retired_ok = bool(old and self._matches(retired, old))
            if active.exists() and not (active_old or active_new) or \
                    staged.exists() and not staged_ok or retired.exists() and not retired_ok:
                raise Refused('generated cheat source or destination changed; preserved it: ' + key)
            if preparing or not recovery:
                valid = (active_old if old else not active.exists()) and not retired.exists() and \
                        (not staged.exists() if preparing else staged_ok if new else not staged.exists())
            else:
                old_kept = bool(active_old) != bool(retired_ok) if old else not retired.exists()
                if old and committed and not retired.exists() and not active_old:
                    old_kept = True  # Verified publication may already have cleaned it.
                new_kept = bool(active_new) != bool(staged_ok) if new else not staged.exists()
                valid = old_kept and new_kept
            if not valid:
                raise Refused('incomplete or duplicated generated cheat ownership; preserved it: ' + key)
            existing = active.parent
            while not existing.exists():
                existing = existing.parent
            if self.private.stat().st_dev != existing.stat().st_dev:
                raise Refused('generated cheat outputs and retained payload must be on the same volume')

    def _preflight(self, journal, recovery=False, preparing=False):
        required = {'format', 'before', 'after', 'moves'}
        if not isinstance(journal, dict) or not required <= set(journal) or set(journal) - required - {'generated'} or \
                type(journal['format']) is not int or journal['format'] != 1:
            raise Refused('invalid write-ahead journal')
        if len(canonical(journal)) > MAX_JOURNAL_BYTES:
            raise Refused('journal exceeds its 8 MiB recovery limit')
        before, after = self._registry(journal['before']), self._registry(journal['after'])
        if journal['moves'] != self._operations(before, after):
            raise Refused('journal move list does not match state changes')
        expected_generated = self._generated(before, after)
        if journal.get('generated') != expected_generated or ('generated' in journal and expected_generated is None):
            raise Refused('generated cheat journal does not match the immutable source and selection')
        if (journal['moves'] or expected_generated) and not _can_move():
            raise Refused('this host has no supported no-replace move; use Windows or Linux with renameat2')
        current = self._registry()
        if not any(self._same_registry(current, expected) for expected in (before, after)):
            raise Refused('registry changed since the journal was written')
        operations = {(m['id'], m['path']): m for m in journal['moves']}
        for mod in after['mods']:
            for logical, item in mod['files'].items():
                retained, active = self._paths(mod, logical)
                operation = operations.get((mod['id'], logical))
                if operation:
                    src, dst = (retained, active) if operation['enabled'] else (active, retained)
                    src_ok, dst_ok = self._matches(src, item), self._matches(dst, item)
                    same = src.exists() and dst.exists() and os.path.samefile(src, dst)
                    if not recovery and (not src_ok or dst.exists()) or recovery and not (
                            src_ok and not dst.exists() or dst_ok and not src.exists() or src_ok and dst_ok and same):
                        raise Refused('move source or destination changed; preserved both: ' + logical)
                    existing = dst.parent
                    while not existing.exists():
                        existing = existing.parent
                    if src.exists() and src.stat().st_dev != existing.stat().st_dev:
                        raise Refused('active and retained payload must be on the same volume')
                else:
                    src, dst = (active, retained) if mod['enabled'] else (retained, active)
                    if not self._matches(src, item) or dst.exists():
                        raise Refused('unrelated package file changed: ' + logical)
        if problems := self._patch_problems(after):
            raise Refused(next(iter(problems.values())))
        if 'cheats' in after:
            for index in range(len(after['cheats']['codes'])):
                self._native_collision(after['cheats']['id'] + f'.c{index:03}')
        self._generated_preflight(journal, recovery, preparing)
        if not expected_generated and (problem := self._cheat_problem(after)):
            raise Refused(problem)
        return before, after

    def _finish(self, journal, fail_after_move=None):
        _, after = self._preflight(journal, recovery=True)
        mods = {mod['id']: mod for mod in after['mods']}
        index = 0
        for operation in journal['moves']:
            mod = mods[operation['id']]
            logical = operation['path']
            retained, active = self._paths(mod, logical)
            src, dst = (retained, active) if operation['enabled'] else (active, retained)
            item = mod['files'][logical]
            # A writer may have changed a file since the complete-plan preflight.
            # Repeat the ownership check immediately before each move or unlink.
            src_ok, dst_ok = self._matches(src, item), self._matches(dst, item)
            if not (src_ok and not dst.exists() or dst_ok and not src.exists() or
                    src_ok and dst_ok and os.path.samefile(src, dst)):
                raise Refused('file changed after preflight; preserved both: ' + logical)
            if src.exists():
                if dst.exists():
                    # Preserve compatibility with a retained same-inode move journal.
                    if not os.path.samefile(src, dst):
                        raise Refused('destination appeared during move: ' + logical)
                    src.unlink()
                else:
                    dst.parent.mkdir(parents=True, exist_ok=True)
                    _move(src, dst)
                index += 1
                if fail_after_move == index:
                    raise InterruptedError('simulated interruption after move ' + str(index))
        generated = journal.get('generated', {})
        for key, item in generated.get('before', {}).items():
            self._generated_preflight(journal, recovery=True)
            active, _, retired = self._generated_paths(journal, key)
            if self._matches(active, item):
                if retired.exists():
                    raise Refused('retained old cheat output already exists: ' + key)
                retired.parent.mkdir(parents=True, exist_ok=True)
                _move(active, retired)
                index += 1
                if fail_after_move == index:
                    raise InterruptedError('simulated interruption after move ' + str(index))
        for key, item in generated.get('after', {}).items():
            self._generated_preflight(journal, recovery=True)
            active, staged, _ = self._generated_paths(journal, key)
            if staged.exists():
                if not self._matches(staged, item) or active.exists():
                    raise Refused('generated cheat install destination changed: ' + key)
                active.parent.mkdir(parents=True, exist_ok=True)
                _move(staged, active)
                index += 1
                if fail_after_move == index:
                    raise InterruptedError('simulated interruption after move ' + str(index))
        for mod in after['mods']:
            if problem := self._problem(mod):
                raise Refused(problem)
        if problem := self._cheat_problem(after):
            raise Refused(problem)
        _write(self.registry_path, after)
        # Keep the journal through catalog publication. Recovery can replay a
        # committed registry if publication fails, without leaving stale UI state.
        catalog = self.inspect(_completed=True)
        _write(self.catalog_path, catalog)
        for key, item in generated.get('before', {}).items():
            _, _, retired = self._generated_paths(journal, key)
            if retired.exists():
                if not self._matches(retired, item):
                    raise Refused('retained cheat output changed before cleanup; preserved it: ' + key)
                retired.unlink()
        if generated:
            directory = _child(self.root, PRIVATE + '/cheats/operation/' + str(after['generation']))
            for folder in (directory, directory.parent):
                try:
                    folder.rmdir()
                except OSError:
                    pass  # Preserve any unknown file; remove only owned empty folders.
        self.journal_path.unlink()
        return catalog

    @_locked
    def apply(self, request, fail_after_move=None):
        before = self._ready()
        observed = self.inspect()
        if isinstance(request, (str, os.PathLike)):
            request = _json(_safe(request), 256 * 1024)
        if len(canonical(request)) > 256 * 1024:
            raise Refused('request exceeds runtime 256 KiB limit')
        if not isinstance(request, dict) or set(request) != {'format', 'revision', 'enabled'} or \
                type(request['format']) is not int or request['format'] != 1 or \
                request['revision'] != observed['revision'] or not isinstance(request['enabled'], dict):
            raise Refused('request does not match the current catalog revision; inspect and export again')
        entries = {m['id']: m for m in observed['mods']}
        after = copy.deepcopy(before)
        mods = {m['id']: m for m in after['mods']}
        cheat_ids = ({after['cheats']['id'] + f'.c{index:03}': index for index in range(len(after['cheats']['codes']))}
                     if 'cheats' in after else {})
        for mod_id, enabled in request['enabled'].items():
            if mod_id not in entries or not entries[mod_id]['manageable'] or type(enabled) is not bool:
                raise Refused('request contains an unknown or unmanageable ID, or a non-boolean state')
            if mod_id in cheat_ids:
                selected = set(after['cheats']['enabled'])
                if enabled:
                    selected.add(cheat_ids[mod_id])
                else:
                    selected.discard(cheat_ids[mod_id])
                after['cheats']['enabled'] = sorted(selected)
            else:
                mods[mod_id]['enabled'] = enabled
        if before != after:
            after['generation'] += 1
        journal = {'format': 1, 'before': before, 'after': after, 'moves': self._operations(before, after)}
        generated = self._generated(before, after)
        if generated:
            journal['generated'] = generated
            self._preflight(journal, preparing=True)
            self._stage_generated(journal)
        self._preflight(journal)
        if before == after:
            return self.publish()
        _write(self.journal_path, journal)
        return self._finish(journal, fail_after_move)

    def set_enabled(self, mod_id, enabled, fail_after_move=None):
        return self.apply({'format': 1, 'revision': self.inspect()['revision'],
                           'enabled': {mod_id: enabled}}, fail_after_move)

    @_locked
    def recover(self):
        if not self.journal_path.exists():
            self._ready()
            return self.publish()
        return self._finish(_json(self.journal_path))

    def _import_assets(self, source, mod_id, name, version, files, enable):
        if enable:
            raise Refused('asset wrappers use Tsuchi native activation; omit --enable')
        if not _can_move():
            raise Refused('this host has no supported no-replace move for asset wrappers')
        names = list(files)
        if len(names) != 3 or any('/' in p or not re.fullmatch(r'[A-Za-z0-9._-]+\.(pak|utoc|ucas)', p) for p in names):
            raise Refused('assets source must contain only one sibling .pak/.utoc/.ucas triple')
        stems = {p.rsplit('.', 1)[0] for p in names}
        if len(stems) != 1 or {p.rsplit('.', 1)[1] for p in names} != {'pak', 'utoc', 'ucas'}:
            raise Refused('asset container triple must have one matching stem')
        if len(next(iter(stems))) > 250 or not re.fullmatch(r'(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)', version) or \
                any(int(part) > 999999999 for part in version.split('.')):
            raise Refused('asset wrapper needs a stem of at most 250 characters and a numeric x.y.z version')
        toc_name = next(p for p in names if p.endswith('.utoc'))
        def toc_id(path):
            with _safe(path).open('rb') as stream:
                head = stream.read(144)
            return int.from_bytes(head[56:64], 'little') if len(head) == 144 and head[:16] == b'-==--==--==--==-' else 0
        incoming_id = toc_id(source / toc_name)
        if not incoming_id:
            raise Refused('asset .utoc needs the supported 144-byte IoStore header and a nonzero container ID')
        global_paks = _child(self.root, self.romfs + '/' + self.game['project'] + '/Content/Paks')
        if global_paks.exists():
            for file in global_paks.iterdir():
                _no_link(file)
                if not file.is_file():
                    continue
                if file.suffix.casefold() == '.utoc' and toc_id(file) == incoming_id:
                    raise Refused('global auto-mounted copy remains in Content/Paks; remove that copy before importing the wrapper')
                if file.name.casefold() in {n.casefold() for n in names}:
                    sibling = _child(global_paks, file.stem + '.utoc')
                    if not sibling.exists() or not toc_id(sibling):
                        raise Refused('matching global container has no readable IoStore identity; inspect Content/Paks before importing the wrapper')
        target = _child(self.root, self.romfs + '/tsuchi/mods/' + mod_id)
        if target.exists():
            raise Refused('asset wrapper destination already exists')
        # Copy outside the autoload tree, then expose the complete folder by one rename.
        staging = _child(self.root, PRIVATE + '/asset-import/' + mod_id)
        if staging.exists():
            raise Refused('previous asset import retained; inspect it before retrying')
        for filename, item in files.items():
            self._copy(source / filename, staging / filename, item)
        manifest = {'id': mod_id, 'name': name, 'version': version,
                    'meta': {'external_import': {'format': 1, 'files': files}}}
        _write(staging / 'mod.json', manifest)
        manifest_bytes = canonical(manifest)
        expected = dict(files, **{'mod.json': {'bytes': len(manifest_bytes),
                                               'sha256': hashlib.sha256(manifest_bytes).hexdigest()}})
        if self._walk(staging) != expected:
            raise Refused('asset import staging changed; preserved it outside the autoload tree')
        target.parent.mkdir(parents=True, exist_ok=True)
        _move(staging, target)
        return {'id': mod_id, 'kind': 'assets', 'activation': 'tsuchi',
                'message': 'Imported code-free mod; use Tsuchi activation and restart the game'}


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', required=True, type=Path, help='one merged emulator overlay or console SD root')
    parser.add_argument('--layout', choices=['emulator', 'console'], default='emulator')
    parser.add_argument('--build', help="the key of the game's build this install is for; the first in the table when not given")
    parser.add_argument('--tsuchi-slot', type=int, choices=range(10), default=9,
                        help='module slot Tsuchi owns; must match the installed registry')
    commands = parser.add_subparsers(dest='command', required=True)
    imp = commands.add_parser('import', help='copy a package once; external files start disabled')
    imp.add_argument('source', type=Path)
    imp.add_argument('--id', required=True)
    imp.add_argument('--name', required=True)
    imp.add_argument('--version', default='1.0.0')
    imp.add_argument('--kind', choices=['assets', 'cheats'])
    imp.add_argument('--enable', action='store_true')
    imp.add_argument('--game-version')
    imp.add_argument('--build-id')
    commands.add_parser('inspect', help='print observed catalog without modifying the install')
    commands.add_parser('recover', help='finish a journal before launching the game')
    apply = commands.add_parser('apply', help='apply an exported in-game request file')
    apply.add_argument('request', type=Path)
    change = commands.add_parser('set', help='change one package for the next launch')
    change.add_argument('id')
    change.add_argument('state', choices=['enabled', 'disabled'])
    args = parser.parse_args(argv)
    try:
        manager = ExternalMods(args.root, args.layout, identity(args.build), tsuchi_slot=args.tsuchi_slot)
        if args.command == 'import':
            if args.game_version and args.game_version != manager.game['version'] or \
                    args.build_id and args.build_id.upper() != manager.game['build']:
                raise Refused('package game version or build ID does not match game/table.yaml')
            result = manager.import_package(args.source, args.id, args.name, args.version,
                                            args.enable, args.kind)
        elif args.command == 'apply':
            result = manager.apply(args.request)
        elif args.command == 'set':
            result = manager.set_enabled(args.id, args.state == 'enabled')
        else:
            result = getattr(manager, args.command)()
        print(json.dumps(result, ensure_ascii=False, indent=2))
        return 0
    except (ValueError, OSError, UnicodeError) as error:
        print('External mod operation refused: ' + str(error), file=sys.stderr)
        return 1


if __name__ == '__main__':
    sys.exit(main())
