"""Reentrant mod-stop acquisition runs the real hold/material/hidden code."""
import shutil
import subprocess
import sys
import unittest
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
class HoldAcquisition(unittest.TestCase):
    def test_native_callback_cancels_acquisition_without_stale_publish(self):
        gen=ROOT/'build/gen'
        binary=ROOT/'build'/('hold_acquisition_test.exe' if sys.platform=='win32' else 'hold_acquisition_test')
        dirs=[gen/'include',gen/'framework',ROOT/'framework/base',ROOT/'framework/core',ROOT/'framework/game']
        sources=[ROOT/'framework/core/registry.cpp',*[ROOT/'framework/game'/name for name in ['holds.cpp','hidden.cpp','materials.cpp','object_retention.cpp']],ROOT/'tests/game/hold_acquisition_test.cpp']
        compile=subprocess.run([shutil.which('clang++'),'-std=c++17','-Wall','-Wextra','-Werror',*[a for d in dirs for a in ['-I',str(d)]],*map(str,sources),'-o',str(binary)],capture_output=True,text=True)
        self.assertEqual(compile.returncode,0,compile.stdout+compile.stderr)
        ran=subprocess.run([str(binary)],capture_output=True,text=True)
        self.assertEqual(ran.returncode,0,ran.stdout+ran.stderr)
if __name__=='__main__':unittest.main()
