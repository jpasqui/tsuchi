"""Candidate receipts reject uncommitted sources, mismatched tags, and changed payloads."""
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
import ci_package
import build_module
import package_release
import package_sdk


class CandidatePackage(unittest.TestCase):
    def setUp(self):
        work = tempfile.TemporaryDirectory()
        self.addCleanup(work.cleanup)
        self.root = Path(work.name)
        (self.root / 'schema').mkdir()
        (self.root / 'schema/core.yaml').write_text('api_version: 0\n', encoding='utf-8')
        self.lock()
        (self.root / 'LICENSE').write_text('MIT License\n', encoding='utf-8')
        (self.root / 'tools').mkdir()
        (self.root / 'tools/Tsuchi-ThirdPartyNotices.txt').write_text('Third-party notices\n', encoding='utf-8')
        self.source = {'commit': 'a' * 40, 'tree': 'b' * 40}
        self.out = self.root / 'build/ci/candidate'

    def lock(self, released=False):
        (self.root / 'schema/abi.lock').write_text(json.dumps(
            {'released': released, 'release_version': '0.2.0', 'api_version': 0}), encoding='utf-8')

    def test_tag_version_and_unreleased_abi_guards(self):
        for tag in ('v0.3.0-rc.1', 'v0.2.0-rc.01', 'v0.2.0-', 'v0.2.0-rc..1'):
            with self.subTest(tag=tag), self.assertRaises(ValueError):
                ci_package.release_identity(self.root, tag)
        self.assertTrue(ci_package.release_identity(self.root, 'v0.2.0-rc.1')['prerelease'])
        # A plain tag of an unreleased ABI is a prerelease.
        self.assertTrue(ci_package.release_identity(self.root, 'v0.2.0')['prerelease'])
        self.lock(released=True)
        self.assertFalse(ci_package.release_identity(self.root, 'v0.2.0')['prerelease'])
        self.assertTrue(ci_package.release_identity(self.root)['prerelease'])

    def test_tool_versions_use_resolved_executable_names(self):
        completed = subprocess.CompletedProcess([], 0, stdout='tool version 22.1.1\n', stderr='')
        with patch.object(ci_package.shutil, 'which', side_effect=lambda name: 'resolved-' + name + '.exe'), \
                patch.object(ci_package.subprocess, 'run', return_value=completed) as run, \
                patch.object(ci_package.importlib.metadata, 'version', return_value='pinned'):
            versions = ci_package.tools_identity()
        self.assertEqual([call.args[0][0] for call in run.call_args_list],
                         ['resolved-clang.exe', 'resolved-ld.lld.exe'])
        self.assertEqual(versions['ld.lld'], 'tool version 22.1.1')

    def install_mocks(self):
        for name, value in (
                ('source_identity', self.source),
                ('supported_builds', [{'key': '1.1.1', 'version': '1.1.1',
                                       'build_id': 'C' * 32, 'program_id': '0100B7C00933A000'}]),
                ('tools_identity', {'python': '3.11.9', 'clang': 'clang version 22.1.1', 'ld.lld': 'LLD 22.1.1'})):
            mock = patch.object(ci_package, name, return_value=value)
            setattr(self, name, mock.start())
            self.addCleanup(mock.stop)
        self.build = patch.object(build_module, 'build').start()
        self.addCleanup(patch.stopall)

        def archive(path, payload):
            path.write_bytes(payload)
            return {'archive': str(path), 'archive_sha256': hashlib.sha256(payload).hexdigest()}

        self.production = patch.object(package_release, 'package',
            side_effect=lambda module, path, **kwargs: archive(path, b'production')).start()
        self.sdk = patch.object(package_sdk, 'package',
            side_effect=lambda path, **kwargs: archive(path, b'sdk')).start()

    def test_candidate_receipt_binds_payload_hashes_and_source_without_local_paths(self):
        self.install_mocks()
        result = ci_package.package(self.root, build=True, tag='v0.2.0-rc.1')
        receipt = json.loads((self.out / 'provenance.json').read_text(encoding='utf-8'))
        self.assertEqual(receipt['source'], self.source)
        self.assertEqual(receipt['build_execution'], 'built')
        self.assertTrue(receipt['prerelease'])
        self.assertEqual(receipt['tag'], 'v0.2.0-rc.1')
        self.assertEqual(receipt['game_builds'][0]['program_id'], '0100B7C00933A000')
        self.assertNotIn(str(self.root), (self.out / 'provenance.json').read_text(encoding='utf-8'))
        self.assertTrue(self.production.call_args.kwargs['prerelease'])
        self.assertTrue(self.sdk.call_args.kwargs['prerelease'])
        self.assertIn('Tsuchi-0.2.0-prerelease.zip', result['files'])
        checksums = (self.out / 'SHA256SUMS').read_text(encoding='utf-8').splitlines()
        self.assertEqual(len(checksums), len(receipt['files']) + 1)
        for line in checksums:
            wanted, name = line.split('  ', 1)
            self.assertEqual(wanted, ci_package.digest(self.out / name))

    def test_tag_candidate_requires_a_source_bound_build(self):
        self.install_mocks()
        with self.assertRaisesRegex(ValueError, 'require --build'):
            ci_package.package(self.root, tag='v0.2.0-rc.1')
        self.production.assert_not_called()

    def test_packager_hash_mismatch_never_writes_completion_receipt(self):
        self.install_mocks()
        original = self.production.side_effect

        def changed_hash(*args, **kwargs):
            result = original(*args, **kwargs)
            result['archive_sha256'] = '0' * 64
            return result

        self.production.side_effect = changed_hash
        with self.assertRaisesRegex(ValueError, 'archive hash changed'):
            ci_package.package(self.root, build=True)
        self.assertFalse((self.out / 'provenance.json').exists())
        self.assertFalse((self.out / 'SHA256SUMS').exists())

    def test_source_change_retires_old_completion_receipt(self):
        self.install_mocks()
        self.out.mkdir(parents=True)
        (self.out / 'provenance.json').write_text('old receipt', encoding='utf-8')
        (self.out / 'SHA256SUMS').write_text('old sums', encoding='utf-8')
        self.source_identity.side_effect = [self.source, {'commit': 'c' * 40, 'tree': 'd' * 40}]
        with self.assertRaisesRegex(ValueError, 'Source changed'):
            ci_package.package(self.root, build=True)
        self.assertFalse((self.out / 'provenance.json').exists())
        self.assertFalse((self.out / 'SHA256SUMS').exists())
        self.production.assert_not_called()

    def test_stale_payloads_and_paths_outside_build_are_refused(self):
        self.install_mocks()
        with self.assertRaisesRegex(ValueError, 'inside build/'):
            ci_package.package(self.root, out=self.root / 'public')
        self.out.mkdir(parents=True)
        (self.out / 'old-version.zip').write_bytes(b'old')
        with self.assertRaisesRegex(ValueError, 'another candidate'):
            ci_package.package(self.root)
        self.production.assert_not_called()

    def test_download_verification_checks_tag_source_hashes_and_writes_notes(self):
        self.install_mocks()
        ci_package.package(self.root, build=True, tag='v0.2.0-rc.1')
        with patch.object(ci_package, 'git', return_value=self.source['commit']):
            notes = self.root / 'build/ci/release-notes.md'
            receipt = ci_package.verify(self.root, self.out, 'v0.2.0-rc.1', notes)
            self.assertTrue(receipt['prerelease'])
            self.assertIn('physical console qualification', notes.read_text(encoding='utf-8'))
            self.assertIn('0100B7C00933A000', notes.read_text(encoding='utf-8'))
            with self.assertRaisesRegex(ValueError, 'release identity'):
                ci_package.verify(self.root, self.out, 'v0.2.0-rc.2')
            (self.out / 'Tsuchi-0.2.0-prerelease.zip').write_bytes(b'tampered production')
            with self.assertRaisesRegex(ValueError, 'payload hash or size'):
                ci_package.verify(self.root, self.out, 'v0.2.0-rc.1')

    def test_download_verification_refuses_rewritten_source_and_extra_files(self):
        self.install_mocks()
        ci_package.package(self.root, build=True, tag='v0.2.0-rc.1')
        with patch.object(ci_package, 'git', return_value='c' * 40), self.assertRaisesRegex(ValueError, 'release tag'):
            ci_package.verify(self.root, self.out, 'v0.2.0-rc.1')
        with patch.object(ci_package, 'git', return_value=self.source['commit']):
            (self.out / 'extra.txt').write_text('unexpected artifact', encoding='utf-8')
            with self.assertRaisesRegex(ValueError, 'files differ'):
                ci_package.verify(self.root, self.out, 'v0.2.0-rc.1')
            (self.out / 'extra.txt').unlink()
            receipt = json.loads((self.out / 'provenance.json').read_text(encoding='utf-8'))
            receipt['source']['commit'] = 'c' * 40
            (self.out / 'provenance.json').write_text(json.dumps(receipt), encoding='utf-8')
            with self.assertRaisesRegex(ValueError, 'source commit'):
                ci_package.verify(self.root, self.out, 'v0.2.0-rc.1')

    def test_verifier_entry_point_needs_only_the_standard_library(self):
        result = subprocess.run([sys.executable, '-S', str(ROOT / 'tools/ci_package.py'), '--help'],
                                capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('--verify', result.stdout)

    @unittest.skipUnless(shutil.which('git'), 'needs git')
    def test_clean_source_identity_refuses_modified_and_untracked_inputs(self):
        subprocess.run(['git', 'init', '-q', str(self.root)], check=True, capture_output=True)
        (self.root / '.gitignore').write_text('build/\n', encoding='utf-8')
        subprocess.run(['git', '-C', str(self.root), 'add', '.'], check=True, capture_output=True)
        subprocess.run(['git', '-C', str(self.root), '-c', 'user.name=Tsuchi CI test',
                        '-c', 'user.email=ci@example.invalid', 'commit', '-qm', 'fixture'],
                       check=True, capture_output=True)
        self.assertEqual(len(ci_package.source_identity(self.root)['commit']), 40)
        (self.root / 'build').mkdir()
        (self.root / 'build/generated.txt').write_text('ignored output', encoding='utf-8')
        ci_package.source_identity(self.root)
        (self.root / 'untracked.txt').write_text('input', encoding='utf-8')
        with self.assertRaisesRegex(ValueError, 'clean source'):
            ci_package.source_identity(self.root)
        (self.root / 'untracked.txt').unlink()
        (self.root / 'LICENSE').write_text('modified', encoding='utf-8')
        with self.assertRaisesRegex(ValueError, 'clean source'):
            ci_package.source_identity(self.root)


if __name__ == '__main__':
    unittest.main()
