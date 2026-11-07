"""Temporary scene ownership, generations and baseline preparation on a native stand-in."""
import shutil
import sys
import unittest

import test_all as checks
from test_all import ROOT, BUILD, GEN, BASE, CORE, GAME, CXX, run


class PreviewScenes(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        checks.Generated.setUpClass()

    def test_generations_baseline_order_and_reentrant_owner_stop(self):
        binary = BUILD / ('preview_scenes_test.exe' if sys.platform == 'win32' else 'preview_scenes_test')
        dirs = [GEN/'include', GEN/'framework', BASE, CORE, GAME]
        run([shutil.which('clang++'), *CXX,
             *[item for directory in dirs for item in ['-I', directory]],
             CORE/'registry.cpp', CORE/'text.cpp', GAME/'preview_scenes.cpp',
             GEN/'framework/game_service.cpp', ROOT/'tests/game/preview_scenes_test.cpp', '-o', binary])
        for case in ['policy', 'nested', 'stopped_retained', 'stopped_clean', 'failed_retained', 'failed_clean',
                     'stopped_first_member', 'stopped_second_member', 'stopped_reserve', 'nested_reserve',
                     'stopped_prepare', 'nested_prepare']:
            with self.subTest(case=case):
                self.assertIn('Preview scene controller checks passed', run([binary, case]).stdout)


if __name__ == '__main__':
    unittest.main()
