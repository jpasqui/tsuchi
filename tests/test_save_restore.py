"""save.restore on a native stand-in: copy into the current save, apply it, keep the game's objects."""
import shutil
import sys
import unittest

import test_all as checks
from test_all import ROOT, BUILD, GEN, BASE, CORE, GAME, CXX, run


class SaveRestore(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        checks.Generated.setUpClass()

    def test_copy_into_the_current_save_then_apply(self):
        fixture = checks.Generated().fixture()
        binary = BUILD / ('save_restore_test.exe' if sys.platform == 'win32' else 'save_restore_test')
        dirs = [GEN/'include', fixture/'framework', GEN/'framework', BASE, CORE, GAME, ROOT/'tests/base']
        command = [shutil.which('clang++'), *CXX, '-ffunction-sections', '-fdata-sections',
                   *[item for directory in dirs for item in ['-I', directory]],
                   *[BASE/name for name in ['names.cpp', 'objects.cpp']],
                   *[CORE/name for name in ['registry.cpp', 'ids.cpp', 'text.cpp', 'texts.cpp']],
                   GAME/'flow.cpp', ROOT/'tests/game/save_restore_test.cpp', '-o', binary]
        command += ['-Xlinker', '/OPT:REF'] if sys.platform == 'win32' else ['-Wl,--gc-sections']
        run(command)
        for mode in ['copy', 'refusals']:
            with self.subTest(mode=mode):
                self.assertIn('Save restore checks passed', run([binary, mode]).stdout)


if __name__ == '__main__':
    unittest.main()
