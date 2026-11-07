"""Camera and input against a made-up game, including the real hold tables and generated validation."""
import shutil
import subprocess
import sys
import unittest
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'tools'))
class CameraInput(unittest.TestCase):
    @unittest.skipUnless(shutil.which('clang++'),'needs clang++')
    def test_calls_holds_restore_and_controller_chains(self):
        import generate
        generate.write(ROOT/'build/gen')
        exe=ROOT/'build'/('camera_input_test.exe' if sys.platform=='win32' else 'camera_input_test')
        from gen import game_table
        fixture=ROOT/'build/gen-fixture'
        for relative, text in game_table.generate(ROOT/'tests/fixtures/game_table.yaml', 'tests/fixtures/game_table.yaml').items():
            path=fixture/relative
            path.parent.mkdir(parents=True,exist_ok=True)
            if not path.exists() or path.read_text(encoding='utf-8') != text:
                path.write_text(text,encoding='utf-8')
        self.run_stand_in(exe,None)
        self.run_stand_in(exe,fixture/'framework')
    def run_stand_in(self,exe,table):
        command=[shutil.which('clang++'),'-std=c++17','-Wall','-Wextra','-Werror']
        if table: command+=['-I',str(table)]
        for inc in ['framework/base','framework/core','framework/game','build/gen/framework','build/gen/include']:
            command+=['-I',str(ROOT/inc)]
        command += [str(ROOT/p) for p in ['tests/game/camera_input_test.cpp','framework/core/registry.cpp','framework/game/holds.cpp','framework/game/camera_backend.cpp','framework/game/input_backend.cpp','build/gen/framework/camera_service.cpp','build/gen/framework/input_service.cpp']]
        compiled=subprocess.run(command+['-o',str(exe)],capture_output=True,text=True)
        self.assertEqual(compiled.returncode,0,compiled.stdout+compiled.stderr)
        ran=subprocess.run([str(exe)],capture_output=True,text=True)
        self.assertEqual(ran.returncode,0,ran.stdout+ran.stderr)
if __name__=='__main__': unittest.main()
