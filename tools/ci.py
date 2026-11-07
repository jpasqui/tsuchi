"""Run the complete repository checks and reject unplanned gaps in coverage.

    python tools/check.py --ci --report build/ci/checks.json

Windows is the full host check platform. Other platforms may omit only the
Windows TLS harness, which remains an explicit entry in the result report.
Documentation is built separately with npm ci and npm run build in site/.
"""
import argparse
import importlib
import importlib.metadata
import json
import os
from pathlib import Path
import platform
import shutil
import subprocess
import sys
import time
import unittest

from host_tools import find_openssl

ROOT = Path(__file__).resolve().parents[1]
TLS_TEST = 'test_network_tls.NetworkTLS.test_certificates_limits_and_cancel'
TLS_SKIP_REASON = 'needs clang and openssl'


def expected_skips(host_platform):
    """Only this exact test/reason pair may be skipped outside Windows."""
    return {} if host_platform == 'win32' else {TLS_TEST: TLS_SKIP_REASON}


def preflight():
    """Find required tools before a missing dependency can become a test skip."""
    problems, tools, dependencies = [], {}, {}
    for name in ('clang', 'clang++', 'ld.lld'):
        executable = shutil.which(name)
        if not executable:
            problems.append(name + ' is required')
            continue
        try:
            completed = subprocess.run([executable, '--version'], capture_output=True,
                                       text=True, timeout=15)
            if completed.returncode:
                raise ValueError('version command exited ' + str(completed.returncode))
            lines = (completed.stdout + completed.stderr).splitlines()
            tools[name] = lines[0] if lines else 'version not reported'
        except (OSError, ValueError, subprocess.TimeoutExpired) as error:
            problems.append(name + ': ' + str(error))
    for module_name, distribution in (('yaml', 'PyYAML'), ('unicorn', 'unicorn')):
        try:
            importlib.import_module(module_name)
            dependencies[distribution] = importlib.metadata.version(distribution)
        except (ImportError, OSError, importlib.metadata.PackageNotFoundError) as error:
            problems.append(distribution + ' is required: ' + str(error))
    if sys.platform == 'win32':
        openssl = find_openssl()
        if openssl is None:
            problems.append('the TLS harness requires OpenSSL; set TSUCHI_OPENSSL to its executable file')
        else:
            try:
                completed = subprocess.run([str(openssl), 'version'], capture_output=True,
                                           text=True, timeout=15)
                if completed.returncode:
                    raise ValueError('version command exited ' + str(completed.returncode))
                tools['openssl'] = completed.stdout.strip().splitlines()[0]
            except (OSError, ValueError, IndexError, subprocess.TimeoutExpired) as error:
                problems.append('openssl: ' + str(error))
    value = os.environ.get('TSUCHI_BUILD_JOBS')
    if value is not None and (not value.isascii() or not value.isdecimal() or int(value) < 1):
        problems.append('TSUCHI_BUILD_JOBS must be a positive integer')
    return {'problems': problems, 'tools': tools, 'dependencies': dependencies}


def result_report(result, host_platform):
    allowed = expected_skips(host_platform)
    skipped = [{'test': test.id(), 'reason': reason,
                'expected': allowed.get(test.id()) == reason} for test, reason in result.skipped]
    unexpected = [entry for entry in skipped if not entry['expected']]
    passed = result.wasSuccessful() and not unexpected and result.testsRun > 0
    return {
        'status': 'passed' if passed else 'failed',
        'tests_run': result.testsRun,
        'failures': [{'test': test.id(), 'traceback': detail[-12000:]} for test, detail in result.failures],
        'errors': [{'test': test.id(), 'traceback': detail[-12000:]} for test, detail in result.errors],
        'skipped': skipped,
        'unexpected_skips': unexpected,
        'expected_failures': [{'test': test.id(), 'traceback': detail[-12000:]}
                              for test, detail in result.expectedFailures],
        'unexpected_successes': [test.id() for test in result.unexpectedSuccesses],
        'allowed_skips': allowed,
    }


def run_checks(report_path, suite=None, stream=None):
    """Run discovery once and write preflight or test results as JSON."""
    started = time.monotonic()
    report = {'format': 1, 'python': platform.python_version(), 'platform': sys.platform,
              'build_jobs': os.environ.get('TSUCHI_BUILD_JOBS'),
              **result_report(unittest.TestResult(), sys.platform)}
    check = preflight()
    report['preflight'] = check
    if check['problems']:
        report['status'] = 'preflight_failed'
        code = 2
        for problem in check['problems']:
            print(problem, file=stream or sys.stderr)
    else:
        try:
            if suite is None:
                suite = unittest.defaultTestLoader.discover(str(ROOT / 'tests'), pattern='test_*.py')
            result = unittest.TextTestRunner(stream=stream, verbosity=2).run(suite)
            report.update(result_report(result, sys.platform))
            code = 0 if report['status'] == 'passed' else 1
            if report['unexpected_skips']:
                names = ', '.join(entry['test'] for entry in report['unexpected_skips'])
                print('CI failed because required checks were skipped: ' + names,
                      file=stream or sys.stderr)
            if not result.testsRun:
                print('CI failed because no checks ran', file=stream or sys.stderr)
        except Exception as error:
            report.update(status='runner_failed', runner_error=type(error).__name__ + ': ' + str(error))
            code = 1
    report['duration_seconds'] = round(time.monotonic() - started, 3)
    report_path = Path(report_path)
    report_path.parent.mkdir(parents=True, exist_ok=True)
    report_path.write_text(json.dumps(report, indent=2, sort_keys=True) + '\n', encoding='utf-8')
    return code


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--report', type=Path, default=ROOT / 'build/ci/checks.json')
    args = parser.parse_args(argv)
    return run_checks(args.report)


if __name__ == '__main__':
    sys.exit(main())
