"""Declared settings use the shared UI model and hold cleanup."""
import shutil
import subprocess
import sys
import unittest
from pathlib import Path
ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
import generate


class UiSettingsTests(unittest.TestCase):
    def test_declared_kinds_and_cleanup(self):
        gen = ROOT / 'build/gen'
        generate.write(gen)
        binary = ROOT / 'build' / ('ui_settings_test.exe' if sys.platform == 'win32' else 'ui_settings_test')
        sources = ['framework/core/registry.cpp', 'framework/core/ids.cpp', 'framework/core/text.cpp',
                   'framework/core/texts.cpp', 'framework/core/json.cpp', 'framework/base/settings.cpp',
                   'framework/game/holds.cpp', 'framework/game/ui_screens.cpp',
                   'framework/game/ui_settings.cpp', 'tests/game/ui_settings_test.cpp']
        command = [shutil.which('clang++'), '-std=c++17', '-Wall', '-Wextra', '-Werror']
        for folder in [gen / 'include', gen / 'framework', ROOT / 'framework/core',
                       ROOT / 'framework/base', ROOT / 'framework/game']:
            command += ['-I', str(folder)]
        command += [str(ROOT / source) for source in sources]
        command += [str(gen / 'framework/ui_service.cpp'), '-o', str(binary)]
        compiled = subprocess.run(command, capture_output=True, text=True, encoding='utf-8')
        self.assertEqual(compiled.returncode, 0, compiled.stdout + compiled.stderr)
        ran = subprocess.run([str(binary)], capture_output=True, text=True, encoding='utf-8', timeout=120)
        self.assertEqual(ran.returncode, 0, ran.stdout + ran.stderr)


if __name__ == '__main__':
    unittest.main()
