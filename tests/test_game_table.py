"""The table carries the types of game calls and hooks through to their callers."""
import copy
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
import generate
from gen import SchemaError, game_table
import yaml


class GameTable(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        generate.write(ROOT / 'build' / 'gen')

    @unittest.skipUnless(shutil.which('clang++'), 'needs clang++')
    def test_rows_names_and_typed_contracts_share_one_header(self):
        """Rows and game names coexist with typed calls, including C++ keyword keys."""
        raw = yaml.safe_load((ROOT / 'tests' / 'fixtures' / 'game_table.yaml').read_text(encoding='utf-8'))
        raw.setdefault('rows', {})['class'] = ['First_Row', 'Second.Row']
        raw.setdefault('names', {})['class'] = 'Function'
        with tempfile.TemporaryDirectory() as folder:
            directory = Path(folder)
            table_path = directory / 'table.yaml'
            table_path.write_text(yaml.safe_dump(raw), encoding='utf-8')
            table = game_table.load(table_path)
            self.assertEqual(table['rows']['class'], ['First_Row', 'Second.Row'])
            self.assertEqual(table['names']['class'], 'Function')
            self.assertEqual(table['data_signatures']['nav_allocate'], 'void* (*)(int32_t, int32_t)')
            header = game_table.generate(table_path, 'tests/fixtures/game_table.yaml')['framework/game_table.h']
            (directory / 'generated_table.h').write_text(header, encoding='utf-8')
            source = directory / 'combined.cpp'
            source.write_text('''#include "generated_table.h"
    #include <type_traits>
    namespace t = tsuchi::game::table;
    static_assert(sizeof(t::row::class_) / sizeof(t::row::class_[0]) == 2);
    static_assert(t::row::class_[0][0] == 'F');
    static_assert(t::row::class_[1][6] == '.');
    static_assert(t::name::class_[0] == 'F');
    static_assert(std::is_same_v<decltype(t::data::nav_allocate)::Function, void* (*)(int32_t, int32_t)>);
    static_assert(std::is_same_v<decltype(t::primitive_table::is_simulating)::Function, bool (*)(void*, uint64_t)>);
    static_assert(std::is_same_v<decltype(t::slot::save_write)::Function, bool (*)(void*, bool, const char16_t*, int32_t, const void*)>);
    static_assert(std::is_same_v<decltype(t::slot_group::actor_register)::Function, void (*)(uint64_t, bool)>);
    static_assert(std::is_same_v<decltype(t::code_site::life_apply_damage)::Function, void (*)(uint64_t, uint64_t, uint64_t, float, float)>);
    static_assert(std::is_same_v<decltype(t::code_site::actor_pool_take)::Function, uint64_t (*)(uint64_t, uint64_t)>);
    ''', encoding='utf-8')
            result = subprocess.run([shutil.which('clang++'), '-std=c++17', '-Wall', '-Wextra', '-Werror',
                                     '-fsyntax-only', str(source)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            # Each vocabulary keeps its own validation when both appear in the same table.
            for group, value in [('rows', []), ('rows', ['']), ('names', 'Bad.Name'), ('names', ['Function'])]:
                with self.subTest(group=group, value=value):
                    changed = copy.deepcopy(raw)
                    changed[group]['class'] = value
                    table_path.write_text(yaml.safe_dump(changed), encoding='utf-8')
                    with self.assertRaises(SchemaError):
                        game_table.load(table_path)

    def test_check_only_entries_stay_out_of_the_framework(self):
        """The fixture table's entries reach the check mods' staging and never the framework's header."""
        table_path, fixtures = ROOT / 'game' / 'table.yaml', ROOT / 'game' / 'fixtures.yaml'
        extra = yaml.safe_load(fixtures.read_text(encoding='utf-8'))
        product = game_table.load(table_path)
        merged = game_table.load(table_path, fixtures)
        header = game_table.generate(table_path, 'game/table.yaml')['framework/game_table.h']
        for group, entries in extra.items():
            for name in entries:
                with self.subTest(group=group, name=name):
                    self.assertNotIn(name, product[group])
                    self.assertIn(name, merged[group])
                    self.assertNotRegex(header, rf'\b{name}\b')
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / 'fixtures.yaml'
            for changed in ({'data': {'world': 1}}, {'slots': {'extra': {}}}):
                with self.subTest(changed=changed):
                    path.write_text(yaml.safe_dump(changed), encoding='utf-8')
                    with self.assertRaises(SchemaError):
                        game_table.load(table_path, path)

    def test_function_signatures_reject_bad_types_and_arguments(self):
        raw = yaml.safe_load((ROOT / 'tests' / 'fixtures' / 'game_table.yaml').read_text(encoding='utf-8'))
        changes = [
            ('data', 'nav_allocate', 'args', None),
            ('data', 'nav_allocate', 'returns', 'missing'),
            ('slots', 'save_write', 'args', 'pointer'),
            ('slot_groups', 'actor_register', 'returns', ['u32']),
            ('code_sites', 'actor_pool_take', 'args', ['missing']),
        ]
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / 'table.yaml'
            for group, name, key, value in changes:
                with self.subTest(group=group, name=name, key=key):
                    changed = copy.deepcopy(raw)
                    changed[group][name][key] = value
                    path.write_text(yaml.safe_dump(changed), encoding='utf-8')
                    with self.assertRaises(SchemaError):
                        game_table.load(path)
            for key, value in [('args', ['missing']), ('returns', ['u32']), ('extra', 1)]:
                with self.subTest(field='is_simulating', key=key):
                    changed = copy.deepcopy(raw)
                    changed['structs']['primitive_table']['fields']['is_simulating'][key] = value
                    path.write_text(yaml.safe_dump(changed), encoding='utf-8')
                    with self.assertRaises(SchemaError):
                        game_table.load(path)

    @unittest.skipUnless(shutil.which('clang++'), 'needs clang++')
    def test_a_hook_must_match_its_table_contract(self):
        prefix = '''#include "hooks.h"
namespace t = tsuchi::game::table;
using namespace tsuchi;
'''
        cases = [
            ('''uint64_t hook(uint64_t, uint64_t);
void check(TSUCHI_Mod* mod) {
    decltype(t::code_site::actor_pool_take)::Function next = nullptr;
    uint64_t id = 0;
    hook_site(mod, t::code_site::actor_pool_take, &hook, &next, &id);
}''', True),
            ('''uint32_t hook(uint64_t, uint64_t);
void check(TSUCHI_Mod* mod) {
    decltype(t::code_site::actor_pool_take)::Function next = nullptr;
    uint64_t id = 0;
    hook_site(mod, t::code_site::actor_pool_take, &hook, &next, &id);
}''', False),
            ('''void hook(uint64_t, uint64_t, uint64_t, float, float);
void check(TSUCHI_Mod* mod) {
    decltype(t::code_site::life_apply_damage)::Function next = nullptr;
    uint64_t id = 0;
    hook_site(mod, t::code_site::life_apply_damage, &hook, &next, &id);
}''', True),
            ('''void hook(uint64_t, uint64_t, uint64_t, double, float);
void check(TSUCHI_Mod* mod) {
    decltype(t::code_site::life_apply_damage)::Function next = nullptr;
    uint64_t id = 0;
    hook_site(mod, t::code_site::life_apply_damage, &hook, &next, &id);
}''', False),
            ('''uint64_t hook(uint64_t, bool);
void check(TSUCHI_Mod* mod) {
    decltype(t::slot_group::actor_register)::Function next = nullptr;
    uint64_t id = 0;
    hook_group_entry(mod, t::slot_group::actor_register, 0, &hook, &next, &id);
}''', False),
            ('''void hook(void*, bool, const char16_t*, int32_t, const void*);
void check(TSUCHI_Mod* mod) {
    decltype(t::slot::save_write)::Function next = nullptr;
    uint64_t id = 0;
    hook_slot(mod, t::slot::save_write, &hook, &next, 0, &id);
}''', False),
        ]
        with tempfile.TemporaryDirectory() as folder:
            source = Path(folder) / 'contract.cpp'
            for body, valid in cases:
                source.write_text(prefix + body, encoding='utf-8')
                result = self.compile(source, '-fsyntax-only')
                self.assertEqual(result.returncode == 0, valid, result.stdout + result.stderr)

    def compile(self, source, *args):
        return subprocess.run([shutil.which('clang++'), '-std=c++17', '-Wall', '-Wextra', '-Werror',
                               '-I', str(ROOT / 'build' / 'gen' / 'include'),
                               '-I', str(ROOT / 'build' / 'gen' / 'framework'),
                               '-I', str(ROOT / 'framework' / 'base'),
                               '-I', str(ROOT / 'framework' / 'core'), str(source), *map(str, args)],
                              capture_output=True, text=True)

    @unittest.skipUnless(shutil.which('clang++'), 'needs clang++')
    def test_typed_access_refuses_unreadable_memory_and_noncode(self):
        source = '''#include "hooks.h"
#include <assert.h>
#include <type_traits>
namespace t = tsuchi::game::table;
namespace tsuchi::game {
uint64_t allowed = 0, code = 0;
uint64_t main_base() { return allowed; }
bool readable(uint64_t address, uint64_t bytes) { return address && address == allowed && bytes == 8; }
bool executable(uint64_t address) { return address == code; }
}
namespace tsuchi {
TSUCHI_Result hook_wrap(TSUCHI_Mod*, uint64_t, uint64_t, void*, uint32_t, uint64_t*, void**) { return TSUCHI_BUSY; }
TSUCHI_Result hook_code_site(TSUCHI_Mod*, uint32_t, void*, uint64_t*, void**) { return TSUCHI_NOT_READY; }
}
void hook(uint64_t, bool) {}
int main() {
    using namespace tsuchi;
    static_assert(std::is_same_v<decltype(game::method(0, t::primitive_table::is_simulating)), bool (*)(void*, uint64_t)>);
    static_assert(std::is_same_v<decltype(game::global_function(t::data::nav_allocate)), void* (*)(int32_t, int32_t)>);
    assert(!game::method(0, t::primitive_table::is_simulating));
    uint64_t entry = 55;
    game::allowed = reinterpret_cast<uint64_t>(&entry);
    assert(!game::global_function(t::DataFunction<void (*)()>{0}));
    game::code = entry;
    assert(game::global_function(t::DataFunction<void (*)()>{0}) == reinterpret_cast<void (*)()>(entry));
    uint64_t id = 123;
    decltype(t::slot_group::actor_register)::Function next = &hook;
    assert(hook_group_entry(nullptr, t::slot_group::actor_register, t::slot_group::actor_register.table_count,
                            &hook, &next, &id) == TSUCHI_INVALID);
    assert(next == &hook && id == 123);
    assert(hook_group_entry(nullptr, t::slot_group::actor_register, 0, &hook, &next, &id) == TSUCHI_BUSY);
    assert(next == &hook && id == 123);
}
'''
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / 'access.cpp'
            exe = Path(folder) / ('access.exe' if sys.platform == 'win32' else 'access')
            path.write_text(source, encoding='utf-8')
            result = self.compile(path, '-o', exe)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            result = subprocess.run([str(exe)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
