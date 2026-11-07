"""OpenSSL discovery works with hosted and local Git layouts."""
import os
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
import host_tools  # noqa: E402


class OpenSSLDiscovery(unittest.TestCase):
    def test_override_wins_over_git_and_path(self):
        with tempfile.TemporaryDirectory() as folder:
            override = Path(folder) / 'selected.exe'
            override.touch()
            with patch.dict(os.environ, {'TSUCHI_OPENSSL': str(override)}), \
                    patch.object(host_tools.shutil, 'which') as which:
                self.assertEqual(host_tools.find_openssl(), override.resolve())
            which.assert_not_called()

    def test_invalid_override_does_not_silently_use_another_installation(self):
        with tempfile.TemporaryDirectory() as folder:
            override = Path(folder) / 'missing.exe'
            with patch.dict(os.environ, {'TSUCHI_OPENSSL': str(override)}), \
                    patch.object(host_tools.shutil, 'which') as which:
                self.assertIsNone(host_tools.find_openssl())
            which.assert_not_called()

    def test_relative_override_is_absolute_for_consumers_with_another_cwd(self):
        (ROOT / 'build').mkdir(exist_ok=True)
        with tempfile.TemporaryDirectory(dir=ROOT / 'build') as folder:
            override = Path(folder) / 'selected.exe'
            override.touch()
            with patch.dict(os.environ, {'TSUCHI_OPENSSL': os.path.relpath(override)}):
                self.assertEqual(host_tools.find_openssl(), override.resolve())

    def test_git_layouts_are_found_from_the_executable_ancestors(self):
        for relative in ('mingw64/bin/openssl.exe', 'usr/bin/openssl.exe', 'bin/openssl.exe'):
            with self.subTest(relative=relative), tempfile.TemporaryDirectory() as folder:
                root = Path(folder)
                git = root / 'cmd/git.exe'
                git.parent.mkdir()
                git.touch()
                openssl = root / relative
                openssl.parent.mkdir(parents=True, exist_ok=True)
                openssl.touch()
                with patch.dict(os.environ, {'TSUCHI_OPENSSL': ''}), \
                        patch.object(host_tools.sys, 'platform', 'win32'), \
                        patch.object(host_tools.shutil, 'which', return_value=str(git)):
                    self.assertEqual(host_tools.find_openssl(), openssl.resolve())

    def test_path_fallback_needs_no_git_installation(self):
        with tempfile.TemporaryDirectory() as folder:
            openssl = Path(folder) / 'openssl.exe'
            openssl.touch()
            with patch.dict(os.environ, {'TSUCHI_OPENSSL': ''}), \
                    patch.object(host_tools.shutil, 'which', side_effect=lambda name: str(openssl) if name == 'openssl' else None):
                self.assertEqual(host_tools.find_openssl(), openssl.resolve())

    def test_directories_and_missing_path_entries_are_not_executables(self):
        with tempfile.TemporaryDirectory() as folder:
            for value in (folder, str(Path(folder) / 'missing.exe')):
                with self.subTest(value=value), patch.dict(os.environ, {'TSUCHI_OPENSSL': ''}), \
                        patch.object(host_tools.shutil, 'which', side_effect=lambda name: value if name == 'openssl' else None):
                    self.assertIsNone(host_tools.find_openssl())
