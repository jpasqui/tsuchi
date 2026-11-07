"""Carry reads against a made-up game's native carrier records."""
import shutil
import subprocess
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
import generate


class Carry(unittest.TestCase):
    @unittest.skipUnless(shutil.which('clang++'), 'needs clang++')
    def test_reads_and_refusals(self):
        generate.write(ROOT / 'build' / 'gen')
        program = ROOT / 'build' / 'carry-backend-test.exe'
        gen = ROOT / 'build' / 'gen'
        command = [shutil.which('clang++'), '-std=c++17', '-Wall', '-Wextra', '-Werror',
                   '-fno-exceptions', '-fno-rtti', '-I', gen / 'include', '-I', gen / 'framework',
                   '-I', ROOT / 'framework' / 'base', '-I', ROOT / 'framework' / 'core',
                   '-I', ROOT / 'framework' / 'game', ROOT / 'framework' / 'core' / 'registry.cpp',
                   gen / 'framework' / 'carry_service.cpp', ROOT / 'framework' / 'game' / 'carry_backend.cpp',
                   ROOT / 'tests' / 'game' / 'carry_backend_test.cpp', '-o', program]
        built = subprocess.run([str(piece) for piece in command], capture_output=True, text=True)
        self.assertEqual(built.returncode, 0, built.stdout + built.stderr)
        run = subprocess.run([str(program)], capture_output=True, text=True)
        self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
        self.assertIn('carry reads passed', run.stdout)
