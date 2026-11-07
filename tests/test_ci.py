"""Coverage gaps must fail the CI gate, even when unittest accepts a skip."""
import io
import json
import os
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
import build_module  # noqa: E402
import check  # noqa: E402
import ci  # noqa: E402


def named_test(name):
    test = unittest.FunctionTestCase(lambda: None)
    test.id = lambda: name
    return test


class CIRunner(unittest.TestCase):
    def test_canonical_entrypoint_routes_ci_arguments_and_exit_status(self):
        with patch.object(ci, 'main', return_value=2) as strict, \
                patch.object(check.subprocess, 'run') as discovery:
            code = check.main(['--ci', '--report', 'build/ci/custom.json'])
        self.assertEqual(code, 2)
        strict.assert_called_once_with(['--report', 'build/ci/custom.json'])
        discovery.assert_not_called()

    def test_canonical_default_keeps_original_full_discovery_command(self):
        with patch.object(check.subprocess, 'run') as discovery, patch.object(ci, 'main') as strict:
            discovery.return_value.returncode = 7
            code = check.main([])
        self.assertEqual(code, 7)
        discovery.assert_called_once_with([sys.executable, '-m', 'unittest', 'discover',
                                          '-s', 'tests', '-p', 'test_*.py', '-v'], cwd=check.ROOT)
        strict.assert_not_called()

    def test_unexpected_skip_fails_despite_unittest_success(self):
        result = unittest.TestResult()
        result.testsRun = 1
        result.addSkip(named_test('required.arm64'), 'no emulator')
        self.assertTrue(result.wasSuccessful())
        report = ci.result_report(result, 'win32')
        self.assertEqual(report['status'], 'failed')
        self.assertEqual(report['unexpected_skips'][0]['test'], 'required.arm64')

    def test_platform_skip_requires_exact_test_and_reason(self):
        for host, name, reason, wanted in (
                ('linux', ci.TLS_TEST, ci.TLS_SKIP_REASON, 'passed'),
                ('win32', ci.TLS_TEST, ci.TLS_SKIP_REASON, 'failed'),
                ('linux', ci.TLS_TEST, 'new unexplained failure', 'failed'),
                ('linux', 'other.test', ci.TLS_SKIP_REASON, 'failed')):
            with self.subTest(host=host, name=name, reason=reason):
                result = unittest.TestResult()
                result.testsRun = 1
                result.addSkip(named_test(name), reason)
                self.assertEqual(ci.result_report(result, host)['status'], wanted)

    def test_empty_suite_is_not_a_passing_check(self):
        self.assertEqual(ci.result_report(unittest.TestResult(), 'win32')['status'], 'failed')

    def test_test_errors_are_reported_as_failures(self):
        result = unittest.TestResult()
        result.testsRun = 1
        result.errors.append((named_test('required.test'), 'trace'))
        report = ci.result_report(result, 'win32')
        self.assertEqual(report['status'], 'failed')
        self.assertEqual(report['errors'], [{'test': 'required.test', 'traceback': 'trace'}])

    def test_preflight_failure_writes_report_without_running_suite(self):
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / 'ci/checks.json'
            suite = unittest.TestSuite([named_test('unused')])
            with patch.object(ci, 'preflight', return_value={'problems': ['missing tool']}), \
                    patch.object(unittest.TextTestRunner, 'run') as runner:
                code = ci.run_checks(path, suite, io.StringIO())
            runner.assert_not_called()
            self.assertEqual(code, 2)
            self.assertEqual(json.loads(path.read_text(encoding='utf-8'))['status'], 'preflight_failed')

    def test_success_writes_result_and_returns_zero(self):
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / 'checks.json'
            with patch.object(ci, 'preflight', return_value={'problems': []}):
                code = ci.run_checks(path, unittest.TestSuite([named_test('required.test')]), io.StringIO())
            report = json.loads(path.read_text(encoding='utf-8'))
            self.assertEqual((code, report['status'], report['tests_run']), (0, 'passed', 1))

    def test_skipped_check_returns_failure_and_records_why(self):
        def skip():
            raise unittest.SkipTest('missing required emulator')

        check = unittest.FunctionTestCase(skip)
        with tempfile.TemporaryDirectory() as folder:
            path, stream = Path(folder) / 'checks.json', io.StringIO()
            with patch.object(ci, 'preflight', return_value={'problems': []}):
                code = ci.run_checks(path, unittest.TestSuite([check]), stream)
            report = json.loads(path.read_text(encoding='utf-8'))
            self.assertEqual(code, 1)
            self.assertEqual(report['unexpected_skips'][0]['reason'], 'missing required emulator')
            self.assertIn('CI failed because required checks were skipped', stream.getvalue())

    def test_preflight_rejects_missing_compilers_and_python_dependencies(self):
        with patch.object(ci.shutil, 'which', return_value=None), \
                patch.object(ci.importlib, 'import_module', side_effect=ImportError('missing')), \
                patch.object(ci, 'find_openssl', return_value=None), \
                patch.dict(os.environ, {'TSUCHI_BUILD_JOBS': '0'}):
            report = ci.preflight()
        problems = '\n'.join(report['problems'])
        for required in ('clang is required', 'clang++ is required', 'ld.lld is required',
                         'PyYAML is required', 'unicorn is required', 'TSUCHI_BUILD_JOBS'):
            self.assertIn(required, problems)

    def test_windows_preflight_accepts_the_shared_openssl_resolver(self):
        completed = ci.subprocess.CompletedProcess([], 0, stdout='tool version\n', stderr='')
        with patch.object(ci.sys, 'platform', 'win32'), \
                patch.object(ci.shutil, 'which', side_effect=lambda name: name if name != 'git' else None), \
                patch.object(ci.importlib, 'import_module'), \
                patch.object(ci.importlib.metadata, 'version', return_value='pinned'), \
                patch.object(ci.subprocess, 'run', return_value=completed), \
                patch.object(ci, 'find_openssl', return_value=Path('chosen-openssl.exe')) as resolver, \
                patch.dict(os.environ, {'TSUCHI_BUILD_JOBS': '2'}):
            report = ci.preflight()
        resolver.assert_called_once_with()
        self.assertEqual(report['problems'], [])
        self.assertEqual(report['tools']['openssl'], 'tool version')

    def test_build_jobs_override_is_positive_and_default_stays_available(self):
        with patch.dict(os.environ, {'TSUCHI_BUILD_JOBS': '2'}):
            self.assertEqual(build_module.build_jobs(), 2)
        with patch.dict(os.environ, {}, clear=True), patch.object(build_module.os, 'cpu_count', return_value=6):
            self.assertEqual(build_module.build_jobs(), 6)
        for value in ('', '0', '-1', 'two', '2.5'):
            with self.subTest(value=value), patch.dict(os.environ, {'TSUCHI_BUILD_JOBS': value}):
                with self.assertRaisesRegex(ValueError, 'positive integer'):
                    build_module.build_jobs()
