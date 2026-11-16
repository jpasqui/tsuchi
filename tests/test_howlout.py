"""The howlout service on the host: its dictionary assembler against vectors from the Python reference,
and its runtime against a made-up recognizer."""
import json
import shutil
import subprocess
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
import generate

BUILD = ROOT / 'build'
FLAGS = ['-std=c++17', '-Wall', '-Wextra', '-Werror', '-fno-exceptions', '-fno-rtti']


def array(name, data):
    return f'static const unsigned char {name}[]={{' + ','.join(map(str, data)) + '};\n'


def vectors_header():
    """The made-up dictionary, sound rows and phrases of tests/fixtures/howlout_vectors.json as C arrays."""
    vectors = json.loads((ROOT / 'tests/fixtures/howlout_vectors.json').read_text(encoding='utf-8'))
    BUILD.mkdir(exist_ok=True)
    phrases = ','.join('{' + json.dumps(sounds) + f',{threshold}' + '}' for sounds, threshold in vectors['phrases'])
    (BUILD / 'howlout_vectors.h').write_text(
        '#pragma once\n#include "howlout_dictionary.h"\n'
        + array('stock', bytes.fromhex(vectors['stock'])) + array('rows', bytes.fromhex(vectors['rows_file']))
        + array('expected', bytes.fromhex(vectors['dictionary']))
        + f"static const unsigned long long expected_fingerprint=0x{vectors['fingerprint']}ull;\n"
        + f'static const tsuchi::howlout::Phrase phrases[]={{{phrases}}};\n'
        + f"static const unsigned phrase_count={len(vectors['phrases'])};\n", encoding='utf-8')


def build(test, name, includes, sources):
    binary = BUILD / (name + ('.exe' if sys.platform == 'win32' else ''))
    command = [shutil.which('clang++'), *FLAGS]
    for path in includes:
        command += ['-I', str(path)]
    command += [str(source) for source in sources] + ['-o', str(binary)]
    compiled = subprocess.run(command, capture_output=True, text=True)
    test.assertEqual(compiled.returncode, 0, compiled.stdout + compiled.stderr)
    return binary


@unittest.skipUnless(shutil.which('clang++'), 'needs clang++')
class Howlout(unittest.TestCase):
    def test_assembler_matches_the_reference_vectors(self):
        vectors_header()
        binary = build(self, 'howlout_dictionary_test', [BUILD, ROOT / 'framework/game'],
                       [ROOT / 'framework/game/howlout_dictionary.cpp', ROOT / 'tests/game/howlout_dictionary_test.cpp'])
        ran = subprocess.run([str(binary)], capture_output=True, text=True)
        self.assertEqual(ran.returncode, 0, ran.stdout + ran.stderr)

    def test_runtime_against_a_made_up_recognizer(self):
        vectors_header()
        gen = BUILD / 'gen'
        generate.write(gen)
        binary = build(self, 'howlout_backend_test',
                       [BUILD, gen / 'include', gen / 'framework', ROOT / 'framework/base', ROOT / 'framework/core',
                        ROOT / 'framework/game'],
                       [ROOT / 'framework/core/registry.cpp', gen / 'framework/howlout_service.cpp',
                        ROOT / 'framework/game/howlout_dictionary.cpp', ROOT / 'framework/game/howlout_backend.cpp',
                        ROOT / 'tests/game/howlout_backend_test.cpp'])
        ran = subprocess.run([str(binary)], capture_output=True, text=True)
        self.assertEqual(ran.returncode, 0, ran.stdout + ran.stderr)
        self.assertIn('howlout runtime passed', ran.stdout)
        # The same service in a game that has no recognizer to find.
        ran = subprocess.run([str(binary), 'absent'], capture_output=True, text=True)
        self.assertEqual(ran.returncode, 0, ran.stdout + ran.stderr)
        self.assertIn('howlout runtime passed without Howl-Outs', ran.stdout)


if __name__ == '__main__':
    unittest.main()
