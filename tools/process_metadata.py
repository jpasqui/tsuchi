"""Generate Tsuchi's launch descriptor from authored policy, without a game dump.

Binary layout: Atmosphere's libraries/libstratosphere/include/stratosphere/ldr/ldr_types.hpp
and switchbrew/switch-tools src/npdmtool.c. No signatures, keys or opaque game
metadata are inputs. Replacement descriptors require an ExeFS replacement loader.
"""
import struct

from native_hook_permissions import REQUIRED, validate

# The two Switch updates share launch parameters. Switch 2 uses six cores and
# RSA-3072-sized ACID fields; its demo has a separate program identity.
PROFILES = {
    0x0100B7C00933A000: ('emulator', 0x200, 0x17, 2),
    0x0400B7C00933A000: ('emulator-switch2', 0x300, 0x37, 5),
    0x0400D60027FE0000: ('emulator-switch2-demo', 0x300, 0x37, 5),
}


def profile(program):
    try:
        return PROFILES[program]
    except KeyError:
        raise ValueError(f'No authored process metadata for program {program:016X}') from None


def generate(program):
    """A complete, deterministic NPDM for one supported program, not a patch."""
    _, header, flags, highest_core = profile(program)
    # Game calls plus the native hook backend. Do not grant all kernel calls.
    calls = set(range(1, 0x2a)) | {0x2c, 0x2d, *range(0x32, 0x37)} | REQUIRED
    calls |= {0x2f, 0x5f} if header == 0x200 else {0x80}
    words = [7 | (59 << 4) | (28 << 10) | (highest_core << 24)]
    for group in sorted({call // 24 for call in calls}):
        mask = sum(1 << (call % 24) for call in calls if call // 24 == group)
        words.append(15 | (mask << 5) | (group << 29))
    words += [0x1fff | (1 << 14),       # Application
              0x3fff | (0x91 << 15),   # Kernel 9.1
              0x7fff | (512 << 16),    # Handle table
              0xffff | (1 << 17)]      # Allow debugging, never force it
    caps = struct.pack('<' + 'I' * len(words), *words)

    # Like a plugin loader, Tsuchi permits filesystem and service clients for
    # the game and its mods. It grants no service-host wildcard. Both sections
    # agree; the descriptor is restricted to this exact program ID.
    fs_permissions = 0xffffffffffffffff
    fac = struct.pack('<BBBBQQQQQ', 1, 0, 0, 0, fs_permissions, 0, 0, 0, 0)
    fah = struct.pack('<IQIIII', 1, fs_permissions, 0x1c, 0, 0x1c, 0)
    services = b'\x00*'

    def section(is_acid):
        base = header if is_acid else 0
        out = bytearray(base + 0x40)
        out[base:base + 4] = b'ACID' if is_acid else b'ACI0'
        if is_acid:
            struct.pack_into('<IQQ', out, base + 0x0c, 3, program, program)
        else:
            struct.pack_into('<Q', out, 0x10, program)
        for field, data in ((0x20, fac if is_acid else fah), (0x28, services), (0x30, caps)):
            out.extend(bytes(-len(out) % 16))
            struct.pack_into('<II', out, base + field, len(out), len(data))
            out.extend(data)
        if is_acid:
            # Signed size excludes only the signature, not the public-key slot.
            struct.pack_into('<I', out, base + 4, len(out) - header // 2)
        return out

    acid, aci = section(True), section(False)
    out = bytearray(0x80)
    out[:4] = b'META'
    struct.pack_into('<BBBB', out, 0x0c, flags, 0, 44, 0)
    struct.pack_into('<III', out, 0x14, 80 * 1024 * 1024, 0, 64 * 1024)
    out[0x20:0x27] = b'Carrot4'
    out.extend(acid)
    out.extend(bytes(-len(out) % 16))
    struct.pack_into('<IIII', out, 0x70, len(out), len(aci), 0x80, len(acid))
    out.extend(aci)
    validate(out)
    return bytes(out)
