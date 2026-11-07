"""Native preview area helpers against reflected host objects and callbacks."""
import shutil
import sys
import unittest

import test_all as checks
from test_all import ROOT, BUILD, GEN, BASE, CORE, GAME, CXX, run


class PreviewArea(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        checks.Generated.setUpClass()

    def test_native_reservation_admission_generations_and_prepare(self):
        fixture = checks.Generated().fixture()
        binary = BUILD / ('preview_area_test.exe' if sys.platform == 'win32' else 'preview_area_test')
        dirs = [GEN/'include', fixture/'framework', GEN/'framework', BASE, CORE, GAME, ROOT/'tests/base']
        command = [shutil.which('clang++'), *CXX, '-ffunction-sections', '-fdata-sections',
                   *[item for directory in dirs for item in ['-I', directory]],
                   *[BASE/name for name in ['names.cpp', 'objects.cpp', 'reflect.cpp', 'engine_call.cpp']],
                   *[CORE/name for name in ['registry.cpp', 'ids.cpp', 'text.cpp', 'texts.cpp', 'json.cpp']],
                   GAME/'flow.cpp', GAME/'records.cpp', ROOT/'tests/game/preview_area_test.cpp', '-o', binary]
        command += ['-Xlinker', '/OPT:REF'] if sys.platform == 'win32' else ['-Wl,--gc-sections']
        run(command)
        for mode in ['reservation', 'admission', 'generations', 'callbacks', 'validation']:
            with self.subTest(mode=mode):
                self.assertIn('Preview area checks passed', run([binary, mode]).stdout)


if __name__ == '__main__':
    unittest.main()
