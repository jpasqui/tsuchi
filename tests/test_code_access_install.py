"""Only authored metadata is used here; no game files are test fixtures."""
import json
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
import install
import native_hook_permissions as permissions
from test_native_hooks import metadata


class CodeAccessInstall(unittest.TestCase):
    def test_adds_missing_groups_and_preserves_other_capabilities(self):
        for signature in (0x200, 0x300):
            for allowed in ({1}, permissions.REQUIRED - {0x74}, permissions.REQUIRED):
                raw = metadata(allowed, signatures=signature)
                original = bytes(raw)
                result = permissions.grant(raw)
                self.assertEqual(bytes(raw), original)
                self.assertEqual(permissions.validate(result)['system_calls'], sorted(permissions.REQUIRED))
                self.assertEqual(permissions.grant(result), result)
                aci_at, aci_size, acid_at, acid_size = struct.unpack_from('<IIII', result, 0x70)
                if 1 in allowed:
                    self.assertIn(1, permissions.syscalls(result[aci_at:aci_at + aci_size], 0x30, 0x40))
                self.assertEqual(struct.unpack_from('<I', result, acid_at + signature + 4)[0], acid_size - signature // 2)
                self.assertEqual(result[acid_at:acid_at + signature], original[struct.unpack_from('<I', original, 0x78)[0]:][:signature])

    def test_preserves_non_syscall_words(self):
        raw = metadata({1})
        aci_at, _, acid_at, _ = struct.unpack_from('<IIII', raw, 0x70)
        # Replace one existing word with an unrelated handle-table descriptor.
        for at, header in ((aci_at, 0), (acid_at, 0x200)):
            offset, size = struct.unpack_from('<II', raw, at + header + 0x30)
            struct.pack_into('<I', raw, at + offset, 0x007f7fff)
        result = permissions.grant(raw)
        aci_at, aci_size, acid_at, acid_size = struct.unpack_from('<IIII', result, 0x70)
        for at, size, header in ((aci_at, aci_size, 0), (acid_at, acid_size, 0x200)):
            offset, count = struct.unpack_from('<II', result, at + header + 0x30)
            self.assertEqual(struct.unpack_from('<I', result, at + offset)[0], 0x007f7fff)

    def test_rejects_overlapping_access_sections_and_bad_signed_size(self):
        raw = metadata({1})
        struct.pack_into('<II', raw, 0x80 + 0x20, 0x40, 4)
        with self.assertRaisesRegex(ValueError, 'overlap'):
            permissions.grant(raw)
        raw = metadata({1})
        acid_at = struct.unpack_from('<I', raw, 0x78)[0]
        struct.pack_into('<I', raw, acid_at + 0x204, 1)
        with self.assertRaisesRegex(ValueError, 'signed size'):
            permissions.grant(raw)

    def test_installer_preserves_dump_is_idempotent_and_refuses_foreign_output(self):
        for fat in (False, True):
            with tempfile.TemporaryDirectory() as temp:
                root = Path(temp)
                source = root / 'dump.npdm'
                source.write_bytes(metadata({1}))
                original = source.read_bytes()
                exefs = root / 'exefs'
                exefs.mkdir()
                (exefs / 'subsdk9').write_bytes(b'authored fixture')
                with patch.object(install.os, 'link', side_effect=OSError('FAT')) if fat else patch.object(install.os, 'link', wraps=install.os.link):
                    install.install(source, exefs)
                prepared = (exefs / 'main.npdm').read_bytes()
                install.install(source, exefs)
                self.assertEqual((exefs / 'main.npdm').read_bytes(), prepared)
                self.assertEqual(source.read_bytes(), original)
                (exefs / 'main.npdm').write_bytes(b'foreign file')
                with self.assertRaisesRegex(ValueError, 'existing main.npdm differs'):
                    install.install(source, exefs)
                self.assertEqual((exefs / 'main.npdm').read_bytes(), b'foreign file')
                self.assertEqual(list(exefs.glob('.tsuchi-npdm-*')), [])

    def test_extracted_installer_needs_no_repo_or_python_packages(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            for helper in ('install.py', 'native_hook_permissions.py'):
                (root / helper).write_bytes((ROOT / 'tools' / helper).read_bytes())
            (root / 'release.json').write_text(json.dumps({'builds': [{'program_id': f'{permissions.PROGRAM:016X}'}]}), encoding='utf-8')
            source, exefs = root / 'dump.npdm', root / 'exefs'
            source.write_bytes(metadata({1}))
            exefs.mkdir()
            (exefs / 'subsdk9').write_bytes(b'authored fixture')
            result = subprocess.run([sys.executable, '-S', root / 'install.py', '--npdm', source, '--exefs', exefs], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            permissions.validate((exefs / 'main.npdm').read_bytes())
