"""Delivery checks use tiny production fixtures, not a native build or game run."""
import copy
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch
import zipfile

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
import package_release
import package_sdk
import package_site
import schema_identity
from gen import game_table, stubs


def write_zip(path, members):
    with zipfile.ZipFile(path, 'w', compression=zipfile.ZIP_DEFLATED) as archive:
        for name, data in members.items():
            archive.writestr(name, data)
    return package_site.digest(path.read_bytes())


class SiteDelivery(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.sdk_members = package_sdk.members(ROOT)
        cls.identity = schema_identity.capture(ROOT)
        cls.builds = game_table.load_builds(ROOT / 'game/table.yaml')
        cls.table = cls.builds[0]
        cls.titles = sorted({package_release.title_of(each) for each in cls.builds})

    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.folder = Path(temporary.name)
        self.built = self.folder / 'site'
        self.built.mkdir()
        (self.built / 'index.html').write_text('<html></html>', encoding='utf-8')
        for page in package_site.PAGES:
            output = self.built / page
            output.mkdir(parents=True, exist_ok=True)
            links = ('/downloads/Tsuchi-1.1.1.zip', '/downloads/Tsuchi-SDK.zip') if page == 'downloads' else ('/downloads/',)
            (output / 'index.html').write_text(''.join('<a href="' + link + '">Download</a>' for link in links), encoding='utf-8')
        module = bytearray(0x100)
        module[:4] = b'NSO0'
        module[0x40:0x60] = bytes.fromhex('ab' * 32)
        version, build_id = self.table['version'], self.table['build_id'].upper()
        # A release of every build the table knows: one module per program ID, and each build's patches.
        self.production = {
            'emulator/Tsuchi/exefs/subsdk9': bytes(module),
            **{'console/atmosphere/contents/' + title + '/exefs/subsdk9': bytes(module) for title in self.titles},
            **{'emulator/Tsuchi/exefs/' + each['key'] + '.pchtxt': b'patch fixture ' + each['key'].encode() for each in self.builds},
            **{'console/atmosphere/exefs_patches/Tsuchi/' + each['build_id'].upper() + '.ips': b'PATCHEOF' for each in self.builds},
            'INSTALL-CONSOLE.md': b'Console instructions', 'INSTALL-EMULATOR.md': b'Emulator instructions',
            'THIRD-PARTY-NOTICES.txt': b'Third-party notices',
        }
        self.report = {'format': 1, 'game_version': version, 'game_build_id': build_id,
                       'builds': [dict(key=each['key'], version=each['version'], build_id=each['build_id'].upper(),
                                       program_id=package_release.title_of(each), patch_hash=f'{stubs.patch_hash(each):016x}')
                                  for each in self.builds],
                       'module_sha256': package_site.digest(module), 'elf_sha256': 'ab' * 32,
                       'game_table_sha256': package_site.digest((ROOT / 'game/table.yaml').read_bytes()),
                       'patch_hash': f'{stubs.patch_hash(self.table):016x}', 'selfcheck': False,
                       **copy.deepcopy(self.identity),
                       'files': {name: {'bytes': len(data), 'sha256': package_site.digest(data)}
                                 for name, data in self.production.items()}}
        self.release = self.folder / 'release.zip'
        self.sdk = self.folder / 'sdk.zip'
        self.release_hash = self.save_release()
        self.sdk_hash = write_zip(self.sdk, self.sdk_members)

    def members(self):
        """The release's files: the guides, the notices and release.json, the emulator's module, one module per program ID,
        and two patches per build."""
        return 5 + len(self.titles) + 2 * len(self.builds)

    def save_release(self):
        self.production['release.json'] = json.dumps(self.report).encode('utf-8')
        return write_zip(self.release, self.production)

    def deliver(self):
        return package_site.package(self.built, self.release, self.sdk, self.release_hash, self.sdk_hash, ROOT)

    def test_valid_pair_has_verified_downloads_and_a_commit_receipt(self):
        receipt = self.deliver()
        downloads = self.built / 'downloads'
        self.assertEqual(json.loads((downloads / 'artifacts.json').read_text(encoding='utf-8')), receipt)
        self.assertEqual((downloads / 'Tsuchi-1.1.1.zip').read_bytes(), self.release.read_bytes())
        self.assertEqual((downloads / 'Tsuchi-SDK.zip').read_bytes(), self.sdk.read_bytes())
        self.assertEqual(receipt['release']['members'], self.members())
        self.assertEqual(receipt['release']['module_file'], 'exefs/subsdk9')
        self.assertEqual(receipt['sdk']['members'], len(self.sdk_members))
        self.assertEqual(receipt['schema_inputs'], self.identity['schema_inputs'])
        self.assertEqual(sorted(path.name for path in downloads.iterdir()),
                         ['Tsuchi-1.1.1.zip', 'Tsuchi-SDK.zip', 'artifacts.json', 'index.html'])

    def test_custom_slot_pair_binds_both_module_paths_and_receipt(self):
        for prefix in ('emulator/Tsuchi/', *('console/atmosphere/contents/' + title + '/' for title in self.titles)):
            old, new = prefix + 'exefs/subsdk9', prefix + 'exefs/subsdk8'
            self.production[new] = self.production.pop(old)
            self.report['files'][new] = self.report['files'].pop(old)
        self.report['module_file'] = 'exefs/subsdk8'
        self.release_hash = self.save_release()
        receipt = self.deliver()
        self.assertEqual(receipt['release']['module_file'], 'exefs/subsdk8')
        self.assertEqual(receipt['release']['members'], self.members())

    def test_invalid_or_mismatched_slot_identity_and_extra_slot_are_refused(self):
        original = copy.deepcopy(self.report)
        for name in ('exefs/subsdk8', 'exefs/subsdk10', '../subsdk8', 'exefs/subsdk8\n', None, []):
            with self.subTest(name=name):
                self.report = {**original, 'module_file': name}
                self.release_hash = self.save_release()
                with self.assertRaises(ValueError):
                    self.deliver()
        self.report = original
        duplicate = 'emulator/Tsuchi/exefs/subsdk8'
        self.production[duplicate] = self.production['emulator/Tsuchi/exefs/subsdk9']
        self.report['files'][duplicate] = self.report['files']['emulator/Tsuchi/exefs/subsdk9']
        self.release_hash = self.save_release()
        with self.assertRaisesRegex(ValueError, 'allowlist of its files'):
            self.deliver()

    def test_unqualified_hash_does_not_change_existing_downloads(self):
        self.deliver()
        before = self.snapshot()
        self.sdk_hash = '00' * 32
        with self.assertRaisesRegex(ValueError, 'qualified SHA-256'):
            self.deliver()
        self.assertEqual(self.snapshot(), before)

    def snapshot(self):
        return {path.name: path.read_bytes() for path in (self.built / 'downloads').iterdir() if path.is_file()}

    def test_extra_or_unsafe_production_member_is_refused(self):
        for name in ('game-data.bin', '../escaped', '/absolute', 'directory\\escaped'):
            with self.subTest(name=name):
                changed = {**self.production, name: b'not allowed'}
                self.release_hash = write_zip(self.release, changed)
                with self.assertRaises(ValueError):
                    self.deliver()
                self.assertFalse((self.built / 'downloads/Tsuchi-1.1.1.zip').exists())

    def test_incompatible_version_or_schema_is_refused(self):
        original = copy.deepcopy(self.report)
        for field, value in (('game_version', 'other'), ('api_version', -1), ('api_version', False), ('schema_inputs', {})):
            with self.subTest(field=field):
                self.report = {**original, field: value}
                self.release_hash = self.save_release()
                with self.assertRaises(ValueError):
                    self.deliver()

    def test_incompatible_sdk_pair_and_changed_headers_are_refused(self):
        original = copy.deepcopy(self.sdk_members)
        inventory = json.loads(original[package_sdk.INVENTORY])
        inventory['inputs']['schema/abi.lock'] = '00' * 32
        changed = {**original, package_sdk.INVENTORY: json.dumps(inventory).encode('utf-8')}
        self.sdk_hash = write_zip(self.sdk, changed)
        with self.assertRaisesRegex(ValueError, 'API identities differ'):
            self.deliver()
        changed = {**original, 'sdk/include/tsuchi/tsuchi.h': b'stale API header'}
        self.sdk_hash = write_zip(self.sdk, changed)
        with self.assertRaisesRegex(ValueError, 'SDK headers or payload differ'):
            self.deliver()

    def test_partial_archive_is_refused_and_existing_pair_is_preserved(self):
        self.deliver()
        before = self.snapshot()
        self.sdk.write_bytes(self.sdk.read_bytes()[:30])
        self.sdk_hash = package_site.digest(self.sdk.read_bytes())
        with self.assertRaises(zipfile.BadZipFile):
            self.deliver()
        self.assertEqual(self.snapshot(), before)

    def test_malformed_inventory_is_refused_and_existing_pair_is_preserved(self):
        self.deliver()
        before = self.snapshot()
        for malformed in ([], {'inputs': []}):
            with self.subTest(inventory=malformed):
                members = {**self.sdk_members, package_sdk.INVENTORY: json.dumps(malformed).encode('utf-8')}
                self.sdk_hash = write_zip(self.sdk, members)
                with self.assertRaisesRegex(ValueError, 'inventory must be a mapping'):
                    self.deliver()
                self.assertEqual(self.snapshot(), before)

    def test_linked_download_target_is_refused_without_replacing_any_file(self):
        self.deliver()
        before = self.snapshot()
        original = package_sdk.linked
        def linked(path):
            return Path(path).name == 'Tsuchi-SDK.zip' or original(path)
        with patch.object(package_sdk, 'linked', side_effect=linked):
            with self.assertRaisesRegex(ValueError, 'Download target is linked'):
                self.deliver()
        self.assertEqual(self.snapshot(), before)

    def test_failure_between_replacements_rolls_back_both_archives_and_receipt(self):
        self.deliver()
        before = self.snapshot()
        self.production['INSTALL-CONSOLE.md'] = b'Updated instructions'
        self.report['files']['INSTALL-CONSOLE.md'] = {'bytes': 20, 'sha256': package_site.digest(b'Updated instructions')}
        self.release_hash = self.save_release()
        original = package_site.os.replace
        failed = False

        def replace(source, target):
            nonlocal failed
            if Path(source).name == 'Tsuchi-SDK.zip' and not failed:
                failed = True
                raise OSError('Simulated interrupted replacement')
            return original(source, target)

        with patch.object(package_site.os, 'replace', side_effect=replace):
            with self.assertRaisesRegex(OSError, 'interrupted replacement'):
                self.deliver()
        self.assertTrue(failed)
        self.assertEqual(self.snapshot(), before)

    def test_missing_built_link_refuses_delivery(self):
        (self.built / 'downloads/index.html').write_text('<a href="/downloads/Tsuchi-SDK.zip">SDK</a>', encoding='utf-8')
        with self.assertRaisesRegex(ValueError, 'Built delivery links differ'):
            self.deliver()


if __name__ == '__main__':
    unittest.main()
