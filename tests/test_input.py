"""The input service against a made-up game, with the real holds and generated validation, on both tables."""
import shutil
import subprocess
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
import generate
from gen import game_table


class Input(unittest.TestCase):
    @unittest.skipUnless(shutil.which('clang++'), 'needs clang++')
    def test_reads_holds_keys_and_import_slots(self):
        generate.write(ROOT / 'build' / 'gen')
        fixture = ROOT / 'build' / 'gen-fixture'
        for relative, text in game_table.generate(ROOT / 'tests' / 'fixtures' / 'game_table.yaml',
                                                  'tests/fixtures/game_table.yaml').items():
            path = fixture / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            if not path.exists() or path.read_text(encoding='utf-8') != text:
                path.write_text(text, encoding='utf-8', newline='\n')
        for name, table in (('real', None), ('fixture', fixture / 'framework')):
            with self.subTest(table=name):
                self.run_stand_in(ROOT / 'build' / f'input-backend-test-{name}.exe', table)

    def run_stand_in(self, program, table):
        gen = ROOT / 'build' / 'gen'
        command = [shutil.which('clang++'), '-std=c++17', '-Wall', '-Wextra', '-Werror', '-fno-exceptions', '-fno-rtti']
        if table:
            command += ['-I', table]
        command += ['-I', gen / 'include', '-I', gen / 'framework', '-I', ROOT / 'framework' / 'base',
                    '-I', ROOT / 'framework' / 'core', '-I', ROOT / 'framework' / 'game',
                    ROOT / 'tests' / 'game' / 'input_backend_test.cpp', ROOT / 'framework' / 'core' / 'registry.cpp',
                    ROOT / 'framework' / 'game' / 'holds.cpp', ROOT / 'framework' / 'game' / 'input_backend.cpp',
                    gen / 'framework' / 'input_service.cpp', '-o', program]
        built = subprocess.run([str(piece) for piece in command], capture_output=True, text=True)
        self.assertEqual(built.returncode, 0, built.stdout + built.stderr)
        ran = subprocess.run([str(program)], capture_output=True, text=True)
        self.assertEqual(ran.returncode, 0, ran.stdout + ran.stderr)
        self.assertIn('input: ', ran.stdout)
        self.assertIn('checks passed', ran.stdout)


if __name__ == '__main__':
    unittest.main()
