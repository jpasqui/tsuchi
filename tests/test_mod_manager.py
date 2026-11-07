"""Dependency review, draft activation, and player order."""
import shutil
import subprocess
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
import generate


class ModManagerTests(unittest.TestCase):
    def test_review_and_save(self):
        gen = ROOT / 'build/gen'
        generate.write(gen)
        binary = ROOT / 'build' / ('mod_manager_test.exe' if sys.platform == 'win32' else 'mod_manager_test')
        command = [shutil.which('clang++'), '-std=c++17', '-Wall', '-Wextra', '-Werror',
                   '-I', str(gen / 'include'), '-I', str(ROOT / 'framework/core'),
                   '-I', str(ROOT / 'framework/base'), str(ROOT / 'framework/core/registry.cpp'),
                   str(ROOT / 'framework/core/text.cpp'), str(ROOT / 'framework/base/mod_choices.cpp'),
                   str(ROOT / 'framework/base/mod_manager.cpp'), str(ROOT / 'tests/base/mod_manager_test.cpp'),
                   '-o', str(binary)]
        compiled = subprocess.run(command, capture_output=True, text=True, encoding='utf-8')
        self.assertEqual(compiled.returncode, 0, compiled.stdout + compiled.stderr)
        ran = subprocess.run([str(binary)], capture_output=True, text=True, encoding='utf-8')
        self.assertEqual(ran.returncode, 0, ran.stdout + ran.stderr)


if __name__ == '__main__':
    unittest.main()
