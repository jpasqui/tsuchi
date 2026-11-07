"""Live-check source validation and host stand-in execution, separate from game proof."""
import copy
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
sys.path[:0] = [str(ROOT / 'tools'), str(ROOT / 'sdk')]
from gen import game_table  # noqa: E402
import build_mod  # noqa: E402
import generate  # noqa: E402
import native_hook_fixture as fixture  # noqa: E402
import stage_test_mods  # noqa: E402


def run(command):
    result = subprocess.run(list(map(str, command)), capture_output=True, text=True)
    if result.returncode:
        raise AssertionError(result.stdout + result.stderr)
    return result.stdout


HOST = r'''
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>
#include "tsuchi/tsuchi.h"
#include "native_hook_targets.h"
struct TSUCHI_Mod { unsigned index; bool live, usable; };
struct NH_MemoryInfo {
    uint64_t base, size;
    uint32_t state, attribute, permission, ipc_count, device_count, padding;
};
extern "C" TSUCHI_Result fixture_base(const TSUCHI_Host *, TSUCHI_Mod *);
extern "C" TSUCHI_Result fixture_chain(const TSUCHI_Host *, TSUCHI_Mod *);
extern "C" TSUCHI_Result fixture_observer(const TSUCHI_Host *, TSUCHI_Mod *);
using Handler = void (*)(TSUCHI_InlineContext *);
using Math = float (*)(float, uint64_t, uint64_t, uint64_t);
struct Record { TSUCHI_Mod *mod; unsigned target; Handler handler; bool active; };
alignas(4096) static unsigned char image[4096];
static std::vector<Record> records;
static TSUCHI_Mod mods[] = {{0, false, true}, {1, false, true}, {2, false, true}};
static const char *ids[] = {"check.native-hooks-base", "check.native-hooks-chain", "check.native-hooks-observer"};
static unsigned target, cursor, query_count, probe_calls, failures;
static bool bad_identity, bad_prefix, leak_cleanup;

static void dispatch(TSUCHI_InlineContext *context, unsigned limit) {
    for (unsigned i = limit; i; --i) {
        Record &r = records[i - 1];
        if (!r.active || !r.mod->usable || r.target != target) continue;
        const unsigned saved = cursor;
        cursor = i - 1;
        r.handler(context);
        cursor = saved;
        return;
    }
}
static void next(TSUCHI_InlineContext *context) { dispatch(context, cursor); }
static TSUCHI_Result wrap(TSUCHI_Mod *mod, uint64_t address, uint32_t expected, void *handler, TSUCHI_Hook *out) {
    for (unsigned i = 0; i < 2; ++i) if (address == nh_targets[i].entry + nh_branch_offset) {
        if (expected != nh_targets[i].words[3]) return TSUCHI_BUSY;
        /* A retired invocation may still be using this wrapper's original.
           The fixture must use another wrapper for every new generation. */
        for (const auto &old : records)
            if (old.mod == mod && old.target == i && old.handler == reinterpret_cast<Handler>(handler)) return TSUCHI_INVALID;
        uint32_t word = 0x14000001;
        std::memcpy(image + address, &word, 4);
        records.push_back({mod, i, reinterpret_cast<Handler>(handler), true});
        out->id.id = records.size(); out->original = reinterpret_cast<void *>(&next);
        return TSUCHI_OK;
    }
    return TSUCHI_INVALID;
}
static TSUCHI_Result remove_hook(TSUCHI_Mod *mod, TSUCHI_HookId id) {
    if (!id.id || id.id > records.size()) return TSUCHI_NOT_FOUND;
    Record &r = records[id.id - 1];
    if (r.mod != mod || !r.active) return TSUCHI_NOT_FOUND;
    r.active = false; return TSUCHI_OK;
}
static TSUCHI_Result check(TSUCHI_Mod *, uint64_t at, const void *expected, uint64_t size) {
    if (at > sizeof image || size > sizeof image - at) return TSUCHI_INVALID;
    return std::memcmp(image + at, expected, size) ? TSUCHI_BUSY : TSUCHI_OK;
}
static TSUCHI_Result log(TSUCHI_Mod *mod, const char *text) {
    std::printf("%s %s\n", ids[mod->index], text);
    const size_t size = std::strlen(text);
    if (std::strstr(text, " fail ") || (size >= 5 && std::strcmp(text + size - 5, " fail") == 0)) ++failures;
    return TSUCHI_OK;
}
static TSUCHI_Result version(TSUCHI_Mod *, const char *other, char *buffer, uint64_t capacity, uint64_t *out) {
    for (unsigned i = 0; i < 3; ++i) if (std::strcmp(ids[i], other) == 0 && mods[i].live) {
        if (capacity < 6) return TSUCHI_CAPACITY;
        std::memcpy(buffer, "1.0.0", 6); *out = 5; return TSUCHI_OK;
    }
    return TSUCHI_NOT_FOUND;
}
extern "C" uint64_t nh_host_thread() { return 1; }
extern "C" uint64_t nh_host_query(NH_MemoryInfo *info, uint64_t at) {
    ++query_count;
    const uint64_t base = reinterpret_cast<uint64_t>(image);
    *info = {};
    if (!at) { info->size = base; return 0; }
    if (at == base) { info->base = base; info->size = nh_code_size; info->permission = 5; return 0; }
    return 1;
}
extern "C" float nh_host_call(uint64_t base, unsigned which, float value, uint64_t marker, uint64_t payload, uint64_t mode) {
    ++probe_calls;
    if (base != reinterpret_cast<uint64_t>(image)) return -10000;
    uint64_t x[31] = {}, q[64] = {};
    x[0] = marker; x[1] = payload; x[2] = mode;
    std::memcpy(q, &value, 4);
    TSUCHI_InlineContext context{x, q, 0, 0, 0, 0};
    target = which; dispatch(&context, static_cast<unsigned>(records.size()));
    std::memcpy(&value, q, 4);
    if (x[17]) return reinterpret_cast<Math>(x[17])(value, x[0], x[1], x[2]);
    return which ? std::sin(value) : std::cos(value);
}
int main(int argc, char **argv) {
    leak_cleanup = argc > 1 && std::strcmp(argv[1], "leak") == 0;
    bad_identity = argc > 1 && std::strcmp(argv[1], "identity") == 0;
    bad_prefix = argc > 1 && std::strcmp(argv[1], "prefix") == 0;
    (void)nh_identity;
    records.reserve(32);
    for (unsigned i = 0; i < 2; ++i) std::memcpy(image + nh_targets[i].entry, nh_targets[i].words, 16);
    if (bad_identity) image[0x40] = 1;
    TSUCHI_ModService mod_api{}; mod_api.table_size = sizeof mod_api; mod_api.log = log; mod_api.version_of = version;
    TSUCHI_HookService hook_api{}; hook_api.table_size = sizeof hook_api; hook_api.wrap_inline = wrap;
    hook_api.remove = remove_hook; hook_api.check_code = check;
    TSUCHI_Host host{}; host.table_size = sizeof host; host.mod = &mod_api; host.hook = &hook_api;
    const TSUCHI_Result a = fixture_base(&host, &mods[0]);
    mods[0].live = a == TSUCHI_OK;
    if (bad_identity) {
        if (a != TSUCHI_NOT_READY || !records.empty() || query_count > 3) return 2;
        return 0;
    }
    if (a != TSUCHI_OK) return 3;
    const unsigned before_calls = probe_calls;
    if (bad_prefix) image[nh_targets[0].entry] ^= 1;
    const TSUCHI_Result b = fixture_chain(&host, &mods[1]);
    if (b != TSUCHI_NOT_READY) return 4;
    mods[1].usable = false;
    if (bad_prefix) {
        const TSUCHI_Result observer = fixture_observer(&host, &mods[2]);
        if (observer != TSUCHI_NOT_READY || records.size() != 2 || probe_calls != before_calls) return 8;
        return 0;
    }
    unsigned pending = 0;
    for (auto &record : records) if (record.mod == &mods[1] && record.active) {
        ++pending;
        if (!leak_cleanup) record.active = false;
    }
    if (pending != 1) return 5;
    /* The real hook engine also filters stopped owners. Intentionally retaining
       the record in leak mode must therefore leave stock-output checks passing;
       those checks cannot demonstrate record or trampoline reclamation. */
    const TSUCHI_Result c = fixture_observer(&host, &mods[2]);
    if (c != TSUCHI_OK || query_count != 6) return 6;
    unsigned active = 0; for (const auto &record : records) active += record.active;
    std::printf("host fixture failures=%u active=%u\n", failures, active);
    if (!leak_cleanup && active) return 7;
    return 0;
}
'''


class NativeHookFixture(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.table = game_table.load(ROOT / 'game/table.yaml', ROOT / 'game/fixtures.yaml')

    def test_generated_targets_and_identity_come_from_the_tables(self):
        header = fixture.files(self.table)[fixture.BASE]['native_hook_targets.h'].decode()
        self.assertIn('UINT64_C(0x62ee120)', header)
        self.assertIn('UINT64_C(0x62ee130)', header)
        self.assertEqual(header.count('UINT32_C(0xd61f0220)'), 2)
        for offset, value in self.table['identity']:
            self.assertIn(f'UINT64_C({offset:#x}), UINT64_C({value:#x})', header)
        malformed = copy.deepcopy(self.table)
        malformed['constants']['native_hook_cos_3'] = 0xd65f03c0
        with self.assertRaisesRegex(ValueError, 'BR x17'):
            fixture.files(malformed)
        malformed = copy.deepcopy(self.table)
        malformed['functions']['pikmin_scatter_cos']['address'] = malformed['constants']['code_size'] - 4
        with self.assertRaisesRegex(ValueError, 'outside main code'):
            fixture.files(malformed)

    def qualification_log(self):
        lines = []
        for mod, names in fixture.EXPECTED.items():
            for name in names:
                lines += [f'[{mod}] check {name} planned', f'[{mod}] check {name} pass']
        lines += [f'[tsuchi] mod {fixture.MODS[0]} 1.0.0 running',
                  f'[tsuchi] mod {fixture.MODS[1]} {fixture.EXPECTED_BLOCK}',
                  f'[tsuchi] mod {fixture.MODS[2]} 1.0.0 running']
        return '\n'.join(lines) + '\n'

    def qualification_receipt(self):
        return {'check_mods': dict(zip(fixture.MODS, fixture.GROUPS)), 'checks': copy.deepcopy(fixture.EXPECTED)}

    def test_native_suite_qualification_preserves_raw_rejection(self):
        self.assertEqual(fixture.EXPECTED, stage_test_mods.expected_checks(fixture.MODS))
        text, receipt = self.qualification_log(), self.qualification_receipt()
        result = fixture.qualify(text, receipt)
        self.assertTrue(result['qualified'], result)
        self.assertEqual(result['exit'], 0)
        self.assertEqual(result['problems'], [])
        self.assertEqual(result['raw_verdict'], fixture.read_checks.read(text, receipt['check_mods'], receipt['checks']))
        self.assertEqual(result['raw_verdict']['exit'], 1)
        self.assertEqual(result['raw_verdict']['mistakes'], [fixture.EXPECTED_MISTAKE])
        self.assertEqual(result['expected_rejections'], [{'mod': fixture.MODS[1], 'state': fixture.EXPECTED_BLOCK,
                                                         'generic_mistake': fixture.EXPECTED_MISTAKE}])
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root / 'log.txt').write_text(text, encoding='utf-8', newline='\n')
            (root / 'checks.json').write_text(json.dumps(receipt), encoding='utf-8')
            output = run([sys.executable, ROOT / 'tools/native_hook_fixture.py', root / 'log.txt',
                          '--staged', root / 'checks.json', '--json', root / 'qualification.json'])
            self.assertIn('native hook qualification: PASS', output)
            self.assertIn('raw generic-reader exit: 1', output)
            self.assertEqual(json.loads((root / 'qualification.json').read_text(encoding='utf-8')), result)

    def test_native_suite_qualification_rejects_incomplete_or_unexpected_evidence(self):
        text, receipt = self.qualification_log(), self.qualification_receipt()
        base, chain, observer = fixture.MODS
        block = f'[tsuchi] mod {chain} {fixture.EXPECTED_BLOCK}\n'
        armed = f'[{chain}] check startup_cleanup_armed pass\n'
        observed = f'[{observer}] check startup_cleanup pass\n'
        cases = {
            'missing_armed': '\n'.join(line for line in text.splitlines() if f'[{chain}] check startup_cleanup_armed ' not in line) + '\n',
            'missing_observer_proof': text.replace(f'[{observer}] check startup_cleanup pass\n', ''),
            'missing_rejection': text.replace(block, ''),
            'armed_after_observer': text.replace(armed, '').replace(observed, observed + armed),
            'wrong_rejection': text.replace(fixture.EXPECTED_BLOCK, 'blocked: mod.nro: its start answered unsupported'),
            'running_chain': text.replace(block, f'[tsuchi] mod {chain} 1.0.0 running\n'),
            'base_blocked': text.replace(f'mod {base} 1.0.0 running', f'mod {base} blocked: no runtime'),
            'observer_stopped': text.replace(f'mod {observer} 1.0.0 running', f'mod {observer} not running'),
            'unexpected_block': text + '[tsuchi] mod other.mod blocked: missing dependency\n',
            'failed_check': text.replace(f'[{base}] check registers pass', f'[{base}] check registers fail registers differed'),
            'missing_check': text.replace(f'[{base}] check registers pass\n', ''),
            'silent_base': '\n'.join(line for line in text.splitlines() if not line.startswith(f'[{base}] ')) + '\n',
            'duplicate_pass': text + f'[{chain}] check chain_order pass\n',
            'duplicate_plan': text + f'[{base}] check registers planned\n',
            'duplicate_status': text + block,
            'late_wait': text + f'[{observer}] check cleanup waiting too late\n',
            'extra_check': text + f'[{observer}] check extra planned\n[{observer}] check extra pass\n',
            'malformed_check': text + f'[{base}] check setup success\n',
            'malformed_mod_state': text + f'[tsuchi] mod {base} broken status\n',
            'full': text + '[tsuchi] the log is full: what is written after this is not in the file\n',
            'truncated': text.rstrip('\n'),
            'empty': '',
        }
        for name, changed in cases.items():
            with self.subTest(name=name):
                result = fixture.qualify(changed, receipt)
                self.assertFalse(result['qualified'], result)
                self.assertEqual(result['exit'], 1)
                self.assertTrue(result['problems'])
                self.assertEqual(result['expected_rejections'], [])
        receipts = [None, {}, copy.deepcopy(receipt), copy.deepcopy(receipt), copy.deepcopy(receipt)]
        receipts[2]['check_mods'].pop(chain)
        receipts[3]['check_mods'][chain] = 'native-hooks'
        receipts[4]['checks'][chain].remove('startup_cleanup_armed')
        for changed in receipts:
            with self.subTest(receipt=changed):
                self.assertFalse(fixture.qualify(text, changed)['qualified'])

    def test_explicit_group_stages_three_mods_and_declared_checks(self):
        table = self.table
        all_checks = stage_test_mods.check_mods()
        self.assertEqual(len(set(all_checks.values())), len(all_checks), 'every check mod has a unique receipt group')
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source = root / 'mods'
            for name in fixture.MODS:
                shutil.copytree(stage_test_mods.SOURCE / name, source / name)
            for name in ('check.script', 'check.compiled-rejected', 'check.default'):
                folder = source / name; folder.mkdir()
                manifest = {'id': name, 'name': name, 'version': '1.0.0'}
                if name == 'check.default': manifest['meta'] = {'check': 'default', 'checks': ['ready']}
                (folder / 'mod.json').write_text(json.dumps(manifest), encoding='utf-8')
            (root / 'tests/lua').mkdir(parents=True)
            (root / 'tests/lua/language.lua').write_text('print("probe")', encoding='utf-8')
            (root / 'build').mkdir()
            (root / 'build' / ('lua_host.exe' if os.name == 'nt' else 'lua_host')).write_bytes(b'')
            with patch.object(stage_test_mods, 'ROOT', root), patch.object(stage_test_mods, 'SOURCE', source), \
                    patch.object(game_table, 'load', return_value=table), \
                    patch.object(stage_test_mods.ai_order_fixture, 'files', return_value={}), \
                    patch.object(build_mod, 'build', return_value=1) as build, \
                    patch.object(stage_test_mods.subprocess, 'run', return_value=subprocess.CompletedProcess([], 0, b'probe\n')):
                defaults = stage_test_mods.stage(target=root / 'staged')
                self.assertTrue(all(name not in defaults for name in fixture.MODS))
                selected = stage_test_mods.stage(['native-hooks'], target=root / 'staged')
                self.assertTrue(set(fixture.MODS) <= set(selected))
                self.assertNotIn('check.default', selected)
                self.assertEqual(build.call_count, 3)
                self.assertTrue(all(call.kwargs['define'] == ['NH_LIVE_CHECK=1'] for call in build.call_args_list))
            receipt = json.loads((root / 'staged/checks.json').read_text(encoding='utf-8'))
            self.assertEqual(receipt['check_mods'], dict(zip(fixture.MODS, fixture.GROUPS)))
            self.assertEqual(receipt['checks'], stage_test_mods.expected_checks(fixture.MODS))
            self.assertEqual((root / 'staged/romfs/tsuchi/checks.txt').read_text(encoding='utf-8'), '\n'.join(fixture.GROUPS) + '\n')
            staged = root / 'staged/romfs/tsuchi/mods'
            self.assertEqual((staged / fixture.BASE / 'native_hook_targets.h').read_bytes(), fixture.files(table)[fixture.BASE]['native_hook_targets.h'])
            chain = json.loads((source / fixture.MODS[1] / 'mod.json').read_text(encoding='utf-8'))
            observer = json.loads((source / fixture.MODS[2] / 'mod.json').read_text(encoding='utf-8'))
            self.assertEqual(chain['requires'], {fixture.BASE: '*'})
            self.assertEqual(observer['requires'], {fixture.BASE: '*'})

    @unittest.skipUnless(shutil.which('clang') and shutil.which('clang++') and shutil.which('ld.lld'), 'needs LLVM')
    def test_same_check_sources_build_for_sdk_and_model_stopped_owner_filter(self):
        work = ROOT / 'build/native-hook-fixture'
        work.mkdir(parents=True, exist_ok=True)
        generated = work / 'gen'
        generate.write(generated)
        copied = work / 'mods'
        for name in fixture.MODS:
            shutil.copytree(stage_test_mods.SOURCE / name, copied / name, dirs_exist_ok=True)
        base = copied / fixture.BASE
        header = base / 'native_hook_targets.h'
        header.write_bytes(fixture.files(self.table)[fixture.BASE][header.name])
        with patch.object(build_mod, 'headers', return_value=generated / 'include'):
            for name in fixture.MODS:
                output = work / name / 'mod.nro'
                build_mod.build([copied / name / 'source/main.c'], output, define=['NH_LIVE_CHECK=1'])
                self.assertEqual(output.read_bytes()[0x10:0x14], b'NRO0')
        # Move only test targets and identity to an authored host region. The
        # fixture logic and its declared expectations are unchanged.
        host_copied = work / 'host-mods'
        for name in fixture.MODS:
            shutil.copytree(stage_test_mods.SOURCE / name, host_copied / name, dirs_exist_ok=True)
        host_base = host_copied / fixture.BASE
        host_table = copy.deepcopy(self.table)
        host_table['constants']['code_size'] = 4096
        for index, name in enumerate(('cos', 'sin')):
            host_table['functions'][f'pikmin_scatter_{name}']['address'] = 0x100 + 0x20 * index
        value = 0xcbf29ce484222325
        for _ in range(16): value = value * 0x100000001b3 & ((1 << 64) - 1)
        host_table['identity'] = [(0x40, value)]
        (host_base / header.name).write_bytes(fixture.files(host_table)[fixture.BASE][header.name])
        driver = work / 'driver.cpp'; driver.write_text(HOST, encoding='utf-8', newline='\n')
        objects = []
        for name, symbol in zip(fixture.MODS, ('base', 'chain', 'observer')):
            obj = work / f'{symbol}.o'; objects.append(obj)
            run([shutil.which('clang'), '-std=c11', '-Wall', '-Wextra', '-Werror', '-DNH_HOST_TEST=1',
                 f'-Dtsuchi_mod_start=fixture_{symbol}', '-I', generated / 'include', '-I', ROOT / 'sdk/check',
                 '-c', host_copied / name / 'source/main.c', '-o', obj])
            # Without an explicit live/test define, these sources fail closed
            # with an unsupported entry, never a simulated qualification pass.
            run([shutil.which('clang'), '-std=c11', '-Wall', '-Wextra', '-Werror',
                 '-I', generated / 'include', '-c', host_copied / name / 'source/main.c', '-o', work / f'{symbol}-unused.o'])
        exe = work / ('host.exe' if os.name == 'nt' else 'host')
        run([shutil.which('clang++'), '-std=c++17', '-Wall', '-Wextra', '-Werror', '-I', generated / 'include', '-I', host_base,
             driver, *objects, '-o', exe])
        output = run([exe])
        self.assertIn('host fixture failures=0 active=0', output)
        for name, checks in stage_test_mods.expected_checks(fixture.MODS).items():
            for check in checks:
                self.assertEqual(len(re.findall(rf'^{re.escape(name)} check {check} pass(?: |$)', output, re.M)), 1, output)
        leaked = run([exe, 'leak'])
        self.assertIn('check.native-hooks-observer check startup_cleanup pass', leaked)
        self.assertIn('host fixture failures=0 active=1', leaked)
        identity = run([exe, 'identity'])
        self.assertIn('check.native-hooks-base check setup fail', identity)
        prefix = run([exe, 'prefix'])
        self.assertIn('check.native-hooks-chain check setup fail', prefix)
        self.assertIn('check.native-hooks-observer check setup fail', prefix)
        self.assertNotIn('check.native-hooks-chain check chain_order pass', prefix)
        self.assertNotIn('check.native-hooks-observer check startup_cleanup pass', prefix)


if __name__ == '__main__':
    unittest.main()
