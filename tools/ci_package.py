"""Build source-bound candidate archives with checksums and a publication receipt."""
import argparse
import hashlib
import importlib.metadata
import json
from pathlib import Path
import platform
import re
import shutil
import stat
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
RECEIPT = 'provenance.json'
CHECKSUMS = 'SHA256SUMS'


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def git(root, *args):
    result = subprocess.run(['git', '--no-optional-locks', '-C', str(root), *args],
                            capture_output=True, text=True, encoding='utf-8', check=False)
    if result.returncode:
        raise ValueError('Git source identity could not be read')
    return result.stdout.strip()


def source_identity(root):
    root = Path(root).resolve()
    if Path(git(root, 'rev-parse', '--show-toplevel')).resolve() != root:
        raise ValueError('The source root must be the Git checkout root')
    if git(root, 'status', '--porcelain=v1', '--untracked-files=all', '--ignore-submodules=none'):
        raise ValueError('Candidate packaging requires a clean source checkout, including untracked files')
    commit = git(root, 'rev-parse', 'HEAD')
    tree = git(root, 'rev-parse', 'HEAD^{tree}')
    if not all(re.fullmatch(r'[0-9a-f]{40}|[0-9a-f]{64}', value) for value in (commit, tree)):
        raise ValueError('Git returned an invalid source identity')
    return {'commit': commit, 'tree': tree}


def release_identity(root, tag=None):
    lock = json.loads((Path(root) / 'schema/abi.lock').read_text(encoding='utf-8'))
    version = lock.get('release_version')
    if not isinstance(version, str) or not re.fullmatch(r'(0|[1-9][0-9]*)(\.(0|[1-9][0-9]*)){2}', version):
        raise ValueError('schema/abi.lock must name a release_version such as 0.2.0')
    released = lock.get('released') is True
    prerelease = True
    if tag is not None:
        match = re.fullmatch(r'v' + re.escape(version) + r'(?:-([0-9A-Za-z-]+(?:\.[0-9A-Za-z-]+)*))?', tag)
        if not match or (match.group(1) and any(
                part.isdigit() and len(part) > 1 and part.startswith('0')
                for part in match.group(1).split('.'))):
            raise ValueError('The tag must match release_version, with an optional SemVer prerelease suffix')
        prerelease = match.group(1) is not None
        if not released and not prerelease:
            raise ValueError('An unreleased ABI lock requires a prerelease tag')
    return {'release_version': version, 'prerelease': prerelease, 'tag': tag}


def supported_builds(root):
    from gen import game_table
    path = Path(root) / 'game/table.yaml'
    tables = game_table.load_builds(path) if hasattr(game_table, 'load_builds') else [game_table.load(path)]
    return [{'key': table.get('key', table['version']), 'version': table['version'],
             'build_id': table['build_id'].upper(),
             'program_id': f"{table['constants']['program_id']:016X}"} for table in tables]


def tools_identity():
    versions = {'python': platform.python_version(), 'python_implementation': platform.python_implementation()}
    for name in ('clang', 'ld.lld'):
        executable = shutil.which(name)
        if not executable:
            raise ValueError(name + ' is required for candidate provenance')
        result = subprocess.run([executable, '--version'], capture_output=True, text=True, check=False)
        if result.returncode or not result.stdout.strip():
            raise ValueError('Required tool version could not be read: ' + name)
        versions[name] = result.stdout.splitlines()[0].strip()
    versions['python_packages'] = {name: importlib.metadata.version(name) for name in ('PyYAML', 'unicorn')}
    return versions


def output_path(root, value):
    path = Path(value)
    if not path.is_absolute():
        path = Path(root) / path
    build = (Path(root) / 'build').resolve()
    if path.is_symlink() or not path.resolve().is_relative_to(build) or path.resolve() == build:
        raise ValueError('Candidate outputs must be in a directory inside build/')
    for ancestor in (path, *path.parents):
        if ancestor == Path(root):
            break
        if ancestor.is_symlink() or (ancestor.exists() and
                bool(getattr(ancestor.lstat(), 'st_file_attributes', 0) & 0x400)):
            raise ValueError('Candidate outputs cannot use linked directories')
    return path


def checked_archive(result, expected):
    if Path(result.get('archive', '')).resolve() != expected.resolve() or not expected.is_file() or expected.is_symlink():
        raise ValueError('Packager returned an unexpected archive')
    actual = digest(expected)
    if result.get('archive_sha256') != actual:
        raise ValueError('Packager archive hash changed')
    return {'bytes': expected.stat().st_size, 'sha256': actual}


def package(root=ROOT, module='build/module', out='build/ci/candidate', build=False, tag=None):
    import build_module
    import package_release
    import package_sdk
    import schema_identity
    root = Path(root).resolve()
    source = source_identity(root)
    release = release_identity(root, tag)
    if tag is not None and not build:
        raise ValueError('Tag candidates require --build to bind the module to this source commit')
    identity = schema_identity.capture(root)
    builds = supported_builds(root)
    versions = tools_identity()
    out = output_path(root, out)
    module = output_path(root, module)
    archives = [schema_identity.archive_path(root, out / ('Tsuchi-' + release['release_version'] + '.zip'), release['prerelease']),
                schema_identity.archive_path(root, out / ('Tsuchi-SDK-' + release['release_version'] + '.zip'), release['prerelease'])]
    expected_names = {path.name for path in archives} | {RECEIPT, CHECKSUMS, 'LICENSE', 'THIRD-PARTY-NOTICES.txt'}
    out.mkdir(parents=True, exist_ok=True)
    if any(path.name not in expected_names or not path.is_file() or path.is_symlink() for path in out.iterdir()):
        raise ValueError('Candidate output contains files from another candidate; use an empty output directory')
    # Completion files never survive an unsuccessful rebuild of this candidate.
    (out / RECEIPT).unlink(missing_ok=True)
    (out / CHECKSUMS).unlink(missing_ok=True)
    if build:
        build_module.build(module)
    if source_identity(root) != source:
        raise ValueError('Source changed while building the candidate')
    files = {}
    files[archives[0].name] = checked_archive(
        package_release.package(module, archives[0], prerelease=release['prerelease']), archives[0])
    files[archives[1].name] = checked_archive(
        package_sdk.package(archives[1], root=root, prerelease=release['prerelease']), archives[1])
    for name, path in (('LICENSE', root / 'LICENSE'),
                       ('THIRD-PARTY-NOTICES.txt', root / 'tools/Tsuchi-ThirdPartyNotices.txt')):
        if not path.is_file() or path.is_symlink():
            raise ValueError('Candidate notice is missing or linked: ' + name)
        data = path.read_bytes()
        (out / name).write_bytes(data)
        files[name] = {'bytes': len(data), 'sha256': hashlib.sha256(data).hexdigest()}
    if source_identity(root) != source:
        raise ValueError('Source changed while packaging the candidate')
    schema_identity.require_current(root, identity)
    receipt = {'format': 1, **release, 'source': source, 'tools': versions, **identity,
               'game_builds': builds, 'build_execution': 'built' if build else 'provided',
               'qualification': 'host build; game and console qualification is separate', 'files': dict(sorted(files.items()))}
    raw = (json.dumps(receipt, indent=2, sort_keys=True) + '\n').encode('utf-8')
    package_release.reject_private_paths({RECEIPT: raw})
    # Recheck payloads before writing the receipt and its checksum as completion markers.
    if any(digest(out / name) != data['sha256'] for name, data in files.items()):
        raise ValueError('Candidate payload changed before receipt publication')
    (out / RECEIPT).write_bytes(raw)
    checksums = {name: data['sha256'] for name, data in files.items()}
    checksums[RECEIPT] = hashlib.sha256(raw).hexdigest()
    (out / CHECKSUMS).write_text(''.join(value + '  ' + name + '\n' for name, value in sorted(checksums.items())), encoding='utf-8')
    return {'release_version': release['release_version'], 'prerelease': release['prerelease'],
            'source_commit': source['commit'], 'files': sorted(expected_names)}


def verify(root, folder, tag, notes=None):
    """Check a downloaded candidate with the standard library and the checked-out tag."""
    root = Path(root).resolve()
    folder = output_path(root, folder)
    release = release_identity(root, tag)
    if tag is None:
        raise ValueError('Candidate verification requires --tag')
    source = source_identity(root)
    if git(root, 'rev-parse', '--verify', 'refs/tags/' + tag + '^{commit}') != source['commit']:
        raise ValueError('The release tag does not point to the checked-out source commit')
    receipt_path = folder / RECEIPT
    if not receipt_path.is_file() or receipt_path.is_symlink() or receipt_path.stat().st_size > 1024 * 1024:
        raise ValueError('Candidate receipt is missing, linked, or too large')
    raw = receipt_path.read_bytes()
    receipt = json.loads(raw)
    fields = {'format', 'release_version', 'prerelease', 'tag', 'source', 'tools', 'api_version',
              'schema_inputs', 'game_builds', 'build_execution', 'qualification', 'files'}
    if not isinstance(receipt, dict) or set(receipt) != fields or receipt.get('format') != 1:
        raise ValueError('Candidate receipt has an unsupported format')
    versions = receipt.get('tools')
    if not isinstance(versions, dict) or not {'python', 'clang', 'ld.lld'} <= set(versions) or any(
            not isinstance(versions[name], str) or not versions[name] for name in ('python', 'clang', 'ld.lld')):
        raise ValueError('Candidate tool versions are missing')
    if any(receipt.get(key) != value for key, value in release.items()) or type(receipt.get('prerelease')) is not bool:
        raise ValueError('Candidate release identity does not match the tag and ABI lock')
    if receipt.get('source') != source or receipt.get('build_execution') != 'built':
        raise ValueError('Candidate was not built from the checked-out source commit')
    lock = json.loads((root / 'schema/abi.lock').read_text(encoding='utf-8'))
    if type(receipt.get('api_version')) is not int or receipt['api_version'] != lock.get('api_version'):
        raise ValueError('Candidate API identity differs from the ABI lock')
    schema_paths = sorted((root / 'schema').glob('*.yaml')) + [root / 'schema/abi.lock']
    schema_inputs = {'schema/' + path.name: digest(path) for path in schema_paths}
    if receipt.get('schema_inputs') != schema_inputs:
        raise ValueError('Candidate schema hashes differ from the checked-out source')
    suffix = '-prerelease' if release['prerelease'] else ''
    expected_payload = {'Tsuchi-' + release['release_version'] + suffix + '.zip',
                        'Tsuchi-SDK-' + release['release_version'] + suffix + '.zip',
                        'LICENSE', 'THIRD-PARTY-NOTICES.txt'}
    files = receipt.get('files')
    if not isinstance(files, dict) or set(files) != expected_payload:
        raise ValueError('Candidate payload inventory differs from the expected archives and notices')
    expected_names = expected_payload | {RECEIPT, CHECKSUMS}
    paths = list(folder.iterdir())
    if {path.name for path in paths} != expected_names or any(
            not stat.S_ISREG(path.lstat().st_mode) or path.is_symlink() or
            bool(getattr(path.lstat(), 'st_file_attributes', 0) & 0x400) for path in paths):
        raise ValueError('Candidate files differ from the inventory or contain linked inputs')
    checksums = {}
    for name, entry in files.items():
        path = folder / name
        if not isinstance(entry, dict) or set(entry) != {'bytes', 'sha256'} or \
                type(entry.get('bytes')) is not int or entry['bytes'] != path.stat().st_size or \
                not isinstance(entry.get('sha256'), str) or not re.fullmatch('[0-9a-f]{64}', entry['sha256']) or \
                digest(path) != entry['sha256']:
            raise ValueError('Candidate payload hash or size differs: ' + name)
        checksums[name] = entry['sha256']
    for name, path in (('LICENSE', root / 'LICENSE'),
                       ('THIRD-PARTY-NOTICES.txt', root / 'tools/Tsuchi-ThirdPartyNotices.txt')):
        if checksums[name] != digest(path):
            raise ValueError('Candidate notices differ from the checked-out source')
    checksums[RECEIPT] = hashlib.sha256(raw).hexdigest()
    expected_sums = ''.join(value + '  ' + name + '\n' for name, value in sorted(checksums.items()))
    if (folder / CHECKSUMS).read_text(encoding='utf-8') != expected_sums:
        raise ValueError('SHA256SUMS differs from the verified candidate files')
    builds = receipt.get('game_builds')
    if not isinstance(builds, list) or not builds or any(
            not isinstance(build, dict) or set(build) != {'key', 'version', 'build_id', 'program_id'} or
            any(not isinstance(build.get(key), str) or not re.fullmatch(r'[A-Za-z0-9_.-]{1,64}', build[key])
                for key in ('key', 'version')) or
            not isinstance(build.get('build_id'), str) or not re.fullmatch(r'[0-9A-F]{32}|[0-9A-F]{64}', build['build_id']) or
            not isinstance(build.get('program_id'), str) or not re.fullmatch(r'[0-9A-F]{16}', build['program_id']) for build in builds):
        raise ValueError('Candidate supported-build list is malformed')
    if notes is not None:
        path = output_path(root, notes)
        if path.resolve().is_relative_to(folder.resolve()):
            raise ValueError('Release notes must be outside the candidate payload folder')
        path.parent.mkdir(parents=True, exist_ok=True)
        lines = ['# Tsuchi ' + release['release_version'], '',
                 'Source commit: `' + source['commit'] + '`.', '',
                 'This draft contains a build candidate. Hosted checks do not establish emulator or physical console qualification.',
                 '', 'Supported game builds:', '']
        for build in builds:
            lines.append('- `' + build['key'] + '` (' + build['version'] + '), program `' +
                         build['program_id'] + '`, build `' + build['build_id'] + '`.')
        lines += ['', 'Install the module and matching patch together. Keep the candidate receipts and checksums with the archives.', '']
        path.write_text('\n'.join(lines), encoding='utf-8')
    return receipt


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--module', default='build/module', help='production module folder inside build/')
    parser.add_argument('--out', default='build/ci/candidate', help='candidate folder inside build/')
    parser.add_argument('--build', action='store_true', help='build production from this clean source commit before packaging')
    parser.add_argument('--tag', help='version tag for a draft release')
    parser.add_argument('--verify', help='verify a downloaded candidate using only the Python standard library')
    parser.add_argument('--notes', help='write verified release notes inside build/, outside the candidate folder')
    args = parser.parse_args()
    try:
        if args.verify:
            receipt = verify(ROOT, args.verify, args.tag, args.notes)
            print(json.dumps({'source_commit': receipt['source']['commit'], 'tag': receipt['tag'],
                              'prerelease': receipt['prerelease']}, sort_keys=True))
        else:
            if args.notes:
                raise ValueError('--notes requires --verify')
            print(json.dumps(package(module=args.module, out=args.out, build=args.build, tag=args.tag), sort_keys=True))
    except (ValueError, OSError, subprocess.SubprocessError, importlib.metadata.PackageNotFoundError) as error:
        parser.exit(1, str(error) + '\n')


if __name__ == '__main__':
    main()
