"""World queries and effects against a made-up engine."""
import unittest
from test_all import ROOT, GEN, BUILD, CORE, BASE, GAME, CXX, EXE, LUA, lua_objects, run, generate, shutil


class WorldEffects(unittest.TestCase):
    def test_calls_cleanup_reentry_and_capacity(self):
        generate.write(GEN)
        program = BUILD / ('world_effects_test' + EXE)
        run([shutil.which('clang++'), *CXX, '-I', GEN / 'include', '-I', GEN / 'framework',
             '-I', CORE, '-I', BASE, '-I', GAME, '-I', GEN / 'lua', '-I', ROOT / 'runtime/lua', '-I', LUA,
             CORE / 'registry.cpp', CORE / 'ids.cpp', GAME / 'holds.cpp',
             GAME / 'effects_backend.cpp', GAME / 'world_backend.cpp',
             GEN / 'framework/effects_service.cpp', GEN / 'framework/world_service.cpp',
             ROOT / 'tests/game/world_effects_test.cpp', *lua_objects(), '-o', program])
        result = run([program])
        self.assertIn('0 failures', result.stdout)

    def test_vector_lists_in_every_binding(self):
        files = generate.write(GEN)
        self.assertIn('TSUCHI_Vec3 *buffer', files['include/tsuchi/world.h'])
        self.assertIn('push_vec3(L, items[i])', files['lua/world_lua.cpp'])
        self.assertIn('push_vec3(ctx, items[i])', files['javascript/world_js.cpp'])
        self.assertIn('Vec3[]', files['javascript/tsuchi.d.ts'])


if __name__ == '__main__':
    unittest.main()
