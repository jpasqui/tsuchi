"""The shared UI backend against a made-up widget host and the real hold tables."""
import shutil
import subprocess
import sys
import unittest
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'tools'))
import generate
class UiScreensTests(unittest.TestCase):
    def test_code_data_controls_and_restore_paths(self):
        gen=ROOT/'build/gen';generate.write(gen)
        binary=ROOT/'build'/('ui_screens_test.exe' if sys.platform=='win32' else 'ui_screens_test')
        sources=['framework/core/registry.cpp','framework/core/ids.cpp','framework/core/text.cpp','framework/core/texts.cpp','framework/core/json.cpp','framework/game/holds.cpp','framework/game/ui_screens.cpp','tests/game/ui_screens_test.cpp']
        command=[shutil.which('clang++'),'-std=c++17','-Wall','-Wextra','-Werror']
        for folder in [gen/'include',gen/'framework',ROOT/'framework/core',ROOT/'framework/base',ROOT/'framework/game']:
            command+=['-I',str(folder)]
        command += [str(ROOT/source) for source in sources]+[str(gen/'framework/ui_service.cpp'),'-o',str(binary)]
        compiled=subprocess.run(command,capture_output=True,text=True)
        self.assertEqual(compiled.returncode,0,compiled.stdout+compiled.stderr)
        ran=subprocess.run([str(binary)],capture_output=True,text=True)
        self.assertEqual(ran.returncode,0,ran.stdout+ran.stderr)
if __name__=='__main__':unittest.main()
