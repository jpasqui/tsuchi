"""Schema names stay usable across the API's supported languages."""
import copy
import sys
import tempfile
import unittest
from pathlib import Path

import yaml

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
from gen import SchemaError, load  # noqa: E402


class SchemaNames(unittest.TestCase):
    CATEGORIES = ('service', 'function', 'argument', 'struct_field', 'event_field',
                  'enum_value', 'flag_value', 'event')
    SAMPLE = {
        'service': 'sample', 'layer': 'core', 'acts_on': 'game', 'lifetime': 'process',
        'brief': 'Describes the sample.', 'description': 'A sample service.',
        'enums': {'Kind': {'open': False, 'doc': 'A sample kind.',
                           'values': [{'name': 'entry', 'value': 0, 'doc': 'An entry.'}]}},
        'flags': {'Options': {'doc': 'Sample options.',
                             'values': [{'name': 'enabled', 'value': 1, 'doc': 'An option.'}]}},
        'structs': {'Record': {'doc': 'A sample record.',
                               'fields': [{'name': 'number', 'type': 'u32', 'doc': 'A number.'}]}},
        'functions': [{'name': 'inspect', 'doc': 'Reads a sample.',
                       'params': [{'name': 'number', 'type': 'u32', 'doc': 'A number.'}],
                       'users': ['sample.first', 'sample.second']}],
        'events': [{'name': 'changed', 'doc': 'Emitted when a sample changes.',
                    'fields': [{'name': 'number', 'type': 'u32', 'doc': 'A number.'}],
                    'users': ['sample.first', 'sample.second']}],
    }

    def named(self, category, name):
        """Load a complete small schema with one public identifier changed."""
        service = copy.deepcopy(self.SAMPLE)
        targets = {
            'service': service,
            'function': service['functions'][0],
            'argument': service['functions'][0]['params'][0],
            'struct_field': service['structs']['Record']['fields'][0],
            'event_field': service['events'][0]['fields'][0],
            'enum_value': service['enums']['Kind']['values'][0],
            'flag_value': service['flags']['Options']['values'][0],
            'event': service['events'][0],
        }
        targets[category]['service' if category == 'service' else 'name'] = name
        with tempfile.TemporaryDirectory() as folder:
            folder = Path(folder)
            core = {'api_version': 0, 'results': [{'name': 'ok', 'doc': 'The call succeeded.'}],
                    'handles': []}
            (folder / 'core.yaml').write_text(yaml.safe_dump(core), encoding='utf-8')
            # safe_dump quotes keyword-like YAML values such as true and false.
            # Windows cannot put a newline in a filename. The service's raw name
            # must be rejected before the loader checks its filename.
            stem = service['service'] if '\n' not in service['service'] else 'sample'
            (folder / (stem + '.yaml')).write_text(
                yaml.safe_dump(service, sort_keys=False), encoding='utf-8')
            return load(folder)

    def test_reserved_words_are_refused_in_every_category(self):
        # Lua-only, JS-only/strict, C-only, C++-only, and shared keyword examples.
        words = ('nil', 'elseif', 'function', 'await', 'instanceof',
                 'implements', 'package', 'restrict', 'typeof_unqual', 'const_cast', 'co_await',
                 'and_eq', 'true', 'false')
        for category in self.CATEGORIES:
            for name in words:
                with self.subTest(category=category, name=name):
                    with self.assertRaisesRegex(SchemaError, f'"{name}" is a reserved word'):
                        self.named(category, name)

    def test_malformed_names_cannot_bypass_keyword_or_global_checks(self):
        for category in self.CATEGORIES:
            for name in ('function\n', 'table\n', 'entry\n', 'entry!', 'entry.name'):
                with self.subTest(category=category, name=name):
                    with self.assertRaisesRegex(SchemaError, 'bad name'):
                        self.named(category, name)

    def test_services_cannot_replace_lua_or_javascript_globals(self):
        for name in ('load', 'print', 'type', 'table', 'math', 'require', 'coroutine',
                     'utf8', 'io', 'os', 'debug', 'eval', 'undefined', 'escape', 'unescape'):
            with self.subTest(name=name):
                with self.assertRaisesRegex(SchemaError, f'"{name}" is a built-in global'):
                    self.named('service', name)

    def test_arguments_and_fields_cannot_hide_lua_globals(self):
        for category in ('argument', 'struct_field', 'event_field'):
            for name in ('load', 'print', 'type', 'table', 'string', 'assert',
                         'require', 'select', 'next', 'pairs', 'pcall', 'warn'):
                with self.subTest(category=category, name=name):
                    with self.assertRaisesRegex(SchemaError, f'"{name}" is a built-in global in Lua'):
                        self.named(category, name)

    def test_function_and_value_names_may_match_globals(self):
        for category in ('function', 'enum_value', 'flag_value', 'event'):
            for name in ('load', 'print', 'type', 'table', 'undefined', 'escape'):
                with self.subTest(category=category, name=name):
                    self.named(category, name)

    def test_javascript_globals_are_allowed_for_arguments_and_fields(self):
        for category in ('argument', 'struct_field', 'event_field'):
            for name in ('eval', 'undefined', 'escape', 'unescape'):
                with self.subTest(category=category, name=name):
                    self.named(category, name)

    def test_javascript_binding_restrictions_are_not_reserved_words(self):
        for category in ('function', 'enum_value', 'flag_value', 'struct_field', 'event_field', 'event'):
            for name in ('eval', 'arguments'):
                with self.subTest(category=category, name=name):
                    self.named(category, name)
        self.named('service', 'arguments')
        self.named('argument', 'arguments')

    def test_contextual_words_and_names_containing_keywords_are_allowed(self):
        for category in self.CATEGORIES:
            for name in ('get', 'set', 'from', 'of', 'override', 'final', 'data_table',
                         'wrapper', 'args', 'my_print', 'function_name', 'load_asset'):
                with self.subTest(category=category, name=name):
                    self.named(category, name)

    def test_builtin_matching_is_case_sensitive(self):
        # JS's Number/Object/Reflect are distinct from these ordinary lowercase names.
        for category in self.CATEGORIES:
            for name in ('number', 'object', 'reflect', 'globalthis', 'array', 'text'):
                with self.subTest(category=category, name=name):
                    self.named(category, name)

    def test_core_names_and_object_membership_are_checked(self):
        import json
        import shutil
        with tempfile.TemporaryDirectory() as name:
            folder = Path(name)
            for source in (ROOT / 'schema').glob('*.yaml'):
                shutil.copyfile(source, folder / source.name)
            core_path = folder / 'core.yaml'
            core = yaml.safe_load(core_path.read_text(encoding='utf-8'))
            for category in ('results', 'handles'):
                old = core[category][-1]['name']
                core[category][-1]['name'] = 'function'
                core_path.write_text(json.dumps(core), encoding='utf-8')
                with self.assertRaisesRegex(SchemaError, 'is a reserved word'):
                    load(folder)
                core[category][-1]['name'] = old
            core['handles'][-1]['object'] = 'false'
            core_path.write_text(json.dumps(core), encoding='utf-8')
            with self.assertRaisesRegex(SchemaError, '"object" is true or false'):
                load(folder)

    def test_the_release_schema_keeps_asset_load_and_existing_services(self):
        schema = load(ROOT / 'schema')
        services = {service.name: service for service in schema.services}
        self.assertIn('load', {fn.name for fn in services['asset'].functions})
        self.assertTrue({'audio', 'behavior', 'combat', 'game', 'hud', 'pikmin',
                         'settings', 'squad', 'storage', 'story', 'text', 'time'} <= services.keys())
        self.assertTrue({'object', 'actor', 'component'} <= schema.object_handles)
        self.assertIn('io', {name for name, _ in schema.results})
        table_params = [param.name for fn in services['datatable'].functions for param in fn.params]
        self.assertIn('data_table', table_params)
        self.assertNotIn('table', table_params)


if __name__ == '__main__':
    unittest.main()
