"""Creator UE shader archives and embedded material metadata on the host."""
import shutil
import sys
import unittest
from pathlib import Path
import test_all as checks
from test_all import ROOT, BUILD, GEN, CORE, GAME, CXX, run

class ShaderArchive(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        checks.Generated.setUpClass()

    def test_creator_archive_and_embedded_material_metadata(self):
        binary = BUILD / ('shader_archive_test.exe' if sys.platform == 'win32' else 'shader_archive_test')
        run([shutil.which('clang++'), *CXX, '-I', GEN/'include', '-I', GEN/'framework', '-I', CORE, '-I', GAME,
             CORE/'json.cpp', ROOT/'tests/game/shader_archive_test.cpp', '-o', binary])
        result=run([binary])
        self.assertIn('creator archive checks passed',result.stdout)

if __name__=='__main__':
    unittest.main()
