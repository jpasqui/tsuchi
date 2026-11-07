"""Footer ownership, localized guides and proven arrow animation dispatch."""
import shutil
import subprocess
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
import generate
from gen import game_table


class UiFeedbackTests(unittest.TestCase):
    @unittest.skipUnless(shutil.which('clang++'), 'needs clang++')
    def test_scoped_guide_and_arrow_feedback(self):
        gen = ROOT / 'build/gen'
        generate.write(gen)
        work = ROOT / 'build/ui-feedback-test'
        work.mkdir(parents=True, exist_ok=True)
        for relative, source in game_table.generate(ROOT / 'tests/fixtures/game_table.yaml', 'tests/fixtures/game_table.yaml').items():
            path = work / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            if not path.exists() or path.read_text(encoding='utf-8') != source:
                path.write_text(source, encoding='utf-8', newline='\n')
        binary = work / ('ui_feedback_test.exe' if sys.platform == 'win32' else 'ui_feedback_test')
        command = [shutil.which('clang++'), '-std=c++17', '-Wall', '-Wextra', '-Werror']
        for folder in [gen / 'include', work / 'framework', gen / 'framework', ROOT / 'framework/core',
                       ROOT / 'framework/base', ROOT / 'framework/game', ROOT / 'tests/base']:
            command += ['-I', str(folder)]
        sources = ['framework/core/registry.cpp', 'framework/core/text.cpp', 'framework/base/names.cpp',
                   'framework/base/objects.cpp', 'framework/game/ui_feedback.cpp', 'tests/game/ui_feedback_test.cpp']
        command += [str(ROOT / source) for source in sources] + ['-o', str(binary)]
        compiled = subprocess.run(command, capture_output=True, text=True)
        self.assertEqual(compiled.returncode, 0, compiled.stdout + compiled.stderr)
        ran = subprocess.run([str(binary)], capture_output=True, text=True)
        self.assertEqual(ran.returncode, 0, ran.stdout + ran.stderr)


if __name__ == '__main__':
    unittest.main()
