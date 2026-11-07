"""Runtime shader admission uses the production manifest transaction on the host."""
import shutil
import sys
import unittest

import test_all as checks
from test_all import ROOT, BUILD, GEN, BASE, CORE, GAME, CXX, run


class ShaderAdmission(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        checks.Generated.setUpClass()

    def test_runtime_validation_ownership_and_reentrant_release(self):
        fixture = checks.Generated().fixture()
        binary = BUILD / ('shader_admission_test.exe' if sys.platform == 'win32' else 'shader_admission_test')
        dirs = [GEN/'include', fixture/'framework', GEN/'framework', BASE, CORE, GAME, ROOT/'tests/base']
        command = [shutil.which('clang++'), *CXX, '-ffunction-sections', '-fdata-sections',
                   *[item for directory in dirs for item in ['-I', directory]],
                   *[BASE/name for name in ['names.cpp', 'objects.cpp', 'reflect.cpp', 'engine_call.cpp']],
                   *[CORE/name for name in ['registry.cpp', 'text.cpp', 'texts.cpp', 'json.cpp']],
                   GAME/'parts.cpp', GAME/'object_retention.cpp',
                   ROOT/'tests/game/shader_admission_test.cpp', '-o', binary]
        command += ['-Xlinker', '/OPT:REF'] if sys.platform == 'win32' else ['-Wl,--gc-sections']
        run(command)
        self.assertIn('Shader admission transaction checks passed', run([binary]).stdout)


if __name__ == '__main__':
    unittest.main()
