"""Bounded external cheat containers and selected prelaunch output."""
import copy
import json
from pathlib import Path
import sys
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
import external_cheats as cheats


# Register loads and debug logs exercise selection without game-memory writes.
SOURCE = b'''{Shared register}
40010000 00000000 4D415354
[Code A]
FFF8C410
40000000 00000000 54534331
FFF8A400
[Code B]
FFF8C410
40000000 00000000 54534332
FFF8B400
'''


def code(name='Code', words=None):
    return {'name': name, 'words': [0] if words is None else words}


class ExternalCheatTests(unittest.TestCase):
    def test_master_and_normal_words_round_trip_through_json(self):
        parsed = cheats.parse(SOURCE)
        self.assertEqual(parsed, json.loads(json.dumps(parsed)))
        self.assertEqual(parsed['master'], code('Shared register', [0x40010000, 0, 0x4D415354]))
        self.assertEqual([row['name'] for row in parsed['codes']], ['Code A', 'Code B'])
        self.assertEqual(parsed['codes'][0]['words'], [0xFFF8C410, 0x40000000, 0, 0x54534331, 0xFFF8A400])

    def test_selection_outputs_only_chosen_codes_and_shared_master(self):
        parsed = cheats.parse(SOURCE)
        original = copy.deepcopy(parsed)
        first = cheats.render(parsed, {0})
        second = cheats.render(parsed, {1})
        self.assertEqual(cheats.parse(first['cheat']), {'master': parsed['master'], 'codes': [parsed['codes'][0]]})
        self.assertEqual(cheats.parse(second['cheat']), {'master': parsed['master'], 'codes': [parsed['codes'][1]]})
        self.assertEqual(first['toggles'], b'[Code A]\ntrue\n')
        self.assertEqual(second['toggles'], b'[Code B]\ntrue\n')
        self.assertEqual(cheats.render(parsed, set()), {})
        self.assertEqual(parsed, original)

    def test_selection_preserves_source_order_and_numeric_words(self):
        parsed = cheats.parse(b'[first]\nffffffff 00000000\n{common}\nAbCdEf01\n[second]\n01234567\n')
        result = cheats.render(parsed, {1, 0})
        self.assertEqual(result['cheat'], b'{common}\nABCDEF01\n[first]\nFFFFFFFF\n00000000\n[second]\n01234567\n')
        self.assertEqual(result['toggles'], b'[first]\ntrue\n[second]\ntrue\n')
        self.assertEqual(cheats.render(parsed, frozenset({0})), cheats.render(parsed, {0}))

    def test_no_master_and_ascii_whitespace_with_unicode_names(self):
        parsed = cheats.parse(' \t[音量]\r\n\t00000000\tFFFFFFFF\r\r[Second]\r00000001'.encode())
        self.assertIsNone(parsed['master'])
        self.assertEqual(cheats.render(parsed, {0})['toggles'], '[音量]\ntrue\n'.encode())

    def test_malformed_sources_are_refused(self):
        invalid = [b'', b' \r\n\t', b'00000000', b'[Empty]', b'{Master}\n00000000',
                   b'[Name\n00000000', b'[Name}\n00000000', b'[Name] 00000000',
                   b'[A]\n00000000\n[B]', b'{M}\n00000000\n{M2}\n00000000\n[A]\n00000000',
                   b'[A]\n0000000', b'[A]\n000000000', b'[A]\n0x000000',
                   b'[A]\n00000000// comment', b'[A]\n00000000 # comment',
                   b'[A]\n0000000000000000', b'[A]\nGGGGGGGG',
                   b'\xef\xbb\xbf[A]\n00000000', b'[A]\n00000000\0',
                   b'[A]\n00000000\v', b'[A]\n00000000\f', b'[A]\n\xff',
                   b'[]\n00000000', b'[   ]\n00000000', b'[A[B]\n00000000',
                   b'[A{B]\n00000000', b'[A]B]\n00000000', b'[A\tB]\n00000000',
                   '[A\u200bB]\n00000000'.encode(), '[A]\n00000000\u00a000000000'.encode(),
                   '[A]\n00000000\u202800000000'.encode()]
        for source in invalid:
            with self.subTest(source=source[:70]), self.assertRaises(ValueError):
                cheats.parse(source)
        for source in ('[A]\n00000000', bytearray(b'[A]\n00000000'), None):
            with self.subTest(type=type(source)), self.assertRaises(ValueError):
                cheats.parse(source)

    def test_duplicate_names_across_all_blocks_are_refused(self):
        for source in (b'[A]\n00000000\n[A]\n00000001',
                       b'{A}\n00000000\n[A]\n00000001'):
            with self.subTest(source=source), self.assertRaises(ValueError):
                cheats.parse(source)

    def test_name_limit_counts_utf8_bytes(self):
        for name in ('A' * 63, '界' * 21):
            self.assertEqual(cheats.parse(('[' + name + ']\n00000000').encode())['codes'][0]['name'], name)
        for name in ('A' * 64, '界' * 21 + 'A'):
            with self.subTest(name=name), self.assertRaises(ValueError):
                cheats.parse(('[' + name + ']\n00000000').encode())

    def test_block_and_code_count_limits(self):
        full = b'[A]\n' + b'00000000\n' * 256
        self.assertEqual(len(cheats.parse(full)['codes'][0]['words']), 256)
        with self.assertRaises(ValueError):
            cheats.parse(full + b'00000000\n')
        source = ''.join(f'[Code {index}]\n00000000\n' for index in range(127)).encode()
        self.assertEqual(len(cheats.parse(source)['codes']), 127)
        with self.assertRaises(ValueError):
            cheats.parse(source + b'[Last]\n00000000\n')

    def test_source_size_limit_is_exclusive(self):
        source = b'[A]\n00000000\n'
        self.assertEqual(cheats.parse(source + b' ' * (65535 - len(source)))['codes'][0], code('A'))
        with self.assertRaises(ValueError):
            cheats.parse(source + b' ' * (65536 - len(source)))

    def test_selected_word_limit_includes_master(self):
        parsed = {'master': code('Master', [0] * 256),
                  'codes': [code(str(index), [index] * 256) for index in range(4)]}
        self.assertIn('cheat', cheats.render(parsed, {0, 1, 2}))
        with self.assertRaises(ValueError):
            cheats.render(parsed, {0, 1, 2, 3})
        self.assertEqual(cheats.render(parsed, set()), {})
        parsed['master'] = None
        self.assertIn('cheat', cheats.render(parsed, {0, 1, 2, 3}))

    def test_forged_parsed_values_are_refused_even_for_empty_selection(self):
        valid = {'master': None, 'codes': [code()]}
        invalid = [None, [], {}, {'codes': [code()]}, dict(valid, extra=True),
                   {'master': None, 'codes': []}, {'master': code(), 'codes': []},
                   {'master': None, 'codes': (code(),)},
                   {'master': [], 'codes': [code()]},
                   {'master': None, 'codes': [dict(code(), extra=True)]},
                   {'master': None, 'codes': [code(words=[])]},
                   {'master': None, 'codes': [code(words=[True])]},
                   {'master': None, 'codes': [code(words=[-1])]},
                   {'master': None, 'codes': [code(words=[1 << 32])]},
                   {'master': None, 'codes': [code(words=[1.0])]},
                   {'master': None, 'codes': [code(words=['00000000'])]},
                   {'master': None, 'codes': [code(words=(0,))]},
                   {'master': None, 'codes': [code(name='A\nB')]},
                   {'master': None, 'codes': [code(name='\ud800')]},
                   {'master': None, 'codes': [code(), code()]},
                   {'master': code(), 'codes': [code()]}]
        for parsed in invalid:
            with self.subTest(parsed=parsed), self.assertRaises(ValueError):
                cheats.render(parsed, set())

    def test_forged_selection_is_refused(self):
        parsed = cheats.parse(SOURCE)
        for enabled in ([0], (0,), {0: True}, None, {True}, {False}, {-1}, {2}, {0.0}, {'0'}):
            with self.subTest(enabled=enabled), self.assertRaises(ValueError):
                cheats.render(parsed, enabled)

    def test_output_size_guard_applies_to_each_derived_file(self):
        parsed = cheats.parse(b'[A]\n00000000\n')
        with mock.patch.object(cheats, 'MAX_FILE_BYTES', 12), self.assertRaises(ValueError):
            cheats.render(parsed, {0})
        # A reduced toggle bound can exceed the normalized one-word source.
        self.assertEqual(len(cheats.render(parsed, {0})['cheat']), 13)
        with mock.patch.object(cheats, '_encoded_block', return_value=b'x'), \
                mock.patch.object(cheats, 'MAX_FILE_BYTES', 9), self.assertRaises(ValueError):
            cheats.render(parsed, {0})


if __name__ == '__main__':
    unittest.main()
