"""Decode bounded main-module patches and refuse conflicting static writes.

Offsets returned here exclude the NSO's 0x100-byte file header. These checks
describe static byte conflicts, not compatibility between gameplay behaviors.
"""
from pathlib import Path
import re

MAX_INPUT_BYTES = 8 * 1024 * 1024
MAX_DECODED_BYTES = 1024 * 1024
MAX_SEGMENTS = 65536
MAX_COMBINED_BYTES = 64 * 1024 * 1024
MAX_COMBINED_SEGMENTS = 65536
NSO_HEADER_BYTES = 0x100
FILE_END = 1 << 32


def _build(value):
    if not isinstance(value, str) or not re.fullmatch(r'[0-9a-fA-F]{32}|[0-9a-fA-F]{64}', value):
        raise ValueError('expected build ID must contain 32 or 64 hexadecimal digits')
    return value.upper().ljust(64, '0')


def _segment(offset, data):
    if type(offset) is not int or offset < 0 or not isinstance(data, bytes) or not data or \
            offset + NSO_HEADER_BYTES + len(data) > FILE_END:
        raise ValueError('patch write is empty, edits the NSO header, or exceeds its address range')


def _normalized(writes, byte_limit=MAX_DECODED_BYTES, segment_limit=MAX_SEGMENTS):
    if not isinstance(writes, (list, tuple)) or len(writes) > segment_limit:
        raise ValueError('patch has too many write segments')
    total = 0
    for write in writes:
        if not isinstance(write, (tuple, list)) or len(write) != 2:
            raise ValueError('patch write must contain an offset and bytes')
        _segment(*write)
        total += len(write[1])
        if total > byte_limit:
            raise ValueError('decoded patch exceeds its byte budget')
    # Merge sorted intervals once. Comparing only the final merged interval
    # bounds overlap work by the input byte budget, including repeated writes.
    merged = []
    for offset, data in sorted(writes, key=lambda write: write[0]):
        if not merged or offset > merged[-1][0] + len(merged[-1][1]):
            merged.append((offset, bytearray(data)))
            continue
        previous, kept = merged[-1]
        start = offset - previous
        overlap = min(len(data), len(kept) - start)
        if kept[start:start + overlap] != data[:overlap]:
            raise ValueError(f'conflicting patch writes overlap at main+0x{offset:X}')
        kept.extend(data[overlap:])
    return [(offset, bytes(data)) for offset, data in merged]


def _ips(data):
    if data.startswith(b'IPS32'):
        width, terminator = 4, b'EEOF'
    elif data.startswith(b'PATCH'):
        width, terminator = 3, b'EOF'
    else:
        raise ValueError('IPS header must be PATCH or IPS32')
    at, total, writes = 5, 0, []
    while True:
        if data[at:at + width] == terminator:
            if at + width != len(data):
                raise ValueError('IPS has trailing bytes or an unsupported truncate extension')
            return writes
        if len(data) - at < width + 2:
            raise ValueError('IPS record or terminator is truncated')
        offset = int.from_bytes(data[at:at + width], 'big') - NSO_HEADER_BYTES
        size = int.from_bytes(data[at + width:at + width + 2], 'big')
        at += width + 2
        if size:
            if len(data) - at < size:
                raise ValueError('IPS record payload is truncated')
            payload = data[at:at + size]
            at += size
        else:
            if len(data) - at < 3:
                raise ValueError('IPS RLE record is truncated')
            size = int.from_bytes(data[at:at + 2], 'big')
            if not size or total + size > MAX_DECODED_BYTES:
                raise ValueError('IPS RLE record is empty or exceeds the decoded byte budget')
            payload = data[at + 2:at + 3] * size
            at += 3
        total += size
        if len(writes) >= MAX_SEGMENTS or total > MAX_DECODED_BYTES:
            raise ValueError('IPS exceeds its decoded write budget')
        _segment(offset, payload)
        writes.append((offset, payload))


def _pchtxt(data, expected):
    try:
        source = data.decode('utf-8-sig')
    except UnicodeError as error:
        raise ValueError('PCHTXT must be UTF-8 text') from error
    if any((ord(c) < 32 and c not in '\t\r\n') or 0x7F <= ord(c) <= 0x9F for c in source):
        raise ValueError('PCHTXT contains unsupported control characters')
    writes, total, shift = [], 0, 0
    header = enabled = stopped = flag = section = False
    for number, line in enumerate(source.split('\n'), 1):
        # This subset supports ordinary line comments, fixed hexadecimal byte
        # writes, one NSO build header, and the generator's control directives.
        line = line.split('//', 1)[0].strip()
        if not line or line.startswith('#'):
            continue
        if stopped:
            raise ValueError(f'PCHTXT has content after @stop on line {number}')
        if line.startswith('@nsobid-'):
            if header or writes or flag or enabled:
                raise ValueError('PCHTXT build header is duplicated or out of order')
            if _build(line[len('@nsobid-'):]) != expected:
                raise ValueError('PCHTXT build ID does not match this game build')
            header = True
            continue
        if not header:
            raise ValueError('PCHTXT must begin with its NSO build header')
        match = re.fullmatch(r'@flag\s+offset_shift\s+(0x[0-9a-fA-F]{1,8})', line)
        if match:
            if flag or section:
                raise ValueError('PCHTXT offset_shift is duplicated or out of order')
            shift, flag = int(match[1], 16), True
        elif line == '@enabled':
            enabled = section = True
        elif line == '@disabled':
            enabled, section = False, True
        elif line == '@stop':
            stopped = True
        else:
            match = re.fullmatch(r'([0-9a-fA-F]{1,8})\s+([0-9a-fA-F]+)', line)
            if not match or len(match[2]) % 2:
                raise ValueError(f'unsupported PCHTXT syntax on line {number}')
            if not section:
                raise ValueError('PCHTXT byte writes need @enabled or @disabled')
            payload = bytes.fromhex(match[2])
            offset = int(match[1], 16) + shift - NSO_HEADER_BYTES
            _segment(offset, payload)
            # Disabled sections are syntax checked but do not write files.
            if enabled:
                total += len(payload)
                if len(writes) >= MAX_SEGMENTS or total > MAX_DECODED_BYTES:
                    raise ValueError('PCHTXT exceeds its decoded write budget')
                writes.append((offset, payload))
    if not header or not stopped:
        raise ValueError('PCHTXT needs an NSO build header and @stop')
    return writes


def decode_patch(data: bytes, suffix: str, expected_build_id: str) -> list[tuple[int, bytes]]:
    """Return effective main writes; unknown syntax and ambiguous writes refuse."""
    expected = _build(expected_build_id)
    if not isinstance(data, bytes) or len(data) > MAX_INPUT_BYTES:
        raise ValueError('patch must be bytes and at most 8 MiB')
    if not isinstance(suffix, str):
        raise ValueError('patch suffix must be .ips or .pchtxt')
    suffix = suffix.lower()
    if suffix == '.ips':
        writes = _ips(data)
    elif suffix == '.pchtxt':
        writes = _pchtxt(data, expected)
    else:
        raise ValueError('unsupported patch suffix: ' + suffix)
    _normalized(writes)
    return writes


def validate_fixed_ranges(writes, tablepath, build=None):
    """Refuse any byte over Tsuchi's reserved stubs or checked patch sites. `build` is the key of a build
    in the table's builds folder; none means the table's own values."""
    import yaml
    checked = _normalized(writes)
    try:
        table = yaml.safe_load(Path(tablepath).read_text(encoding='utf-8'))
        if build:
            own = yaml.safe_load((Path(tablepath).parent / 'builds' / (build + '.yaml')).read_text(encoding='utf-8'))
            table = {'constants': own['constants'], 'patch_space': own.get('patch_space') or [],
                     **{group: own.get(group, {}) for group in ('code_sites', 'native_routes', 'conditional_sites')}}
        constants = table['constants']
        first, end = constants['stub_base'], constants['stub_end']
        if type(first) is not int or type(end) is not int or not 0 <= first <= end:
            raise ValueError('invalid Tsuchi stub reservation')
        protected = [(first, end, 'stub reservation')]
        for address, size in table.get('patch_space') or []:
            protected.append((address, address + size, 'patch space'))
        for group in ('code_sites', 'native_routes', 'conditional_sites'):
            for name, site in table.get(group, {}).items():
                # A route at the same place as another (same_as) has none of its own.
                if site == 'absent' or (isinstance(site, dict) and 'same_as' in site):
                    continue
                start = site['address']
                if type(start) is not int or start < 0:
                    raise ValueError('invalid Tsuchi patch address')
                protected.append((start, start + (8 if group == 'conditional_sites' else 4), group + '.' + name))
    except (KeyError, TypeError, AttributeError, yaml.YAMLError) as error:
        raise ValueError('invalid Tsuchi patch table') from error
    for offset, data in checked:
        for first, end, name in protected:
            if offset < end and first < offset + len(data):
                raise ValueError(f'patch overlaps Tsuchi {name} at main+0x{max(offset, first):X}')


def validate_patch_conflicts(namedwrites: dict[str, list[tuple[int, bytes]]]):
    """Allow identical overlap; refuse different bytes across enabled packages."""
    if not isinstance(namedwrites, dict) or len(namedwrites) > 256:
        raise ValueError('patch conflict check accepts at most 256 named packages')
    combined, total = [], 0
    for name, writes in namedwrites.items():
        if not isinstance(name, str) or not name:
            raise ValueError('patch package needs a name')
        normalized = _normalized(writes)
        total += sum(len(data) for _, data in normalized)
        if total > MAX_COMBINED_BYTES:
            raise ValueError('combined patch writes exceed 64 MiB')
        if len(combined) + len(normalized) > MAX_COMBINED_SEGMENTS:
            raise ValueError('combined patches exceed 65536 write segments')
        combined.extend(normalized)
    _normalized(combined, MAX_COMBINED_BYTES, MAX_COMBINED_SEGMENTS)
