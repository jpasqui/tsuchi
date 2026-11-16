"""The Academy service: the schedule a mod supplies and its dates, and its state, events, boards and upload guard."""
import shutil
import subprocess
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
import generate
from gen import game_table

GEN = ROOT / 'build/gen'
FIXTURE = ROOT / 'build/academy-fixture'
BASE = ROOT / 'framework/base'
CORE = ROOT / 'framework/core'
CHECKS = ROOT / 'tests/game'


@unittest.skipUnless(shutil.which('clang++'), 'needs clang++')
class Academy(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        # The generated bindings and the fixture's game table, once for every program here.
        generate.write(GEN)
        for relative, text in game_table.generate(ROOT / 'tests/fixtures/game_table.yaml').items():
            path = FIXTURE / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            if not path.exists() or path.read_text(encoding='utf-8') != text:
                path.write_text(text, encoding='utf-8', newline='\n')

    def program(self, name, sources, passed, fixture_table=True):
        """Builds the host program `name` from `sources` and runs it. `passed` is the line it prints last."""
        program = ROOT / 'build' / f'{name}.exe'
        includes = [GEN / 'include', *([FIXTURE / 'framework'] if fixture_table else []), GEN / 'framework',
                    BASE, CORE, ROOT / 'framework/game', ROOT / 'tests/base']
        command = [shutil.which('clang++'), '-std=c++17', '-Wall', '-Wextra', '-Werror', '-fno-exceptions', '-fno-rtti',
                   *(piece for include in includes for piece in ('-I', include)), *sources, '-o', program]
        built = subprocess.run([str(piece) for piece in command], capture_output=True, text=True)
        self.assertEqual(built.returncode, 0, built.stdout + built.stderr)
        run = subprocess.run([str(program)], capture_output=True, text=True)
        self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
        self.assertIn(passed, run.stdout)

    def test_owned_schedule_refresh_preserves_other_native_requests(self):
        self.program('academy-local-test', [CORE / 'registry.cpp', CHECKS / 'academy_local_test.cpp'],
                     'academy owned schedule refresh checks passed')

    def test_native_board_display_ownership_and_restoration(self):
        self.program('academy-boards-test',
                     [CORE / 'registry.cpp', CORE / 'json.cpp', CORE / 'text.cpp', CHECKS / 'academy_boards_test.cpp'],
                     'academy native board ownership checks passed')

    def test_native_menu_observation_and_reopening(self):
        self.program('academy-events-test',
                     [CORE / 'registry.cpp', GEN / 'framework/academy_service.cpp', CHECKS / 'academy_events_test.cpp'],
                     'academy menu event checks passed')

    def test_unlock_uses_owned_config_without_save_changes(self):
        self.program('academy-unlock-test', [CORE / 'registry.cpp', CORE / 'text.cpp', CHECKS / 'academy_unlock_test.cpp'],
                     'academy unlock hold checks passed')

    def test_upload_guard_and_worker_completion(self):
        self.program('academy-online-test', [CORE / 'registry.cpp', GEN / 'framework/academy_service.cpp', CHECKS / 'academy_online_test.cpp'], 'academy upload guard checks passed')

    def test_native_state_results_and_api_guards(self):
        self.program('academy-backend-test',
                     [BASE / 'names.cpp', CORE / 'registry.cpp', GEN / 'framework/academy_service.cpp',
                      CHECKS / 'academy_backend_test.cpp'],
                     'academy backend checks passed')

    def test_schedule_reader_and_dates(self):
        self.program('academy-test',
                     [CORE / 'json.cpp', CORE / 'text.cpp', CORE / 'texts.cpp', CORE / 'registry.cpp',
                      CHECKS / 'academy_test.cpp'],
                     'academy schedule checks passed', fixture_table=False)


if __name__ == '__main__':
    unittest.main()
