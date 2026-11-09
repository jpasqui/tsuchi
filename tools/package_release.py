"""Package one production module with the matching emulator and console patches of each build.

The archive contains only framework binaries, install instructions, third-party notices, and hashes.
It is a candidate until the repository's release qualification is complete.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import sys
import struct
import tempfile
from urllib.parse import urlsplit, urlunsplit
import zipfile
import schema_identity

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
DOCS_BASE = 'https://tsuchi.jpasqui.dev'


def digest(data):
    return hashlib.sha256(data).hexdigest()


def title_of(table):
    """The program ID of a build, as folder names spell it."""
    return f"{table['constants']['program_id']:016X}"


def first_title():
    import yaml
    return f"{yaml.safe_load((ROOT / 'game/table.yaml').read_text(encoding='utf-8'))['constants']['program_id']:016X}"


TITLE = first_title()


def guide_link(target):
    if not target.startswith('/') or target.startswith('//'):
        return target
    parts = urlsplit(target)
    local = {'/players/install-console': 'INSTALL-CONSOLE.md',
             '/players/install-tsuchi': 'INSTALL-EMULATOR.md'}.get(parts.path.rstrip('/'))
    if local:
        return urlunsplit(('', '', local, parts.query, parts.fragment))
    return DOCS_BASE + target


def render_guide(source):
    """Render the install pages as standalone Markdown with usable links."""
    import yaml
    text = source.replace('\r\n', '\n').replace('\r', '\n')
    title = ''
    if text.startswith('---\n'):
        end = text.find('\n---\n', 4)
        if end < 0:
            raise ValueError('Install guide front matter is not closed')
        meta = yaml.safe_load(text[4:end]) or {}
        if not isinstance(meta, dict):
            raise ValueError('Install guide front matter must be a mapping')
        title = meta.get('title', '')
        if title and (not isinstance(title, str) or '\n' in title):
            raise ValueError('Install guide title must be one line')
        text = text[end + 5:]
    # The installation page uses only Starlight's Steps wrapper. Refuse other
    # MDX dependencies instead of shipping instructions that require the site.
    lines = []
    fence = None
    aside = False
    source_lines = text.splitlines()
    index = 0
    while index < len(source_lines):
        line = source_lines[index]
        index += 1
        mark = re.match(r'^\s{0,3}(`{3,}|~{3,})', line)
        if mark:
            token = mark.group(1)
            if fence is None:
                fence = token
            elif token[0] == fence[0] and len(token) >= len(fence):
                fence = None
            lines.append(line)
            continue
        if fence:
            lines.append(line)
            continue
        # A Starlight aside, such as `:::caution[WIP]`, becomes a quoted block with its title in bold.
        opening = re.fullmatch(r'[ \t]{0,3}:::(note|tip|caution|danger)(?:\[([^\]\n]*)\])?[ \t]*', line)
        if opening and not aside:
            aside = True
            lines.append('> **' + (opening.group(2) or opening.group(1).capitalize()) + '**')
            lines.append('>')
            continue
        if aside and re.fullmatch(r'[ \t]{0,3}:::[ \t]*', line):
            aside = False
            continue
        if re.match(r'[ \t]*import\s', line):
            remaining = '\n'.join(source_lines[index - 1:])
            match = re.match(r"[ \t]*import\s+\{\s*Steps\s*\}\s+from\s+['\"]@astrojs/starlight/components['\"];?[ \t]*(?:\n|$)", remaining)
            if not match:
                raise ValueError('Install guide has an unsupported MDX import')
            index += max(1, match.group(0).count('\n')) - 1
            continue
        if re.fullmatch(r'[ \t]*</?Steps>[ \t]*', line):
            continue
        if re.match(r'[ \t]*export\s', line) or re.search(r'</?[A-Z][A-Za-z0-9]*(?:\s|/?>)', line):
            raise ValueError('Install guide has an unsupported MDX component')
        # Inline code stays literal. Markdown destinations can be plain or
        # angle-bracketed, and reference-style links are handled as well.
        pieces = re.split(r'(`+[^`]*`+)', line)
        for piece_index in range(0, len(pieces), 2):
            pieces[piece_index] = re.sub(r'(\]\()(<)?(/[^\s)>]+)(?(2)>|)',
                lambda match: match.group(1) + ('<' if match.group(2) else '') +
                guide_link(match.group(3)) + ('>' if match.group(2) else ''), pieces[piece_index])
        line = ''.join(pieces)
        line = re.sub(r'^(\s{0,3}\[[^]\n]+\]:\s*)(<)?(/[^\s>]+)(?(2)>|)',
            lambda match: match.group(1) + ('<' if match.group(2) else '') +
            guide_link(match.group(3)) + ('>' if match.group(2) else ''), line)
        lines.append(('> ' + line).rstrip() if aside else line)
    if aside:
        raise ValueError('Install guide aside is not closed')
    body = '\n'.join(lines).strip()
    return (('# ' + title + '\n\n' if title else '') + body + '\n').encode('utf-8')


def module_path(value):
    """One controlled ExeFS module, including when a release uses a different slot."""
    if not isinstance(value, str) or not re.fullmatch(r'exefs/subsdk[0-9]', value):
        raise ValueError('Production module path must be exefs/subsdk0 through exefs/subsdk9')
    return value


def install_guide(source, module):
    guide = render_guide(source).decode('utf-8')
    name = module_path(module).split('/')[1]
    if name == 'subsdk9':
        return guide.encode('utf-8')
    guide = guide.replace('subsdk9', name)
    # A custom-slot archive is an explicit installation choice. Installing it
    # over an older archive must not leave two copies of Tsuchi loaded.
    note = ('This archive installs Tsuchi as `' + name + '`. Use this slot only when it is free. '
            'When changing Tsuchi slots, remove the previous Tsuchi module before starting the game. '
            'Keep every foreign module at its original filename. Configure the external manager with '
            '`--tsuchi-slot ' + name[-1] + '` for this installation.\n\n')
    if guide.startswith('# ') and '\n\n' in guide:
        heading, guide = guide.split('\n\n', 1)
        guide = heading + '\n\n' + note + guide
    else:
        guide = note + guide
    return guide.encode('utf-8')


def validate_module(folder, report):
    import build_module
    module = (folder / module_path(report.get('file'))).read_bytes()
    if len(module) != report.get('bytes') or digest(module) != report.get('sha256') or module[:4] != b'NSO0':
        raise ValueError('Production module differs from its build report')
    elf = (folder / 'tsuchi.elf').read_bytes()
    elf_hash = digest(elf)
    if module[0x40:0x60] != hashlib.sha256(elf).digest() or report.get('elf_sha256', elf_hash) != elf_hash:
        raise ValueError('Production module is not bound to the linked ELF')
    try:
        loads, sections, symbols = build_module.read_elf(elf)
        if any('selfcheck_start' in name or 'selfcheck_frame' in name for name in symbols):
            raise ValueError('A module with native selfchecks cannot be released')
        memory = build_module.check(elf, loads, sections, symbols)
        reproduced = build_module.module_file(elf, loads, sections)
    except (SystemExit, KeyError, IndexError, UnicodeError, ValueError, struct.error) as error:
        raise ValueError('Production ELF validation failed: ' + str(error)) from error
    if report.get('memory_bytes') != memory or module != reproduced:
        raise ValueError('Production module does not reproduce from its linked ELF')
    return module, elf_hash


def reject_private_paths(out):
    roots = {str(ROOT.resolve()).encode('utf-8'), ROOT.resolve().as_posix().encode('utf-8')}
    local = re.compile(rb'(?:[A-Za-z]:[\\/](?:Users|home)[\\/]|(?:^|[\s\x00\"\x27`])/(?:home|Users)/|(?:^|[\s\x00\"\x27`])(?:private|\.git)[\\/])', re.I)
    for name, data in out.items():
        if any(root.lower() in data.lower() for root in roots) or local.search(data):
            raise ValueError('Release member contains a private path: ' + name)


def members(folder):
    from gen import game_table, stubs
    import build_patch
    import process_metadata
    folder = Path(folder)
    report = json.loads((folder / 'report.json').read_text(encoding='utf-8'))
    table_bytes = (ROOT / 'game/table.yaml').read_bytes()
    builds = game_table.load_builds(ROOT / 'game/table.yaml')
    table = builds[0]
    if report.get('selfcheck') is not False:
        raise ValueError('A production build report with selfcheck=false is required')
    if report.get('native_code_access') or any(folder.rglob('*.npdm')):
        raise ValueError('Local process metadata cannot be released')
    import build_module
    if report.get('game_table_sha256') != digest(table_bytes) or report.get('game_builds_sha256') != build_module.builds_digest(ROOT):
        raise ValueError('The production build uses another game table')
    identity = schema_identity.capture(ROOT)
    if type(report.get('api_version')) is not int or any(report.get(key) != value for key, value in identity.items()):
        raise ValueError('The production build uses another API schema')
    version, build_id = table['version'], table['build_id'].upper()
    patch_files = sorted(name for each in builds for name in build_patch.patch_names(each))
    patch = report.get('patch')
    patch_hash = f'{stubs.patch_hash(table):016x}'
    module_file = module_path(report.get('file'))
    if not (folder / module_file).is_file() or not isinstance(patch, dict) or \
            patch.get('files') != patch_files or patch.get('hash') != patch_hash:
        raise ValueError('Production module and patch identity disagree')
    module, elf_hash = validate_module(folder, report)
    out = {}
    listed = []
    # Regenerate from the table, without writing into the supplied build folder.
    with tempfile.TemporaryDirectory(prefix='tsuchi-package-patch-') as work:
        for each in builds:
            text_name, binary_name = build_patch.patch_names(each)
            pchtxt, ips = (folder / text_name).read_bytes(), (folder / binary_name).read_bytes()
            own = Path(work) / each['key']
            own.mkdir()
            changed = build_patch.records(each, own)
            if pchtxt != build_patch.pchtxt(each, changed).encode('ascii') or ips != build_patch.ips(changed):
                raise ValueError('Patch bytes differ from the production game table')
            # One module serves every build, and each program ID the builds have is a folder of its own on the console.
            program = each['constants']['program_id']
            emulator_root = process_metadata.profile(program)[0] + '/Tsuchi/'
            metadata = process_metadata.generate(program)
            out[emulator_root + module_file] = module
            out[emulator_root + 'exefs/main.npdm'] = metadata
            out[emulator_root + text_name] = pchtxt
            out['console/atmosphere/contents/' + title_of(each) + '/' + module_file] = module
            out['console/atmosphere/contents/' + title_of(each) + '/exefs/main.npdm'] = metadata
            out['console/atmosphere/exefs_patches/Tsuchi/' + binary_name.removeprefix('ips/')] = ips
            listed.append(dict(key=each['key'], version=each['version'], build_id=each['build_id'].upper(),
                               program_id=title_of(each), patch_hash=f'{stubs.patch_hash(each):016x}',
                               emulator_folder=emulator_root.rstrip('/'), metadata_sha256=digest(metadata)))
    docs = ROOT / 'site/src/content/docs/players'
    out['INSTALL-CONSOLE.md'] = install_guide((docs / 'install-console.md').read_text(encoding='utf-8'), module_file)
    out['INSTALL-EMULATOR.md'] = install_guide((docs / 'install-tsuchi.mdx').read_text(encoding='utf-8'), module_file)
    out['THIRD-PARTY-NOTICES.txt'] = (ROOT / 'tools/Tsuchi-ThirdPartyNotices.txt').read_bytes()
    release = schema_identity.release_version(ROOT)
    named = {} if release is None else dict(release_version=release)
    out['release.json'] = (json.dumps(dict(format=1, **named, game_version=version,
        game_build_id=build_id, builds=listed, module_file=module_file, module_sha256=digest(module), elf_sha256=elf_hash,
        game_table_sha256=digest(table_bytes), patch_hash=patch_hash, selfcheck=False,
        **identity,
        files={name: dict(bytes=len(data), sha256=digest(data)) for name, data in sorted(out.items())}), indent=2) + '\n').encode('utf-8')
    reject_private_paths(out)
    schema_identity.require_current(ROOT, identity)
    return out


def package(folder, archive, prerelease=False):
    archive = schema_identity.archive_path(ROOT, archive, prerelease)
    wanted = members(folder)
    archive.parent.mkdir(parents=True, exist_ok=True)
    # A unique sibling keeps concurrent writers' temporary files separate, and
    # replacement happens only after the completed archive has been verified.
    handle, name = tempfile.mkstemp(prefix=archive.name + '.', suffix='.pending.zip', dir=archive.parent)
    os.close(handle)
    pending = Path(name)
    try:
        with zipfile.ZipFile(pending, 'w', compression=zipfile.ZIP_DEFLATED, compresslevel=6) as output:
            for name, data in sorted(wanted.items()):
                info = zipfile.ZipInfo(name, date_time=(1980, 1, 1, 0, 0, 0))
                info.create_system = 3
                info.compress_type = zipfile.ZIP_DEFLATED
                info.external_attr = 0o100644 << 16
                output.writestr(info, data, compresslevel=6)
        with zipfile.ZipFile(pending) as check:
            if set(check.namelist()) != set(wanted) or len(check.namelist()) != len(wanted) or check.testzip():
                raise ValueError('Release archive contents differ')
            if any(check.read(name) != data for name, data in wanted.items()):
                raise ValueError('Release archive bytes differ')
        os.replace(pending, archive)
    finally:
        pending.unlink(missing_ok=True)
    return dict(archive=str(archive), archive_sha256=digest(archive.read_bytes()), files=len(wanted))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--built', type=Path, default=ROOT / 'build/module')
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--prerelease', action='store_true',
                        help='package with an unreleased ABI lock; the archive name says prerelease')
    args = parser.parse_args()
    try:
        print(json.dumps(package(args.built, args.out, args.prerelease), indent=2))
    except ValueError as error:
        parser.exit(1, str(error) + '\n')


if __name__ == '__main__':
    main()
