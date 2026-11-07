"""Build the author SDK from the current schema, without generating repository outputs."""
import argparse
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import re
import sys
import tempfile
import zipfile

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
from gen import c_header, load, typescript
import schema_identity

RESOURCES = ('build_mod.py', 'support.c', 'start.S', 'mod.ld', 'libc/string.h',
             'check/check.lua', 'check/tsuchi_check.h', 'README.md',
             'examples/native-hooks/README.md', 'examples/native-hooks/mod.json',
             'examples/native-hooks/main.c', 'examples/native-hooks/targets.example.h')
EXAMPLES = ('mod.json', 'main.c', 'main.lua', 'main.js')
GUIDE = 'site/src/content/docs/develop/first-mod.mdx'
INVENTORY = 'sdk/inventory.json'
MAX_MEMBER = 2 * 1024 * 1024
MAX_TOTAL = 8 * 1024 * 1024
MAX_MEMBERS = 128


def digest(data):
    return hashlib.sha256(data).hexdigest()


def linked(path):
    return path.is_symlink() or bool(getattr(path.lstat(), 'st_file_attributes', 0) & 0x400)


def read_source(root, name):
    path = root / name
    # Every input is a small, explicit source file. Do not follow linked inputs.
    if not path.is_file():
        raise ValueError('SDK input is missing or too large: ' + name)
    if any(linked(candidate) for candidate in (path, *path.parents)):
        raise ValueError('SDK input is a linked path: ' + name)
    if not path.is_file() or path.stat().st_size > MAX_MEMBER:
        raise ValueError('SDK input is missing or too large: ' + name)
    data = path.read_bytes()
    if len(data) > MAX_MEMBER:
        raise ValueError('SDK input grew beyond its bound: ' + name)
    return data


def examples(source):
    out = {}
    for name, language in (('mod.json', 'json'), ('main.c', 'c'), ('main.lua', 'lua'), ('main.js', 'javascript')):
        pattern = r'^```' + language + r' title="' + re.escape(name) + r'"[ \t]*\n(.*?)^```[ \t]*$'
        matches = re.findall(pattern, source.replace('\r\n', '\n'), re.M | re.S)
        if len(matches) != 1:
            raise ValueError('First-mod guide must contain one example: ' + name)
        out[name] = matches[0].encode('utf-8')
    manifest = json.loads(out['mod.json'])
    if manifest != {'id': 'olimar.clock', 'name': 'Clock', 'version': '1.0.0'}:
        raise ValueError('First-mod manifest identity changed, review the SDK layout')
    return out


def validate_payload(out, expected, root):
    if set(out) != set(expected) or len(out) > MAX_MEMBERS:
        raise ValueError('SDK members differ from its explicit allowlist')
    if sum(map(len, out.values())) > MAX_TOTAL:
        raise ValueError('SDK payload exceeds its bound')
    roots = {str(root.resolve()).encode('utf-8'), root.resolve().as_posix().encode('utf-8')}
    local = re.compile(rb'(?:[A-Za-z]:[\\/](?:Users|home)[\\/]|/(?:home|Users|tmp)/|(?:^|[\s\x00"\x27`])(?:private|\.git)[\\/])', re.I)
    for name, data in out.items():
        path = PurePosixPath(name)
        if not name.startswith('sdk/') or path.as_posix() != name or '..' in path.parts or '\\' in name or ':' in name:
            raise ValueError('Unsafe SDK member path: ' + name)
        if not isinstance(data, bytes) or len(data) > MAX_MEMBER:
            raise ValueError('SDK member is invalid or too large: ' + name)
        if any(value.lower() in data.lower() for value in roots) or local.search(data):
            raise ValueError('SDK member contains a private path: ' + name)


def members(root=ROOT):
    root = Path(root).resolve()
    inputs = {}
    names = ['schema/core.yaml', 'schema/abi.lock']
    names += [path.relative_to(root).as_posix() for path in sorted((root / 'schema').glob('*.yaml'))
              if path.name != 'core.yaml']
    for name in names:
        inputs[name] = read_source(root, name)
    schema = load(root / 'schema')
    headers = c_header.generate(schema)
    wanted_headers = {'include/tsuchi/core.h', 'include/tsuchi/tsuchi.h'}
    wanted_headers.update('include/tsuchi/' + service.name + '.h' for service in schema.services)
    if set(headers) != wanted_headers:
        raise ValueError('C header generator returned unexpected members')
    # A concurrent schema edit must not produce a mixed input receipt.
    if any(read_source(root, name) != data for name, data in inputs.items()):
        raise ValueError('SDK schema changed while generating its headers')
    out = {'sdk/' + name: text.encode('utf-8') for name, text in headers.items()}
    for name in RESOURCES:
        key = 'sdk/' + name
        out[key] = inputs[key] = read_source(root, key)
    inputs[GUIDE] = read_source(root, GUIDE)
    for name, data in examples(inputs[GUIDE].decode('utf-8')).items():
        out['sdk/examples/olimar.clock/' + name] = data
    out['sdk/tsuchi.d.ts'] = typescript.declaration(schema).encode('utf-8')
    expected = {'sdk/tsuchi.d.ts'} | {'sdk/' + name for name in wanted_headers}
    expected.update('sdk/' + name for name in RESOURCES)
    expected.update('sdk/examples/olimar.clock/' + name for name in EXAMPLES)
    validate_payload(out, expected, root)
    release = schema_identity.release_version(root)
    inventory = {
        'format': 1, 'api_version': schema.api_version,
        **({} if release is None else {'release_version': release}),
        'inputs': {name: digest(data) for name, data in sorted(inputs.items())},
        'files': {name: {'bytes': len(data), 'sha256': digest(data)} for name, data in sorted(out.items())},
    }
    # The inventory describes every payload member and deliberately excludes itself.
    out[INVENTORY] = (json.dumps(inventory, sort_keys=True, indent=2) + '\n').encode('utf-8')
    validate_payload(out, expected | {INVENTORY}, root)
    return out


def verify_archive(path, wanted):
    with zipfile.ZipFile(path) as archive:
        if len(archive.namelist()) != len(wanted) or set(archive.namelist()) != set(wanted) or archive.testzip():
            raise ValueError('SDK archive members differ')
        if any(archive.read(name) != data for name, data in wanted.items()):
            raise ValueError('SDK archive bytes differ')
        inventory = json.loads(archive.read(INVENTORY))
        if set(inventory['files']) != set(wanted) - {INVENTORY}:
            raise ValueError('SDK inventory members differ')
        for name, entry in inventory['files'].items():
            data = archive.read(name)
            if entry != {'bytes': len(data), 'sha256': digest(data)}:
                raise ValueError('SDK inventory hash differs: ' + name)


def package(archive, root=ROOT, prerelease=False):
    archive = schema_identity.archive_path(root, archive, prerelease)
    wanted = members(root)
    archive.parent.mkdir(parents=True, exist_ok=True)
    handle, pending_name = tempfile.mkstemp(prefix=archive.name + '.', suffix='.pending.zip', dir=archive.parent)
    os.close(handle)
    pending = Path(pending_name)
    try:
        with zipfile.ZipFile(pending, 'w', compression=zipfile.ZIP_DEFLATED, compresslevel=6) as output:
            for name, data in sorted(wanted.items()):
                info = zipfile.ZipInfo(name, date_time=(1980, 1, 1, 0, 0, 0))
                info.create_system = 3
                info.compress_type = zipfile.ZIP_DEFLATED
                info.external_attr = 0o100644 << 16
                output.writestr(info, data, compresslevel=6)
        verify_archive(pending, wanted)
        os.replace(pending, archive)
    finally:
        pending.unlink(missing_ok=True)
    return {'archive': str(archive), 'archive_sha256': digest(archive.read_bytes()), 'bytes': archive.stat().st_size,
            'members': len(wanted), 'api_version': json.loads(wanted[INVENTORY])['api_version']}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--out', required=True, type=Path)
    parser.add_argument('--prerelease', action='store_true',
                        help='package with an unreleased ABI lock; the archive name says prerelease')
    args = parser.parse_args()
    try:
        print(json.dumps(package(args.out, prerelease=args.prerelease), sort_keys=True))
    except (ValueError, OSError) as error:
        parser.exit(1, str(error) + '\n')


if __name__ == '__main__':
    main()
