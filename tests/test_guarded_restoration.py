"""Focused guarded reflection restoration preparation and native boundaries."""
import shutil
import subprocess
import sys
import unittest
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'tools'))
from gen import game_table
class GuardedRestoration(unittest.TestCase):
    def test_lookup_allocation_initialization_write_and_call_guards(self):
        fixture=ROOT/'build'/'gen-fixture'
        for relative,text in game_table.generate(ROOT/'tests'/'fixtures'/'game_table.yaml','tests/fixtures/game_table.yaml').items():
            path=fixture/relative;path.parent.mkdir(parents=True,exist_ok=True)
            if not path.exists() or path.read_text(encoding='utf-8')!=text:path.write_text(text,encoding='utf-8')
        gen=ROOT/'build'/'gen'
        binary=ROOT/'build'/('guarded_restoration_test.exe' if sys.platform=='win32' else 'guarded_restoration_test')
        dirs=[gen/'include',fixture/'framework',ROOT/'framework'/'base',ROOT/'framework'/'core',ROOT/'tests'/'base']
        sources=[*[ROOT/'framework'/'base'/name for name in ['names.cpp','objects.cpp','reflect.cpp','engine_call.cpp']],ROOT/'framework'/'core'/'registry.cpp',ROOT/'tests'/'base'/'guarded_restoration_test.cpp']
        command=[shutil.which('clang++'),'-std=c++17','-Wall','-Wextra','-Werror',*[item for d in dirs for item in ['-I',str(d)]],*map(str,sources),'-o',str(binary)]
        compiled=subprocess.run(command,capture_output=True,text=True)
        self.assertEqual(compiled.returncode,0,compiled.stdout+compiled.stderr)
        ran=subprocess.run([str(binary)],capture_output=True,text=True)
        self.assertEqual(ran.returncode,0,ran.stdout+ran.stderr)
if __name__=='__main__':unittest.main()
