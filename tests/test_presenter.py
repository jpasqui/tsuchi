"""The presenter against a made-up game: the clock, snapping, the camera's resets, the view's clocks, the widgets
that count frames and the controllers' presses. A result against host stand-ins, not one in the game."""
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))


class Presenter(unittest.TestCase):
    @unittest.skipUnless(shutil.which('clang++'), 'needs clang++')
    def test_steps_pictures_and_what_is_kept_right_between(self):
        import generate
        generate.write(ROOT / 'build/gen')
        with tempfile.TemporaryDirectory() as folder:
            exe = Path(folder) / ('presenter.exe' if sys.platform == 'win32' else 'presenter')
            command = [shutil.which('clang++'), '-std=c++17', '-Wall', '-Wextra', '-Werror']
            for include in ['framework/base', 'framework/core', 'framework/game', 'build/gen/framework', 'build/gen/include']:
                command += ['-I', str(ROOT / include)]
            command += [str(ROOT / 'tests/game/presenter_test.cpp'), str(ROOT / 'framework/game/presenter.cpp'),
                        str(ROOT / 'framework/game/presenter_scene.cpp'), str(ROOT / 'framework/game/presenter_effects.cpp'),
                        str(ROOT / 'framework/game/presenter_markers.cpp'),
                        str(ROOT / 'framework/core/native_routes.cpp'),
                        str(ROOT / 'framework/core/json.cpp'), '-o', str(exe)]
            compiled = subprocess.run(command, capture_output=True, text=True)
            self.assertEqual(compiled.returncode, 0, compiled.stdout + compiled.stderr)
            for initial_vsync in ('1', '0', '-1', '2', 'read-refused', 'counter-unhooked'):
                with self.subTest(initial_vsync=initial_vsync):
                    ran = subprocess.run([str(exe), initial_vsync], capture_output=True, text=True)
                    self.assertEqual(ran.returncode, 0, ran.stdout + ran.stderr)
                    if initial_vsync in ('1', '0', '-1', '2'):
                        self.assertIn('loop and view clock conservation passed', ran.stdout)


if __name__ == '__main__':
    unittest.main()
