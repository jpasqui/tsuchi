"""Player activation and load order before mods or containers start."""
import shutil
import subprocess
import sys
import unittest
from pathlib import Path
ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
import generate
class ModChoicesTests(unittest.TestCase):
    def test_saved_choices_and_order(self):
        gen = ROOT / 'build/gen'
        generate.write(gen)
        binary = ROOT / 'build' / ('mod_choices_test.exe' if sys.platform == 'win32' else 'mod_choices_test')
        command = [shutil.which('clang++'), '-std=c++17', '-Wall', '-Wextra', '-Werror',
                   '-I', str(gen / 'include'), '-I', str(ROOT / 'framework/core'), '-I', str(ROOT / 'framework/base'),
                   str(ROOT / 'framework/core/registry.cpp'), str(ROOT / 'framework/core/text.cpp'),
                   str(ROOT / 'framework/base/mod_choices.cpp'), str(ROOT / 'tests/base/mod_choices_test.cpp'), '-o', str(binary)]
        compiled = subprocess.run(command, capture_output=True, text=True)
        self.assertEqual(compiled.returncode, 0, compiled.stdout + compiled.stderr)
        ran = subprocess.run([str(binary)], capture_output=True, text=True)
        self.assertEqual(ran.returncode, 0, ran.stdout + ran.stderr)
    def test_committed_file_before_containers(self):
        from gen import game_table
        gen = ROOT / 'build/gen'
        generate.write(gen)
        fixture = ROOT / 'build/mod-choices-fixture'
        for name, text in game_table.generate(ROOT / 'tests/fixtures/game_table.yaml').items():
            path = fixture / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(text, encoding='utf-8')
        binary = ROOT / 'build' / ('mod_choices_game_test.exe' if sys.platform == 'win32' else 'mod_choices_game_test')
        command = [shutil.which('clang++'), '-std=c++17', '-Wall', '-Wextra', '-Werror',
                   '-I', str(gen / 'include'), '-I', str(fixture / 'framework'),
                   '-I', str(ROOT / 'framework/core'), '-I', str(ROOT / 'framework/base'),
                   '-I', str(ROOT / 'framework/game'), '-I', str(ROOT / 'tests/base'),
                   str(ROOT / 'framework/core/registry.cpp'), str(ROOT / 'framework/core/text.cpp'),
                   str(ROOT / 'framework/base/mod_choices.cpp'), str(ROOT / 'framework/game/mod_choices_game.cpp'),
                   str(ROOT / 'framework/core/json.cpp'), str(ROOT / 'framework/core/texts.cpp'),
                   str(ROOT / 'tests/game/mod_choices_game_test.cpp'), '-o', str(binary)]
        compiled = subprocess.run(command, capture_output=True, text=True)
        self.assertEqual(compiled.returncode, 0, compiled.stdout + compiled.stderr)
        ran = subprocess.run([str(binary)], capture_output=True, text=True)
        self.assertEqual(ran.returncode, 0, ran.stdout + ran.stderr)
if __name__ == '__main__': unittest.main()
