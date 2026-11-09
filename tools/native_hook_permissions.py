"""Validate process permissions before building a local native-code-access module.

Reads user-supplied process metadata. It does not grant permissions, modify the
input, or produce metadata for distribution.
"""
import hashlib
from pathlib import Path
import struct

import yaml

ROOT = Path(__file__).resolve().parents[1]
# The program ID of each build the table knows.
PROGRAM = yaml.safe_load((ROOT / 'game' / 'table.yaml').read_text(encoding='utf-8'))['constants']['program_id']
PROGRAMS = {PROGRAM} | {yaml.safe_load(path.read_text(encoding='utf-8'))['constants']['program_id']
                        for path in (ROOT / 'game' / 'builds').glob('*.yaml')}
REQUIRED = frozenset((0x06, 0x08, 0x09, 0x0A, 0x16, 0x18, 0x21, 0x29, 0x40, 0x43,
                      0x73, 0x74, 0x75, 0x77, 0x78))
MAX_BYTES = 1024 * 1024


def span(raw, start, size, minimum):
    if start < 0x80 or size < minimum or start > len(raw) or size > len(raw) - start:
        raise ValueError('Process metadata has an invalid section range')
    return raw[start:start + size]


def syscalls(section, field, minimum):
    offset, size = struct.unpack_from('<II', section, field)
    if offset < minimum or size % 4 or offset > len(section) or size > len(section) - offset:
        raise ValueError('Process metadata has an invalid capability range')
    allowed, groups = set(), set()
    for (word,) in struct.iter_unpack('<I', section[offset:offset + size]):
        if word & 31 != 15:
            continue
        group, mask = word >> 29, (word >> 5) & 0xffffff
        if group in groups:
            raise ValueError('Process metadata repeats a system-call capability group')
        groups.add(group)
        allowed.update(24 * group + bit for bit in range(24) if mask & (1 << bit))
    return allowed


def validate(raw):
    if not 0x80 <= len(raw) <= MAX_BYTES or raw[:4] != b'META' or not raw[0x0c] & 1:
        raise ValueError('Native hooks need bounded 64-bit process metadata')
    aci_at, aci_size, acid_at, acid_size = struct.unpack_from('<IIII', raw, 0x70)
    aci = span(raw, aci_at, aci_size, 0x40)
    acid = span(raw, acid_at, acid_size, 0x240)
    if aci_at < 0x80 or acid_at < 0x80 or not (aci_at + aci_size <= acid_at or acid_at + acid_size <= aci_at):
        raise ValueError('Process metadata sections overlap')
    # The ACID header follows its signatures: 0x200 bytes on Switch, 0x300 on Switch 2.
    header = next((at for at in (0x200, 0x300) if acid[at:at + 4] == b'ACID' and len(acid) >= at + 0x40), None)
    if aci[:4] != b'ACI0' or header is None:
        raise ValueError('Process metadata section identity differs')
    program = struct.unpack_from('<Q', aci, 0x10)[0]
    if program not in PROGRAMS:
        raise ValueError('Process metadata is for another program')
    low, high = struct.unpack_from('<QQ', acid, header + 0x10)
    if not low <= program <= high:
        raise ValueError('Process metadata does not admit the program')
    granted = syscalls(aci, 0x30, 0x40)
    admitted = syscalls(acid, header + 0x30, header + 0x40)
    missing = REQUIRED - (granted & admitted)
    if missing:
        raise ValueError('Native code access needs system calls: ' + ', '.join(f'{number:#04x}' for number in sorted(missing)))
    return {'program_id': f'{program:016X}', 'sha256': hashlib.sha256(raw).hexdigest(), 'bytes': len(raw), 'system_calls': sorted(REQUIRED)}


def read(path):
    path = Path(path)
    if path.is_symlink() or not path.is_file() or path.stat().st_size > MAX_BYTES:
        raise ValueError('Process metadata input is missing, linked, or too large')
    raw = path.read_bytes()
    return raw, validate(raw)
