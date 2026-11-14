"""Declared settings use the shared UI model and hold cleanup."""
import shutil
import subprocess
import sys
import unittest
from pathlib import Path
ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
import generate


def build_fixture():
    """Build the host controller once for UI and companion integration tests."""
    global _fixture
    if _fixture is not None:
        return _fixture
    gen = ROOT / 'build/ui-mod-list/gen'
    generate.write(gen)
    binary = ROOT / 'build' / ('ui_mod_list_test.exe' if sys.platform == 'win32' else 'ui_mod_list_test')
    sources = ['framework/core/registry.cpp', 'framework/core/ids.cpp', 'framework/core/text.cpp',
               'framework/core/texts.cpp', 'framework/core/json.cpp', 'framework/base/mod_choices.cpp',
               'framework/base/mod_manager.cpp', 'framework/base/settings.cpp', 'framework/base/external_mods.cpp',
               'framework/game/holds.cpp', 'framework/game/ui_screens.cpp',
               'framework/game/ui_mod_list.cpp', 'framework/game/ui_cheats.cpp', 'tests/game/ui_mod_list_test.cpp']
    command = [shutil.which('clang++'), '-std=c++17', '-Wall', '-Wextra', '-Werror']
    for folder in [gen / 'include', gen / 'framework', ROOT / 'framework/core',
                   ROOT / 'framework/base', ROOT / 'framework/game']:
        command += ['-I', str(folder)]
    command += [str(ROOT / source) for source in sources]
    command += [str(gen / 'framework/ui_service.cpp'), str(gen / 'framework/dialog_service.cpp'), '-o', str(binary)]
    compiled = subprocess.run(command, capture_output=True, text=True, encoding='utf-8')
    if compiled.returncode:
        raise RuntimeError(compiled.stdout + compiled.stderr)
    _fixture = binary
    return binary


_fixture = None


class UiModListTests(unittest.TestCase):
    def test_player_review_order_settings_save_restore(self):
        binary = build_fixture()
        ran = subprocess.run([str(binary)], capture_output=True, text=True, encoding='utf-8')
        self.assertEqual(ran.returncode, 0, ran.stdout + ran.stderr)


if __name__ == '__main__':
    unittest.main()
