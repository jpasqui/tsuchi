"""Packages: bounded ZIP extraction, receipts, streaming hashes, the install coordinator, and installed folders."""
import hashlib
import io
import shutil
import struct
import subprocess
import sys
import unittest
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
import generate  # noqa: E402

CHEAT = b'[Original name]\n400F0000 00000000 00000001\n'


def archive(entries, method=zipfile.ZIP_DEFLATED):
    out = io.BytesIO()
    with zipfile.ZipFile(out, 'w', compression=method) as made:
        for name, body in entries:
            made.writestr(name, body)
    return out.getvalue()


def cooked(stem, prefix='romfs/Carrot4/Content/Paks/'):
    return [(f'{prefix}{stem}.{ext}', bytes([i]) * 130) for i, ext in enumerate(('pak', 'utoc', 'ucas'), 1)]


def compile_binary(name, sources, std='c++17', extra=()):
    gen = ROOT / 'build/packages/gen'
    generate.write(gen)
    out = ROOT / 'build/packages'
    out.mkdir(parents=True, exist_ok=True)
    binary = out / (name + ('.exe' if sys.platform == 'win32' else ''))
    folders = [gen / 'include', gen / 'framework', ROOT / 'framework/core', ROOT / 'framework/base', ROOT / 'framework/game']
    command = [shutil.which('clang++'), f'-std={std}', '-Wall', '-Wextra', '-Werror', *extra]
    for folder in folders:
        command += ['-I', str(folder)]
    command += [str(ROOT / source) if not Path(source).is_absolute() else str(source) for source in sources]
    command += ['-o', str(binary)]
    built = subprocess.run(command, capture_output=True, text=True, encoding='utf-8', errors='replace')
    if built.returncode:
        raise AssertionError(built.stdout + built.stderr)
    return binary, gen


class PackageArchives(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.binary, _ = compile_binary('package_zip', ['framework/core/mod_zip.cpp', 'framework/core/sha256.cpp', 'tests/core/package_zip_test.cpp'])
        cls.out = ROOT / 'build/packages'

    def run_case(self, mode, data, *args, success=True):
        path = self.out / 'input.bin'
        path.write_bytes(data if isinstance(data, bytes) else data.encode())
        result = subprocess.run([str(self.binary), mode, str(path), *args], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0 if success else 2, result.stdout + result.stderr)
        return result.stdout

    def test_stored_deflated_wrapped_and_multi_mod_packages(self):
        for method in (zipfile.ZIP_STORED, zipfile.ZIP_DEFLATED):
            for wrapper in ('', 'Release/'):
                entries = [(wrapper + 'romfs/tsuchi/mods/author.one/mod.json', b'{"id":"author.one"}'),
                           (wrapper + 'romfs/tsuchi/mods/author.one/main.lua', b'print("loaded")'),
                           (wrapper + 'romfs/tsuchi/mods/author.two/mod.json', b'{"id":"author.two"}'),
                           (wrapper + 'romfs/tsuchi/mods/author.two/data.bin', bytes(range(256)) * 1500), (wrapper + 'README.md', b'Notes')]
                result = self.run_case('zip', archive(entries, method))
                for name, body in entries[:-1]:
                    self.assertIn(name.split('/mods/')[1] + ' ' + hashlib.sha256(body).hexdigest(), result)
                self.assertIn('kind mods', result)

    def test_paths_aliases_links_and_wrong_package_types_are_refused(self):
        good = ('romfs/tsuchi/mods/author.mod/mod.json', b'{}')
        for path in ('../escape', 'romfs/tsuchi/mods/author.mod/../escape', 'romfs/tsuchi/mods/author.mod/a:b', 'romfs/tsuchi/mods/author.mod/NUL.txt',
                     'romfs/tsuchi/mods/author.mod/trailing.', 'romfs/tsuchi/mods/tsuchi.self/mod.json', 'romfs/Carrot4/Content/Paks/mod.pak', 'exefs/main'):
            with self.subTest(path=path):
                self.run_case('zip', archive([good, (path, b'x')]), success=False)
        self.run_case('zip', archive([good, ('romfs/tsuchi/mods/author.mod/MOD.JSON', b'{}')]), success=False)
        link = zipfile.ZipInfo('romfs/tsuchi/mods/author.mod/link')
        link.create_system = 3
        link.external_attr = (0o120777 << 16)
        self.run_case('zip', archive([good, (link, b'target')]), success=False)
        link.external_attr = 0x400
        self.run_case('zip', archive([good, (link, b'target')]), success=False)

    def test_corruption_truncation_bombs_and_cancel_publish_nothing(self):
        entries = [('romfs/tsuchi/mods/author.mod/mod.json', b'{}'), ('romfs/tsuchi/mods/author.mod/main.lua', b'hello' * 20000)]
        data = archive(entries)
        for how in ('cancel', 'midcancel', 'write'):
            self.run_case('zip', data, how, success=False)
        self.run_case('zip', data[:-3], success=False)
        broken = bytearray(archive(entries, zipfile.ZIP_STORED))
        name_len = struct.unpack_from('<H', broken, 26)[0]
        broken[30 + name_len] ^= 1
        self.run_case('zip', bytes(broken), success=False)
        self.run_case('zip', archive([entries[0], ('romfs/tsuchi/mods/author.mod/bomb', bytes(2 << 20))]), success=False)
        self.run_case('zip', archive([(f'romfs/tsuchi/mods/author.mod/f{i}', b'x') for i in range(257)]), success=False)

    def test_cooked_container_goes_under_the_derived_id_whatever_its_name(self):
        first = self.run_case('zip', archive(cooked('Map_P')), 'derived')
        renamed = self.run_case('zip', archive(cooked('Another_Name_P', 'release/')), 'derived')
        self.assertIn('external.0123456789abcdef01234567/Map_P.pak ', first)
        self.assertIn('external.0123456789abcdef01234567/Another_Name_P.pak ', renamed)
        self.assertIn('kind assets', first)
        for _, data in cooked('Map_P'):
            self.assertIn(hashlib.sha256(data).hexdigest(), first)
        # Without a derived ID, a cooked container is not admitted.
        self.run_case('zip', archive(cooked('Map_P')), success=False)
        for files in (cooked('Map_P')[:-1], cooked('Map_P') + cooked('Second_P'), cooked('Map_P') + [('exefs/main', b'x')],
                      cooked('Map_P') + [('cheats/060F827BD2B38703.txt', CHEAT)], cooked('Map_P') + [('romfs/tsuchi/mods/author.mod/mod.json', b'{}')]):
            self.run_case('zip', archive(files), 'derived', success=False)

    def test_cheat_archives_keep_names_and_refuse_mixed_or_wrong_game(self):
        bare = self.run_case('zip', archive([('060f827bd2b38703.txt', CHEAT)]), 'derived')
        wrapped = self.run_case('zip', archive([('Release/atmosphere/contents/0100B7C00933A000/cheats/060F827BD2B38703.txt', CHEAT)]), 'derived')
        self.assertEqual(bare, wrapped)
        self.assertIn('external.0123456789abcdef01234567/cheats/060F827BD2B38703.txt ' + hashlib.sha256(CHEAT).hexdigest(), bare)
        self.assertIn('kind cheats', bare)
        good = ('cheats/060F827BD2B38703.txt', CHEAT)
        for extra in [('romfs/a.pak', b'x'), ('romfs/tsuchi/mods/a/mod.json', b'{}'), ('cheats/build.txt', CHEAT), ('other/060f827bd2b38703.txt', CHEAT)]:
            self.run_case('zip', archive([good, extra]), 'derived', success=False)
        self.run_case('zip', archive([('atmosphere/contents/0100000000000000/cheats/060F827BD2B38703.txt', CHEAT)]), 'derived', success=False)
        self.run_case('zip', archive([(good[0], bytes(65536))]), 'derived', success=False)
        self.run_case('zip', archive([(good[0], b'')]), 'derived', success=False)

    def test_receipts_and_incremental_hash(self):
        self.run_case('receipt', b'')
        for size in (0, 1, 55, 56, 63, 64, 65, 127, 128, 100003):
            self.run_case('hash', (bytes(range(256)) * ((size + 255) // 256))[:size])


class PackageInstalls(unittest.TestCase):
    def test_install_update_remove_cancel_and_capability(self):
        gen = ROOT / 'build/packages/gen'
        generate.write(gen)
        sources = ['framework/core/json.cpp', 'framework/core/text.cpp', 'framework/core/texts.cpp', 'framework/core/sha256.cpp', 'framework/core/registry.cpp',
                   'framework/core/ids.cpp', 'framework/core/mod_zip.cpp', 'framework/core/cheat_vm.cpp', 'tests/game/packages_test.cpp',
                   gen / 'framework/packages_service.cpp']
        binary, _ = compile_binary('packages_test', sources, std='c++20')
        zips = ROOT / 'build/packages/zips'
        zips.mkdir(parents=True, exist_ok=True)
        made = {
            'mods': [('romfs/tsuchi/mods/author.mod/mod.json', b'{"id":"author.mod"}'), ('romfs/tsuchi/mods/author.mod/main.lua', b"print('one')")],
            'mods2': [('Release/romfs/tsuchi/mods/author.mod/mod.json', b'{"id":"author.mod"}'), ('Release/romfs/tsuchi/mods/author.mod/main.lua', b"print('two')")],
            'other': [('romfs/tsuchi/mods/other.mod/mod.json', b'{"id":"other.mod"}'), ('romfs/tsuchi/mods/other.mod/main.lua', b'print(3)')],
            'unsupported': [('romfs/tsuchi/mods/author.mod/mod.json', b'{}'), ('exefs/main', b'x')],
            'cooked': cooked('Map_P'),
            'cooked_renamed': cooked('Map_Renamed_P', 'Release/'),
            'cooked_two': cooked('Map_P') + cooked('Second_P'),
            'cheats': [('cheats/060f827bd2b38703.txt', CHEAT)],
        }
        for name, entries in made.items():
            (zips / f'{name}.zip').write_bytes(archive(entries))
        ran = subprocess.run([str(binary), str(zips)], capture_output=True, text=True)
        self.assertEqual(ran.returncode, 0, ran.stdout + ran.stderr)
        self.assertIn('packages ok', ran.stdout)

    def test_manifest_admission_capabilities_and_scan_failure(self):
        sources = ['framework/core/registry.cpp', 'framework/core/ids.cpp', 'framework/core/json.cpp', 'framework/core/text.cpp', 'framework/core/texts.cpp',
                   'framework/base/settings.cpp', 'framework/base/mod_choices.cpp', 'framework/base/mods.cpp', 'tests/base/mod_install_test.cpp']
        extra = ['-ffunction-sections', '-fdata-sections'] + (['-Xlinker', '/OPT:REF'] if sys.platform == 'win32' else ['-Wl,--gc-sections'])
        binary, _ = compile_binary('mod_install', sources, extra=extra)
        ran = subprocess.run([str(binary)], capture_output=True, text=True)
        self.assertEqual(ran.returncode, 0, ran.stdout + ran.stderr)


if __name__ == '__main__':
    unittest.main()
