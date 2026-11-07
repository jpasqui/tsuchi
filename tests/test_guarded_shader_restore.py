"""Focused actual shader restoration resolver against controlled engine callbacks."""
import shutil
import subprocess
import sys
import unittest
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
class GuardedShaderRestore(unittest.TestCase):
    def test_creation_copy_provider_removal_and_relocation(self):
        gen=ROOT/'build'/'gen'
        fixture=ROOT/'build'/'gen-fixture'
        binary=ROOT/'build'/('guarded_shader_restore_test.exe' if sys.platform=='win32' else 'guarded_shader_restore_test')
        dirs=[gen/'include',fixture/'framework',gen/'framework',ROOT/'framework'/'base',ROOT/'framework'/'core',ROOT/'framework'/'game',ROOT/'tests'/'base']
        sources=[*[ROOT/'framework'/'base'/name for name in ['names.cpp','objects.cpp','reflect.cpp','engine_call.cpp']],*[ROOT/'framework'/'core'/name for name in ['registry.cpp','text.cpp','texts.cpp','json.cpp']],ROOT/'framework'/'game'/'parts.cpp',ROOT/'framework'/'game'/'object_retention.cpp',ROOT/'tests'/'game'/'guarded_shader_restore_test.cpp']
        command=[shutil.which('clang++'),'-std=c++17','-Wall','-Wextra','-Werror','-ffunction-sections','-fdata-sections',*[item for d in dirs for item in ['-I',str(d)]],*map(str,sources),'-o',str(binary)]
        command+=['-Xlinker','/OPT:REF'] if sys.platform=='win32' else ['-Wl,--gc-sections']
        compiled=subprocess.run(command,capture_output=True,text=True)
        self.assertEqual(compiled.returncode,0,compiled.stdout+compiled.stderr)
        ran=subprocess.run([str(binary)],capture_output=True,text=True)
        self.assertEqual(ran.returncode,0,ran.stdout+ran.stderr)
if __name__=='__main__': unittest.main()
