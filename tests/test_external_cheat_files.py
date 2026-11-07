"""Generated cheat ownership and recovery in disposable install trees."""
import copy
import hashlib
import json
from pathlib import Path
import re
import sys
import tempfile
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
import external_mods as external

GAME = {'title': '0100B7C00933A000', 'version': '1.1.1',
        'build': '060F827BD2B38703E9EA11E3210D89BC', 'project': 'Carrot4'}
BUILD = GAME['build'][:16]
PREFIX = 'external.debug'
FIRST = PREFIX + '.c000'
SECOND = PREFIX + '.c001'
SOURCE = (b'{Shared setup}\n400F0000 00000000 12345678\n\n'
          b'[First code]\n40000000 00000000 11111111\n\n'
          b'[Second code]\n40010000 00000000 22222222\n')


class ExternalCheatFiles(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='tsuchi-cheat-files-test-')
        self.addCleanup(self.temp.cleanup)
        self.home = Path(self.temp.name)
        self.install = self.home / 'install'
        self.install.mkdir()
        self.manager = external.ExternalMods(self.install, game=GAME)
        self.counter = 0

    def package(self, data=SOURCE, files=None):
        self.counter += 1
        folder = self.home / ('package-' + str(self.counter))
        folder.mkdir()
        for relative, contents in (files or {'cheats/' + BUILD + '.txt': data}).items():
            path = folder / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(contents)
        return folder

    def imported(self, manager=None, prefix=PREFIX, source=SOURCE):
        manager = manager or self.manager
        folder = self.package(source)
        result = manager.import_package(folder, prefix, 'Debug choices', kind='cheats')
        return folder, result

    def active(self, manager=None):
        manager = manager or self.manager
        base = manager.root if manager.layout == 'emulator' else (
            manager.root / 'atmosphere/contents' / GAME['title'])
        return base / 'cheats' / (BUILD + '.txt')

    def toggles(self, manager):
        return manager.root / 'atmosphere/contents' / GAME['title'] / 'cheats/toggles.txt'

    def snapshot(self, install=None):
        install = install or self.install
        return {p.relative_to(install).as_posix(): p.read_bytes()
                for p in install.rglob('*') if p.is_file() and p.name != 'manager.lock'}

    def request(self, changes, manager=None):
        manager = manager or self.manager
        return {'format': 1, 'revision': manager.inspect()['revision'], 'enabled': changes}

    def headers(self, manager=None):
        text = self.active(manager).read_text(encoding='utf-8')
        return [line[1:-1] for line in text.splitlines()
                if re.fullmatch(r'\[[^\]]+\]|\{[^}]+\}', line)]

    def assert_choices(self, selected, manager=None):
        manager = manager or self.manager
        expected = ['Shared setup'] + selected if selected else []
        if selected:
            self.assertTrue(self.active(manager).is_file())
            self.assertEqual(self.headers(manager), expected)
            self.assertLess(self.active(manager).stat().st_size, 64 * 1024)
        else:
            self.assertFalse(self.active(manager).exists())

    def regular(self, manager=None, enabled=False):
        manager = manager or self.manager
        folder = self.package(files={'romfs/Carrot4/Content/choice.bin': b'ordinary payload'})
        return manager.import_package(folder, 'external.asset', 'Asset', enable=enabled)

    def test_import_has_one_row_per_normal_block_and_starts_off(self):
        folder, result = self.imported()
        self.assertEqual(result['mods'], [
            {'id': FIRST, 'name': 'First code', 'version': '1.0.0', 'kind': 'cheats',
             'state': 'disabled', 'problem': 'Required shared code: Shared setup', 'manageable': True},
            {'id': SECOND, 'name': 'Second code', 'version': '1.0.0', 'kind': 'cheats',
             'state': 'disabled', 'problem': 'Required shared code: Shared setup', 'manageable': True}])
        retained = self.install / '.tsuchi-external/cheats/source.txt'
        self.assertEqual(retained.read_bytes(), SOURCE)
        self.assertEqual((folder / 'cheats' / (BUILD + '.txt')).read_bytes(), SOURCE)
        self.assert_choices([])
        self.assertEqual(json.loads(self.manager.catalog_path.read_text(encoding='utf-8')), result)
        self.assertEqual(self.manager.inspect(), result)

    def test_selection_keeps_master_first_and_source_order_then_removes_all_off(self):
        self.imported()
        self.manager.set_enabled(SECOND, True)
        self.assert_choices(['Second code'])
        self.assertNotIn('11111111', self.active().read_text(encoding='utf-8'))
        self.manager.set_enabled(FIRST, True)
        self.assert_choices(['First code', 'Second code'])
        self.manager.set_enabled(SECOND, False)
        self.assert_choices(['First code'])
        result = self.manager.set_enabled(FIRST, False)
        self.assert_choices([])
        self.assertEqual([m['state'] for m in result['mods']], ['disabled', 'disabled'])
        self.assertEqual((self.install / '.tsuchi-external/cheats/source.txt').read_bytes(), SOURCE)
        self.assertFalse(self.manager.journal_path.exists())

    def test_without_master_only_selected_normal_blocks_are_rendered(self):
        self.imported(source=SOURCE[SOURCE.index(b'[First code]'):])
        self.manager.set_enabled(SECOND, True)
        self.assertEqual(self.headers(), ['Second code'])
        self.assertNotIn('12345678', self.active().read_text(encoding='utf-8'))

    def test_new_instance_preserves_ids_selection_and_revision(self):
        self.imported()
        result = self.manager.set_enabled(SECOND, True)
        self.manager = external.ExternalMods(self.install, game=GAME)
        self.assertEqual(self.manager.inspect(), result)
        self.assert_choices(['Second code'])
        self.manager.set_enabled(FIRST, True)
        self.assert_choices(['First code', 'Second code'])

    def test_unchanged_selection_preserves_generation_and_revision(self):
        self.imported()
        first = self.manager.set_enabled(FIRST, True)
        registry = self.manager.registry_path.read_bytes()
        result = self.manager.apply(self.request({FIRST: True, SECOND: False}))
        self.assertEqual(result, first)
        self.assertEqual(self.manager.registry_path.read_bytes(), registry)
        self.assertFalse(self.manager.journal_path.exists())

    def test_stale_export_cannot_replay_after_selection_returns_to_original(self):
        self.imported()
        request = self.request({FIRST: True})
        self.manager.apply(request)
        self.manager.set_enabled(FIRST, False)
        before = self.snapshot()
        with self.assertRaisesRegex(external.Refused, 'revision'):
            self.manager.apply(request)
        self.assertEqual(self.snapshot(), before)
        self.assert_choices([])

    def test_master_and_set_prefix_are_not_independently_manageable(self):
        self.imported()
        for mod_id in (PREFIX, PREFIX + '.master', PREFIX + '.c002'):
            with self.subTest(mod_id=mod_id), self.assertRaises(external.Refused):
                self.manager.set_enabled(mod_id, True)
        for value in (1, None, 'true'):
            with self.subTest(value=value), self.assertRaises(external.Refused):
                self.manager.apply(self.request({FIRST: value}))
        self.assert_choices([])

    def test_import_cannot_enable_all_cheats_implicitly(self):
        with self.assertRaises(external.Refused):
            self.manager.import_package(self.package(), PREFIX, 'Debug choices', enable=True, kind='cheats')
        self.assertEqual(self.manager.inspect()['mods'], [])
        self.assertFalse((self.install / '.tsuchi-external/cheats/source.txt').exists())
        self.assert_choices([])

    def test_console_outputs_selected_true_toggles_and_removes_both_files(self):
        manager = external.ExternalMods(self.install, layout='console', game=GAME)
        self.imported(manager)
        self.assertFalse(self.toggles(manager).exists())
        manager.set_enabled(SECOND, True)
        self.assert_choices(['Second code'], manager)
        self.assertEqual(self.toggles(manager).read_text(encoding='utf-8').split(),
                         ['[Second', 'code]', 'true'])
        manager.set_enabled(FIRST, True)
        text = self.toggles(manager).read_text(encoding='utf-8')
        self.assertNotIn('Shared setup', text)
        self.assertLess(text.index('[First code]'), text.index('[Second code]'))
        self.assertEqual(text.split().count('true'), 2)
        manager.apply(self.request({FIRST: False, SECOND: False}, manager))
        self.assert_choices([], manager)
        self.assertFalse(self.toggles(manager).exists())
        self.assertFalse((self.install / 'cheats').exists())

    def test_unmanaged_cheat_or_console_toggle_is_preserved_even_with_same_bytes(self):
        for layout, which in (('emulator', 'cheat'), ('console', 'cheat'), ('console', 'toggle')):
            with self.subTest(layout=layout, which=which):
                target = self.home / (layout + '-' + which)
                target.mkdir()
                manager = external.ExternalMods(target, layout=layout, game=GAME)
                foreign = self.active(manager) if which == 'cheat' else self.toggles(manager)
                foreign.parent.mkdir(parents=True)
                foreign.write_bytes(SOURCE)
                before = self.snapshot(target)
                with self.assertRaises(external.Refused):
                    self.imported(manager)
                self.assertEqual(self.snapshot(target), before)
                self.assertEqual(foreign.read_bytes(), SOURCE)

    def test_one_build_matched_set_is_required_and_a_second_set_is_refused(self):
        wrong = '0' * 16
        for files in ({'cheats/' + wrong + '.txt': SOURCE},
                      {'cheats/' + BUILD.lower() + '.txt': SOURCE},
                      {'cheats/' + GAME['build'] + '.txt': SOURCE},
                      {'cheats/' + BUILD + '.txt': SOURCE, 'readme.txt': b'read me'},
                      {'cheats/' + BUILD + '.txt': SOURCE, 'cheats/' + wrong + '.txt': SOURCE},
                      {'romfs/cheats/' + BUILD + '.txt': SOURCE}):
            with self.subTest(files=list(files)), self.assertRaises(external.Refused):
                self.manager.import_package(self.package(files=files), PREFIX, 'Debug choices', kind='cheats')
        self.imported()
        before = self.snapshot()
        with self.assertRaises(external.Refused):
            self.imported(prefix='external.other')
        self.assertEqual(self.snapshot(), before)

    def test_cheat_prefix_length_reserves_space_for_stable_virtual_ids(self):
        prefix = 'external.' + 'x' * 33
        self.assertEqual(len(prefix), 42)
        _, result = self.imported(prefix=prefix)
        self.assertEqual(result['mods'][0]['id'], prefix + '.c000')
        self.assertEqual(len(result['mods'][0]['id']), 47)
        other = self.home / 'too-long'
        other.mkdir()
        manager = external.ExternalMods(other, game=GAME)
        with self.assertRaises(external.Refused):
            self.imported(manager, prefix=prefix + 'x')
        self.assertFalse((other / '.tsuchi-external/cheats/source.txt').exists())

    def test_native_id_collision_refuses_import_and_later_blocks_all_writes(self):
        native = self.install / 'romfs/tsuchi/mods' / SECOND
        native.mkdir(parents=True)
        with self.assertRaises(external.Refused):
            self.imported()
        self.assertFalse((self.install / '.tsuchi-external/cheats/source.txt').exists())
        native.rmdir()
        self.imported()
        native.mkdir()
        entry = next(m for m in self.manager.inspect()['mods'] if m['id'] == SECOND)
        self.assertEqual(entry['state'], 'conflict')
        self.assertFalse(entry['manageable'])
        before = self.snapshot()
        with self.assertRaises(external.Refused):
            self.manager.set_enabled(FIRST, True)
        self.assertEqual(self.snapshot(), before)

    def test_virtual_ids_cannot_collide_with_managed_regular_packages(self):
        self.manager.import_package(self.package(files={'romfs/Carrot4/regular': b'file'}),
                                    FIRST, 'Regular package')
        before = self.snapshot()
        with self.assertRaises(external.Refused):
            self.imported()
        self.assertEqual(self.snapshot(), before)

    def test_changed_source_blocks_apply_and_cannot_change_selected_names(self):
        self.imported()
        self.manager.set_enabled(FIRST, True)
        source = self.install / '.tsuchi-external/cheats/source.txt'
        source.write_bytes(SOURCE.replace(b'First code', b'Other code'))
        before = self.snapshot()
        observed = self.manager.inspect()
        self.assertTrue(all(m['state'] == 'conflict' and not m['manageable'] for m in observed['mods']))
        with self.assertRaises(external.Refused):
            self.manager.set_enabled(FIRST, False)
        self.assertEqual(self.snapshot(), before)
        self.assert_choices(['First code'])

    def test_unknown_generated_replacement_blocks_apply_without_overwrite(self):
        self.imported()
        self.manager.set_enabled(FIRST, True)
        self.active().write_bytes(b'foreign replacement')
        before = self.snapshot()
        with self.assertRaises(external.Refused):
            self.manager.set_enabled(SECOND, True)
        self.assertEqual(self.snapshot(), before)
        self.assertEqual(self.active().read_bytes(), b'foreign replacement')
        self.assertFalse(self.manager.journal_path.exists())

    def test_file_created_while_disabled_is_preserved_before_enable(self):
        self.imported()
        self.active().parent.mkdir(parents=True)
        self.active().write_bytes(b'unmanaged cheat file')
        before = self.snapshot()
        with self.assertRaises(external.Refused):
            self.manager.set_enabled(FIRST, True)
        self.assertEqual(self.snapshot(), before)
        self.assertFalse(self.manager.journal_path.exists())

    def test_changed_console_toggle_blocks_complete_output_replacement(self):
        manager = external.ExternalMods(self.install, layout='console', game=GAME)
        self.imported(manager)
        manager.set_enabled(FIRST, True)
        self.toggles(manager).write_bytes(b'[Foreign choice]\ntrue\n')
        before = self.snapshot()
        with self.assertRaises(external.Refused):
            manager.set_enabled(SECOND, True)
        self.assertEqual(self.snapshot(), before)
        self.assert_choices(['First code'], manager)
        self.assertFalse(manager.journal_path.exists())

    def test_selection_word_limit_includes_master_and_refuses_before_output_changes(self):
        words = ' '.join(['00000000'] * 256) + '\n\n'
        source = ('{Shared setup}\n' + words + ''.join(
            '[' + name + ']\n' + words for name in ('First code', 'Second code', 'Third code', 'Fourth code'))).encode()
        self.imported(source=source)
        third, fourth = PREFIX + '.c002', PREFIX + '.c003'
        self.manager.apply(self.request({FIRST: True, SECOND: True, third: True}))
        self.assert_choices(['First code', 'Second code', 'Third code'])
        before = self.snapshot()
        with self.assertRaisesRegex(ValueError, '1024'):
            self.manager.set_enabled(fourth, True)
        self.assertEqual(self.snapshot(), before)
        self.assertFalse(self.manager.journal_path.exists())

    def test_mixed_request_changes_files_and_cheats_in_one_generation(self):
        self.regular()
        self.imported()
        before = self.manager._registry()['generation']
        result = self.manager.apply(self.request({'external.asset': True, SECOND: True}))
        self.assertEqual(self.manager._registry()['generation'], before + 1)
        self.assertEqual((self.install / 'romfs/Carrot4/Content/choice.bin').read_bytes(), b'ordinary payload')
        self.assert_choices(['Second code'])
        self.assertEqual({m['id']: m['state'] for m in result['mods']},
                         {'external.asset': 'enabled', FIRST: 'disabled', SECOND: 'enabled'})
        self.assertFalse(self.manager.journal_path.exists())

    def test_every_generated_move_can_recover_to_the_requested_selection(self):
        # The three transitions cover new output, replacement and removal.
        for layout in ('emulator', 'console'):
            outputs = 1 if layout == 'emulator' else 2
            for initial, changes, selected, moves in (
                    ({}, {FIRST: True}, ['First code'], outputs),
                    ({FIRST: True}, {FIRST: False, SECOND: True}, ['Second code'], outputs * 2),
                    ({FIRST: True}, {FIRST: False}, [], outputs)):
                for number in range(1, moves + 1):
                    with self.subTest(layout=layout, changes=changes, after_move=number):
                        target = self.home / ('recover-' + str(self.counter))
                        target.mkdir()
                        manager = external.ExternalMods(target, layout=layout, game=GAME)
                        self.imported(manager)
                        if initial:
                            manager.apply(self.request(initial, manager))
                        with self.assertRaises(InterruptedError):
                            manager.apply(self.request(changes, manager), fail_after_move=number)
                        self.assertTrue(manager.journal_path.exists())
                        self.assertTrue(all(m['state'] == 'recovery' and not m['manageable']
                                            for m in manager.inspect()['mods']))
                        with self.assertRaises(external.Refused):
                            manager.set_enabled(SECOND, False)
                        manager = external.ExternalMods(target, layout=layout, game=GAME)
                        result = manager.recover()
                        self.assert_choices(selected, manager)
                        self.assertEqual(manager.recover(), result)
                        self.assertFalse(manager.journal_path.exists())

    def test_every_mixed_move_recovers_together_in_both_directions(self):
        for wanted in (True, False):
            for number in (1, 2, 3):
                # Console has one regular move plus two generated output moves.
                with self.subTest(enabled=wanted, after_move=number):
                    target = self.home / ('mixed-' + str(wanted) + '-' + str(number))
                    target.mkdir()
                    manager = external.ExternalMods(target, layout='console', game=GAME)
                    self.regular(manager, enabled=not wanted)
                    self.imported(manager)
                    if not wanted:
                        manager.set_enabled(FIRST, True)
                    with self.assertRaises(InterruptedError):
                        manager.apply(self.request({'external.asset': wanted, FIRST: wanted}, manager),
                                      fail_after_move=number)
                    result = external.ExternalMods(target, layout='console', game=GAME).recover()
                    self.assert_choices(['First code'] if wanted else [], manager)
                    self.assertTrue(all(m['state'] == ('enabled' if wanted else 'disabled')
                                        for m in result['mods'] if m['id'] != SECOND))
                    active = target / 'atmosphere/contents' / GAME['title'] / 'romfs/Carrot4/Content/choice.bin'
                    self.assertEqual(active.exists(), wanted)
                    self.assertFalse(manager.journal_path.exists())

    def test_recovery_preserves_unknown_active_replacement_then_resumes(self):
        self.imported()
        self.manager.set_enabled(FIRST, True)
        with self.assertRaises(InterruptedError):
            self.manager.apply(self.request({FIRST: False, SECOND: True}), fail_after_move=1)
        self.assertFalse(self.active().exists())
        self.active().write_bytes(b'foreign replacement')
        before = self.snapshot()
        with self.assertRaises(external.Refused):
            self.manager.recover()
        self.assertEqual(self.snapshot(), before)
        preserved = self.home / 'preserved-cheats.txt'
        self.active().rename(preserved)
        self.manager = external.ExternalMods(self.install, game=GAME)
        self.manager.recover()
        self.assert_choices(['Second code'])
        self.assertEqual(preserved.read_bytes(), b'foreign replacement')

    def test_changed_source_also_blocks_pending_recovery(self):
        self.imported()
        with self.assertRaises(InterruptedError):
            self.manager.set_enabled(FIRST, True, fail_after_move=1)
        source = self.install / '.tsuchi-external/cheats/source.txt'
        source.write_bytes(SOURCE + b'\n')
        before = self.snapshot()
        with self.assertRaises(external.Refused):
            self.manager.recover()
        self.assertEqual(self.snapshot(), before)
        self.assertTrue(self.manager.journal_path.exists())

    def test_catalog_publication_failure_keeps_recoverable_generated_outputs(self):
        self.imported()
        self.manager.set_enabled(FIRST, True)
        original = external._write
        def fail_catalog(path, value):
            if path == self.manager.catalog_path:
                raise OSError('simulated cheat catalog publication failure')
            return original(path, value)
        with mock.patch.object(external, '_write', side_effect=fail_catalog):
            with self.assertRaisesRegex(OSError, 'publication failure'):
                self.manager.apply(self.request({FIRST: False, SECOND: True}))
        self.assertTrue(self.manager.journal_path.exists())
        self.assert_choices(['Second code'])
        result = external.ExternalMods(self.install, game=GAME).recover()
        self.assertEqual(json.loads(self.manager.catalog_path.read_text(encoding='utf-8')), result)
        self.assertFalse(self.manager.journal_path.exists())

    def test_registry_source_descriptor_and_names_are_not_trusted(self):
        self.imported()
        original = json.loads(self.manager.registry_path.read_text(encoding='utf-8'))
        for alteration in ('source_hash', 'source_size', 'name', 'selection'):
            forged = copy.deepcopy(original)
            if alteration == 'source_hash':
                forged['cheats']['source']['sha256'] = '0' * 64
            elif alteration == 'source_size':
                forged['cheats']['source']['bytes'] += 1
            elif alteration == 'name':
                forged['cheats']['codes'][0] = 'Invented code'
            else:
                forged['cheats']['enabled'] = [127]
            self.manager.registry_path.write_bytes(external.canonical(forged))
            before = self.snapshot()
            with self.subTest(alteration=alteration), self.assertRaises(external.Refused):
                self.manager.set_enabled(FIRST, True)
            self.assertEqual(self.snapshot(), before)
        self.manager.registry_path.write_bytes(external.canonical(original))
        self.manager.set_enabled(FIRST, True)
        self.assert_choices(['First code'])

    def test_generated_journal_hashes_keys_and_paths_are_rederived(self):
        self.imported()
        self.manager.set_enabled(FIRST, True)
        with self.assertRaises(InterruptedError):
            self.manager.apply(self.request({FIRST: False, SECOND: True}), fail_after_move=1)
        original = json.loads(self.manager.journal_path.read_text(encoding='utf-8'))
        for alteration in ('old_hash', 'new_hash', 'new_size', 'unknown_key', 'path'):
            forged = copy.deepcopy(original)
            if alteration == 'old_hash':
                forged['generated']['before']['cheat']['sha256'] = '0' * 64
            elif alteration == 'new_hash':
                forged['generated']['after']['cheat']['sha256'] = '0' * 64
            elif alteration == 'new_size':
                forged['generated']['after']['cheat']['bytes'] += 1
            elif alteration == 'unknown_key':
                forged['generated']['after']['../outside'] = forged['generated']['after']['cheat']
            else:
                forged['generated']['after']['cheat']['path'] = '../outside'
            self.manager.journal_path.write_bytes(external.canonical(forged))
            before = self.snapshot()
            with self.subTest(alteration=alteration), self.assertRaises(external.Refused):
                self.manager.recover()
            self.assertEqual(self.snapshot(), before)
            self.assertFalse((self.home / 'outside').exists())
        self.manager.journal_path.write_bytes(external.canonical(original))
        self.manager.recover()
        self.assert_choices(['Second code'])

    def test_cleanup_interruption_recovers_after_registry_and_catalog_publication(self):
        manager = external.ExternalMods(self.install, layout='console', game=GAME)
        self.imported(manager)
        manager.set_enabled(FIRST, True)
        original = Path.unlink
        interrupted = False
        def fail_old_output(path, *args, **kwargs):
            nonlocal interrupted
            if path.suffix == '.old' and not interrupted:
                interrupted = True
                raise OSError('simulated old-output cleanup interruption')
            return original(path, *args, **kwargs)
        with mock.patch.object(Path, 'unlink', new=fail_old_output):
            with self.assertRaisesRegex(OSError, 'cleanup interruption'):
                manager.apply(self.request({FIRST: False, SECOND: True}, manager))
        self.assertTrue(interrupted)
        self.assertTrue(manager.journal_path.exists())
        self.assert_choices(['Second code'], manager)
        published = json.loads(manager.catalog_path.read_text(encoding='utf-8'))
        self.assertEqual([m['state'] for m in published['mods']], ['disabled', 'enabled'])
        self.assertTrue(all(m['state'] == 'recovery' for m in manager.inspect()['mods']))
        manager = external.ExternalMods(self.install, layout='console', game=GAME)
        self.assertEqual(manager.recover(), published)
        self.assertFalse(manager.journal_path.exists())
        self.assertFalse(list((self.install / '.tsuchi-external/cheats').rglob('*.old')))
        self.assert_choices(['Second code'], manager)

    def test_changed_retired_output_is_preserved_during_cleanup_recovery(self):
        self.imported()
        self.manager.set_enabled(FIRST, True)
        original = Path.unlink
        def fail_old_output(path, *args, **kwargs):
            if path.suffix == '.old':
                raise OSError('simulated old-output cleanup interruption')
            return original(path, *args, **kwargs)
        with mock.patch.object(Path, 'unlink', new=fail_old_output):
            with self.assertRaises(OSError):
                self.manager.apply(self.request({FIRST: False, SECOND: True}))
        retired = list((self.install / '.tsuchi-external/cheats').rglob('*.old'))
        self.assertEqual(len(retired), 1)
        retired[0].write_bytes(b'foreign retained replacement')
        before = self.snapshot()
        with self.assertRaises(external.Refused):
            self.manager.recover()
        self.assertEqual(self.snapshot(), before)
        self.assertEqual(retired[0].read_bytes(), b'foreign retained replacement')
        self.assert_choices(['Second code'])

    def test_combined_catalog_limit_counts_each_cheat_choice(self):
        # Seed valid ordinary packages to reach the real limit without repeatedly
        # rehashing all older packages through 129 separate import operations.
        registry = self.manager._empty()
        contents = b'ordinary payload'
        descriptor = {'bytes': len(contents), 'sha256': hashlib.sha256(contents).hexdigest()}
        for number in range(129):
            mod_id = 'external.regular' + str(number).zfill(3)
            logical = 'romfs/Carrot4/Content/file' + str(number) + '.bin'
            retained = self.install / '.tsuchi-external/payload' / mod_id / logical
            retained.parent.mkdir(parents=True, exist_ok=True)
            retained.write_bytes(contents)
            registry['mods'].append({'id': mod_id, 'name': mod_id, 'version': '1.0.0',
                                     'enabled': False, 'files': {logical: descriptor}})
        self.manager.registry_path.write_bytes(external.canonical(registry))
        source = ''.join('[Code ' + str(i) + ']\n40000000 00000000 00000000\n\n'
                         for i in range(127)).encode('utf-8')
        _, result = self.imported(source=source)
        self.assertEqual(len(result['mods']), 256)
        self.assertEqual(len({m['id'] for m in result['mods']}), 256)
        before = self.snapshot()
        with self.assertRaises(external.Refused):
            self.manager.import_package(self.package(files={'romfs/Carrot4/extra': b'extra'}),
                                        'external.extra', 'Extra')
        self.assertEqual(self.snapshot(), before)


if __name__ == '__main__':
    unittest.main()
