"""Focused Engine actor backend checks. Native world/lifetime behavior needs game proof."""
import json
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
import generate
import stage_test_mods

class EngineActorBackend(unittest.TestCase):
    def test_game_donor_group_is_staged_only_when_selected(self):
        # Exercise the real stager selection and receipt, without compiling mods
        # or claiming that the game-only donor assertions ran on this host.
        source_mod = ROOT / 'tests' / 'fixtures' / 'mods' / 'check.engine-actors'
        manifest = json.loads((source_mod / 'mod.json').read_text(encoding='utf-8'))
        self.assertEqual(len(manifest['meta']['checks']), 12)
        self.assertTrue(stage_test_mods.explicit_check(source_mod))
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source = root / 'mods'
            shutil.copytree(source_mod, source / source_mod.name)
            for name in ('check.script', 'check.compiled-rejected', 'check.default'):
                folder = source / name
                folder.mkdir(parents=True)
                data = {'id': name, 'name': name, 'version': '1.0.0'}
                if name == 'check.default':
                    data['meta'] = {'check': 'default', 'checks': ['ready']}
                (folder / 'mod.json').write_text(json.dumps(data), encoding='utf-8')
            (root / 'tests' / 'lua').mkdir(parents=True)
            (root / 'tests' / 'lua' / 'language.lua').write_text('print("probe")', encoding='utf-8')
            (root / 'build').mkdir()
            (root / 'build' / ('lua_host.exe' if sys.platform == 'win32' else 'lua_host')).write_bytes(b'')
            target = root / 'staged'
            with patch.object(stage_test_mods, 'ROOT', root), patch.object(stage_test_mods, 'SOURCE', source), \
                    patch.object(stage_test_mods.game_table, 'load', return_value={}), \
                    patch.object(stage_test_mods.ai_order_fixture, 'files', return_value={}), \
                    patch.object(stage_test_mods.subprocess, 'run', return_value=subprocess.CompletedProcess([], 0, b'probe\n')):
                defaults = stage_test_mods.stage(target=target)
                self.assertIn('check.default', defaults)
                self.assertNotIn('check.engine-actors', defaults)
                selected = stage_test_mods.stage(['engine-actors'], target=target)
                self.assertIn('check.engine-actors', selected)
                self.assertNotIn('check.default', selected)
                receipt = json.loads((target / 'checks.json').read_text(encoding='utf-8'))
                self.assertEqual(receipt['check_mods'], {'check.engine-actors': 'engine-actors'})
                self.assertEqual(receipt['checks'], {'check.engine-actors': manifest['meta']['checks']})
                staged_mod = target / 'romfs' / 'tsuchi' / 'mods' / 'check.engine-actors'
                self.assertEqual((staged_mod / 'main.lua').read_bytes(), (source_mod / 'main.lua').read_bytes())
                self.assertEqual((staged_mod / 'check.lua').read_bytes(), stage_test_mods.HELPER.read_bytes())

    def test_root_pose_and_world_list_do_not_use_gameplay_fields(self):
        out = ROOT / 'build'
        gen = out / 'gen'
        generate.write(gen)
        binary = out / ('actors_backend_test.exe' if sys.platform == 'win32' else 'actors_backend_test')
        command = [shutil.which('clang++'), '-std=c++17', '-Wall', '-Wextra', '-Werror',
                   '-I', str(gen / 'include'), '-I', str(gen / 'framework'),
                   '-I', str(ROOT / 'framework' / 'base'), '-I', str(ROOT / 'framework' / 'game'),
                   str(ROOT / 'framework' / 'game' / 'actors_backend.cpp'),
                   str(ROOT / 'tests' / 'game' / 'actors_backend_test.cpp'), '-o', str(binary)]
        compiled = subprocess.run(command, capture_output=True, text=True)
        self.assertEqual(compiled.returncode, 0, compiled.stdout + compiled.stderr)
        ran = subprocess.run([str(binary)], capture_output=True, text=True)
        self.assertEqual(ran.returncode, 0, ran.stdout + ran.stderr)

if __name__ == '__main__':
    unittest.main()
