"""Release packaging checks use a small ELF, not a game or a framework rebuild."""
import copy
import hashlib
import json
from pathlib import Path, PurePosixPath, PureWindowsPath
import shlex
import struct
import sys
import tempfile
import unittest
from unittest.mock import patch
import zipfile

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
import build_module
import build_patch
from gen import game_table, stubs
import package_release
import schema_identity


def small_elf(extra_symbol=None, payload=b''):
    """Three load segments and the loader metadata checked by build_module."""
    raw = bytearray(0x1800)
    raw[:7] = b'\x7fELF\x02\x01\x01'
    struct.pack_into('<HHIQQQIHHHHHH', raw, 16, 3, 183, 1, 0x40, 0x40, 0x1500,
                     0, 64, 56, 3, 64, 10, 9)
    for index, (flags, offset, address, size, memory) in enumerate(
            ((5, 0x1000, 0, 0x100, 0x100), (4, 0x1100, 0x1000, 0x100, 0x100),
             (6, 0x1200, 0x2000, 0x40, 0x60))):
        struct.pack_into('<IIQQQQQQ', raw, 0x40 + index * 56,
                         1, flags, offset, address, address, size, memory, 0x1000)
    struct.pack_into('<I', raw, 0x1004, 0x20)
    raw[0x1020:0x1024] = b'MOD0'
    struct.pack_into('<i', raw, 0x1024, 0x2000 - 0x20)
    raw[0x1080:0x1080 + len(payload)] = payload
    struct.pack_into('<QQQQ', raw, 0x1200, build_module.DT_INIT, 0x40, 0, 0)
    names = b'\0tsuchi_initialize\0'
    symbols = [('tsuchi_initialize', 0x40)]
    if extra_symbol:
        names += extra_symbol.encode('ascii') + b'\0'
        symbols.append((extra_symbol, 0x80))
    raw[0x1380:0x1380 + len(names)] = names
    for index, (name, address) in enumerate(symbols, 1):
        struct.pack_into('<IBBHQQ', raw, 0x1300 + index * 24,
                         names.index(name.encode('ascii')), 0x12, 0, 1, address, 4)
    descriptions = [('', 0, 0, 0, 0, 0), ('.text', 1, 0, 0x1000, 0x100, 0),
                    ('.rodata', 1, 0x1000, 0x1100, 0x100, 0),
                    ('.data', 1, 0x2000, 0x1200, 0x40, 0),
                    ('.dynamic', 6, 0x2000, 0x1200, 32, 5),
                    ('.dynstr', 3, 0x1000, 0x1100, 1, 0),
                    ('.dynsym', 11, 0x1020, 0x1120, 24, 5),
                    ('.symtab', 2, 0, 0x1300, (len(symbols) + 1) * 24, 8),
                    ('.strtab', 3, 0, 0x1380, len(names), 0),
                    ('.shstrtab', 3, 0, 0x1400, 0, 0)]
    section_names = b'\0' + b''.join(name.encode('ascii') + b'\0' for name, *_ in descriptions[1:])
    raw[0x1400:0x1400 + len(section_names)] = section_names
    for index, (name, kind, address, offset, size, link) in enumerate(descriptions):
        struct.pack_into('<IIQQQQIIQQ', raw, 0x1500 + index * 64,
                         section_names.index(name.encode('ascii')) if name else 0,
                         kind, 0, address, offset, len(section_names) if index == 9 else size,
                         link, 0, 8, 24 if kind in (2, 11) else 0)
    return bytes(raw)


class ReleasePackage(unittest.TestCase):
    def test_all_editions_package_matching_metadata_and_patches(self):
        import process_metadata
        tables = game_table.load_builds(ROOT / 'game/table.yaml')
        self.assembly.side_effect = lambda table, work: self.records
        names = []
        for table in tables:
            text_name, binary_name = build_patch.patch_names(table)
            (self.built / text_name).write_bytes(build_patch.pchtxt(table, self.records).encode('ascii'))
            (self.built / binary_name).write_bytes(build_patch.ips(self.records))
            names.extend((text_name, binary_name))
        self.report['patch']['files'] = sorted(names)
        self.save_report()
        with patch.object(game_table, 'load_builds', return_value=tables):
            members = package_release.members(self.built)
        manifest = json.loads(members['release.json'])
        self.assertNotIn('install.py', members)
        for table, entry in zip(tables, manifest['builds']):
            program = table['constants']['program_id']
            folder = process_metadata.profile(program)[0] + '/Tsuchi/'
            expected = process_metadata.generate(program)
            self.assertEqual(members[folder + 'exefs/main.npdm'], expected)
            self.assertEqual(members[f'console/atmosphere/contents/{program:016X}/exefs/main.npdm'], expected)
            self.assertEqual(entry['metadata_sha256'], package_release.digest(expected))
            self.assertEqual(entry['emulator_folder'], folder.rstrip('/'))
            for candidate in tables:
                present = folder + build_patch.patch_names(candidate)[0] in members
                self.assertEqual(present, candidate['constants']['program_id'] == program)

    def test_code_access_backend_ships_without_local_process_metadata(self):
        self.set_module(small_elf(extra_symbol='tsuchi_native_code_access'))
        members = package_release.members(self.built)
        import process_metadata
        self.assertNotIn('install.py', members)
        self.assertNotIn('native_hook_permissions.py', members)
        self.assertEqual(members['emulator/Tsuchi/exefs/main.npdm'],
                         process_metadata.generate(self.table['constants']['program_id']))
        (self.built / 'exefs/main.npdm').write_bytes(b'local metadata')
        with self.assertRaisesRegex(ValueError, 'process metadata'):
            package_release.members(self.built)
        (self.built / 'exefs/main.npdm').unlink()
        self.report['native_code_access'] = {'sha256': '0' * 64}
        self.save_report()
        with self.assertRaisesRegex(ValueError, 'process metadata'):
            package_release.members(self.built)

    def test_github_user_endpoint_is_not_a_local_user_directory(self):
        package_release.reject_private_paths({'module': b'https://api.github.com/users/'})
        # Joined here so the repository's own check for local paths doesn't take the examples for real ones.
        users, home = b'/' + b'Users/name/repo', b'/' + b'home/name/repo'
        for path in (users, home, b'prefix\0' + users):
            with self.assertRaisesRegex(ValueError, 'private path'):
                package_release.reject_private_paths({'module': path})

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.built = self.root / 'built'
        (self.root / 'schema').mkdir()
        (self.root / 'schema/core.yaml').write_text('api_version: 0\n', encoding='utf-8')
        (self.root / 'schema/actor.yaml').write_text('service: actor\n', encoding='utf-8')
        (self.root / 'schema/abi.lock').write_text('{"released": true}\n', encoding='utf-8')
        (self.root / 'tools').mkdir()
        (self.root / 'tools/native_hook_permissions.py').write_bytes((ROOT / 'tools/native_hook_permissions.py').read_bytes())
        (self.root / 'tools/Tsuchi-ThirdPartyNotices.txt').write_bytes((ROOT / 'tools/Tsuchi-ThirdPartyNotices.txt').read_bytes())
        (self.root / 'game').mkdir()
        self.table_bytes = (ROOT / 'game/table.yaml').read_bytes()
        (self.root / 'game/table.yaml').write_bytes(self.table_bytes)
        self.table = game_table.load(self.root / 'game/table.yaml')
        self.docs = self.root / 'site/src/content/docs/players'
        self.docs.mkdir(parents=True)
        (self.docs / 'install-console.md').write_text('---\ntitle: Console install\n---\n\nCopy `console`.\n', encoding='utf-8')
        (self.docs / 'install-tsuchi.mdx').write_text(
            "---\ntitle: Emulator install\n---\n\nimport { Steps } from '@astrojs/starlight/components';\n\n<Steps>\n\n1. Copy `emulator/Tsuchi`.\n\n</Steps>\n", encoding='utf-8')
        (self.built / 'exefs').mkdir(parents=True)
        (self.built / 'ips').mkdir()
        self.patch_files = ['exefs/' + self.table['version'] + '.pchtxt',
                            'ips/' + self.table['build_id'].upper() + '.ips']
        self.records = [(0x1000, b'\x00\x00\x00\x14'), (0x2000, b'patch words!')]
        (self.built / self.patch_files[0]).write_bytes(build_patch.pchtxt(self.table, self.records).encode('ascii'))
        (self.built / self.patch_files[1]).write_bytes(build_patch.ips(self.records))
        self.set_module(small_elf())
        root_patch = patch.object(package_release, 'ROOT', self.root)
        root_patch.start()
        self.addCleanup(root_patch.stop)

        def records(table, work):
            self.assertEqual(table, self.table)
            self.assertFalse(work == self.built or self.built in work.parents)
            (work / 'assembly-marker').write_bytes(b'scratch only')
            return self.records
        assembly_patch = patch.object(build_patch, 'records', side_effect=records)
        self.assembly = assembly_patch.start()
        self.addCleanup(assembly_patch.stop)

    def set_module(self, elf):
        loads, sections, symbols = build_module.read_elf(elf)
        memory = build_module.check(elf, loads, sections, symbols)
        module = build_module.module_file(elf, loads, sections)
        (self.built / 'tsuchi.elf').write_bytes(elf)
        (self.built / 'exefs/subsdk9').write_bytes(module)
        self.report = dict(file='exefs/subsdk9', bytes=len(module), memory_bytes=memory,
                           sha256=package_release.digest(module), elf_sha256=package_release.digest(elf),
                           selfcheck=False, game_table_sha256=package_release.digest(self.table_bytes),
                           **schema_identity.capture(self.root),
                           patch=dict(files=self.patch_files, hash=f'{stubs.patch_hash(self.table):016x}'))
        self.save_report()

    def save_report(self):
        (self.built / 'report.json').write_text(json.dumps(self.report), encoding='utf-8')

    def changed_module(self, offset, update_report=False):
        path = self.built / 'exefs/subsdk9'
        data = bytearray(path.read_bytes())
        data[offset] ^= 1
        path.write_bytes(data)
        if update_report:
            self.report['sha256'] = package_release.digest(data)
            self.save_report()

    def test_every_subsdk_slot_packages_only_the_reported_module(self):
        module = (self.built / 'exefs/subsdk9').read_bytes()
        (self.docs / 'install-console.md').write_text('# Install\n\nCopy `exefs/subsdk9`.\n', encoding='utf-8')
        (self.docs / 'install-tsuchi.mdx').write_text('# Install\n\nReplace `subsdk9`.\n', encoding='utf-8')
        for slot in range(10):
            with self.subTest(slot=slot):
                relative = 'exefs/subsdk' + str(slot)
                (self.built / relative).write_bytes(module)
                self.report['file'] = relative
                self.save_report()
                members = package_release.members(self.built)
                manifest = json.loads(members['release.json'])
                self.assertEqual(manifest['module_file'], relative)
                modules = {name for name in members if name.rsplit('/', 1)[-1].startswith('subsdk')}
                self.assertEqual(modules, {'emulator/Tsuchi/' + relative,
                    'console/atmosphere/contents/' + package_release.TITLE + '/' + relative})
                self.assertTrue(all(members[name] == module for name in modules))
                for guide in ('INSTALL-CONSOLE.md', 'INSTALL-EMULATOR.md'):
                    text = members[guide].decode('utf-8')
                    self.assertIn('subsdk' + str(slot), text)
                    if slot != 9:
                        self.assertNotIn('subsdk9', text)
                        self.assertIn('remove the previous Tsuchi module', text)
                        self.assertIn('Keep every foreign module at its original filename', text)
                        self.assertIn('--tsuchi-slot ' + str(slot), text)

    def test_invalid_module_paths_are_refused_before_files_are_read(self):
        for name in (None, [], 'exefs/subsdk10', 'exefs/subsdk-1', 'exefs/subsdk09',
                     'exefs/Subsdk8', 'exefs/main', '../subsdk8', 'exefs/subsdk8/extra',
                     'exefs/subsdk8\n', 'exefs\\subsdk8'):
            with self.subTest(name=name):
                self.report['file'] = name
                self.save_report()
                with self.assertRaisesRegex(ValueError, 'Production module path'):
                    package_release.members(self.built)
        self.assembly.assert_not_called()

    def test_module_build_validates_names_before_work_and_can_emit_another_slot(self):
        old_module = (self.built / 'exefs/subsdk9').read_bytes()
        for name in (None, 8, '', 'subsdk10', 'subsdk09', 'Subsdk8', '../subsdk8', 'subsdk8\n'):
            with self.subTest(name=name), patch.object(build_module.shutil, 'which') as which:
                with self.assertRaisesRegex(ValueError, 'module name must be'):
                    build_module.build(self.built, name=name)
                which.assert_not_called()

        def run(command):
            if command[0] == 'linker':
                arguments = shlex.split(Path(str(command[1])[1:]).read_text(encoding='utf-8'))
                Path(arguments[arguments.index('-o') + 1]).write_bytes(small_elf())

        with patch.object(build_module, 'ROOT', self.root), \
                patch.object(build_module, 'FRAMEWORK', self.root / 'framework'), \
                patch.object(build_module.shutil, 'which', side_effect=['clang', 'linker']), \
                patch.object(build_module.generate, 'write'), \
                patch.object(build_module, 'run', side_effect=run), \
                patch.object(build_patch, 'build', return_value={'files': []}):
            report = build_module.build(self.built, name='subsdk8')
        self.assertEqual(report['file'], 'exefs/subsdk8')
        self.assertEqual((self.built / report['file']).read_bytes(), old_module)
        self.assertEqual({path.name for path in (self.built / 'exefs').glob('subsdk*')}, {'subsdk8'})
        self.assertEqual(build_module.module_output_preflight(self.built)['file'], 'exefs/subsdk8')

    def test_ordinary_rebuild_retires_only_its_own_process_metadata(self):
        def run(command):
            if command[0] == 'linker':
                arguments = shlex.split(Path(str(command[1])[1:]).read_text(encoding='utf-8'))
                Path(arguments[arguments.index('-o') + 1]).write_bytes(small_elf())

        def ordinary_build():
            with patch.object(build_module, 'ROOT', self.root), \
                    patch.object(build_module, 'FRAMEWORK', self.root / 'framework'), \
                    patch.object(build_module.shutil, 'which', side_effect=['clang', 'linker']), \
                    patch.object(build_module.generate, 'write'), \
                    patch.object(build_module, 'run', side_effect=run), \
                    patch.object(build_patch, 'build', return_value={'files': []}):
                return build_module.build(self.built, name='subsdk9')

        metadata = self.built / 'exefs' / 'main.npdm'
        # Metadata a build of this folder did not write is kept, and nothing is built.
        metadata.write_bytes(b'foreign metadata')
        before = {str(path): path.read_bytes() for path in self.built.rglob('*') if path.is_file()}
        with patch.object(build_module.shutil, 'which') as which:
            with self.assertRaisesRegex(ValueError, 'process metadata no build of it wrote'):
                build_module.build(self.built, name='subsdk9')
            which.assert_not_called()
        self.assertEqual(before, {str(path): path.read_bytes() for path in self.built.rglob('*') if path.is_file()})
        # The metadata a native-hook build recorded is retired by an ordinary build.
        metadata.write_bytes(b'native hook metadata')
        self.report['native_code_access'] = {'sha256': hashlib.sha256(b'native hook metadata').hexdigest()}
        self.save_report()
        report = ordinary_build()
        self.assertNotIn('native_code_access', report)
        self.assertFalse(metadata.exists())

    def test_module_output_unknown_slots_refuse_before_any_build_work(self):
        for name in ('subsdk7', 'subsdk8'):
            with self.subTest(name=name):
                unexpected = self.built / 'exefs' / name
                unexpected.write_bytes(b'NSO0foreign module must survive')
                before = {str(path): path.read_bytes() for path in self.built.rglob('*') if path.is_file()}
                with patch.object(build_module.shutil, 'which') as which:
                    with self.assertRaisesRegex(ValueError, 'unowned slots'):
                        build_module.build(self.built, name='subsdk8')
                    which.assert_not_called()
                self.assertEqual(before, {str(path): path.read_bytes() for path in self.built.rglob('*') if path.is_file()})
                unexpected.unlink()

    def test_module_output_changed_previous_module_and_invalid_report_refuse_without_mutation(self):
        old = self.built / 'exefs/subsdk9'
        original = old.read_bytes()
        old.write_bytes(original[:-1] + bytes([original[-1] ^ 1]))
        before = old.read_bytes()
        with self.assertRaisesRegex(ValueError, 'previous module output is missing or changed'):
            build_module.build(self.built, name='subsdk8')
        self.assertEqual(old.read_bytes(), before)
        old.write_bytes(original)
        report = self.built / 'report.json'
        for invalid in ('{', '[]', '{}', json.dumps({**self.report, 'file': '../subsdk9'}),
                        json.dumps({**self.report, 'bytes': True}), json.dumps({**self.report, 'sha256': 'unknown'})):
            with self.subTest(report=invalid):
                report.write_text(invalid, encoding='utf-8')
                before = {str(path): path.read_bytes() for path in self.built.rglob('*') if path.is_file()}
                with patch.object(build_module.shutil, 'which') as which:
                    with self.assertRaisesRegex(ValueError, 'invalid previous module build report'):
                        build_module.build(self.built, name='subsdk9')
                    which.assert_not_called()
                self.assertEqual(before, {str(path): path.read_bytes() for path in self.built.rglob('*') if path.is_file()})

    def test_module_output_without_report_refuses_and_cleanup_rechecks_owned_bytes(self):
        previous = build_module.module_output_preflight(self.built)
        self.assertEqual(previous['file'], 'exefs/subsdk9')
        new = self.built / 'exefs/subsdk8'
        new.write_bytes(b'NSO0new module')
        old = self.built / previous['file']
        old.write_bytes(b'NSO0replacement must survive')
        with self.assertRaisesRegex(ValueError, 'previous module output is missing or changed'):
            build_module.remove_previous_module(self.built, 'subsdk8', previous)
        self.assertEqual(old.read_bytes(), b'NSO0replacement must survive')
        self.assertEqual(new.read_bytes(), b'NSO0new module')
        (self.built / 'report.json').unlink()
        with self.assertRaisesRegex(ValueError, 'no ownership report'):
            build_module.module_output_preflight(self.built)
        self.assertEqual(old.read_bytes(), b'NSO0replacement must survive')

    def test_exact_inventory_hashes_reproducible_zip_and_read_only_build(self):
        (self.built / 'game.xci').write_bytes(b'game input must stay out')
        (self.built / 'private').mkdir()
        (self.built / 'private/secret.txt').write_bytes(b'private input must stay out')
        snapshot = {p.relative_to(self.built).as_posix(): p.read_bytes() for p in self.built.rglob('*') if p.is_file()}
        first, second = self.root / 'first.zip', self.root / 'second.zip'
        a = package_release.package(self.built, first)
        b = package_release.package(self.built, second)
        self.assertEqual(first.read_bytes(), second.read_bytes())
        self.assertEqual((a.pop('archive'), b.pop('archive')), (str(first), str(second)))
        self.assertEqual(a, b)
        self.assertEqual(a['archive_sha256'], package_release.digest(first.read_bytes()))
        with zipfile.ZipFile(first) as archive:
            expected = {'emulator/Tsuchi/exefs/subsdk9', 'emulator/Tsuchi/' + self.patch_files[0],
                        'console/atmosphere/contents/' + package_release.TITLE + '/exefs/subsdk9',
                        'console/atmosphere/exefs_patches/Tsuchi/' + Path(self.patch_files[1]).name,
                        'INSTALL-CONSOLE.md', 'INSTALL-EMULATOR.md', 'THIRD-PARTY-NOTICES.txt', 'release.json',
                        'emulator/Tsuchi/exefs/main.npdm',
                        'console/atmosphere/contents/' + package_release.TITLE + '/exefs/main.npdm'}
            self.assertEqual(set(archive.namelist()), expected)
            self.assertEqual(a['files'], 10)
            self.assertEqual(archive.namelist(), sorted(expected))
            manifest = json.loads(archive.read('release.json'))
            self.assertEqual(set(manifest['files']), expected - {'release.json'})
            self.assertEqual(manifest['elf_sha256'], self.report['elf_sha256'])
            self.assertEqual(manifest['game_table_sha256'], self.report['game_table_sha256'])
            self.assertEqual(manifest['patch_hash'], self.report['patch']['hash'])
            self.assertFalse(manifest['selfcheck'])
            self.assertEqual(manifest['api_version'], self.report['api_version'])
            self.assertEqual(manifest['schema_inputs'], self.report['schema_inputs'])
            self.assertEqual(archive.read('emulator/Tsuchi/exefs/subsdk9'),
                             archive.read('console/atmosphere/contents/' + package_release.TITLE + '/exefs/subsdk9'))
            for name, value in manifest['files'].items():
                self.assertEqual(value, dict(bytes=len(archive.read(name)), sha256=package_release.digest(archive.read(name))))
            for info in archive.infolist():
                self.assertEqual(info.date_time, (1980, 1, 1, 0, 0, 0))
                self.assertEqual(info.external_attr >> 16, 0o100644)
            self.assertNotIn(b'<Steps>', archive.read('INSTALL-EMULATOR.md'))
        self.assertEqual(snapshot, {p.relative_to(self.built).as_posix(): p.read_bytes()
                                    for p in self.built.rglob('*') if p.is_file()})
        for call in self.assembly.call_args_list:
            self.assertFalse(call.args[1].exists(), 'package patch scratch is removed')

    def test_unreleased_lock_is_packaged_only_as_a_prerelease(self):
        (self.root / 'schema/abi.lock').write_text('{"released": false}\n', encoding='utf-8')
        self.set_module(small_elf())
        archive = self.root / 'tsuchi.zip'
        with self.assertRaisesRegex(ValueError, 'prerelease'):
            package_release.package(self.built, archive)
        self.assertFalse(archive.exists())
        report = package_release.package(self.built, archive, prerelease=True)
        self.assertEqual(Path(report['archive']), self.root / 'tsuchi-prerelease.zip')
        self.assertTrue(Path(report['archive']).is_file())
        self.assertFalse(archive.exists())
        named = package_release.package(self.built, self.root / 'tsuchi-prerelease-2.zip', prerelease=True)
        self.assertEqual(Path(named['archive']), self.root / 'tsuchi-prerelease-2.zip')

    def test_release_version_comes_from_the_lock(self):
        self.assertNotIn('release_version', json.loads(package_release.members(self.built)['release.json']))
        (self.root / 'schema/abi.lock').write_text('{"released": true, "release_version": "0.1.0"}\n', encoding='utf-8')
        self.set_module(small_elf())
        manifest = json.loads(package_release.members(self.built)['release.json'])
        self.assertEqual(manifest['release_version'], '0.1.0')
        for bad in ('"0.1"', '"01.0.0"', '"v0.1.0"', '"0.1.0-beta"', '1'):
            (self.root / 'schema/abi.lock').write_text('{"released": true, "release_version": ' + bad + '}\n', encoding='utf-8')
            self.set_module(small_elf())
            with self.assertRaisesRegex(ValueError, 'release_version'):
                package_release.members(self.built)

    def test_packaged_lua_notice_contains_complete_linked_license(self):
        header = (build_module.LUA / 'lua.h').read_text(encoding='utf-8')
        block = header.split('/******************************************************************************\n* Copyright', 1)[1]
        block = 'Copyright' + block.split('\n******************************************************************************/', 1)[0]
        expected = '\n'.join(line[2:] if line.startswith('* ') else '' if line == '*' else line
                             for line in block.splitlines()) + '\n'
        notice = package_release.members(self.built)['THIRD-PARTY-NOTICES.txt'].decode('utf-8')
        self.assertIn('Lua 5.4.7', notice)
        self.assertIn(expected, notice)
        self.assertEqual(notice.count(expected), 1)
        self.assertNotIn('third_party/', notice)
        self.assertNotIn('src/lua.h', notice)

    def test_archive_includes_tls_license_and_certificate_source_notices(self):
        path = self.root / 'network.zip'
        package_release.package(self.built, path)
        with zipfile.ZipFile(path) as archive:
            notice = archive.read('THIRD-PARTY-NOTICES.txt').decode('utf-8')
        self.assertIn('Mbed TLS 3.6.7', notice)
        self.assertIn('Apache-2.0', notice)
        self.assertIn('Mozilla CA certificate store', notice)
        self.assertIn('https://curl.se/docs/caextract.html', notice)
        self.assertIn('MPL-2.0', notice)
        self.assertEqual(notice, (ROOT / 'tools/Tsuchi-ThirdPartyNotices.txt').read_bytes().decode('utf-8'))

    def test_nonproduction_or_missing_selfcheck_report_is_refused(self):
        for value in (True, None, 0, 'false'):
            with self.subTest(value=value):
                self.report['selfcheck'] = value
                self.save_report()
                with self.assertRaisesRegex(ValueError, 'selfcheck=false'):
                    package_release.members(self.built)
        self.assembly.assert_not_called()

    def test_native_selfcheck_symbols_are_refused_even_if_report_says_production(self):
        for symbol in ('_ZN6tsuchi6target15selfcheck_startEv', '_ZN6tsuchi6target15selfcheck_frameEv'):
            with self.subTest(symbol=symbol):
                self.set_module(small_elf(extra_symbol=symbol))
                with self.assertRaisesRegex(ValueError, 'native selfchecks'):
                    package_release.members(self.built)
        self.assembly.assert_not_called()

    def test_stale_table_hash_is_refused(self):
        self.report['game_table_sha256'] = 'stale'
        self.save_report()
        with self.assertRaisesRegex(ValueError, 'another game table'):
            package_release.members(self.built)
        self.assembly.assert_not_called()

    def test_stale_api_binary_is_refused_with_the_current_game_table(self):
        (self.root / 'schema/actor.yaml').write_text('service: entity\n', encoding='utf-8')
        self.assertEqual(self.report['game_table_sha256'], package_release.digest(self.table_bytes))
        with self.assertRaisesRegex(ValueError, 'another API schema'):
            package_release.members(self.built)
        self.assembly.assert_not_called()

    def test_missing_or_incompatible_api_receipt_is_refused(self):
        original = copy.deepcopy(self.report)
        for key in ('api_version', 'schema_inputs'):
            with self.subTest(key=key):
                self.report = copy.deepcopy(original)
                del self.report[key]
                self.save_report()
                with self.assertRaisesRegex(ValueError, 'another API schema'):
                    package_release.members(self.built)
        self.assembly.assert_not_called()

    def test_module_build_refuses_generation_and_compile_schema_drift_before_final_outputs(self):
        for phase in ('generation', 'compile'):
            with self.subTest(phase=phase):
                (self.root / 'schema/actor.yaml').write_text('service: actor\n', encoding='utf-8')
                before = {name: (self.built / name).read_bytes()
                          for name in ('tsuchi.elf', 'exefs/subsdk9', 'report.json')}

                def mutate():
                    (self.root / 'schema/actor.yaml').write_text('service: entity\n', encoding='utf-8')

                def generate(folder):
                    if phase == 'generation':
                        mutate()

                def run(command):
                    if command[0] == 'linker':
                        self.assertEqual(len(command), 2)
                        self.assertTrue(str(command[1]).startswith('@'))
                        arguments = shlex.split(Path(str(command[1])[1:]).read_text(encoding='utf-8'))
                        self.assertIn('-shared', arguments)
                        self.assertIn('--no-undefined', arguments)
                        Path(arguments[arguments.index('-o') + 1]).write_bytes(small_elf())
                        if phase == 'compile':
                            mutate()

                with patch.object(build_module, 'ROOT', self.root), \
                        patch.object(build_module, 'FRAMEWORK', self.root / 'framework'), \
                        patch.object(build_module.shutil, 'which', side_effect=['clang', 'linker']), \
                        patch.object(build_module.generate, 'write', side_effect=generate), \
                        patch.object(build_module, 'run', side_effect=run) as commands:
                    with self.assertRaisesRegex(ValueError, 'API schema changed during module build'):
                        build_module.build(self.built)
                    if phase == 'generation':
                        commands.assert_not_called()
                self.assertEqual(before, {name: (self.built / name).read_bytes() for name in before})

    def test_wrong_module_or_patch_identity_is_refused(self):
        original = copy.deepcopy(self.report)
        for change in ('missing_module', 'patch_hash', 'patch_files', 'missing_patch'):
            with self.subTest(change=change):
                self.report = copy.deepcopy(original)
                if change == 'missing_module': self.report['file'] = 'exefs/subsdk8'
                elif change == 'patch_hash': self.report['patch']['hash'] = 'stale'
                elif change == 'patch_files': self.report['patch']['files'] = ['exefs/other.pchtxt']
                else: self.report['patch'] = None
                self.save_report()
                with self.assertRaisesRegex(ValueError, 'identity disagree'):
                    package_release.members(self.built)
        self.assembly.assert_not_called()

    def test_changed_module_bytes_are_refused(self):
        self.changed_module(-1)
        with self.assertRaisesRegex(ValueError, 'differs from its build report'):
            package_release.members(self.built)

    def test_changed_module_with_updated_report_does_not_reproduce(self):
        self.changed_module(-1, update_report=True)
        with self.assertRaisesRegex(ValueError, 'does not reproduce'):
            package_release.members(self.built)

    def test_another_elf_build_id_is_refused(self):
        self.changed_module(0x40, update_report=True)
        with self.assertRaisesRegex(ValueError, 'not bound to the linked ELF'):
            package_release.members(self.built)

    def test_stale_reported_elf_hash_is_refused(self):
        self.report['elf_sha256'] = 'stale'
        self.save_report()
        with self.assertRaisesRegex(ValueError, 'not bound to the linked ELF'):
            package_release.members(self.built)

    def test_invalid_elf_is_refused_even_with_matching_digest(self):
        path = self.built / 'tsuchi.elf'
        path.write_bytes(b'not an ELF')
        module_path = self.built / 'exefs/subsdk9'
        data = bytearray(module_path.read_bytes())
        data[0x40:0x60] = hashlib.sha256(path.read_bytes()).digest()
        module_path.write_bytes(data)
        self.report.update(sha256=package_release.digest(data), elf_sha256=package_release.digest(path.read_bytes()))
        self.save_report()
        with self.assertRaisesRegex(ValueError, 'ELF validation failed'):
            package_release.members(self.built)

    def test_truncated_elf_is_refused_as_a_validation_error(self):
        path = self.built / 'tsuchi.elf'
        path.write_bytes(path.read_bytes()[:0x100])
        module_path = self.built / 'exefs/subsdk9'
        data = bytearray(module_path.read_bytes())
        data[0x40:0x60] = hashlib.sha256(path.read_bytes()).digest()
        module_path.write_bytes(data)
        self.report.update(sha256=package_release.digest(data), elf_sha256=package_release.digest(path.read_bytes()))
        self.save_report()
        with self.assertRaisesRegex(ValueError, 'ELF validation failed'):
            package_release.members(self.built)

    def test_older_report_still_binds_elf_and_records_its_hash(self):
        del self.report['elf_sha256']
        self.save_report()
        result = package_release.members(self.built)
        manifest = json.loads(result['release.json'])
        self.assertEqual(manifest['elf_sha256'], package_release.digest((self.built / 'tsuchi.elf').read_bytes()))

    def test_wrong_memory_size_is_refused(self):
        self.report['memory_bytes'] += 1
        self.save_report()
        with self.assertRaisesRegex(ValueError, 'does not reproduce'):
            package_release.members(self.built)

    def test_each_patch_format_is_regenerated_and_checked(self):
        for name in self.patch_files:
            with self.subTest(name=name):
                path = self.built / name
                original = path.read_bytes()
                path.write_bytes(original + b'changed')
                with self.assertRaisesRegex(ValueError, 'Patch bytes differ'):
                    package_release.members(self.built)
                path.write_bytes(original)

    def test_validation_failure_preserves_existing_archive(self):
        archive = self.root / 'release.zip'
        archive.write_bytes(b'previous release')
        self.report['selfcheck'] = True
        self.save_report()
        with self.assertRaises(ValueError):
            package_release.package(self.built, archive)
        self.assertEqual(archive.read_bytes(), b'previous release')
        self.assertEqual(list(self.root.glob('*.pending.zip')), [])

    def test_archive_verification_failure_preserves_existing_archive(self):
        archive = self.root / 'release.zip'
        archive.write_bytes(b'previous release')
        with patch.object(zipfile.ZipFile, 'testzip', return_value='bad member'):
            with self.assertRaisesRegex(ValueError, 'archive contents differ'):
                package_release.package(self.built, archive)
        self.assertEqual(archive.read_bytes(), b'previous release')
        self.assertEqual(list(self.root.glob('*.pending.zip')), [])

    def test_replace_failure_preserves_existing_archive_and_removes_pending(self):
        archive = self.root / 'release.zip'
        archive.write_bytes(b'previous release')
        with patch.object(package_release.os, 'replace', side_effect=OSError('cannot replace')):
            with self.assertRaisesRegex(OSError, 'cannot replace'):
                package_release.package(self.built, archive)
        self.assertEqual(archive.read_bytes(), b'previous release')
        self.assertEqual(list(self.root.glob('*.pending.zip')), [])

    def test_private_paths_in_guides_and_module_are_refused(self):
        windows = PureWindowsPath('C:/', 'Users', 'Owner', 'private', 'source')
        unix = PurePosixPath('/', 'home', 'owner', 'source')
        for text in (str(windows), str(unix), 'private/research/input'):
            with self.subTest(text=text):
                (self.docs / 'install-console.md').write_text(text, encoding='utf-8')
                with self.assertRaisesRegex(ValueError, 'private path'):
                    package_release.members(self.built)
        (self.docs / 'install-console.md').write_text('Console install', encoding='utf-8')
        self.set_module(small_elf(payload=windows.as_posix().encode('utf-8')))
        with self.assertRaisesRegex(ValueError, 'private path.*subsdk9'):
            package_release.members(self.built)


class StandaloneGuides(unittest.TestCase):
    def test_mdx_front_matter_steps_and_links_render_as_standalone_markdown(self):
        text = """---
title: Install Tsuchi
---

import {
    Steps
} from "@astrojs/starlight/components";

 <Steps>
1. Read [Console](/players/install-console/#mods).
2. Read [Help](</players/troubleshooting/> "Help").
 </Steps>

[more]: /players/install-a-mod/
`[literal](/players/example/)`
```markdown
[example](/players/example/)
```
"""
        out = package_release.render_guide(text.replace('\n', '\r\n')).decode('utf-8')
        self.assertTrue(out.startswith('# Install Tsuchi\n\n'))
        self.assertNotIn('import', out)
        self.assertNotIn('Steps', out)
        self.assertIn('[Console](INSTALL-CONSOLE.md#mods)', out)
        self.assertIn('[Help](<https://tsuchi.jpasqui.dev/players/troubleshooting/> "Help")', out)
        self.assertIn('[more]: https://tsuchi.jpasqui.dev/players/install-a-mod/', out)
        self.assertIn('`[literal](/players/example/)`', out)
        self.assertIn('```markdown\n[example](/players/example/)\n```', out)

    def test_component_import_in_a_code_example_stays_literal(self):
        text = "```mdx\nimport { Steps } from '@astrojs/starlight/components';\n<Steps>\n```\n"
        self.assertEqual(package_release.render_guide(text).decode('utf-8'), text)

    def test_external_links_and_emulator_cross_link(self):
        text = '[Emulator](/players/install-tsuchi/) [Source](https://example.test/docs)\n'
        self.assertEqual(package_release.render_guide(text).decode(),
                         '[Emulator](INSTALL-EMULATOR.md) [Source](https://example.test/docs)\n')

    def test_asides_render_as_quoted_blocks(self):
        text = ':::caution[WIP]\nRead [Help](/players/troubleshooting/).\n:::\n\n:::note\nA note.\n:::\n'
        self.assertEqual(package_release.render_guide(text).decode(),
                         '> **WIP**\n>\n> Read [Help](https://tsuchi.jpasqui.dev/players/troubleshooting/).\n\n'
                         '> **Note**\n>\n> A note.\n')

    def test_unknown_mdx_and_bad_front_matter_are_refused(self):
        for text in ('import Other from "other";\n', '<Tabs>\n', '---\ntitle: Unclosed\n',
                     '---\n- not a mapping\n---\ntext\n', ':::caution[WIP]\nUnclosed.\n'):
            with self.subTest(text=text), self.assertRaises(ValueError):
                package_release.render_guide(text)


if __name__ == '__main__':
    unittest.main()
