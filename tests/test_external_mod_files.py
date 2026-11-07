"""External file ownership and recovery in disposable install trees, without a game."""
import copy
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
import external_mods as external

GAME = {'title': '0100B7C00933A000', 'version': '1.1.1',
        'build': '060F827BD2B38703E9EA11E3210D89BC', 'project': 'Carrot4'}


def assets(container_id=23):
    toc = bytearray(144)
    toc[:16] = b'-==--==--==--==-'
    toc[56:64] = container_id.to_bytes(8, 'little')
    return {'Glow_P.pak': b'empty pak sidecar', 'Glow_P.utoc': bytes(toc), 'Glow_P.ucas': b'cooked content'}


def pchtxt(offset, data=b'\x01\x02\x03\x04'):
    return ('@nsobid-' + GAME['build'].ljust(64, '0') + '\n@flag offset_shift 0x100\n@enabled\n' +
            f'{offset:08X} {data.hex()}\n@stop\n').encode()


class ExternalFiles(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='tsuchi-external-test-')
        self.addCleanup(self.temp.cleanup)
        self.home = Path(self.temp.name)
        self.install = self.home / 'install'
        self.install.mkdir()
        self.manager = external.ExternalMods(self.install, game=GAME)
        self.counter = 0

    def package(self, files=None):
        self.counter += 1
        source = self.home / ('package-' + str(self.counter))
        source.mkdir()
        for relative, value in (files or {'romfs/Carrot4/Content/config.bin': b'configuration'}).items():
            path = source / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(value)
        return source

    def imported(self, count=3, enabled=False):
        files = {'romfs/Carrot4/Content/file' + str(i) + '.bin': str(i).encode() for i in range(count)}
        source = self.package(files)
        result = self.manager.import_package(source, 'external.onion', 'Onion', enable=enabled)
        return source, files, result

    def snapshot(self):
        return {p.relative_to(self.install).as_posix(): p.read_bytes()
                for p in self.install.rglob('*') if p.is_file()}

    def test_inspect_without_install_state_is_read_only(self):
        self.assertEqual(self.manager.inspect()['mods'], [])
        self.assertEqual(list(self.install.iterdir()), [])

    def test_import_defaults_disabled_and_catalog_matches_runtime_shape(self):
        source, files, result = self.imported()
        self.assertRegex(result['revision'], r'^[0-9a-f]{64}$')
        self.assertEqual(set(result), {'format', 'revision', 'backend', 'mods'})
        self.assertEqual(result['mods'], [{'id': 'external.onion', 'name': 'Onion', 'version': '1.0.0',
                                          'kind': 'romfs', 'state': 'disabled', 'problem': '', 'manageable': True}])
        self.assertEqual(json.loads(self.manager.catalog_path.read_text(encoding='utf-8')), result)
        for logical, data in files.items():
            self.assertFalse((self.install / logical).exists())
            self.assertEqual((self.install / '.tsuchi-external/payload/external.onion' / logical).read_bytes(), data)
            self.assertEqual((source / logical).read_bytes(), data)
        snapshot = self.snapshot()
        self.assertEqual(self.manager.inspect(), result)
        self.assertEqual(self.snapshot(), snapshot)

    def test_enable_disable_moves_same_payload_and_changes_revision(self):
        _, files, first = self.imported()
        retained = self.install / '.tsuchi-external/payload/external.onion' / next(iter(files))
        inode = retained.stat().st_ino
        enabled = self.manager.set_enabled('external.onion', True)
        self.assertEqual(enabled['mods'][0]['state'], 'enabled')
        self.assertNotEqual(first['revision'], enabled['revision'])
        self.assertEqual((self.install / next(iter(files))).stat().st_ino, inode)
        for logical, data in files.items():
            self.assertEqual((self.install / logical).read_bytes(), data)
        disabled = self.manager.set_enabled('external.onion', False)
        self.assertEqual(disabled['mods'], first['mods'])
        self.assertNotEqual(disabled['revision'], first['revision'])
        self.assertEqual(retained.stat().st_ino, inode)
        self.assertFalse(self.manager.journal_path.exists())

    def test_enable_at_import_and_shared_disabled_destination_refuses(self):
        _, files, result = self.imported(enabled=True)
        self.assertEqual(result['mods'][0]['state'], 'enabled')
        self.manager.set_enabled('external.onion', False)
        before = self.snapshot()
        with self.assertRaisesRegex(external.Refused, 'share destination'):
            self.manager.import_package(self.package(files), 'external.glow', 'Glow')
        self.assertEqual(self.snapshot(), before)

    def test_case_folded_collision_in_inventory_and_existing_install(self):
        self.imported(count=1)
        with self.assertRaisesRegex(external.Refused, 'share destination'):
            self.manager.import_package(self.package({'romfs/Carrot4/Content/FILE0.bin': b'other'}), 'external.glow', 'Glow')
        foreign = self.install / 'romfs/Carrot4/Content/Foreign.bin'
        foreign.parent.mkdir(parents=True, exist_ok=True)
        foreign.write_bytes(b'foreign')
        with self.assertRaisesRegex(external.Refused, 'case-folded'):
            self.manager.import_package(self.package({'romfs/Carrot4/Content/foreign.bin': b'other'}), 'external.glow', 'Glow')
        self.assertEqual(foreign.read_bytes(), b'foreign')

    def test_existing_foreign_same_bytes_are_not_adopted_or_overwritten(self):
        source = self.package()
        foreign = self.install / 'romfs/Carrot4/Content/config.bin'
        foreign.parent.mkdir(parents=True)
        foreign.write_bytes(b'configuration')
        before = self.snapshot()
        with self.assertRaisesRegex(external.Refused, 'foreign file'):
            self.manager.import_package(source, 'external.onion', 'Onion')
        after = self.snapshot()
        after.pop('.tsuchi-external/manager.lock')
        self.assertEqual(after, before)

    def test_whole_plan_preflight_changes_no_files_on_late_conflict(self):
        _, files, _ = self.imported()
        last = self.install / sorted(files)[-1]
        last.parent.mkdir(parents=True, exist_ok=True)
        last.write_bytes(b'foreign')
        before = self.snapshot()
        with self.assertRaises(external.Refused):
            self.manager.set_enabled('external.onion', True)
        self.assertEqual(self.snapshot(), before)
        self.assertFalse(self.manager.journal_path.exists())
        self.assertEqual(self.manager.inspect()['mods'][0]['state'], 'conflict')

    def test_changed_owned_file_is_conflict_and_cannot_be_disabled(self):
        _, files, _ = self.imported(enabled=True)
        target = self.install / next(iter(files))
        target.write_bytes(b'changed by someone else')
        observed = self.manager.inspect()
        self.assertEqual(observed['mods'][0]['state'], 'conflict')
        self.assertFalse(observed['mods'][0]['manageable'])
        with self.assertRaisesRegex(external.Refused, 'missing or changed'):
            self.manager.set_enabled('external.onion', False)
        self.assertEqual(target.read_bytes(), b'changed by someone else')

    def test_saved_request_revision_unknown_ids_and_bool_types(self):
        _, _, first = self.imported()
        request = {'format': 1, 'revision': first['revision'], 'enabled': {'external.onion': True}}
        path = self.home / 'exported-request.json'
        path.write_bytes(external.canonical(request))
        self.manager.apply(path)
        with self.assertRaisesRegex(external.Refused, 'revision'):
            self.manager.apply(request)
        for changes in ({'external.unknown': False}, {'external.onion': 1}, {'external.onion': None}):
            with self.subTest(changes=changes):
                with self.assertRaises(external.Refused):
                    self.manager.apply({'format': 1, 'revision': self.manager.inspect()['revision'], 'enabled': changes})
        request = {'format': 1, 'revision': self.manager.inspect()['revision'], 'enabled': {}}
        before = self.manager.inspect()
        self.assertEqual(self.manager.apply(request), before)

    def test_every_enable_and_disable_move_can_recover_idempotently(self):
        for wanted in (True, False):
            for number in (1, 2, 3):
                with self.subTest(enabled=wanted, after_move=number):
                    target = self.home / ('install-' + str(wanted) + '-' + str(number))
                    target.mkdir()
                    manager = external.ExternalMods(target, game=GAME)
                    files = {'romfs/Carrot4/Content/' + str(i): str(i).encode() for i in range(3)}
                    manager.import_package(self.package(files), 'external.onion', 'Onion', enable=not wanted)
                    with self.assertRaises(InterruptedError):
                        manager.set_enabled('external.onion', wanted, fail_after_move=number)
                    self.assertTrue(manager.journal_path.exists())
                    self.assertEqual(manager.inspect()['mods'][0]['state'], 'recovery')
                    with self.assertRaisesRegex(external.Refused, 'recover'):
                        manager.set_enabled('external.onion', not wanted)
                    # New process/instance can finish exactly the previously approved direction.
                    manager = external.ExternalMods(target, game=GAME)
                    result = manager.recover()
                    self.assertEqual(result['mods'][0]['state'], 'enabled' if wanted else 'disabled')
                    self.assertEqual(manager.recover(), result)
                    self.assertFalse(manager.journal_path.exists())
                    for logical, data in files.items():
                        active = target / logical
                        retained = target / '.tsuchi-external/payload/external.onion' / logical
                        self.assertEqual((active if wanted else retained).read_bytes(), data)
                        self.assertFalse((retained if wanted else active).exists())

    def test_recovery_refuses_unknown_replacement_and_can_resume_when_preserved(self):
        _, files, _ = self.imported()
        with self.assertRaises(InterruptedError):
            self.manager.set_enabled('external.onion', True, fail_after_move=1)
        destination = self.install / sorted(files)[1]
        destination.write_bytes(b'foreign replacement')
        before = self.snapshot()
        with self.assertRaises(external.Refused):
            self.manager.recover()
        self.assertEqual(self.snapshot(), before)
        self.assertTrue(self.manager.journal_path.exists())
        destination.rename(self.home / 'preserved-foreign.bin')
        self.assertEqual(self.manager.recover()['mods'][0]['state'], 'enabled')

    def test_recovery_does_not_trust_forged_paths_inventory_or_moves(self):
        self.imported()
        with self.assertRaises(InterruptedError):
            self.manager.set_enabled('external.onion', True, fail_after_move=1)
        original = json.loads(self.manager.journal_path.read_text(encoding='utf-8'))
        for alteration in ('path', 'inventory', 'unknown'):
            forged = copy.deepcopy(original)
            if alteration == 'path':
                forged['moves'][0]['path'] = '../outside.bin'
            elif alteration == 'inventory':
                next(iter(forged['after']['mods'][0]['files'].values()))['sha256'] = '0' * 64
            else:
                forged['moves'].append({'id': 'external.other', 'path': 'romfs/unknown', 'enabled': True})
            self.manager.journal_path.write_bytes(external.canonical(forged))
            before = self.snapshot()
            with self.subTest(alteration=alteration), self.assertRaises(external.Refused):
                self.manager.recover()
            self.assertEqual(self.snapshot(), before)
        self.manager.journal_path.write_bytes(external.canonical(original))
        self.manager.recover()

    def test_registry_rejects_escape_protected_targets_and_case_collisions(self):
        self.imported(count=1)
        original = self.manager._registry()
        for logical in ('../outside.bin', 'romfs/../outside.bin', 'romfs/tsuchi/settings.json',
                        'exefs/main', 'exefs/main.npdm', 'exefs/subsdk9', 'exefs/1.1.1.pchtxt',
                        'romfs/Carrot4/file:stream', 'romfs/Carrot4/file.', 'romfs/Carrot4/CON'):
            forged = copy.deepcopy(original)
            forged['mods'][0]['files'] = {logical: next(iter(forged['mods'][0]['files'].values()))}
            self.manager.registry_path.write_bytes(external.canonical(forged))
            with self.subTest(path=logical), self.assertRaises(external.Refused):
                self.manager.inspect()
        self.manager.registry_path.write_bytes(external.canonical(original))

    def test_native_id_collision_at_import_and_after_install(self):
        folder = self.install / 'romfs/tsuchi/mods/external.onion'
        folder.mkdir(parents=True)
        with self.assertRaisesRegex(external.Refused, 'Tsuchi mod'):
            self.manager.import_package(self.package(), 'external.onion', 'Onion')
        folder.rmdir()
        self.imported()
        folder.mkdir()
        observed = self.manager.inspect()['mods'][0]
        self.assertEqual(observed['state'], 'conflict')
        self.assertFalse(observed['manageable'])
        with self.assertRaises(external.Refused):
            self.manager.set_enabled('external.onion', True)

    def test_text_limits_match_runtime_utf8_bytes_and_controls(self):
        for value in ('x' * 129, 'é' * 65, 'name\x7f', 'name\x85'):
            with self.subTest(name=repr(value)), self.assertRaises(external.Refused):
                self.manager.import_package(self.package(), 'external.onion', value)
        result = self.manager.import_package(self.package(), 'external.onion', 'é' * 64)
        self.assertEqual(len(result['mods'][0]['name'].encode('utf-8')), 128)

    def test_problem_display_is_bounded_valid_utf8(self):
        self.imported(count=1)
        with mock.patch.object(self.manager, '_problem', return_value='é' * 500 + '\x85\n'):
            value = self.manager.inspect()['mods'][0]['problem']
        self.assertLessEqual(len(value.encode('utf-8')), 512)
        self.assertNotIn('\x85', value)

    def test_cheat_import_is_refused_without_payload_or_checkbox(self):
        with self.assertRaisesRegex(external.Refused, 'cheat import requires exactly'):
            self.manager.import_package(self.package(), 'external.cheats', 'Cheats', kind='cheats')
        self.assertEqual(self.manager.inspect()['mods'], [])
        self.assertFalse((self.install / '.tsuchi-external/payload').exists())

    def test_exefs_build_header_and_protected_files(self):
        good = ('@nsobid-' + GAME['build'].ljust(64, '0') + '\n@enabled\n00101000 01020304\n@stop\n').encode()
        source = self.package({'exefs/Onion.pchtxt': good, 'exefs/subsdk2': b'NSO0module'})
        result = self.manager.import_package(source, 'external.onion', 'Onion', enable=True)
        self.assertEqual(result['mods'][0]['kind'], 'exefs')
        self.assertEqual((self.install / 'exefs/subsdk2').read_bytes(), b'NSO0module')
        for relative, data in (('exefs/Other.pchtxt', b'@nsobid-' + b'0' * 64),
                               ('exefs/subsdk1', b'not NSO'), ('exefs/main', b'NSO0'),
                               ('exefs/rtld', b'NSO0'), ('exefs/sdk', b'NSO0'),
                               ('exefs/subsdk9', b'NSO0'), ('exefs/main.npdm', b'metadata'),
                               ('romfs/tsuchi/mods/foreign/mod.json', b'{}')):
            with self.subTest(relative=relative), self.assertRaises(external.Refused):
                self.manager.import_package(self.package({relative: data}), 'external.other', 'Other')

    def test_console_layout_ips_and_romfs_lifecycle(self):
        manager = external.ExternalMods(self.install, 'console', GAME)
        name = GAME['build'] + '.ips'
        result = manager.import_package(self.package({'ips/' + name: b'IPS32EEOF', 'romfs/Carrot4/file': b'asset'}),
                                        'external.onion', 'Onion', enable=True)
        self.assertEqual(result['mods'][0]['kind'], 'mixed')
        ips = self.install / 'atmosphere/exefs_patches/external.onion' / name
        asset = self.install / ('atmosphere/contents/' + GAME['title'] + '/romfs/Carrot4/file')
        self.assertEqual(ips.read_bytes(), b'IPS32EEOF')
        self.assertEqual(asset.read_bytes(), b'asset')
        manager.set_enabled('external.onion', False)
        self.assertFalse(ips.exists())
        self.assertFalse(asset.exists())
        self.assertEqual(manager.catalog_path.relative_to(self.install).as_posix(),
                         'atmosphere/contents/' + GAME['title'] + '/romfs/tsuchi/external-mods.json')

    def test_layouts_refuse_wrong_patch_format_and_game_identity(self):
        with self.assertRaises(external.Refused):
            self.manager.import_package(self.package({'ips/' + GAME['build'] + '.ips': b'IPS32EEOF'}), 'external.ips', 'IPS')
        console = external.ExternalMods(self.install, 'console', GAME)
        with self.assertRaises(external.Refused):
            console.import_package(self.package({'exefs/Onion.pchtxt': b'@nsobid-' + GAME['build'].encode()}),
                                   'external.patch', 'Patch')
        self.imported()
        changed_game = dict(GAME, build='A' * 32)
        with self.assertRaisesRegex(external.Refused, 'identity'):
            external.ExternalMods(self.install, game=changed_game).inspect()

    def test_source_inside_install_and_unknown_files_are_refused(self):
        source = self.install / 'incoming'
        source.mkdir()
        with self.assertRaisesRegex(external.Refused, 'separate trees'):
            self.manager.import_package(source, 'external.onion', 'Onion')
        with self.assertRaisesRegex(external.Refused, 'package paths must begin'):
            self.manager.import_package(self.package({'README.txt': b'text'}), 'external.onion', 'Onion')

    def test_static_symlink_and_reparse_components_are_refused(self):
        source = self.package()
        link = source / 'romfs/link'
        try:
            link.symlink_to(self.home, target_is_directory=True)
        except OSError:
            # Windows can disable unprivileged symlink creation; the native reparse bit
            # check remains exercised without granting a privilege or touching an SD.
            original = external._no_link
            fake = mock.Mock(st_mode=0, st_file_attributes=0x400)
            with mock.patch.object(Path, 'lstat', return_value=fake), self.assertRaisesRegex(external.Refused, 'reparse'):
                original(link)
        else:
            with self.assertRaisesRegex(external.Refused, 'symlink'):
                self.manager.import_package(source, 'external.onion', 'Onion')

    def test_manager_lock_excludes_second_process_and_releases_after_crash(self):
        code = ('import sys;sys.path.insert(0,sys.argv[1]);import external_mods as e;'
                'm=e.ExternalMods(sys.argv[2],game=' + repr(GAME) + ');'
                '\ntry:\n with m._lock(): print("acquired")\nexcept e.Refused: print("busy")')
        with self.manager._lock():
            result = subprocess.run([sys.executable, '-c', code, str(ROOT / 'tools'), str(self.install)],
                                    capture_output=True, text=True, check=True)
            self.assertEqual(result.stdout.strip(), 'busy')
        result = subprocess.run([sys.executable, '-c', code, str(ROOT / 'tools'), str(self.install)],
                                capture_output=True, text=True, check=True)
        self.assertEqual(result.stdout.strip(), 'acquired')

    def test_stale_pending_json_is_preserved_and_does_not_block_recovery(self):
        self.imported()
        with self.assertRaises(InterruptedError):
            self.manager.set_enabled('external.onion', True, fail_after_move=1)
        pending = self.manager.registry_path.with_name('registry.json.abandoned.pending')
        pending.write_bytes(b'interrupted metadata write')
        self.assertEqual(self.manager.recover()['mods'][0]['state'], 'enabled')
        self.assertEqual(pending.read_bytes(), b'interrupted metadata write')

    def test_recheck_preserves_source_replaced_after_preflight(self):
        _, files, _ = self.imported()
        original = self.manager._preflight
        calls = 0
        target = self.install / '.tsuchi-external/payload/external.onion' / sorted(files)[0]
        def changed(journal, recovery=False):
            nonlocal calls
            result = original(journal, recovery)
            calls += 1
            if calls == 2:
                target.write_bytes(b'replaced after preflight')
            return result
        with mock.patch.object(self.manager, '_preflight', side_effect=changed):
            with self.assertRaisesRegex(external.Refused, 'after preflight'):
                self.manager.set_enabled('external.onion', True)
        self.assertEqual(target.read_bytes(), b'replaced after preflight')
        self.assertFalse((self.install / sorted(files)[0]).exists())
        self.assertTrue(self.manager.journal_path.exists())

    def test_assets_wrapper_copies_unchanged_triple_for_native_activation(self):
        files = assets()
        result = self.manager.import_package(self.package(files), 'external.glow', 'Glow', kind='assets')
        folder = self.install / 'romfs/tsuchi/mods/external.glow'
        self.assertEqual(result['activation'], 'tsuchi')
        manifest = json.loads((folder / 'mod.json').read_text(encoding='utf-8'))
        self.assertEqual({key: manifest[key] for key in ('id', 'name', 'version')},
                         {'id': 'external.glow', 'name': 'Glow', 'version': '1.0.0'})
        self.assertNotIn('code', manifest)
        for filename, data in files.items():
            self.assertEqual((folder / filename).read_bytes(), data)
        self.assertEqual(self.manager.inspect()['mods'], [])

    def test_assets_wrapper_rejects_global_copy_incomplete_triple_and_bad_version(self):
        files = assets()
        global_file = self.install / 'romfs/Carrot4/Content/Paks/Renamed_P.utoc'
        global_file.parent.mkdir(parents=True)
        global_file.write_bytes(files['Glow_P.utoc'])
        with self.assertRaisesRegex(external.Refused, 'global auto-mounted'):
            self.manager.import_package(self.package(files), 'external.glow', 'Glow', kind='assets')
        global_file.unlink()
        for invalid, version in (({'Glow_P.pak': b'pak'}, '1.0.0'), (files, 'beta'),
                                 (dict(files, readme=b'extra'), '1.0.0'), (files, '01.0.0'),
                                 (files, '1000000000.0.0')):
            with self.subTest(invalid=invalid, version=version), self.assertRaises(external.Refused):
                self.manager.import_package(self.package(invalid), 'external.glow', 'Glow', version, kind='assets')
        self.assertFalse((self.install / 'romfs/tsuchi/mods/external.glow').exists())

    def test_prefix_collisions_across_disabled_packages_refuse_before_copy(self):
        for first, second in (('romfs/Carrot4/a', 'romfs/Carrot4/a/b'),
                              ('romfs/Carrot4/A/b', 'romfs/Carrot4/a')):
            with self.subTest(first=first, second=second):
                target = self.home / ('prefix-' + str(self.counter))
                target.mkdir()
                manager = external.ExternalMods(target, game=GAME)
                manager.import_package(self.package({first: b'first'}), 'external.first', 'First')
                before = {p.relative_to(target).as_posix(): p.read_bytes() for p in target.rglob('*') if p.is_file()}
                with self.assertRaisesRegex(external.Refused, 'share destination'):
                    manager.import_package(self.package({second: b'second'}), 'external.second', 'Second')
                after = {p.relative_to(target).as_posix(): p.read_bytes() for p in target.rglob('*') if p.is_file()}
                self.assertEqual(after, before)

    def test_prefix_collision_cannot_hide_behind_lexically_intervening_package(self):
        self.manager.import_package(self.package({'romfs/Carrot4/foo': b'file'}),
                                    'external.first', 'First')
        self.manager.import_package(self.package({'romfs/Carrot4/foo-bar': b'other file'}),
                                    'external.second', 'Second')
        before = self.snapshot()
        with self.assertRaisesRegex(external.Refused, 'file/directory prefix'):
            self.manager.import_package(self.package({'romfs/Carrot4/foo/bar': b'descendant'}),
                                        'external.third', 'Third')
        self.assertEqual(self.snapshot(), before)
        self.assertFalse((self.install / '.tsuchi-external/payload/external.third').exists())
        self.assertFalse((self.install / 'romfs/Carrot4/foo').exists())
        self.assertFalse(self.manager.journal_path.exists())

        # A forged persisted inventory must fail before apply can build a move plan.
        forged = self.manager._registry()
        forged['mods'].append({'id': 'external.third', 'name': 'Third', 'version': '1.0.0',
                               'enabled': False, 'files': {'romfs/Carrot4/foo/bar':
                                   {'bytes': 10, 'sha256': hashlib.sha256(b'descendant').hexdigest()}}})
        self.manager.registry_path.write_bytes(external.canonical(forged))
        with self.assertRaisesRegex(external.Refused, 'file/directory prefix'):
            self.manager.apply({'format': 1, 'revision': '0' * 64, 'enabled': {'external.first': True}})
        self.assertFalse((self.install / 'romfs/Carrot4/foo').exists())
        self.assertFalse(self.manager.journal_path.exists())

    def test_catalog_write_failure_keeps_recoverable_journal_after_registry_commit(self):
        self.imported(count=1)
        original = external._write
        def fail_catalog(path, value):
            if path == self.manager.catalog_path:
                raise OSError('simulated catalog write failure')
            return original(path, value)
        with mock.patch.object(external, '_write', side_effect=fail_catalog):
            with self.assertRaisesRegex(OSError, 'catalog write'):
                self.manager.set_enabled('external.onion', True)
        self.assertTrue(self.manager.journal_path.exists())
        self.assertEqual(self.manager.inspect()['mods'][0]['state'], 'recovery')
        result = self.manager.recover()
        self.assertEqual(result['mods'][0]['state'], 'enabled')
        self.assertEqual(json.loads(self.manager.catalog_path.read_text(encoding='utf-8')), result)

    def test_import_catalog_write_failure_also_recovers(self):
        original = external._write
        def fail_catalog(path, value):
            if path == self.manager.catalog_path:
                raise OSError('simulated catalog write failure')
            return original(path, value)
        with mock.patch.object(external, '_write', side_effect=fail_catalog):
            with self.assertRaises(OSError):
                self.imported(count=1)
        self.assertTrue(self.manager.journal_path.exists())
        self.assertEqual(self.manager.recover()['mods'][0]['state'], 'disabled')

    def test_old_export_cannot_replay_after_state_returns_to_original(self):
        _, _, first = self.imported(count=1)
        old = {'format': 1, 'revision': first['revision'], 'enabled': {'external.onion': True}}
        self.manager.apply(old)
        self.manager.set_enabled('external.onion', False)
        with self.assertRaisesRegex(external.Refused, 'revision'):
            self.manager.apply(old)

    def test_distinct_global_container_can_share_empty_pak_sidecar(self):
        files = assets()
        paks = self.install / 'romfs/Carrot4/Content/Paks'
        paks.mkdir(parents=True)
        for name, data in assets(24).items():
            (paks / name.replace('Glow', 'Other')).write_bytes(data)
        self.manager.import_package(self.package(files), 'external.glow', 'Glow', kind='assets')

    def test_distinct_global_container_can_share_filename_with_wrapper(self):
        paks = self.install / 'romfs/Carrot4/Content/Paks'
        paks.mkdir(parents=True)
        for name, data in assets(24).items():
            (paks / name).write_bytes(data)
        self.manager.import_package(self.package(assets(23)), 'external.glow', 'Glow', kind='assets')

    def test_unsupported_host_move_refuses_before_journaling(self):
        self.imported(count=1)
        with mock.patch.object(external, '_can_move', return_value=False):
            with self.assertRaisesRegex(external.Refused, 'no-replace move'):
                self.manager.set_enabled('external.onion', True)
        self.assertFalse(self.manager.journal_path.exists())

    def test_static_patches_cannot_overlap_tsuchi_even_with_different_filename(self):
        import yaml
        table = yaml.safe_load((ROOT / 'game/table.yaml').read_text(encoding='utf-8'))
        addresses = [table['constants']['stub_base'], next(iter(table['code_sites'].values()))['address'],
                     next(iter(table['native_routes'].values()))['address'],
                     next(iter(table['conditional_sites'].values()))['address'] + 4]
        for address in addresses:
            with self.subTest(address=address), self.assertRaisesRegex(external.Refused, 'overlaps Tsuchi'):
                self.manager.import_package(self.package({'exefs/Onion.pchtxt': pchtxt(address)}),
                                            'external.onion', 'Onion')
        self.assertFalse((self.install / '.tsuchi-external/payload').exists())

    def test_static_patch_conflicts_check_all_disabled_packages(self):
        self.manager.import_package(self.package({'exefs/Onion.pchtxt': pchtxt(0x100000)}),
                                    'external.onion', 'Onion')
        with self.assertRaisesRegex(external.Refused, 'overlap'):
            self.manager.import_package(self.package({'exefs/Glow.pchtxt': pchtxt(0x100002, b'other')}),
                                        'external.glow', 'Glow')
        # Identical overlapping bytes are safe even when another patch repeats them.
        self.manager.import_package(self.package({'exefs/Glow.pchtxt': pchtxt(0x100002, b'\x03\x04')}),
                                    'external.glow', 'Glow')
        self.assertEqual(len(self.manager.inspect()['mods']), 2)

    def test_romfs_file_with_patch_extension_is_preserved_as_data(self):
        self.manager.import_package(self.package({'romfs/Carrot4/readme.pchtxt': b'not an executable patch'}),
                                    'external.onion', 'Onion', enable=True)
        self.assertEqual(self.manager.inspect()['mods'][0]['state'], 'enabled')

    def test_recoverable_metadata_limits_refuse_before_copy_or_moves(self):
        with mock.patch.object(external, 'MAX_REGISTRY_BYTES', 100):
            with self.assertRaisesRegex(external.Refused, 'metadata limit'):
                self.manager.import_package(self.package(), 'external.onion', 'Onion')
        self.assertFalse((self.install / '.tsuchi-external/payload').exists())
        with mock.patch.object(external, 'MAX_JOURNAL_BYTES', 100):
            with self.assertRaisesRegex(external.Refused, 'recovery limit'):
                self.manager.import_package(self.package(), 'external.onion', 'Onion')
        self.assertFalse((self.install / '.tsuchi-external/payload').exists())
        self.imported(count=1)
        before = self.snapshot()
        with mock.patch.object(external, 'MAX_JOURNAL_BYTES', 100):
            with self.assertRaisesRegex(external.Refused, 'recovery limit'):
                self.manager.set_enabled('external.onion', True)
        self.assertEqual(self.snapshot(), before)

    def test_registry_near_real_boundary_keeps_journal_readable(self):
        registry = self.manager._empty()
        # Validate the real cap with synthetic inventory, without creating payloads.
        files = {'romfs/Carrot4/' + str(i).zfill(6) + '-' + 'x' * 100: {'bytes': 0, 'sha256': '0' * 64}
                 for i in range(12000)}
        registry['mods'] = [{'id': 'external.onion', 'name': 'Onion', 'version': '1.0.0', 'enabled': False, 'files': files}]
        self.assertLess(len(external.canonical(registry)), external.MAX_REGISTRY_BYTES)
        self.manager._registry(registry)
        after = copy.deepcopy(registry)
        after['generation'] += 1
        after['mods'][0]['enabled'] = True
        moves = self.manager._operations(registry, after)
        journal = {'format': 1, 'before': registry, 'after': after, 'moves': moves}
        # Journal operation paths also need room; oversized plans are refused by
        # _preflight before inspecting or moving their synthetic file inventory.
        with mock.patch.object(external, 'MAX_JOURNAL_BYTES', len(external.canonical(journal)) - 1):
            with self.assertRaisesRegex(external.Refused, 'recovery limit'):
                self.manager._preflight(journal)

    def test_assets_staging_tamper_is_preserved_without_exposing_wrapper(self):
        original = self.manager._copy
        copied = False
        def inject(source, destination, expected):
            nonlocal copied
            original(source, destination, expected)
            if not copied:
                copied = True
                (destination.parent / 'mod.nro').write_bytes(b'unexpected executable')
        with mock.patch.object(self.manager, '_copy', side_effect=inject):
            with self.assertRaisesRegex(external.Refused, 'staging changed'):
                self.manager.import_package(self.package(assets()), 'external.glow', 'Glow', kind='assets')
        self.assertFalse((self.install / 'romfs/tsuchi/mods/external.glow').exists())
        self.assertEqual((self.install / '.tsuchi-external/asset-import/external.glow/mod.nro').read_bytes(),
                         b'unexpected executable')

    def test_cli_inspect_and_wrong_game_declaration_do_not_modify_install(self):
        base = [sys.executable, str(ROOT / 'tools/external_mods.py'), '--root', str(self.install)]
        result = subprocess.run(base + ['inspect'], capture_output=True, text=True, encoding='utf-8')
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(json.loads(result.stdout)['mods'], [])
        result = subprocess.run(base + ['import', str(self.package()), '--id', 'external.onion', '--name', 'Onion',
                                        '--game-version', '0.0.0'], capture_output=True, text=True, encoding='utf-8')
        self.assertEqual(result.returncode, 1)
        self.assertIn('does not match', result.stderr)
        self.assertEqual(list(self.install.iterdir()), [])

    def test_selected_tsuchi_slot_is_protected_and_slot_nine_can_be_foreign(self):
        manager = external.ExternalMods(self.install, game=GAME, tsuchi_slot=2)
        result = manager.import_package(self.package({'exefs/subsdk9': b'NSO0foreign module'}),
                                        'external.onion', 'Onion', enable=True)
        self.assertEqual(result['mods'][0]['state'], 'enabled')
        self.assertEqual((self.install / 'exefs/subsdk9').read_bytes(), b'NSO0foreign module')
        with self.assertRaisesRegex(external.Refused, 'protected ExeFS'):
            manager.import_package(self.package({'exefs/subsdk2': b'NSO0other module'}), 'external.glow', 'Glow')
        with self.assertRaisesRegex(external.Refused, 'Tsuchi slot differs'):
            external.ExternalMods(self.install, game=GAME).inspect()
        manager.set_enabled('external.onion', False)
        self.assertFalse((self.install / 'exefs/subsdk9').exists())

    def test_cli_malformed_cheat_refuses_without_traceback_or_payload(self):
        package = self.package({'cheats/' + GAME['build'][:16] + '.txt': b'[Broken]\nnot hexadecimal\n'})
        result = subprocess.run([sys.executable, str(ROOT / 'tools/external_mods.py'), '--root', str(self.install),
                                 'import', str(package), '--id', 'external.cheats', '--name', 'Cheats', '--kind', 'cheats'],
                                capture_output=True, text=True, encoding='utf-8')
        self.assertEqual(result.returncode, 1)
        self.assertIn('External mod operation refused:', result.stderr)
        self.assertNotIn('Traceback', result.stderr)
        self.assertFalse(self.manager._cheat_source().exists())
        self.assertFalse(self.manager.registry_path.exists())

    def test_asset_wrapper_cannot_take_existing_virtual_cheat_row_id(self):
        source = self.package({'cheats/' + GAME['build'][:16] + '.txt': b'[Glow]\n40000000 00000000 11111111\n'})
        initial = self.manager.import_package(source, 'external.debug', 'Debug', kind='cheats')
        before = self.snapshot()
        with self.assertRaisesRegex(external.Refused, 'ID already imported'):
            self.manager.import_package(self.package(assets()), 'external.debug.c000', 'Glow', kind='assets')
        self.assertEqual(self.snapshot(), before)
        self.assertEqual(self.manager.inspect(), initial)
        self.assertFalse((self.install / 'romfs/tsuchi/mods/external.debug.c000').exists())

    def test_legacy_registry_and_first_import_journal_keep_slot_nine_meaning(self):
        self.imported(count=1)
        legacy = self.manager._registry()
        legacy.pop('tsuchi_slot')
        self.manager.registry_path.write_bytes(external.canonical(legacy))
        self.assertEqual(self.manager.inspect()['mods'][0]['state'], 'disabled')
        with self.assertRaises(InterruptedError):
            self.manager.set_enabled('external.onion', True, fail_after_move=1)
        self.assertEqual(self.manager.recover()['mods'][0]['state'], 'enabled')
        with self.assertRaisesRegex(external.Refused, 'Tsuchi slot differs'):
            external.ExternalMods(self.install, game=GAME, tsuchi_slot=0).inspect()

        # Historical first-import journals can precede the registry's creation.
        other = self.home / 'old-first-import'
        other.mkdir()
        manager = external.ExternalMods(other, game=GAME)
        before = manager._empty()
        before.pop('tsuchi_slot')
        after = copy.deepcopy(before)
        after['generation'] += 1
        logical = 'romfs/Carrot4/legacy.bin'
        payload = other / '.tsuchi-external/payload/external.legacy' / logical
        payload.parent.mkdir(parents=True)
        payload.write_bytes(b'legacy')
        after['mods'] = [{'id': 'external.legacy', 'name': 'Legacy', 'version': '1.0.0', 'enabled': False,
                          'files': {logical: external.digest(payload)}}]
        manager.journal_path.write_bytes(external.canonical({'format': 1, 'before': before, 'after': after, 'moves': []}))
        self.assertEqual(manager.recover()['mods'][0]['state'], 'disabled')


if __name__ == '__main__':
    unittest.main()
