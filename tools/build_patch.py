"""Builds the patch that redirects the game table's code sites into the framework.

    python tools/build_patch.py            writes build/module/exefs/<build key>.pchtxt and build/module/ips/<build id>.ips for each build

One instruction is changed at each code site, and the spare space at the end of
the game's code is filled with the block tools/gen/stubs.py lays out. With the
framework absent, or one built from another table, every site behaves as the
game does unpatched: the block carries a hash of what it was made from, and the
framework comes into it only when its own is the same. Nothing from the game is
needed to build: the table holds the instruction at each site.
"""
import argparse
import shutil
import struct
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))

import build_module  # noqa: E402
from gen import game_table, scatter, stubs  # noqa: E402

def records(table, work):
    """[(address, bytes)] for everything the patch changes, in address order."""
    if 'status_words' not in table['data']:
        sys.exit('the game table needs data.status_words')
    source, script = stubs.assembly(table, stubs.patch_hash(table))
    work.mkdir(parents=True, exist_ok=True)
    (work / 'patch.S').write_text(source, encoding='utf-8', newline='\n')
    (work / 'patch.ld').write_text(script, encoding='utf-8', newline='\n')
    clang, linker = shutil.which('clang'), shutil.which('ld.lld')
    if not clang or not linker:
        sys.exit('clang and ld.lld are needed')
    build_module.run([clang, '--target=aarch64-none-elf', '-c', work / 'patch.S', '-o', work / 'patch.o'])
    build_module.run([linker, '-T', work / 'patch.ld', '-e', 'shared', '-o', work / 'patch.elf', work / 'patch.o'])
    raw = (work / 'patch.elf').read_bytes()
    _, sections, symbols = build_module.read_elf(raw)
    if stubs.in_pieces(table):
        return pieces_records(table, raw, sections, symbols)
    sites = list(table['code_sites'].values())
    base = table['constants']['stub_base']
    entries, originals = stubs.layout(base, table['constants']['stub_end'], len(sites))
    if (symbols['shared'], symbols['entries'], symbols['originals']) != (base + stubs.MARK, entries, originals):
        sys.exit('the assembled block is not laid out as tools/gen/stubs.py says')
    native_shared, native_return, _ = stubs.native_layout(table)
    block_end = stubs.block_end(table)
    if table.get('native_routes') or table['constants'].get('dynamic_hook_slots', 0):
        if symbols['native_shared'] != native_shared or symbols['native_return'] != native_return:
            sys.exit('the assembled native gateways have an unexpected layout')
        for index, site in enumerate(table['native_routes'].values()):
            for field in ('entry', 'returned', 'original', 'resume'):
                if symbols[f'native_{index}_{field}'] != site[field]:
                    sys.exit('the assembled native route disagrees with the game table')
            for name, entry in site.get('exit_entries', {}).items():
                if symbols.get(f'native_{index}_exit_{name}', entry) != entry:
                    sys.exit('the assembled native alternate exit disagrees with the game table')
    for index, site in enumerate(table.get('conditional_sites', {}).values()):
        if not site.get('absent') and symbols[f'condition_{index}'] != site['entry']:
            sys.exit('the assembled conditional route disagrees with the game table')
    if table['constants'].get('dynamic_hook_slots', 0) and symbols.get('dynamic_gateways') != table['dynamic_hook_base']:
        sys.exit('the assembled dynamic gateway reservation has an unexpected layout')
    if sections['.stub']['size'] != block_end - base:
        sys.exit('the assembled block has an unexpected size')
    out = []
    for name in ['.stub'] + [f'.site{index}' for index, site in enumerate(sites) if not site.get('absent')] + \
            [f'.native_site{index}' for index, site in enumerate(table.get('native_routes', {}).values()) if not site.get('absent')] + \
            [f'.condition{index}' for index, site in enumerate(table.get('conditional_sites', {}).values()) if not site.get('absent')]:
        section = sections[name]
        out.append((section['address'], raw[section['offset']:section['offset'] + section['size']]))
    out.sort()
    for left, right in zip(out, out[1:]):
        if left[0] + len(left[1]) > right[0]:
            sys.exit('the assembled patch records overlap')
    return out


def pieces_records(table, raw, sections, symbols):
    """The records of a patch in pieces, each where tools/gen/scatter.py said it would be."""
    expected, labels = scatter.expected_sections(table)
    extra = {name for name in sections if name.startswith(('.piece', '.site', '.native_site', '.condition'))} - set(expected)
    if extra:
        sys.exit(f'the linker added {sorted(extra)[0]} to the patch')
    for name, address in labels.items():
        if symbols.get(name) != address:
            sys.exit(f'the assembled {name} is not where tools/gen/scatter.py says')
    out = []
    for name, (address, size) in expected.items():
        section = sections.get(name)
        # A range-extension thunk from the linker would show here: it has no place in what was checked as free.
        if not section or section['address'] != address or section['size'] != size:
            sys.exit(f'the assembled {name} is not where tools/gen/scatter.py says')
        out.append((address, raw[section['offset']:section['offset'] + size]))
    out.sort()
    for left, right in zip(out, out[1:]):
        if left[0] + len(left[1]) > right[0]:
            sys.exit('the assembled patch records overlap')
    return out


def pchtxt(table, changed):
    """The patch as text, one instruction per line, with no blank line inside the block."""
    lines = [f'@nsobid-{table["build_id"].upper():0<64}', '@flag offset_shift 0x100', '@enabled']
    for address, data in changed:
        for at in range(0, len(data), 4):
            lines.append(f'{address + at:08X} {data[at:at + 4].hex().upper()}')
    return '\n'.join(lines + ['@stop']) + '\n'


def ips(changed):
    out = bytearray(b'IPS32')
    for address, data in changed:
        out += struct.pack('>IH', address + 0x100, len(data)) + data
    return bytes(out + b'EEOF')


def patch_names(table):
    """The two files of a build's patch: by build key, and by the ID the console's loader looks for. A patch
    finds its build by the ID inside or in its name, so those of several builds share a folder."""
    return f'exefs/{table["key"]}.pchtxt', f'ips/{table["build_id"].upper()}.ips'


def build_one(table, out):
    """Writes one build's patch under `out` and says what it wrote."""
    if len(table['build_id']) != 32:
        sys.exit('the game table needs build_id, 32 hex digits')
    changed = records(table, out / 'patch' / table['key'])
    text_name, binary_name = patch_names(table)
    files = {text_name: pchtxt(table, changed).encode('ascii'), binary_name: ips(changed)}
    for relative, data in files.items():
        path = out / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data)
    return {'files': sorted(files), 'sites': len(table['code_sites']), 'native_routes': len(table.get('native_routes', {})), 'conditional_sites': len(table.get('conditional_sites', {})), 'bytes_changed': sum(len(d) for _, d in changed),
            'hash': f'{stubs.patch_hash(table):016x}'}


def build(out, path=None):
    """Every build's patch. What the report says first is the first build's; `builds` has each by its key."""
    builds = game_table.load_builds(path or ROOT / 'game' / 'table.yaml')
    table = builds[0]
    if not table['code_sites'] and not table.get('native_routes') and not table.get('conditional_sites'):
        return {}
    # The base table's build has its patch proven in the game. Another's places and sites have to have
    # been read against that build's program.
    for each in builds[1:]:
        if each.get('patch_checked') != stubs.checked_hash(each):
            sys.exit(f'build {each["key"]}: its patch has not been checked against its program, or it has changed since. '
                     f'Run tools/patch_space.py check --build {each["key"]} --program <its main> --write')
    reports = {each['key']: build_one(each, out) for each in builds}
    files = sorted(name for each in reports.values() for name in each['files'])
    return {**reports[table['key']], 'files': files, 'builds': reports}


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('--out', type=Path, default=ROOT / 'build' / 'module')
    report = build(parser.parse_args().out)
    print(report or 'the game table has no code sites')


if __name__ == '__main__':
    try:
        main()
    except subprocess.CalledProcessError as error:
        sys.exit(str(error))
