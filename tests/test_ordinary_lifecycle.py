"""Focused private ordinary-actor teardown admission and restoration checks."""
import shutil
import subprocess
import sys
import unittest
from pathlib import Path
ROOT = Path(__file__).resolve().parents[1]
class OrdinaryLifecycle(unittest.TestCase):
    def test_admission_restoration_and_reentry(self):
        gen = ROOT / 'build' / 'gen'
        binary = ROOT / 'build' / ('ordinary_lifecycle_test.exe' if sys.platform == 'win32' else 'ordinary_lifecycle_test')
        dirs = [gen/'include',gen/'framework',ROOT/'framework'/'base',ROOT/'framework'/'core',ROOT/'framework'/'game']
        sources = [ROOT/'framework'/'core'/'registry.cpp', *[ROOT/'framework'/'game'/name for name in ['holds.cpp','hidden.cpp','materials.cpp','clips.cpp','added.cpp','object_retention.cpp']], ROOT/'tests'/'game'/'ordinary_lifecycle_test.cpp']
        command = [shutil.which('clang++'),'-std=c++17','-Wall','-Wextra','-Werror', *[item for d in dirs for item in ['-I',str(d)]], *map(str,sources),'-o',str(binary)]
        compiled = subprocess.run(command,capture_output=True,text=True)
        self.assertEqual(compiled.returncode,0,compiled.stdout+compiled.stderr)
        ran = subprocess.run([str(binary)],capture_output=True,text=True)
        self.assertEqual(ran.returncode,0,ran.stdout+ran.stderr)
if __name__ == '__main__': unittest.main()
