"""The generated script API through both VMs and the real native hook chain."""
from pathlib import Path
import copy
import shutil
import sys
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
from gen import c_header, js_binding, lua_binding, load, service_glue
from test_all import CXX, EXE, CORE, BASE, LUA, lua_objects, run
import quickjs_build
import test_all

class ScriptHooks(unittest.TestCase):
    def test_lua_and_javascript_through_native_chains(self):
        schema = load(ROOT / 'schema')
        subset = copy.copy(schema)
        subset.services = [s for s in schema.services if s.name == 'hook']
        out = ROOT / 'build/script-hooks-tests'
        files = c_header.generate(schema) | service_glue.generate(subset) | lua_binding.generate(subset) | js_binding.generate(subset)
        for name, text in files.items():
            path = out / name
            path.parent.mkdir(parents=True, exist_ok=True)
            if not path.exists() or path.read_text(encoding='utf-8') != text:
                path.write_text(text, encoding='utf-8', newline='\n')
        fixture = test_all.Generated().fixture()
        program = out / ('script_hooks' + EXE)
        sources = [CORE / (n + '.cpp') for n in ('registry', 'ids', 'hooks', 'dynamic_hooks')]
        sources += [out / 'framework/hook_service.cpp'] + sorted((out / 'lua').glob('*.cpp')) + sorted((out / 'javascript').glob('*.cpp'))
        sources += [ROOT / 'runtime/lua' / (n + '.cpp') for n in ('tsuchi_lua', 'script_hooks')]
        sources += [ROOT / 'runtime/javascript' / (n + '.cpp') for n in ('tsuchi_js', 'script_hooks')]
        sources += [ROOT / 'framework/target/uint128.cpp', ROOT / 'tests/core/script_hooks_test.cpp']
        run([shutil.which('clang++'), *CXX, '-I', out / 'include', '-I', fixture / 'framework', '-I', out / 'framework',
             '-I', CORE, '-I', BASE, '-I', ROOT / 'tests/base', '-I', ROOT / 'runtime/lua', '-I', LUA,
             '-I', ROOT / 'runtime/javascript', '-isystem', quickjs_build.QUICKJS,
             *sources, *lua_objects(), *quickjs_build.objects(), '-o', program])
        result = run([program, ROOT / 'tests/lua/hooks.lua', ROOT / 'tests/javascript/hooks.js'])
        self.assertIn('script hooks passed', result.stdout)

if __name__ == '__main__':
    unittest.main()
