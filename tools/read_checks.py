"""Reads the checks out of the framework's log: one row per check, passed, failed or not done.

    python tools/read_checks.py LOG [--staged build/test-mods/checks.json] [--json OUT]

LOG is the log's file from the save, `tsuchi/tsuchi.log` in the game's save folder. A check mod
writes a line for each thing that happens to a check:

    [<mod id>] check <name> planned
    [<mod id>] check <name> waiting <for what>
    [<mod id>] check <name> pass [<note>]
    [<mod id>] check <name> fail <why>

With --staged, the file tools/stage_test_mods.py writes, a check mod that was installed and
reported nothing is told as well, with the framework's own line about it as the reason.

Exits with 0 when every check passed, 1 when one failed or a mod reported nothing, and 2 when
none failed and one is not done.
"""
import argparse
import json
import re
import sys
from pathlib import Path

LINE = re.compile(r'^\[([^\]\s]+)\] (.*)$')
CHECK = re.compile(r'^check ([A-Za-z0-9_.\-]+) (planned|waiting|pass|fail)(?: (.*))?$')
MOD = re.compile(r'^mod (\S+) (?:blocked: (.*)|not running|(\S+) running)$')
FRAMEWORK = 'tsuchi'


def read(text, staged=(), expected=None):
    """What a log's text says of the checks. `staged` is the IDs of the check mods that were installed."""
    checks = {}    # by (mod, name), in the order first seen
    mistakes = []  # lines that break the rules a check's lines keep
    mods = {}      # by ID: what the framework said of the mod
    full = False
    planned = set()
    text = text.replace('\0', '')
    truncated = bool(text and not text.endswith('\n'))
    for line in text.splitlines():
        whose = LINE.match(line)
        if not whose:
            continue
        mod, said = whose.groups()
        if mod == FRAMEWORK:
            about = MOD.match(said)
            if about:
                mods[about.group(1)] = 'blocked: ' + about.group(2) if about.group(2) is not None else 'running' if about.group(3) else 'not running'
            full = full or said.startswith('the log is full')
            continue
        told = CHECK.match(said)
        if not told:
            if said.startswith('check '):
                mistakes.append(f'{mod}: a check line is not written correctly: {said}')
            continue
        name, word, rest = told.groups()
        key = (mod, name)
        if word == 'planned':
            if key in planned:
                mistakes.append(f'{mod}: check {name} was planned twice')
            planned.add(key)
        elif key not in planned:
            mistakes.append(f'{mod}: check {name} was reported before it was planned')
        check = checks.setdefault((mod, name), {'mod': mod, 'name': name, 'state': 'not done', 'detail': ''})
        if check['state'] in ('pass', 'fail'):
            if word in ('pass', 'fail'):
                mistakes.append(f'{mod}: check {name} ended twice')
            continue
        if word in ('pass', 'fail'):
            check['state'], check['detail'] = word, rest or ''
        elif word == 'waiting':
            check['detail'] = 'waiting ' + (rest or '')
    reported = {check['mod'] for check in checks.values()}
    for mod, names in (expected or {}).items():
        for name in names:
            checks.setdefault((mod, name), {'mod': mod, 'name': name, 'state': 'not done', 'detail': 'no report for a staged check'})
    rows = list(checks.values())
    silent = {mod: mods.get(mod, 'the log has nothing of it') for mod in staged if mod not in reported}
    counts = {state: sum(check['state'] == state for check in rows) for state in ('pass', 'fail', 'not done')}
    for mod in staged:
        state = mods.get(mod, '')
        if state.startswith('blocked:') or state == 'not running':
            mistakes.append(f'{mod}: the framework reports {state}')
    if counts['fail'] or silent or mistakes:
        verdict = 1
    elif counts['not done'] or full or truncated or not rows:
        verdict = 2
    else:
        verdict = 0
    return {'checks': rows, 'silent': silent, 'mistakes': mistakes, 'mods': mods, 'full': full, 'truncated': truncated, 'counts': counts, 'exit': verdict}


def table(result):
    """The rows as text, then a line that sums them up."""
    rows = [(check['mod'], check['name'], check['state'], check['detail']) for check in result['checks']]
    rows += [(mod, '', 'nothing', why) for mod, why in sorted(result['silent'].items())]
    widths = [max([len(row[i]) for row in rows] + [0]) for i in range(3)]
    lines = ['  '.join(part.ljust(width) for part, width in zip(row[:3], widths)) + ('  ' + row[3] if row[3] else '') for row in rows]
    lines += result['mistakes']
    if result['full']:
        lines.append('the log was full: checks made after that are not in it')
    if result['truncated']:
        lines.append('the log ends within a line: the run is not done')
    counts = result['counts']
    total = sum(counts.values())
    if not total:
        lines.append('no checks were reported')
    lines.append(f"{total} checks: {counts['pass']} passed, {counts['fail']} failed, {counts['not done']} not done"
                 + (f"; {len(result['silent'])} check mods reported nothing" if result['silent'] else ''))
    return '\n'.join(line.rstrip() for line in lines)


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('log', type=Path, help='the log file from the save: tsuchi/tsuchi.log')
    parser.add_argument('--staged', type=Path, help='the checks.json that tools/stage_test_mods.py wrote')
    parser.add_argument('--json', type=Path, dest='out', help='write the result here as JSON')
    arguments = parser.parse_args()
    staged = json.loads(arguments.staged.read_text(encoding='utf-8')) if arguments.staged else {}
    result = read(arguments.log.read_text(encoding='utf-8', errors='replace'), staged.get('check_mods', {}), staged.get('checks', {}))
    if arguments.out:
        arguments.out.write_text(json.dumps(result, indent=1) + '\n', encoding='utf-8')
    print(table(result))
    return result['exit']


if __name__ == '__main__':
    sys.exit(main())
