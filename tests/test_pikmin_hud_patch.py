"""The native HUD comparison route preserves registers and every stock decision."""
import copy
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
import build_patch
from gen import SchemaError, game_table, stubs
import yaml


class PikminHudPatch(unittest.TestCase):
    def raw(self):
        raw = {'game': 'HUDStandIn', 'version': '0.0', 'data': {'status_words': 0x1000},
               'constants': {'stub_base': 0x6000000, 'stub_end': 0x6001000},
               'code_sites': {'entry': {'address': 0x2000000, 'instruction': 0xD503201F}}}
        raw['conditional_sites'] = {'admit': {'address': 0x2000100, 'instruction': 0x7100443F,
            'next_instruction': 0x5400004A, 'register': 1, 'threshold': 17, 'admit': [19, 21]}}
        return raw

    def load(self, raw, directory):
        path = directory / 'table.yaml'
        path.write_text(yaml.safe_dump(raw), encoding='utf-8')
        return game_table.load(path)

    def test_rejects_mismatched_instructions_and_unbounded_admissions(self):
        cases = [('instruction', 0x7100403F), ('instruction', 0x7100445F),
                 ('register', 31), ('threshold', 4096), ('address', 0x2000101),
                 ('next_instruction', 0x14000000), ('next_instruction', 0x5400004B),
                 ('admit', []), ('admit', [19, 19]), ('admit', [16]),
                 ('admit', [4095]), ('admit', [True]), ('admit', list(range(17, 26)))]
        with tempfile.TemporaryDirectory() as folder:
            directory = Path(folder)
            for key, value in cases:
                with self.subTest(key=key, value=value):
                    raw = self.raw()
                    raw['conditional_sites']['admit'][key] = value
                    with self.assertRaises(SchemaError): self.load(raw, directory)
            raw = self.raw()
            raw['constants']['stub_end'] = raw['constants']['stub_base'] + stubs.MARK + stubs.SHARED + 2 * stubs.PER_SITE * len(raw['code_sites'])
            with self.assertRaises(SchemaError): self.load(raw, directory)
            raw = self.raw()
            first = next(iter(raw['code_sites'].values()))
            first['address'] = 0x2000104
            with self.assertRaises(SchemaError): self.load(raw, directory)

    def test_hash_and_layout_include_the_comparison_route(self):
        with tempfile.TemporaryDirectory() as folder:
            directory = Path(folder)
            raw = self.raw()
            table = self.load(raw, directory)
            base = table['constants']['stub_base']
            _, originals = stubs.layout(base, table['constants']['stub_end'], len(table['code_sites']))
            self.assertEqual(table['conditional_sites']['admit']['entry'], originals + stubs.PER_SITE * len(table['code_sites']))
            self.assertEqual(stubs.block_size(table), stubs.MARK + stubs.SHARED + 2 * stubs.PER_SITE * len(table['code_sites']) + 32)
            baseline = stubs.patch_hash(table)
            for key, value in [('admit', [19, 22]), ('address', 0x20000FC), ('next_instruction', 0x5400006A)]:
                changed = copy.deepcopy(raw)
                changed['conditional_sites']['admit'][key] = value
                self.assertNotEqual(stubs.patch_hash(self.load(changed, directory)), baseline)
            renamed = copy.deepcopy(table)
            renamed['conditional_sites'] = {'another': next(iter(renamed['conditional_sites'].values()))}
            self.assertEqual(stubs.patch_hash(renamed), baseline)

    @unittest.skipUnless(shutil.which('clang') and shutil.which('ld.lld'), 'needs ARM64 assembler and linker')
    def test_emitted_native_route_changes_no_register_and_only_admits_two_colors(self):
        def signed(word, bits):
            return word - (1 << bits) if word & (1 << (bits - 1)) else word
        def flags(a, b):
            result = (a - b) & 0xFFFFFFFF
            return (bool(result & 0x80000000), result == 0, a >= b,
                    bool(((a ^ b) & (a ^ result)) & 0x80000000))
        with tempfile.TemporaryDirectory() as folder:
            directory = Path(folder)
            table = self.load(self.raw(), directory)
            records = build_patch.records(table, directory / 'patch')
            words = {address + i: struct.unpack('<I', data[i:i + 4])[0]
                     for address, data in records for i in range(0, len(data), 4)}
            site = table['conditional_sites']['admit']
            self.assertNotIn(site['address'] + 4, words)
            for color in list(range(256)) + [0xFFFFFFFF, 0x80000000, 0x7FFFFFFF]:
                regs = [0xABCDEF0000000000 + i for i in range(31)]
                regs[1] = color
                before = list(regs)
                pc, nzcv = site['address'], (True, True, True, True)
                for _ in range(16):
                    if pc == site['address'] + 4: break
                    word = words[pc]
                    if word & 0x7C000000 == 0x14000000:
                        pc += signed(word & 0x03FFFFFF, 26) * 4
                    elif word & 0xFF00001F == 0x54000000:
                        pc += signed((word >> 5) & 0x7FFFF, 19) * 4 if nzcv[1] else 4
                    elif word & 0xFFC003FF == (0x7100001F | (1 << 5)):
                        nzcv = flags(regs[1] & 0xFFFFFFFF, (word >> 10) & 0xFFF)
                        pc += 4
                    else: self.fail(f'unexpected bridge instruction {word:08x}')
                else: self.fail('route did not return to the stock B.GE')
                self.assertEqual(regs, before)
                if color in (19, 21): self.assertNotEqual(nzcv[0], nzcv[3])
                else: self.assertEqual(nzcv, flags(color, 17))

    @unittest.skipUnless(shutil.which('clang++'), 'needs clang++')
    def test_native_hud_rows_and_icon_borrowing_with_host_standins(self):
        import generate
        generate.write(ROOT / 'build/gen')
        with tempfile.TemporaryDirectory() as folder:
            exe = Path(folder) / ('hud.exe' if sys.platform == 'win32' else 'hud')
            command = [shutil.which('clang++'), '-std=c++17', '-Wall', '-Wextra', '-Werror']
            for include in ['framework/base', 'framework/core', 'framework/game', 'build/gen/framework', 'build/gen/include']:
                command += ['-I', str(ROOT / include)]
            command += [str(ROOT / path) for path in ['tests/game/pikmin_hud_test.cpp',
                'framework/game/pikmin_hud.cpp', 'framework/game/holds.cpp', 'framework/core/registry.cpp']]
            command += ['-o', str(exe)]
            compiled = subprocess.run(command, capture_output=True, text=True)
            self.assertEqual(compiled.returncode, 0, compiled.stdout + compiled.stderr)
            ran = subprocess.run([str(exe)], capture_output=True, text=True)
            self.assertEqual(ran.returncode, 0, ran.stdout + ran.stderr)
