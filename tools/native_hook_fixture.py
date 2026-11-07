"""Generate and qualify the isolated native hook checks.

All game-specific numbers come from the table and its private-check overlay.
This produces first-party C headers; it never reads or exports a game image.

    python tools/native_hook_fixture.py LOG --staged checks.json --json result.json
"""
import argparse
from collections import Counter
import json
from pathlib import Path
import re
import sys

import read_checks

BASE = 'check.native-hooks-base'
MODS = (BASE, 'check.native-hooks-chain', 'check.native-hooks-observer')
GROUPS = ('native-hooks', 'native-hooks-chain', 'native-hooks-observer')
EXPECTED = {
    MODS[0]: ['setup', 'registers', 'ordinary', 'self_remove'],
    MODS[1]: ['setup', 'chain_order', 'ordinary', 'base_removed', 'remove', 'restart', 'startup_cleanup_armed'],
    MODS[2]: ['setup', 'startup_cleanup', 'wrong_bytes', 'reuse', 'ordinary', 'cleanup'],
}
EXPECTED_BLOCK = 'blocked: mod.nro: its start answered not_ready'
EXPECTED_MISTAKE = f'{MODS[1]}: the framework reports {EXPECTED_BLOCK}'


def qualify(text, receipt):
    """Validate this suite's intentional startup rejection without hiding raw failures.

    The generic reader must still fail a blocked staged mod. Only this exact
    three-mod suite, all seventeen passes, and the expected final mod states
    make its one deliberate rejection acceptable. This proves callback
    inactivity after refusal; hook-storage reclamation needs separate evidence.
    """
    staged = dict(zip(MODS, GROUPS))
    problems = []
    if not isinstance(receipt, dict) or receipt.get('check_mods') != staged or receipt.get('checks') != EXPECTED:
        problems.append('staged receipt must name exactly the three native hook mods, their groups and all seventeen declared checks')
    # Canonical expectations stay in force even when an untrusted receipt omits
    # one of the proof obligations. Preserve the generic result unchanged.
    raw = read_checks.read(text, staged, EXPECTED)
    keys = {(mod, name) for mod, names in EXPECTED.items() for name in names}
    reported = [(row['mod'], row['name']) for row in raw['checks']]
    if len(reported) != len(keys) or set(reported) != keys:
        problems.append('log must contain exactly the seventeen declared native hook checks')
    if raw['counts'] != {'pass': 17, 'fail': 0, 'not done': 0}:
        problems.append('every declared native hook check must pass')
    if raw['silent']:
        problems.append('a staged native hook mod reported nothing')
    if raw['full'] or raw['truncated']:
        problems.append('native hook log must be complete, untruncated and not full')
    wanted_states = {MODS[0]: 'running', MODS[1]: EXPECTED_BLOCK, MODS[2]: 'running'}
    if raw['mods'] != wanted_states:
        problems.append('framework must report only base and observer running and the exact deliberate chain rejection')
    if raw['mistakes'] != [EXPECTED_MISTAKE] or raw['exit'] != 1:
        problems.append('the sole generic-reader failure must be the exact deliberate chain startup rejection')
    statuses = Counter()
    ended = set()
    terminal_positions = {}
    for position, line in enumerate(text.replace('\0', '').splitlines()):
        whose = read_checks.LINE.match(line)
        if not whose:
            continue
        mod, said = whose.groups()
        if mod == read_checks.FRAMEWORK and said.startswith('mod '):
            about = read_checks.MOD.match(said)
            if not about:
                problems.append('framework mod status line is malformed')
            else:
                reported_mod = about.group(1)
                statuses[reported_mod] += 1
                if reported_mod == MODS[1] and said != f'mod {reported_mod} {EXPECTED_BLOCK}':
                    problems.append('chain status line must contain the exact deliberate startup rejection')
                elif reported_mod in (MODS[0], MODS[2]) and not re.fullmatch(r'mod ' + re.escape(reported_mod) + r' (?!not )\S+ running', said):
                    # Verify the exact status text as well as the parsed state.
                    problems.append(f'{reported_mod}: framework status must report a running version')
        elif (check := read_checks.CHECK.match(said)):
            name, word, _ = check.groups()
            key = (mod, name)
            if word in ('planned', 'waiting') and key in ended:
                problems.append(f'{mod}: check {name} has a report after it ended')
            if word in ('pass', 'fail'):
                ended.add(key)
                terminal_positions[key] = position
    if statuses != Counter({mod: 1 for mod in MODS}):
        problems.append('each staged native hook mod must have exactly one framework status report')
    armed = terminal_positions.get((MODS[1], 'startup_cleanup_armed'))
    observed = terminal_positions.get((MODS[2], 'startup_cleanup'))
    if armed is None or observed is None or armed >= observed:
        problems.append('the chain must arm its failed-start hook before the observer proves handler inactivity')
    accepted = not problems
    return {'qualified': accepted, 'exit': 0 if accepted else 1, 'problems': problems,
            'expected_rejections': [{'mod': MODS[1], 'state': EXPECTED_BLOCK, 'generic_mistake': EXPECTED_MISTAKE}] if accepted else [],
            'raw_verdict': raw}


def files(table):
    constants = table['constants']
    displacement = constants['native_hook_branch_offset']
    if displacement != 12:
        raise ValueError('native hook fixture expects the fourth import-veneer instruction')
    targets = []
    for name in ('cos', 'sin'):
        entry = table['functions'][f'pikmin_scatter_{name}']['address']
        words = [constants[f'native_hook_{name}_{i}'] for i in range(4)]
        if entry & 3 or entry < 0 or entry + 16 > constants['code_size']:
            raise ValueError('native hook fixture target is outside main code')
        if any(not isinstance(word, int) or not 0 <= word <= 0xffffffff for word in words):
            raise ValueError('native hook fixture instruction must be one word')
        if words[3] != 0xd61f0220:
            raise ValueError('native hook fixture requires a checked BR x17')
        targets.append('    {UINT64_C(%#x), {%s}}' % (entry, ', '.join(f'UINT32_C({word:#x})' for word in words)))
    identity = table['identity']
    if not identity or any(offset < 0 or offset + 16 > constants['code_size'] for offset, _ in identity):
        raise ValueError('native hook fixture needs bounded main-code identity anchors')
    anchors = ',\n'.join('    {UINT64_C(%#x), UINT64_C(%#x)}' % (offset, value) for offset, value in identity)
    header = '''/* Generated from game/table.yaml and game/fixtures.yaml. */
#ifndef NATIVE_HOOK_TARGETS_H
#define NATIVE_HOOK_TARGETS_H
#include <stdint.h>
static const uint64_t nh_code_size = UINT64_C(%#x);
static const uint64_t nh_branch_offset = UINT64_C(%#x);
static const struct NH_Target { uint64_t entry; uint32_t words[4]; } nh_targets[2] = {
%s
};
static const struct NH_Identity { uint64_t offset, hash; } nh_identity[] = {
%s
};
#endif
''' % (constants['code_size'], displacement, ',\n'.join(targets), anchors)
    return {BASE: {'native_hook_targets.h': header.encode('utf-8')}}


def main():
    parser = argparse.ArgumentParser(description='Qualify the isolated native hook suite and preserve its raw generic verdict.')
    parser.add_argument('log', type=Path)
    parser.add_argument('--staged', type=Path, required=True)
    parser.add_argument('--json', type=Path, dest='out')
    arguments = parser.parse_args()
    try:
        result = qualify(arguments.log.read_text(encoding='utf-8'), json.loads(arguments.staged.read_text(encoding='utf-8')))
        if arguments.out:
            arguments.out.write_text(json.dumps(result, indent=1) + '\n', encoding='utf-8')
    except (OSError, ValueError) as error:
        parser.error(str(error))
    print('native hook qualification: ' + ('PASS' if result['qualified'] else 'FAIL'))
    for problem in result['problems']:
        print(problem)
    for rejection in result['expected_rejections']:
        print(f"expected rejection: {rejection['mod']} {rejection['state']}")
    print(f"raw generic-reader exit: {result['raw_verdict']['exit']}")
    return result['exit']


if __name__ == '__main__':
    sys.exit(main())
