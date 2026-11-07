"""Lossless unsigned IDs through the real Lua runtime and generated API calls."""
import copy
from pathlib import Path
import shutil
import sys
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
from gen import c_header, load, lua_binding
from gen.schema import Function, Named, Service, Type, Value
from test_all import CXX, EXE, LUA, lua_objects, run


def precision_service():
    field = Value('placement', Type('u64'), 'The full-width placement ID.')
    out = Value('out', Type('u64'), 'The same bits.')
    record = Type('struct', 'IdRecord')
    functions = [Function('capture', 'Returns the same ID.', [Value('value', Type('u64'), 'The ID.')], out, {}, []),
                 Function('roundtrip', 'Returns the same fields.', [Value('value', record, 'The record.')],
                          Value('out', record, 'The record.'), {}, []),
                 Function('with_default', 'Returns the supplied ID or default.',
                          [Value('value', Type('u64'), 'The ID.', default=(1 << 64)-1, has_default=True)], out, {}, []),
                 Function('echo', 'Returns any ID.', [Value('value', Type('u64'), 'The ID.')], out, {}, []),
                 Function('bounded', 'Returns the smaller unsigned value.', [Value('value', Type('u32'), 'The value.')],
                          Value('out', Type('u32'), 'The value.'), {}, [])]
    return Service('precision', 'core', '', 'process', 'Tests unsigned values.', '', [], [],
                   [Named('IdRecord', 'A record.', fields=[field])], functions, [],
                   table_order=[fn.name for fn in functions])


class LuaU64(unittest.TestCase):
    def test_generated_readers_and_writers_share_the_unsigned_helpers(self):
        scalar = Type('u64')
        self.assertEqual(lua_binding.read(Value('id', scalar, ''), 1),
                         'const uint64_t id = tsuchi_lua::check_u64(L, 1);')
        self.assertEqual(lua_binding.push(scalar, 'value'), 'tsuchi_lua::push_u64(L, value);')
        defaulted = Value('id', scalar, '', default=(1 << 64)-1, has_default=True)
        self.assertIn('18446744073709551615ULL', lua_binding.read(defaulted, 2))
        entity = next(service for service in load(ROOT / 'schema').services if service.name == 'entity')
        generated = lua_binding.service_binding(entity)
        self.assertIn('const uint64_t placement = tsuchi_lua::check_u64(L, 1);', generated)
        self.assertIn('tsuchi_lua::push_u64(L, out);', generated)
        self.assertIn('tsuchi_lua::push_u64(L, event->placement);', generated)

    def test_native_backend_receives_exact_bits_from_calls_structs_and_events(self):
        self.assertIsNotNone(shutil.which('clang++'), 'Clang is required')
        schema = load(ROOT / 'schema')
        precision = precision_service()
        schema.services.append(precision)
        schema.host_order.append(precision)
        entity = next(service for service in schema.services if service.name == 'entity')
        opened = copy.copy(schema)
        opened.services = [entity, precision]
        out = ROOT / 'build/lua-u64'
        files = c_header.generate(schema)
        files.update({'lua/entity_lua.cpp': lua_binding.service_binding(entity),
                      'lua/precision_lua.cpp': lua_binding.service_binding(precision),
                      'lua/services_lua.cpp': lua_binding.services_binding(opened)})
        for name, text in files.items():
            path = out / name
            path.parent.mkdir(parents=True, exist_ok=True)
            if not path.exists() or path.read_text(encoding='utf-8') != text:
                path.write_text(text, encoding='utf-8', newline='\n')
        program = out / ('u64_test' + EXE)
        run([shutil.which('clang++'), *CXX, '-I', out / 'include', '-I', ROOT / 'runtime/lua', '-I', LUA,
             ROOT / 'runtime/lua/tsuchi_lua.cpp', *sorted((out / 'lua').glob('*.cpp')),
             ROOT / 'tests/lua/u64_test.cpp', *lua_objects(), '-o', program])
        result = run([program, ROOT / 'tests/lua/u64.lua'])
        self.assertIn('u64 exact native calls, results, structs, events and refusals passed', result.stdout)


if __name__ == '__main__':
    unittest.main()
