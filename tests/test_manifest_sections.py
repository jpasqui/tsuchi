"""Selected manifest preparation, startup adoption and rollback against made-up mod files."""
import shutil
import subprocess
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
import generate


class ManifestSections(unittest.TestCase):
    def test_selected_manifest_phases_adopt_and_rollback_owned_contexts(self):
        gen = ROOT / 'build/gen'
        generate.write(gen)
        binary = ROOT / 'build' / ('manifest_sections_test.exe' if sys.platform == 'win32' else 'manifest_sections_test')
        base, core = ROOT / 'framework/base', ROOT / 'framework/core'
        sources = [base / 'mods.cpp', base / 'mod_choices.cpp', base / 'settings.cpp',
                   *[core / (name + '.cpp') for name in ('registry', 'text', 'texts', 'json')],
                   ROOT / 'tests/base/manifest_sections_test.cpp']
        command = [shutil.which('clang++'), '-std=c++17', '-Wall', '-Wextra', '-Werror',
                   '-I', str(gen / 'include'), '-I', str(gen / 'framework'), '-I', str(core), '-I', str(base),
                   *map(str, sources), '-o', str(binary)]
        compiled = subprocess.run(command, capture_output=True, text=True, encoding='utf-8')
        self.assertEqual(compiled.returncode, 0, compiled.stdout + compiled.stderr)
        ran = subprocess.run([str(binary)], capture_output=True, text=True, encoding='utf-8')
        self.assertEqual(ran.returncode, 0, ran.stdout + ran.stderr)


if __name__ == '__main__':
    unittest.main()
