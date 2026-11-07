"""A build's patch in pieces: the layout, the proof against a program, and what the routes do.

The programs here are made up, and the routes run on tests/patch_machine.py. None of this ran in the game.
"""
import copy
import shutil
import struct
import sys
import tempfile
import unittest
from pathlib import Path

import yaml

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
sys.path.insert(0, str(ROOT / 'tests'))

import build_patch  # noqa: E402
import external_patch_checks as external  # noqa: E402
import patch_space  # noqa: E402
from gen import game_table, scatter, stubs  # noqa: E402
from patch_machine import Machine  # noqa: E402

SUB_SP = 0xD10043FF     # a replaced instruction, which the machine only counts
PACIBSP = 0xD503237F
ADD_X0 = 0x91000400
RET = 0xD65F03C0
ADRP_X8 = 0x90000008     # adrp x8, with the pages to add
FUNCTION = 64           # the made-up code is functions this long, each ending in padding
TOOLS = shutil.which('clang') and shutil.which('ld.lld')
SITE_JUMP, NATIVE_JUMP = 0x5000, 0x6000     # where the framework would be
STATUS = 0x7000000000
BUILD_ID = 'AB' * 16


def branch(at, to, top=0x14000000):
    return top | (((to - at) >> 2) & 0x3FFFFFF)


def short(top, at, to, low=0):
    return top | ((((to - at) >> 2) & 0x7FFFF) << 5) | low


class Program:
    """A made-up program as an NSO header and a memory image: functions, padding, a place for the rest."""
    def __init__(self, functions=0x2000, tail=400):
        self.code_bytes = functions * FUNCTION + 0x1000 - tail
        self.tail = tail
        self.text = [ADD_X0] * (self.code_bytes // 4)
        for at in range(0, self.code_bytes - FUNCTION, FUNCTION):
            self.text[at // 4 + 11] = RET
            gap = 3 if (at // FUNCTION) % 2 else 2
            self.text[at // 4 + 12 + 3 - gap] = RET
            for word in range(16 - gap, 16):
                self.text[at // 4 + word] = 0
        self.code_end = (self.code_bytes + 0xFFF) & ~0xFFF
        self.ro_at, self.ro_size = self.code_end, 0x1000
        self.data_at, self.data_size = self.ro_at + self.ro_size, 0x1000
        self.bss_end = self.data_at + self.data_size + 0x100
        self.status_words = self.bss_end + 0x10
        self.relocation_targets = []
        self.listed = (0, 16)         # the one function the unwind table lists: its start and bytes
        self.plain_unwind = False     # True gives the table another form, which the tool does not read
        self.values = []
        self.text[1] = self.ro_at      # where MOD0 is

    def write(self, path):
        ro = bytearray(self.ro_size)
        ro[:0x1C] = b'MOD0' + struct.pack('<iiiiii', self.ro_size, self.bss_end - self.ro_at, self.bss_end - self.ro_at, 0x20, 0x34, 0)
        # An unwind table with the functions this program lists, each by where it starts and an entry that gives its length.
        ro[0x20:0x2C] = bytes((1, 0x1B, 0x03, 0x3B)) + struct.pack('<iI', 0, 1)
        struct.pack_into('<ii', ro, 0x2C, self.listed[0] - (self.ro_at + 0x20), 0x20)
        struct.pack_into('<I', ro, 0x40 + 12, self.listed[1])
        if self.plain_unwind:
            ro[0x20] = 2
        for at, value in enumerate(self.values):
            struct.pack_into('<Q', ro, 0x100 + 8 * at, value)
        data = bytearray(self.data_size)
        struct.pack_into('<QQQQQQ', data, 0, 7, self.data_at + 0x100, 8, 24 * len(self.relocation_targets), 0, 0)
        for at, target in enumerate(self.relocation_targets):
            struct.pack_into('<QQq', data, 0x100 + 24 * at, self.data_at + 0x800, 0x403, target)
        image = struct.pack(f'<{len(self.text)}I', *self.text) + bytes(self.tail)
        image += bytes(self.ro_at - len(image)) + bytes(ro) + bytes(data)
        head = bytearray(0x100)
        head[:4] = b'NSO0'
        struct.pack_into('<II', head, 0x14, 0, self.code_bytes)
        struct.pack_into('<II', head, 0x24, self.ro_at, self.ro_size)
        struct.pack_into('<II', head, 0x34, self.data_at, self.data_size)
        head[0x40:0x50] = bytes.fromhex(BUILD_ID)
        Path(path).write_bytes(bytes(head) + image)
        return path


def fixture(program, sites=40):
    """A table for a made-up program, and the program's words at each of its sites."""
    text = program.text
    at = FUNCTION * 8
    def next_site():
        nonlocal at
        at += FUNCTION * 4
        return at
    code_sites, native, conditional = {}, {}, {}
    for number in range(sites):
        where = next_site()
        word = PACIBSP if number % 5 == 1 else SUB_SP
        if number % 7 == 3:
            word = branch(where, where + 0x4000, 0x94000000)
            code_sites[f'site_{number}'] = {'address': where, 'instruction': word, 'call': True}
        else:
            code_sites[f'site_{number}'] = {'address': where, 'instruction': word}
        text[where // 4] = word
    where = next_site()
    code_sites['page'] = {'address': where, 'instruction': ADRP_X8 | (3 << 29) | (5 << 5)}
    text[where // 4] = code_sites['page']['instruction']
    def route(name, word, **extra):
        where = next_site()
        if callable(word):
            word = word(where)
        text[where // 4] = word
        native[name] = {'address': where, 'instruction': word, **extra}
    route('plain', SUB_SP, resume=False)
    route('with_exits', SUB_SP, exits={'one': FUNCTION * 100, 'two': FUNCTION * 101})
    route('branches', lambda at: branch(at, at + 0x800), resume=False)
    route('calls', lambda at: branch(at, at + 0x800, 0x94000000), resume=False)
    route('calls_register', 0xD63F0100, resume=False)
    route('jumps_register', 0xD61F0040, resume=False)
    route('if_equal', lambda at: short(0x54000000, at, at + 0x40), resume=False)
    route('if_zero', lambda at: short(0xB4000000, at, at + 0x40, 3), resume=False)
    route('if_bit', lambda at: 0x36080003 | (((0x40 >> 2) & 0x3FFF) << 5), resume=False)
    route('address_page', lambda at: 0x90000003 | (1 << 29), resume=False)
    route('address_near', lambda at: 0x10000003 | (8 << 5), resume=False)
    route('literal', lambda at: 0x58000003 | (16 << 5), resume=False)
    where = next_site()
    conditional['admission'] = {'address': where, 'instruction': 0x7100443F, 'next_instruction': 0x5400004A,
                                'register': 1, 'threshold': 17, 'admit': [19, 21]}
    text[where // 4], text[where // 4 + 1] = 0x7100443F, 0x5400004A
    return {'game': 'stand-in', 'version': '1', 'key': 'base', 'build_id': 'CD' * 16,
            'data': {'status_words': program.status_words},
            'constants': {'stub_base': program.code_bytes, 'stub_end': program.code_bytes + program.tail,
                          'code_size': program.code_end, 'native_jump_offset': 3480, 'dynamic_hook_slots': 9},
            'code_sites': code_sites, 'native_routes': native, 'conditional_sites': conditional,
            'identity': [{'offset': 0x100, 'hash': 1}]}


def build_file(raw, program):
    def span(offset):
        return {'offset': offset, 'hash': patch_space.span_hash(struct.pack('<4I', *program.text[offset // 4:offset // 4 + 4]))}
    return {'version': '2', 'build_id': BUILD_ID, 'identity': [span(0x104), span(0x124)],
            'data': raw['data'], 'constants': {key: value for key, value in raw['constants'].items()},
            'code_sites': {name: {key: value for key, value in body.items() if key in ('address', 'instruction')} for name, body in raw['code_sites'].items()},
            'native_routes': {name: {key: value for key, value in body.items() if key in ('address', 'instruction', 'exits')} for name, body in raw['native_routes'].items()},
            'conditional_sites': {name: {key: value for key, value in body.items() if key != 'admit'} for name, body in raw['conditional_sites'].items()}}


class Base(unittest.TestCase):
    def setUp(self):
        folder = tempfile.TemporaryDirectory()
        self.addCleanup(folder.cleanup)
        self.root = Path(folder.name)

    def laid_out(self, program=None, change=None):
        """A table and a build in a folder, with the build's patch space found and checked."""
        program = program or Program()
        raw = fixture(program)
        if change:
            change(raw)
        (self.root / 'builds').mkdir(exist_ok=True)
        self.table_path = self.root / 'table.yaml'
        base = copy.deepcopy(raw)
        # The base table's own block fits a space of its own, far from what is tested.
        base['constants']['stub_end'] = base['constants']['stub_base'] + 0x1000
        self.table_path.write_text(yaml.safe_dump(base, sort_keys=False), encoding='utf-8')
        self.build_path = self.root / 'builds' / 'second.yaml'
        self.build_path.write_text(yaml.safe_dump(build_file(raw, program), sort_keys=False), encoding='utf-8')
        self.program_path = program.write(self.root / 'main.nso')
        return program

    def find(self):
        return patch_space.find(self.table_path, 'second', patch_space.Program(self.program_path), True)

    def check(self, write=True):
        return patch_space.check(self.table_path, 'second', patch_space.Program(self.program_path), write)

    def table(self):
        return game_table.load(self.table_path, build=self.build_path)


class Pieces(Base):
    def test_a_tail_of_400_bytes(self):
        self.laid_out()
        self.assertEqual(self.find()['form'], 'pieces')
        table = self.table()
        self.assertTrue(stubs.in_pieces(table))
        pieces = stubs.claimed(table)
        constants = table['constants']
        needed = sum(size for _, size in pieces)
        self.assertGreater(needed, 400)
        self.assertGreater(sum(size for at, size in pieces if at >= constants['stub_base']), 300)
        for at, size in pieces:
            self.assertTrue(at + size <= constants['stub_base'] or (at >= constants['stub_base'] and at + size <= constants['stub_end']))
        said = self.check()
        self.assertEqual(said['form'], 'pieces')
        self.assertEqual(self.table()['patch_checked'], said['hash'])

    def test_no_space_after_the_code_at_all(self):
        def none(raw):
            raw['constants']['stub_end'] = raw['constants']['stub_base']
        self.laid_out(change=none)
        self.find()
        table = self.table()
        cells = stubs.cells(table)
        self.assertNotEqual(cells['block_hash'], cells['block'] + 8)   # no place holds both values of the mark
        self.assertEqual(self.check()['form'], 'pieces')

    def test_a_block_that_fits_stays_a_block(self):
        program = Program()
        raw = fixture(program)
        raw['constants']['stub_end'] = raw['constants']['stub_base'] + 0x1000
        alone = game_table.load_raw(copy.deepcopy(raw), 'x')
        raw['patch_space'] = [[FUNCTION * 3 - 12, 12]]
        listed = game_table.load_raw(raw, 'x')
        self.assertFalse(stubs.in_pieces(listed))
        self.assertEqual(stubs.patch_hash(alone), stubs.patch_hash(listed))
        self.assertEqual(stubs.cells(listed)['block_hash'], stubs.cells(listed)['block'] + 8)

    def test_the_real_table_stays_one_block(self):
        table = game_table.load(ROOT / 'game' / 'table.yaml')
        self.assertFalse(stubs.in_pieces(table))
        self.assertIsNone(table['patch_space'])

    def test_refusals(self):
        program = Program()
        raw = fixture(program)
        with self.assertRaisesRegex(game_table.SchemaError, 'spare space'):      # no list: as it always was
            game_table.load_raw(copy.deepcopy(raw), 'x')
        few = copy.deepcopy(raw)
        few['patch_space'] = [[FUNCTION * 3 - 12, 12]]
        with self.assertRaisesRegex(game_table.SchemaError, 'patch_space.py find'):
            game_table.load_raw(few, 'x')
        site = next(iter(raw['code_sites'].values()))['address']
        for places, why in (([[site, 12]], 'overlaps a patch site'),
                            ([[FUNCTION * 3 - 12, 12], [FUNCTION * 2 - 12, 12]], 'in order'),
                            ([[6, 12]], 'multiples of 4'),
                            ([[program.code_end, 12]], 'outside the code'),
                            ([[raw['constants']['stub_base'], 8]], 'spare space'),
                            ('nothing', 'a list')):
            changed = copy.deepcopy(raw)
            changed['patch_space'] = places
            with self.subTest(places=places), self.assertRaisesRegex(game_table.SchemaError, why):
                game_table.load_raw(changed, 'x')
        changed = copy.deepcopy(raw)
        changed['patch_space'] = [[0xFC, 8]]
        with self.assertRaisesRegex(game_table.SchemaError, 'identity'):
            game_table.load_raw(changed, 'x')

    def test_a_far_site_with_no_list_is_refused(self):
        raw = far_table()
        raw.pop('patch_space')
        raw['constants']['stub_end'] = raw['constants']['stub_base'] + 0x1000
        with self.assertRaisesRegex(game_table.SchemaError, 'more than 128 MB'):
            game_table.load_raw(raw, 'x')

    def test_a_list_too_sparse_to_go_far_is_refused(self):
        raw = far_table()
        raw['patch_space'] = [place for place in raw['patch_space'] if place[0] < 0x1000000 or place[0] > 0xD000000]
        with self.assertRaisesRegex(game_table.SchemaError, 'no free place on the way'):
            game_table.load_raw(raw, 'x')

    def test_external_patches_keep_off_every_listed_place(self):
        self.laid_out()
        self.find()
        places = yaml.safe_load(self.build_path.read_text(encoding='utf-8'))['patch_space']
        address, size = places[0]
        external.validate_fixed_ranges([(address - 4, b'\0\0\0\0')], self.table_path, 'second')
        with self.assertRaisesRegex(ValueError, 'patch space'):
            external.validate_fixed_ranges([(address + size - 4, b'\0\0\0\0')], self.table_path, 'second')

    def test_the_table_gives_where_each_value_of_the_mark_is(self):
        self.laid_out()
        self.find()
        cells = game_table.flatten(self.table())
        self.assertEqual(cells['stub.block'], stubs.cells(self.table())['block'])
        self.assertEqual(cells['stub.block_hash'], stubs.cells(self.table())['block_hash'])

    def test_patch_builder_wants_the_record(self):
        self.laid_out()
        self.find()
        with self.assertRaises(SystemExit) as refused:
            build_patch.build(self.root / 'out', self.table_path)
        self.assertIn('not been checked', str(refused.exception))
        self.check()
        text = self.build_path.read_text(encoding='utf-8')
        self.build_path.write_text(text.replace('patch_checked: "', 'patch_checked: "0'), encoding='utf-8')
        with self.assertRaises(SystemExit):
            build_patch.build(self.root / 'out', self.table_path)
        self.build_path.write_text(text, encoding='utf-8')
        # A changed list is a changed patch.
        said = yaml.safe_load(text)
        said['patch_space'].pop()
        self.build_path.write_text(yaml.safe_dump(said, sort_keys=False), encoding='utf-8')
        with self.assertRaises((SystemExit, game_table.SchemaError)):
            build_patch.build(self.root / 'out', self.table_path)


class Identity(Base):
    def test_a_wrong_hash_is_a_problem(self):
        self.laid_out()
        self.find()
        said = yaml.safe_load(self.build_path.read_text(encoding='utf-8'))
        said['identity'][0]['hash'] = 12345
        self.build_path.write_text(yaml.safe_dump(said, sort_keys=False), encoding='utf-8')
        self.assertRegex('\n'.join(self.check(False)['problems']), 'identity: the span at 0x104')

    def test_a_span_over_a_site_is_refused(self):
        self.laid_out()
        said = yaml.safe_load(self.build_path.read_text(encoding='utf-8'))
        said['identity'][0]['offset'] = said['code_sites']['site_0']['address'] - 8
        self.build_path.write_text(yaml.safe_dump(said, sort_keys=False), encoding='utf-8')
        with self.assertRaisesRegex(game_table.SchemaError, 'overlaps a patch site'):
            self.table()

    def test_the_record_covers_what_identifies_the_program(self):
        self.laid_out()
        self.find()
        self.check()
        said = yaml.safe_load(self.build_path.read_text(encoding='utf-8'))
        before = stubs.checked_hash(self.table())
        self.assertEqual(said['patch_checked'], before)
        said['build_id'] = 'EF' * 16
        self.build_path.write_text(yaml.safe_dump(said, sort_keys=False), encoding='utf-8')
        self.assertNotEqual(stubs.checked_hash(self.table()), before)
        with self.assertRaises(SystemExit):
            build_patch.build(self.root / 'out', self.table_path)


class Unwind(Base):
    def test_another_form_stops_the_tool(self):
        program = Program()
        program.plain_unwind = True
        self.laid_out(program)
        with self.assertRaisesRegex(ValueError, 'unwind table'):
            self.find()

    def test_a_place_inside_a_listed_function_is_refused(self):
        program = self.laid_out()
        self.find()
        table = self.table()
        first = next(at for at, _ in stubs.claimed(table) if at < table['constants']['stub_base'])
        program.listed = (first - 16, 64)
        program.write(self.program_path)
        self.assertRegex('\n'.join(self.check(False)['problems']), 'inside a function the unwind table lists')


class Forms(unittest.TestCase):
    def test_no_places_stay_a_list(self):
        import patch_space
        self.assertEqual(yaml.safe_load(patch_space.render([])), {'patch_space': []})

    def test_a_route_whose_displaced_call_is_far_from_the_block(self):
        target, address = 0x1000000, 0x5000000
        word = 0x94000000 | (((target - address) >> 2) & 0x3FFFFFF)
        route = {'address': address, 'instruction': word, 'entry': 0xA000000, 'exits': {}}
        self.assertEqual(stubs.block_out_of_reach({'native_routes': {'far': route}}), 'native_routes.far')
        self.assertIsNone(stubs.block_out_of_reach({'native_routes': {'far': {**route, 'entry': 0x6000000}}}))

    def test_a_block_that_cannot_be_aligned_goes_in_pieces(self):
        self.assertTrue(stubs.in_pieces({'patch_space': [], 'constants': {'stub_base': 0x1004}, 'code_sites': {}}))


class Proof(Base):
    """Each thing `check` must notice, on a program otherwise fine."""
    def setUp(self):
        super().setUp()
        self.program = self.laid_out()
        self.find()
        self.assertNotIn('problems', self.check())
        table = self.table()
        self.first = next(at for at, _ in stubs.claimed(table) if at < table['constants']['stub_base'])

    def refused(self, why):
        self.program.write(self.program_path)
        said = self.check(False)
        self.assertRegex('\n'.join(said.get('problems', [])), why)

    def test_a_word_in_use(self):
        self.program.text[self.first // 4] = ADD_X0
        self.refused('neither zero nor nop')

    def test_flow_running_into_the_place(self):
        at = self.first // 4
        while patch_space.is_free_word(self.program.text[at - 1]):
            at -= 1
        self.program.text[at - 1] = ADD_X0
        self.refused('can run on into it')

    def test_a_run_not_ending_on_a_boundary(self):
        at = self.first // 4
        while patch_space.is_free_word(self.program.text[at]):
            at += 1
        self.program.text[at] = 0
        self.program.text[at + 1] = ADD_X0
        self.refused('16-byte boundary')

    def refers(self, word):
        self.program.text[0x40] = word(0x40 * 4)
        self.refused('referred to')

    def test_a_branch_into_it(self):
        self.refers(lambda at: branch(at, self.first))

    def test_a_conditional_branch_into_it(self):
        self.refers(lambda at: short(0x54000000, at, self.first))

    def test_an_adr_at_it(self):
        self.refers(lambda at: 0x10000000 | (((self.first - at) & 3) << 29) | ((((self.first - at) >> 2) & 0x7FFFF) << 5))

    def test_an_adrp_pair_at_it(self):
        pages = (self.first >> 12) - (0x100 >> 12)
        self.program.text[0x40] = 0x90000003 | ((pages & 3) << 29) | (((pages >> 2) & 0x7FFFF) << 5)
        self.program.text[0x41] = 0x91000063 | ((self.first & 0xFFF) << 10)
        self.refused('address made')

    def test_a_relocation_at_it(self):
        self.program.relocation_targets = [self.first]
        self.refused('value at')

    def test_a_value_in_data_at_it(self):
        self.program.values = [self.first]
        self.refused('value at')

    def test_another_instruction_at_a_site(self):
        address = next(iter(self.table()['code_sites'].values()))['address']
        self.program.text[address // 4] = ADD_X0
        self.refused('the table says')

    def test_status_words_past_the_page(self):
        self.program.bss_end = self.program.status_words + 8
        self.refused('status_words')

    def test_spare_space_not_zero(self):
        self.program.tail = 400
        # The spare space is whatever follows the code in the image.
        original = self.program.write
        def write(path):
            original(path)
            raw = bytearray(Path(path).read_bytes())
            raw[0x100 + self.program.code_bytes] = 1
            Path(path).write_bytes(bytes(raw))
        self.program.write = write
        self.refused('not zero')

    def test_another_program(self):
        self.program_path.write_bytes(self.program_path.read_bytes()[:0x40] + b'\x11' * 16 + self.program_path.read_bytes()[0x50:])
        said = self.check(False)
        self.assertRegex('\n'.join(said['problems']), 'build ID')


def far_table(end=0xE000000):
    """A lot of code, 224 MB unless asked, with what shared code there is at its end."""
    sites = {'low': {'address': 0x1000, 'instruction': SUB_SP},
             'low_call': {'address': 0x2000, 'instruction': branch(0x2000, 0x1000000, 0x94000000), 'call': True},
             'middle': {'address': 0x7000000, 'instruction': PACIBSP},
             'page': {'address': 0x8000, 'instruction': ADRP_X8 | (1 << 29) | (0x40 << 5)},
             'high': {'address': end - 0x2000, 'instruction': SUB_SP}}
    routes = {'low_route': {'address': 0x3000, 'instruction': SUB_SP, 'exits': {'far_away': end - 0x2000000}},
              'high_route': {'address': end - 0x3000, 'instruction': SUB_SP, 'resume': False}}
    conditions = {'low': {'address': 0x4000, 'instruction': 0x7100443F, 'next_instruction': 0x5400004A, 'register': 1, 'threshold': 17, 'admit': [19]}}
    # Padding near each site, on a ladder over the whole length, and before the end.
    spots = [0x100 + 0x10 * n for n in range(32)]
    spots += [address + 0x100 + 0x10 * n for address in (0x1000, 0x2000, 0x3000, 0x4000, 0x8000) for n in range(32)]
    spots += [address + 0x1000 + 0x10 * n for address in range(0x1000000, end, 0x2000000) for n in range(8)]
    spots += [end - 0x4000 + 0x10 * n for n in range(64)]
    spots += [0x7000000 + 0x100 + 0x10 * n for n in range(32)]
    return {'game': 'stand-in', 'version': '1', 'key': 'far', 'build_id': 'EF' * 16,
            'data': {'status_words': end + 0x10000},
            'constants': {'stub_base': end - 0x190, 'stub_end': end, 'code_size': end, 'native_jump_offset': 3480},
            'code_sites': sites, 'native_routes': routes, 'conditional_sites': conditions,
            'patch_space': sorted([spot + 4, 12] for spot in set(spots))}


@unittest.skipUnless(TOOLS, 'needs clang and ld.lld')
class Routes(Base):
    """The patch assembled and linked, then run: what each route does against what the game does unpatched."""
    def setUp(self):
        super().setUp()
        self.memory = {}

    def words_of(self, table, program_words=None):
        records = build_patch.records(table, self.root / 'patch')
        words = dict(program_words or {})
        for address, data in records:
            for at in range(0, len(data) - len(data) % 4, 4):
                words[address + at] = int.from_bytes(data[at:at + 4], 'little')
        return words

    def with_framework(self, table):
        memory = dict(self.memory)
        def put(address, value, size=8):
            for i in range(size):
                memory[address + i] = (value >> (8 * i)) & 0xFF
        status = table['data']['status_words']
        put(status, stubs.STATUS_MAGIC)
        put(status + 8, STATUS)
        put(STATUS, stubs.STATUS_MAGIC)
        put(STATUS + 12, 0x10000, 4)
        put(STATUS + 32, 1, 4)
        put(STATUS + stubs.STATUS_SITE_JUMP, SITE_JUMP)
        put(STATUS + table['constants']['native_jump_offset'], NATIVE_JUMP)
        return memory

    def code_sites(self, table, words, opaque):
        for index, site in enumerate(table['code_sites'].values()):
            address = site['address']
            page = None if site['call'] else stubs.moved_page(address, site['instruction'])
            with self.subTest(site=address):
                # No framework: as if unpatched, but for x16, x17 and the flags, which are free at such a place.
                machine = Machine(words, self.memory, opaque)
                machine.pc = address
                before = machine.state()
                if site['call']:
                    machine.step()
                    self.assertEqual(machine.x[30], address + 4)
                    machine.run({site['target']})
                    self.assertEqual(machine.ran, [])
                    machine.x[30] = before[0][30]
                else:
                    machine.run({address + 4})
                    self.assertEqual(machine.ran, [] if page else [site['instruction']])
                    if page:
                        self.assertEqual(machine.x[8], page)
                self.same_but(before, machine.state(), (16, 17, 8) if page else (16, 17))
                # With one: into the framework with the site's number and everything as it came.
                machine = Machine(words, self.with_framework(table), opaque)
                machine.pc = address
                before = machine.state()
                if site['call']:
                    machine.step()
                    machine.x[30] = before[0][30]
                machine.run({SITE_JUMP})
                self.assertEqual(machine.x[17] & 0xFFFFFFFF, index)
                self.same_but(before, machine.state(), (16, 17))
                # And the framework's way back runs what was replaced.
                machine = Machine(words, self.memory, opaque)
                machine.pc = site['original']
                machine.run({site.get('target', address + 4)})
                self.assertEqual(machine.ran, [] if site['call'] or page else [site['instruction']])
                if page:
                    self.assertEqual(machine.x[8], page, 'the way back loads the page the game would have')

    def same_but(self, before, after, free):
        registers = [(n, a, b) for n, (a, b) in enumerate(zip(before[0], after[0])) if n not in free and a != b]
        self.assertEqual(registers, [])
        self.assertEqual(before[1], after[1])

    def native_routes(self, table, words, originals, opaque, setups):
        for index, (name, site) in enumerate(table['native_routes'].items()):
            address = site['address']
            for setup in setups:
                with self.subTest(route=name, setup=setup.__name__):
                    # The game's own instruction, once, says where it goes and what it changes.
                    game = Machine(originals, self.memory, opaque)
                    game.pc = address
                    setup(game)
                    game.step()
                    # No framework.
                    machine = Machine(words, self.memory, opaque)
                    machine.pc = address
                    setup(machine)
                    machine.run({game.pc})
                    self.assertEqual(machine.state(), game.state())
                    self.assertEqual(machine.ran, game.ran)
                    # A framework: its frame as tsuchi_native_jump reads it, then back the way it goes back.
                    machine = Machine(words, self.with_framework(table), opaque)
                    machine.pc = address
                    setup(machine)
                    before = machine.state()
                    machine.run({NATIVE_JUMP})
                    self.assertEqual(machine.x[30], site['returned'])
                    sp = machine.sp
                    self.assertEqual(sp, before[1] - 64)
                    self.assertEqual([machine.read(sp, 8), machine.read(sp + 8, 8), machine.read(sp + 16, 8)], [before[0][0], before[0][1], before[2]])
                    self.assertEqual([machine.read(sp + 32, 8), machine.read(sp + 48, 8), machine.read(sp + 56, 8)], [before[0][17], before[0][30], before[0][16]])
                    machine.x[0], machine.x[1], machine.nzcv, machine.x[17] = before[0][0], before[0][1], before[2], before[0][17]
                    machine.sp = sp + 48
                    frame_sp = machine.sp
                    machine.pc = site['returned']
                    machine.run({game.pc})
                    self.assertEqual(machine.state(), game.state())
                    # The other ways on from a handler.
                    ways = [(site['resume'], address + 4)] if site['resume'] else []
                    ways += [(site['exit_entries'][exit_name], target) for exit_name, target in site['exits'].items()]
                    for entry, target in ways:
                        machine = Machine(words, self.with_framework(table), opaque)
                        setup(machine)
                        machine.write(frame_sp, machine.x[30], 8)
                        machine.write(frame_sp + 8, machine.x[16], 8)
                        machine.sp = frame_sp
                        kept = machine.state()
                        machine.pc = entry
                        machine.run({target})
                        self.assertEqual(machine.state()[0], kept[0])
                        self.assertEqual(machine.sp, frame_sp + 16)

    def conditions(self, table, words, originals):
        for site in table['conditional_sites'].values():
            for value in (5, 16, 17, 18, *site['admit'], 40):
                with self.subTest(value=value):
                    machine = Machine(words, self.memory)
                    machine.pc = site['address']
                    machine.x[site['register']] = value
                    machine.run({site['address'] + 4})
                    taken = bool((machine.nzcv >> 31) & 1) == bool((machine.nzcv >> 28) & 1)
                    self.assertEqual(taken, value >= site['threshold'] and value not in site['admit'])

    def everything(self, table, program_words, opaque, setups):
        words = self.words_of(table, program_words)
        self.code_sites(table, words, opaque)
        self.native_routes(table, words, program_words, opaque, setups)
        self.conditions(table, words, program_words)
        return words

    def setups(self):
        def plain(machine):
            machine.nzcv = 0x60000000
        def zero_flags_clear(machine):
            machine.x[3], machine.nzcv = 0, 0x90000000
        def bits_set(machine):
            machine.x[3], machine.nzcv = 0xFF, 0x40000000
        return plain, zero_flags_clear, bits_set

    def test_a_tail_of_400_bytes_runs(self):
        program = self.laid_out()
        self.find()
        table = self.table()
        self.assertTrue(stubs.in_pieces(table))
        literal = table['native_routes']['literal']
        target = literal['address'] + 4 * 16
        self.memory.update({target + i: 0x40 + i for i in range(8)})
        original_words = {4 * n: word for n, word in enumerate(program.text)}
        words = self.everything(table, original_words, {SUB_SP, PACIBSP}, self.setups())
        # What the patch's mark says, where the framework reads it.
        cells = stubs.cells(table)
        self.assertEqual(words[cells['block']] | words[cells['block'] + 4] << 32, stubs.PATCH_MAGIC)
        said = stubs.patch_hash(table)
        self.assertEqual(words[cells['block_hash']] | words[cells['block_hash'] + 4] << 32, said)

    def test_files_of_a_build_in_pieces(self):
        self.laid_out()
        self.find()
        self.check()
        report = build_patch.build(self.root / 'out', self.table_path)
        self.assertEqual(sorted(report['builds']), ['base', 'second'])
        text = (self.root / 'out' / 'exefs' / 'second.pchtxt').read_text(encoding='ascii')
        table = self.table()
        for address, size in stubs.claimed(table):
            self.assertIn(f'\n{address:08X} ', text)

    def test_no_space_after_the_code_runs(self):
        def none(raw):
            raw['constants']['stub_end'] = raw['constants']['stub_base']
        program = self.laid_out(change=none)
        self.find()
        table = self.table()
        target = table['native_routes']['literal']['address'] + 4 * 16
        self.memory.update({target + i: 0x40 + i for i in range(8)})
        self.everything(table, {4 * n: word for n, word in enumerate(program.text)}, {SUB_SP, PACIBSP}, self.setups())

    def far(self, end, jumps):
        table = game_table.load_raw(far_table(end), 'far')
        self.assertTrue(stubs.in_pieces(table))
        placed = scatter.place(table)
        low = table['code_sites']['low']
        # The site cannot reach what it needs in one branch, and needs no more than one to reach its entry.
        self.assertGreater(abs(placed.labels['shared'] - low['address']), scatter.REACH)
        self.assertLessEqual(abs(low['entry'] - low['address']), scatter.REACH)
        self.assertGreaterEqual(placed.jumps, jumps)
        words = self.words_of(table)
        originals = {site['address']: site['instruction'] for site in table['native_routes'].values()}
        self.code_sites(table, words, {SUB_SP, PACIBSP})
        self.native_routes(table, words, originals, {SUB_SP}, self.setups()[:1])
        self.conditions(table, words, originals)

    def test_a_far_destination(self):
        self.far(0xE000000, 1)

    def test_a_destination_more_than_one_jump_away(self):
        self.far(0x20000000, 2)


if __name__ == '__main__':
    unittest.main()
