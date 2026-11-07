"""New services keep public result/handle numbers and do not publish the development ABI."""
import json
import unittest
from pathlib import Path

import yaml

ROOT = Path(__file__).resolve().parents[1]


class ReleasedLayout(unittest.TestCase):
    def test_new_services_do_not_publish_the_development_abi(self):
        after = json.loads((ROOT / 'schema/abi.lock').read_text(encoding='utf-8'))
        self.assertFalse(after['released'])
        self.assertEqual(after['release_version'], '0.2.0')
        self.assertEqual(after['api_version'], 0)

    def test_published_result_and_handle_numbers_remain_prefixes(self):
        before = yaml.safe_load((ROOT / 'tests/fixtures/core-0.1.0.yaml').read_text(encoding='utf-8'))
        after = yaml.safe_load((ROOT / 'schema/core.yaml').read_text(encoding='utf-8'))
        for group in ('results', 'handles'):
            with self.subTest(group=group):
                names = [entry['name'] for entry in before[group]]
                self.assertEqual([entry['name'] for entry in after[group]][:len(names)], names)
