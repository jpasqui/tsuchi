"""External installed-file catalog and bounded saved activation requests."""
from pathlib import Path
import shutil
import subprocess
import sys
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
import generate
from gen import game_table


class ExternalModsGame(unittest.TestCase):
    def test_bounded_catalog_saved_requests_and_storage_failures(self):
        work = ROOT / 'build/external-mods-game'
        gen = work / 'gen'
        generate.write(gen)
        for name, text in game_table.generate(ROOT / 'tests/fixtures/game_table.yaml').items():
            path = work / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(text, encoding='utf-8')
        binary = work / ('adapter.exe' if sys.platform == 'win32' else 'adapter')
        includes = [gen / 'include', work / 'framework', ROOT / 'framework/core',
                    ROOT / 'framework/base', ROOT / 'framework/game', ROOT / 'tests/base']
        sources = ['framework/core/registry.cpp', 'framework/core/text.cpp',
                   'framework/core/texts.cpp', 'framework/core/json.cpp',
                   'framework/base/external_mods.cpp', 'framework/game/external_mods_game.cpp',
                   'tests/game/external_mods_game_test.cpp']
        command = [shutil.which('clang++'), '-std=c++17', '-Wall', '-Wextra', '-Werror',
                   *[arg for path in includes for arg in ('-I', str(path))],
                   *[str(ROOT / name) for name in sources], '-o', str(binary)]
        built = subprocess.run(command, capture_output=True, text=True)
        self.assertEqual(built.returncode, 0, built.stdout + built.stderr)
        ran = subprocess.run([str(binary)], capture_output=True, text=True)
        self.assertEqual(ran.returncode, 0, ran.stdout + ran.stderr)


if __name__ == '__main__':
    unittest.main()
