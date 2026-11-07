"""Optional setup routes retain their actors until explicit cleanup or a bounded timeout."""
import importlib.util
import json
import subprocess
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('stage_route', ROOT / 'tools/stage_route.py')
stage_route = importlib.util.module_from_spec(spec)
spec.loader.exec_module(stage_route)


class Routes(unittest.TestCase):
    def test_added_pikmin_depart_on_their_next_update_and_rejoin_immediately(self):
        runner = ROOT / 'build' / ('lua_host.exe' if sys.platform == 'win32' else 'lua_host')
        result = subprocess.run([runner, ROOT / 'tests/routes/new_pikmin_test.lua'], cwd=ROOT,
                                capture_output=True, text=True, check=True)
        self.assertIn('added Pikmin next-update departure and immediate rejoin pass', result.stdout)

    def test_chappy_waits_and_cleans_up_only_its_owned_actor(self):
        runner = ROOT / 'build' / ('lua_host.exe' if sys.platform == 'win32' else 'lua_host')
        result = subprocess.run([runner, ROOT / 'tests/routes/chappy_test.lua'], cwd=ROOT,
                                capture_output=True, text=True, check=True)
        self.assertIn('coordinated cleanup and timeouts pass', result.stdout)

    def test_captain_reports_missing_and_mounted_data(self):
        runner = ROOT / 'build' / ('lua_host.exe' if sys.platform == 'win32' else 'lua_host')
        result = subprocess.run([runner, ROOT / 'tests/routes/captain_test.lua'], cwd=ROOT,
                                capture_output=True, text=True, check=True)
        self.assertIn('mounted and missing captain data reported read-only', result.stdout)

    def test_dayplay_restores_original_character_and_leaders(self):
        runner = ROOT / 'build' / ('lua_host.exe' if sys.platform == 'win32' else 'lua_host')
        result = subprocess.run([runner, ROOT / 'tests/routes/dayplay_test.lua'], cwd=ROOT,
                                capture_output=True, text=True, check=True)
        self.assertIn('day captain setup and exact restoration pass', result.stdout)

    def test_motiontrace_observes_bounded_read_only_data(self):
        runner = ROOT / 'build' / ('lua_host.exe' if sys.platform == 'win32' else 'lua_host')
        result = subprocess.run([runner, ROOT / 'tests/routes/motiontrace_test.lua'], cwd=ROOT,
                                capture_output=True, text=True, check=True)
        self.assertIn('motion trace is bounded and read-only', result.stdout)

    def test_setup_is_explicit_and_keeps_the_sdk_receipt(self):
        target = ROOT / 'build/route-staging'
        target.mkdir(parents=True, exist_ok=True)
        receipt = json.dumps({'check_mods': {'check.anim': 'anim'}, 'checks': {'check.anim': ['found']}})
        (target / 'checks.json').write_text(receipt, encoding='utf-8')
        staged = stage_route.stage('chappy', target)
        self.assertTrue((staged / 'main.lua').is_file())
        self.assertEqual((target / 'checks.json').read_text(encoding='utf-8'), receipt)
        (staged / 'finish.txt').write_text('finish\n', encoding='utf-8')
        stage_route.stage('chappy', target)
        self.assertFalse((staged / 'finish.txt').exists())
        day = stage_route.stage('dayplay', target)
        self.assertTrue((day / 'main.lua').is_file())
        self.assertFalse((day / 'finish.txt').exists())
        self.assertEqual((target / 'checks.json').read_text(encoding='utf-8'), receipt)
        diagnostic = stage_route.stage('captain', target)
        self.assertTrue((diagnostic / 'main.lua').is_file())
        self.assertEqual((target / 'checks.json').read_text(encoding='utf-8'), receipt)
        observer = stage_route.stage('motiontrace', target)
        self.assertTrue((observer / 'main.lua').is_file())
        self.assertEqual((target / 'checks.json').read_text(encoding='utf-8'), receipt)
        with self.assertRaises(ValueError):
            stage_route.stage('../mods', target)
        with self.assertRaises(ValueError):
            stage_route.stage('chappy', target / 'not-staged')
