"""Validate and prepare local process metadata; never distribute its output."""
import hashlib
from pathlib import Path
import struct

import json

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
# Release copies use their inventory and need only Python's standard library.
if (HERE / 'release.json').is_file():
    builds = json.loads((HERE / 'release.json').read_text(encoding='utf-8'))['builds']
    PROGRAMS = {int(build['program_id'], 16) for build in builds}
    PROGRAM = int(builds[0]['program_id'], 16)
else:
    import yaml
    PROGRAM = yaml.safe_load((ROOT / 'game/table.yaml').read_text(encoding='utf-8'))['constants']['program_id']
    PROGRAMS = {PROGRAM} | {yaml.safe_load(path.read_text(encoding='utf-8'))['constants']['program_id']
                            for path in (ROOT / 'game/builds').glob('*.yaml')}
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


def validate(raw, require_access=True):
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
    if missing and require_access:
        raise ValueError('Native code access needs system calls: ' + ', '.join(f'{number:#04x}' for number in sorted(missing)))
    return {'program_id': f'{program:016X}', 'sha256': hashlib.sha256(raw).hexdigest(), 'bytes': len(raw), 'system_calls': sorted(REQUIRED)}


def read(path):
    path = Path(path)
    if path.is_symlink() or not path.is_file() or path.stat().st_size > MAX_BYTES:
        raise ValueError('Process metadata input is missing, linked, or too large')
    raw = path.read_bytes()
    return raw, validate(raw)


def grant(raw):
    """Preserve the player's metadata and add only the backend's required calls.

    Offsets follow the public NPDM definitions in Atmosphere's ldr_types.hpp.
    ACID.Size excludes its signature (half the bytes before the ACID header).
    Modified metadata is for a loader accepting local ExeFS replacements.
    """
    validate(raw, require_access=False)
    aci_at, aci_size, acid_at, acid_size = struct.unpack_from('<IIII', raw, 0x70)
    updated = []
    for at, size, acid in ((aci_at, aci_size, False), (acid_at, acid_size, True)):
        section = bytearray(raw[at:at + size])
        header = next(h for h in (0x200, 0x300) if section[h:h + 4] == b'ACID') if acid else 0
        minimum = header + 0x40
        ranges = []
        for field in (0x20, 0x28, 0x30):
            offset, count = struct.unpack_from('<II', section, header + field)
            if not count:
                continue
            if offset < minimum or offset > len(section) or count > len(section) - offset:
                raise ValueError('Process metadata has an invalid access-control range')
            if any(offset < end and start < offset + count for start, end in ranges):
                raise ValueError('Process metadata access-control sections overlap')
            ranges.append((offset, offset + count))
        offset, count = struct.unpack_from('<II', section, header + 0x30)
        words = [word for (word,) in struct.iter_unpack('<I', section[offset:offset + count])]
        groups = {word >> 29: index for index, word in enumerate(words) if word & 31 == 15}
        for call in sorted(REQUIRED):
            group = call // 24
            if group not in groups:
                index = words.index(0xffffffff) if 0xffffffff in words else len(words)
                if index == len(words):
                    words.append(0)
                words[index] = (group << 29) | 15
                groups[group] = index
            words[groups[group]] |= 1 << (5 + call % 24)
        caps = b''.join(struct.pack('<I', word) for word in words)
        if len(caps) == count:
            section[offset:offset + count] = caps
        else:
            # Relocate just the capability table; preserve other access data and padding.
            section.extend(bytes((-len(section)) % 4))
            offset = len(section)
            section.extend(caps)
            struct.pack_into('<II', section, header + 0x30, offset, len(caps))
        if acid:
            signed = struct.unpack_from('<I', section, header + 4)[0]
            if signed != size - header // 2:
                raise ValueError('Process metadata has an invalid ACID signed size')
            struct.pack_into('<I', section, header + 4, len(section) - header // 2)
        updated.append((at, size, bytes(section), 0x78 if acid else 0x70))
    # Keep section order and every byte outside those sections, adjusting absolute offsets.
    out, cursor = bytearray(), 0
    locations = []
    for at, size, section, field in sorted(updated):
        out.extend(raw[cursor:at])
        out.extend(bytes((-len(out)) % 16))
        locations.append((field, len(out), len(section)))
        out.extend(section)
        cursor = at + size
    out.extend(raw[cursor:])
    for field, at, size in locations:
        struct.pack_into('<II', out, field, at, size)
    out[0x0c] &= 0x7f  # Code aliases need code reads, including on Switch 2.
    validate(out)
    return bytes(out)
