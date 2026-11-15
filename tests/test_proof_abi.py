"""The isolated native proof keeps the released native-mod ABI while adding memory."""
import json
from pathlib import Path
import unittest

ROOT=Path(__file__).resolve().parents[1]
class ProofABI(unittest.TestCase):
    def test_released_services_and_members_keep_their_positions(self):
        lock=json.loads((ROOT/'schema/abi.lock').read_text(encoding='utf-8'))
        released=json.loads((ROOT/'tests/fixtures/abi-0.3.1.lock').read_text(encoding='utf-8'))
        before=released['services']
        self.assertEqual(lock['services'][:len(before)],before)
        for name,members in released['tables'].items():
            with self.subTest(service=name):
                self.assertIn(name,lock['tables'])
                self.assertEqual(lock['tables'][name][:len(members)],members)
        self.assertIn('memory',lock['services'][len(before):])
        old_mod=released['tables']['mod']
        self.assertEqual(lock['tables']['mod'][len(old_mod)],'game_build')
        self.assertEqual(lock['tables']['input'][8:10],['hold_state','release_state'])

if __name__=='__main__': unittest.main()
