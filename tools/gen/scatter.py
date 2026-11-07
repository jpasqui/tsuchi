"""A build's patch in pieces, for a build where the one block of tools/gen/stubs.py cannot go.

The same routes as the block, placed in the free places the build's file lists under `patch_space`
and in the spare space after the code, which is used first. A piece is as small as two instructions,
and its last jumps to the next. See DESIGN.md, "The patch's space".

    a code site     mov w17, #number; adr x16, way back; b shared. The way back is the instruction
                    that was replaced and a jump to the one after it, or for a call, a jump to what
                    it called.
    a native route  push x30, x16; bl native_shared; pop them; what was replaced; and for each way on
                    from the framework, a pop and a jump.
    a conditional   as in the block.
    the mark        the value that marks a patch, then its hash. These two may sit apart.

Every address is decided here, before anything is assembled, so that the framework's table and a
checker can know them with no assembler. The assembler and linker then have to agree; the patch
builder compares them.
"""
from . import stubs

REACH = (1 << 27) - 4  # how far b and bl go
SHORT = 1 << 20        # how far b.cond, cbz and adr go
NEAR = SHORT // 2      # pieces this near one place reach each other with those


class Word:
    """One instruction, as assembly text. `ends` says nothing runs on after it."""
    def __init__(self, text, ends=False):
        self.text, self.ends, self.words = text, ends, 1


class Jump:
    """A b or bl to an address or a label, by further jumps if it is too far for one."""
    def __init__(self, target, link='b'):
        self.target, self.link, self.ends, self.words = target, link, link == 'b', 1


class Short:
    """An instruction ending in a place it reaches only over SHORT: `b.ne`, `cbz x1,`, `adr x16,`."""
    def __init__(self, start, target):
        self.start, self.target, self.ends, self.words = start, target, False, 1


class Moved:
    """A literal load, whose reach has to be worked out where it lands."""
    def __init__(self, site):
        self.site, self.ends, self.words = site, False, 1


class Together:
    """Instructions that have to stay next to each other."""
    def __init__(self, items):
        self.items, self.ends, self.words = items, items[-1].ends, sum(item.words for item in items)


class Quad:
    """A value the framework reads, not code."""
    def __init__(self, text):
        self.text, self.ends, self.words = text, True, 2


class Region:
    def __init__(self, first, end, tail=False):
        self.first, self.at, self.end, self.tail = first, first, end, tail

    def free(self):
        return (self.end - self.at) // 4


class Piece:
    def __init__(self, address):
        self.address, self.items, self.lines = address, [], []

    def words(self):
        return sum(item.words for item in self.items if not isinstance(item, str))


def mov64(register, value):
    """A constant with no literal, so that no piece holds data."""
    return [Word(f'movz {register}, #{value & 0xFFFF}')] + [Word(f'movk {register}, #{(value >> shift) & 0xFFFF}, lsl #{shift}')
                                                            for shift in (16, 32, 48)]


def shared_chain():
    """Like the block's shared code, with x16 holding the way back and not computed from the number."""
    return ['shared',
            Word('stp x0, x1, [sp, #-16]!'),
            *mov64('x0', stubs.STATUS_MAGIC),
            Word('adrp x1, status_words'),
            Word('ldr x1, [x1, :lo12:status_words]'),
            Word('cmp x0, x1'),                              # has the framework published its status?
            Word('adrp x1, status_words'),
            Word('ldr x1, [x1, :lo12:status_words + 8]'),
            Short('b.ne', 'absent'),
            Word(f'ldr x1, [x1, #{stubs.STATUS_SITE_JUMP}]'),  # the framework's entry for code sites
            Short('cbz x1,', 'absent'),
            Word('mov x16, x1'),
            Word('ldp x0, x1, [sp], #16'),
            Word('br x16', True),
            'absent',                                        # no framework, or one built with another patch
            Word('ldp x0, x1, [sp], #16'),
            Word('br x16', True)]


def native_shared_chain(offset):
    """The block's native_shared, instruction for instruction, with no literal."""
    return ['native_shared',
            Word('stp x17, xzr, [sp, #-16]!'),
            Word('stp x0, x1, [sp, #-32]!'),
            Word('mrs x17, nzcv'),
            Word('str x17, [sp, #16]'),
            Word('adrp x16, status_words'),
            Word('ldr x0, [x16, :lo12:status_words]'),
            *mov64('x1', stubs.STATUS_MAGIC),
            Word('cmp x0, x1'),
            Short('b.ne', 'native_absent'),
            Word('ldr x16, [x16, :lo12:status_words + 8]'),
            Short('cbz x16,', 'native_absent'),
            Word('ldr x0, [x16]'),
            Word('cmp x0, x1'),
            Short('b.ne', 'native_absent'),
            Word('ldr w0, [x16, #12]'),
            Word(f'mov w1, #{offset + 8}'),
            Word('cmp w0, w1'),
            Short('b.lo', 'native_absent'),
            Word('ldr w0, [x16, #32]'),
            Word('cmp w0, #1'),
            Short('b.ne', 'native_absent'),
            Word(f'ldr x16, [x16, #{offset}]'),
            Short('cbz x16,', 'native_absent'),
            Word('br x16', True),
            'native_absent',
            Word('ldr x17, [sp, #16]'),
            Word('msr nzcv, x17'),
            Word('ldp x0, x1, [sp], #32'),
            Word('ldp x17, xzr, [sp], #16'),
            Word('ret', True)]


def replay_items(site, label):
    """What stubs.native_replay says, as items."""
    word, address = site['instruction'], site['address']
    target = stubs.displaced_target(site)
    lines = stubs.native_replay(site, label)
    if (word & 0x3B000000) == 0x18000000:
        return [Moved(site), Jump(address + 4)]
    items = []
    for line in lines:
        if line == f'b {label}_target':
            items.append(Jump(target))
        elif line == f'b {label}_next':
            items.append(Jump(address + 4))
        else:
            items.append(Word(line, line.startswith('br ') or (word & 0xFFFFFC1F) in (0xD61F0000, 0xD65F0000)))
    # An inverted short branch skips exactly the one jump after it.
    return [Together(items)] if len(items) == 3 and items[0].text.startswith('.inst') and isinstance(items[1], Jump) else items


class Placed:
    """Where everything went. `labels` gives the address of each route's parts."""
    def __init__(self):
        self.pieces, self.labels, self.symbols = [], {}, {}
        self.block = self.block_hash = self.jumps = 0


class Space:
    def __init__(self, table):
        constants = table['constants']
        self.regions = [Region(constants['stub_base'], constants['stub_end'], True)]
        self.regions += [Region(address, address + size) for address, size in table.get('patch_space') or []]
        self.key = table['key']
        self.home = constants['stub_base']
        self.placed = Placed()
        self.hops = {}

    def refuse(self, what, near, words):
        free = sum(region.free() for region in self.regions)
        raise ValueError(f'build {self.key}: nowhere for {what} ({words} words near {near:#x}); {free} words are free in its '
                         f'patch space. Give it more with tools/patch_space.py find')

    def take(self, need, whole, near, limits, what):
        """The region a chain goes on in: the space after the code if all of it fits there, then the
        nearest place that holds all of it, then the nearest that holds any of it."""
        fits = [region for region in self.regions if region.free() >= need and
                all(abs(region.at - place) <= reach and abs(region.end - 4 - place) <= reach for place, reach in limits)]
        whole_fits = [region for region in fits if region.free() >= whole]
        for region in whole_fits:
            if region.tail:
                return region
        chosen = whole_fits or fits
        if not chosen:
            self.refuse(what, near, need)
        return min(chosen, key=lambda region: abs(region.at - near))

    def data(self, text):
        """Two words on an 8-byte boundary, for the framework to read."""
        best = None
        for region in self.regions:
            aligned = (region.at + 7) & ~7
            if aligned + 8 > region.end:
                continue
            if region.tail:
                best = region
                break
            if best is None or abs(region.at - self.home) < abs(best.at - self.home):
                best = region
        if best is None:
            self.refuse('the mark', self.home, 2)
        best.at = (best.at + 7) & ~7
        piece = Piece(best.at)
        piece.items.append(Quad(text))
        best.at += 8
        self.placed.pieces.append(piece)
        return piece.address

    def put(self, chain, near, limits, what):
        """Lays a chain of labels and items out in pieces, cutting it where a place runs out."""
        chain, at, cuts = list(chain), 0, 0
        while at < len(chain):
            next_word = at
            while isinstance(chain[next_word], str):
                next_word += 1
            region = self.take(self.first_need(chain, next_word), sum(item.words for item in chain[at:] if not isinstance(item, str)), near, limits, what)
            piece = Piece(region.at)
            room, last = region.free(), None
            while at < len(chain):
                word = at
                while isinstance(chain[word], str):
                    word += 1
                item = chain[word]
                if item.words + (not item.ends and word + 1 < len(chain)) > room:
                    if last is not None and not last.ends:
                        # What comes next starts with this jump, a label included, so an address just after a bl stays that.
                        piece.items += chain[at:word]
                        cuts += 1
                        label = f'{what}_cut_{cuts}'
                        piece.items.append(Jump(label))
                        room -= 1
                        at = word
                        chain.insert(at, label)
                    break
                piece.items += chain[at:word + 1]
                room -= item.words
                at, last = word + 1, item
            region.at += 4 * piece.words()
            self.placed.pieces.append(piece)
            near = piece.address

    @staticmethod
    def first_need(chain, word):
        item = chain[word]
        return item.words + (not item.ends and word + 1 < len(chain))

    def route(self, start, target):
        """Where a branch at `start` should go to get to `target`: there, or a jump on the way."""
        if abs(target - start) <= REACH:
            return target
        hops = self.hops.setdefault(target, [])
        known = [hop for hop in hops if abs(hop - start) <= REACH and abs(hop - target) < abs(start - target)]
        if known:
            return min(known, key=lambda hop: abs(hop - target))
        best = None
        for region in self.regions:
            if region.free() and abs(region.at - start) <= REACH and (best is None or abs(region.at - target) < abs(best.at - target)):
                best = region
        if best is None or abs(best.at - target) >= abs(start - target):
            raise ValueError(f'build {self.key}: no free place on the way from {start:#x} to {target:#x}, more than 128 MB apart. '
                             f'Give it more with tools/patch_space.py find')
        hop = best.at
        best.at += 4
        piece = Piece(hop)
        piece.items.append(Word('', True))
        piece.lines.append(f'    b {self.symbol(self.route(hop, target))}\n')
        hops.append(hop)
        self.placed.pieces.append(piece)
        self.placed.jumps += 1
        return hop

    def symbol(self, address):
        name = f'at_{address:x}'
        self.placed.symbols[name] = address
        return name


def lay_out_words(space, piece, labels):
    """Fills in a laid-out piece's lines now that every label has an address."""
    at = piece.address

    def address_of(target):
        return labels[target] if isinstance(target, str) else target

    def line(item):
        nonlocal at
        if isinstance(item, Jump):
            text = f'{item.link} {space.symbol(space.route(at, address_of(item.target)))}'
        elif isinstance(item, Short):
            target = address_of(item.target)
            if abs(target - at) >= SHORT:
                raise ValueError(f'build {space.key}: {item.start} at {at:#x} cannot reach {target:#x}')
            text = f'{item.start} {space.symbol(target)}'
        elif isinstance(item, Moved):
            word, address = item.site['instruction'], item.site['address']
            delta = address + 4 * stubs.signed((word >> 5) & 0x7FFFF, 19) - at
            if delta % 4 or abs(delta) >= SHORT:
                raise ValueError(f'build {space.key}: native route at {address:#x}: literal load target is outside relocated reach')
            text = f'.inst {(word & ~(0x7FFFF << 5)) | ((delta // 4 & 0x7FFFF) << 5):#010x}'
        else:
            text = item.text
        at += 4
        return f'    {text}\n'

    for item in piece.items:
        if isinstance(item, str):
            piece.lines.append(f'{item}:\n')
        elif isinstance(item, Quad):
            piece.lines.append(f'    .quad {item.text}\n')
            at += 8
        elif isinstance(item, Together):
            piece.lines += [line(each) for each in item.items]
        else:
            piece.lines.append(line(item))


def place(table):
    """Every piece of a build's patch, and each route's addresses written back into the table."""
    space = Space(table)
    constants = table['constants']
    placed = space.placed
    placed.block = space.data(f'{stubs.PATCH_MAGIC:#x}')
    placed.block_hash = space.data('PATCH_HASH')
    live_sites = any(not site.get('absent') for site in table['code_sites'].values())
    routes = table.get('native_routes') or {}
    live_routes = any(not site.get('absent') for site in routes.values())
    home = [(space.home, NEAR)]
    if live_sites:
        space.put(shared_chain(), space.home, home, 'shared')
    if live_routes or constants.get('dynamic_hook_slots', 0):
        space.put(native_shared_chain(constants['native_jump_offset']), space.home, home, 'native')
        space.put(['native_return', Word('ldp x30, x16, [sp], #16'), Word('ret', True)], space.home, home, 'native_return')
    exits = {}
    for index, (name, site) in enumerate(routes.items()):
        if site.get('absent'):
            continue
        word, address = site['instruction'], site['address']
        moved = (word & 0x3B000000) == 0x18000000
        limits = [(address, NEAR if moved else REACH)]
        label = f'native_{index}'
        target = stubs.displaced_target(site)
        if target is not None:
            if target < 0:
                raise ValueError(f'native route at {address:#x}: displaced target is negative')
            placed.symbols[f'{label}_target'] = target
        placed.symbols[f'{label}_next'] = address + 4
        chain = [f'{label}_entry', Word('stp x30, x16, [sp, #-16]!'), Jump('native_shared', 'bl'),
                 f'{label}_returned', Word('ldp x30, x16, [sp], #16'), f'{label}_original', *replay_items(site, label)]
        if site['has_resume']:
            chain += [f'{label}_resume', Word('ldp x30, x16, [sp], #16'), Jump(address + 4)]
        space.put(chain, address, limits, label)
        for exit_name, exit_target in site['exits'].items():
            if exit_target not in exits:
                exits[exit_target] = f'{label}_exit_{exit_name}'
                space.put([exits[exit_target], Word('ldp x30, x16, [sp], #16'), Jump(exit_target)], address, [(address, REACH)], exits[exit_target])
    for index, site in enumerate((table.get('conditional_sites') or {}).values()):
        if site.get('absent'):
            continue
        register, back = site['register'], site['address'] + 4
        chain = [f'condition_{index}']
        for value in site['admit']:
            chain += [Word(f'cmp w{register}, #{value}'), Short('b.eq', f'admitted_{index}')]
        chain += [Word(f'.inst {site["instruction"]:#010x}'), Jump(back),
                  f'admitted_{index}', Word(f'cmp w{register}, #{max(site["admit"]) + 1}'), Jump(back)]
        space.put(chain, site['address'], [(site['address'], NEAR)], f'condition_{index}')
    for index, site in enumerate(table['code_sites'].values()):
        if site.get('absent'):
            continue
        address = site['address']
        page = None if site['call'] else stubs.moved_page(address, site['instruction'])
        replaced = Word(f'.inst {site["instruction"]:#010x}') if page is None else Word(f'adrp x{site["instruction"] & 31}, {space.symbol(page)}')
        way_back = [Jump(site['target'])] if site['call'] else [replaced, Jump(address + 4)]
        before = len(placed.pieces)
        space.put([f'original_{index}', *way_back], address, [(address, REACH - NEAR)], f'original_{index}')
        original = placed.pieces[before].address
        space.put([f'entry_{index}', Word(f'mov w17, #{index}'), Short('adr x16,', f'original_{index}'), Jump('shared')],
                  original, [(original, NEAR), (address, REACH)], f'entry_{index}')
    # Every piece's size is settled; a jump too far takes more words from what is left.
    for piece in placed.pieces:
        at = piece.address
        for item in piece.items:
            if isinstance(item, str):
                placed.labels[item] = at
            else:
                at += 4 * item.words
    for piece in list(placed.pieces):
        lay_out_words(space, piece, placed.labels)
    labels = placed.labels
    for index, site in enumerate(table['code_sites'].values()):
        site['entry'], site['original'] = (0, 0) if site.get('absent') else (labels[f'entry_{index}'], labels[f'original_{index}'])
    for index, site in enumerate(routes.values()):
        label = f'native_{index}'
        site['number'] = index
        if site.get('absent'):
            site['entry'] = site['returned'] = site['original'] = site['resume'] = 0
            site['exit_entries'] = {name: 0 for name in site['exits']}
            continue
        site['entry'], site['returned'], site['original'] = labels[f'{label}_entry'], labels[f'{label}_returned'], labels[f'{label}_original']
        site['resume'] = labels[f'{label}_resume'] if site['has_resume'] else 0
        site['exit_entries'] = {name: labels[exits[target]] for name, target in site['exits'].items()}
    for index, site in enumerate((table.get('conditional_sites') or {}).values()):
        site['entry'] = 0 if site.get('absent') else labels[f'condition_{index}']
    table['dynamic_hook_base'] = 0
    placed.pieces.sort(key=lambda piece: piece.address)
    placed.native_shared = labels.get('native_shared', 0)
    placed.native_return = labels.get('native_return', 0)
    return placed


def assembly(table, patch_hash):
    """(assembly text, linker script) for a build in pieces."""
    placed = place(table)
    source, sections = [], []
    for number, piece in enumerate(placed.pieces):
        name = f'.piece{number}'
        source.append(f'.section {name}, "ax"\n')
        source += [line.replace('PATCH_HASH', f'{patch_hash:#x}') for line in piece.lines]
        sections.append((piece.address, name))
    sites = [('.site', index, site, 'bl' if site['call'] else 'b', f'entry_{index}') for index, site in enumerate(table['code_sites'].values())]
    sites += [('.native_site', index, site, 'b', f'native_{index}_entry') for index, site in enumerate((table.get('native_routes') or {}).values())]
    sites += [('.condition', index, site, 'b', f'condition_{index}') for index, site in enumerate((table.get('conditional_sites') or {}).values())]
    for kind, index, site, branch, target in sites:
        if site.get('absent'):
            continue
        if abs(placed.labels[target] - site['address']) > REACH:
            raise ValueError(f'build {table["key"]}: the site at {site["address"]:#x} cannot reach its entry')
        source.append(f'.section {kind}{index}, "ax"\n    {branch} {target}\n')
        sections.append((site['address'], f'{kind}{index}'))
    script = [f'status_words = {table["data"].get("status_words", 0):#x};\n']
    script += [f'{name} = {address:#x};\n' for name, address in sorted(placed.symbols.items())]
    script.append('SECTIONS {\n')
    script += [f'    {name} {address:#x} : {{ *({name}) }}\n' for address, name in sorted(sections)]
    script.append('    /DISCARD/ : { *(.comment .note .note.* .eh_frame .eh_frame_hdr) }\n}\n')
    return ''.join(source), ''.join(script)


def expected_sections(table):
    """{section name: (address, bytes)} of a build in pieces, for the patch builder to hold the linker to."""
    placed = place(table)
    out = {f'.piece{number}': (piece.address, 4 * piece.words()) for number, piece in enumerate(placed.pieces)}
    for kind, group in (('.site', 'code_sites'), ('.native_site', 'native_routes'), ('.condition', 'conditional_sites')):
        for index, site in enumerate((table.get(group) or {}).values()):
            if not site.get('absent'):
                out[f'{kind}{index}'] = (site['address'], 4)
    return out, placed.labels


def claimed(table):
    """[(address, bytes)] of everything placed in free space."""
    return [(piece.address, 4 * piece.words()) for piece in place(table).pieces]


def unused_regions(table):
    """The listed regions a layout took nothing from."""
    space_used = claimed(table)
    return [[address, size] for address, size in table.get('patch_space') or []
            if not any(address < at + bytes_ and at < address + size for at, bytes_ in space_used)]
