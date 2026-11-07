"""The setup routes call only what scripts can call. The routes run only in the game, so a renamed
or removed function in one shows up nowhere else before a game run."""
import re
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))

from gen.schema import load  # noqa: E402

ALIASES = re.compile(r'^[ \t]*local[ \t]+([\w \t,]+?)[ \t]*=[ \t]*([\w \t,]+?)[ \t]*$', re.M)
USE = re.compile(r'\b([A-Za-z_]\w*)\.([A-Za-z_]\w*)\b')


def script_names():
    """Every service's names a script can use: its functions, and on_ and the name of each event."""
    return {service.name: {fn.name for fn in service.functions if service.scripted(fn)} | {f'on_{event.name}' for event in service.events}
            for service in load(ROOT / 'schema').services if service.has_bindings}


def unknown_calls(text, names):
    """`service.name` uses in a script's text, directly or through a local alias, that scripts do not have."""
    text = re.sub(r'--[^\n]*', '', text)
    text = re.sub(r'"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'', '""', text)
    services = {name: name for name in names}
    for left, right in ALIASES.findall(text):
        for alias, service in zip((part.strip() for part in left.split(',')), (part.strip() for part in right.split(','))):
            if service in names:
                services[alias] = service
    found = []
    for owner, member in USE.findall(text):
        service = services.get(owner)
        if service and member not in names[service]:
            found.append(f'{owner}.{member}' if owner == service else f'{owner}.{member} ({service}.{member})')
    return sorted(set(found))


class RouteCalls(unittest.TestCase):
    def test_every_route_calls_only_script_functions(self):
        names = script_names()
        scripts = [path for path in sorted((ROOT / 'tests' / 'routes').rglob('*.lua')) if not path.name.endswith('_test.lua')]
        self.assertTrue(scripts)
        for path in scripts:
            with self.subTest(route=path.relative_to(ROOT).as_posix()):
                self.assertEqual(unknown_calls(path.read_text(encoding='utf-8'), names), [])

    def test_an_old_name_is_found_through_an_alias(self):
        names = script_names()
        text = 'local entity_api, captain_api = entity, captain\nlocal at = entity_api.transform(a)\ncaptain_api.mount(c)\n'
        self.assertEqual(unknown_calls(text, names), ['captain_api.mount (captain.mount)', 'entity_api.transform (entity.transform)'])
        self.assertEqual(unknown_calls('-- entity.transform(a)\nlocal s = "entity.alive"\n', names), [])


if __name__ == '__main__':
    unittest.main()
