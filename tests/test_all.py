"""The repository's checks. Run them all with: python tools/check.py"""
import copy
import json
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))

import build_module  # noqa: E402
import check_words  # noqa: E402
import generate  # noqa: E402
import link_args  # noqa: E402
import read_checks  # noqa: E402
import stage_test_mods  # noqa: E402
from gen import SchemaError, c_header, docs_page, game_table, load, lock, lua_binding, service_glue, stubs  # noqa: E402

BUILD = ROOT / 'build'
GEN = BUILD / 'gen'
LUA = ROOT / 'third_party' / 'lua-5.4.7' / 'src'
CORE = ROOT / 'framework' / 'core'
BASE = ROOT / 'framework' / 'base'
GAME = ROOT / 'framework' / 'game'
FRAMEWORK_TARGET = ROOT / 'framework' / 'target'
# The framework is built with no warnings for what it has deprecated: it still offers those, and uses them to do so.
CXX = ['-std=c++17', '-Wall', '-Wextra', '-Werror', '-fno-exceptions', '-fno-rtti', '-DTSUCHI_DEPRECATED(text)=']
EXE = '.exe' if sys.platform == 'win32' else ''


def run(command):
    result = subprocess.run([str(c) for c in command], capture_output=True, text=True)
    if result.returncode:
        raise AssertionError(f'{" ".join(str(c) for c in command[:3])} ...\n{result.stdout}{result.stderr}')
    return result


def build_program(program, flags, sources, extra_objects=()):
    """Compiles each source on its own, all at once across the machine's cores, then links them into one program."""
    from concurrent.futures import ThreadPoolExecutor
    out = BUILD / 'obj' / Path(program).stem
    shutil.rmtree(out, ignore_errors=True)
    out.mkdir(parents=True)
    compiler = shutil.which('clang++')
    built = [Path(source) for source in sources if Path(source).suffix in ('.o', '.obj')]  # already compiled: linked as they are
    sources = [source for source in sources if Path(source).suffix not in ('.o', '.obj')]
    extra_objects = [*built, *extra_objects]
    objects = [out / f'{number:03}-{Path(source).stem}.o' for number, source in enumerate(sources)]
    with ThreadPoolExecutor(build_module.build_jobs()) as pool:
        list(pool.map(lambda pair: run([compiler, *flags, '-c', pair[0], '-o', pair[1]]), zip(sources, objects)))
    # A worktree adds enough path text for the full framework's link to exceed
    # Windows' command-line limit. LLVM reads the same arguments from a file.
    response = out / 'link.rsp'
    arguments = [*objects, *extra_objects, '-o', program]
    response.write_text('\n'.join('"' + str(value).replace('\\', '/').replace('"', '\\"') + '"'
                                  for value in arguments) + '\n', encoding='utf-8')
    run([compiler, '@' + str(response)])
    return program


def lua_objects():
    """Lua compiled once into build/obj/lua-checked and reused. It checks how it is called, as the module's does not."""
    out = BUILD / 'obj' / 'lua-checked'
    out.mkdir(parents=True, exist_ok=True)
    objects = []
    for source in sorted(LUA.glob('*.c')):
        if source.name in ('lua.c', 'luac.c'):
            continue
        obj = out / (source.stem + '.o')
        if not obj.exists() or obj.stat().st_mtime < source.stat().st_mtime:
            run([shutil.which('clang'), '-std=c99', '-O1', '-DLUA_USE_APICHECK', '-c', source, '-o', obj])
        objects.append(obj)
    return objects


# The parts of Lua the framework is built with: the core, and the libraries that need nothing of the machine.
SCRIPT_LUA = ('lapi', 'lcode', 'lctype', 'ldebug', 'ldo', 'ldump', 'lfunc', 'lgc', 'llex', 'lmem', 'lobject', 'lopcodes', 'lparser', 'lstate',
              'lstring', 'ltable', 'ltm', 'lundump', 'lvm', 'lzio', 'lauxlib', 'lbaselib', 'lcorolib', 'lmathlib', 'lstrlib', 'ltablib', 'lutf8lib')
SCRIPT_CONFIG = ['-I', ROOT / 'runtime' / 'lua', '-DLUA_USER_H="tsuchi_luauser.h"']
LIBC = ROOT / 'runtime' / 'lua' / 'libc'
# Lua against the framework's own C library, on this machine: its names get a prefix, and this machine's headers are left out.
OWN_LIBC = ['-ffreestanding', '-nostdlibinc', '-U_WIN32', '-I', LIBC / 'include', '-include', LIBC / 'host_names.h']


def script_lua_objects(own_library):
    """The framework's Lua, compiled once for this machine: on its C library, or on the framework's own."""
    out = BUILD / 'obj' / ('lua-own' if own_library else 'lua-host')
    out.mkdir(parents=True, exist_ok=True)
    flags = ['-std=c99', '-O1', '-w', *(OWN_LIBC if own_library else []), *SCRIPT_CONFIG]
    newest = max(path.stat().st_mtime for path in (*LIBC.rglob('*'), ROOT / 'runtime' / 'lua' / 'tsuchi_luauser.h') if path.is_file())
    sources = [LUA / f'{name}.c' for name in SCRIPT_LUA] + ([LIBC / 'libc.c'] if own_library else [])
    objects = []
    for source in sources:
        obj = out / (source.stem + '.o')
        if not obj.exists() or obj.stat().st_mtime < max(source.stat().st_mtime, newest):
            run([shutil.which('clang'), *flags, '-c', source, '-o', obj])
        objects.append(obj)
    return objects


MODS = ROOT / 'tests' / 'fixtures' / 'mods'
SDK_CHECK = ROOT / 'sdk' / 'check'


def c_text(data):
    """Bytes as a C string literal, a few lines of it, with every byte that could be misread written as its number."""
    parts = []
    for at in range(0, len(data), 64):
        piece = ''.join(chr(byte) if 32 <= byte < 127 and chr(byte) not in '"\\?' else f'\\{byte:03o}' for byte in data[at:at + 64])
        parts.append(f'"{piece}"')
    return '\n    '.join(parts) or '""'


def check_mods_for_host(fixture):
    """The check mods among the fixtures as the checks against the made-up game install them.

    Writes check_mods_data.h into the fixture's folder: every file of each check mod and of the mods it needs, the
    SDK's helper for scripts, and the start of each compiled check mod. Returns the compiled check mods' code, built
    for this machine, each under a name of its own. The files under tests/game/check_mods take the place of a
    mod's own of the same name: they name what the made-up game has."""
    # Native inline qualification has its own same-source host stand-in and
    # genuine ARM64 SDK builds; this made-up game's model has no live veneers.
    checks = {mod: group for mod, group in stage_test_mods.check_mods().items()
              if mod not in stage_test_mods.native_hook_fixture.MODS}
    wanted = list(checks)
    wanted.append(stage_test_mods.ai_order_fixture.VALUES)
    wanted.append('check.javascript')
    ai_files = stage_test_mods.ai_order_fixture.files(game_table.load(ROOT / "tests" / "fixtures" / "game_table.yaml"))
    for mod in wanted:  # and what each needs, and what those need
        manifest = json.loads((MODS / mod / 'mod.json').read_text(encoding='utf-8'))
        wanted += [other for other in manifest.get('requires', {}) if other not in wanted]
    if "check.cleanup" in wanted:
        wanted.append("check.cleanup-producer")
    files, starts, objects = [], [], []
    out = BUILD / 'obj' / 'check-mods'
    out.mkdir(parents=True, exist_ok=True)
    for mod in wanted:
        own = {path.relative_to(MODS / mod).as_posix(): path for path in sorted((MODS / mod).rglob('*')) if path.is_file()}
        sources = [path for name, path in own.items() if name.startswith('source/')]
        own = {name: path for name, path in own.items() if not name.startswith('source/')}
        overlay = ROOT / 'tests' / 'game' / 'check_mods' / mod
        own.update({path.relative_to(overlay).as_posix(): path for path in sorted(overlay.rglob('*')) if path.is_file()})
        if mod in checks and 'main.lua' in own:
            own.setdefault('check.lua', SDK_CHECK / 'check.lua')
        if mod == 'check.javascript':
            import quickjs_build
            from test_javascript import JavaScript
            if not JavaScript.__dict__.get('programs'): JavaScript.setUpClass()
            told = subprocess.run([str(JavaScript.programs[False]), str(ROOT / 'tests/javascript/language.js')], capture_output=True, check=True).stdout
            files.append((mod, 'language.txt', told.split(b'javascript host passed')[0].replace(b'\r\n', b'\n').rstrip(b'\n')))
        generated = ai_files.get(mod, {})
        files += [(mod, name, path.read_bytes()) for name, path in own.items() if name not in generated]
        files += [(mod, name, data) for name, data in generated.items()]
        if mod == 'check.tick-state':
            import tick_state_fixture
            files.append((mod, 'targets.txt', tick_state_fixture.targets(ROOT / 'tests' / 'fixtures' / 'game_table.yaml').encode('utf-8')))
        if not sources:
            continue
        symbol = 'check_mod_' + re.sub('[^a-z0-9]', '_', mod)
        starts.append((mod, symbol))
        for source in sources:
            compiler = [shutil.which('clang'), '-std=c11'] if source.suffix == '.c' else [shutil.which('clang++'), *CXX]
            obj = out / f'{mod}-{source.stem}.o'
            run([*compiler, '-Wall', '-Wextra', '-Werror', '-I', GEN / 'include', '-I', SDK_CHECK, f'-Dtsuchi_mod_start={symbol}', '-c', source, '-o', obj])
            objects.append(obj)
    js_files = [f for f in files if f[0] == 'check.javascript']
    files = [f for f in files if f[0] != 'check.javascript']
    js_starts = [f for f in starts if f[0] == 'check.javascript']
    starts = [f for f in starts if f[0] != 'check.javascript']
    helper = (SDK_CHECK / 'check.lua').read_bytes()
    lines = ['// Made by tests/test_all.py from the check mods in tests/fixtures/mods. Do not edit.', '#pragma once', '#include <cstddef>', '#include <cstdlib>', '',
             '#include "tsuchi/tsuchi.h"', '#include "tsuchi_check.h"', '', 'extern "C" {']
    lines += [f'TSUCHI_Result {symbol}(const TSUCHI_Host*, TSUCHI_Mod*);' for _, symbol in starts + js_starts]
    lines += ['}', '', 'struct CheckModFile {', '    const char* mod;', '    const char* path;', '    const char* bytes;', '    size_t size;', '};',
              'struct CheckModStart {', '    const char* mod;', '    TSUCHI_Result (*start)(const TSUCHI_Host*, TSUCHI_Mod*);', '};', '',
              '// Each list ends with an entry that names no mod.', 'inline const CheckModFile check_mod_files[] = {']
    lines += [f'    {{"{mod}", "{name}",\n    {c_text(data)}, {len(data)}}},' for mod, name, data in files]
    lines += ['    {nullptr, nullptr, nullptr, 0},', '};', 'inline const CheckModStart check_mod_starts[] = {']
    lines += [f'    {{"{mod}", {symbol}}},' for mod, symbol in starts]
    lines += ['    {nullptr, nullptr},', '};', 'inline const char* const check_mod_ids[] = {' + ''.join(f'"{mod}", ' for mod in checks) + 'nullptr};',
              'inline const char* const check_mod_explicit_ids[] = {' + ''.join(f'"{mod}", ' for mod in checks if stage_test_mods.explicit_check(MODS / mod)) + 'nullptr};',
              f'inline const char check_mods_helper[] =\n    {c_text(helper)};', f'inline const size_t check_mods_helper_size = {len(helper)};', '']
    lines += ['inline const CheckModFile javascript_mod_files[] = {']
    lines += [f'    {{"{mod}", "{name}",\n    {c_text(data)}, {len(data)}}},' for mod, name, data in js_files]
    lines += ['    {nullptr, nullptr, nullptr, 0},', '};', 'inline const CheckModStart javascript_mod_starts[] = {']
    lines += [f'    {{"{mod}", {symbol}}},' for mod, symbol in js_starts]
    lines += ['    {nullptr, nullptr},', '};']
    path = fixture / 'framework' / 'check_mods_data.h'
    text = '\n'.join(lines)
    if not path.exists() or path.read_text(encoding='utf-8') != text:
        path.write_text(text, encoding='utf-8', newline='\n')
    return objects


class Words(unittest.TestCase):
    def test_nothing_that_does_not_belong(self):
        self.assertEqual(check_words.find(ROOT), [])


class SchemaRules(unittest.TestCase):
    def broken(self, *edits, version=None):
        """The schema with edits to combat.yaml: an old text and a new one, any number of times. `version` is
        the framework's version there, when it is to be another than today's."""
        folder = Path(self.enterContext(tempfile.TemporaryDirectory()))
        for path in (ROOT / 'schema').glob('*.yaml'):
            text = path.read_text(encoding='utf-8')
            if path.name == 'combat.yaml':
                for old, new in zip(edits[::2], edits[1::2]):
                    self.assertIn(old, text)
                    text = text.replace(old, new)
            if path.name == 'core.yaml' and version is not None:
                self.assertIn('api_version: 0 ', text)
                text = text.replace('api_version: 0 ', f'api_version: {version} ')
            (folder / path.name).write_text(text, encoding='utf-8')
        return folder

    def test_the_schema_loads(self):
        schema = load(ROOT / 'schema')
        self.assertEqual([s.name for s in schema.services], ['actor', 'animation', 'asset', 'audio', 'battle', 'behavior', 'camera', 'captain', 'carry', 'collision', 'combat', 'component', 'content', 'creature', 'datatable', 'dialog', 'effects', 'entity', 'file', 'game', 'gimmicks', 'hook', 'hud', 'input', 'mod', 'net', 'pikmin', 'player', 'reflection', 'save', 'session', 'settings', 'squad', 'storage', 'story', 'text', 'time', 'ui', 'world'])

    def test_engine_and_game_actor_contracts_are_distinct(self):
        schema = load(ROOT / 'schema')
        services = {service.name: service for service in schema.services}
        actor, entity, collision, component = (services[name] for name in ('actor', 'entity', 'collision', 'component'))
        self.assertEqual((actor.layer, entity.layer, collision.layer, services['audio'].layer), ('engine', 'game', 'game', 'game'))
        self.assertEqual([fn.name for fn in actor.functions], ['list', 'alive', 'transform', 'set_transform'])
        self.assertEqual([fn.name for fn in collision.functions], ['hold_off', 'release_off', 'off'])
        self.assertFalse(set(fn.name for fn in component.functions) & set(fn.name for fn in collision.functions))
        self.assertIn('pose', [fn.name for fn in entity.functions])
        self.assertNotIn('transform', [fn.name for fn in entity.functions])
        self.assertEqual([event.name for event in entity.events], ['spawned', 'destroyed'])
        headers = c_header.generate(schema)
        core = headers['include/tsuchi/core.h']
        self.assertEqual(core.count('typedef struct TSUCHI_Actor '), 1)
        self.assertNotIn('TSUCHI_Entity ', core)
        actor_header, entity_header = (headers[f'include/tsuchi/{name}.h'] for name in ('actor', 'entity'))
        self.assertIn('TSUCHI_Transform', actor_header)
        self.assertNotIn('TSUCHI_ActorTransform', actor_header)
        self.assertNotIn('TSUCHI_Pose', actor_header)
        self.assertIn('TSUCHI_Pose', entity_header)
        self.assertNotIn('TSUCHI_Transform ', entity_header)
        self.assertIn('TSUCHI_EntitySpawnedFn', entity_header)
        self.assertIn('TSUCHI_EntityDestroyedFn', entity_header)
        self.assertNotIn('TSUCHI_ActorSpawned', entity_header)

    def test_a_call_needs_two_users(self):
        with self.assertRaisesRegex(SchemaError, 'at least 2 users'):
            load(self.broken('users: [groink, online-coop]', 'users: [groink]'))

    def test_a_call_with_no_users_completes_a_family_that_has_them(self):
        """A call gets a typed form for two users, or because it completes a family that has them: then it names one of the family."""
        mods = next(s for s in load(ROOT / 'schema').services if s.name == 'mod')
        asked = {fn.name: fn for fn in mods.functions}
        self.assertEqual((asked['framework'].completes, asked['framework'].users), ('version', []))
        self.assertGreaterEqual(len(set(asked['version'].users)), 2)
        # knockback's own failures, which no other function of the service lists in this order
        knock = ('      invalid: The damage is negative.\n'
                 '      not_ready: "The target can\'t be hit right now, like when it is asleep or the game is stopped."\n'
                 '      no_memory: "The game is out of memory."\n    users: [groink, online-coop]\n')
        kill = '      not_ready: "The game can\'t remove actors right now."\n    users: [groink, online-coop]\n'
        load(self.broken(knock, knock.replace('users: [groink, online-coop]', 'completes: damage')))
        load(self.broken(knock, knock.replace('users: [groink, online-coop]', 'completes: damaged')))  # an event is of the family too
        for named in ('no_such', 'knockback', 'entity.destroy', '[damage]'):
            with self.assertRaisesRegex(SchemaError, 'combat.knockback: (a typed call names its users|"completes" names a function or an event of the same service)'):
                load(self.broken(knock, knock.replace('users: [groink, online-coop]', f'completes: {named}')))
        with self.assertRaisesRegex(SchemaError, 'combat.knockback: a typed call names its users, or with "completes"'):
            load(self.broken(knock, knock + '    completes: damage\n'))
        # A member that only completes a family does not make one.
        with self.assertRaisesRegex(SchemaError, 'combat.knockback: "completes" names a function or an event of the same service that has 2 users'):
            load(self.broken(knock, knock.replace('users: [groink, online-coop]', 'completes: kill_pikmin'),
                             kill, kill.replace('users: [groink, online-coop]', 'completes: damage')))

    def test_a_type_must_exist(self):
        with self.assertRaisesRegex(SchemaError, 'unknown enum'):
            load(self.broken('enum<AttackKind>', 'enum<HitKind>'))

    def test_a_result_must_exist(self):
        with self.assertRaisesRegex(SchemaError, 'unknown result'):
            load(self.broken('no_memory: "The game is out of memory."', 'refused: "The game is out of memory."'))

    def test_a_description_is_a_sentence(self):
        with self.assertRaisesRegex(SchemaError, 'full sentence'):
            load(self.broken('doc: The actor to read.', 'doc: the actor'))

    def test_a_table_keeps_its_places(self):
        """A compiled mod reaches each service, function and event by its place, and the lock keeps the places."""
        recorded = lock.read(ROOT / 'schema')
        self.assertIsNotNone(recorded, 'schema/abi.lock is missing: run python tools/lock.py')
        combat = next(s for s in load(ROOT / 'schema').services if s.name == 'combat')
        self.assertEqual(combat.table_order, recorded['tables']['combat'])

        def with_lock(changed):
            folder = self.broken('service: combat', 'service: combat')
            (folder / lock.NAME).write_text(json.dumps(changed), encoding='utf-8')
            return folder

        # A function the lock has no place for is refused until the lock is brought up to date.
        short = copy.deepcopy(recorded)
        short['tables']['combat'].remove('set_life')
        with self.assertRaisesRegex(SchemaError, 'combat.set_life: not in schema/abi.lock'):
            load(with_lock(short))
        # Before a release the lock follows the schema, so a name only the lock has is a mistake.
        longer = copy.deepcopy(recorded)
        longer['released'] = False
        longer['tables']['combat'].insert(1, 'once_offered')
        with self.assertRaisesRegex(SchemaError, 'combat.once_offered: in schema/abi.lock and not in the schema'):
            load(with_lock(longer))
        # After one, what the schema no longer has keeps its place as a gap, and nothing after it moves.
        longer['released'] = True
        schema = load(with_lock(longer))
        combat = next(s for s in schema.services if s.name == 'combat')
        self.assertIsNone(combat.table_order[1])
        self.assertEqual([m for m in combat.table_order if m], recorded['tables']['combat'])
        header = c_header.generate(schema)['include/tsuchi/combat.h']
        first, second = recorded['tables']['combat'][:2]
        self.assertLess(header.index(f'(*{first})('), header.index('void *gone_1;'))
        self.assertLess(header.index('void *gone_1;'), header.index(f'(*{second})('))
        # And what the schema adds goes to the end of its table.
        grown = lock.grown(schema, longer)
        self.assertEqual(grown['tables'], longer['tables'])
        short['released'] = True
        self.assertEqual(lock.grown(load(ROOT / 'schema'), short)['tables']['combat'], short['tables']['combat'] + ['set_life'])

    # A function, an enum's value, a struct and its field, and an event of combat, each with something said of it.
    KNOCKBACK = '  - name: knockback\n'
    SET_LIFE = '  - name: set_life\n'
    BUBBLE = '{name: bubble, value: 7, doc: "A bubble that traps the target."}'
    LIFE = '  Life:\n'
    EXTRA = '{name: extra, type: f32, doc: "Current bonus health, like a shield. `0` for actors that don\'t have any."}'
    DEFEATED = '  - name: defeated\n'

    def test_what_was_added_after_the_first_release_says_so(self):
        """`since` is the version of the framework that added a thing. What the first release has says nothing."""
        self.assertEqual(load(ROOT / 'schema').api_version, 0)  # nothing is released, so nothing can have been added later
        added = (self.KNOCKBACK, self.KNOCKBACK + '    since: 3\n')
        with self.assertRaisesRegex(SchemaError, 'combat.knockback: added in version 3, and the framework is at version 0'):
            load(self.broken(*added))
        with self.assertRaisesRegex(SchemaError, 'combat.knockback: added in version 3, and the framework is at version 2'):
            load(self.broken(*added, version=2))
        for said in ('1', '0', '"3"', 'true', '2.5'):
            with self.assertRaisesRegex(SchemaError, 'combat.knockback: "since" is the version of the framework that added it'):
                load(self.broken(self.KNOCKBACK, self.KNOCKBACK + f'    since: {said}\n', version=4))
        # A struct and an event never grow, so a field has no version of its own.
        with self.assertRaisesRegex(SchemaError, 'combat.structs.Life.extra: a field has no "since"'):
            load(self.broken(self.EXTRA, self.EXTRA[:-1] + ', since: 2}', version=4))
        with self.assertRaisesRegex(SchemaError, 'combat.defeated.actor: a field has no "since"'):
            load(self.broken('{name: actor, type: handle<actor>, doc: The actor that was defeated.}',
                             '{name: actor, type: handle<actor>, doc: The actor that was defeated., since: 2}', version=4))
        # Nothing is older than what it uses, or than what it is in.
        with self.assertRaisesRegex(SchemaError, 'combat.life: it is in the API from version 1, before Life, which was added in version 3: say "since: 3" or later'):
            load(self.broken(self.LIFE, self.LIFE + '    since: 3\n', version=4))
        with self.assertRaisesRegex(SchemaError, 'combat.damage: it is in the API from version 2, before AttackKind, which was added in version 3'):
            load(self.broken('  AttackKind:\n', '  AttackKind:\n    since: 3\n', '  - name: damage\n', '  - name: damage\n    since: 2\n', version=4))
        with self.assertRaisesRegex(SchemaError, 'combat.enums.AttackKind.bubble: it is in the API from version 2, before AttackKind'):
            load(self.broken('  AttackKind:\n', '  AttackKind:\n    since: 3\n', self.BUBBLE, self.BUBBLE[:-1] + ', since: 2}', version=4))
        with self.assertRaisesRegex(SchemaError, 'combat.knockback: it is in the API from version 3, before the service combat'):
            load(self.broken('service: combat\n', 'service: combat\nsince: 4\n', *added, version=4))

        # What the generators make of it: the header and the reference page say it once, at the end of the entry.
        schema = load(self.broken(*added, self.BUBBLE, self.BUBBLE[:-1] + ', since: 2}', version=4))
        combat = next(s for s in schema.services if s.name == 'combat')
        self.assertEqual([combat.since(fn) for fn in combat.functions], [0, 0, 0, 3, 0])
        header = c_header.generate(schema)['include/tsuchi/combat.h']
        self.assertIn('     * Added in version 3. A mod that also runs on a framework older than that asks TSUCHI_HAS\n'
                      '     * before it uses this. */\n    TSUCHI_Result (*knockback)(', header)
        self.assertEqual(header.count('Added in version'), 2)
        self.assertIn('    TSUCHI_ATTACK_KIND_BUBBLE = 7u, /* A bubble that traps the target. Added in version 2. */', header)
        self.assertIn('#define TSUCHI_API_VERSION 4u', c_header.generate(schema)['include/tsuchi/core.h'])
        page = docs_page.generate(schema)['docs/reference/combat.mdx']
        self.assertIn('Fails with `no_memory`: The game is out of memory.\n\nAdded in version 3.\n\n### kill_pikmin', page)
        self.assertIn('| `bubble` | A bubble that traps the target. Added in version 2. |', page)
        self.assertEqual(page.count('Added in version'), 2)
        # A script has the function or does not: nothing in the binding turns on the version.
        self.assertEqual(lua_binding.generate(schema)['lua/combat_lua.cpp'], lua_binding.generate(load(self.broken()))['lua/combat_lua.cpp'])
        # A whole service added later tells it of each of its members, since each is as new as the service.
        schema = load(self.broken('service: combat\n', 'service: combat\nsince: 2\n', *added, version=4))
        combat = next(s for s in schema.services if s.name == 'combat')
        self.assertEqual([combat.since(fn) for fn in combat.functions], [2, 2, 2, 3, 2])
        self.assertIn('    const TSUCHI_CombatService *combat; /* Added in version 2. */', c_header.generate(schema)['include/tsuchi/tsuchi.h'])
        self.assertIn('A Pikmin is either alive or dead.\n\nAdded in version 2.\n\n## Tutorials',
                      docs_page.generate(schema)['docs/reference/combat.mdx'])

    def test_what_is_deprecated_names_what_to_use(self):
        """`deprecated` is the version from which something else is to be used, and names it. The thing still works."""
        def retired(use='damage', version=3):
            return (self.SET_LIFE, self.SET_LIFE + f'    deprecated: {{in: {version}, use: {use}}}\n')

        with self.assertRaisesRegex(SchemaError, 'combat.set_life: deprecated in version 3, and the framework is at version 0'):
            load(self.broken(*retired()))
        for said in ('3', 'true', '{in: 3}', '{use: damage}', '{in: 3, use: ""}', '{in: 3, use: damage, also: life}'):
            with self.assertRaisesRegex(SchemaError, 'combat.set_life: "deprecated" says the version and what to use in its place'):
                load(self.broken(self.SET_LIFE, self.SET_LIFE + f'    deprecated: {said}\n', version=4))
        # Not in the release that first has it: version 1 for what says no `since`.
        with self.assertRaisesRegex(SchemaError, 'combat.set_life: "deprecated" names a version after the one that added it'):
            load(self.broken(*retired(version=1), version=4))
        with self.assertRaisesRegex(SchemaError, 'combat.set_life: "deprecated" names a version after the one that added it'):
            load(self.broken(self.SET_LIFE, self.SET_LIFE + '    since: 3\n    deprecated: {in: 3, use: damage}\n', version=4))
        # What to use is there, is not itself deprecated, and was there by then.
        for use in ('no_such', 'set_life', 'actor.no_such', 'nowhere.damage', 'Life'):
            with self.assertRaisesRegex(SchemaError, f'combat.set_life: deprecated in favor of "{use}", and there is no such thing'):
                load(self.broken(*retired(use), version=4))
        with self.assertRaisesRegex(SchemaError, 'combat.set_life: deprecated in favor of "damage", which is deprecated itself'):
            load(self.broken(*retired(), '  - name: damage\n', '  - name: damage\n    deprecated: {in: 4, use: knockback}\n', version=4))
        with self.assertRaisesRegex(SchemaError, 'combat.set_life: deprecated in version 3 in favor of "damage", which was only added in version 4'):
            load(self.broken(*retired(), '  - name: damage\n', '  - name: damage\n    since: 4\n', version=4))
        with self.assertRaisesRegex(SchemaError, 'combat.enums.AttackKind.bubble: deprecated in favor of "drowned", and there is no such thing'):
            load(self.broken(self.BUBBLE, self.BUBBLE[:-1] + ', deprecated: {in: 2, use: drowned}}', version=4))
        with self.assertRaisesRegex(SchemaError, 'combat.structs.Life.extra: deprecated in favor of "life_left", and there is no such thing'):
            load(self.broken(self.EXTRA, self.EXTRA[:-1] + ', deprecated: {in: 2, use: life_left}}', version=4))
        with self.assertRaisesRegex(SchemaError, 'combat: deprecated in favor of "fights", and there is no such thing'):
            load(self.broken('service: combat\n', 'service: combat\ndeprecated: {in: 2, use: fights}\n', version=4))

        # What the generators make of it. In C the compiler warns where a mod uses the thing; a script's mod is
        # told once in its log; and the reference page says it at the end of the entry, with the way to the other.
        schema = load(self.broken(*retired(), self.BUBBLE, self.BUBBLE[:-1] + ', deprecated: {in: 2, use: water}}',
                                  self.EXTRA, self.EXTRA[:-1] + ', deprecated: {in: 4, use: max_extra}}',
                                  self.DEFEATED, self.DEFEATED + '    deprecated: {in: 3, use: damaged}\n',
                                  '  - name: kill_pikmin\n', '  - name: kill_pikmin\n    since: 2\n    deprecated: {in: 4, use: entity.destroy}\n',
                                  '  LifeFlags:\n', '  LifeFlags:\n    deprecated: {in: 3, use: EntityParts}\n', version=4))
        headers = c_header.generate(schema)
        header = headers['include/tsuchi/combat.h']
        self.assertIn('     * Deprecated in version 3: use `combat.damage`. */\n'
                      '    TSUCHI_Result (*set_life)(TSUCHI_Mod *mod, TSUCHI_Actor actor, float life, TSUCHI_LifeFlags flags) TSUCHI_DEPRECATED("use combat.damage");', header)
        self.assertIn('     * Added in version 2. A mod that also runs on a framework older than that asks TSUCHI_HAS\n     * before it uses this.\n'
                      '     *\n     * Deprecated in version 4: use `entity.destroy`. */\n    TSUCHI_Result (*kill_pikmin)(', header)
        self.assertIn('    TSUCHI_ATTACK_KIND_BUBBLE TSUCHI_DEPRECATED("use water") = 7u, /* A bubble that traps the target. Deprecated in version 2: use `water`. */', header)
        self.assertIn('    float extra TSUCHI_DEPRECATED("use max_extra"); /* Current bonus health,', header)
        self.assertIn('TSUCHI_Subscription *out) TSUCHI_DEPRECATED("use combat.damaged");', header)
        self.assertIn('/* Options for `set_life`. Deprecated in version 3: use `EntityParts`. */', header)
        self.assertIn('#define TSUCHI_DEPRECATED(text) __attribute__((deprecated(text)))', headers['include/tsuchi/core.h'])
        script = lua_binding.generate(schema)['lua/combat_lua.cpp']
        self.assertIn('int l_set_life(lua_State* L) {\n    tsuchi_lua::deprecated(L, "combat.set_life", 3, "combat.damage");\n', script)
        self.assertIn('int l_on_defeated(lua_State* L) {\n    tsuchi_lua::deprecated(L, "combat.on_defeated", 3, "combat.damaged");\n', script)
        self.assertEqual(script.count('tsuchi_lua::deprecated('), 3)
        page = docs_page.generate(schema)['docs/reference/combat.mdx']
        self.assertIn('Fails with `not_ready`: The game isn\'t ready.\n\nDeprecated in version 3: use [`combat.damage`](#damage).\n\n### damage', page)
        self.assertIn('Added in version 2. Deprecated in version 4: use [`entity.destroy`](/reference/entity/#destroy).\n\n## Event descriptions', page)
        self.assertIn('Deprecated in version 3: use [`combat.damaged`](#damaged).\n\n### pikmin_died', page)
        self.assertIn('| `bubble` | A bubble that traps the target. Deprecated in version 2: use `water`. |', page)
        self.assertIn('for actors that don\'t have any. Deprecated in version 4: use `max_extra`. |', page)
        self.assertIn('Deprecated in version 3: use [`EntityParts`](/reference/entity/#entityparts).', page)
        self.assertIn('| [`set_life`](#set_life) | Changes an actor\'s health without a hit. Deprecated. |', page)

        # The headers as a compiler takes them. They are plain C still; a mod that uses none of it is not warned;
        # one that does is warned at each use; and a build that defines the mark away is left in peace.
        folder = Path(self.enterContext(tempfile.TemporaryDirectory()))
        for relative, text in headers.items():
            (folder / relative).parent.mkdir(parents=True, exist_ok=True)
            (folder / relative).write_text(text, encoding='utf-8')
        compile_c = [shutil.which('clang'), '-std=c11', '-Wall', '-Wextra', '-Werror', '-pedantic', '-fsyntax-only', '-I', folder / 'include']
        head = '#include "tsuchi/tsuchi.h"\nint uses(const TSUCHI_Host *host, TSUCHI_Mod *mod, TSUCHI_Actor actor, TSUCHI_Life *life) {\n    '
        careful = folder / 'careful.c'
        careful.write_text(head + 'return host->combat->life(mod, actor, life) + TSUCHI_ATTACK_KIND_WATER + (int)life->max_extra;\n}\n', encoding='utf-8')
        run([*compile_c, careful])
        uses = {'function': 'return host->combat->set_life(mod, actor, life->life, 0);', 'value': '(void)host, (void)mod, (void)actor, (void)life;\n    return TSUCHI_ATTACK_KIND_BUBBLE;',
                'field': '(void)host, (void)mod, (void)actor;\n    return (int)life->extra;',
                'event': '(void)actor, (void)life;\n    return host->combat->on_defeated(mod, 0, 0, 0);'}
        advice = {'function': 'use combat.damage', 'value': 'use water', 'field': 'use max_extra', 'event': 'use combat.damaged'}
        for what, body in uses.items():
            source = folder / f'{what}.c'
            source.write_text(head + body + '\n}\n', encoding='utf-8')
            refused = subprocess.run([str(c) for c in (*compile_c, source)], capture_output=True, text=True)
            self.assertNotEqual(refused.returncode, 0, what)
            self.assertIn(f"is deprecated: {advice[what]}", refused.stderr, what)
            run([*compile_c, '-DTSUCHI_DEPRECATED(text)=', source])

    def test_the_lock_keeps_the_version_a_thing_was_added_in(self):
        """Once released, the version that added a member is a fact about frameworks people have, and so is its place."""
        recorded = lock.read(ROOT / 'schema')
        self.assertNotIn('marks', recorded)  # nothing says anything of versions yet
        self.assertNotIn('marks', lock.fresh(load(ROOT / 'schema')))
        added = (self.KNOCKBACK, self.KNOCKBACK + '    since: 3\n')
        retired = (self.SET_LIFE, self.SET_LIFE + '    deprecated: {in: 3, use: damage}\n')

        def folder(with_lock, *edits):
            made = self.broken(*edits, version=4)
            (made / lock.NAME).write_text(json.dumps(with_lock), encoding='utf-8')
            return made

        # A lock that does not have what the schema says is out of date, as with a member it has no place for.
        with self.assertRaisesRegex(SchemaError, 'combat.knockback: schema/abi.lock and the schema do not agree on its version'):
            load(folder(recorded, *added))
        schema = load(self.broken(*added, *retired, version=4))
        made = lock.fresh(schema)
        unreleased = lock.fresh(load(ROOT / 'schema'))
        self.assertEqual(made['marks'], {'combat.knockback': {'since': 3}, 'combat.set_life': {'deprecated': 3}})
        self.assertEqual({key: value for key, value in made.items() if key not in ('marks', 'api_version')},
                         {key: value for key, value in unreleased.items() if key not in ('api_version', 'release_version')})
        load(folder(made, *added, *retired))
        with self.assertRaisesRegex(SchemaError, 'combat.set_life: schema/abi.lock and the schema do not agree on its version'):
            load(folder(made, *added))

        # A released table is in the order its members were added: a framework has everything up to the length of
        # its table, so what was added in version 3 cannot sit before what the first release had.
        made['released'] = True
        with self.assertRaisesRegex(SchemaError, 'combat.kill_pikmin: its place in schema/abi.lock is after knockback, which was added in version 3'):
            load(folder(made, *added, *retired))
        members = made['tables']['combat']
        members.append(members.pop(members.index('knockback')))
        load(folder(made, *added, *retired))
        self.assertEqual(lock.grown(schema, made), made)
        # And what was released does not change its mind.
        for edits, problem in (((self.KNOCKBACK, self.KNOCKBACK + '    since: 2\n', *retired), 'combat.knockback: released as added in version 3, and the schema says added in version 2'),
                               (retired, 'combat.knockback: released as added in version 3, and the schema says there from the first release'),
                               ((*added, *retired, '  - name: damage\n', '  - name: damage\n    since: 2\n'), 'combat.damage: released as there from the first release, and the schema says added in version 2'),
                               ((*added, self.SET_LIFE, self.SET_LIFE + '    deprecated: {in: 4, use: damage}\n'), 'combat.set_life: released as deprecated in version 3'),
                               (added, 'combat.set_life: released as deprecated in version 3')):
            with self.assertRaisesRegex(SchemaError, problem):
                lock.grown(load(self.broken(*edits, version=4)), made)
        # What is new goes to the end with its version, and a thing may be deprecated after its release.
        later = lock.grown(load(self.broken(*added, *retired, '  - name: life\n', '  - name: life\n    deprecated: {in: 4, use: knockback}\n',
                                            '\nevents:\n', '\n  - name: stagger\n    since: 4\n    doc: Makes an actor stagger where it stands.\n'
                                            '    params:\n      - {name: actor, type: handle<actor>, doc: The actor.}\n'
                                            '    users: [groink, online-coop]\n\nevents:\n', version=4)), made)
        self.assertEqual(later['tables']['combat'], made['tables']['combat'] + ['stagger'])
        self.assertEqual(later['marks'], {'combat.knockback': {'since': 3}, 'combat.life': {'deprecated': 4}, 'combat.set_life': {'deprecated': 3},
                                          'combat.stagger': {'since': 4}})

    def test_a_pointer_is_for_compiled_mods(self):
        with self.assertRaisesRegex(SchemaError, 'only: native'):
            load(self.broken('type: handle<actor>, doc: The actor to read.', 'type: pointer, doc: The actor to read.'))

    def test_a_function_is_for_both_or_for_one(self):
        with self.assertRaisesRegex(SchemaError, '"only" is'):
            load(self.broken('- name: life', '- name: life\n    only: sometimes'))

    def test_a_default_is_a_value_of_its_enum(self):
        with self.assertRaisesRegex(SchemaError, 'not a value of AttackKind'):
            load(self.broken('default: normal', 'default: gentle'))

    # The kinds of hit as they would be if mods could add to them: an open enum the game keeps as numbers.
    OPENED = ('  AttackKind:\n    open: false\n', '  AttackKind:\n    open: true\n    most: 255\n')

    def test_an_enum_says_whether_it_is_open(self):
        """Closed, the game decides the set and it is numbers with names. Open, mods add to it and it is IDs."""
        with self.assertRaisesRegex(SchemaError, 'combat.enums.AttackKind: an enum says whether it is open'):
            load(self.broken('  AttackKind:\n    open: false\n', '  AttackKind:\n'))
        with self.assertRaisesRegex(SchemaError, 'whether it is open'):
            load(self.broken('  AttackKind:\n    open: false\n', '  AttackKind:\n    open: perhaps\n'))
        with self.assertRaisesRegex(SchemaError, 'combat.flags.LifeFlags: a set of flags is always closed'):
            load(self.broken('  LifeFlags:\n', '  LifeFlags:\n    open: true\n'))
        # A closed enum lists its values, and has no use for the largest number the game can hold.
        with self.assertRaisesRegex(SchemaError, '"most" is for an open enum with values'):
            load(self.broken('  AttackKind:\n    open: false\n', '  AttackKind:\n    open: false\n    most: 255\n'))
        # An open enum the game keeps as numbers says how far the numbers go, which is no less than the ones it lists.
        with self.assertRaisesRegex(SchemaError, 'the largest number the game can hold'):
            load(self.broken('  AttackKind:\n    open: false\n', '  AttackKind:\n    open: true\n'))
        with self.assertRaisesRegex(SchemaError, 'the largest number the game can hold'):
            load(self.broken('  AttackKind:\n    open: false\n', '  AttackKind:\n    open: true\n    most: 33\n'))
        combat = next(s for s in load(self.broken(*self.OPENED)).services if s.name == 'combat')
        kinds = next(e for e in combat.enums if e.name == 'AttackKind')
        self.assertTrue(kinds.open and kinds.numbered and kinds.most == 255)
        self.assertFalse(next(e for e in combat.enums if e.name == 'DeathCause').open)

    def test_an_id_the_framework_does_not_know_fails_one_way(self):
        """A function that takes an ID says when it fails with `invalid` and with `not_found`, whatever the service."""
        opened = ('  DeathCause:\n    open: false\n', '  DeathCause:\n    open: true\n    most: 255\n')
        with self.assertRaisesRegex(SchemaError, 'combat.kill_pikmin.cause: a function that takes an ID fails with "invalid"'):
            load(self.broken(*opened))
        load(self.broken(*opened, '      not_ready: "The game can\'t remove actors right now."',
                         '      not_ready: "The game can\'t remove actors right now."\n      invalid: "`cause` is not spelled as an ID is."'))
        # A default is an ID too: one of the listed names.
        with self.assertRaisesRegex(SchemaError, 'not a value of AttackKind'):
            load(self.broken(*self.OPENED, 'default: normal', 'default: gentle'))

    def test_an_id_in_a_struct_is_only_given(self):
        """A struct a function fills in holds an ID whole. One that a function takes would have to trust the text in it."""
        field = ('      - {name: max_extra, type: f32, doc: "Maximum bonus health."}',
                 '      - {name: max_extra, type: f32, doc: "Maximum bonus health."}\n      - {name: weak_to, type: enum<AttackKind>, doc: What hurts it most.}')
        load(self.broken(*self.OPENED, *field))
        with self.assertRaisesRegex(SchemaError, 'combat.set_life.flags: a struct with an ID in it is only returned'):
            load(self.broken(*self.OPENED, *field, '{name: flags, type: flags<LifeFlags>, default: 0, doc: Options.}',
                             '{name: flags, type: struct<Life>, doc: Options.}'))

    def test_an_open_enum_is_text_in_c_and_in_a_script(self):
        """What the generators make of an open enum: an ID in every form, and a table of the game's own numbers."""
        field = ('      - {name: max_extra, type: f32, doc: "Maximum bonus health."}',
                 '      - {name: max_extra, type: f32, doc: "Maximum bonus health."}\n      - {name: weak_to, type: enum<AttackKind>, doc: What hurts it most.}')
        schema = load(self.broken(*self.OPENED, *field))
        header = c_header.generate(schema)['include/tsuchi/combat.h']
        self.assertIn('#define TSUCHI_ATTACK_KIND_FIRE "fire" /* Fire. */', header)
        self.assertNotIn('TSUCHI_AttackKind', header)
        self.assertIn('float amount, const char* kind, TSUCHI_Actor attacker);', header)   # an argument
        self.assertIn('    char weak_to[TSUCHI_ID_SIZE]; /* What hurts it most. */', header)  # in a struct a function fills in
        self.assertIn('    const char* attack; /*', header)                                 # in an event
        self.assertIn('typedef uint32_t TSUCHI_DeathCause;', header)                         # a closed enum is as it was
        self.assertIn('#define TSUCHI_ID_SIZE 96', c_header.generate(schema)['include/tsuchi/core.h'])
        glue = service_glue.generate(schema)
        self.assertIn('if (!id_valid(kind)) return TSUCHI_INVALID;', glue['framework/combat_service.cpp'])
        self.assertNotIn('valid_AttackKind', glue['framework/combat_service.cpp'])
        self.assertIn('constexpr IdName AttackKind_own[] = {{"normal", 0}, {"direct_hit", 1},', glue['framework/combat_service.cpp'])
        self.assertIn('Vocabulary AttackKind_vocabulary{"combat.AttackKind", AttackKind_own, 12, 255};', glue['framework/combat_service.cpp'])
        self.assertIn('extern Vocabulary AttackKind_vocabulary;', glue['framework/combat_service.h'])
        script = lua_binding.generate(schema)['lua/combat_lua.cpp']
        self.assertIn('const char* kind = luaL_optstring(L, 3, "normal");', script)
        self.assertIn('lua_pushstring(L, value.weak_to);', script)
        self.assertNotIn('AttackKind_names', script)
        self.assertIn('DeathCause_names', script)
        # A closed enum's value that has no name is a string to a script as well, never a number.
        self.assertIn('tsuchi_lua::push_unnamed(L, value);', script)
        self.assertNotIn('lua_pushinteger(L, value)', script)
        page = docs_page.generate(schema)['docs/reference/combat.mdx']
        self.assertIn('### AttackKind\n\nThe kind of hit. This decides how the target reacts.\n\nThis set is open, so mods can add to it.', page)
        self.assertIn('### DeathCause\n\nThe cause of a Pikmin\'s death, as recorded by the game.\n\nThis set is closed, so mods can\'t add to it.', page)


class Generated(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.files = generate.write(GEN)

    def test_headers_are_plain_c(self):
        core = self.files['include/tsuchi/core.h']
        for name in ('dialog', 'pause_hold', 'hook_id', 'prepared_call'):
            self.assertNotIn(f'tsuchi_{name}_as_object', core)
        for service in load(ROOT / 'schema').services:
            table = 'TSUCHI_' + ''.join(part.capitalize() for part in service.name.split('_')) + 'Service'
            self.assertIn(f'typedef struct {table}', self.files[f'include/tsuchi/{service.name}.h'])
        source = BUILD / 'header_check.c'
        source.write_text('#include "tsuchi/tsuchi.h"\nint main(void) { return sizeof(TSUCHI_Host) ? 0 : 1; }\n', encoding='utf-8')
        run([shutil.which('clang'), '-std=c11', '-Wall', '-Wextra', '-Werror', '-pedantic', '-fsyntax-only',
             '-I', GEN / 'include', source])

    def test_the_core_keeps_its_promises(self):
        program = BUILD / f'registry_test{EXE}'
        run([shutil.which('clang++'), *CXX, '-I', GEN / 'include', '-I', CORE,
             CORE / 'registry.cpp', ROOT / 'tests' / 'core' / 'registry_test.cpp', '-o', program])
        run([program])

    def test_ids_are_spelled_one_way(self):
        """How an ID is spelled, and what a name the framework does not know comes to, in both directions."""
        program = BUILD / f'ids_test{EXE}'
        run([shutil.which('clang++'), *CXX, '-I', GEN / 'include', '-I', CORE,
             CORE / 'registry.cpp', CORE / 'ids.cpp', ROOT / 'tests' / 'core' / 'ids_test.cpp', '-o', program])
        run([program])

    def test_json_is_read_strictly(self):
        program = BUILD / f'json_test{EXE}'
        run([shutil.which('clang++'), *CXX, '-I', GEN / 'include', '-I', CORE,
             CORE / 'registry.cpp', CORE / 'json.cpp', ROOT / 'tests' / 'core' / 'json_test.cpp', '-o', program])
        run([program])

    def test_modules_are_found_and_module_files_checked(self):
        """The hash, a loaded module's names, and what a module file and its list have to be, against made-up ones."""
        fixture = self.fixture()
        program = BUILD / f'modules_test{EXE}'
        run([shutil.which('clang++'), *CXX, '-I', GEN / 'include', '-I', fixture / 'framework', '-I', BASE, '-I', CORE, '-I', ROOT / 'tests' / 'base',
             CORE / 'sha256.cpp', CORE / 'registry.cpp', BASE / 'modules.cpp', ROOT / 'tests' / 'base' / 'modules_test.cpp', '-o', program])
        run([program])

    def test_a_compiled_mod_builds_with_the_sdk(self):
        """The SDK's tool builds the checks' own compiled mods, and refuses a mod the framework could not start."""
        sys.path.insert(0, str(ROOT / 'sdk'))
        import build_mod
        mods = ROOT / 'tests' / 'fixtures' / 'mods'
        out = BUILD / 'sdk-check'
        for name, source in (('check.compiled', 'main.cpp'), ('check.compiled-refuses', 'main.c')):
            size = build_mod.build([mods / name / 'source' / source], out / name / 'mod.nro')
            image = (out / name / 'mod.nro').read_bytes()
            self.assertEqual(size, len(image))
            self.assertEqual(len(image) % 0x1000, 0)
            self.assertEqual(image[0x10:0x14], b'NRO0')
            self.assertEqual(struct.unpack_from('<I', image, 0x18)[0], len(image))
            self.assertIn(b'tsuchi_mod_start\0', image)
        # What the compiler warns of reaches whoever builds the mod: that is how a use of something deprecated is told.
        warned = out / 'warned'
        warned.mkdir(parents=True, exist_ok=True)
        (warned / 'main.c').write_text('#include "tsuchi/tsuchi.h"\n#warning told to the author\n'
                                       'TSUCHI_Result tsuchi_mod_start(const TSUCHI_Host *host, TSUCHI_Mod *mod) { (void)host, (void)mod; return TSUCHI_OK; }\n',
                                       encoding='utf-8', newline='\n')
        warnings = []
        build_mod.build([warned / 'main.c'], warned / 'mod.nro', warnings=warnings)
        self.assertIn('told to the author', ''.join(warnings))
        # The first mod keeps 6,000 bytes that start as zeros, which are not in its file.
        compiled = (out / 'check.compiled' / 'mod.nro').read_bytes()
        self.assertGreaterEqual(struct.unpack_from('<I', compiled, 0x38)[0], 0x2000)
        bad = out / 'bad'
        bad.mkdir(parents=True, exist_ok=True)
        cases = {'no_start.c': '#include "tsuchi/tsuchi.h"\nint nothing(void) { return 1; }\n',
                 'undefined.c': '#include "tsuchi/tsuchi.h"\nint elsewhere(int);\n'
                                'TSUCHI_Result tsuchi_mod_start(const TSUCHI_Host *host, TSUCHI_Mod *mod) { (void)host, (void)mod; return elsewhere(1) ? TSUCHI_OK : TSUCHI_INVALID; }\n'}
        for name, text in cases.items():
            (bad / name).write_text(text, encoding='utf-8', newline='\n')
            with self.assertRaises(build_mod.BuildError, msg=name):
                build_mod.build([bad / name], bad / 'mod.nro')
        with self.assertRaises(build_mod.BuildError):
            build_mod.build([bad / 'absent.c'], bad / 'mod.nro')

    def test_the_checks_are_read_from_the_log(self):
        """What tools/read_checks.py makes of a log: where each check ended, and what stands in the way of a clean run."""
        def read(lines, staged=()):
            return read_checks.read('\n'.join(lines) + '\n', staged)

        clean = ['[tsuchi] api 0, game 1.1.1: 3 mods, 2 running, 1 blocked', '[tsuchi] mod check.one 1.0.0 running', '[tsuchi] mod check.two 1.0.0 running',
                 '[tsuchi] mod check.three blocked: mod.json, line 4, column 1: a value is missing', '[check.one] check kept planned', '[check.one] check left planned',
                 '[check.two] check read planned', '[check.one] some other line', '[check.one] check kept waiting the save: busy', '[other.mod] checkers are a game',
                 '[check.one] check kept pass', '[check.one] check left pass fresh', '[check.two] check read pass']
        result = read(clean, ['check.one', 'check.two'])
        self.assertEqual((result['exit'], result['counts']), (0, {'pass': 3, 'fail': 0, 'not done': 0}))
        self.assertEqual([(c['mod'], c['name'], c['state'], c['detail']) for c in result['checks']],
                         [('check.one', 'kept', 'pass', ''), ('check.one', 'left', 'pass', 'fresh'), ('check.two', 'read', 'pass', '')])
        self.assertEqual(result['mods']['check.three'], 'blocked: mod.json, line 4, column 1: a value is missing')
        self.assertEqual(read_checks.table(result).splitlines()[-1], '3 checks: 3 passed, 0 failed, 0 not done')
        # A check that failed, with why. One failure is enough for the run.
        result = read(clean + ['[check.two] check second planned', '[check.two] check second fail the size: got 5, wanted 6'])
        self.assertEqual((result['exit'], result['checks'][-1]['state'], result['checks'][-1]['detail']), (1, 'fail', 'the size: got 5, wanted 6'))
        self.assertIn('check.two  second  fail  the size: got 5, wanted 6', read_checks.table(result))
        # A check that was planned and never ended is not done, and its last reason for waiting is why.
        result = read(clean + ['[check.two] check level planned', '[check.two] check level waiting a level', '[check.two] check level waiting the player in control',
                               '[check.two] check bare planned'])
        self.assertEqual((result['exit'], result['counts']['not done']), (2, 2))
        self.assertEqual([c['detail'] for c in result['checks'][-2:]], ['waiting the player in control', ''])
        self.assertIn('check.two  level  not done  waiting the player in control', read_checks.table(result))
        # A check mod that was installed and said nothing: the framework's own line about it is the reason.
        result = read(clean, ['check.one', 'check.two', 'check.three', 'check.four'])
        self.assertEqual(result['exit'], 1)
        self.assertEqual(result['silent'], {'check.three': 'blocked: mod.json, line 4, column 1: a value is missing', 'check.four': 'the log has nothing of it'})
        self.assertEqual(read_checks.table(result).splitlines()[-1], '3 checks: 3 passed, 0 failed, 0 not done; 2 check mods reported nothing')
        # A check ends once.
        result = read(clean + ['[check.two] check read fail after all'])
        self.assertEqual((result['exit'], result['mistakes'], result['checks'][-1]['state']), (1, ['check.two: check read ended twice'], 'pass'))
        # A log that filled up may lack checks that came later: the run is not clean.
        result = read(clean + ['[tsuchi] the log is full: what is written after this is not in the file'])
        self.assertEqual((result['exit'], result['full']), (2, True))
        # Missing, torn and malformed reports cannot turn into an empty or partial success.
        self.assertEqual(read([], [])['exit'], 2)
        self.assertEqual(read_checks.read('[check.one] check one planned\n[check.one] check one pass')['exit'], 2)
        self.assertIn('the log ends within a line', read_checks.table(read_checks.read('[check.one] check one planned')))
        self.assertEqual(read(['[check.one] check one pass'])['exit'], 1)
        self.assertEqual(read(['[check.one] check one planned', '[check.one] check one planned'])['exit'], 1)
        self.assertEqual(read(clean + ['[check.one] check invalid/name pass'])['exit'], 1)
        self.assertEqual(read(clean + ['[tsuchi] mod check.one blocked: an error after its checks'], ['check.one', 'check.two'])['exit'], 1)
        result = read(clean + ['[tsuchi] mod check.one not running'], ['check.one', 'check.two'])
        self.assertEqual((result['exit'], result['mods']['check.one']), (1, 'not running'))
        self.assertIn('check.one: the framework reports not running', result['mistakes'])
        # The stage receipt's declared names expose even a check whose planned and terminal lines were both lost.
        result = read_checks.read('\n'.join(clean) + '\n', ['check.one', 'check.two'], {'check.one': ['kept', 'left', 'lost']})
        self.assertEqual((result['exit'], result['checks'][-1]['name'], result['checks'][-1]['detail']), (2, 'lost', 'no report for a staged check'))
        result = read_checks.read('', ['check.one'], {'check.one': ['lost']})
        self.assertEqual((result['exit'], result['silent'], result['counts']['not done']), (1, {'check.one': 'the log has nothing of it'}, 1))
        # What an emulator leaves between two writes, and a line that is not the log's, are passed over.
        result = read(['\0\0[check.one] check kept planned', 'no mod wrote this', '[check.one] check kept pass\0'])
        self.assertEqual((result['exit'], result['counts']['pass']), (0, 1))

    def host_lua(self, own_library):
        """The program that runs a script on the framework's Lua, on this machine's C library or on the framework's own."""
        program = BUILD / f'lua_{"own" if own_library else "host"}{EXE}'
        run([shutil.which('clang'), '-std=c99', '-O1', '-w', '-I', LUA, *SCRIPT_CONFIG, ROOT / 'tests' / 'lua' / 'run_script.c',
             *script_lua_objects(own_library), '-o', program])
        return program

    @unittest.skipUnless(shutil.which('ld.lld'), 'needs ld.lld')
    def test_the_check_mods_are_laid_out(self):
        """tools/stage_test_mods.py lays out every fixture, or with names the check mods of those names and no other check mod."""
        self.host_lua(False)
        target = BUILD / 'test-mods-check'
        checks = stage_test_mods.check_mods()
        self.assertIn('check.storage', checks)
        self.assertEqual(len(set(checks.values())), len(checks), 'two check mods make a check of the same name')
        defaults = {mod: group for mod, group in checks.items() if not stage_test_mods.explicit_check(MODS / mod)}
        self.assertNotIn('check.fight', defaults)
        every = stage_test_mods.stage(target=target)
        mods = target / 'romfs' / 'tsuchi' / 'mods'
        self.assertEqual(sorted(path.name for path in mods.iterdir()), every)
        self.assertEqual(json.loads((target / 'checks.json').read_text(encoding='utf-8')), {'check_mods': defaults, 'checks': stage_test_mods.expected_checks(defaults)})
        self.assertFalse((target / 'romfs' / 'tsuchi' / 'checks.txt').exists())
        for mod in defaults:
            # A compiled one is built with the SDK as any mod is, and a script has the SDK's helper beside it.
            compiled, script = (MODS / mod / 'source').is_dir(), (MODS / mod / 'main.lua').is_file()
            self.assertTrue(compiled or script, mod)
            self.assertEqual((mods / mod / 'mod.nro').is_file(), compiled)
            self.assertFalse((mods / mod / 'source').exists())
            if script:
                self.assertEqual((mods / mod / 'check.lua').read_bytes(), (SDK_CHECK / 'check.lua').read_bytes())
        # With names: those check mods, and every folder that is no check mod, which the check build's own checks look at.
        fixtures = [name for name in every if name not in checks]
        some = stage_test_mods.stage(['storage', 'spawn'], target=target)
        self.assertEqual(some, sorted(fixtures + ['check.storage', 'check.spawn']))
        self.assertEqual(sorted(path.name for path in mods.iterdir()), some)
        self.assertEqual(json.loads((target / 'checks.json').read_text(encoding='utf-8')), {'check_mods': {'check.storage': 'storage', 'check.spawn': 'spawn'}, 'checks': stage_test_mods.expected_checks(['check.storage', 'check.spawn'])})
        self.assertEqual((target / 'romfs' / 'tsuchi' / 'checks.txt').read_bytes(), b'storage\nspawn\n')
        # An explicit destructive group has its own exact receipt and never comes along by default.
        explicit = stage_test_mods.stage(['fight'], target=target)
        self.assertEqual(explicit, sorted(fixtures + ['check.fight']))
        self.assertEqual(json.loads((target / 'checks.json').read_text(encoding='utf-8')),
                         {'check_mods': {'check.fight': 'fight'}, 'checks': stage_test_mods.expected_checks(['check.fight'])})
        self.assertEqual(sorted(path.name for path in mods.iterdir()), explicit)

    def test_a_builds_targets_are_its_own(self):
        """With a build, the targets the stager writes are that build's numbers, and the checks' own entries the base's."""
        for build in sorted((ROOT / 'game' / 'builds').glob('*.yaml')):
            table = game_table.load(ROOT / 'game' / 'table.yaml', None, build)
            staged = stage_test_mods.staged_table(build)
            self.assertEqual(staged['slots']['engine_tick'], table['slots']['engine_tick'], build.stem)
            self.assertIn('tick_state_actor_table', staged['data'], build.stem)
            self.assertIn('actor_request_tick', staged['functions'], build.stem)
            own = stage_test_mods.BUILD_FIXTURES / build.name
            if own.is_file():
                # The checks' own entries are the build's where it gives them.
                given = game_table.load(ROOT / 'game' / 'table.yaml', own)
                self.assertEqual(staged['data']['tick_state_actor_table'], given['data']['tick_state_actor_table'], build.stem)
                self.assertEqual(staged['structs']['ai_start_observation'], given['structs']['ai_start_observation'], build.stem)
        base = stage_test_mods.staged_table()
        self.assertEqual(base['slots']['engine_tick'], game_table.load(ROOT / 'game' / 'table.yaml')['slots']['engine_tick'])


    def test_the_language_is_the_same_on_the_frameworks_own_c_library(self):
        """A script that uses the whole language gives the same text on this machine's C library and on the framework's."""
        told = []
        for own_library in (False, True):
            told.append(run([self.host_lua(own_library), ROOT / 'tests' / 'lua' / 'language.lua']).stdout)
        self.assertGreater(told[0].count('\n'), 40)
        self.assertEqual(told[0], told[1])

    def fixture(self):
        """The made-up game's table, generated into its own folder."""
        fixture = BUILD / 'gen-fixture'
        for relative, text in game_table.generate(ROOT / 'tests' / 'fixtures' / 'game_table.yaml',
                                                  'tests/fixtures/game_table.yaml').items():
            path = fixture / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            if not path.exists() or path.read_text(encoding='utf-8') != text:
                path.write_text(text, encoding='utf-8', newline='\n')
        return fixture

    def test_the_base_reads_a_game_from_its_table(self):
        """Names, objects, handles and classes, against a made-up game with its own layout."""
        fixture = self.fixture()
        program = BUILD / f'base_test{EXE}'
        run([shutil.which('clang++'), *CXX, '-I', GEN / 'include', '-I', fixture / 'framework', '-I', BASE, '-I', CORE,
             '-I', ROOT / 'tests' / 'base', BASE / 'names.cpp', BASE / 'objects.cpp', CORE / 'registry.cpp',
             ROOT / 'tests' / 'base' / 'base_test.cpp', '-o', program])
        run([program])

    def test_hooks_come_and_go_in_any_order(self):
        fixture = self.fixture()
        program = BUILD / f'hooks_test{EXE}'
        run([shutil.which('clang++'), *CXX, '-I', GEN / 'include', '-I', fixture / 'framework', '-I', GEN / 'framework',
             '-I', BASE, '-I', CORE, '-I', ROOT / 'tests' / 'base', CORE / 'registry.cpp', CORE / 'ids.cpp', CORE / 'hooks.cpp',
             *sorted((GEN / 'framework').glob('*.cpp')), ROOT / 'tests' / 'core' / 'hooks_test.cpp', '-o', program])
        run([program])

    def test_dynamic_hooks_carry_script_hooks(self):
        """Gateways, signatures, the barrier on mod calls, watches and removal, the base of script hooks."""
        fixture = self.fixture()
        program = BUILD / f'dynamic_hooks_test{EXE}'
        run([shutil.which('clang++'), *CXX, '-I', GEN / 'include', '-I', fixture / 'framework', '-I', GEN / 'framework',
             '-I', BASE, '-I', CORE, '-I', ROOT / 'tests' / 'base', CORE / 'registry.cpp', CORE / 'ids.cpp', CORE / 'hooks.cpp',
             CORE / 'dynamic_hooks.cpp', *sorted((GEN / 'framework').glob('*.cpp')),
             ROOT / 'tests' / 'core' / 'dynamic_hooks_test.cpp', '-o', program])
        self.assertIn('dynamic hooks passed', run([program]).stdout)

    def test_fields_and_functions_are_reached_by_name(self):
        """The reflect service end to end, against a made-up game that describes itself."""
        fixture = self.fixture()
        program = BUILD / f'reflect_test{EXE}'
        run([shutil.which('clang++'), *CXX, '-I', GEN / 'include', '-I', fixture / 'framework', '-I', GEN / 'framework',
             '-I', BASE, '-I', CORE, '-I', ROOT / 'tests' / 'base', CORE / 'registry.cpp', CORE / 'ids.cpp', CORE / 'reflection.cpp', CORE / 'json.cpp', CORE / 'sha256.cpp', CORE / 'text.cpp', CORE / 'texts.cpp',
             *sorted(BASE.glob('*.cpp')), *sorted((GEN / 'framework').glob('*.cpp')),
             ROOT / 'tests' / 'base' / 'reflect_test.cpp', '-o', program])
        run([program])

    def containers_program(self):
        fixture = self.fixture()
        program = BUILD / f'containers_test{EXE}'
        run([shutil.which('clang++'), *CXX, '-I', GEN / 'include', '-I', fixture / 'framework', '-I', GEN / 'framework',
             '-I', BASE, '-I', CORE, '-I', ROOT / 'tests' / 'base', CORE / 'registry.cpp', CORE / 'ids.cpp', CORE / 'reflection.cpp',
             CORE / 'json.cpp', CORE / 'sha256.cpp', CORE / 'text.cpp', CORE / 'texts.cpp', *sorted(BASE.glob('*.cpp')),
             *sorted((GEN / 'framework').glob('*.cpp')), ROOT / 'tests' / 'base' / 'containers_test.cpp', '-o', program])
        return program

    def test_mods_containers_are_mounted_in_order(self):
        """Mods' containers against a made-up game: mount order and engine order, blocked mods, names, and packages in common."""
        run([self.containers_program()])

    def test_a_script_reaches_fields_and_functions_by_name(self):
        """The same service from Lua: plain values in and out, and a function called in one step."""
        fixture = self.fixture()
        program = BUILD / f'reflect_lua_test{EXE}'
        run([shutil.which('clang++'), *CXX, '-I', GEN / 'include', '-I', fixture / 'framework', '-I', GEN / 'framework',
             '-I', BASE, '-I', CORE, '-I', ROOT / 'tests' / 'base', '-I', ROOT / 'runtime' / 'lua', '-I', LUA,
             CORE / 'registry.cpp', CORE / 'ids.cpp', CORE / 'reflection.cpp', CORE / 'json.cpp', CORE / 'sha256.cpp', CORE / 'text.cpp', CORE / 'texts.cpp', *sorted(BASE.glob('*.cpp')),
             *sorted((GEN / 'framework').glob('*.cpp')), *sorted((GEN / 'lua').glob('*.cpp')),
             *sorted((ROOT / 'runtime' / 'lua').glob('*.cpp')), ROOT / 'tests/host/no_dynamic_hooks.cpp', ROOT / 'tests' / 'base' / 'reflect_lua_test.cpp',
             *lua_objects(), '-o', program])
        run([program, ROOT / 'tests' / 'base' / 'reflect.lua'])

    def test_shader_renderer_setters_keep_restoration_guards(self):
        """Actual renderer dispatch and admission, including null and texture brush originals."""
        fixture = self.fixture()
        program = BUILD / f'shader_renderer_guards{EXE}'
        run([shutil.which('clang++'), *CXX, '-I', GEN / 'include', '-I', fixture / 'framework', '-I', GEN / 'framework',
             '-I', BASE, '-I', CORE, '-I', GAME, '-I', ROOT / 'tests' / 'base',
             CORE / 'registry.cpp', CORE / 'ids.cpp', CORE / 'reflection.cpp', CORE / 'json.cpp', CORE / 'sha256.cpp', CORE / 'text.cpp', CORE / 'texts.cpp',
             *sorted(BASE.glob('*.cpp')), *sorted((GEN / 'framework').glob('*.cpp')), GAME / 'parts.cpp',
             ROOT / 'tests' / 'game' / 'shader_renderer_guards.cpp', '-o', program])
        run([program])

    def test_added_pikmin_manifest_ids(self):
        """Parsed declarations register qualified IDs once and release them when the provider stops."""
        program = BUILD / f'pikmin_content_test{EXE}'
        run([shutil.which('clang++'), *CXX, '-I', GEN / 'include', '-I', GEN / 'framework',
             '-I', CORE, '-I', BASE, '-I', GAME,
             CORE / 'registry.cpp', CORE / 'ids.cpp', CORE / 'json.cpp', CORE / 'text.cpp', CORE / 'texts.cpp',
             GAME / 'records.cpp', GEN / 'framework' / 'pikmin_service.cpp',
             ROOT / 'tests' / 'game' / 'pikmin_content_test.cpp', '-o', program])
        run([program])

    def test_added_pikmin_saved_army_births(self):
        """Native restore limits, stock ordering and decoration counts admit only safe added births."""
        program = BUILD / f'pikmin_birth_test{EXE}'
        run([shutil.which('clang++'), *CXX, '-I', GEN / 'include', '-I', GEN / 'framework',
             '-I', CORE, '-I', BASE, '-I', GAME, GAME / 'pikmin_birth.cpp', ROOT / 'tests' / 'game' / 'pikmin_birth_test.cpp', '-o', program])
        run([program])

    def test_added_pikmin_native_admission(self):
        """Engine-owned sparse maps and enum names retain stock identity and reject collisions."""
        program = BUILD / f'pikmin_native_test{EXE}'
        run([shutil.which('clang++'), *CXX, '-I', GEN / 'include', '-I', GEN / 'framework',
             '-I', BASE, '-I', GAME, GAME / 'pikmin_native.cpp', ROOT / 'tests' / 'game' / 'pikmin_native_test.cpp', '-o', program])
        run([program])

    def test_added_pikmin_save_storage(self):
        """Added colors survive native storage growth and explicit lifecycle resets."""
        program = BUILD / f'pikmin_save_test{EXE}'
        run([shutil.which('clang++'), *CXX, '-I', GEN / 'include', '-I', GEN / 'framework',
             '-I', CORE, '-I', BASE, '-I', GAME, GAME / 'pikmin_save.cpp', ROOT / 'tests' / 'game' / 'pikmin_save_test.cpp', '-o', program])
        run([program])

    def test_added_pikmin_native_counts(self):
        """The game's Pikmin totals count the added colors, and an added color's icon gets its own count."""
        program = BUILD / f'pikmin_counts_test{EXE}'
        run([shutil.which('clang++'), *CXX, '-I', GEN / 'include', '-I', GEN / 'framework',
             '-I', CORE, '-I', BASE, '-I', GAME, GAME / 'pikmin_counts.cpp', ROOT / 'tests' / 'game' / 'pikmin_counts_test.cpp', '-o', program])
        run([program])

    def test_added_pikmin_follow_snapshot(self):
        """Native classification preserves stock rows and refuses unsafe added-color snapshots."""
        program = BUILD / f'pikmin_snapshot_test{EXE}'
        run([shutil.which('clang++'), *CXX, '-I', GEN / 'include', '-I', GEN / 'framework',
             '-I', CORE, '-I', BASE, '-I', GAME, GAME / 'pikmin_snapshot.cpp', ROOT / 'tests' / 'game' / 'pikmin_snapshot_test.cpp', '-o', program])
        run([program])


    built_programs = {}  # one build per program for the whole run, as nothing it is made from changes during it

    def objects_program(self, name="objects_test"):
        if name in Generated.built_programs:
            return Generated.built_programs[name]
        import quickjs_build
        fixture = self.fixture()
        check_mods = check_mods_for_host(fixture)
        flags = [*CXX, '-O1', '-I', GEN / 'include', '-I', fixture / 'framework', '-I', GEN / 'framework',
                 '-I', BASE, '-I', CORE, '-I', GAME, '-I', ROOT / 'tests' / 'base', '-I', ROOT / 'runtime' / 'lua', '-I', LUA, '-I', SDK_CHECK,
                 '-I', ROOT / 'runtime' / 'lua' / 'mods', '-I', FRAMEWORK_TARGET,
                 '-I', ROOT / 'runtime/javascript', '-isystem', quickjs_build.QUICKJS]
        sources = [*sorted(CORE.glob('*.cpp')), *sorted(BASE.glob('*.cpp')), *sorted(GAME.glob('*.cpp')),
                   *sorted((GEN / 'framework').glob('*.cpp')), *sorted((GEN / 'lua').glob('*.cpp')),
                   *sorted((ROOT / 'runtime' / 'lua').glob('*.cpp')), *sorted((ROOT / 'runtime' / 'lua' / 'mods').glob('*.cpp')),
                   FRAMEWORK_TARGET / 'status_observer.cpp', ROOT / 'tests' / 'game' / 'objects_test.cpp', *check_mods,
                   *sorted((GEN / 'javascript').glob('*.cpp')), *sorted((ROOT / 'runtime/javascript').glob('*.cpp')),
                   FRAMEWORK_TARGET / 'uint128.cpp']
        program = build_program(BUILD / (name + EXE), flags, sources, [*quickjs_build.objects(), *lua_objects()])
        Generated.built_programs[name] = program
        return program

    def test_actors_are_listed_described_and_moved(self):
        """The objects service from compiled code and from a script, against a made-up game.

        The same program runs the check mods among the fixtures against the made-up game, each as the game would
        start it, and prints the log of each of three runs to be read as a log from the game is."""
        program = self.objects_program()
        from concurrent.futures import ThreadPoolExecutor  # the three runs share nothing, so they run at once
        with ThreadPoolExecutor(3) as pool:
            runs = list(pool.map(run, [[program, ROOT / 'tests' / 'game' / 'objects.lua'],
                                       [program, ROOT / 'tests' / 'game' / 'objects.lua', '--ai-order'],
                                       [program, ROOT / 'tests/javascript/objects.js']]))
        told = runs[0].stdout + runs[1].stdout
        logs = [part.split('==== its end\n')[0] for part in told.split('==== the log of a run of the check mods\n')[1:]]
        all_checks = {mod: group for mod, group in stage_test_mods.check_mods().items()
                      if mod not in stage_test_mods.native_hook_fixture.MODS}
        staged = {mod: group for mod, group in all_checks.items() if not stage_test_mods.explicit_check(MODS / mod)}
        self.assertGreaterEqual(len(staged), 1)
        self.assertEqual(len(logs), 3)
        for number, log in enumerate(logs, 1):
            (BUILD / f'check-mods-{number}.log').write_text(log, encoding='utf-8', newline='\n')  # to look at, and to try the reader on
            result = read_checks.read(log, staged, stage_test_mods.expected_checks(staged))
            self.assertEqual(result['exit'], 0, read_checks.table(result))
            self.assertEqual({check['mod'] for check in result['checks']}, set(staged))
            self.assertEqual({(check['mod'], check['name']) for check in result['checks']},
                             {(mod, name) for mod, names in stage_test_mods.expected_checks(staged).items() for name in names})
            self.assertEqual({mod: said for mod, said in result['mods'].items() if mod in staged}, {mod: 'running' for mod in staged})

        # Explicit groups run alone with independent receipts. The AI observer proves
        # first-install ordering once per process. Other groups cover three save starts.
        # Engine actor donor qualification is game-only. Its backend has a focused
        # host test; checking_fixtures deliberately does not model those donors.
        # Gimmick and battle proof also needs a real scene. Its focused test runs the real backends.
        # The ground observer has a dedicated host fixture; flight and landing need the game.
        explicit = {mod: group for mod, group in all_checks.items()
                    if stage_test_mods.explicit_check(MODS / mod) and mod not in ('check.engine-actors', 'check.presenter', 'check.ui', 'check.restart', 'check.ordinary-lifetime',
                                    'check.ordinary-lifetime-startup', 'check.ordinary-lifetime-startup-producer', 'check.gimmicks-battle', 'check.ground')}
        separate = [part.split('==== its end\n')[0] for part in told.split('==== the log of an explicit SDK run ')[1:]]
        runs_for = {mod: 1 if mod == stage_test_mods.ai_order_fixture.OBSERVER else 3 for mod in explicit}
        self.assertEqual(len(separate), sum(runs_for.values()))
        for mod, group in explicit.items():
            receipt = {'check_mods': {mod: group}, 'checks': stage_test_mods.expected_checks([mod])}
            (BUILD / f'check-explicit-{group}.json').write_text(json.dumps(receipt, indent=1) + '\n', encoding='utf-8', newline='\n')
            own = [part.split('\n', 1)[1] for part in separate if part.split('\n', 1)[0] == mod]
            self.assertEqual(len(own), runs_for[mod])
            for number, log in enumerate(own, 1):
                (BUILD / f'check-explicit-{group}-{number}.log').write_text(log, encoding='utf-8', newline='\n')
                result = read_checks.read(log, receipt['check_mods'], receipt['checks'])
                self.assertEqual(result['exit'], 0, read_checks.table(result))
                self.assertEqual({check['mod'] for check in result['checks']}, {mod})
                self.assertEqual(result['mods'].get(mod), 'running')

    def test_map_changes_precede_level_play(self):
        program = self.objects_program()
        completed = subprocess.run([str(program), str(ROOT / 'tests' / 'game' / 'objects.lua'), '--early-maps'],
                                   capture_output=True, text=True, timeout=60)
        self.assertEqual(completed.returncode, 0, completed.stdout + completed.stderr)
        # The late fallback, for a level already playing when its mod starts, is checked by the full run of
        # this program in test_actors_are_listed_described_and_moved.

    def test_map_content_joins_the_initial_streaming_flush(self):
        """Prepared map descriptors use the original native mode-2 flush before play."""
        program = self.objects_program()
        completed = subprocess.run([str(program), str(ROOT / 'tests' / 'game' / 'objects.lua'), '--initial-streaming'],
                                   capture_output=True, text=True, timeout=60)
        self.assertEqual(completed.returncode, 0, completed.stdout + completed.stderr)

    def test_the_real_table_has_what_the_base_uses(self):
        for source in (*sorted(BASE.glob('*.cpp')), *sorted(GAME.glob('*.cpp')), CORE / 'hooks.cpp', CORE / 'reflection.cpp'):
            run([shutil.which('clang++'), *CXX, '-fsyntax-only', '-I', GEN / 'include', '-I', GEN / 'framework',
                 '-I', BASE, '-I', CORE, '-I', GAME, source])

    @unittest.skipUnless(shutil.which('ld.lld'), 'needs ld.lld')
    def test_the_module_builds_for_the_game(self):
        report = build_module.build(BUILD / 'module')
        self.assertGreater(report['bytes'], 0x100)
        self.assertTrue((BUILD / 'module' / 'exefs' / 'subsdk9').read_bytes().startswith(b'NSO0'))
        # The build with the framework's own checks is another file, and far the larger: the checks are
        # in no part of the build people run.
        checking = build_module.build(BUILD / 'check-module', selfcheck=True)
        self.assertGreater(checking['bytes'], report['bytes'] + 0x8000)
        self.assertNotIn(b'check.library', (BUILD / 'module' / 'exefs' / 'subsdk9').read_bytes())

        # The patch: one changed instruction per code site, each a jump to that site's entry in
        # the block, plus the block itself. No blank line inside it; some loaders stop at one.
        table = game_table.load(ROOT / 'game' / 'table.yaml')
        lines = (BUILD / 'module' / 'exefs' / f'{table["version"]}.pchtxt').read_text(encoding='ascii').splitlines()
        self.assertEqual(lines[:3], [f'@nsobid-{table["build_id"]:0<64}', '@flag offset_shift 0x100', '@enabled'])
        self.assertEqual(lines[-1], '@stop')
        self.assertNotIn('', lines)
        changed = {int(address, 16): bytes.fromhex(data) for address, data in (line.split() for line in lines[3:-1])}
        for site in table['code_sites'].values():
            if site.get('absent'):
                continue  # a site of another's place (same_as) changes nothing
            word = (0x94000000 if site['call'] else 0x14000000) | (((site['entry'] - site['address']) >> 2) & 0x03FFFFFF)
            self.assertEqual(changed[site['address']], word.to_bytes(4, 'little'))
        for site in table.get('conditional_sites', {}).values():
            word = 0x14000000 | (((site['entry'] - site['address']) >> 2) & 0x03FFFFFF)
            self.assertEqual(changed[site['address']], word.to_bytes(4, 'little'))
            self.assertNotIn(site['address'] + 4, changed)
        sites = {*[s['address'] for s in table['code_sites'].values()],
                 *[s['address'] for s in table.get('conditional_sites', {}).values()],
                 *[s['address'] for s in table.get('native_routes', {}).values()]}
        block = [a for a in changed if a not in sites]
        base = table['constants']['stub_base']
        self.assertEqual(min(block), base)
        self.assertLessEqual(max(block) + 4, table['constants']['stub_end'])
        self.assertEqual(report['patch']['sites'], len(table['code_sites']))
        self.assertEqual(report['patch']['conditional_sites'], len(table.get('conditional_sites', {})))
        # The block begins with the value that marks it and the hash of what it was made from, and the
        # framework is built with the same hash: that is how it knows the patch beside it for its own.
        said = stubs.patch_hash(table)
        self.assertEqual(b''.join(changed[base + at] for at in range(0, stubs.MARK, 4)), struct.pack('<QQ', stubs.PATCH_MAGIC, said))
        header = (GEN / 'framework' / 'game_table.h').read_text(encoding='utf-8')
        rows = re.search(r'cells\[cell_count\] = \{\n(.*?)\n\};', header, re.DOTALL)[1].split('\n')
        stub_section = header[header.index('namespace stub {'):]
        def stub_cell(name):
            return int(rows[int(re.search(rf'Cell<uint64_t> {name}\{{(\d+)\}};', stub_section)[1])].split(',')[0], 16)
        self.assertEqual(stub_cell('block'), base)
        self.assertEqual(stub_cell('patch_hash'), said)
        self.assertEqual(report['patch']['hash'], f'{said:016x}')
        self.assertEqual(checking['patch']['hash'], report['patch']['hash'])
        self.assertEqual((BUILD / 'check-module' / 'exefs' / f'{table["version"]}.pchtxt').read_bytes(),
                         (BUILD / 'module' / 'exefs' / f'{table["version"]}.pchtxt').read_bytes())

    def test_the_patch_says_what_it_was_made_from(self):
        """The hash a patch carries and the framework is built with follows everything the patch is made from."""
        for path in (ROOT / 'game' / 'table.yaml', ROOT / 'tests' / 'fixtures' / 'game_table.yaml'):
            table = game_table.load(path)
            said = stubs.patch_hash(table)
            self.assertEqual(said, stubs.patch_hash(game_table.load(path)))
            self.assertTrue(0 < said < 1 << 64)
            first, second = list(table['code_sites'])[:2]

            def other(change):
                changed = copy.deepcopy(table)
                change(changed)
                return stubs.patch_hash(changed)

            def swap(changed):
                sites = list(changed['code_sites'].items())
                changed['code_sites'] = dict([sites[1], sites[0]] + sites[2:])

            def another_place(changed):
                changed['code_sites'][first]['address'] += 4

            def another_instruction(changed):
                site = next(s for s in changed['code_sites'].values() if not s['call'])
                site['instruction'] ^= 0x400

            def another_callee(changed):
                next(s for s in changed['code_sites'].values() if s['call'])['target'] += 4

            def one_less(changed):
                del changed['code_sites'][second]

            def block_elsewhere(changed):
                changed['constants']['stub_base'] -= 0x10

            def status_elsewhere(changed):
                changed['data']['status_words'] = changed['data'].get('status_words', 0) + 0x10

            changes = (swap, another_place, another_instruction, another_callee, one_less, block_elsewhere, status_elsewhere)
            hashes = {change.__name__: other(change) for change in changes}
            self.assertNotIn(said, hashes.values())
            self.assertEqual(len(set(hashes.values())), len(changes))
            # A site's name is the framework's own business: the patch knows sites by their order.
            renamed = copy.deepcopy(table)
            renamed['code_sites'] = {name + '_renamed': site for name, site in renamed['code_sites'].items()}
            self.assertEqual(stubs.patch_hash(renamed), said)
        # A table with no code sites has no patch, and the framework built from it looks for none.
        bare = copy.deepcopy(table)
        bare['code_sites'] = {}
        bare['conditional_sites'] = {}
        bare['native_routes'] = {}
        self.assertEqual(stubs.patch_hash(bare), 0)

    def test_a_script_runs_as_a_mod(self):
        """A Lua script through the generated bindings, the generated service code and the core."""
        sources = [CORE / 'registry.cpp', CORE / 'ids.cpp', CORE / 'frame.cpp', ROOT / 'tests/host/no_dynamic_hooks.cpp', *sorted((ROOT / 'runtime' / 'lua').glob('*.cpp')),
                   ROOT / 'tests' / 'host' / 'fake_host.cpp']
        sources += sorted((GEN / 'framework').glob('*.cpp')) + sorted((GEN / 'lua').glob('*.cpp'))
        program = BUILD / f'combat_host{EXE}'
        run([shutil.which('clang++'), *CXX, '-I', GEN / 'include', '-I', GEN / 'framework', '-I', CORE,
             '-I', ROOT / 'runtime' / 'lua', '-I', LUA, *sources, *lua_objects(), '-o', program])
        run([program, ROOT / 'tests' / 'host' / 'combat.lua'])

    def test_the_reference_page_follows_the_layout(self):
        page = self.files['docs/reference/combat.mdx']
        order = ['## Description', '## Tutorials', '## Functions', '## Events', '## Types', '## Function descriptions',
                 '## Event descriptions', '## Type descriptions']
        positions = [page.index(heading + '\n') for heading in order]
        self.assertEqual(positions, sorted(positions))
        for name in ('life', 'set_life', 'damage', 'knockback', 'damaged', 'defeated', 'Life', 'AttackKind'):
            self.assertIn(f'### {name}\n', page)
        self.assertIn('Fails with `not_found`', page)
        # A table in a description keeps one row per line.
        self.assertIn('\n| Kind | Value | Options in the manifest |\n| --- | --- | --- |\n| `toggle` |',
                      self.files['docs/reference/settings.mdx'])


if __name__ == '__main__':
    unittest.main(verbosity=2)
