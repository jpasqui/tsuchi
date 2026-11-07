"""Content class-default identity through the production shared hold tables."""
import shutil
import subprocess
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
import generate


class ContentIdentity(unittest.TestCase):
    @unittest.skipUnless(shutil.which('clang++'), 'needs clang++')
    def test_identity_hold_paths(self):
        gen = ROOT / 'build' / 'gen'
        generate.write(gen)
        program = ROOT / 'build' / 'content-identity-test.exe'
        core = ROOT / 'framework' / 'core'
        game = ROOT / 'framework' / 'game'
        command = [shutil.which('clang++'), '-std=c++17', '-Wall', '-Wextra', '-Werror',
                   '-fno-exceptions', '-fno-rtti', '-I', gen / 'include', '-I', gen / 'framework',
                   '-I', ROOT / 'framework' / 'base', '-I', core, '-I', game,
                   core / 'registry.cpp', game / 'holds.cpp',
                   ROOT / 'tests' / 'game' / 'content_identity_test.cpp', '-o', program]
        built = subprocess.run([str(piece) for piece in command], capture_output=True, text=True)
        self.assertEqual(built.returncode, 0, built.stdout + built.stderr)
        run = subprocess.run([str(program)], capture_output=True, text=True)
        self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
        self.assertIn('content identity hold paths passed', run.stdout)
