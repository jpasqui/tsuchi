"""One framework on several builds of the game, against a made-up second build. Nothing here runs in the game."""
import copy
import re
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest
from concurrent.futures import ThreadPoolExecutor

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
import build_patch
from gen import SchemaError, game_table, generate_all, stubs
import yaml

FIXTURE = ROOT / 'tests' / 'fixtures' / 'game_table.yaml'
TABLE = ROOT / 'game' / 'table.yaml'
EXE = '.exe' if sys.platform == 'win32' else ''


def span_hash(byte):
    value = 0xCBF29CE484222325
    for _ in range(16):
        value = ((value ^ byte) * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    return value


def complete_build(raw):
    """A build file that states every entry the base table has, with the base table's values."""
    fixed = raw.get('fixed') or {}
    def plain(value):
        return value['at'] if isinstance(value, dict) else value
    build = {'version': '9.9', 'build_id': 'AB' * 16, 'identity': [{'offset': 0x200, 'hash': span_hash(0x22)}, {'offset': 0x210, 'hash': span_hash(0x22)}]}
    build['data'] = {name: plain(value) for name, value in raw['data'].items()}
    build['constants'] = {name: value for name, value in raw['constants'].items() if name not in fixed.get('constants', [])}
    build['structs'] = {}
    for name, body in raw['structs'].items():
        if name in fixed.get('structs', []) or (not body.get('fields') and 'size' not in body):
            continue
        said = {'fields': {field: plain(value) for field, value in (body.get('fields') or {}).items()}}
        if 'size' in body:
            said['size'] = body['size']
        build['structs'][name] = said
    build['functions'] = {name: body['address'] for name, body in raw['functions'].items()}
    build['slots'] = {name: {key: body[key] for key in ('table', 'offset', 'stock')} for name, body in raw['slots'].items()}
    build['tables'] = copy.deepcopy(raw['tables'])
    build['slot_groups'] = {name: {'offset': body['offset'], 'stock': list(body['stock'])} for name, body in raw['slot_groups'].items()}
    build['code_sites'] = {name: {key: body[key] for key in ('address', 'instruction', 'same_as') if key in body} for name, body in raw['code_sites'].items()}
    build['conditional_sites'] = {name: {key: body[key] for key in ('address', 'instruction', 'next_instruction', 'register', 'threshold')}
                                  for name, body in (raw.get('conditional_sites') or {}).items()}
    build['native_routes'] = {name: {key: body[key] for key in ('address', 'instruction', 'exits', 'same_as') if key in body}
                              for name, body in raw['native_routes'].items()}
    build['route_frames'] = copy.deepcopy(raw['route_frames'])
    build['names'] = {name: raw['names'][name] + 'Two' for name in raw['build_names']}
    return build


FAMILIES = {'core': {'files': ['base/core_user.cpp'], 'entries': ['data.status_words']},
            'families': {'world': {'services': ['world'], 'files': ['game/world_user.cpp']},
                         'other': {'files': ['game/other_user.cpp']}}}


class Layout:
    """A table, one made-up build and a small framework for the generator to read, in a temporary folder."""
    def __init__(self, folder, change=None):
        self.root = Path(folder)
        self.raw = yaml.safe_load(FIXTURE.read_text(encoding='utf-8'))
        self.raw['identity'] = [{'offset': 0x100, 'hash': span_hash(0x11)}]
        self.raw['build_id'] = 'CD' * 16
        self.build = complete_build(self.raw)
        self.build['data']['world'] = 'absent'
        self.build['functions']['spawn_now'] = 'absent'
        self.build['code_sites']['draw_call'] = 'absent'
        self.build['structs']['game_actor']['fields']['color'] += 4
        self.build['constants']['code_size'] = 0x10000000
        self.build['constants']['program_id'] = 0x0423456789ABCDEF
        if change:
            change(self.raw, self.build)
        self.write()

    def write(self):
        (self.root / 'game' / 'builds').mkdir(parents=True, exist_ok=True)
        (self.root / 'game' / 'table.yaml').write_text(yaml.safe_dump(self.raw), encoding='utf-8')
        (self.root / 'game' / 'builds' / 'made-up.yaml').write_text(yaml.safe_dump(self.build), encoding='utf-8')
        (self.root / 'game' / 'families.yaml').write_text(yaml.safe_dump(FAMILIES, sort_keys=False), encoding='utf-8')
        for name, text in (('base/core_user.cpp', 'namespace t = game::table; int a = t::data::status_words;\n'),
                           ('game/world_user.cpp', 'namespace t = game::table; void f() { t::fn::spawn_now(); t::code_site::draw_call; t::data::world; uint8_t b[t::squad_node::most]; }\n'),
                           ('game/other_user.cpp', 'namespace t = game::table; int b = t::game_actor::color;\n')):
            path = self.root / 'framework' / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(text, encoding='utf-8')

    @property
    def table(self):
        return self.root / 'game' / 'table.yaml'


class MultiBuild(unittest.TestCase):
    def layout(self, change=None):
        folder = tempfile.TemporaryDirectory()
        self.addCleanup(folder.cleanup)
        return Layout(folder.name, change)

    def refused(self, change, text):
        with self.assertRaisesRegex(SchemaError, text):
            game_table.generate(self.layout(change).table)

    def test_a_build_keeps_the_tables_shape(self):
        def add(raw, build):
            build['functions']['brand_new'] = 0x10
        def leave_out(raw, build):
            del build['functions']['spawn_now']
        def leave_out_field(raw, build):
            del build['structs']['game_actor']['fields']['color']
        def shorter(raw, build):
            name = next(iter(build['tables']))
            build['tables'][name] = build['tables'][name][:-1]
        def shorter_stock(raw, build):
            name = next(iter(build['slot_groups']))
            build['slot_groups'][name]['stock'] = []
        def signature(raw, build):
            build['functions']['spawn_now'] = {'address': 0x10, 'returns': 'u64'}
        def other_group(raw, build):
            build['assets'] = {}
        def other_name(raw, build):
            build['names']['project'] = 'Other'
        def add_exit(raw, build):
            name = next(iter(build['native_routes']))
            build['native_routes'][name]['exits'] = {**build['native_routes'][name].get('exits', {}), 'new_exit': 8}
        self.refused(add, 'never adds a name')
        self.refused(leave_out, 'does not say')
        self.refused(leave_out_field, 'does not say')
        self.refused(shorter, 'same length')
        self.refused(shorter_stock, 'same length')
        self.refused(signature, 'whole number or "absent"')
        self.refused(other_group, 'has no "assets"')
        self.refused(other_name, 'names gives exactly')
        self.refused(add_exit, 'never adds a name')

    def test_a_build_cannot_share_or_leave_its_bounds(self):
        def outside(raw, build):
            build['identity'][0]['offset'] = 0x10000000
        def same_identity(raw, build):
            raw['identity'] = build['identity']
        def same_id(raw, build):
            raw['build_id'] = build['build_id']
        def past_code(raw, build):
            build['functions']['actor_request_tick'] = 0x10000000
        def fixed_differs(raw, build):
            build['structs']['array'] = {'size': 24, 'fields': raw['structs']['array']['fields']}
        self.refused(outside, 'lies outside the code')
        self.refused(same_identity, 'cannot be told from')
        self.refused(same_id, 'its build is the one')
        self.refused(past_code, 'past the end of the code')
        self.refused(fixed_differs, 'is fixed')

    def test_absent_is_for_a_build_and_for_what_can_go(self):
        def in_base(raw, build):
            raw['functions']['spawn_now']['address'] = 'absent'
        def by_value(raw, build):
            build['functions']['text_from_string'] = 'absent'
        def core_file(raw, build):
            build['data']['status_words'] = 'absent'
        def buffer(raw, build):
            build['structs']['squad_node']['fields']['kind'] = 'absent'
        def where_code_is(raw, build):
            build['constants']['stub_base'] = 'absent'
        self.refused(in_base, 'only a build file')
        self.refused(by_value, 'by value cannot be absent')
        self.refused(core_file, 'the framework does not run without it')
        self.refused(buffer, 'a buffer of its own')
        self.refused(where_code_is, '.')

    def test_every_file_that_reads_the_table_has_a_family(self):
        def nothing(raw, build):
            pass
        layout = self.layout(nothing)
        (layout.root / 'framework' / 'game' / 'stray.cpp').write_text('int a = game::table::data::world;\n', encoding='utf-8')
        with self.assertRaisesRegex(SchemaError, 'stray.cpp reads the game table'):
            game_table.generate(layout.table)

    def test_route_frames_are_checked(self):
        def register(raw, build):
            build['route_frames']['dialog_setup_call']['request'] = 99
        def stack(raw, build):
            build['route_frames']['cursor_written']['saved_widget'] = 0x61
        def frame_without_route(raw, build):
            build['route_frames']['scene_publish'] = 'absent'
        self.refused(register, 'a register is a number')
        self.refused(stack, 'an offset from the stack')
        self.refused(frame_without_route, 'a route that stays needs its frame')
        def both_gone(raw, build):
            build['route_frames']['scene_publish'] = 'absent'
            build['native_routes']['scene_publish'] = 'absent'
        self.assertIn('native_routes.scene_publish', game_table.load_builds(self.layout(both_gone).table)[1]['absent'])

    def test_a_route_can_name_the_same_place(self):
        """A table whose code has a place once names it with same_as: that route takes no place and is not missing, and
        another build gives it a place of its own."""
        def once(raw, build):
            pass
        base, made_up = game_table.load_builds(self.layout(once).table)
        for table in (base, made_up):
            self.assertTrue(table['native_routes']['particle_lone_place']['absent'])
            self.assertNotIn('native_routes.particle_lone_place', table['absent'])
        def own_place(raw, build):
            build['native_routes']['particle_lone_place'] = {'address': 0x2944, 'instruction': 0xB9409029}
        made_up = game_table.load_builds(self.layout(own_place).table)[1]
        self.assertEqual(made_up['native_routes']['particle_lone_place']['address'], 0x2944)
        self.assertFalse(made_up['native_routes']['particle_lone_place'].get('absent'))
        def nowhere(raw, build):
            raw['native_routes']['particle_lone_place'] = {'same_as': 'no_such_route', 'resume': False}
        def chained(raw, build):
            raw['native_routes']['particle_lone_xy'] = {'same_as': 'particle_lone_place', 'resume': False}
        self.refused(nowhere, 'same_as names another route')
        self.refused(chained, 'same_as names another route')

    def test_builds_stay_apart(self):
        def one_span(raw, build):
            build['identity'] = build['identity'][:1]
        def same_version(raw, build):
            build['version'] = raw['version']
        def among_the_others(raw, build):
            raw['identity'] = build['identity'] + [{'offset': 0x220, 'hash': span_hash(0x22)}]
        self.refused(one_span, 'at least two spans')
        self.refused(same_version, 'its version is the one')
        self.refused(among_the_others, 'cannot be told from')

    def test_a_flag_cannot_be_absent(self):
        def flag(raw, build):
            name = next(name for name in build['constants'] if name not in ('stub_base', 'stub_end', 'code_size', 'program_id'))
            build['constants'][name] = 'absent'
        self.refused(flag, 'only a constant under address_constants')

    def test_each_use_belongs_to_the_alias_before_it(self):
        with tempfile.TemporaryDirectory() as folder:
            (Path(folder) / 'one.cpp').write_text(
                'void a() { namespace m = t::first; m::kept; }\nvoid b() { namespace m = t::second; m::other; }\n', encoding='utf-8')
            uses = game_table.scan_uses(folder, {'structs': {'first': 1, 'second': 1}})
        self.assertIn(('first', 'kept'), uses)
        self.assertIn(('second', 'other'), uses)
        self.assertNotIn(('second', 'kept'), uses)
        self.assertNotIn(('first', 'other'), uses)

    def test_a_family_gone_does_not_start(self):
        """A family with no calls of its own has nothing else to stop it."""
        families = game_table.load_families(TABLE, game_table.load(TABLE))
        text = ''.join(path.read_text(encoding='utf-8', errors='replace') for path in (ROOT / 'framework').rglob('*.cpp'))
        for name in families['names']:
            if not families['families'][name]['services']:
                self.assertIn(f'family::{name})', text, f'{name} has no calls, and nothing checks it')

    def test_how_much_cannot_be_absent(self):
        """The count DESIGN.md's limits speak of. A build missing one of these is refused until the code changes."""
        table = game_table.load(TABLE)
        families = game_table.load_families(TABLE, table)
        refused = kept = 0
        for path in game_table.flatten(table):
            if path.startswith('stub.') or not path.split('.')[0] in ('data', 'constants', 'structs', 'functions', 'slots', 'route_frames'):
                continue
            try:
                game_table.absent_families({**table, 'absent': [path]}, families, 'test')
                kept += 1
            except SchemaError:
                refused += 1
        self.assertGreater(kept, refused)
        self.assertGreater(refused, 0)

    def test_the_real_tables_families(self):
        table = game_table.load(TABLE)
        families = game_table.load_families(TABLE, table)
        def lost(*entries):
            return game_table.absent_families({**table, 'absent': list(entries)}, families, 'test')
        # Each of these is used under an alias a file binds more than once.
        self.assertIn('story', lost('structs.travel_request.captain'))
        self.assertIn('combat', lost('structs.damage_message.riding_applies'))
        self.assertEqual(set(lost('functions.kill_actor')), {'actor'})
        self.assertIn('presenter', lost('native_routes.particle_sprite'))
        self.assertIn('pikmin', lost('structs.pikmin_birth.onion'))
        for entry in ('data.status_words', 'constants.stub_base', 'data.object_chunks', 'structs.spawn_request.team'):
            with self.assertRaises(SchemaError):
                lost(entry)

    def test_what_the_mods_start_waits_on_cannot_be_absent(self):
        """With actor_node.size absent, game_framework() found nothing, world_begun() stayed false and no mod started, on a
        status block that said running. Seen on a real build of the game."""
        table = game_table.load(TABLE)
        families = game_table.load_families(TABLE, table)
        self.assertTrue(families['mod_start'])
        for entry in families['mod_start']:
            with self.assertRaisesRegex(SchemaError, 'no mod starts without it'):
                game_table.absent_families({**table, 'absent': [entry]}, families, 'test')

    def test_the_mods_start_list_covers_the_helper_it_goes_through(self):
        """What game_framework reads is on the list."""
        table = game_table.load(TABLE)
        families = game_table.load_families(TABLE, table)
        text = (ROOT / 'framework/game/actors.cpp').read_text(encoding='utf-8')
        body = re.search(r'uint64_t game_framework\(\) \{(.*?)\n\}', text, re.S).group(1)
        said = re.findall(r't::(\w+)::(\w+)', body)
        self.assertTrue(said)
        for first, second in said:
            at = f'data.{second}' if first == 'data' else f'structs.{first}.{second}'
            self.assertIn(at, families['mod_start'])

    def test_the_mods_start_reasons_are_told(self):
        text = (ROOT / 'framework/target/module.cpp').read_text(encoding='utf-8')
        self.assertIn('the mods have not started: ', text)
        self.assertIn('tell_mods_wait(why)', text)
        self.assertIn('game::world_not_begun()', text)

    def test_a_spawn_request_can_lack_only_what_is_guarded(self):
        table = game_table.load(TABLE)
        families = game_table.load_families(TABLE, table)
        self.assertEqual(game_table.absent_families({**table, 'absent': ['structs.spawn_request.decoration']}, families, 'test'), {})
        self.assertIn('t::present(r::decoration)', (ROOT / 'framework/game/actors.cpp').read_text(encoding='utf-8'))
        with self.assertRaisesRegex(SchemaError, 'a buffer of its own'):
            game_table.absent_families({**table, 'absent': ['structs.spawn_request.color']}, families, 'test')

    def test_a_manifest_key_of_a_family_gone_is_told_as_that(self):
        text = (ROOT / 'framework/target/module.cpp').read_text(encoding='utf-8')
        for key in ('maps', 'content', 'new_maps', 'shaders'):
            self.assertIn(f'add_manifest_key_off("{key}", t::families[t::family::{key}])', text)

    def test_a_guarded_entry_takes_no_family(self):
        table = game_table.load(TABLE)
        families = game_table.load_families(TABLE, table)
        self.assertEqual(game_table.absent_families({**table, 'absent': ['functions.pikmin_deco_stored', 'functions.pikmin_deco_field']}, families, 'test'), {})

    def test_a_gap_in_a_list_takes_no_family(self):
        raw = yaml.safe_load(TABLE.read_text(encoding='utf-8'))
        with tempfile.TemporaryDirectory() as folder:
            build = complete_build(raw)
            rows = list(build['tables']['actor_tables'])
            rows[3] = 'absent'
            build['tables']['actor_tables'] = rows
            table = game_table.load_raw(game_table.with_build(raw, build, 'gap.yaml'), TABLE, 'gap')
            self.assertEqual(table['tables']['actor_tables'][3], game_table.ABSENT_VALUE)
            self.assertNotIn('tables.actor_tables', table['absent'])
            build['tables']['actor_tables'] = ['absent'] * len(rows)
            table = game_table.load_raw(game_table.with_build(raw, build, 'gap.yaml'), TABLE, 'gap')
            self.assertIn('tables.actor_tables', table['absent'])

    def test_one_build_generates_what_it_always_did(self):
        """The first build's patch and hash do not depend on the rest of the table's format or on the other builds."""
        table = game_table.load(TABLE)
        self.assertEqual(table['key'], '1.1.1')
        builds = game_table.load_builds(TABLE)
        self.assertEqual(builds[0]['build_id'], table['build_id'])
        self.assertEqual(len(builds), 1 + len(list((TABLE.parent / 'builds').glob('*.yaml'))))
        header = game_table.generate(TABLE)['framework/game_table.h']
        self.assertIn(f'constexpr unsigned build_count = {len(builds)};', header)
        self.assertEqual(stubs.patch_hash(builds[0]), stubs.patch_hash(table))
        self.assertEqual(stubs.cells(builds[0]), stubs.cells(table))
        self.assertEqual(table['absent'], [])
        self.assertEqual(build_patch.patch_names(table), ('exefs/1.1.1.pchtxt', 'ips/060F827BD2B38703E9EA11E3210D89BC.ips'))

    def test_fixed_and_most(self):
        def bigger(raw, build):
            raw['structs']['spawn_request']['size'] = 0x90
            build['structs']['spawn_request']['size'] = 0xA0
        header = game_table.generate(self.layout(bigger).table)['framework/game_table.h']
        self.assertIn('namespace spawn_request {\nconstexpr Cell<uint64_t> size', header)
        self.assertIn('constexpr uint32_t most = 0xa0;', header)
        self.assertIn('namespace array {\nconstexpr uint32_t size = 0x10;', header)

    def test_each_build_has_its_patch(self):
        layout = self.layout()
        builds = game_table.load_builds(layout.table)
        self.assertEqual([build['key'] for build in builds], ['0.0', 'made-up'])
        self.assertNotEqual(stubs.patch_hash(builds[0]), stubs.patch_hash(builds[1]))
        self.assertEqual(builds[1]['code_sites']['draw_call']['entry'], builds[0]['code_sites']['draw_call']['entry'])
        source, script = stubs.assembly(builds[1], 0)
        site = list(builds[1]['code_sites']).index('draw_call')
        self.assertNotIn(f'.section .site{site},', source)
        self.assertIn(f'.site{site - 1} ' if site else '.stub', script)
        self.assertNotIn(f'site_{site} =', script)

    @unittest.skipUnless(shutil.which('clang') and shutil.which('ld.lld'), 'needs clang and ld.lld')
    def test_the_patch_of_a_build_leaves_an_absent_site_alone(self):
        layout = self.layout()
        first, second = game_table.load_builds(layout.table)
        with tempfile.TemporaryDirectory() as folder:
            before = dict(build_patch.records(first, Path(folder) / 'a'))
            after = dict(build_patch.records(second, Path(folder) / 'b'))
        address = first['code_sites']['draw_call']['address']
        self.assertIn(address, before)
        self.assertNotIn(address, after)
        self.assertEqual(sum(map(len, before.values())) - 4, sum(map(len, after.values())))

    @unittest.skipUnless(shutil.which('clang') and shutil.which('ld.lld'), 'needs clang and ld.lld')
    def test_patches_of_several_builds_share_a_folder(self):
        layout = self.layout()
        # What tools/patch_space.py records when a build's patch has been checked against its program.
        layout.build['patch_checked'] = stubs.checked_hash(game_table.load_builds(layout.table)[1])
        layout.write()
        with tempfile.TemporaryDirectory() as folder:
            report = build_patch.build(Path(folder), layout.table)
            self.assertEqual(report['files'], sorted(['exefs/0.0.pchtxt', 'exefs/made-up.pchtxt', 'ips/' + 'AB' * 16 + '.ips', 'ips/' + 'CD' * 16 + '.ips']))
            self.assertEqual(set(report['builds']), {'0.0', 'made-up'})
            text = (Path(folder) / 'exefs' / 'made-up.pchtxt').read_text(encoding='ascii')
            self.assertTrue(text.startswith('@nsobid-' + 'AB' * 16))

    def test_external_mods_keep_each_builds_registry_apart(self):
        import external_mods
        import external_patch_checks
        game = {'title': '0100000000000000', 'version': '1.0.0', 'build': 'CD' * 16, 'project': 'P'}
        with tempfile.TemporaryDirectory() as folder:
            first = external_mods.ExternalMods(folder, game=game)
            second = external_mods.ExternalMods(folder, game={**game, 'build': 'AB' * 16, 'key': 'made-up'})
            self.assertNotEqual(first.registry_path, second.registry_path)
            self.assertNotEqual(first.catalog_path, second.catalog_path)
            first.inspect(), second.inspect()
            with self.assertRaises(external_mods.Refused):
                external_mods.identity('no-such-build')
        self.assertEqual(external_mods.identity()['version'], game_table.load(TABLE)['version'])
        layout = self.layout()
        site = layout.build['code_sites']['update']['address']
        external_patch_checks.validate_fixed_ranges([(0x1000000, b'\0')], layout.table, 'made-up')
        with self.assertRaisesRegex(ValueError, 'patch overlaps'):
            external_patch_checks.validate_fixed_ranges([(site, b'\0')], layout.table, 'made-up')

    @unittest.skipUnless(shutil.which('clang'), 'needs clang')
    def test_the_framework_compiles_with_a_second_build_of_other_sizes(self):
        """The real framework against the real table and a made-up build. Every structure is bigger, so `most` is not `size`,
        and a function and a field are absent. What this finds is what the compiler can: it runs nothing."""
        import build_module
        raw = yaml.safe_load(TABLE.read_text(encoding='utf-8'))
        build = complete_build(raw)
        for body in build['structs'].values():
            if 'size' in body:
                body['size'] += 0x40
        build['functions']['kill_actor'] = 'absent'
        build['structs']['clock_table']['fields']['day'] = 'absent'
        with tempfile.TemporaryDirectory() as folder:
            work = Path(folder)
            (work / 'game' / 'builds').mkdir(parents=True)
            shutil.copy(TABLE, work / 'game' / 'table.yaml')
            shutil.copy(TABLE.parent / 'families.yaml', work / 'game' / 'families.yaml')
            shutil.copytree(ROOT / 'framework', work / 'framework')
            (work / 'game' / 'builds' / 'made-up.yaml').write_text(yaml.safe_dump(build, sort_keys=False), encoding='utf-8')
            gen = work / 'gen'
            for relative, text in generate_all(ROOT / 'schema', work / 'game' / 'table.yaml').items():
                path = gen / relative
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text(text, encoding='utf-8', newline='\n')
            (gen / 'https_roots.h').write_text('static const unsigned char tsuchi_https_roots[] = {0};\n', encoding='utf-8')
            framework = ROOT / 'framework'
            includes = [framework / 'target' / 'include', gen / 'include', gen / 'framework', framework / 'core', framework / 'base', framework / 'game',
                        framework / 'target', build_module.LUA_RUNTIME / 'mods', build_module.HTTPS / 'include', build_module.MBEDTLS / 'include', build_module.HTTPS, gen]
            sources = [path for part in ('core', 'base', 'game', 'target') for path in sorted((framework / part).glob('*.cpp'))]
            sources += sorted((framework / 'game/browser').glob('*.cpp'))
            build_module.embed_browser_layout(gen)
            def compiled(source):
                done = subprocess.run([shutil.which('clang'), *build_module.COMPILE, '-DTSUCHI_SELFCHECK=1', '-DMBEDTLS_CONFIG_FILE="https_tls_config.h"',
                                       *[f'-I{folder}' for folder in includes], '-fsyntax-only', str(source)], capture_output=True, text=True, errors='replace')
                return f'{source.name}:\n{done.stderr[:1500]}' if done.returncode else ''
            with ThreadPoolExecutor(3) as pool:
                problems = [text for text in pool.map(compiled, sources) if text]
            self.assertEqual(problems, [])

    @unittest.skipUnless(shutil.which('clang++'), 'needs clang++')
    def test_selecting_a_build_against_a_stand_in(self):
        """Selection, per-build values, absent entries and refused families. A stand-in result, not a game result."""
        def in_pieces(raw, build):
            # No spare space after the code, and two places for the mark that the fake game's memory holds.
            build['constants']['stub_end'] = build['constants']['stub_base']
            build['patch_space'] = [[0x3D20, 8], [0x3D30, 8], [0x10000, 0x2000]]
        layout = self.layout(in_pieces)
        with tempfile.TemporaryDirectory() as folder:
            work = Path(folder)
            for relative, text in generate_all(ROOT / 'schema', layout.table).items():
                path = work / relative
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text(text, encoding='utf-8', newline='\n')
            program = work / ('multi_build' + EXE)
            result = subprocess.run(
                [shutil.which('clang++'), '-std=c++17', '-Wall', '-Wextra', '-Werror', '-fno-exceptions', '-fno-rtti', '-DTSUCHI_DEPRECATED(text)=',
                 '-I', work / 'include', '-I', work / 'framework', '-I', ROOT / 'framework' / 'base', '-I', ROOT / 'framework' / 'core',
                 '-I', ROOT / 'tests' / 'base', ROOT / 'framework' / 'core' / 'registry.cpp', ROOT / 'framework' / 'core' / 'ids.cpp',
                 ROOT / 'framework' / 'core' / 'hooks.cpp', *sorted((work / 'framework').glob('*.cpp')),
                 ROOT / 'tests' / 'core' / 'multi_build_test.cpp', '-o', program],
                capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            result = subprocess.run([str(program)], capture_output=True, text=True, timeout=120)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == '__main__':
    unittest.main()
