"""Private native-layout retirement: safe pending ownership and reversible lookup publication."""
import shutil
import subprocess
import sys
import unittest
from pathlib import Path
ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
import generate

class ContainerLiveTests(unittest.TestCase):
    def test_native_lookup_retirement(self):
        gen = ROOT / 'build/gen'
        generate.write(gen)
        binary = ROOT / 'build' / ('container_live_test.exe' if sys.platform == 'win32' else 'container_live_test')
        cmd = [shutil.which('clang++'), '-std=c++17', '-Wall', '-Wextra', '-Werror']
        for folder in (gen/'include', gen/'framework', ROOT/'framework/base'):
            cmd += ['-I', str(folder)]
        cmd += [str(ROOT/'framework/base/container_live.cpp'), str(ROOT/'tests/base/container_live_test.cpp'), '-o', str(binary)]
        ran = subprocess.run(cmd, capture_output=True, text=True, encoding='utf-8')
        self.assertEqual(ran.returncode, 0, ran.stdout+ran.stderr)
        ran = subprocess.run([str(binary)], capture_output=True, text=True, encoding='utf-8')
        self.assertEqual(ran.returncode, 0, ran.stdout+ran.stderr)

if __name__ == '__main__':
    unittest.main()
