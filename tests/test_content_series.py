"""Series bonus cache holds against mapped memory and a reentrant allocator."""
import shutil
import subprocess
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


class ContentSeries(unittest.TestCase):
    def test_shared_holds_and_native_cache(self):
        build = ROOT / 'build'
        build.mkdir(exist_ok=True)
        binary = build / ('content_series_test.exe' if sys.platform == 'win32' else 'content_series_test')
        dirs = [build / 'gen/include', build / 'gen/framework', ROOT / 'framework/base', ROOT / 'framework/core', ROOT / 'framework/game']
        # The test includes the production translation unit to reach its central
        # hold table for explicit release. No replacement implementation runs.
        sources = [ROOT / 'framework/core/registry.cpp', ROOT / 'framework/game/holds.cpp', ROOT / 'tests/game/content_series_test.cpp']
        compiled = subprocess.run([shutil.which('clang++'), '-std=c++17', '-Wall', '-Wextra', '-Werror', *[a for d in dirs for a in ['-I', str(d)]], *map(str, sources), '-o', str(binary)], capture_output=True, text=True)
        self.assertEqual(compiled.returncode, 0, compiled.stdout + compiled.stderr)
        ran = subprocess.run([str(binary)], capture_output=True, text=True)
        self.assertEqual(ran.returncode, 0, ran.stdout + ran.stderr)


if __name__ == '__main__':
    unittest.main()
