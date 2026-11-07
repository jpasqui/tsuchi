"""The real bank and voice holds against a made-up sound engine."""
import shutil
import struct
import subprocess
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def bank(seed, pcm_bytes=32):
    def chunk(tag, body):
        return tag + struct.pack('<I', len(body)) + body
    def node(kind, number, body):
        return struct.pack('<BII', kind, len(body) + 4, number) + body
    pcm = struct.pack('<HHIIHHHHI', 0xfffe, 1, 22050, 44100, 2, 16, 6, 16, 0x4101)
    riff = b'WAVE' + chunk(b'fmt ', pcm) + chunk(b'JUNK', bytes(4)) + chunk(b'data', bytes(pcm_bytes))
    media = b'RIFF' + struct.pack('<I', len(riff)) + riff
    sound = struct.pack('<IBIIB', 0x10001, 0, seed + 1, len(media), 0x80)
    sound += bytes([1, 0, 0]) + struct.pack('<II', 100, 0) + bytes(3)
    sound += bytes([3, 1]) + bytes(5) + bytes([0x10]) + bytes(9)
    hirc = struct.pack('<I', 3) + node(2, seed + 2, sound)
    hirc += node(3, seed + 3, struct.pack('<HIBBBBI', 0x403, seed + 2, 0, 0, 0, 4, seed))
    hirc += node(4, seed + 4, bytes([1]) + struct.pack('<I', seed + 3))
    return chunk(b'BKHD', struct.pack('<5I', 135, seed, 0, 16, 456) + bytes(8)) + chunk(b'DIDX', struct.pack('<III', seed + 1, 0, len(media))) + chunk(b'DATA', media) + chunk(b'HIRC', hirc)


class ContentSound(unittest.TestCase):
    def test_bank_voice_and_restore_paths(self):
        build = ROOT / 'build'
        build.mkdir(exist_ok=True)
        fixture = bank(1000)
        (build / 'content_sound_fixture.h').write_text(
            '#pragma once\nstatic const unsigned char fixture[]={' + ','.join(map(str, fixture)) + '};\n'
            + 'static const unsigned char large_fixture[]={' + ','.join(map(str, bank(9000,44100))) + '};\n', encoding='utf-8')
        binary = build / ('content_sound_test.exe' if sys.platform == 'win32' else 'content_sound_test')
        dirs = [build, build / 'gen/include', build / 'gen/framework', ROOT / 'framework/base', ROOT / 'framework/core', ROOT / 'framework/game']
        sources = [ROOT / 'framework/core/registry.cpp', ROOT / 'framework/core/ids.cpp', ROOT / 'framework/game/holds.cpp', ROOT / 'framework/game/content_sound.cpp', ROOT / 'tests/game/content_sound_test.cpp']
        compiled = subprocess.run([shutil.which('clang++'), '-std=c++17', '-Wall', '-Wextra', '-Werror', *[a for d in dirs for a in ['-I', str(d)]], *map(str, sources), '-o', str(binary)], capture_output=True, text=True)
        self.assertEqual(compiled.returncode, 0, compiled.stdout + compiled.stderr)
        ran = subprocess.run([str(binary)], capture_output=True, text=True)
        self.assertEqual(ran.returncode, 0, ran.stdout + ran.stderr)


if __name__ == '__main__':
    unittest.main()
