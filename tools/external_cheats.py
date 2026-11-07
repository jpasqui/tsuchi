"""Parse cheat blocks and render a selected set for the prelaunch manager.

This module checks the text container and its storage limits. Atmosphere or the
emulator executes the words; parsing does not establish gameplay compatibility,
bound loop execution, or provide a way to undo writes.
"""
import re
import unicodedata

MAX_FILE_BYTES = 64 * 1024
MAX_CODES = 127
MAX_NAME_BYTES = 63
MAX_BLOCK_WORDS = 256
MAX_SELECTED_WORDS = 1024
WORD = re.compile(r'[0-9a-fA-F]{8}\Z')


def _name(value):
    if type(value) is not str or not value.strip() or any(
            c in '[]{}' or unicodedata.category(c).startswith('C') or
            (c.isspace() and c != ' ') for c in value):
        raise ValueError('cheat names must contain text without delimiters or control characters')
    if len(value.encode('utf-8')) > MAX_NAME_BYTES:
        raise ValueError('cheat name exceeds 63 UTF-8 bytes')
    return value


def _block(value):
    if type(value) is not dict or set(value) != {'name', 'words'}:
        raise ValueError('cheat block must contain only name and words')
    _name(value['name'])
    words = value['words']
    if type(words) is not list or not 1 <= len(words) <= MAX_BLOCK_WORDS:
        raise ValueError('cheat block must contain 1 to 256 words')
    if any(type(word) is not int or not 0 <= word <= 0xFFFFFFFF for word in words):
        raise ValueError('cheat words must be unsigned 32-bit integers')


def _encoded_block(block, master=False):
    left, right = ('{', '}') if master else ('[', ']')
    return (left + block['name'] + right + '\n' +
            ''.join(f'{word:08X}\n' for word in block['words'])).encode('utf-8')


def _validate(parsed):
    if type(parsed) is not dict or set(parsed) != {'master', 'codes'}:
        raise ValueError('parsed cheats must contain only master and codes')
    codes, master = parsed['codes'], parsed['master']
    if type(codes) is not list or not 1 <= len(codes) <= MAX_CODES:
        raise ValueError('cheat set must contain 1 to 127 normal blocks')
    names = set()
    size = 0
    for is_master, block in ([(True, master)] if master is not None else []) + \
            [(False, code) for code in codes]:
        _block(block)
        if block['name'] in names:
            raise ValueError('cheat names must be unique across all blocks')
        names.add(block['name'])
        size += len(_encoded_block(block, is_master))
        if size >= MAX_FILE_BYTES:
            raise ValueError('normalized cheat set must be smaller than 64 KiB')


def parse(data):
    """Return JSON-compatible master/codes blocks from bounded UTF-8 bytes.

    Headers occupy their own lines. Word lines contain space- or tab-separated
    eight-digit hexadecimal words. Names are preserved, and comments and byte
    order marks are refused. A set requires at least one normal block.
    """
    if type(data) is not bytes or len(data) >= MAX_FILE_BYTES:
        raise ValueError('cheat source must be bytes smaller than 64 KiB')
    try:
        source = data.decode('utf-8')
    except UnicodeError as error:
        raise ValueError('cheat source must be UTF-8') from error
    if any((unicodedata.category(c).startswith('C') and c not in '\t\r\n')
           for c in source):
        raise ValueError('cheat source contains a control character or byte order mark')
    parsed = {'master': None, 'codes': []}
    current = None
    for line in source.replace('\r\n', '\n').replace('\r', '\n').split('\n'):
        line = line.strip(' \t')
        if not line:
            continue
        if line[0] in '[{':
            close = ']' if line[0] == '[' else '}'
            if not line.endswith(close):
                raise ValueError('cheat header is malformed or shares a line with words')
            current = {'name': _name(line[1:-1]), 'words': []}
            if line[0] == '{':
                if parsed['master'] is not None:
                    raise ValueError('cheat set may contain only one master block')
                parsed['master'] = current
            else:
                if len(parsed['codes']) >= MAX_CODES:
                    raise ValueError('cheat set exceeds 127 normal blocks')
                parsed['codes'].append(current)
            continue
        words = re.split(r'[ \t]+', line)
        if current is None or any(not WORD.fullmatch(word) for word in words):
            raise ValueError('cheat words require a header and eight hexadecimal digits each')
        if len(current['words']) + len(words) > MAX_BLOCK_WORDS:
            raise ValueError('cheat block exceeds 256 words')
        current['words'].extend(int(word, 16) for word in words)
    _validate(parsed)
    return parsed


def render(parsed, enabled):
    """Render zero-based selected code indexes, including their shared master.

    Empty selection returns no files. Otherwise the keys are ``cheat`` and
    ``toggles``. Selection never changes block order or edits instruction words.
    Every selected normal block receives an explicit true console toggle.
    """
    _validate(parsed)
    if type(enabled) not in (set, frozenset) or any(
            type(index) is not int or not 0 <= index < len(parsed['codes'])
            for index in enabled):
        raise ValueError('enabled cheats must be a set of valid zero-based code indexes')
    if not enabled:
        return {}
    selected = [code for index, code in enumerate(parsed['codes']) if index in enabled]
    master = parsed['master']
    count = sum(len(code['words']) for code in selected)
    if master is not None:
        count += len(master['words'])
    if count > MAX_SELECTED_WORDS:
        raise ValueError('selected cheats and master exceed 1024 words')
    cheat = (_encoded_block(master, True) if master is not None else b'') + \
        b''.join(_encoded_block(code) for code in selected)
    toggles = ''.join('[' + code['name'] + ']\ntrue\n' for code in selected).encode('utf-8')
    if len(cheat) >= MAX_FILE_BYTES or len(toggles) >= MAX_FILE_BYTES:
        raise ValueError('generated cheat files must be smaller than 64 KiB')
    return {'cheat': cheat, 'toggles': toggles}
