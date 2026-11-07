"""Finds and proves free places for a build's patch, by reading that build's program.

    python tools/patch_space.py find --build <key> --program <main> [--write]
    python tools/patch_space.py check --build <key> --program <main> [--write]

The program is an NSO header of 256 bytes followed by its memory image. Nothing of it is kept: `find`
writes addresses into game/builds/<key>.yaml and `check` writes the patch's hash. See DESIGN.md, "The
patch's space".
"""
import argparse
import re
import struct
import sys
from pathlib import Path

import yaml

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))

from gen import game_table, scatter, stubs  # noqa: E402

HEADER = 0x100
NOP = 0xD503201F
LOOK_AHEAD = 64     # words after an adrp in which its register is looked for
NEAR_SITE = 1 << 18 # bytes around a site that are searched
PER_SITE = 24       # places proposed around each site, of each size
NEAR_END = 1 << 18  # bytes before the end of the code searched for the shared code
AT_END = 64         # places proposed there
RUNG = 48 << 20     # how far apart places are proposed in code too long for one branch
PER_RUNG = 8


def span_hash(raw):
    """FNV-1a over 64 bits, the hash the framework reads an identity span with."""
    value = 0xCBF29CE484222325
    for byte in raw:
        value = ((value ^ byte) * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    return value


def is_free_word(word):
    return word == 0 or word == NOP


def ends_flow(word):
    """ret, retaa, retab, br, braa and the like, b, brk."""
    return ((word & 0xFFFFFC1F) in (0xD65F0000, 0xD61F0000) or word in (0xD65F0BFF, 0xD65F0FFF) or
            (word & 0xFEFFF800) == 0xD61F0800 or (word & 0xFC000000) == 0x14000000 or (word & 0xFFE0001F) == 0xD4200000)


class Program:
    """The parts of a program this tool reads."""
    def __init__(self, path):
        self.path = Path(path)
        with self.path.open('rb') as file:
            head = file.read(HEADER)
            if len(head) < HEADER or head[:4] != b'NSO0':
                raise ValueError(f'{path}: not an NSO header')
            self.text_at, self.text_size = struct.unpack_from('<II', head, 0x14)
            self.ro_at, self.ro_size = struct.unpack_from('<II', head, 0x24)
            self.data_at, self.data_size = struct.unpack_from('<II', head, 0x34)
            self.build_id = head[0x40:0x50].hex().upper()
            size = self.path.stat().st_size
            if size < HEADER + self.data_at + self.data_size:
                raise ValueError(f'{path}: its segments are not laid out as a memory image; is it still compressed?')
            file.seek(HEADER + self.text_at)
            raw = file.read(self.text_size)
            self.text = memoryview(raw).cast('I')
            module = struct.unpack_from('<I', raw, 4)[0]
            file.seek(HEADER + module)
            self.mod0 = file.read(0x1C)
        if self.mod0[:4] != b'MOD0':
            raise ValueError(f'{path}: no MOD0 where the code says')
        fields = struct.unpack_from('<iiiii', self.mod0, 4)
        _, _, self.bss_end, self.unwind_start, self.unwind_end = [module + field for field in fields]

    def read(self, address, size):
        with self.path.open('rb') as file:
            file.seek(HEADER + address)
            return file.read(size)

    def run_of(self, index):
        """(first word, end word) of the run of free words holding word `index`."""
        text, first, end = self.text, index, index
        while first and is_free_word(text[first - 1]):
            first -= 1
        while end < len(text) and is_free_word(text[end]):
            end += 1
        return first, end

    def run_problem(self, first, end):
        """Why the run of free words first..end is not padding between two functions, or None."""
        if first == 0 or end >= len(self.text):
            return 'it touches an end of the code'
        if not ends_flow(self.text[first - 1]):
            return f'the word before it, {self.text[first - 1]:#010x}, can run on into it'
        if (end * 4) % 16:
            return 'it does not end on a 16-byte boundary'
        return None

    def references(self, claimed):
        """{word index: what refers to it} for the words in `claimed`, from anywhere in the program."""
        found = {}
        pages = {(index * 4) >> 12 for index in claimed}
        limit = len(self.text)
        text = self.text
        kinds = [None] * 256
        for top in (0x14, 0x15, 0x16, 0x17, 0x94, 0x95, 0x96, 0x97):
            kinds[top] = 'branch'
        for top in (0x54, 0x34, 0x35, 0xB4, 0xB5):
            kinds[top] = 'short branch'
        for top in (0x36, 0x37, 0xB6, 0xB7):
            kinds[top] = 'bit branch'
        for top in (0x10, 0x30, 0x50, 0x70):
            kinds[top] = 'adr'
        for top in (0x90, 0xB0, 0xD0, 0xF0):
            kinds[top] = 'adrp'
        for top in (0x18, 0x58, 0x98, 0xD8, 0x1C, 0x5C, 0x9C):
            kinds[top] = 'literal'
        for at, word in enumerate(text):
            kind = kinds[word >> 24]
            if kind is None:
                continue
            if kind == 'branch':
                offset = word & 0x3FFFFFF
                target = at + (offset - 0x4000000 if offset & 0x2000000 else offset)
            elif kind == 'short branch' or kind == 'literal':
                offset = (word >> 5) & 0x7FFFF
                target = at + (offset - 0x80000 if offset & 0x40000 else offset)
            elif kind == 'bit branch':
                offset = (word >> 5) & 0x3FFF
                target = at + (offset - 0x4000 if offset & 0x2000 else offset)
            elif kind == 'adr':
                offset = ((word >> 29) & 3) | (((word >> 5) & 0x7FFFF) << 2)
                target = (at * 4 + (offset - 0x200000 if offset & 0x100000 else offset)) // 4
            else:
                offset = ((word >> 29) & 3) | (((word >> 5) & 0x7FFFF) << 2)
                page = ((at * 4) >> 12) + (offset - 0x200000 if offset & 0x100000 else offset)
                if page in pages:
                    self.paired(at, word & 31, page << 12, claimed, found)
                continue
            if target in claimed:
                found.setdefault(target, f'{kind} at {at * 4:#x}')
        self.scan_values(claimed, found, limit)
        return found

    def paired(self, at, register, page, claimed, found):
        """Looks after an adrp for the add or load that completes its address."""
        for later in range(at + 1, min(at + LOOK_AHEAD, len(self.text))):
            word = self.text[later]
            if (word >> 5) & 31 != register:
                continue
            if (word & 0xFF800000) == 0x91000000:      # add
                low = (word >> 10) & 0xFFF
            elif (word & 0x3B400000) == 0x39400000:    # a load with an unsigned offset
                scale = word >> 30
                if (word & 0x04800000) == 0x04800000:
                    scale = 4
                low = ((word >> 10) & 0xFFF) << scale
            else:
                continue
            target = (page + low) // 4
            if target in claimed:
                found.setdefault(target, f'address made at {at * 4:#x}')

    def scan_values(self, claimed, found, limit):
        """Any 64-bit value in the program's read-only or written data that points at a claimed word. The
        program's relocations are among them: each holds the address it is for, in whichever form."""
        for start, size in ((self.ro_at, self.ro_size), (self.data_at, self.data_size)):
            raw = self.read(start, size)
            for at, value in enumerate(memoryview(raw[: len(raw) & ~7]).cast('Q')):
                if value < limit * 4 and not value & 3 and (value >> 2) in claimed:
                    found.setdefault(value >> 2, f'value at {start + at * 8:#x}')

    def unwind_ranges(self):
        """[(first byte, end byte)] of the code the unwind table lists. Raises if its form is not the usual one, as nothing
        then shows that a place is outside every function."""
        raw = self.read(self.unwind_start, self.unwind_end - self.unwind_start)
        if len(raw) < 12 or raw[:4] != bytes((1, 0x1B, 0x03, 0x3B)):
            raise ValueError('the unwind table is not in the form this tool reads, so it cannot show a place lies outside every function')
        count = struct.unpack_from('<I', raw, 8)[0]
        ranges = []
        if len(raw) < 12 + 8 * count or not count:
            raise ValueError('the unwind table lists no functions, so it cannot show a place lies outside every function')
        table = struct.unpack_from(f'<{2 * count}i', raw, 12)
        frames = [self.unwind_start + value for value in table[1::2]]
        low = min(frames)
        data = self.read(low, max(frames) + 16 - low)
        for start, frame in zip(table[0::2], frames):
            start += self.unwind_start
            ranges.append((start, start + struct.unpack_from('<I', data, frame - low + 12)[0]))
        ranges.sort()
        return ranges


def merged_raw(table_path, key):
    """The table as `key` has it, before it is laid out, and the path of its file (None for the base table's)."""
    raw = yaml.safe_load(Path(table_path).read_text(encoding='utf-8'))
    if key == str(raw.get('key', raw['version'])):
        return raw, None
    file = Path(table_path).parent / 'builds' / f'{key}.yaml'
    if not file.is_file():
        raise ValueError(f'no build {key}: {file} is not there')
    return game_table.with_build(raw, yaml.safe_load(file.read_text(encoding='utf-8')), file.name), file


def proposals(program, raw):
    """[[address, bytes]] of whole runs of padding near what the patch has to reach."""
    sites = [body['address'] for group in ('code_sites', 'native_routes', 'conditional_sites')
             for body in (raw.get(group) or {}).values() if isinstance(body, dict) and not body.get('absent') and 'address' in body]
    total = len(program.text)
    wanted = {}

    def around(address, radius, each):
        center = min(address // 4, total - 1)
        low, high = max(1, center - radius // 4), min(total - 1, center + radius // 4)
        runs = {2: [], 3: []}
        at = low
        while at < high:
            if not is_free_word(program.text[at]):
                at += 1
                continue
            first, end = program.run_of(at)
            at = end
            size = end - first
            if size >= 2 and program.run_problem(first, end) is None:
                runs[min(size, 3)].append((abs(first - center), first, size))
        for size, found in runs.items():
            for _, first, whole in sorted(found)[:each]:
                wanted[first] = whole

    for address in sites:
        around(address, NEAR_SITE, PER_SITE)
    around(total * 4, NEAR_END, AT_END)
    if total * 4 > stubs.REACH:
        for address in range(RUNG, total * 4, RUNG):
            around(address, NEAR_SITE, PER_RUNG)
    return sorted([first * 4, size * 4] for first, size in wanted.items())


def without_refused(program, places):
    """The places nothing refers to."""
    claimed = {address // 4 + word for address, size in places for word in range(size // 4)}
    if not claimed:
        return places, 0
    found = program.references(claimed)
    ranges = program.unwind_ranges()
    def good(address, size):
        if any(address // 4 + word in found for word in range(size // 4)):
            return False
        return not any(start < address + size and address < end for start, end in ranges)
    kept = [place for place in places if good(*place)]
    return kept, len(places) - len(kept)


def render(places):
    # An empty list stays a list: with nothing under it the key would read as no list at all, the one-block form.
    return 'patch_space: []\n' if not places else 'patch_space:\n' + ''.join(f'  - [{address:#x}, {size}]\n' for address, size in places)


def set_top_key(text, key, rendered):
    """A build file with one top-level key replaced, or added at its end."""
    pattern = re.compile(rf'^{key}:.*(?:\n[ \t-].*)*\n?', re.MULTILINE)
    if pattern.search(text):
        return pattern.sub(lambda _: rendered, text)
    return text + ('' if text.endswith('\n') or not text else '\n') + rendered


def find(table_path, key, program, write):
    raw, file = merged_raw(table_path, key)
    places, dropped = without_refused(program, [place for place in proposals(program, raw)
                                                if not overlaps_site(place, raw)])
    raw = {**raw, 'patch_space': places}
    table = game_table.load_raw(raw, table_path, None if file is None else key)
    unused = scatter.unused_regions(table)
    before = stubs.patch_hash(table)
    kept = [place for place in places if place not in unused]
    table = game_table.load_raw({**raw, 'patch_space': kept}, table_path, None if file is None else key)
    if stubs.patch_hash(table) != before:
        raise ValueError('the layout changed when unused places were dropped')
    if write:
        if file is None:
            raise ValueError('the base table keeps its patch in one block; only a build file takes a list')
        text = file.read_text(encoding='utf-8')
        file.write_text(set_top_key(text, 'patch_space', render(kept)), encoding='utf-8', newline='\n')
    else:
        print(render(kept), end='')
    return {'places': len(kept), 'bytes': sum(size for _, size in kept), 'proposed': len(places) + dropped, 'refused': dropped,
            'form': 'pieces' if stubs.in_pieces(table) else 'block'}


def overlaps_site(place, raw):
    address, size = place
    for group in ('code_sites', 'native_routes', 'conditional_sites'):
        for body in (raw.get(group) or {}).values():
            if isinstance(body, dict) and not body.get('absent') and 'address' in body and address <= body['address'] < address + size + (8 if group == 'conditional_sites' else 0):
                return True
    return False


def problems(program, table):
    """Everything wrong between a build's table and its program. Empty when all of it holds."""
    out = []
    constants = table['constants']
    if table['build_id'] and table['build_id'].upper()[:32] != program.build_id:
        out.append(f'the program\'s build ID is {program.build_id}, and the table says {table["build_id"]}')
    # The table's code size is what the loader maps: the code up to the end of its last page.
    tail_end = (program.text_size + 0xFFF) & ~0xFFF
    if constants.get('code_size') != tail_end:
        out.append(f'code_size is {constants.get("code_size")}, and the program\'s code takes {tail_end} bytes')
    for group in ('code_sites', 'native_routes', 'conditional_sites'):
        for name, site in (table.get(group) or {}).items():
            if site.get('absent'):
                continue
            address = site['address']
            if address + 4 > program.text_size:
                out.append(f'{group}.{name}: {address:#x} is outside the code')
                continue
            if program.text[address // 4] != site['instruction']:
                out.append(f'{group}.{name}: the program has {program.text[address // 4]:#010x} at {address:#x}, and the table says {site["instruction"]:#010x}')
            if group == 'conditional_sites' and program.text[address // 4 + 1] != site['next_instruction']:
                out.append(f'{group}.{name}: the instruction after it is not the table\'s')
    for offset, said in table['identity']:
        if any(at < offset + 16 and offset < at + size for at, size in stubs.claimed(table)):
            out.append(f'identity: the span at {offset:#x} overlaps what the patch writes')
        # The framework selects a build by these with the patch applied, and a site's word is checked above to be unpatched.
        if span_hash(program.read(offset, 16)) != said:
            out.append(f'identity: the span at {offset:#x} hashes to {span_hash(program.read(offset, 16)):#x} in the program, and the table says {said:#x}')
    # The spare space after the code is in no segment, and the loader has it as zero.
    if constants['stub_base'] < program.text_size or constants['stub_end'] > tail_end:
        out.append(f'stub_base..stub_end must lie in {program.text_size:#x}..{tail_end:#x}')
    claimed = set()
    for address, size in stubs.claimed(table):
        if address >= constants['stub_base'] and address + size <= constants['stub_end']:
            if any(program.read(address, size)):
                out.append(f'the spare space at {address:#x} is not zero')
            continue
        if address % 4 or size % 4 or address + size > program.text_size:
            out.append(f'the piece at {address:#x} is outside the code')
            continue
        for index in range(address // 4, (address + size) // 4):
            if not is_free_word(program.text[index]):
                out.append(f'{index * 4:#x} holds {program.text[index]:#010x}, which is neither zero nor nop')
                continue
            claimed.add(index)
    seen = set()
    for index in sorted(claimed):
        first, end = program.run_of(index)
        if first in seen:
            continue
        seen.add(first)
        problem = program.run_problem(first, end)
        if problem:
            out.append(f'the place at {first * 4:#x}: {problem}')
    for index, what in sorted(program.references(claimed).items()) if claimed else ():
        out.append(f'{index * 4:#x} is referred to: {what}')
    if claimed:
        ranges = program.unwind_ranges()
        for index in sorted(claimed):
            address = index * 4
            if any(start <= address < end for start, end in ranges):
                out.append(f'{address:#x} is inside a function the unwind table lists')
    status = table['data'].get('status_words')
    if status is None or status % 8:
        out.append('data.status_words must be aligned to 8 bytes')
    else:
        last = (program.bss_end + 0xFFF) & ~0xFFF
        if status < program.bss_end or status + 16 > last:
            out.append(f'data.status_words at {status:#x} must lie past the end of .bss, {program.bss_end:#x}, and before {last:#x}')
    return out


def check(table_path, key, program, write):
    raw, file = merged_raw(table_path, key)
    table = game_table.load_raw(raw, table_path, None if file is None else key)
    found = problems(program, table)
    if found:
        return {'problems': found}
    said = stubs.checked_hash(table)
    if write:
        if file is None:
            raise ValueError('the base table has no record of this; only a build file does')
        text = file.read_text(encoding='utf-8')
        file.write_text(set_top_key(text, 'patch_checked', f'patch_checked: "{said}"\n'), encoding='utf-8', newline='\n')
    pieces = stubs.claimed(table)
    return {'form': 'pieces' if stubs.in_pieces(table) else 'block', 'hash': said, 'writes': len(pieces), 'bytes': sum(size for _, size in pieces)}


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('action', choices=('find', 'check'))
    parser.add_argument('--build', required=True, help='the build\'s key')
    parser.add_argument('--program', required=True, type=Path, help='its main program, an NSO header and memory image')
    parser.add_argument('--table', type=Path, default=ROOT / 'game' / 'table.yaml')
    parser.add_argument('--write', action='store_true', help='write the result into the build\'s file')
    args = parser.parse_args()
    report = (find if args.action == 'find' else check)(args.table, args.build, Program(args.program), args.write)
    for problem in report.get('problems', ()):
        print(problem, file=sys.stderr)
    if 'problems' in report:
        sys.exit(f'{len(report["problems"])} problems')
    print(report)


if __name__ == '__main__':
    try:
        main()
    except (ValueError, game_table.SchemaError) as error:
        sys.exit(str(error))
