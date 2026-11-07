"""Builds a compiled mod: C and C++ sources into the `mod.nro` of a mod's folder.

    python build_mod.py main.cpp more.cpp --out my.mod/mod.nro

The sources include "tsuchi/tsuchi.h" and define tsuchi_mod_start. They are compiled for the game's
machine with nothing to link against: no C or C++ library, no exceptions, no run-time type
information. `-I DIR` adds a folder of headers, and `-D NAME` or `-D NAME=VALUE` a definition.
"tsuchi_check.h", the helper for checks a mod makes in the game, is found without one.

Needs Clang and LLD with AArch64 support, on PATH. Nothing from the game is needed.
"""
import argparse
import hashlib
import shutil
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

SDK = Path(__file__).resolve().parent
PAGE = 0x1000
START = 'tsuchi_mod_start'
COMMON = ['--target=aarch64-none-elf', '-O2', '-ffreestanding', '-fPIC', '-fvisibility=hidden', '-ffixed-x18', '-fno-stack-protector',
          '-fno-unwind-tables', '-fno-asynchronous-unwind-tables', '-mno-outline-atomics', '-nostdlib']
CXX = ['-std=c++17', '-fno-exceptions', '-fno-rtti', '-fno-threadsafe-statics']
C = ['-std=c11']
DT_NEEDED, DT_RELA, DT_RELASZ, DT_INIT, DT_INIT_ARRAYSZ, DT_JMPREL = 1, 7, 8, 12, 27, 23
R_AARCH64_RELATIVE = 0x403


class BuildError(Exception):
    pass


def headers():
    """Where the framework's headers are: beside this file in a packaged SDK, or as the repository generates them."""
    for folder in (SDK / 'include', SDK.parent / 'build' / 'gen' / 'include'):
        if (folder / 'tsuchi' / 'tsuchi.h').is_file():
            return folder
    raise BuildError('the framework\'s headers were not found; run tools/generate.py')


def run(command):
    """Runs a tool and returns what it said while it succeeded, which is its warnings."""
    result = subprocess.run([str(c) for c in command], capture_output=True, text=True)
    if result.returncode:
        raise BuildError(f'{Path(str(command[0])).name} failed:\n{result.stdout}{result.stderr}')
    return result.stderr


def page_up(value):
    return (value + PAGE - 1) & ~(PAGE - 1)


def read_elf(raw):
    """(load segments, sections by name, the names the module offers) of what the linker made."""
    if raw[:7] != b'\x7fELF\x02\x01\x01' or struct.unpack_from('<H', raw, 18)[0] != 183:
        raise BuildError('the linker did not produce a 64-bit AArch64 ELF')
    program_at, section_at = struct.unpack_from('<QQ', raw, 32)
    program_size, program_count, section_size, section_count, names_index = struct.unpack_from('<HHHHH', raw, 54)
    section_headers = [struct.unpack_from('<IIQQQQIIQQ', raw, section_at + i * section_size) for i in range(section_count)]
    names = raw[section_headers[names_index][4]:section_headers[names_index][4] + section_headers[names_index][5]]

    def text(table, at):
        return table[at:table.index(b'\0', at)].decode()

    sections = {text(names, h[0]): {'address': h[3], 'offset': h[4], 'size': h[5]} for h in section_headers}
    loads = []
    for i in range(program_count):
        kind, flags, offset, address, _, file_size, memory_size, _ = struct.unpack_from('<IIQQQQQQ', raw, program_at + i * program_size)
        if kind == 1:
            loads.append({'flags': flags, 'offset': offset, 'address': address, 'file_size': file_size, 'memory_size': memory_size})
    offered = set()
    strings = raw[sections['.dynstr']['offset']:sections['.dynstr']['offset'] + sections['.dynstr']['size']]
    for at in range(sections['.dynsym']['offset'], sections['.dynsym']['offset'] + sections['.dynsym']['size'], 24):
        name, info, _, section, _, _ = struct.unpack_from('<IBBHQQ', raw, at)
        if name and section and info >> 4:
            offered.add(text(strings, name))
    return loads, sections, offered


def module_file(raw):
    """The bytes of mod.nro, from what the linker made. Refuses what the loader would not take."""
    loads, sections, offered = read_elf(raw)
    if [s['flags'] for s in loads] != [5, 4, 6] or loads[0]['address'] != 0:
        raise BuildError('expected three parts: code, what is only read, data')
    code, constant, data = loads
    if any(s['address'] % PAGE for s in loads):
        raise BuildError('each part has to start on a page')
    if constant['address'] != page_up(code['memory_size']) or data['address'] != page_up(constant['address'] + constant['memory_size']):
        raise BuildError('the parts have to follow one another')
    if START not in offered:
        raise BuildError(f'no source defines {START}')
    zeros_start = data['address'] + page_up(data['file_size'])
    zeros_end = data['address'] + data['memory_size']
    image = bytearray(zeros_start)
    for s in loads:
        image[s['address']:s['address'] + s['file_size']] = raw[s['offset']:s['offset'] + s['file_size']]
    header = struct.unpack_from('<I', image, 4)[0]
    if image[header:header + 4] != b'MOD0':
        raise BuildError('the module header is missing')
    dynamic = header + struct.unpack_from('<i', image, header + 4)[0]
    tags = {}
    for at in range(dynamic, dynamic + sections['.dynamic']['size'], 16):
        tag, value = struct.unpack_from('<QQ', image, at)
        if not tag:
            break
        tags[tag] = value
    if DT_NEEDED in tags or tags.get(DT_JMPREL):
        raise BuildError('a mod cannot be linked against anything')
    if DT_INIT not in tags or tags.get(DT_INIT_ARRAYSZ, 0):
        raise BuildError('the start-up code is missing')
    for at in range(tags.get(DT_RELA, 0), tags.get(DT_RELA, 0) + tags.get(DT_RELASZ, 0), 24):
        _, kind, _ = struct.unpack_from('<QQq', image, at)
        if kind != R_AARCH64_RELATIVE:
            raise BuildError('the mod uses something it does not define')
    # The file's header, in the room the start of the code leaves for it.
    struct.pack_into('<4sIII', image, 0x10, b'NRO0', 0, len(image), 0)
    struct.pack_into('<IIIIII', image, 0x20, 0, constant['address'], constant['address'], data['address'] - constant['address'],
                     data['address'], zeros_start - data['address'])
    struct.pack_into('<I', image, 0x38, page_up(max(zeros_end - zeros_start, 0)))
    image[0x40:0x60] = hashlib.sha256(raw).digest()
    for at, name in ((0x70, '.dynstr'), (0x78, '.dynsym')):
        struct.pack_into('<II', image, at, sections[name]['address'] - constant['address'], sections[name]['size'])
    return bytes(image)


def build(sources, out, include=(), define=(), warnings=None):
    """Compiles and links the sources and writes the module file. Returns its size. What the compiler warned
    of is added to `warnings`, a list, when one is given: a use of something the framework has deprecated is
    told that way."""
    clang, lld = shutil.which('clang'), shutil.which('ld.lld')
    if not clang or not lld:
        raise BuildError('Clang and LLD are needed on PATH')
    flags = COMMON + ['-I', headers(), '-I', SDK / 'libc', '-I', SDK / 'check'] + [f'-I{folder}' for folder in include] + [f'-D{name}' for name in define]
    with tempfile.TemporaryDirectory() as work:
        objects = []
        for index, source in enumerate([*map(Path, sources), SDK / 'support.c', SDK / 'start.S']):
            if not source.is_file():
                raise BuildError(f'{source} is not there')
            obj = Path(work) / f'{index}-{source.stem}.o'
            if source.suffix == '.S':
                language = []
            elif source.suffix == '.c':
                language = C + (['-fno-builtin'] if source.parent == SDK else [])
            else:
                language = CXX
            said = run([clang, *flags, *language, '-c', source, '-o', obj])
            if said and warnings is not None:
                warnings.append(said)
            objects.append(obj)
        linked = Path(work) / 'mod.elf'
        run([lld, '-shared', '-Bsymbolic', '-z', 'now', '--hash-style=sysv', '--no-undefined', '--gc-sections', '-init=tsuchi_sdk_setup',
             '-T', SDK / 'mod.ld', '-soname', 'mod', '-o', linked, *objects])
        image = module_file(linked.read_bytes())
    out = Path(out)
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_bytes(image)
    return len(image)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('sources', nargs='+')
    parser.add_argument('--out', required=True, help='the file to write: mod.nro in the mod\'s folder')
    parser.add_argument('-I', dest='include', action='append', default=[])
    parser.add_argument('-D', dest='define', action='append', default=[])
    arguments = parser.parse_args()
    warnings = []
    try:
        size = build(arguments.sources, arguments.out, arguments.include, arguments.define, warnings)
    except BuildError as problem:
        sys.exit(str(problem))
    sys.stderr.write(''.join(warnings))
    print(f'{arguments.out}: {size} bytes')


if __name__ == '__main__':
    main()
