"""Actual typed host hook trampoline ABI with lifecycle provenance enabled."""
import shutil
import subprocess
import sys
import unittest
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
class CallableHookABI(unittest.TestCase):
    def test_chained_float_and_integer_original_arguments(self):
        gen=ROOT/'build'/'gen';fixture=ROOT/'build'/'gen-fixture'
        binary=ROOT/'build'/('callable_hook_abi_test.exe' if sys.platform=='win32' else 'callable_hook_abi_test')
        dirs=[gen/'include',fixture/'framework',gen/'framework',ROOT/'framework'/'base',ROOT/'framework'/'core',ROOT/'tests'/'base']
        sources=[*[ROOT/'framework'/'core'/name for name in ['registry.cpp','ids.cpp','hooks.cpp']],*sorted((gen/'framework').glob('*.cpp')),ROOT/'tests'/'core'/'callable_hook_abi_test.cpp']
        command=[shutil.which('clang++'),'-std=c++17','-Wall','-Wextra','-Werror',*[item for d in dirs for item in ['-I',str(d)]],*map(str,sources),'-o',str(binary)]
        compiled=subprocess.run(command,capture_output=True,text=True)
        self.assertEqual(compiled.returncode,0,compiled.stdout+compiled.stderr)
        ran=subprocess.run([str(binary)],capture_output=True,text=True)
        self.assertEqual(ran.returncode,0,ran.stdout+ran.stderr)
if __name__=='__main__': unittest.main()
