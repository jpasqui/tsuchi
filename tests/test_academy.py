"""The Academy schedule a mod supplies: its dates, and what the reader takes and refuses."""
import shutil
import subprocess
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
import generate


class Academy(unittest.TestCase):
    @unittest.skipUnless(shutil.which('clang++'), 'needs clang++')
    def test_schedule_reader_and_dates(self):
        gen = ROOT / 'build' / 'gen'
        generate.write(gen)
        program = ROOT / 'build' / 'academy-test.exe'
        core = ROOT / 'framework' / 'core'
        command = [shutil.which('clang++'), '-std=c++17', '-Wall', '-Wextra', '-Werror', '-fno-exceptions', '-fno-rtti',
                   '-I', gen / 'include', '-I', gen / 'framework', '-I', ROOT / 'framework' / 'base', '-I', core,
                   '-I', ROOT / 'framework' / 'game', '-I', ROOT / 'tests' / 'base',
                   core / 'json.cpp', core / 'text.cpp', core / 'texts.cpp', core / 'registry.cpp',
                   ROOT / 'tests' / 'game' / 'academy_test.cpp', '-o', program]
        built = subprocess.run([str(piece) for piece in command], capture_output=True, text=True)
        self.assertEqual(built.returncode, 0, built.stdout + built.stderr)
        run = subprocess.run([str(program)], capture_output=True, text=True)
        self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
        self.assertIn('academy schedule checks passed', run.stdout)


if __name__ == '__main__':
    unittest.main()
