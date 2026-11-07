"""Owned UMG widgets against host memory and the real handles, holds and events."""
import shutil
import subprocess
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
import generate
from gen import game_table


class UiWidgetsTests(unittest.TestCase):
    @unittest.skipUnless(shutil.which('clang++'), 'needs clang++')
    def test_owned_widgets_and_native_button_observation(self):
        gen = ROOT / 'build' / 'gen'
        generate.write(gen)
        fixture = ROOT / 'build' / 'gen-fixture'
        for relative, source in game_table.generate(
                ROOT / 'tests' / 'fixtures' / 'game_table.yaml',
                'tests/fixtures/game_table.yaml').items():
            path = fixture / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            if not path.exists() or path.read_text(encoding='utf-8') != source:
                path.write_text(source, encoding='utf-8', newline='\n')
        binary = ROOT / 'build' / ('ui_widgets_test.exe' if sys.platform == 'win32' else 'ui_widgets_test')
        sources = [
            'framework/core/registry.cpp', 'framework/core/ids.cpp', 'framework/core/text.cpp', 'framework/core/texts.cpp',
            'framework/base/names.cpp', 'framework/base/objects.cpp',
            'framework/game/holds.cpp', 'framework/game/object_retention.cpp',
            'framework/game/ui_widgets.cpp', 'tests/game/ui_widgets_test.cpp',
            'build/gen/framework/ui_service.cpp',
        ]
        command = [shutil.which('clang++'), '-std=c++17', '-Wall', '-Wextra', '-Werror']
        for folder in [gen / 'include', fixture / 'framework', gen / 'framework',
                       ROOT / 'framework/core', ROOT / 'framework/base',
                       ROOT / 'framework/game', ROOT / 'tests/base']:
            command += ['-I', str(folder)]
        command += [str(ROOT / source) for source in sources] + ['-o', str(binary)]
        compiled = subprocess.run(command, capture_output=True, text=True)
        self.assertEqual(compiled.returncode, 0, compiled.stdout + compiled.stderr)
        ran = subprocess.run([str(binary)], capture_output=True, text=True)
        self.assertEqual(ran.returncode, 0, ran.stdout + ran.stderr)


if __name__ == '__main__':
    unittest.main()
