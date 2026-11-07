"""Import unchanged files, choose activation in the Mods controller, and apply."""
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
import external_mods
from test_ui_mod_list import build_fixture


class ExternalModIntegration(unittest.TestCase):
    def test_import_menu_save_apply_and_next_launch_observation(self):
        binary = build_fixture()
        with tempfile.TemporaryDirectory(prefix='tsuchi-external-menu-') as temporary:
            home = Path(temporary)
            source, install = home / 'package', home / 'install'
            files = {'exefs/subsdk8': b'NSO0stand-in module; never executed',
                     'romfs/Carrot4/Content/external-example.bin': b'unchanged asset bytes'}
            for name, value in files.items():
                path = source / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_bytes(value)
            install.mkdir()
            manager = external_mods.ExternalMods(install)
            manager.import_package(source, 'external.example', 'External example')
            catalog = install / 'romfs/tsuchi/external-mods.json'
            request = home / 'exported-external-requests.json'
            before = manager.inspect()
            self.assertEqual(before['mods'][0]['state'], 'disabled')
            for choice in ('enable', 'disable'):
                ran = subprocess.run([str(binary), str(catalog), str(request), choice],
                                     capture_output=True, text=True)
                self.assertEqual(ran.returncode, 0, ran.stdout + ran.stderr)
                requested = json.loads(request.read_text(encoding='utf-8'))
                self.assertEqual(requested['revision'], before['revision'])
                self.assertEqual(requested['enabled'], {'external.example': choice == 'enable'})
                # Saving the menu changes only requests; files wait for the manager.
                for name in files:
                    self.assertEqual((install / name).exists(), choice == 'disable')
                applied = manager.apply(request)
                self.assertEqual(applied['mods'][0]['state'], 'enabled' if choice == 'enable' else 'disabled')
                self.assertNotEqual(applied['revision'], before['revision'])
                for name, value in files.items():
                    target = install / name
                    self.assertEqual(target.exists(), choice == 'enable')
                    if target.exists():
                        self.assertEqual(target.read_bytes(), value)
                    self.assertEqual((source / name).read_bytes(), value)
                with self.assertRaises(external_mods.Refused):
                    manager.apply(request)
                before = applied

    def test_individual_cheats_and_unchanged_foreign_slot_nine(self):
        binary = build_fixture()
        for layout in ('emulator', 'console'):
            with self.subTest(layout=layout), tempfile.TemporaryDirectory(prefix='tsuchi-cheat-menu-') as temporary:
                home = Path(temporary)
                install = home / 'install'
                install.mkdir()
                manager = external_mods.ExternalMods(install, layout=layout, tsuchi_slot=8)
                title = manager.game['title']
                base = install if layout == 'emulator' else install / 'atmosphere/contents' / title
                own_module = base / 'exefs/subsdk8'
                own_module.parent.mkdir(parents=True)
                own_bytes = b'NSO0controlled Tsuchi stand-in; never executed'
                own_module.write_bytes(own_bytes)
                foreign = home / 'foreign/exefs/subsdk9'
                foreign.parent.mkdir(parents=True)
                foreign_bytes = b'NSO0unchanged foreign stand-in; never executed'
                foreign.write_bytes(foreign_bytes)
                manager.import_package(foreign.parent.parent, 'external.foreign', 'Foreign module')
                manager.set_enabled('external.foreign', True)

                build_name = manager.game['build'][:16] + '.txt'
                source = home / 'cheat-set/cheats' / build_name
                source.parent.mkdir(parents=True)
                original = (b'{Shared}\n40010000 00000000 4D415354\n'
                            b'[Alpha]\n40000000 00000000 54534331\nFFF8A400 FFF8C410\n'
                            b'[Beta]\n40000000 00000000 54534332\nFFF8B400 FFF8C410\n')
                source.write_bytes(original)
                manager.import_package(source.parent.parent, 'external.codes', 'Codes', kind='cheats')
                catalog = base / 'romfs/tsuchi/external-mods.json'
                installed = base / 'cheats' / build_name
                toggles = base / 'cheats/toggles.txt'
                exported = home / 'exported-requests.json'
                code_ids = {'Alpha': 'external.codes.c000', 'Beta': 'external.codes.c001'}
                rows = {row['id']: row for row in manager.inspect()['mods']}
                self.assertEqual({rows[value]['name'] for value in code_ids.values()}, set(code_ids))
                self.assertTrue(all(rows[value]['state'] == 'disabled' for value in code_ids.values()))

                for selected in ('Alpha', 'Beta'):
                    for enabled in (True, False):
                        before = manager.inspect()
                        ran = subprocess.run([str(binary), str(catalog), str(exported),
                                              'enable' if enabled else 'disable', code_ids[selected]],
                                             capture_output=True, text=True)
                        self.assertEqual(ran.returncode, 0, ran.stdout + ran.stderr)
                        request = json.loads(exported.read_text(encoding='utf-8'))
                        self.assertEqual(request['revision'], before['revision'])
                        self.assertEqual(installed.exists(), not enabled)
                        after = manager.apply(exported)
                        self.assertNotEqual(after['revision'], before['revision'])
                        self.assertEqual(installed.exists(), enabled)
                        if enabled:
                            text = installed.read_text(encoding='utf-8')
                            self.assertIn('{Shared}', text)
                            self.assertIn('[' + selected + ']', text)
                            self.assertNotIn('[Beta]' if selected == 'Alpha' else '[Alpha]', text)
                            if layout == 'console':
                                self.assertEqual(toggles.read_text(encoding='utf-8'), '[' + selected + ']\ntrue\n')
                        self.assertEqual(toggles.exists(), enabled and layout == 'console')
                        self.assertEqual(own_module.read_bytes(), own_bytes)
                        self.assertEqual((base / 'exefs/subsdk9').read_bytes(), foreign_bytes)
                        self.assertEqual(source.read_bytes(), original)
                        self.assertEqual(foreign.read_bytes(), foreign_bytes)
                        with self.assertRaises(external_mods.Refused):
                            manager.apply(exported)


if __name__ == '__main__':
    unittest.main()
