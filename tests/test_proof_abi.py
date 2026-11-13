"""The isolated native proof keeps the existing native-mod ABI while adding memory."""
import json
from pathlib import Path
import unittest

ROOT=Path(__file__).resolve().parents[1]
class ProofABI(unittest.TestCase):
    def test_existing_services_and_mod_members_keep_their_positions(self):
        lock=json.loads((ROOT/'schema/abi.lock').read_text(encoding='utf-8'))
        before=['actor','animation','asset','audio','battle','behavior','camera','captain','carry','collision',
                'combat','component','content','creature','datatable','dialog','effects','entity','file','game',
                'gimmicks','hook','hud','input','mod','net','pikmin','player','reflection','save','session',
                'settings','squad','storage','story','text','time','ui','voice','world']
        self.assertEqual(lock['services'][:len(before)],before)
        self.assertEqual(lock['services'][len(before)],'memory')
        old_mod=['id','name','version','version_of','log','framework','restart','state','problem','on_restarted']
        self.assertEqual(lock['tables']['mod'][:len(old_mod)],old_mod)
        self.assertEqual(lock['tables']['mod'][len(old_mod)],'game_build')
        self.assertEqual(lock['tables']['input'][8:10],['hold_state','release_state'])

if __name__=='__main__': unittest.main()
