"""External catalog observations, queued companion requests, and strict limits."""
import shutil
import subprocess
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
import generate


class ExternalModsTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.output = ROOT / 'build/external-mods'
        gen = cls.output / 'gen'
        generate.write(gen)
        cls.binary = cls.output / ('external_mods_test.exe' if sys.platform == 'win32' else 'external_mods_test')
        command = [shutil.which('clang++'), '-std=c++17', '-Wall', '-Wextra', '-Werror',
                   '-I', str(gen / 'include'), '-I', str(ROOT / 'framework/core'),
                   '-I', str(ROOT / 'framework/base'),
                   str(ROOT / 'framework/core/registry.cpp'), str(ROOT / 'framework/core/text.cpp'),
                   str(ROOT / 'framework/core/json.cpp'), str(ROOT / 'framework/base/external_mods.cpp'),
                   str(ROOT / 'tests/base/external_mods_test.cpp'), '-o', str(cls.binary)]
        compiled = subprocess.run(command, capture_output=True, text=True)
        if compiled.returncode:
            raise AssertionError(compiled.stdout + compiled.stderr)

    def run_case(self, mode):
        ran = subprocess.run([str(self.binary), mode], capture_output=True, text=True)
        self.assertEqual(ran.returncode, 0, ran.stdout + ran.stderr)

    def test_observed_saved_and_draft_are_separate(self):
        self.run_case('observed')

    def test_catalog_schema_and_text_are_strict(self):
        self.run_case('catalog')

    def test_catalog_byte_text_and_record_limits(self):
        self.run_case('bounds')

    def test_bad_and_stale_requests_use_observed_defaults(self):
        self.run_case('requests')

    def test_unmanaged_cheats_conflicts_and_recovery_cannot_toggle(self):
        self.run_case('blocked')

    def test_failed_write_discard_and_saved_roundtrip(self):
        self.run_case('save')

    def test_allocation_failure_keeps_complete_snapshots(self):
        self.run_case('allocation')


if __name__ == '__main__':
    unittest.main()
