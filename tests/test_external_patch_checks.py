"""Static external patch decoding and fixed/differing byte conflict checks."""
from pathlib import Path
import struct
import sys
import tempfile
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
import external_patch_checks as checks

BUILD = '0123456789ABCDEF0123456789ABCDEF'


def ips(records, width=4):
    output = b'IPS32' if width == 4 else b'PATCH'
    for offset, data in records:
        output += offset.to_bytes(width, 'big') + len(data).to_bytes(2, 'big') + data
    return output + (b'EEOF' if width == 4 else b'EOF')


def text(body, shift='@flag offset_shift 0x100\n', build=BUILD):
    return ('@nsobid-' + build.ljust(64, '0') + '\n' + shift + body + '\n@stop\n').encode()


class ExternalPatchTests(unittest.TestCase):
    def test_generator_formats_roundtrip_at_main_offsets(self):
        import build_patch
        from gen import game_table
        table = game_table.load(ROOT / 'game/table.yaml')
        records = [(0x1000, b'\x01\x02\x03\x04'), (0x2000, b'\x05\x06\x07\x08')]
        self.assertEqual(checks.decode_patch(build_patch.ips(records), '.ips', table['build_id']), records)
        self.assertEqual(checks.decode_patch(build_patch.pchtxt(table, records).encode(), '.pchtxt', table['build_id']), records)

    def test_ips_offsets_and_rle_in_both_formats(self):
        for width, header, end in ((3, b'PATCH', b'EOF'), (4, b'IPS32', b'EEOF')):
            with self.subTest(width=width):
                self.assertEqual(checks.decode_patch(ips([(0x1100, b'ABC')], width), '.ips', BUILD), [(0x1000, b'ABC')])
                data = header + (0x2100).to_bytes(width, 'big') + b'\0\0\0\x05Z' + end
                self.assertEqual(checks.decode_patch(data, '.ips', BUILD), [(0x2000, b'ZZZZZ')])
                self.assertEqual(checks.decode_patch(header + end, '.ips', BUILD), [])

    def test_ips_rejects_truncation_header_writes_and_trailing_extensions(self):
        good = ips([(0x1100, b'ABCD')])
        invalid = [b'', b'IPS31EEOF', good[:-1], good + b'\0', good + b'\0\0\0\0',
                   b'IPS32\0\0\x01\0\0', b'IPS32\0\0\x01\0\0\x04AB',
                   b'IPS32\0\0\x01\0\0\0\0\x02', b'IPS32\0\0\x01\0\0\0\0\0ZEEOF',
                   ips([(0xFF, b'A')]), ips([(0xFFFFFFFE, b'ABC')]),
                   b'PATCHEOF\0\0\0']
        for source in invalid:
            with self.subTest(source=source[:30]), self.assertRaises(ValueError):
                checks.decode_patch(source, '.ips', BUILD)

    def test_pchtxt_build_shift_and_disabled_sections(self):
        source = text('@enabled\n00001000 AABB // instruction\n@disabled\n00002000 CCDD\n@enabled\n00003000 EEFF')
        self.assertEqual(checks.decode_patch(source, '.pchtxt', BUILD.lower()), [(0x1000, b'\xAA\xBB'), (0x3000, b'\xEE\xFF')])
        # Without a shift, printed offsets address the NSO file including its header.
        self.assertEqual(checks.decode_patch(text('@enabled\n00001100 AABB', shift=''), '.pchtxt', BUILD), [(0x1000, b'\xAA\xBB')])
        bom = b'\xef\xbb\xbf' + source.replace(b'\n', b'\r\n')
        self.assertEqual(checks.decode_patch(bom, '.PCHTXT', BUILD), [(0x1000, b'\xAA\xBB'), (0x3000, b'\xEE\xFF')])

    def test_pchtxt_rejects_unsupported_or_ambiguous_syntax(self):
        valid = text('@enabled\n00001000 AABB')
        invalid = [text('@enabled\n00001000 AABB', build='F' * 32),
                   valid.replace(b'@nsobid-', b'@nsobid-z'), valid.replace(b'@stop\n', b''),
                   valid + b'00002000 AABB\n', text('@enabled\n00001000 ABC'),
                   text('@enabled\n-1 AABB'), text('@enabled\n100000000 AABB'),
                   text('@enabled\nFFFFFFFF AABB'), text('@enabled\n00001000 GG'),
                   text('@enabled\n@flag print_values'), text('@enabled\n@flag offset_shift 0x100'),
                   text('@enabled\n000000FF AA', shift=''), text('00001000 AABB'),
                   text('@enabled\n00001000 AABB\n@nsobid-' + BUILD),
                   b'@enabled\n00001000 AABB\n@stop\n', text('@disabled\n00001000 XX'),
                   text('@enabled\n00001000 AA\x00'), text('@enabled\n00001000 AA\x85'),
                   text('@enabled\n00001000 AA', shift='@flag offset_shift -0x100\n'),
                   text('@enabled\n00001000 AA', shift='@flag offset_shift 0x100\n@flag offset_shift 0x100\n')]
        for source in invalid:
            with self.subTest(source=source[:100]), self.assertRaises(ValueError):
                checks.decode_patch(source, '.pchtxt', BUILD)
        for build in ('', 'Z' * 32, 'A' * 33):
            with self.subTest(build=build), self.assertRaises(ValueError):
                checks.decode_patch(valid, '.pchtxt', build)
        with self.assertRaises(ValueError):
            checks.decode_patch(valid, '.unknown', BUILD)

    def test_decoding_budgets_refuse_before_unbounded_expansion(self):
        # Sixteen maximum RLE records fit below 1 MiB; the next must refuse.
        record = struct.pack('>IH', 0x1100, 0) + b'\xff\xffA'
        self.assertEqual(len(checks.decode_patch(b'IPS32' + record * 16 + b'EEOF', '.ips', BUILD)), 16)
        with self.assertRaises(ValueError):
            checks.decode_patch(b'IPS32' + record * 17 + b'EEOF', '.ips', BUILD)
        with self.assertRaises(ValueError):
            checks.decode_patch(b' ' * (checks.MAX_INPUT_BYTES + 1), '.ips', BUILD)
        with mock.patch.object(checks, 'MAX_SEGMENTS', 2), self.assertRaises(ValueError):
            checks.decode_patch(ips([(0x1100, b'A'), (0x1200, b'B'), (0x1300, b'C')]), '.ips', BUILD)
        with mock.patch.object(checks, 'MAX_DECODED_BYTES', 3), self.assertRaises(ValueError):
            checks.decode_patch(text('@enabled\n00001000 AABBCCDD'), '.pchtxt', BUILD)

    def test_self_conflicts_are_checked_and_identical_bytes_admitted(self):
        self.assertEqual(checks.decode_patch(ips([(0x1100, b'ABCD'), (0x1102, b'CDE')]), '.ips', BUILD),
                         [(0x1000, b'ABCD'), (0x1002, b'CDE')])
        with self.assertRaisesRegex(ValueError, 'conflicting'):
            checks.decode_patch(ips([(0x1100, b'ABCD'), (0x1102, b'XYZ')]), '.ips', BUILD)
        with self.assertRaisesRegex(ValueError, 'conflicting'):
            checks.decode_patch(text('@enabled\n00001000 AABBCC\n00001001 DD'), '.pchtxt', BUILD)

    def test_fixed_stub_and_all_site_word_boundaries(self):
        with tempfile.TemporaryDirectory(prefix='tsuchi-patch-check-') as folder:
            table = Path(folder) / 'table.yaml'
            table.write_text('constants: {stub_base: 0x3000, stub_end: 0x4000}\n'
                             'code_sites: {code: {address: 0x1000}}\n'
                             'native_routes: {inline: {address: 0x2000}}\n'
                             'conditional_sites: {condition: {address: 0x5000}}\n'
                             'data: {unprotected: 0x6000}\n', encoding='utf-8')
            allowed = [(0x0FFF, b'X'), (0x1004, b'X'), (0x2004, b'X'), (0x2FFF, b'X'),
                       (0x4000, b'X'), (0x5008, b'X'), (0x6000, b'X')]
            checks.validate_fixed_ranges(allowed, table)
            for offset, data in [(0xFFF, b'XY'), (0x1000, b'X'), (0x1003, b'X'),
                                 (0x2002, b'X'), (0x2FFF, b'XY'), (0x3FFF, b'X'),
                                 (0x5000, b'X'), (0x5004, b'X'), (0x5007, b'X')]:
                with self.subTest(offset=hex(offset)), self.assertRaisesRegex(ValueError, 'Tsuchi'):
                    checks.validate_fixed_ranges([(offset, data)], table)

    def test_real_table_reservations_are_used_without_protecting_all_main_data(self):
        import yaml
        tablepath = ROOT / 'game/table.yaml'
        table = yaml.safe_load(tablepath.read_text(encoding='utf-8'))
        targets = [table['constants']['stub_base'], table['constants']['stub_end'] - 1]
        targets.extend(site['address'] for group in ('code_sites', 'native_routes', 'conditional_sites')
                       for site in table.get(group, {}).values() if 'address' in site)  # a same_as route has none
        targets.extend(site['address'] + 4 for site in table.get('conditional_sites', {}).values())
        for offset in targets:
            with self.subTest(offset=hex(offset)), self.assertRaises(ValueError):
                checks.validate_fixed_ranges([(offset, b'\0')], tablepath)
        checks.validate_fixed_ranges([(0x1000, b'data')], tablepath)

    def test_cross_package_conflicts_compare_bytes_in_partial_overlaps(self):
        checks.validate_patch_conflicts({'external.a': [(0x1000, b'ABCD')],
                                        'external.b': [(0x1002, b'CDEF')],
                                        'external.c': [(0x1006, b'G')]})
        checks.validate_patch_conflicts({'external.a': [], 'external.b': [(0x1000, b'A')]})
        for named in ({'external.a': [(0x1000, b'ABCD')], 'external.b': [(0x1002, b'XD')]},
                      {'external.a': [(0x1000, b'A'), (0x1000, b'B')]},
                      {'external.a': [(-1, b'A')]}, {'external.a': [(0x1000, b'')]},
                      {'external.a': [(True, b'A')]}, {'external.a': [(0x1000, bytearray(b'A'))]}):
            with self.subTest(named=named), self.assertRaises(ValueError):
                checks.validate_patch_conflicts(named)
        with mock.patch.object(checks, 'MAX_COMBINED_BYTES', 3), self.assertRaises(ValueError):
            checks.validate_patch_conflicts({'a': [(0x1000, b'AB')], 'b': [(0x2000, b'CD')]})
        with mock.patch.object(checks, 'MAX_COMBINED_SEGMENTS', 1), self.assertRaises(ValueError):
            checks.validate_patch_conflicts({'a': [(0x1000, b'A'), (0x2000, b'B')]})


if __name__ == '__main__':
    unittest.main()
