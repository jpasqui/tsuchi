"""The actual SDK JavaScript fixture retries unloaded worlds without hiding other errors."""
import re
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
from test_all import EXE, run


class JavaScriptFixture(unittest.TestCase):
    def test_actual_fixture_retries_readiness_and_rethrows_other_errors(self):
        fixture = ROOT / 'tests/fixtures/mods/check.javascript'
        # The existing host evaluates a global script. Remove only ESM declarations,
        # keeping the real helper, language check, and entire fixture body unchanged.
        dependencies = '\n'.join(re.sub(r'^export ', '', (fixture / name).read_text(encoding='utf-8'), flags=re.M)
                                 for name in ('lib/helper.js', 'language.js'))
        actual = re.sub(r'^import .*;\n', '', (fixture / 'main.js').read_text(encoding='utf-8'), flags=re.M)
        driver = (ROOT / 'tests/javascript/fixture_readiness.js').read_text(encoding='utf-8')
        self.assertEqual(driver.count('// ACTUAL_FIXTURE_HERE'), 1)
        script = ROOT / 'build/javascript-tests/fixture_readiness.js'
        script.parent.mkdir(parents=True, exist_ok=True)
        script.write_text(dependencies + '\n' + driver.replace('// ACTUAL_FIXTURE_HERE', actual), encoding='utf-8', newline='\n')
        programs = [script.parent / ('host-' + str(own) + EXE) for own in (False, True)]
        if not all(program.is_file() for program in programs):
            from test_javascript import JavaScript
            JavaScript.setUpClass()
        for program in programs:
            with self.subTest(program=program.name):
                told = run([program, script])
                self.assertIn('actual SDK JavaScript readiness and unexpected-error visibility passed', told.stdout)


if __name__ == '__main__':
    unittest.main()
