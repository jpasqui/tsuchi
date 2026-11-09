"""Builds the framework as a module the game's system loads beside the game.

    python tools/build_module.py            writes build/module/exefs/subsdk9 and the patch beside it
    python tools/build_module.py --selfcheck   the same with the framework's own checks built in, in build/check-module

Needs Clang and LLD with AArch64 support. Nothing from the game is needed to build.
"""
import argparse
import hashlib
import json
import os
import re
import shutil
import stat
import struct
import subprocess
from concurrent.futures import ThreadPoolExecutor
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))

import generate  # noqa: E402
import schema_identity  # noqa: E402

FRAMEWORK = ROOT / 'framework'
COMPILE = ['--target=aarch64-none-elf', '-O2', '-ffreestanding', '-fPIC', '-fvisibility=hidden', '-ffixed-x18',
           '-fno-exceptions', '-fno-rtti', '-fno-stack-protector', '-fno-unwind-tables',
           '-fno-asynchronous-unwind-tables', '-fno-threadsafe-statics', '-mno-outline-atomics', '-nostdlib',
           '-std=c++17', '-Wall', '-Wextra', '-Werror', '-Wshorten-64-to-32',
           '-DTSUCHI_DEPRECATED(text)=']  # the framework still offers what it has deprecated, and uses it to do so
# Lua, and what it is built on in the module: a C library of the framework's own.
LUA = ROOT / 'third_party' / 'lua-5.4.7' / 'src'
LUA_RUNTIME = ROOT / 'runtime' / 'lua'
LUA_PARTS = ('lapi', 'lcode', 'lctype', 'ldebug', 'ldo', 'ldump', 'lfunc', 'lgc', 'llex', 'lmem', 'lobject', 'lopcodes', 'lparser', 'lstate',
             'lstring', 'ltable', 'ltm', 'lundump', 'lvm', 'lzio', 'lauxlib', 'lbaselib', 'lcorolib', 'lmathlib', 'lstrlib', 'ltablib', 'lutf8lib')
LUA_CONFIG = ['-nostdlibinc', f'-I{LUA_RUNTIME / "libc" / "include"}', f'-I{LUA}', f'-I{LUA_RUNTIME}', '-DLUA_USER_H="tsuchi_luauser.h"']
COMPILE_C = ['--target=aarch64-none-elf', '-O2', '-ffreestanding', '-fPIC', '-fvisibility=hidden', '-ffixed-x18', '-fno-stack-protector',
             '-fno-unwind-tables', '-fno-asynchronous-unwind-tables', '-mno-outline-atomics', '-nostdlib', '-std=c99', '-w']
QUICKJS = ROOT / 'third_party' / 'quickjs-2025-09-13'
JS_RUNTIME = ROOT / 'runtime' / 'javascript'
QUICKJS_PARTS = ('quickjs', 'cutils', 'libregexp', 'libunicode', 'dtoa')
QUICKJS_CONFIG = ['-nostdlibinc', f'-I{LUA_RUNTIME / "libc" / "include"}',
                  '-DTSUCHI_QUICKJS', '-DCONFIG_VERSION="2025-09-13"']
HTTPS = ROOT / 'runtime' / 'https'
MBEDTLS = ROOT / 'third_party' / 'mbedtls-3.6.7'
HTTPS_CONFIG = ['-nostdlibinc', f'-I{HTTPS / "include"}', f'-I{LUA_RUNTIME / "libc" / "include"}', f'-I{MBEDTLS / "include"}', f'-I{HTTPS}', '-DMBEDTLS_CONFIG_FILE="https_tls_config.h"']
ENTRY = 'tsuchi_initialize'
DT_NEEDED, DT_RELA, DT_RELASZ, DT_INIT, DT_JMPREL, DT_INIT_ARRAYSZ = 1, 7, 8, 12, 23, 27
R_AARCH64_RELATIVE = 0x403
PAGE = 0x1000


def build_jobs():
    """Limit compiler processes on a CI runner; preserve the local default."""
    value = os.environ.get('TSUCHI_BUILD_JOBS')
    if value is None:
        return os.cpu_count() or 4
    if not value.isascii() or not value.isdecimal() or int(value) < 1:
        raise ValueError('TSUCHI_BUILD_JOBS must be a positive integer')
    return int(value)


def run(command):
    result = subprocess.run([str(c) for c in command], capture_output=True, text=True)
    if result.returncode:
        sys.exit(f'{Path(str(command[0])).name} failed:\n{result.stdout}{result.stderr}')


def page_up(value):
    return (value + PAGE - 1) & ~(PAGE - 1)


def read_elf(raw):
    """(load segments, sections by name, symbols by name) of a 64-bit AArch64 ELF."""
    if raw[:7] != b'\x7fELF\x02\x01\x01' or struct.unpack_from('<H', raw, 18)[0] != 183:
        sys.exit('the linker did not produce a 64-bit AArch64 ELF')
    program_at, section_at = struct.unpack_from('<QQ', raw, 32)
    program_size, program_count, section_size, section_count, names_index = struct.unpack_from('<HHHHH', raw, 54)
    headers = [struct.unpack_from('<IIQQQQIIQQ', raw, section_at + i * section_size) for i in range(section_count)]
    names = raw[headers[names_index][4]:headers[names_index][4] + headers[names_index][5]]

    def text(table, at):
        return table[at:table.index(b'\0', at)].decode()

    sections = {text(names, h[0]): {'address': h[3], 'offset': h[4], 'size': h[5], 'link': h[6]} for h in headers}
    symbols = {}
    if '.symtab' in sections:
        table = sections['.symtab']
        strings_header = headers[table['link']]
        strings = raw[strings_header[4]:strings_header[4] + strings_header[5]]
        for at in range(table['offset'], table['offset'] + table['size'], 24):
            name, _, _, _, value, _ = struct.unpack_from('<IBBHQQ', raw, at)
            if name:
                symbols[text(strings, name)] = value
    loads = []
    for i in range(program_count):
        kind, flags, offset, address, _, file_size, memory_size, _ = struct.unpack_from('<IIQQQQQQ', raw, program_at + i * program_size)
        if kind == 1:
            loads.append({'flags': flags, 'offset': offset, 'address': address, 'file_size': file_size, 'memory_size': memory_size})
    return loads, sections, symbols


def check(raw, loads, sections, symbols):
    """Refuses a module the loader would not start the way the framework expects."""
    if [s['flags'] for s in loads] != [5, 4, 6] or loads[0]['address'] != 0:
        sys.exit('expected three segments: code, read-only data, data')
    for a, b in zip(loads, loads[1:]):
        if a['address'] + page_up(a['memory_size']) > b['address']:
            sys.exit('segments overlap')
    image = bytearray(page_up(loads[-1]['address'] + loads[-1]['memory_size']))
    for s in loads:
        image[s['address']:s['address'] + s['file_size']] = raw[s['offset']:s['offset'] + s['file_size']]
    header = struct.unpack_from('<I', image, 4)[0]
    if image[header:header + 4] != b'MOD0':
        sys.exit('the module header is missing')
    dynamic = header + struct.unpack_from('<i', image, header + 4)[0]
    tags = {}
    for at in range(dynamic, dynamic + sections['.dynamic']['size'], 16):
        tag, value = struct.unpack_from('<QQ', image, at)
        if not tag:
            break
        tags[tag] = value
    if tags.get(DT_INIT) != symbols.get(ENTRY):
        sys.exit(f'the loader would not call {ENTRY}')
    # The loader runs the entry and nothing else, so a global that needs code to
    # set it up would stay zero. Such a global shows up as an init-array entry.
    if tags.get(DT_INIT_ARRAYSZ, 0):
        sys.exit('a global variable needs start-up code to initialize it; make it constant-initialized')
    if DT_NEEDED in tags or tags.get(DT_JMPREL):
        sys.exit('the module must not import anything')
    for at in range(tags.get(DT_RELA, 0), tags.get(DT_RELA, 0) + tags.get(DT_RELASZ, 0), 24):
        target, kind, addend = struct.unpack_from('<QQq', image, at)
        if kind != R_AARCH64_RELATIVE or not loads[2]['address'] <= target <= len(image) - 8 or not 0 <= addend < len(image):
            sys.exit(f'unexpected relocation at {at:#x}')
    names = sections['.dynsym']
    for at in range(names['offset'], names['offset'] + names['size'], 24):
        name, info, _, section, _, _ = struct.unpack_from('<IBBHQQ', raw, at)
        if name and section and info >> 4:
            sys.exit('the module must not export anything')
    return len(image)


def module_file(raw, loads, sections):
    """The module in the format the system loads: a 0x100-byte header, then the three segments."""
    out = bytearray(0x100)
    out[:4] = b'NSO0'
    struct.pack_into('<I', out, 0x0C, 0x38)             # the three segments are stored as they are, each with a hash
    out[0x40:0x60] = hashlib.sha256(raw).digest()       # the module's ID
    for i, s in enumerate(loads):
        payload = raw[s['offset']:s['offset'] + s['file_size']]
        struct.pack_into('<III', out, 0x10 + 16 * i, len(out), s['address'], len(payload))
        struct.pack_into('<I', out, 0x60 + 4 * i, len(payload))
        out[0xA0 + 32 * i:0xC0 + 32 * i] = hashlib.sha256(payload).digest()
        out.extend(payload)
    struct.pack_into('<I', out, 0x3C, loads[2]['memory_size'] - loads[2]['file_size'])
    for field, name in ((0x90, '.dynstr'), (0x98, '.dynsym')):
        struct.pack_into('<II', out, field, sections[name]['address'] - loads[1]['address'], sections[name]['size'])
    return bytes(out)


def _output_info(path):
    try:
        info = path.lstat()
    except FileNotFoundError:
        return None
    if stat.S_ISLNK(info.st_mode) or getattr(info, 'st_file_attributes', 0) & 0x400:
        raise ValueError('module output contains a symlink or reparse point: ' + str(path))
    return info


def _require_owned_module(out, previous):
    if not isinstance(previous.get('file'), str) or not re.fullmatch(r'exefs/subsdk[0-9]', previous['file']):
        raise ValueError('previous module output has an invalid slot path')
    path = out / previous['file']
    info = _output_info(path)
    if info is None or not stat.S_ISREG(info.st_mode) or info.st_size != previous['bytes']:
        raise ValueError('previous module output is missing or changed: ' + str(path))
    raw = path.read_bytes()
    if raw[:4] != b'NSO0' or hashlib.sha256(raw).hexdigest() != previous['sha256']:
        raise ValueError('previous module output is missing or changed: ' + str(path))


def module_output_preflight(out):
    """Prove ownership before rebuilding or removing a module from an older slot."""
    out = Path(out)
    _output_info(out / 'exefs')
    existing = {'exefs/subsdk' + str(slot) for slot in range(10)
                if _output_info(out / 'exefs' / ('subsdk' + str(slot))) is not None}
    report_path = out / 'report.json'
    info = _output_info(report_path)
    metadata = _output_info(out / 'exefs' / 'main.npdm')
    if info is None:
        if existing or metadata is not None:
            raise ValueError('module output has no ownership report; preserve unexpected files and use an empty output folder')
        return None
    try:
        if not stat.S_ISREG(info.st_mode):
            raise ValueError('report is not a regular file')
        report = json.loads(report_path.read_text(encoding='utf-8'))
        if not isinstance(report, dict) or not isinstance(report.get('file'), str) or \
                not re.fullmatch(r'exefs/subsdk[0-9]', report['file']) or \
                type(report.get('bytes')) is not int or report['bytes'] < 0x100 or \
                not isinstance(report.get('sha256'), str) or not re.fullmatch('[0-9a-f]{64}', report['sha256']):
            raise ValueError('module ownership fields are invalid')
    except (ValueError, UnicodeError) as error:
        raise ValueError('invalid previous module build report; preserve the outputs before rebuilding: ' + str(error)) from error
    previous = {key: report[key] for key in ('file', 'bytes', 'sha256')}
    if existing != {previous['file']}:
        raise ValueError('module output includes missing or unowned slots; preserve unexpected files and use an empty output folder')
    _require_owned_module(out, previous)
    # Process metadata is the previous build's only when its report names those bytes.
    if metadata is not None:
        access = report.get('native_code_access')
        recorded = access.get('sha256') if isinstance(access, dict) else None
        if not stat.S_ISREG(metadata.st_mode) or not isinstance(recorded, str) or \
                hashlib.sha256((out / 'exefs' / 'main.npdm').read_bytes()).hexdigest() != recorded:
            raise ValueError('module output has process metadata no build of it wrote; preserve it and use an empty output folder')
        previous['metadata_sha256'] = recorded
    return previous


def remove_previous_module(out, name, previous):
    """Remove only the old slot whose bytes were proved at build preflight."""
    if previous is not None and previous['file'] != 'exefs/' + name:
        _require_owned_module(out, previous)
        (out / previous['file']).unlink()


def builds_digest(root=ROOT):
    """What the other builds' files and the API families hash to, or None when the table has no other build."""
    files = sorted((Path(root) / 'game' / 'builds').glob('*.yaml'))
    if not files:
        return None
    hashed = hashlib.sha256()
    for path in [*files, Path(root) / 'game' / 'families.yaml']:
        hashed.update(path.name.encode('utf-8') + b'\0' + path.read_bytes())
    return hashed.hexdigest()


def build(out, name='subsdk9', selfcheck=False, native_npdm=None):
    """`selfcheck` builds in the framework's own checks, which run in the game: never the build people run."""
    if not isinstance(name, str) or not re.fullmatch(r'subsdk[0-9]', name):
        raise ValueError('module name must be subsdk0 through subsdk9')
    previous = module_output_preflight(out)
    clang, linker = shutil.which('clang'), shutil.which('ld.lld')
    if not clang or not linker:
        sys.exit('clang and ld.lld are needed')
    native_raw, native_info = None, None
    if native_npdm is not None:
        import native_hook_permissions
        native_raw, native_info = native_hook_permissions.read(native_npdm)
    identity = schema_identity.capture(ROOT)
    table_hash = hashlib.sha256((ROOT / 'game/table.yaml').read_bytes()).hexdigest()
    other_builds = builds_digest()
    gen = ROOT / 'build' / 'gen'
    generate.write(gen)
    schema_identity.require_current(ROOT, identity)
    objects = out / 'obj'
    objects.mkdir(parents=True, exist_ok=True)
    includes = [FRAMEWORK / 'target' / 'include', gen / 'include', gen / 'framework', FRAMEWORK / 'core',
                FRAMEWORK / 'base', FRAMEWORK / 'game', FRAMEWORK / 'target', LUA_RUNTIME / 'mods', HTTPS / 'include', MBEDTLS / 'include', HTTPS, objects]
    sources = sorted((FRAMEWORK / 'core').glob('*.cpp')) + sorted((FRAMEWORK / 'base').glob('*.cpp'))
    sources += sorted((FRAMEWORK / 'game').glob('*.cpp'))
    roots = (MBEDTLS / 'cacert.pem').read_bytes()
    (objects / 'https_roots.h').write_text('#pragma once\nstatic const unsigned char tsuchi_https_roots[] = {' + ','.join(str(b) for b in roots + b'\0') + '};\n', encoding='utf-8')
    sources += sorted(HTTPS.glob('*.cpp'))
    sources += sorted((FRAMEWORK / 'target').glob('*.cpp')) + sorted((gen / 'framework').glob('*.cpp'))
    built, jobs = [], []  # every object compiles on its own, so they compile at once, and link in this order
    checks = ['-DTSUCHI_SELFCHECK=1'] if selfcheck else []
    for source in sources:
        target = objects / f'{source.parent.name}_{source.stem}.o'
        jobs.append([clang, *COMPILE, *checks, '-DMBEDTLS_CONFIG_FILE="https_tls_config.h"', *[f'-I{folder}' for folder in includes], '-c', source, '-o', target])
        built.append(target)
    tls_sources = sorted((MBEDTLS / 'library').glob('*.c'))
    for source in tls_sources:
        target = objects / f'mbedtls_{source.stem}.o'
        jobs.append([clang, *COMPILE_C, *HTTPS_CONFIG, '-c', source, '-o', target])
        built.append(target)
    sources += tls_sources
    # The script runtime: the bindings, which see the framework through its public headers and a few of its own,
    # then Lua itself and the C library it stands on.
    script_sources = sorted(LUA_RUNTIME.glob('*.cpp')) + sorted((LUA_RUNTIME / 'mods').glob('*.cpp')) + sorted((gen / 'lua').glob('*.cpp'))
    for source in script_sources:
        target = objects / f'lua_{source.stem}.o'
        jobs.append([clang, *COMPILE, *LUA_CONFIG, *[f'-I{folder}' for folder in includes[1:]], '-c', source, '-o', target])
        built.append(target)
    c_sources = [LUA / f'{part}.c' for part in LUA_PARTS] + [LUA_RUNTIME / 'libc' / 'libc.c']
    for source in c_sources:
        target = objects / f'c_{source.stem}.o'
        jobs.append([clang, *COMPILE_C, *LUA_CONFIG, '-c', source, '-o', target])
        built.append(target)
    js_sources = sorted(JS_RUNTIME.glob('*.cpp')) + sorted((gen / 'javascript').glob('*.cpp'))
    for source in js_sources:
        target = objects / f'js_{source.stem}.o'
        jobs.append([clang, *COMPILE, *QUICKJS_CONFIG, f'-I{JS_RUNTIME}', '-isystem', QUICKJS,
             *[f'-I{folder}' for folder in includes[1:]], '-c', source, '-o', target])
        built.append(target)
    quick_sources = [QUICKJS / (part + '.c') for part in QUICKJS_PARTS]
    for source in quick_sources:
        target = objects / f'quickjs_{source.stem}.o'
        jobs.append([clang, *COMPILE_C, *QUICKJS_CONFIG, '-c', source, '-o', target])
        built.append(target)
    sources += js_sources + quick_sources
    jumps = objects / 'setjmp.o'
    jobs.append([clang, '--target=aarch64-none-elf', '-c', LUA_RUNTIME / 'libc' / 'setjmp_aarch64.S', '-o', jumps])
    built.append(jumps)
    sources += script_sources + c_sources
    start = objects / 'start.o'
    jobs.append([clang, '--target=aarch64-none-elf', '-c', FRAMEWORK / 'target' / 'start.S', '-o', start])
    with ThreadPoolExecutor(build_jobs()) as pool:
        list(pool.map(run, jobs))
    elf = objects / 'tsuchi.pending.elf'
    # A worktree path can put hundreds of object paths beyond Windows' command
    # line limit. LLVM accepts the same arguments through a UTF-8 response file.
    link_args = ['-shared', '-Bsymbolic', '--hash-style=sysv', '--no-undefined', '-T', FRAMEWORK / 'target' / 'module.ld',
                 '-soname', 'tsuchi', '-init', ENTRY, '-o', elf, start, *built]
    response = objects / 'link.rsp'
    response.write_text('\n'.join('"' + str(part).replace('\\', '/').replace('"', '\\"') + '"' for part in link_args) + '\n', encoding='utf-8')
    run([linker, '@' + str(response)])
    raw = elf.read_bytes()
    loads, sections, symbols = read_elf(raw)
    memory = check(raw, loads, sections, symbols)
    module = module_file(raw, loads, sections)
    schema_identity.require_current(ROOT, identity)
    import build_patch  # here, since it uses this module's helpers
    patch_folder = objects / 'patch-build'
    patch = build_patch.build(patch_folder)
    schema_identity.require_current(ROOT, identity)
    if hashlib.sha256((ROOT / 'game/table.yaml').read_bytes()).hexdigest() != table_hash or builds_digest() != other_builds:
        raise ValueError('Game table changed during module build')
    if module_output_preflight(out) != previous:
        raise ValueError('module output ownership changed during build')
    for relative in patch.get('files', []):
        target = out / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        os.replace(patch_folder / relative, target)
    os.replace(elf, out / 'tsuchi.elf')
    (out / 'exefs').mkdir(exist_ok=True)
    (out / 'exefs' / name).write_bytes(module)
    remove_previous_module(out, name, previous)
    if native_raw is not None:
        (out / 'exefs' / 'main.npdm').write_bytes(native_raw)
    elif previous is not None and 'metadata_sha256' in previous:
        # An ordinary build retires the process metadata an earlier native-hook build of this folder wrote.
        (out / 'exefs' / 'main.npdm').unlink()
    report = {'file': f'exefs/{name}', 'bytes': len(module), 'memory_bytes': memory, 'sha256': hashlib.sha256(module).hexdigest(),
              'selfcheck': bool(selfcheck),
              'elf_sha256': hashlib.sha256(raw).hexdigest(),
              'game_table_sha256': table_hash,
              **({} if other_builds is None else {'game_builds_sha256': other_builds}),
              **identity,
              'patch': patch,
              'sources': [s.relative_to(ROOT).as_posix() if ROOT in s.parents else s.name for s in sources]}
    if native_info:
        report['native_code_access'] = native_info
    (out / 'report.json').write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('--out', type=Path)
    parser.add_argument('--selfcheck', action='store_true', help="build in the framework's own checks, for a run that checks it in the game")
    parser.add_argument('--module-slot', type=int, choices=range(10), default=9,
                        help='Tsuchi subsdk slot, 0–9 (default: 9); choose a free slot and keep foreign modules unchanged')
    parser.add_argument('--native-hooks-npdm', type=Path,
                        help='stage locally prepared process metadata alongside the standard module')
    args = parser.parse_args()
    folder = 'native-module' if args.native_hooks_npdm else 'check-module' if args.selfcheck else 'module'
    try:
        report = build(args.out or ROOT / 'build' / folder, name='subsdk' + str(args.module_slot),
                       selfcheck=args.selfcheck, native_npdm=args.native_hooks_npdm)
    except (ValueError, OSError) as error:
        parser.exit(1, str(error) + '\n')
    print(f"{report['file']}: {report['bytes']} bytes, {report['memory_bytes']} in memory")


if __name__ == '__main__':
    main()
