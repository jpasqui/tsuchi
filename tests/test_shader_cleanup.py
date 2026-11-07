"""Production shader cleanup/refresh with controlled native reentry boundaries."""
import shutil,sys,unittest
from pathlib import Path
import test_all as checks
from test_all import GEN,ROOT,BUILD,CXX,BASE,CORE,GAME,run
class ShaderCleanup(unittest.TestCase):
    @classmethod
    def setUpClass(cls): checks.Generated.setUpClass()
    def test_cleanup_setter_stop_and_refresh_provider_swap(self):
        fixture=checks.Generated().fixture()
        binary=BUILD/('shader_cleanup_test.exe' if sys.platform=='win32' else 'shader_cleanup_test')
        command=[shutil.which('clang++'),*CXX,'-ffunction-sections','-fdata-sections',
                 *[item for d in [GEN/'include',fixture/'framework',GEN/'framework',BASE,CORE,GAME,ROOT/'tests/base'] for item in ['-I',d]],
                 *[BASE/n for n in ['names.cpp','objects.cpp','reflect.cpp','engine_call.cpp']],
                 *[CORE/n for n in ['registry.cpp','text.cpp','texts.cpp','json.cpp']],GAME/'parts.cpp',GAME/'object_retention.cpp',
                 ROOT/'tests/game/shader_cleanup_test.cpp','-o',binary]
        command+=['-Xlinker','/OPT:REF'] if sys.platform=='win32' else ['-Wl,--gc-sections']
        run(command);run([binary])
if __name__=='__main__':unittest.main()
