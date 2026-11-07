"""Runtime bundle upload and ownership against an SD filesystem stand-in."""
import shutil
import sys
import unittest

import test_all as checks
from test_all import ROOT, BUILD, GEN, BASE, CORE, GAME, CXX, run


class AssetBundles(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        checks.Generated.setUpClass()

    def test_upload_limits_retries_mount_and_owner_cleanup(self):
        fixture = checks.Generated().fixture()
        binary = BUILD / ('asset_bundles_test.exe' if sys.platform == 'win32' else 'asset_bundles_test')
        dirs = [GEN/'include', fixture/'framework', GEN/'framework', BASE, CORE, GAME]
        run([shutil.which('clang++'), *CXX,
             *[item for directory in dirs for item in ['-I', directory]],
             CORE/'registry.cpp', CORE/'json.cpp', CORE/'texts.cpp', BASE/'files.cpp',
             GEN/'framework/asset_service.cpp',
             ROOT/'tests/game/asset_bundles_test.cpp', '-o', binary])
        for case in ['policy', 'spool_sweep', 'sd_denied', 'write_denied', 'mount_failure', 'reentrant_mount',
                     'reentrant_shader', 'recursive_shader', 'recursive_mount', 'retained_limit',
                     'disk_limit', 'cleanup_failure', 'rollback_failed', 'fresh_cleanup_failed']:
            with self.subTest(case=case):
                self.assertIn('Asset bundle checks passed', run([binary, case]).stdout)


if __name__ == '__main__':
    unittest.main()
