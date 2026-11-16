"""Owned prompt textures against the real registry/hold implementation."""
import shutil, subprocess, sys, unittest
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'tools'))
import generate
class IconTextures(unittest.TestCase):
    def test_resource_ownership_cache_and_restoration(self):
        gen=ROOT/'build/gen';generate.write(gen)
        out=ROOT/'build/ui_icons_test.exe'
        sources=['framework/core/registry.cpp','framework/core/ids.cpp','framework/game/holds.cpp','framework/game/ui_icons.cpp','tests/game/ui_icons_test.cpp']
        command=[shutil.which('clang++'),'-std=c++17','-Wall','-Wextra','-Werror']
        for folder in (gen/'include',gen/'framework',ROOT/'framework/core',ROOT/'framework/base',ROOT/'framework/game'):
            command+=['-I',str(folder)]
        command += [str(ROOT/p) for p in sources]+[str(gen/'framework/ui_service.cpp'),'-o',str(out)]
        result=subprocess.run(command,capture_output=True,text=True)
        self.assertEqual(result.returncode,0,result.stdout+result.stderr)
        result=subprocess.run([str(out)],capture_output=True,text=True)
        self.assertEqual(result.returncode,0,result.stdout+result.stderr)
if __name__=='__main__':unittest.main()
