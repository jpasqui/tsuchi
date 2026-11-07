"""Attach a qualified production/SDK pair to an already built documentation folder.

Supply both archives and their reviewed SHA-256 hashes. No archive is built here.
The final pair receipt is downloads/artifacts.json. Publishing is a separate step.
"""
import argparse
import hashlib
from html.parser import HTMLParser
import io
import json
import os
from pathlib import Path, PurePosixPath
import re
import stat
import sys
import tempfile
import zipfile

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
import package_release
import package_sdk
import schema_identity
from gen import game_table, stubs

MAX_ARCHIVE = 24 * 1024 * 1024
MAX_MEMBER = 8 * 1024 * 1024
SDK_NAME = 'Tsuchi-SDK.zip'
RECEIPT = 'artifacts.json'
PAGES = ('downloads', 'players/install-tsuchi', 'players/install-console',
         'develop/compiled-mod', 'develop/first-mod')


def digest(data):
    return hashlib.sha256(data).hexdigest()


def archive(path, expected_hash):
    if not isinstance(expected_hash, str) or not re.fullmatch('[0-9a-f]{64}', expected_hash):
        raise ValueError('An explicit qualified archive SHA-256 is required')
    path = Path(path)
    if not path.is_file() or path.is_symlink() or path.stat().st_size > MAX_ARCHIVE:
        raise ValueError('Archive is missing, linked, or too large')
    raw = path.read_bytes()
    if len(raw) > MAX_ARCHIVE or digest(raw) != expected_hash:
        raise ValueError('Archive differs from its qualified SHA-256')
    out = {}
    with zipfile.ZipFile(io.BytesIO(raw)) as zipped:
        infos = zipped.infolist()
        if len(infos) > 128 or sum(item.file_size for item in infos) > MAX_ARCHIVE:
            raise ValueError('Archive payload exceeds its bound')
        for item in infos:
            name = item.filename
            member = PurePosixPath(name)
            if (member.as_posix() != name or member.is_absolute() or '..' in member.parts or
                    '\\' in name or ':' in name or item.is_dir() or name in out or
                    stat.S_IFMT(item.external_attr >> 16) == stat.S_IFLNK):
                raise ValueError('Unsafe or duplicate archive member: ' + name)
            if item.file_size > MAX_MEMBER:
                raise ValueError('Archive member exceeds its bound: ' + name)
            out[name] = zipped.read(item)
    return raw, out


def qualified_pair(release, sdk, release_hash, sdk_hash, root=ROOT):
    root = Path(root)
    identity = schema_identity.capture(root)
    table_bytes = (root / 'game/table.yaml').read_bytes()
    builds = game_table.load_builds(root / 'game/table.yaml')
    table = builds[0]
    version, build_id = table['version'], table['build_id'].upper()
    release_name = 'Tsuchi-' + version + '.zip'
    if version != '1.1.1':
        raise ValueError('Download page targets Pikmin 4 version 1.1.1')
    release_raw, production = archive(release, release_hash)
    sdk_raw, author = archive(sdk, sdk_hash)
    report = json.loads(production.get('release.json', b'{}'))
    if not isinstance(report, dict) or not isinstance(report.get('files'), dict):
        raise ValueError('Production release inventory must be a mapping')
    # Releases before configurable slots used subsdk9 and did not name it here.
    module_file = package_release.module_path(report.get('module_file', 'exefs/subsdk9'))
    emulator_module = 'emulator/Tsuchi/' + module_file
    console_module = 'console/atmosphere/contents/' + package_release.title_of(table) + '/' + module_file
    console_modules = {'console/atmosphere/contents/' + package_release.title_of(each) + '/' + module_file for each in builds}
    paths = {emulator_module, *console_modules,
             *['emulator/Tsuchi/exefs/' + each['key'] + '.pchtxt' for each in builds],
             *['console/atmosphere/exefs_patches/Tsuchi/' + each['build_id'].upper() + '.ips' for each in builds],
             'INSTALL-CONSOLE.md', 'INSTALL-EMULATOR.md', 'THIRD-PARTY-NOTICES.txt', 'release.json'}
    if set(production) != paths:
        raise ValueError('Production archive differs from the allowlist of its files')
    if (report.get('format') != 1 or report.get('selfcheck') is not False or
            report.get('game_version') != version or report.get('game_build_id') != build_id or
            report.get('game_table_sha256') != digest(table_bytes) or
            report.get('patch_hash') != f'{stubs.patch_hash(table):016x}'):
        raise ValueError('Production release version, table, or patch identity differs')
    if type(report.get('api_version')) is not int or any(report.get(key) != value for key, value in identity.items()):
        raise ValueError('Production archive uses another API schema')
    if set(report.get('files', {})) != paths - {'release.json'}:
        raise ValueError('Production inventory members differ')
    for name, entry in report['files'].items():
        if entry != {'bytes': len(production[name]), 'sha256': digest(production[name])}:
            raise ValueError('Production inventory hash differs: ' + name)
    if report.get('builds') != [dict(key=each['key'], version=each['version'], build_id=each['build_id'].upper(),
                                     program_id=package_release.title_of(each), patch_hash=f'{stubs.patch_hash(each):016x}') for each in builds]:
        raise ValueError('Production release lists other builds of the game')
    module = production[emulator_module]
    if (len(module) < 0x100 or module[:4] != b'NSO0' or
            any(module != production[name] for name in console_modules) or
            digest(module) != report.get('module_sha256') or
            module[0x40:0x60].hex() != report.get('elf_sha256')):
        raise ValueError('Production module/ELF identity differs')
    package_release.reject_private_paths(production)
    inventory = json.loads(author.get(package_sdk.INVENTORY, b'{}'))
    if not isinstance(inventory, dict) or not isinstance(inventory.get('inputs'), dict):
        raise ValueError('SDK input inventory must be a mapping')
    schema_inputs = {name: value for name, value in inventory.get('inputs', {}).items()
                     if name.startswith('schema/')}
    if inventory.get('api_version') != report['api_version'] or schema_inputs != report['schema_inputs']:
        raise ValueError('Production and SDK API identities differ')
    # Rebuild the SDK payload in memory using the current schema and support files.
    # Its header list follows the schema rather than a pinned service count.
    expected = package_sdk.members(root)
    if author != expected:
        raise ValueError('SDK headers or payload differ from the current SDK recipe')
    schema_identity.require_current(root, identity)
    receipt = {'format': 1, **identity, 'game_version': version, 'game_build_id': build_id,
               'release': {'filename': release_name, 'sha256': digest(release_raw),
                           'module_file': module_file, 'module_sha256': digest(module), 'elf_sha256': report['elf_sha256'],
                           'members': len(production)},
               'sdk': {'filename': SDK_NAME, 'sha256': digest(sdk_raw), 'members': len(author)}}
    return {release_name: release_raw, SDK_NAME: sdk_raw}, receipt


class Links(HTMLParser):
    def __init__(self):
        super().__init__()
        self.hrefs = set()

    def handle_starttag(self, tag, attrs):
        if tag == 'a':
            self.hrefs.update(value for key, value in attrs if key == 'href' and value)


def check_pages(built, names):
    built = Path(built)
    if built.is_symlink() or not (built / 'index.html').is_file():
        raise ValueError('An existing built documentation folder is required')
    for page in PAGES:
        path = built / page / 'index.html'
        if not path.is_file() or path.is_symlink() or path.stat().st_size > MAX_MEMBER:
            raise ValueError('Built delivery page is missing or invalid: ' + page)
        links = Links()
        links.feed(path.read_text(encoding='utf-8'))
        wanted = {'/downloads/' + name for name in names} if page == 'downloads' else {'/downloads/'}
        if not wanted <= links.hrefs:
            raise ValueError('Built delivery links differ: ' + page)


def populate(built, payload, receipt):
    """Stage and verify the whole pair, then replace files atomically, receipt last.

    The receipt is the commit record: readers must verify its archive hashes.
    Hard-linked backups permit rollback without copying existing archives.
    """
    built = Path(built)
    check_pages(built, payload)
    folder = built / 'downloads'
    if folder.is_symlink():
        raise ValueError('Download folder is linked')
    for path in (folder, *folder.parents):
        if package_sdk.linked(path):
            raise ValueError('Built delivery path is linked')
    lock = folder / '.tsuchi-delivery.lock'
    descriptor = os.open(lock, os.O_CREAT | os.O_EXCL | os.O_WRONLY, 0o600)
    os.close(descriptor)
    try:
        with tempfile.TemporaryDirectory(prefix='.tsuchi-delivery-', dir=folder) as temporary:
            staging = Path(temporary)
            complete = {**payload, RECEIPT: (json.dumps(receipt, sort_keys=True, indent=2) + '\n').encode('utf-8')}
            backups, changed = {}, []
            for name, data in complete.items():
                target = folder / name
                if target.is_symlink() or (target.exists() and package_sdk.linked(target)):
                    raise ValueError('Download target is linked: ' + name)
                if target.exists():
                    if not target.is_file():
                        raise ValueError('Download target is linked or not a file: ' + name)
                    backup = staging / ('old-' + name)
                    os.link(target, backup)
                    backups[name] = backup
                pending = staging / name
                pending.write_bytes(data)
                if pending.read_bytes() != data:
                    raise ValueError('Staged download readback differs: ' + name)
            try:
                for name in complete:  # receipt is last
                    os.replace(staging / name, folder / name)
                    changed.append(name)
                    if (folder / name).read_bytes() != complete[name]:
                        raise ValueError('Installed download readback differs: ' + name)
            except BaseException:
                for name in reversed(changed):
                    if name in backups:
                        os.replace(backups[name], folder / name)
                    else:
                        (folder / name).unlink()
                raise
    finally:
        lock.unlink()
    return receipt


def package(built, release, sdk, release_hash, sdk_hash, root=ROOT):
    payload, receipt = qualified_pair(release, sdk, release_hash, sdk_hash, root)
    return populate(built, payload, receipt)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--built', required=True, type=Path)
    parser.add_argument('--release', required=True, type=Path)
    parser.add_argument('--sdk', required=True, type=Path)
    parser.add_argument('--release-sha256', required=True)
    parser.add_argument('--sdk-sha256', required=True)
    args = parser.parse_args()
    try:
        receipt = package(args.built, args.release, args.sdk, args.release_sha256, args.sdk_sha256)
        print(json.dumps(receipt, sort_keys=True))
    except (ValueError, OSError, zipfile.BadZipFile, KeyError) as error:
        parser.exit(1, str(error) + '\n')


if __name__ == '__main__':
    main()
