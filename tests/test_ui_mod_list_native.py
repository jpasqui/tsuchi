"""Original Settings categories and row holds against the made-up game."""
import shutil
import subprocess
import sys
import unittest
from pathlib import Path
ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
import generate
from gen import game_table


class UiModListNativeTests(unittest.TestCase):
    @unittest.skipUnless(shutil.which('clang++'), 'needs clang++')
    def test_native_rows_lifecycle_and_restore(self):
        gen = ROOT / 'build/gen'
        generate.write(gen)
        work = ROOT / 'build/ui-mod-list-native-test'
        work.mkdir(parents=True, exist_ok=True)
        table = ROOT / 'tests/fixtures/game_table.yaml'
        for relative, source in game_table.generate(table, 'tests/fixtures/game_table.yaml').items():
            path = work / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            if not path.exists() or path.read_text(encoding='utf-8') != source:
                path.write_text(source, encoding='utf-8', newline='\n')
        binary = work / ('ui_mod_list_native_test.exe' if sys.platform == 'win32' else 'ui_mod_list_native_test')
        sources = ['framework/core/registry.cpp', 'framework/core/text.cpp', 'framework/base/names.cpp',
                   'framework/base/objects.cpp', 'framework/game/holds.cpp',
                   'framework/game/ui_mod_list_native.cpp', 'framework/game/ui_feedback.cpp', 'tests/game/ui_mod_list_native_test.cpp']
        command = [shutil.which('clang++'), '-std=c++17', '-Wall', '-Wextra', '-Werror']
        for folder in [gen / 'include', work / 'framework', gen / 'framework',
                       ROOT / 'framework/core', ROOT / 'framework/base', ROOT / 'framework/game', ROOT / 'tests/base']:
            command += ['-I', str(folder)]
        command += [str(ROOT / source) for source in sources] + ['-o', str(binary)]
        compiled = subprocess.run(command, capture_output=True, text=True)
        self.assertEqual(compiled.returncode, 0, compiled.stdout + compiled.stderr)
        ran = subprocess.run([str(binary)], capture_output=True, text=True)
        self.assertEqual(ran.returncode, 0, ran.stdout + ran.stderr)
        for mode in ['success', 'failure', 'prepare', 'borrow', 'refresh', 'fit']:
            with self.subTest(mode=mode):
                ran = subprocess.run([str(binary), mode], capture_output=True, text=True)
                self.assertEqual(ran.returncode, 0, ran.stdout + ran.stderr)


if __name__ == '__main__':
    unittest.main()
