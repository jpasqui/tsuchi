"""Runs every check in the repository. This is the one command to run before a commit.

    python tools/check.py                                   every check, test classes side by side on every core
    python tools/check.py test_ui_title test_all.Generated  only these modules, classes or tests
    python tools/check.py --serial                          every check in one process, one after another
    python tools/check.py --ci --report build/ci/checks.json

Side by side, each test module runs in a process of its own (each class, in the modules BY_CLASS names), the slowest
first by the times of the last run, kept in build/check-times.json. The generated folders and the libraries several
of them link are made once before they start. Tests that write the same files are listed in TOGETHER and run one
after another in one process.
"""
import argparse
import json
import os
import re
import subprocess
import sys
import time
import unittest
from concurrent.futures import ThreadPoolExecutor, as_completed
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
TESTS = ROOT / 'tests'
TIMES = ROOT / 'build' / 'check-times.json'

# A module's classes often write the same files (a folder named once at the top of the module), so each module runs
# in one process. These modules' classes write only files of their own, and run each in a process of its own.
BY_CLASS = ('test_all',)
# Tests that write the same files across modules or classes, so they never run at the same time. Each list runs in one
# process, in this order; a name is a module, a class or a single test. Every other test runs with the rest of its
# module (or its class, in BY_CLASS).
TOGETHER = [
    # The JavaScript programs in build/javascript-tests, and the check mods' objects in build/obj/check-mods that every
    # build of the stand-in game compiles.
    ['test_javascript', 'test_javascript_fixture',
     'test_all.Generated.test_actors_are_listed_described_and_moved',
     'test_all.Generated.test_map_changes_precede_level_play',
     'test_all.Generated.test_map_content_joins_the_initial_streaming_flush',
     'test_shader_target_lifetimes'],
    # The module builds into build/module and build/check-module, which no other test writes: on its own, so the
    # rest of its class does not wait for it.
    ['test_all.Generated.test_the_module_builds_for_the_game'],
]


def flatten(suite):
    for item in suite:
        if isinstance(item, unittest.TestSuite):
            yield from flatten(item)
        else:
            yield item


def load(names):
    """The tests to run, by id, in the order unittest finds them; and the load errors, as text."""
    sys.path.insert(0, str(TESTS))
    loader = unittest.TestLoader()
    suite = loader.loadTestsFromNames(names) if names else loader.discover(str(TESTS), pattern='test_*.py', top_level_dir=str(TESTS))
    return [test.id() for test in flatten(suite)], list(loader.errors)


def units(ids):
    """The processes to run: (name, the tests to give unittest, how many)."""
    def place(test):
        for number, names in enumerate(TOGETHER):
            for rank, name in enumerate(names):
                if test == name or test.startswith(name + '.'):
                    return number, rank
        return None

    groups, classes = {}, {}
    for order, test in enumerate(ids):
        where = place(test)
        if where:
            groups.setdefault(where[0], []).append((where[1], order, test))
        else:
            module = test.split('.', 1)[0]
            classes.setdefault(test.rsplit('.', 1)[0] if module in BY_CLASS else module, []).append(test)
    out = []
    for number, members in sorted(groups.items()):
        names = TOGETHER[number]
        out.append((names[0] if len(names) == 1 else f'{names[0]} and {len(names) - 1} more',
                    [test for _, _, test in sorted(members)], len(members)))
    out += [(name, tests, len(tests)) for name, tests in classes.items()]
    return out


def prepare():
    """What several classes write or link, made once before they start, so none of them makes it while another
    reads it. Each of these writes a file only when it changes, so the classes' own calls then change nothing."""
    started = time.perf_counter()
    sys.path.insert(0, str(TESTS))
    import test_all
    import quickjs_build
    test_all.generate.write(test_all.GEN)
    test_all.Generated().fixture()
    test_all.lua_objects()
    test_all.tls_archive()
    for own in (False, True):
        quickjs_build.objects(own)
        test_all.script_lua_objects(own)
        test_all.Generated().host_lua(own)  # build/lua_host, which several modules run and test_all builds
    return time.perf_counter() - started


def environment():
    return dict(os.environ, PYTHONPATH=os.pathsep.join(filter(None, [str(TESTS), os.environ.get('PYTHONPATH')])))


def run_unit(unit):
    name, tests, count = unit
    started = time.perf_counter()
    result = subprocess.run([sys.executable, '-m', 'unittest', '-v', *tests], cwd=ROOT, env=environment(),
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    text = result.stdout.decode('utf-8', errors='replace')
    ran = re.search(r'^Ran (\d+) tests?', text, re.MULTILINE)
    return name, result.returncode, time.perf_counter() - started, text, int(ran.group(1)) if ran else 0


def side_by_side(names, jobs):
    began = time.perf_counter()
    ids, errors = load(names)
    if errors:
        print('\n'.join(errors), file=sys.stderr)
        return 1
    if not ids:
        print('no tests to run', file=sys.stderr)
        return 1
    try:
        print(f'prepared the shared builds in {prepare():.0f}s', flush=True)
    except Exception as error:  # the classes that need them then fail with the details
        print(f'preparing the shared builds failed: {error}', flush=True)
    try:
        last = json.loads(TIMES.read_text(encoding='utf-8'))
    except (OSError, ValueError):
        last = {}
    todo = sorted(units(ids), key=lambda unit: -last.get(unit[0], float('inf')))  # the slowest, and new ones, first
    failed, ran, times = [], 0, {}
    with ThreadPoolExecutor(jobs) as pool:
        running = [pool.submit(run_unit, unit) for unit in todo]
        for done, future in enumerate(as_completed(running), 1):
            name, code, seconds, text, count = future.result()
            times[name], ran = round(seconds, 1), ran + count
            print(f'{"ok  " if code == 0 else "FAIL"} {seconds:6.1f}s  [{done}/{len(todo)}]  {name} ({count})', flush=True)
            if code:
                failed.append((name, text))
    TIMES.parent.mkdir(parents=True, exist_ok=True)
    TIMES.write_text(json.dumps({**last, **times}, indent=1, sort_keys=True), encoding='utf-8')
    for name, text in failed:
        print(f'\n{"=" * 70}\n{name}\n{"=" * 70}\n{text}')
    print(f'\nRan {ran} tests in {len(todo)} processes in {time.perf_counter() - began:.0f}s: '
          + ('OK' if not failed else f'FAILED ({len(failed)}: {", ".join(name for name, _ in failed)})'))
    return 1 if failed else 0


def main(argv=None):
    argv = sys.argv[1:] if argv is None else list(argv)
    if '--ci' in argv:
        import ci
        return ci.main([arg for arg in argv if arg != '--ci'])
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--serial', action='store_true', help='run every check in one process, one after another')
    parser.add_argument('-j', '--jobs', type=int, default=os.cpu_count() or 4, help='processes at once (default: one per core)')
    parser.add_argument('names', nargs='*', help='modules, classes or tests to run, as unittest names them (default: all)')
    args = parser.parse_args(argv)
    if args.serial:
        if args.names:
            return subprocess.run([sys.executable, '-m', 'unittest', '-v', *args.names], cwd=ROOT, env=environment()).returncode
        return subprocess.run([sys.executable, '-m', 'unittest', 'discover', '-s', 'tests', '-p', 'test_*.py', '-v'],
                              cwd=ROOT).returncode
    return side_by_side(args.names, max(1, args.jobs))


if __name__ == '__main__':
    sys.exit(main())
