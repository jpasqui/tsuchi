"""Console-variable ownership, priority, replacement and mod-stop behavior."""
import shutil
import subprocess
import sys
import unittest
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'tools'))
import generate
from gen import game_table

class ConsoleVariables(unittest.TestCase):
    def test_numeric_holds_restore_and_revalidate(self):
        gen=ROOT/'build/gen';generate.write(gen)
        fixture=ROOT/'build/console-fixture'
        for relative,source in game_table.generate(ROOT/'tests/fixtures/game_table.yaml').items():
            path=fixture/relative;path.parent.mkdir(parents=True,exist_ok=True)
            path.write_text(source,encoding='utf-8')
        binary=ROOT/'build/console_variables_test.exe'
        command=[shutil.which('clang++'),'-std=c++17','-Wall','-Wextra','-Werror']
        for path in (gen/'include',fixture/'framework',gen/'framework',ROOT/'framework/core',ROOT/'framework/base',ROOT/'framework/game',ROOT/'tests/base'):
            command+=['-I',str(path)]
        command += [str(ROOT/path) for path in ('framework/core/registry.cpp','framework/game/holds.cpp',
                    'framework/game/console_variables.cpp','tests/game/console_variables_test.cpp')]
        command+=['-o',str(binary)]
        built=subprocess.run(command,capture_output=True,text=True)
        self.assertEqual(built.returncode,0,built.stdout+built.stderr)
        ran=subprocess.run([str(binary)],capture_output=True,text=True)
        self.assertEqual(ran.returncode,0,ran.stdout+ran.stderr)

if __name__=='__main__':unittest.main()
