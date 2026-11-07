"""Ground-contact observer against a made-up native contact pass, without game physics."""
import shutil
import subprocess
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
import generate


class GroundContact(unittest.TestCase):
    def test_live_fixture_waits_for_settled_support(self):
        """A supporting contact can precede the body's settled vertical speed."""
        harness = (ROOT / 'tests/lua/ground_fixture.lua').read_text(encoding='utf-8')
        fixture = (ROOT / 'tests/fixtures/mods/check.ground/main.lua').read_text(encoding='utf-8')
        script = ROOT / 'build/ground-fixture-test.lua'
        script.parent.mkdir(exist_ok=True)
        script.write_text(harness.replace('-- FIXTURE HERE', fixture), encoding='utf-8')
        runner = ROOT / 'build' / ('lua_host.exe' if sys.platform == 'win32' else 'lua_host')
        ran = subprocess.run([str(runner), str(script)], capture_output=True, text=True)
        self.assertEqual(ran.returncode, 0, ran.stdout + ran.stderr)
        self.assertIn('ground fixture waits for a settled supported sample', ran.stdout)

    @unittest.skipUnless(shutil.which('clang++'), 'needs clang++')
    def test_completed_contact_samples_and_refusals(self):
        out = ROOT / 'build' / 'ground-contact-tests'
        out.mkdir(parents=True, exist_ok=True)
        gen = out / 'gen'
        generate.write(gen)
        program = out / ('ground_contact_test.exe' if sys.platform == 'win32' else 'ground_contact_test')
        command = [shutil.which('clang++'), '-std=c++17', '-Wall', '-Wextra', '-Werror',
                   '-fno-exceptions', '-fno-rtti']
        for include in [gen / 'include', gen / 'framework', ROOT / 'framework' / 'base',
                        ROOT / 'framework' / 'core', ROOT / 'framework' / 'game']:
            command += ['-I', include]
        command += [ROOT / 'framework' / 'game' / 'ground_contact.cpp',
                    ROOT / 'tests' / 'game' / 'ground_contact_test.cpp', '-o', program]
        built = subprocess.run([str(piece) for piece in command], capture_output=True, text=True)
        self.assertEqual(built.returncode, 0, built.stdout + built.stderr)
        ran = subprocess.run([str(program)], capture_output=True, text=True)
        self.assertEqual(ran.returncode, 0, ran.stdout + ran.stderr)
        self.assertIn('ground-contact observer stand-in checks passed', ran.stdout)


if __name__ == '__main__':
    unittest.main()
