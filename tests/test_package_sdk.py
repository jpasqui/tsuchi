"""Author SDK checks use small fixtures and a fresh extracted first-mod build."""
import ast
import hashlib
import json
from pathlib import Path, PureWindowsPath
import shutil
import subprocess
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch
import zipfile

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
import package_sdk


class SDKPackage(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.work = Path(self.temp.name)
        self.root = self.work / 'source'
        for name in package_sdk.RESOURCES:
            target = self.root / 'sdk' / name
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes((ROOT / 'sdk' / name).read_bytes())
        guide = self.root / package_sdk.GUIDE
        guide.parent.mkdir(parents=True, exist_ok=True)
        guide.write_bytes((ROOT / package_sdk.GUIDE).read_bytes())
        (self.root / 'schema').mkdir()
        (self.root / 'schema/core.yaml').write_text('api_version: 3\n', encoding='utf-8')
        (self.root / 'schema/abi.lock').write_text('{"released": true}\n', encoding='utf-8')
        self.headers = {'include/tsuchi/core.h': '#define TSUCHI_API_VERSION 3\n',
                        'include/tsuchi/tsuchi.h': '#include "core.h"\n'}
        self.schema = SimpleNamespace(api_version=3, services=[], handles=[])
        self.load = patch.object(package_sdk, 'load', return_value=self.schema).start()
        self.generate = patch.object(package_sdk.c_header, 'generate', return_value=self.headers).start()
        self.addCleanup(patch.stopall)

    def test_unreleased_lock_is_packaged_only_as_a_prerelease(self):
        archive = self.work / 'tsuchi-sdk.zip'
        for lock in ('{"released": false}\n', '{}\n', 'not json\n'):
            with self.subTest(lock=lock):
                (self.root / 'schema/abi.lock').write_text(lock, encoding='utf-8')
                with self.assertRaisesRegex(ValueError, 'prerelease'):
                    package_sdk.package(archive, self.root)
                self.assertFalse(archive.exists())
        report = package_sdk.package(archive, self.root, prerelease=True)
        self.assertEqual(Path(report['archive']), self.work / 'tsuchi-sdk-prerelease.zip')
        self.assertTrue(Path(report['archive']).is_file())
        self.assertFalse(archive.exists())

    def test_explicit_layout_and_byte_identical_sdk_sources(self):
        files = package_sdk.members(self.root)
        expected = {'sdk/tsuchi.d.ts'} | {'sdk/' + name for name in package_sdk.RESOURCES}
        expected.update('sdk/' + name for name in self.headers)
        expected.update('sdk/examples/olimar.clock/' + name for name in package_sdk.EXAMPLES)
        self.assertEqual(set(files), expected | {package_sdk.INVENTORY})
        for name in package_sdk.RESOURCES:
            self.assertEqual(files['sdk/' + name], (ROOT / 'sdk' / name).read_bytes())
        self.load.assert_called_once_with(self.root.resolve() / 'schema')
        self.generate.assert_called_once_with(self.schema)

    def test_inventory_covers_payload_and_inputs(self):
        files = package_sdk.members(self.root)
        inventory = json.loads(files[package_sdk.INVENTORY])
        self.assertEqual(inventory['api_version'], 3)
        self.assertEqual(set(inventory['files']), set(files) - {package_sdk.INVENTORY})
        for name, entry in inventory['files'].items():
            self.assertEqual(entry, {'bytes': len(files[name]), 'sha256': hashlib.sha256(files[name]).hexdigest()})
        self.assertEqual(inventory['inputs']['schema/abi.lock'],
                         hashlib.sha256((self.root / 'schema/abi.lock').read_bytes()).hexdigest())

    def test_headers_ignore_stale_generated_output(self):
        stale = self.root / 'build/gen/include/tsuchi/tsuchi.h'
        stale.parent.mkdir(parents=True)
        stale.write_text('stale header', encoding='utf-8')
        self.assertEqual(package_sdk.members(self.root)['sdk/include/tsuchi/tsuchi.h'],
                         self.headers['include/tsuchi/tsuchi.h'].encode())
        self.assertEqual(stale.read_text(encoding='utf-8'), 'stale header')

    def test_unexpected_generated_file_refused(self):
        self.generate.return_value = {**self.headers, 'runtime/source.cpp': 'runtime'}
        with self.assertRaisesRegex(ValueError, 'unexpected members'):
            package_sdk.members(self.root)

    def test_concurrent_schema_edit_refused(self):
        def change_schema(folder):
            (folder / 'core.yaml').write_text('api_version: 4\n', encoding='utf-8')
            return self.schema
        self.load.side_effect = change_schema
        with self.assertRaisesRegex(ValueError, 'schema changed'):
            package_sdk.members(self.root)

    def test_exact_first_mod_examples(self):
        source = (ROOT / package_sdk.GUIDE).read_text(encoding='utf-8')
        actual = package_sdk.examples(source)
        files = package_sdk.members(self.root)
        for name, data in actual.items():
            self.assertEqual(files['sdk/examples/olimar.clock/' + name], data)
        self.assertIn(b'tsuchi_mod_start', actual['main.c'])
        self.assertIn(b'time.on_step', actual['main.lua'])

    def test_missing_duplicate_or_changed_manifest_refused(self):
        source = (ROOT / package_sdk.GUIDE).read_text(encoding='utf-8')
        for changed in (source.replace('title="main.c"', 'title="other.c"'),
                        source + '\n```lua title="main.lua"\nprint(1)\n```\n',
                        source.replace('"olimar.clock"', '"other.clock"')):
            with self.subTest(source=changed[-50:]):
                with self.assertRaises(ValueError):
                    package_sdk.examples(changed)

    def test_missing_support_resource_refused_preserving_prior(self):
        archive = self.work / 'sdk.zip'
        archive.write_bytes(b'prior archive')
        (self.root / 'sdk/mod.ld').unlink()
        with self.assertRaisesRegex(ValueError, 'missing or too large'):
            package_sdk.package(archive, self.root)
        self.assertEqual(archive.read_bytes(), b'prior archive')

    def test_private_paths_refused(self):
        values = (PureWindowsPath('C:/', 'Users', 'Owner', 'private', 'source').as_posix(),
                  str(self.root.resolve()), 'private/research/source')
        for value in values:
            (self.root / 'sdk/README.md').write_text(value, encoding='utf-8')
            with self.subTest(value=value):
                with self.assertRaisesRegex(ValueError, 'private path'):
                    package_sdk.members(self.root)

    def test_unsafe_member_paths_and_payload_bounds_refused(self):
        for name, value in (('sdk/../outside', b'x'), ('sdk/a\\b', b'x'),
                            ('sdk/too-large', b'x' * (package_sdk.MAX_MEMBER + 1))):
            with self.subTest(name=name):
                with self.assertRaises(ValueError):
                    package_sdk.validate_payload({name: value}, {name}, self.root)

    def test_linked_sdk_input_refused(self):
        original = package_sdk.linked
        with patch.object(package_sdk, 'linked', side_effect=lambda path: path.name == 'mod.ld' or original(path)):
            with self.assertRaisesRegex(ValueError, 'linked path'):
                package_sdk.members(self.root)

    def test_windows_reparse_attributes_count_as_linked(self):
        with patch.object(Path, 'is_symlink', return_value=False), \
             patch.object(Path, 'lstat', return_value=SimpleNamespace(st_file_attributes=0x400)):
            self.assertTrue(package_sdk.linked(self.root / 'sdk/mod.ld'))

    def test_combined_payload_and_member_count_bounds(self):
        files = {'sdk/one': b'abc', 'sdk/two': b'def'}
        with patch.object(package_sdk, 'MAX_TOTAL', 5):
            with self.assertRaisesRegex(ValueError, 'payload exceeds'):
                package_sdk.validate_payload(files, set(files), self.root)
        with patch.object(package_sdk, 'MAX_MEMBERS', 1):
            with self.assertRaisesRegex(ValueError, 'allowlist'):
                package_sdk.validate_payload(files, set(files), self.root)

    def test_deterministic_archive_and_inventory(self):
        archive = self.work / 'sdk.zip'
        report = package_sdk.package(archive, self.root)
        first = archive.read_bytes()
        self.assertEqual(report, package_sdk.package(archive, self.root))
        self.assertEqual(first, archive.read_bytes())
        with zipfile.ZipFile(archive) as packed:
            self.assertEqual(packed.namelist(), sorted(packed.namelist()))
            for info in packed.infolist():
                self.assertEqual(info.date_time, (1980, 1, 1, 0, 0, 0))
                self.assertEqual(info.external_attr >> 16, 0o100644)

    def test_failed_verification_preserves_prior_archive_and_cleans_pending(self):
        archive = self.work / 'sdk.zip'
        archive.write_bytes(b'prior archive')
        with patch.object(package_sdk, 'verify_archive', side_effect=ValueError('verification failed')):
            with self.assertRaisesRegex(ValueError, 'verification failed'):
                package_sdk.package(archive, self.root)
        self.assertEqual(archive.read_bytes(), b'prior archive')
        self.assertEqual(list(self.work.glob('*.pending.zip')), [])

    def test_failed_atomic_replace_preserves_prior_archive(self):
        archive = self.work / 'sdk.zip'
        archive.write_bytes(b'prior archive')
        with patch.object(package_sdk.os, 'replace', side_effect=OSError('replacement refused')):
            with self.assertRaisesRegex(OSError, 'replacement refused'):
                package_sdk.package(archive, self.root)
        self.assertEqual(archive.read_bytes(), b'prior archive')
        self.assertEqual(list(self.work.glob('*.pending.zip')), [])

    def test_inventory_tampering_refused(self):
        files = package_sdk.members(self.root)
        inventory = json.loads(files[package_sdk.INVENTORY])
        inventory['files']['sdk/build_mod.py']['sha256'] = '0' * 64
        files[package_sdk.INVENTORY] = json.dumps(inventory).encode()
        archive = self.work / 'bad.zip'
        with zipfile.ZipFile(archive, 'w') as packed:
            for name, data in files.items():
                packed.writestr(name, data)
        with self.assertRaisesRegex(ValueError, 'inventory hash'):
            package_sdk.verify_archive(archive, files)

    def test_builder_uses_only_standard_library(self):
        tree = ast.parse((ROOT / 'sdk/build_mod.py').read_text(encoding='utf-8'))
        for node in ast.walk(tree):
            if isinstance(node, ast.Import):
                names = [alias.name.split('.')[0] for alias in node.names]
            elif isinstance(node, ast.ImportFrom):
                names = [(node.module or '').split('.')[0]]
            else:
                continue
            self.assertTrue(set(names) <= sys.stdlib_module_names, names)


class SDKFirstMod(unittest.TestCase):
    def test_real_schema_headers_and_extracted_c_first_mod(self):
        if not shutil.which('clang') or not shutil.which('ld.lld'):
            self.skipTest('Clang and LLD with AArch64 support are needed')
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            archive = Path(package_sdk.package(root / 'sdk.zip', ROOT, prerelease=True)['archive'])
            with zipfile.ZipFile(archive) as packed:
                packed.extractall(root / 'installed')
            installed = root / 'installed'
            sdk = installed / 'sdk'
            self.assertFalse((installed / 'tools').exists())
            built = subprocess.run([sys.executable, '-I', str(sdk / 'build_mod.py'),
                                    str(sdk / 'examples/olimar.clock/main.c'), '--out',
                                    str(installed / 'olimar.clock/mod.nro')],
                                   cwd=installed, capture_output=True, text=True, timeout=60)
            self.assertEqual(built.returncode, 0, built.stdout + built.stderr)
            module = (installed / 'olimar.clock/mod.nro').read_bytes()
            self.assertEqual(module[0x10:0x14], b'NRO0')
            self.assertIn(b'tsuchi_mod_start', module)
            self.assertIn(b'tsuchi_mod_built_for', module)
            self.assertNotIn(str(ROOT).encode(), module)
            inventory = json.loads((sdk / 'inventory.json').read_bytes())
            for name, entry in inventory['files'].items():
                self.assertEqual(hashlib.sha256((installed / name).read_bytes()).hexdigest(), entry['sha256'])

    def test_lua_first_mod_with_existing_language_harness(self):
        suffix = '.exe' if sys.platform == 'win32' else ''
        candidates = (ROOT / 'build' / ('lua_host' + suffix),
                      ROOT.parent / 'tsuchi/build' / ('lua_host' + suffix))
        program = next((path for path in candidates if path.is_file()), None)
        if program is None:
            self.skipTest('The existing Lua host harness has not been built')
        before = """local lines, handler = {}, nil
print = function(line) lines[#lines + 1] = line end
mod = {name = function() return 'Clock' end, version = function() return '1.0.0' end}
time = {on_step = function(fn) assert(handler == nil) handler = fn end}
"""
        after = """
assert(type(handler) == 'function')
handler({number=1})
handler({number=1799})
assert(#lines == 1 and lines[1] == 'Started Clock 1.0.0')
handler({number=1800})
handler({number=3600})
assert(#lines == 3)
assert(lines[2] == 'The world has run for 1 minutes')
assert(lines[3] == 'The world has run for 2 minutes')
return 'first Lua mod passed'
"""
        with tempfile.TemporaryDirectory() as folder:
            archive = Path(package_sdk.package(Path(folder) / 'sdk.zip', ROOT, prerelease=True)['archive'])
            with zipfile.ZipFile(archive) as packed:
                example = packed.read('sdk/examples/olimar.clock/main.lua').decode('utf-8')
            script = Path(folder) / 'first.lua'
            script.write_text(before + example + after, encoding='utf-8')
            result = subprocess.run([str(program), str(script)], capture_output=True, text=True, timeout=10)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(result.stdout, 'first Lua mod passed')


if __name__ == '__main__':
    unittest.main()
