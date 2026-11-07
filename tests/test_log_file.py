"""SD logging and savedata fallback against a filesystem stand-in, not a console run."""
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))


class LogFile(unittest.TestCase):
    @unittest.skipUnless(shutil.which('clang++'), 'needs clang++')
    def test_sd_context_destination_and_fallback(self):
        import generate
        generate.write(ROOT / 'build/gen')
        with tempfile.TemporaryDirectory() as folder:
            exe = Path(folder) / ('logging.exe' if sys.platform == 'win32' else 'logging')
            command = [shutil.which('clang++'), '-std=c++17', '-Wall', '-Wextra', '-Werror']
            for include in ['framework/base', 'framework/core', 'framework/game',
                            'build/gen/framework', 'build/gen/include']:
                command += ['-I', str(ROOT / include)]
            command += [str(ROOT / 'tests/game/log_file_test.cpp'),
                        str(ROOT / 'framework/game/log_file.cpp'), str(ROOT / 'framework/game/sd_card.cpp'), '-o', str(exe)]
            built = subprocess.run(command, capture_output=True, text=True)
            self.assertEqual(built.returncode, 0, built.stdout + built.stderr)
            for case in ['sd', 'no_sdk', 'no_context', 'mount_denied', 'write_denied', 'later_refused']:
                with self.subTest(case=case):
                    ran = subprocess.run([str(exe), case], capture_output=True, text=True)
                    self.assertEqual(ran.returncode, 0, ran.stdout + ran.stderr)


if __name__ == '__main__':
    unittest.main()
