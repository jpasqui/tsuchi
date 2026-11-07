"""The explicit portal route reports actual arrivals, never an accepted request alone."""
import subprocess
import sys
import unittest
from pathlib import Path
ROOT = Path(__file__).resolve().parents[1]
class PortalRoute(unittest.TestCase):
    def test_real_destination_and_control_are_observed_after_each_request(self):
        runner = ROOT / 'build' / ('lua_host.exe' if sys.platform == 'win32' else 'lua_host')
        result = subprocess.run([runner, ROOT / 'tests/routes/portal_test.lua'], cwd=ROOT,
                                capture_output=True, text=True, check=True)
        self.assertIn('real portal selection and asynchronous departure observations pass', result.stdout)
