"""A call into a Lua script that comes while the script is running continues its turn."""
import copy
from pathlib import Path
import shutil
import sys
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
from gen import c_header, load, lua_binding
from test_all import CXX, EXE, LUA, lua_objects, run


class LuaTurns(unittest.TestCase):
    def test_nested_calls_share_the_outer_turn(self):
        self.assertIsNotNone(shutil.which('clang++'), 'Clang is required')
        schema = load(ROOT / 'schema')
        entity = next(service for service in schema.services if service.name == 'entity')
        opened = copy.copy(schema)
        opened.services = [entity]
        out = ROOT / 'build/lua-turns'
        files = c_header.generate(schema)
        files['lua/entity_lua.cpp'] = lua_binding.service_binding(entity)
        files['lua/services_lua.cpp'] = lua_binding.services_binding(opened)
        for name, text in files.items():
            path = out / name
            path.parent.mkdir(parents=True, exist_ok=True)
            if not path.exists() or path.read_text(encoding='utf-8') != text:
                path.write_text(text, encoding='utf-8', newline='\n')
        program = out / ('turn_test' + EXE)
        run([shutil.which('clang++'), *CXX, '-I', out / 'include', '-I', ROOT / 'runtime/lua', '-I', LUA,
             ROOT / 'runtime/lua/tsuchi_lua.cpp', *sorted((out / 'lua').glob('*.cpp')),
             ROOT / 'tests/lua/turn_test.cpp', *lua_objects(), '-o', program])
        result = run([program, ROOT / 'tests/lua/turns.lua'])
        self.assertIn('lua nested turns passed', result.stdout)


if __name__ == '__main__':
    unittest.main()
