"""Core after-step timing and reentrant stop, with the real registry and time events."""
import shutil
import subprocess
import sys
import unittest
from pathlib import Path
ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
class FramePhases(unittest.TestCase):
    @unittest.skipUnless(shutil.which('clang++'), 'needs clang++')
    def test_after_step_order_and_refused_frames(self):
        import generate
        generate.write(ROOT / 'build/gen')
        exe = ROOT / 'build' / ('frame_phases.exe' if sys.platform == 'win32' else 'frame_phases')
        command = [shutil.which('clang++'), '-std=c++17', '-Wall', '-Wextra', '-Werror']
        for inc in ['framework/base', 'framework/core', 'build/gen/framework', 'build/gen/include']:
            command += ['-I', str(ROOT / inc)]
        command += [str(ROOT / p) for p in ['tests/core/frame_test.cpp', 'framework/core/registry.cpp', 'framework/core/frame.cpp', 'build/gen/framework/time_service.cpp']]
        compiled = subprocess.run(command + ['-o', str(exe)], capture_output=True, text=True)
        self.assertEqual(compiled.returncode, 0, compiled.stdout + compiled.stderr)
        ran = subprocess.run([str(exe)], capture_output=True, text=True)
        self.assertEqual(ran.returncode, 0, ran.stdout + ran.stderr)
if __name__ == '__main__': unittest.main()
