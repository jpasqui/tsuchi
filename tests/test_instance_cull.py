"""Real instance-cull backend: native calls, whole-pair ownership and teardown."""
import shutil
import subprocess
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
import generate
from gen import game_table


class InstanceCull(unittest.TestCase):
    def test_native_distances_ownership_and_lifecycle(self):
        gen = ROOT / 'build' / 'gen'
        generate.write(gen)
        fixture = ROOT / 'build' / 'instance-cull-fixture'
        for relative, source in game_table.generate(ROOT / 'tests' / 'fixtures' / 'game_table.yaml').items():
            path = fixture / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(source, encoding='utf-8')
        binary = ROOT / 'build' / ('instance_cull_test.exe' if sys.platform == 'win32' else 'instance_cull_test')
        command = [shutil.which('clang++'), '-std=c++17', '-Wall', '-Wextra', '-Werror']
        for path in (gen / 'include', fixture / 'framework', gen / 'framework',
                     ROOT / 'framework' / 'core', ROOT / 'framework' / 'base',
                     ROOT / 'framework' / 'game', ROOT / 'tests' / 'base'):
            command += ['-I', str(path)]
        command += [str(ROOT / path) for path in (
            'framework/core/registry.cpp', 'framework/base/objects.cpp',
            'framework/base/names.cpp', 'framework/game/holds.cpp',
            'framework/game/instance_cull.cpp', 'tests/game/instance_cull_test.cpp')]
        command += ['-o', str(binary)]
        compiled = subprocess.run(command, capture_output=True, text=True)
        self.assertEqual(compiled.returncode, 0, compiled.stdout + compiled.stderr)
        ran = subprocess.run([str(binary)], capture_output=True, text=True)
        self.assertEqual(ran.returncode, 0, ran.stdout + ran.stderr)


if __name__ == '__main__':
    unittest.main()
