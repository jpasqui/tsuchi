"""Run declared squad calls against a made-up native game."""
import shutil
import subprocess
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
import generate


def main():
    gen = ROOT / 'build/gen'
    generate.write(gen)
    program = ROOT / 'build/tests/content_squad_test.exe'
    program.parent.mkdir(parents=True, exist_ok=True)
    command = [shutil.which('clang++'), '-std=c++17', '-Wall', '-Wextra', '-Werror',
               '-fno-exceptions', '-fno-rtti']
    for directory in ['build/gen/include', 'build/gen/framework', 'framework/core',
                      'framework/base', 'framework/game']:
        command += ['-I', str(ROOT / directory)]
    for source in ['framework/core/registry.cpp', 'framework/core/ids.cpp',
                   'framework/core/json.cpp', 'framework/core/text.cpp', 'framework/game/holds.cpp',
                   'framework/game/content_squad.cpp', 'tests/game/content_squad_test.cpp']:
        command.append(str(ROOT / source))
    command += [str(gen / 'framework/squad_service.cpp'), str(gen / 'framework/asset_service.cpp'),
                '-o', str(program)]
    subprocess.run(command, check=True)
    subprocess.run([str(program)], check=True)
    print('content squad host calls and restore paths passed')


class ContentSquad(unittest.TestCase):
    @unittest.skipUnless(shutil.which('clang++'), 'needs clang++')
    def test_native_members_selection_and_restore(self):
        main()


if __name__ == '__main__':
    unittest.main()
