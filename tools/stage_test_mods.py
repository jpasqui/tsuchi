"""Lays the checks' own mods out the way the game sees installed mods.

    python tools/stage_test_mods.py              the default checks
    python tools/stage_test_mods.py tables text  the checks of those names, and no others
    python tools/stage_test_mods.py --build sw2-demo hooks   targets written with that build's numbers

A mod is installed as `romfs/tsuchi/mods/<id>/` in a folder the emulator or the console
layers over the game's own files. This copies tests/fixtures/mods there, under
build/test-mods.

A folder whose manifest names a check under "meta" is a check mod: it makes its checks through
the API and writes what it finds to the log. With names, only the check mods of those names are
laid out. A meta.check_explicit check is installed only when its group is named, so
travel and destructive routes can run separately on isolated saves. The other folders are what the checks still made by the check build look at, and are
always laid out; the names are written to `romfs/tsuchi/checks.txt` for that build as well.
`checks.json` lists the check mods that were laid out, for tools/read_checks.py.
"""
import json
import re
import shutil
import struct
import subprocess
import sys
from pathlib import Path

from gen import game_table
import tick_state_fixture
import ai_order_fixture
import native_hook_fixture

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'sdk'))

import build_mod  # noqa: E402

SOURCE = ROOT / 'tests' / 'fixtures' / 'mods'
TARGET = ROOT / 'build' / 'test-mods'
HELPER = ROOT / 'sdk' / 'check' / 'check.lua'
TABLE = ROOT / 'game' / 'table.yaml'
# Entries only the check mods read. The framework is built from TABLE alone.
FIXTURES = ROOT / 'game' / 'fixtures.yaml'
# The same entries with another build's numbers, by the build's key.
BUILD_FIXTURES = ROOT / 'game' / 'build-fixtures'


def check_of(folder):
    """The name of the check a fixture makes, from its manifest's "meta". None for a folder that is no check mod."""
    try:
        meta = json.loads((folder / 'mod.json').read_text(encoding='utf-8')).get('meta')
    except (OSError, ValueError, AttributeError):  # some fixtures have no manifest, or a broken one, on purpose
        return None
    return meta.get('check') if isinstance(meta, dict) else None


def explicit_check(folder):
    """Whether this check requires its group to be selected for an isolated run."""
    try:
        meta = json.loads((folder / 'mod.json').read_text(encoding='utf-8')).get('meta')
    except (OSError, ValueError, AttributeError):
        return False
    value = meta.get('check_explicit', False) if isinstance(meta, dict) else False
    if not isinstance(value, bool):
        raise ValueError(f'{folder.name}: meta.check_explicit must be a boolean')
    return value


def check_mods():
    """Every check mod among the fixtures: its ID, and the name of its check."""
    found = {folder.name: check_of(folder) for folder in sorted(SOURCE.iterdir()) if folder.is_dir()}
    return {name: check for name, check in found.items() if check}


def expected_checks(mods):
    """The names each check mod declares, so a dropped line cannot hide a missing check."""
    expected = {}
    for name in mods:
        names = json.loads((SOURCE / name / 'mod.json').read_text(encoding='utf-8')).get('meta', {}).get('checks', [])
        if not isinstance(names, list) or any(not isinstance(item, str) or not re.fullmatch(r'[A-Za-z0-9_.\-]+', item) for item in names):
            raise ValueError(f'{name}: meta.checks must list check names')
        if len(set(names)) != len(names):
            raise ValueError(f'{name}: meta.checks names a check twice')
        if names:
            expected[name] = names
    return expected


def merged(given, base):
    """`given` with what only `base` has added in: a build's table with the entries of the checks' own mods."""
    if not isinstance(given, dict) or not isinstance(base, dict):
        return given
    out = dict(base)
    for key, value in given.items():
        out[key] = merged(value, base.get(key))
    return out


def staged_table(build_file=None):
    """The table the stager writes targets from. A build file says every entry of the framework's table but none of
    the checks' own (game/fixtures.yaml): those are the build's from game/build-fixtures/<key>.yaml, where it has one,
    and the base table's otherwise."""
    base = game_table.load(TABLE, FIXTURES)
    if not build_file:
        return base
    own = BUILD_FIXTURES / build_file.name
    if own.is_file():
        base = merged(game_table.load(TABLE, own), base)
    return merged(game_table.load(TABLE, None, build_file), base)


def stage(only=(), target=TARGET, build=None):
    """`only` names the checks a run wants; none named means the default checks. `build` is the key of a file in
    game/builds/: the targets the stager writes are then that build's, not the base table's. Returns the IDs of the
    mods laid out in `target`."""
    # These modules qualify one ordered sequence, with distinct check names in
    # receipts. Any native-hook name selects the complete isolated suite.
    if any(group in only for group in native_hook_fixture.GROUPS):
        only = tuple(dict.fromkeys((*only, *native_hook_fixture.GROUPS)))
    mods = target / 'romfs' / 'tsuchi' / 'mods'
    if target.exists():
        shutil.rmtree(target)
    mods.mkdir(parents=True)
    checks = check_mods()
    build_file = ROOT / 'game' / 'builds' / f'{build}.yaml' if build else None
    if build_file and not build_file.is_file():
        sys.exit(f'{build_file} is missing')
    table = staged_table(build_file)
    ai_files = ai_order_fixture.files(table)
    native_files = native_hook_fixture.files(table) if 'native-hooks' in only else {}
    names = sorted(folder.name for folder in SOURCE.iterdir() if folder.is_dir())
    names = [name for name in names if name not in checks or
             (checks[name] in only if only else not explicit_check(SOURCE / name))]
    for name in names:
        shutil.copytree(SOURCE / name, mods / name)
        if name == 'check.tick-state':
            (mods / name / 'targets.txt').write_text(tick_state_fixture.targets(TABLE, FIXTURES, table), encoding='utf-8', newline='\n')
        if name == 'check.hooks':
            tick = table['slots']['engine_tick']
            (mods / name / 'targets.txt').write_text(
                f"{tick['table'] + tick['offset']:#x}\n{tick['stock']:#x}\n",
                encoding='utf-8', newline='\n')
        if name in ('check.cleanup', 'check.cleanup-producer'):
            actor = table['structs']['game_actor']['fields']
            values = [table['data']['tick_state_actor_table'], table['structs']['actor_table']['fields']['request_tick'],
                      table['functions']['actor_request_tick']['address'], actor['ai'], actor['movement'],
                      actor['tick_requested'], table['structs']['game_component']['fields']['tick_requested']]
            paths = [table['assets']['tick_state_actor'], table['assets']['cleanup_material']]
            (mods / name / 'targets.txt').write_text(''.join(f'{value:#x}\n' for value in values) + '\n'.join(paths) + '\n',
                                                    encoding='utf-8', newline='\n')
        for relative, data in ai_files.get(name, {}).items():
            path = mods / name / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(data)
        for relative, data in native_files.get(name, {}).items():
            path = mods / name / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(data)
        # A check mod written in Lua gets the SDK's helper, where any mod's author would put it.
        if name in checks and (mods / name / 'main.lua').is_file() and not (mods / name / 'check.lua').exists():
            shutil.copyfile(HELPER, mods / name / 'check.lua')
    # All dependencies are in place before a compiled mod includes a shared fixture helper.
    for name in names:
        # A compiled mod is kept here as its sources, and built with the SDK as any mod is.
        sources = mods / name / 'source'
        if sources.is_dir():
            defines = ['NH_LIVE_CHECK=1'] if name in native_hook_fixture.MODS else []
            build_mod.build(sorted(path for path in sources.iterdir() if path.suffix in ('.c', '.cpp')), mods / name / 'mod.nro', define=defines)
            shutil.rmtree(sources)
    # A file the framework takes for a module and the system's loader does not: its parts leave a gap between them.
    rejected = bytearray(b'\x5A' * 0x4000)
    struct.pack_into('<4sIII', rejected, 0x10, b'NRO0', 0, len(rejected), 0)
    struct.pack_into('<IIIIIII', rejected, 0x20, 0, 0x1000, 0x2000, 0x1000, 0x3000, 0x1000, 0)
    (mods / 'check.compiled-rejected' / 'mod.nro').write_bytes(rejected)
    # The script mod runs the language check in the game and compares with the text from this machine.
    language = ROOT / 'tests' / 'lua' / 'language.lua'
    runner = ROOT / 'build' / ('lua_host.exe' if sys.platform == 'win32' else 'lua_host')
    if not runner.exists():
        sys.exit('build/lua_host is missing: run tools/check.py first')
    shutil.copyfile(language, mods / 'check.script' / 'language.lua')
    told = subprocess.run([runner, language], capture_output=True, check=True).stdout
    # As the script has it, whatever this machine ends its lines with.
    (mods / 'check.script' / 'language.txt').write_bytes(told.replace(b'\r\n', b'\n'))
    javascript = mods / 'check.javascript'
    if javascript.is_dir():
        runner = ROOT / 'build/javascript-tests' / ('host-False.exe' if sys.platform == 'win32' else 'host-False')
        if not runner.exists():
            run = subprocess.run([sys.executable, '-m', 'unittest', 'discover', '-s', 'tests', '-p', 'test_javascript.py', '-k', 'test_language_on_host_and_framework_libc'], cwd=ROOT)
            if run.returncode: sys.exit('JavaScript language runner could not be built')
        told = subprocess.run([runner, ROOT / 'tests/javascript/language.js'], capture_output=True, check=True).stdout
        marker = b'javascript host passed'
        told = told.split(marker)[0].replace(b'\r\n', b'\n').rstrip(b'\n')
        (javascript / 'language.txt').write_bytes(told)
    if only:
        (target / 'romfs' / 'tsuchi' / 'checks.txt').write_text('\n'.join(only) + '\n', encoding='ascii', newline='\n')
    staged = {name: checks[name] for name in names if name in checks}
    (target / 'checks.json').write_text(json.dumps({'check_mods': staged, 'checks': expected_checks(staged)}, indent=1) + '\n', encoding='utf-8', newline='\n')
    return names


if __name__ == '__main__':
    arguments = sys.argv[1:]
    build = None
    if '--build' in arguments:
        at = arguments.index('--build')
        if at + 1 >= len(arguments):
            sys.exit("--build needs a build's key")
        build = arguments[at + 1]
        del arguments[at:at + 2]
    print(f'{len(stage(arguments, build=build))} mods in {TARGET}')
    sys.exit(0)
