"""Native work-object and battle calls against the made-up game."""
import shutil
import subprocess
import sys
import unittest
from pathlib import Path
ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT/'tools'))
import generate

class GimmicksBattle(unittest.TestCase):
    def test_calls_events_and_outcome_restoration(self):
        gen=ROOT/'build/gen'
        generate.write(gen)
        binary=ROOT/'build'/('gimmicks_battle_test.exe' if sys.platform=='win32' else 'gimmicks_battle_test')
        dirs=[gen/'include',gen/'framework',ROOT/'framework/base',ROOT/'framework/core',ROOT/'framework/game']
        sources=[ROOT/'framework/core/registry.cpp',ROOT/'framework/core/ids.cpp',ROOT/'framework/game/holds.cpp',
                 ROOT/'framework/game/gimmicks_backend.cpp',ROOT/'framework/game/battle_backend.cpp',
                 gen/'framework/gimmicks_service.cpp',gen/'framework/battle_service.cpp',ROOT/'tests/game/gimmicks_battle_test.cpp']
        compiled=subprocess.run([shutil.which('clang++'),'-std=c++17','-Wall','-Wextra','-Werror',
            *[part for directory in dirs for part in ('-I',str(directory))],*map(str,sources),'-o',str(binary)],capture_output=True,text=True)
        self.assertEqual(compiled.returncode,0,compiled.stdout+compiled.stderr)
        ran=subprocess.run([str(binary)],capture_output=True,text=True)
        self.assertEqual(ran.returncode,0,ran.stdout+ran.stderr)

if __name__=='__main__': unittest.main()
