"""Global audio parameters against an asynchronous native queue stand-in."""
import shutil
import re
import subprocess
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
import generate
from gen import game_table


def startup_owner_violations(source):
    """Check target wiring that the host backend fixture cannot execute."""
    initializer = re.search(r'extern "C" void tsuchi_initialize\(\)\s*\{(.*?)^\}',
                            source, re.MULTILINE | re.DOTALL)
    if not initializer:
        raise AssertionError('target initializer not found')
    body = re.sub(r'/\*.*?\*/|//[^\n]*', '', initializer.group(1), flags=re.DOTALL)
    created = re.search(r'\bself\s*=\s*mod_create\("tsuchi\.self"\)\s*;', body)
    if not created:
        raise AssertionError('framework owner creation not found')
    starts = list(re.finditer(r'\b(\w+_start)\s*\(\s*self\b', body))
    if not starts:
        raise AssertionError('no framework-owned service startup found')
    return [call.group(1) for call in starts if call.start() < created.end()]


class AudioParametersTests(unittest.TestCase):
    def test_owned_services_start_after_framework_context_creation(self):
        source = (ROOT / 'framework/target/module.cpp').read_text(encoding='utf-8')
        self.assertEqual(startup_owner_violations(source), [])
        # Exercise the same owner-order violation independently of any service
        # name: an early consumer retains a null copy even if self is set later.
        previous_order = '''extern "C" void tsuchi_initialize() {
    owner_start(self);
    self = mod_create("tsuchi.self");
    other_start(self, callback);
}
'''
        self.assertEqual(startup_owner_violations(previous_order), ['owner_start'])

    @unittest.skipUnless(shutil.which('clang++'), 'needs clang++')
    def test_asynchronous_parameter_holds_and_cleanup(self):
        gen = ROOT / 'build' / 'gen'
        generate.write(gen)
        fixture = ROOT / 'build' / 'gen-fixture'
        for relative, source in game_table.generate(
                ROOT / 'tests' / 'fixtures' / 'game_table.yaml',
                'tests/fixtures/game_table.yaml').items():
            path = fixture / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            if not path.exists() or path.read_text(encoding='utf-8') != source:
                path.write_text(source, encoding='utf-8', newline='\n')
        binary = ROOT / 'build' / ('audio_parameters_test.exe' if sys.platform == 'win32' else 'audio_parameters_test')
        sources = ['framework/core/registry.cpp', 'framework/game/audio_parameters.cpp',
                   'framework/game/audio_backend.cpp', 'build/gen/framework/audio_service.cpp',
                   'tests/game/audio_parameters_test.cpp']
        command = [shutil.which('clang++'), '-std=c++17', '-Wall', '-Wextra', '-Werror']
        for folder in [gen / 'include', fixture / 'framework', gen / 'framework',
                       ROOT / 'framework/core', ROOT / 'framework/base',
                       ROOT / 'framework/game', ROOT / 'tests/base']:
            command += ['-I', str(folder)]
        command += [str(ROOT / source) for source in sources] + ['-o', str(binary)]
        compiled = subprocess.run(command, capture_output=True, text=True)
        self.assertEqual(compiled.returncode, 0, compiled.stdout + compiled.stderr)
        ran = subprocess.run([str(binary)], capture_output=True, text=True)
        self.assertEqual(ran.returncode, 0, ran.stdout + ran.stderr)


if __name__ == '__main__':
    unittest.main()
