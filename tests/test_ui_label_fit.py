"""Render-only fitting against measured stock geometry and transform invariants."""
import shutil
import subprocess
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


class UiLabelFitTests(unittest.TestCase):
    @unittest.skipUnless(shutil.which('clang++'), 'needs clang++')
    def test_edges_stability_and_unsupported_geometry(self):
        output = ROOT / 'build' / ('ui_label_fit_test.exe' if sys.platform == 'win32' else 'ui_label_fit_test')
        output.parent.mkdir(parents=True, exist_ok=True)
        command = [shutil.which('clang++'), '-std=c++17', '-Wall', '-Wextra', '-Werror',
                   '-I', str(ROOT / 'framework' / 'game'),
                   str(ROOT / 'tests' / 'game' / 'ui_label_fit_test.cpp'), '-o', str(output)]
        compiled = subprocess.run(command, capture_output=True, text=True, encoding='utf-8', timeout=30)
        self.assertEqual(compiled.returncode, 0, compiled.stdout + compiled.stderr)
        ran = subprocess.run([str(output)], capture_output=True, text=True, encoding='utf-8', timeout=10)
        self.assertEqual(ran.returncode, 0, ran.stdout + ran.stderr)


if __name__ == '__main__':
    unittest.main()
