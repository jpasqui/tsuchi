"""Presenter policy ownership, validation, and automatic mod-stop release."""
import shutil
import subprocess
import sys
import unittest
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'tools'))
import generate

class PresentationPolicy(unittest.TestCase):
    def test_owned_policy(self):
        gen=ROOT/'build/gen'
        generate.write(gen)
        binary=ROOT/'build/presentation_policy_test.exe'
        command=[shutil.which('clang++'),'-std=c++17','-Wall','-Wextra','-Werror']
        for path in ('framework/core','framework/game','build/gen/framework','build/gen/include'):
            command += ['-I',str(ROOT/path)]
        command += [str(ROOT/path) for path in ('framework/core/registry.cpp',
                    'framework/game/presentation_backend.cpp','tests/game/presentation_policy_test.cpp',
                    'build/gen/framework/time_service.cpp')]
        command += ['-o',str(binary)]
        built=subprocess.run(command,capture_output=True,text=True)
        self.assertEqual(built.returncode,0,built.stdout+built.stderr)
        ran=subprocess.run([str(binary)],capture_output=True,text=True)
        self.assertEqual(ran.returncode,0,ran.stdout+ran.stderr)

if __name__=='__main__': unittest.main()
