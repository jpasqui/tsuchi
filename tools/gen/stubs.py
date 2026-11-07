"""Where things sit in the spare space at the end of the game's code.

Some functions the game calls directly, not through a table, so a hook on them
needs one instruction of the game's code changed to jump elsewhere. All of
those jumps land in one block the framework owns, laid out here. The patch
builder (tools/build_patch.py) writes the block; the framework reads the same
numbers from game_table.h to know where each site leads.

    base                      the mark: a value that says the block is there, then the hash of
                              everything the block was made from
    base + MARK               shared code: is the framework there? if so, jump into it
    base + MARK + SHARED      one entry per site: its number, then a jump to the shared code
    ... + 8 * sites           one way back per site: the instruction that was replaced, then a
                              jump to the instruction after it (or, for a call, a jump to what
                              the call called)

The block passes a site's number, and the framework answers from its own table,
so the two have to be made from the same table. The hash is how the framework
tells: it is built with the hash of its own table, and comes into the block only
when the block carries the same one.

A site a build does not have (`absent`) keeps its place in the block, so the sites keep their
numbers, and changes nothing in the game's code.

A build with a list of free places, `patch_space`, whose block would not fit or could not be reached
from every site, has its patch in pieces instead (tools/gen/scatter.py). A build with no such list
is refused.
"""
import hashlib

from . import scatter

MARK = 16     # bytes: the value that marks a block, then the hash
SHARED = 64   # bytes of shared code and its constant
PER_SITE = 8  # two instructions

# What the shared code knows about the framework's status block: the value that marks it, and
# where in it the entry for code sites is kept. framework/target/module.cpp is held to both.
STATUS_MAGIC = 0x31544154534D3450  # "P4MSTAT1"
STATUS_SITE_JUMP = 192
# The value a block begins with.
PATCH_MAGIC = 0x4843544150555354  # "TSUPATCH"

CODE = '''\
.section .stub, "ax"
    .quad {patch_magic:#x}
    .quad {patch_hash:#x}
shared:                                  // x17 holds the site's number
    stp x0, x1, [sp, #-16]!
    adrp x16, status_words
    ldr x0, [x16, :lo12:status_words]
    ldr x1, magic
    cmp x0, x1                           // has the framework published its status?
    ldr x16, [x16, :lo12:status_words + 8]
    ldp x0, x1, [sp], #16
    b.ne absent
    ldr x16, [x16, #{site_jump}]          // the framework's entry for code sites
    cbz x16, absent
    br x16
absent:                                  // no framework, or one built with another patch: straight back to the game
    adr x16, originals
    add x16, x16, w17, uxtw #3
    br x16
magic:
    .quad {magic:#x}
entries:
'''


def layout(base, end, count):
    """(entries, originals) addresses for `count` sites, or an error when they do not fit."""
    entries = base + MARK + SHARED
    originals = entries + PER_SITE * count
    if originals + PER_SITE * count > end:
        raise ValueError(f'{count} code sites need {MARK + SHARED + 2 * PER_SITE * count:#x} bytes; the spare space has {end - base:#x}')
    return entries, originals


def is_position_dependent(word):
    """True for an instruction that means something else when moved: branches and address loads."""
    return ((word & 0x7C000000) == 0x14000000 or   # b, bl
            (word & 0x1F000000) == 0x10000000 or   # adr, adrp
            (word & 0x7E000000) == 0x34000000 or   # cbz, cbnz
            (word & 0x7E000000) == 0x36000000 or   # tbz, tbnz
            (word & 0xFF000010) == 0x54000000 or   # b.cond
            (word & 0x3B000000) == 0x18000000)     # ldr from a literal


def moved_page(address, word):
    """The page an `adrp` at `address` loads, which the way back loads again from where it is; None for anything else."""
    if (word & 0x9F000000) != 0x90000000 or word & 31 == 31:
        return None
    pages = signed(((word >> 29) & 3) | (((word >> 5) & 0x7FFFF) << 2), 21)
    return (address & ~4095) + (pages << 12)


def call_target(address, word):
    """What a `bl` at `address` calls."""
    if (word & 0xFC000000) != 0x94000000:
        raise ValueError(f'{word:#010x} at {address:#x} is not a call')
    offset = word & 0x3FFFFFF
    if offset & 0x2000000:
        offset -= 0x4000000
    return address + 4 * offset


def ordinary_assembly(table, patch_hash):
    """(assembly text, linker script) for the block and the changed instruction at each site."""
    sites = list(table['code_sites'].values())
    text = [CODE.format(patch_magic=PATCH_MAGIC, patch_hash=patch_hash, site_jump=STATUS_SITE_JUMP, magic=STATUS_MAGIC)]
    for index in range(len(sites)):
        text.append(f'    mov w17, #{index}\n    b shared\n')
    text.append('originals:\n')
    for index, site in enumerate(sites):
        if site.get('absent'):
            text.append('    nop\n    nop\n')
        elif site['call']:
            text.append(f'    b target_{index}\n    nop\n')
        elif moved_page(site['address'], site['instruction']) is not None:
            text.append(f'    adrp x{site["instruction"] & 31}, page_{index}\n    b site_{index} + 4\n')
        else:
            text.append(f'    .word {site["instruction"]:#010x}\n    b site_{index} + 4\n')
    for index, site in enumerate(sites):
        if not site.get('absent'):
            text.append(f'.section .site{index}, "ax"\n    {"bl" if site["call"] else "b"} entries + {PER_SITE * index}\n')
    script = [f'status_words = {table["data"].get("status_words", 0):#x};\n']
    placed = [(table['constants']['stub_base'], '.stub')]
    for index, site in enumerate(sites):
        if site.get('absent'):
            continue
        script.append(f'site_{index} = {site["address"]:#x};\n')
        if site['call']:
            script.append(f'target_{index} = {site["target"]:#x};\n')
        elif moved_page(site['address'], site['instruction']) is not None:
            script.append(f'page_{index} = {moved_page(site["address"], site["instruction"]):#x};\n')
        placed.append((site['address'], f'.site{index}'))
    script.append('SECTIONS {\n')
    script += [f'    {name} {address:#x} : {{ *({name}) }}\n' for address, name in sorted(placed)]
    script.append('    /DISCARD/ : { *(.comment .note .note.* .eh_frame .eh_frame_hdr) }\n}\n')
    return ''.join(text), ''.join(script)


def displaced_target(site):
    """Where the instruction a native route replaces goes or points, or None if it does neither."""
    word, address = site['instruction'], site['address']
    if (word & 0x7C000000) == 0x14000000:
        return address + 4 * signed(word & 0x3ffffff, 26)
    if (word & 0x7E000000) in (0x34000000, 0x36000000) or (word & 0xFF000010) == 0x54000000:
        bits = 14 if (word & 0x7E000000) == 0x36000000 else 19
        return address + 4 * signed((word >> 5) & ((1 << bits) - 1), bits)
    if (word & 0x1F000000) == 0x10000000:
        immediate = signed(((word >> 29) & 3) | (((word >> 5) & 0x7ffff) << 2), 21)
        return ((address & ~4095) + (immediate << 12)) if word & 0x80000000 else address + immediate
    return None


def patch_hash(table):
    """64 bits that stand for everything the patch is made from: the block's code, where it is put,
    and each site's place and instruction, in the table's order. 0 for a table with no sites."""
    if not table['code_sites'] and not table.get('native_routes') and not table.get('conditional_sites') and not table['constants'].get('dynamic_hook_slots', 0):
        return 0
    made_from = '\n'.join(assembly(table, 0))
    return int.from_bytes(hashlib.sha256(made_from.encode('utf-8')).digest()[:8], 'little') or 1


def checked_hash(table):
    """What a passing tools/patch_space.py records: the patch's hash, and what says which program it was checked against."""
    made_from = f'{patch_hash(table)}|{table["build_id"].upper()}|{sorted(table["identity"])}|{table["constants"].get("code_size")}'
    return hashlib.sha256(made_from.encode('utf-8')).hexdigest()[:16]


NATIVE_SHARED = 116


def signed(value, bits):
    return value - (1 << bits) if value & (1 << (bits - 1)) else value


def native_replay(site, label):
    """Relocate native branches/address loads without sacrificing a live register."""
    word, address = site['instruction'], site['address']
    if (word & 0xFC000000) == 0x94000000 or (word & 0xFFFFFC1F) == 0xD63F0000:
        # Tail into the called function with precisely the native call's LR.
        target = call_target(address, word) if (word & 0xFC000000) == 0x94000000 else None
        finish = f'b {label}_target' if target is not None else f'br x{(word >> 5) & 31}'
        return [f'adrp x30, {label}_next', f'add x30, x30, :lo12:{label}_next', finish]
    if (word & 0x7C000000) == 0x14000000:
        return [f'b {label}_target']
    if (word & 0xFFFFFC1F) in (0xD61F0000, 0xD65F0000):
        # BR/RET already leave this gateway. A branch to next is unreachable.
        return [f'.inst {word:#010x}']
    if (word & 0xFF000010) == 0x54000000 and (word & 15) >= 14:
        return [f'b {label}_target']
    if (word & 0x7E000000) in (0x34000000, 0x36000000) or (word & 0xFF000010) == 0x54000000:
        bits = 14 if (word & 0x7E000000) == 0x36000000 else 19
        target = address + 4 * signed((word >> 5) & ((1 << bits) - 1), bits)
        # Invert the condition to skip one unrestricted B. This also handles
        # CBNZ/TBNZ sites too far from the spare space for their short branch.
        inverted = word ^ (1 if (word & 0xFF000010) == 0x54000000 else 0x01000000)
        inverted = (inverted & ~(((1 << bits) - 1) << 5)) | (2 << 5)
        return [f'.inst {inverted:#010x}', f'b {label}_target', f'b {label}_next']
    if (word & 0x1F000000) == 0x10000000:
        immediate = signed(((word >> 29) & 3) | (((word >> 5) & 0x7ffff) << 2), 21)
        register = word & 31
        if register == 31:
            # ADR/ADRP discard their result in XZR. Moving the instruction is
            # safe; spelling ADD with register31 would instead change SP.
            return [f'.inst {word:#010x}', f'b {label}_next']
        target = ((address & ~4095) + (immediate << 12)) if word & 0x80000000 else address + immediate
        lines = [f'adrp x{register}, {label}_target']
        if not word & 0x80000000:
            lines.append(f'add x{register}, x{register}, #{target & 4095}')
        return lines + [f'b {label}_next']
    if (word & 0x3B000000) == 0x18000000:
        # Literal loads need a checked short relocation. Their size is fixed;
        # refuse a target outside the encoding instead of clobbering an IP.
        return [f'.inst {word:#010x}', f'b {label}_next']
    return [f'.inst {word:#010x}', f'b {label}_next']


def native_layout(table):
    """Assign native gateways after the unchanged ordinary-hook layout."""
    count = len(table['code_sites'])
    base, end = table['constants']['stub_base'], table['constants']['stub_end']
    _, originals = layout(base, end, count)
    cursor = originals + PER_SITE * count
    rows = table.get('native_routes', {})
    if not rows and not table['constants'].get('dynamic_hook_slots', 0):
        return cursor, 0, cursor
    shared = cursor
    cursor += NATIVE_SHARED
    exit_targets = {}
    for index, site in enumerate(rows.values()):
        site['entry'], site['returned'], site['original'] = cursor, cursor + 8, cursor + 12
        site['number'] = index
        replay_bytes = 4 * len(native_replay(site, f'native_{index}'))
        cursor = site['original'] + replay_bytes
        site['resume'] = cursor if site.get('has_resume', True) else 0
        if site['resume']:
            cursor += 8
        site['exit_entries'] = {}
        for name, target in site.get('exits', {}).items():
            # An absent route's exits keep their room apart, so the block's layout does not depend on which are absent.
            key = (index, name) if site.get('absent') else target
            if key not in exit_targets:
                exit_targets[key] = cursor
                cursor += 8
            site['exit_entries'][name] = exit_targets[key]
    native_return = cursor
    cursor += 8
    if cursor > end:
        raise ValueError(f'native routes and code sites need {cursor-base:#x} bytes; the spare space has {end-base:#x}')
    return shared, native_return, cursor


def native_assembly(table, patch_hash):
    source, script = ordinary_assembly(table, patch_hash)
    rows = table.get('native_routes', {})
    if not rows and not table['constants'].get('dynamic_hook_slots', 0):
        return source, script
    native_layout(table)
    offset = table['constants']['native_jump_offset']
    source += f'.section .stub, "ax"\nnative_shared:\n' + f"""    stp x17, xzr, [sp, #-16]!
    stp x0, x1, [sp, #-32]!
    mrs x17, nzcv
    str x17, [sp, #16]
    adrp x16, status_words
    ldr x0, [x16, :lo12:status_words]
    ldr x1, magic
    cmp x0, x1
    b.ne native_absent
    ldr x16, [x16, :lo12:status_words + 8]
    cbz x16, native_absent
    // Reuse the expected magic before the size check overwrites w1.
    ldr x0, [x16]
    cmp x0, x1
    b.ne native_absent
    ldr w0, [x16, #12]
    mov w1, #{offset + 8}
    cmp w0, w1
    b.lo native_absent
    ldr w0, [x16, #32]
    cmp w0, #1
    b.ne native_absent
    ldr x16, [x16, #{offset}]
    cbz x16, native_absent
    br x16
native_absent:
    ldr x17, [sp, #16]
    msr nzcv, x17
    ldp x0, x1, [sp], #32
    ldp x17, xzr, [sp], #16
    ret
    .space {NATIVE_SHARED} - (. - native_shared)
"""
    emitted_exits = {}
    for index, site in enumerate(rows.values()):
        word, address = site['instruction'], site['address']
        if site.get('absent'):
            # Room only: nothing in the game's code leads here.
            source += f'.section .stub, "ax"\nnative_{index}_entry:\n    nop\n    nop\nnative_{index}_returned:\n    nop\nnative_{index}_original:\n    nop\n    nop\n'
            if site['resume']:
                source += f'native_{index}_resume:\n    nop\n    nop\n'
            else:
                source += f'.set native_{index}_resume, 0\n'
            for exit_name in site['exits']:
                source += f'native_{index}_exit_{exit_name}:\n    nop\n    nop\n'
            continue
        target = displaced_target(site)
        if target is not None:
            if target < 0:
                raise ValueError(f'native route at {address:#x}: displaced target is negative')
            script = script.replace('SECTIONS {', f'native_{index}_target = {target:#x};\nSECTIONS {{')
        source += f""".section .stub, "ax"
native_{index}_entry:
    stp x30, x16, [sp, #-16]!
    bl native_shared
native_{index}_returned:
    ldp x30, x16, [sp], #16
native_{index}_original:
"""
        replay = native_replay(site, f'native_{index}')
        # LDR literal encodings depend on the assembled instruction address.
        if (site['instruction'] & 0x3B000000) == 0x18000000:
            target = site['address'] + 4 * signed((site['instruction'] >> 5) & 0x7ffff, 19)
            delta = target - site['original']
            if delta % 4 or not -(1 << 20) <= delta < (1 << 20):
                raise ValueError(f'native route at {site["address"]:#x}: literal load target is outside relocated reach')
            relocated = (site['instruction'] & ~(0x7ffff << 5)) | ((delta // 4 & 0x7ffff) << 5)
            replay[0] = f'.inst {relocated:#010x}'
        source += ''.join(f'    {line}\n' for line in replay)
        if site['resume']:
            source += f"""native_{index}_resume:
    ldp x30, x16, [sp], #16
    b native_{index}_next
"""
        else:
            source += f'.set native_{index}_resume, 0\n'
        for exit_name, target in site.get('exits', {}).items():
            symbol = f'native_{index}_exit_{exit_name}'
            if target in emitted_exits:
                source += f'.set {symbol}, {emitted_exits[target]}\n'
            else:
                emitted_exits[target] = symbol
                source += f'{symbol}:\n    ldp x30, x16, [sp], #16\n    b {symbol}_target\n'
                script = script.replace('SECTIONS {', f'{symbol}_target = {target:#x};\nSECTIONS {{')
        source += f'.section .native_site{index}, "ax"\n    b native_{index}_entry\n'
        script = script.replace('SECTIONS {', f'native_{index}_next = {site["address"]+4:#x};\nSECTIONS {{')
        script = script.replace('    /DISCARD/', f'    .native_site{index} {site["address"]:#x} : {{ *(.native_site{index}) }}\n    /DISCARD/')
    source += '.section .stub, "ax"\nnative_return:\n    ldp x30, x16, [sp], #16\n    ret\n'
    # LLD's file placement follows section order as well as explicit virtual
    # addresses. Keep native and ordinary sites in one address-sorted list.
    prefix = script.split('SECTIONS {', 1)[0]
    placed = [(table['constants']['stub_base'], '.stub')]
    placed += [(site['address'], f'.site{index}') for index, site in enumerate(table['code_sites'].values()) if not site.get('absent')]
    placed += [(site['address'], f'.native_site{index}') for index, site in enumerate(rows.values()) if not site.get('absent')]
    script = prefix + 'SECTIONS {\n'
    script += ''.join(f'    {name} {address:#x} : {{ *({name}) }}\n' for address, name in sorted(placed))
    script += '    /DISCARD/ : { *(.comment .note .note.* .eh_frame .eh_frame_hdr) }\n}\n'
    return source, script


def conditional_size(sites):
    return sum(4 * (2 * len(site['admit']) + 4) for site in sites.values() if not site.get('absent'))


def block_end(table):
    """Assign conditional entries after native gateways and bound the entire block."""
    cursor = native_layout(table)[2]
    for site in table.get('conditional_sites', {}).values():
        site['entry'] = cursor
        if not site.get('absent'):
            cursor += 4 * (2 * len(site['admit']) + 4)
    slots = table['constants'].get('dynamic_hook_slots', 0)
    if not isinstance(slots, int) or not 0 <= slots <= 64:
        raise ValueError('dynamic_hook_slots must be between 0 and 64')
    if slots:
        cursor = (cursor + 7) & ~7
    table['dynamic_hook_base'] = cursor
    cursor += 64 * slots
    base, end = table['constants']['stub_base'], table['constants']['stub_end']
    if cursor > end:
        raise ValueError(f'all patch routes need {cursor-base:#x} bytes; the spare space has {end-base:#x}')
    return cursor


def block_size(table):
    return block_end(table) - table['constants']['stub_base']


REACH = (1 << 27) - 4  # how far b and bl go


def block_out_of_reach(table):
    """A site, or where it goes, that the one block is too far from for a b or a bl, or None."""
    for group in ('code_sites', 'native_routes', 'conditional_sites'):
        for name, site in (table.get(group) or {}).items():
            if site.get('absent'):
                continue
            places = [site['entry'] - site['address']]
            if 'target' in site:
                places.append(site['original'] - site['target'])
            # A route replays what it displaced from the block, so a branch's target has to be in reach of it too.
            word = site.get('instruction', 0)
            if group == 'native_routes' and (word & 0x7C000000) == 0x14000000:
                places.append(site['entry'] - (site['address'] + 4 * signed(word & 0x3FFFFFF, 26)))
            places += [site['entry'] - target for target in site.get('exits', {}).values()]
            if any(abs(place) > REACH for place in places):
                return f'{group}.{name}'
    return None


def block_assign(table):
    """Writes the block's addresses into the table, or raises when it does not fit or cannot be reached."""
    if table['constants']['stub_base'] % 8:
        raise ValueError('the block starts on an 8-byte boundary, and the spare space after the code does not')
    entries, originals = layout(table['constants']['stub_base'], table['constants']['stub_end'], len(table['code_sites']))
    for index, site in enumerate(table['code_sites'].values()):
        site['entry'], site['original'] = entries + PER_SITE * index, originals + PER_SITE * index
    block_end(table)
    far = block_out_of_reach(table)
    if far:
        raise ValueError(f'{far} is more than 128 MB from the spare space, and this build lists no patch_space to reach it from')


def in_pieces(table):
    """True for a build whose patch goes in pieces: it lists free places, and the one block will not do."""
    if table.get('patch_space') is None:
        return False
    try:
        block_assign(table)
    except ValueError:
        return True
    return False


def place(table):
    """Decides where a build's patch goes, and writes every route's addresses into the table."""
    if in_pieces(table):
        scatter.place(table)
    else:
        block_assign(table)


def cells(table):
    """The numbers the framework's table holds about the patch as a whole."""
    routed = table['code_sites'] or table.get('native_routes') or table.get('conditional_sites') or table['constants'].get('dynamic_hook_slots', 0)
    if in_pieces(table):
        placed = scatter.place(table)
        return {'block': placed.block, 'block_hash': placed.block_hash, 'native_return': placed.native_return,
                'dynamic_base': 0, 'native_shared': placed.native_shared}
    has_region = 'stub_base' in table['constants'] and 'stub_end' in table['constants']
    if has_region:
        block_end(table)
    block = table['constants'].get('stub_base', 0) if routed else 0
    return {'block': block, 'block_hash': block + 8 if routed else 0,
            'native_return': native_layout(table)[1] if table.get('native_routes') or table['constants'].get('dynamic_hook_slots', 0) else 0,
            'dynamic_base': table.get('dynamic_hook_base', 0), 'native_shared': native_layout(table)[0] if has_region else 0}


def claimed(table):
    """[(address, bytes)] of everything the patch writes outside its sites."""
    if in_pieces(table):
        return scatter.claimed(table)
    return [(table['constants']['stub_base'], block_size(table))]


def assembly(table, patch_hash):
    """Ordinary, full-frame native and conditional routes share one finite block/hash."""
    if in_pieces(table):
        return scatter.assembly(table, patch_hash)
    block_end(table)
    source, script = native_assembly(table, patch_hash)
    conditions = [site for site in table.get('conditional_sites', {}).values()]
    if not any(not site.get('absent') for site in conditions):
        return dynamic_reservation(source, table), script
    for index, site in enumerate(conditions):
        if site.get('absent'):
            source += f'.set condition_{index}, 0\n'
            continue
        source += f'.section .stub, "ax"\ncondition_{index}:\n'
        for value in site['admit']:
            source += f'    cmp w{site["register"]}, #{value}\n    b.eq admitted_{index}\n'
        source += (f'    .word {site["instruction"]:#010x}\n    b condition_return_{index}\n'
                   f'admitted_{index}:\n    cmp w{site["register"]}, #{max(site["admit"])+1}\n'
                   f'    b condition_return_{index}\n'
                   f'.section .condition{index}, "ax"\n    b condition_{index}\n')
    prefix = script.split('SECTIONS {', 1)[0]
    for index, site in enumerate(conditions):
        if site.get('absent'):
            continue
        prefix += (f'condition_return_{index} = {site["address"]+4:#x};\n'
                   f'condition_next_{index} = {site["next_instruction"]:#x};\n')
    placed = [(table['constants']['stub_base'], '.stub')]
    placed += [(site['address'], f'.site{i}') for i, site in enumerate(table['code_sites'].values()) if not site.get('absent')]
    placed += [(site['address'], f'.native_site{i}') for i, site in enumerate(table.get('native_routes', {}).values()) if not site.get('absent')]
    placed += [(site['address'], f'.condition{i}') for i, site in enumerate(conditions) if not site.get('absent')]
    script = prefix + 'SECTIONS {\n'
    script += ''.join(f'    {name} {address:#x} : {{ *({name}) }}\n' for address, name in sorted(placed))
    script += '    /DISCARD/ : { *(.comment .note .note.* .eh_frame .eh_frame_hdr) }\n}\n'
    return dynamic_reservation(source, table), script


def dynamic_reservation(source, table):
    slots = table['constants'].get('dynamic_hook_slots', 0)
    if slots:
        source += f'.section .stub, "ax"\n.balign 8\ndynamic_gateways:\n    .space {64 * slots}\n'
    return source
