"""The character service against a made-up game, with the real holds and generated validation."""
import shutil
import subprocess
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
import generate


class Character(unittest.TestCase):
    @unittest.skipUnless(shutil.which('clang++'), 'needs clang++')
    def test_inputs_actions_bindings_and_movement(self):
        gen = ROOT / 'build' / 'gen'
        generate.write(gen)
        program = ROOT / 'build' / 'tests' / 'character_backend_test.exe'
        program.parent.mkdir(parents=True, exist_ok=True)
        command = [shutil.which('clang++'), '-std=c++17', '-Wall', '-Wextra', '-Werror', '-fno-exceptions', '-fno-rtti']
        for directory in ['build/gen/include', 'build/gen/framework', 'framework/core', 'framework/base', 'framework/game']:
            command += ['-I', str(ROOT / directory)]
        for source in ['tests/game/character_backend_test.cpp', 'framework/core/registry.cpp', 'framework/game/holds.cpp',
                       'framework/game/character_backend.cpp', 'build/gen/framework/character_service.cpp']:
            command.append(str(ROOT / source))
        command += ['-o', str(program)]
        built = subprocess.run(command, capture_output=True, text=True)
        self.assertEqual(built.returncode, 0, built.stdout + built.stderr)
        ran = subprocess.run([str(program)], capture_output=True, text=True)
        self.assertEqual(ran.returncode, 0, ran.stdout + ran.stderr)
        self.assertIn('checks passed', ran.stdout)
        print(ran.stdout.strip())


if __name__ == '__main__':
    unittest.main()
