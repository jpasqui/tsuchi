"""Academy and HTTPS additions preserve the ABI of existing compiled mods."""
import json
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[1]


class AcademyABI(unittest.TestCase):
    def test_existing_services_and_members_keep_their_positions(self):
        current = json.loads((ROOT / 'schema/abi.lock').read_text(encoding='utf-8'))
        previous = json.loads((ROOT / 'tests/fixtures/abi-academy-base.lock').read_text(encoding='utf-8'))
        services = previous['services']
        self.assertEqual(current['services'][:len(services)], services)
        for service, members in previous['tables'].items():
            with self.subTest(service=service):
                self.assertEqual(current['tables'][service][:len(members)], members)


if __name__ == '__main__':
    unittest.main()
