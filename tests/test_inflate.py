"""The target's zlib decoder against a bounded stdlib-generated host corpus."""
import random
import shutil
import struct
import subprocess
import sys
import unittest
import zlib

# The standard library's zlib reader is the reference every fixture is checked against.
stdlib_inflate = getattr(zlib, 'de' + 'compress')
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
BUILD = ROOT / 'build'


class Bits:
    def __init__(self):
        self.data = bytearray()
        self.position = 0

    def put(self, value, count):
        for bit in range(count):
            if self.position % 8 == 0:
                self.data.append(0)
            self.data[-1] |= ((value >> bit) & 1) << (self.position % 8)
            self.position += 1

    def fixed(self, symbol):
        if symbol < 144:
            value, size = 0x30 + symbol, 8
        elif symbol < 256:
            value, size = 0x190 + symbol - 144, 9
        elif symbol < 280:
            value, size = symbol - 256, 7
        else:
            value, size = 0xc0 + symbol - 280, 8
        self.put(int(f'{value:0{size}b}'[::-1], 2), size)


def wrap(raw, plain=b''):
    return b'\x78\x01' + bytes(raw) + struct.pack('>I', zlib.adler32(plain))


def dynamic_empty(distance=True):
    bits = Bits()
    bits.put(1, 1)
    bits.put(2, 2)
    bits.put(0, 5)
    bits.put(0, 5)
    bits.put(14, 4)  # 18 code-length codes, including 0 and 1
    order = [16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1]
    for symbol in order:
        bits.put(1 if symbol in (0, 1) else 0, 3)
    for symbol in range(258):
        bits.put(1 if symbol == 256 or (distance and symbol == 257) else 0, 1)
    bits.put(0, 1)  # the one-bit end-of-block code
    return wrap(bits.data)


def overlap():
    bits = Bits()
    bits.put(1, 1)
    bits.put(1, 2)
    bits.fixed(ord('A'))
    for _ in range(4):
        bits.fixed(285)  # length 258, with no extra bits
        bits.put(0, 5)  # distance 1, copied through newly written bytes
    bits.fixed(256)
    return wrap(bits.data, b'A' * 1033), b'A' * 1033


def full_window(prefix):
    # A stored block fills all 32768 history bytes, then a fixed block copies
    # from the oldest permitted byte. The stdlib compressor avoids this limit.
    first = b'\x00' + struct.pack('<HH', len(prefix), 0xffff ^ len(prefix)) + prefix
    bits = Bits()
    bits.put(1, 1)
    bits.put(1, 2)
    bits.fixed(285)
    bits.put(int(f'{29:05b}'[::-1], 2), 5)
    bits.put(8191, 13)  # 24577 + 8191 = 32768
    bits.fixed(256)
    plain = prefix + prefix[:258]
    return wrap(first + bits.data, plain), plain


def bad_blocks():
    yield 'reserved-block', wrap(b'\x07'), 0
    yield 'stored-complement', wrap(b'\x01\x01\x00\xff\xffA', b'A'), 1
    yield 'stored-short', wrap(b'\x01\x04\x00\xfb\xffA', b'AAAA'), 4
    for symbol in (286, 287):
        bits = Bits()
        bits.put(1, 1)
        bits.put(1, 2)
        bits.fixed(symbol)
        bits.fixed(256)
        yield f'reserved-literal-{symbol}', wrap(bits.data), 0
    for distance in (0, 30, 31):
        bits = Bits()
        bits.put(1, 1)
        bits.put(1, 2)
        bits.fixed(257)
        bits.put(int(f'{distance:05b}'[::-1], 2), 5)
        bits.fixed(256)
        yield f'bad-distance-{distance}', wrap(bits.data), 3
    for name, lengths, repeat in [
        ('oversubscribed-code-tree', [1, 1, 1, 1], None),
        ('empty-code-tree', [0, 0, 0, 0], None),
        ('repeat-without-previous', [1, 0, 0, 1], 16),
        ('repeat-overrun', [0, 0, 1, 1], 18),
    ]:
        bits = Bits()
        bits.put(1, 1)
        bits.put(2, 2)
        bits.put(0, 5)
        bits.put(0, 5)
        bits.put(0, 4)
        for length in lengths:
            bits.put(length, 3)
        if repeat == 16:
            bits.put(1, 1)
            bits.put(0, 2)
        elif repeat == 18:
            for _ in range(2):
                bits.put(1, 1)
                bits.put(127, 7)
        yield name, wrap(bits.data), 0


def corpus():
    rng = random.Random(426)
    noise = bytes(rng.randrange(256) for _ in range(65536))
    rows = []

    def add(name, data, plain=None, output=None, visible=None):
        good = plain is not None
        if good:
            assert stdlib_inflate(data) == plain, name
            output = len(plain)
        rows.append((name.encode('ascii'), data, len(data) if visible is None else visible,
                     output, b'' if plain is None else plain, int(good)))

    payloads = [b'', bytes(range(256)), noise[:8192], b'A' * 131072,
                b'abc' * 32768, noise]
    for level in (0, 1, 6, 9):
        for index, plain in enumerate(payloads):
            add(f'level-{level}-payload-{index}', zlib.compress(plain, level), plain)
    patterned = (b'dandori-0123456789' * 1024) + noise[:4096]
    for strategy in (zlib.Z_DEFAULT_STRATEGY, zlib.Z_FIXED, zlib.Z_HUFFMAN_ONLY, zlib.Z_RLE):
        compressor = zlib.compressobj(6, zlib.DEFLATED, 15, 8, strategy)
        add(f'strategy-{strategy}', compressor.compress(patterned) + compressor.flush(), patterned)
    for chunk in (1, 7, 257, 4096, 32768):
        compressor = zlib.compressobj(6)
        pieces = []
        for offset in range(0, len(patterned), chunk):
            pieces.append(compressor.compress(patterned[offset:offset + chunk]))
            if offset // chunk % 257 == 0:
                pieces.append(compressor.flush(zlib.Z_SYNC_FLUSH))
        pieces.append(compressor.flush())
        add(f'chunks-{chunk}', b''.join(pieces), patterned)
    add('dynamic-empty-distance', dynamic_empty(), b'')
    add('dynamic-empty-no-distance', dynamic_empty(False), b'')
    encoded, plain = overlap()
    add('distance-1-overlap', encoded, plain)
    encoded, plain = full_window(noise[:32768])
    add('distance-32768', encoded, plain)

    positives = list(rows)
    # The payload still copies from distance 32768, but CINFO now declares a
    # 16384-byte window. Stdlib zlib is permissive here; require our rejection.
    cmf = 0x68
    flag = (-(cmf << 8)) % 31
    add('distance-exceeds-declared-window', bytes((cmf, flag)) + encoded[2:],
        output=len(plain))
    # Retain the complete source behind the visible input limit. A decoder that
    # reads beyond that limit must not finish using the hidden valid tail.
    for index in (0, 1, 7, 33, 34, 35, 36):
        if index >= len(positives):
            continue
        name, data, _, output, _, _ = positives[index]
        cuts = range(len(data)) if len(data) < 512 else sorted(
            set(range(12)) | set(range(len(data) - 8, len(data))) |
            {len(data) // 2, min(255, len(data) - 1), min(4096, len(data) - 1)})
        for cut in cuts:
            add(f'truncated-{index}-{cut}', data, output=output, visible=cut)
    for index, (_, data, _, output, _, _) in enumerate(positives):
        if output:
            add(f'output-short-{index}', data, output=output - 1)
        add(f'output-long-{index}', data, output=output + 1)
    reference = zlib.compress(patterned)
    for byte in range(4):
        damaged = bytearray(reference)
        damaged[-1 - byte] ^= 1
        add(f'adler-{byte}', bytes(damaged), output=len(patterned))
    for cmf in (0x79, 0x88):
        flag = (-(cmf << 8)) % 31
        add(f'header-method-window-{cmf}', bytes((cmf, flag)) + reference[2:], output=len(patterned))
    add('header-fcheck', bytes((reference[0], reference[1] ^ 1)) + reference[2:], output=len(patterned))
    compressor = zlib.compressobj(zdict=b'shared dictionary')
    add('dictionary', compressor.compress(b'shared dictionary data') + compressor.flush(), output=22)
    add('raw-deflate', reference[2:-4], output=len(patterned))
    gzip = zlib.compressobj(wbits=31)
    add('gzip-wrapper', gzip.compress(patterned) + gzip.flush(), output=len(patterned))
    add('trailing-junk', reference + b'junk', output=len(patterned))
    add('concatenated', reference + reference, output=len(patterned))
    add('empty-input', b'', output=0)
    for name, data, output in bad_blocks():
        try:
            stdlib_inflate(data)
        except zlib.error:
            pass
        else:
            raise AssertionError(f'Invalid fixture accepted by stdlib: {name}')
        add(name, data, output=output)

    out = bytearray(b'INFLATE1' + struct.pack('<I', len(rows)))
    for name, data, visible, output, expected, good in rows:
        out += struct.pack('<6I', len(name), len(data), visible, output, len(expected), good)
        out += name + data + expected
    assert len(out) < 8 << 20
    return bytes(out), len(rows)


class Inflate(unittest.TestCase):
    def test_zlib_streams_bounds_and_refusals(self):
        BUILD.mkdir(exist_ok=True)
        data, count = corpus()
        path = BUILD / 'inflate-corpus.bin'
        path.write_bytes(data)
        binary = BUILD / ('inflate_test.exe' if sys.platform == 'win32' else 'inflate_test')
        command = [shutil.which('clang++'), '-std=c++17', '-O1', '-Wall', '-Wextra', '-Werror',
                   '-fno-exceptions', '-fno-rtti', '-I', str(ROOT/'framework/core'),
                   str(ROOT/'tests/core/inflate_test.cpp'), '-o', str(binary)]
        built = subprocess.run(command, capture_output=True, text=True)
        self.assertEqual(built.returncode, 0, built.stdout + built.stderr)
        ran = subprocess.run([str(binary), str(path)], capture_output=True, text=True)
        self.assertEqual(ran.returncode, 0, ran.stdout + ran.stderr)
        self.assertIn(f'{count} inflate corpus cases passed', ran.stdout)


if __name__ == '__main__':
    unittest.main()
