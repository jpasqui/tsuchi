"""Combined finite patch encoder regressions, using synthetic native facts."""
import copy
from pathlib import Path
import shutil
import struct
import sys
import unittest

import yaml

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
import build_patch
from gen import game_table, stubs
from gen.schema import SchemaError
try:
    from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM, UC_HOOK_CODE
    from unicorn import arm64_const as R
except ImportError:
    Uc = None
OUTPUT = ROOT / 'build' / 't2-presenter'


def condition():
    return {'address': 0x8000, 'instruction': 0x7100443f, 'next_instruction': 0x5400004a,
            'register': 1, 'threshold': 17, 'admit': [19, 21]}


def fixture():
    return {'game': 'combined fixture', 'version': '1', 'build_id': '0' * 32,
            'data': {'status_words': 0x90000},
            'constants': {'stub_base': 0x80000, 'stub_end': 0x80770, 'native_jump_offset': 6152},
            'code_sites': {'ordinary': {'address': 0x4000, 'instruction': 0xd10043ff}},
            'native_routes': {'native': {'address': 0x1000, 'instruction': 0xd10043ff, 'resume': False}},
            'conditional_sites': {'admission': condition()}}


def load(raw):
    OUTPUT.mkdir(parents=True, exist_ok=True)
    path = OUTPUT / 'combined-table.yaml'
    path.write_text(yaml.safe_dump(raw, sort_keys=False), encoding='utf-8')
    return game_table.load(path)


def full_fixture():
    raw = fixture()
    raw['code_sites'] = {f'ordinary_{i}': {'address': 0x4000 + i*16, 'instruction': 0xd10043ff}
                         for i in range(21)}
    raw['native_routes'] = {f'native_{i}': {'address': 0x1000 + i*32, 'instruction': 0xd10043ff, 'resume': False}
                            for i in range(64)}
    for i in range(8):
        raw['native_routes'][f'native_{i}']['instruction'] = 0xd63f0100
    for i in (8, 9):
        raw['native_routes'][f'native_{i}']['instruction'] = 0x35000040
    raw['native_routes']['native_10']['instruction'] = 0xd61f0040
    for i in (0, 1):
        raw['native_routes'][f'native_{i}']['exits'] = {'later': 0x7000 + i*16}
    return raw


class CombinedEncoder(unittest.TestCase):
    def test_ordinary_bytes_and_hash_stay_unchanged(self):
        raw = fixture()
        raw.pop('native_routes'); raw.pop('conditional_sites')
        table = load(raw)
        self.assertEqual(stubs.assembly(table, stubs.patch_hash(table)),
                         stubs.ordinary_assembly(table, stubs.patch_hash(table)))

    def test_conditional_facts_share_header_layout_and_hash(self):
        raw = fixture(); table = load(raw)
        self.assertEqual(table['conditional_sites']['admission']['entry'], stubs.native_layout(table)[2])
        self.assertEqual(stubs.block_size(table), stubs.native_layout(table)[2] - raw['constants']['stub_base'] + 32)
        header = game_table.generate(OUTPUT / 'combined-table.yaml')['framework/game_table.h']
        self.assertIn('constexpr unsigned conditional_site_count = 1;', header)
        self.assertIn('constexpr unsigned native_route_count = 1;', header)
        original = stubs.patch_hash(table)
        for key, value in [('next_instruction', 0x5400006a), ('admit', [19, 22])]:
            changed = copy.deepcopy(raw); changed['conditional_sites']['admission'][key] = value
            self.assertNotEqual(stubs.patch_hash(load(changed)), original)

    def test_cross_kind_and_protected_next_collisions_fail_closed(self):
        for kind in ('code_sites', 'native_routes', 'conditional_sites'):
            changed = fixture()
            if kind == 'conditional_sites':
                changed[kind]['admission']['address'] = changed['native_routes']['native']['address']
            else:
                changed[kind][next(iter(changed[kind]))]['address'] = 0x8000
            with self.assertRaisesRegex(SchemaError, 'more than one patch owner'):
                load(changed)
        for kind in ('code_sites', 'native_routes'):
            changed = fixture(); changed[kind][next(iter(changed[kind]))]['address'] = 0x8004
            with self.assertRaisesRegex(SchemaError, 'protected B.GE'):
                load(changed)
        changed = fixture(); changed['conditional_sites']['admission']['address'] = 0x7fffc
        with self.assertRaisesRegex(SchemaError, 'protected B.GE overlaps the patch block'):
            load(changed)

    def test_exact_1904_and_extra_route_or_compare_overflow(self):
        raw = full_fixture(); table = load(raw)
        self.assertEqual(stubs.block_size(table), 1904)
        changed = copy.deepcopy(raw)
        changed['native_routes']['extra'] = {'address': 0x3000, 'instruction': 0xd10043ff, 'resume': False}
        with self.assertRaisesRegex(SchemaError, 'spare space'):
            load(changed)
        changed = copy.deepcopy(raw); changed['conditional_sites']['admission']['admit'].append(22)
        with self.assertRaisesRegex(SchemaError, 'spare space'):
            load(changed)

    @unittest.skipUnless(shutil.which('clang') and shutil.which('ld.lld'), 'ARM tools unavailable')
    def test_native_only_conditional_only_and_full_combined_records(self):
        variants = []
        for native, conditional in [(True, False), (False, True)]:
            raw = fixture(); raw['code_sites'] = {}
            if not native: raw.pop('native_routes')
            if not conditional: raw.pop('conditional_sites')
            variants.append(raw)
        variants.append(full_fixture())
        for raw in variants:
            table = load(raw); records = build_patch.records(table, OUTPUT / 'combined-patch')
            self.assertEqual(next(len(b) for a, b in records if a == raw['constants']['stub_base']), stubs.block_size(table))
            self.assertEqual(len(records), 1 + len(table['code_sites']) + len(table['native_routes']) + len(table['conditional_sites']))
            for left, right in zip(records, records[1:]):
                self.assertLessEqual(left[0] + len(left[1]), right[0])


@unittest.skipUnless(Uc and shutil.which('clang') and shutil.which('ld.lld'), 'Unicorn/ARM tools unavailable')
class ConditionalARM(unittest.TestCase):
    def test_admitted_and_stock_values_preserve_all_registers_and_fp_state(self):
        raw = fixture(); raw['code_sites'] = {}; raw.pop('native_routes')
        table = load(raw); records = build_patch.records(table, OUTPUT / 'combined-patch')
        for base in (0, 0x71000000, 0x8000000000):
            for value in (0, 16, 17, 19, 20, 21, 22, 0xffffffff):
                with self.subTest(base=base, value=value):
                    u = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
                    u.mem_map(base + 0x8000, 4096); u.mem_map(base + 0x80000, 4096)
                    for address, data in records: u.mem_write(base + address, data)
                    u.mem_write(base + 0x8004, struct.pack('<III', condition()['next_instruction'], 0xd503201f, 0xd503201f))
                    registers = [0xabc0000000000000 + i for i in range(31)]
                    registers[1] = 0xabc0000000000000 | value
                    vectors = [0x12340000000000000000 + i for i in range(32)]
                    for i, v in enumerate(registers): u.reg_write(getattr(R, f'UC_ARM64_REG_X{i}'), v)
                    for i, v in enumerate(vectors): u.reg_write(getattr(R, f'UC_ARM64_REG_Q{i}'), v)
                    u.reg_write(R.UC_ARM64_REG_SP, 0x70004000); u.reg_write(R.UC_ARM64_REG_NZCV, 0xa0000000)
                    u.reg_write(R.UC_ARM64_REG_FPCR, 0x400000); u.reg_write(R.UC_ARM64_REG_FPSR, 0x11)
                    stopped = []
                    def stop(engine, address, size, user):
                        if address in (base + 0x8008, base + 0x800c):
                            stopped.append(address); engine.emu_stop()
                    u.hook_add(UC_HOOK_CODE, stop); u.emu_start(base + 0x8000, 0, count=100)
                    threshold = 22 if value in (19, 21) else 17
                    result = (value - threshold) & 0xffffffff
                    n, z, c = result >> 31, int(result == 0), int(value >= threshold)
                    v = int(bool((value ^ threshold) & (value ^ result) & 0x80000000))
                    flags = n << 31 | z << 30 | c << 29 | v << 28
                    self.assertEqual(stopped, [base + (0x800c if n == v else 0x8008)])
                    self.assertEqual(u.reg_read(R.UC_ARM64_REG_NZCV), flags)
                    self.assertEqual([u.reg_read(getattr(R, f'UC_ARM64_REG_X{i}')) for i in range(31)], registers)
                    self.assertEqual([u.reg_read(getattr(R, f'UC_ARM64_REG_Q{i}')) for i in range(32)], vectors)
                    self.assertEqual(u.reg_read(R.UC_ARM64_REG_SP), 0x70004000)
                    self.assertEqual((u.reg_read(R.UC_ARM64_REG_FPCR), u.reg_read(R.UC_ARM64_REG_FPSR)), (0x400000, 0x11))


if __name__ == '__main__':
    unittest.main()
