"""Owned native hooks on the host, and gateway execution on an ARM64 emulator.

Neither result qualifies these hooks in the game.
"""
import copy
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import unittest
import zipfile

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
from gen import SchemaError, game_table, stubs  # noqa: E402
import build_module  # noqa: E402
import build_patch  # noqa: E402
import generate  # noqa: E402
import native_hook_permissions as permissions  # noqa: E402
import package_sdk  # noqa: E402

try:
    import unicorn
    from unicorn import arm64_const as arm
except ImportError:
    unicorn = None


def run(command, **kwargs):
    result = subprocess.run(list(map(str, command)), capture_output=True, text=True, **kwargs)
    if result.returncode:
        raise AssertionError(result.stdout + result.stderr)
    return result


def metadata(allowed=permissions.REQUIRED, signatures=0x200):
    groups = {}
    for call in allowed:
        groups[call // 24] = groups.get(call // 24, 0) | (1 << (call % 24))
    caps = b''.join(struct.pack('<I', group << 29 | mask << 5 | 15) for group, mask in sorted(groups.items()))
    aci = bytearray(0x40 + len(caps))
    acid = bytearray(signatures + 0x40 + len(caps))
    aci[:4] = b'ACI0'
    struct.pack_into('<Q', aci, 0x10, permissions.PROGRAM)
    struct.pack_into('<II', aci, 0x30, 0x40, len(caps))
    aci[0x40:] = caps
    acid[signatures:signatures + 4] = b'ACID'
    struct.pack_into('<I', acid, signatures + 4, len(acid) - signatures // 2)
    struct.pack_into('<QQ', acid, signatures + 0x10, permissions.PROGRAM, permissions.PROGRAM)
    struct.pack_into('<II', acid, signatures + 0x30, signatures + 0x40, len(caps))
    acid[signatures + 0x40:] = caps
    header = bytearray(0x80)
    header[:4], header[0x0c] = b'META', 1
    struct.pack_into('<IIII', header, 0x70, 0x80, len(aci), 0x80 + len(aci), len(acid))
    return header + aci + acid


class NativePermissions(unittest.TestCase):
    def test_permissions_are_checked_in_both_sections(self):
        raw = metadata()
        self.assertEqual(permissions.validate(raw)['system_calls'], sorted(permissions.REQUIRED))
        with self.assertRaisesRegex(ValueError, '0x74'):
            permissions.validate(metadata(permissions.REQUIRED - {0x74}))
        # An ACID restriction cannot be bypassed by granting only ACI0.
        acid_at = struct.unpack_from('<I', raw, 0x78)[0]
        at = acid_at + 0x240 + 4 * 3
        struct.pack_into('<I', raw, at, struct.unpack_from('<I', raw, at)[0] & ~(1 << (5 + 0x74 % 24)))
        with self.assertRaisesRegex(ValueError, '0x74'):
            permissions.validate(raw)

    def test_switch_2_metadata_has_longer_acid_signatures(self):
        # Switch 2 programs sign their ACID with 0x300 bytes, so its header and capabilities start 0x100 later.
        self.assertEqual(permissions.validate(metadata(signatures=0x300))['system_calls'], sorted(permissions.REQUIRED))
        with self.assertRaisesRegex(ValueError, '0x43'):
            permissions.validate(metadata(permissions.REQUIRED - {0x43}, signatures=0x300))
        with self.assertRaisesRegex(ValueError, 'identity'):
            permissions.validate(metadata(signatures=0x280))

    def test_malformed_metadata_and_another_program_are_refused(self):
        for raw in (b'', b'META', metadata()[:-1]):
            with self.subTest(size=len(raw)), self.assertRaises(ValueError):
                permissions.validate(raw)
        raw = metadata()
        struct.pack_into('<Q', raw, 0x90, permissions.PROGRAM + 1)
        with self.assertRaisesRegex(ValueError, 'another program'):
            permissions.validate(raw)


@unittest.skipUnless(shutil.which('clang++') and shutil.which('ld.lld'), 'needs LLVM')
class NativeHooks(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.work = ROOT / 'build/native-hooks'
        cls.work.mkdir(parents=True, exist_ok=True)
        cls.gen = cls.work / 'gen'
        generate.write(cls.gen)
        for name, text in game_table.generate(ROOT / 'tests/fixtures/native_hooks.yaml').items():
            path = cls.work / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(text, encoding='utf-8', newline='\n')
        cls.table = game_table.load(ROOT / 'tests/fixtures/native_hooks.yaml')
        cls.suffix = '.exe' if os.name == 'nt' else ''
        cls.dump = cls.work / ('codegen' + cls.suffix)
        run([shutil.which('clang++'), '-std=c++17', '-Wall', '-Wextra', '-Werror',
             '-I', ROOT / 'framework/core', ROOT / 'tests/core/native_codegen_dump.cpp', '-o', cls.dump])

    def test_ownership_chaining_conflicts_and_instruction_checks(self):
        includes = [self.work / 'framework', self.gen / 'include', self.gen / 'framework',
                    ROOT / 'framework/base', ROOT / 'framework/core']
        sources = [ROOT / 'framework/core' / name for name in ('registry.cpp', 'ids.cpp', 'hooks.cpp', 'native_routes.cpp', 'native_hooks.cpp')]
        sources += sorted((self.gen / 'framework').glob('*.cpp'))
        sources += [ROOT / 'tests/core/native_hooks_test.cpp']
        exe = self.work / ('native_hooks' + self.suffix)
        args = ['-std=c++17', '-Wall', '-Wextra', '-Werror', *[v for path in includes for v in ('-I', path)], *sources, '-o', exe]
        response = self.work / 'host.rsp'
        response.write_text('\n'.join('"' + str(value).replace('\\', '/').replace('"', '\\"') + '"' for value in args), encoding='utf-8')
        run([shutil.which('clang++'), '@' + str(response)])
        self.assertIn('PASS', run([exe]).stdout)
        for scenario in ('missing-arena', 'missing-alias', 'missing-flush', 'arena-refused',
                         'zero-rx', 'zero-rw', 'unaligned-rx', 'unaligned-rw', 'wrong-size',
                         'overflow-rx', 'overflow-rw', 'overlap-rx-main', 'overlap-rw-main',
                         'overlap-rx-rw', 'unreachable', 'branch-endpoint', 'hook-capacity',
                         'publication-race', 'late-alias-refused', 'backward-branch', 'call'):
            with self.subTest(scenario=scenario):
                self.assertIn('PASS', run([exe, scenario]).stdout)

    def test_gateway_reservation_fits_and_is_in_the_patch_hash(self):
        actual = game_table.load(ROOT / 'game/table.yaml')
        self.assertLessEqual(stubs.block_end(actual), actual['constants']['stub_end'])
        self.assertEqual(actual['constants']['dynamic_hook_slots'], 7)
        self.assertEqual(len(actual['code_sites']), 58)
        # The combined table recovers three unused legacy slots. With the LoadMap
        # and package-store routes, the lone particle routes and the material
        # setters, restoring the eighth slot exceeds the admitted region by 24
        # bytes without moving the native shared entry, return entry, or start
        # of the padding.
        restored = copy.deepcopy(actual)
        restored['constants']['dynamic_hook_slots'] = 8
        constants = actual['constants']
        available = constants['stub_end'] - constants['stub_base']
        with self.assertRaisesRegex(ValueError,
                                    f'need {available + 24:#x} bytes; the spare space has {available:#x}'):
            stubs.block_end(restored)
        self.assertEqual(stubs.native_layout(restored), stubs.native_layout(actual))
        self.assertEqual(restored['dynamic_hook_base'], actual['dynamic_hook_base'])
        self.assertEqual(restored['dynamic_hook_base'] + 64 * 8 - constants['stub_end'], 24)
        different = copy.deepcopy(actual)
        different['constants']['dynamic_hook_slots'] -= 1
        self.assertNotEqual(stubs.patch_hash(actual), stubs.patch_hash(different))
        too_big = copy.deepcopy(actual)
        too_big['constants']['dynamic_hook_slots'] = 64
        with self.assertRaisesRegex(ValueError, 'spare space'):
            stubs.block_end(too_big)
        records = build_patch.records(actual, self.work / 'production-patch')
        block = next(data for at, data in records if at == actual['constants']['stub_base'])
        start = actual['dynamic_hook_base'] - actual['constants']['stub_base']
        self.assertEqual(block[start:], b'\0' * 448)

    def test_platform_backend_compiles_with_and_without_permission_gate(self):
        for enabled in (False, True):
            args = [shutil.which('clang++'), '--target=aarch64-none-elf', '-std=c++17', '-O2',
                    '-ffreestanding', '-ffixed-x18', '-Wall', '-Wextra', '-Werror']
            if not enabled:
                args += ['-DTSUCHI_NATIVE_CODE_ACCESS=0']
            for path in (ROOT / 'sdk/libc', self.gen / 'include', self.gen / 'framework',
                         ROOT / 'framework/base', ROOT / 'framework/core', ROOT / 'framework/target'):
                args += ['-I', path]
            run([*args, '-c', ROOT / 'framework/target/code_memory.cpp', '-o', self.work / f'code_memory_{enabled}.o'])

    def test_two_hook_example_builds_from_the_extracted_sdk(self):
        report = package_sdk.package(self.work / 'Tsuchi-SDK.zip', prerelease=True)
        extracted = self.work / 'sdk-extracted'
        with zipfile.ZipFile(report['archive']) as archive:
            archive.extractall(extracted)
        sdk = extracted / 'sdk'
        example = sdk / 'examples/native-hooks'
        (example / 'targets.h').write_bytes((example / 'targets.example.h').read_bytes())
        output = self.work / 'example-native-hooks/mod.nro'
        run([sys.executable, sdk / 'build_mod.py', example / 'main.c', '--out', output])
        self.assertGreater(output.stat().st_size, 0)

    def emitted(self, mode, word, at=0x100000, gateway=0x7000000, shared=0x7001000):
        return bytes.fromhex(run([self.dump, mode, hex(word), hex(at), hex(gateway), hex(shared)]).stdout)

    @unittest.skipUnless(unicorn, 'needs Unicorn for ARM64 execution')
    def test_call_gateway_preserves_arguments_and_returns_after_callee(self):
        at, gateway, wrapper, target, stop, stack = 0x100000, 0x7000000, 0x7100000, 0x120000, 0x130000, 0x9000000
        for wrapped in (False, True):
            cpu = unicorn.Uc(unicorn.UC_ARCH_ARM64, unicorn.UC_MODE_ARM)
            for address in (at, gateway, wrapper, target, stop, stack): cpu.mem_map(address, 4096)
            # Save the outer LR, BL the gateway, consume the return, restore LR.
            caller = [0xa9bf7bfd, 0x94000000 | ((gateway - at - 4) // 4 & 0x3ffffff),
                      0x91001000, 0xa8c17bfd, 0xd65f03c0]
            cpu.mem_write(at, struct.pack('<5I', *caller))
            cpu.mem_write(gateway, self.emitted('call', 0, at + 4, gateway, wrapper if wrapped else target))
            # Callee adds x1 to x0; Q0 and callee-saved registers remain intact.
            cpu.mem_write(target, struct.pack('<II', 0x8b010000, 0xd65f03c0))
            # Wrapper calls the original, then adds three, like a scoped after-hook.
            cpu.mem_write(wrapper, struct.pack('<6IQ', 0xa9bf7bfd, 0x580000b0, 0xd63f0200,
                0x91000c00, 0xa8c17bfd, 0xd65f03c0, target))
            for i in range(30): cpu.reg_write(getattr(arm, f'UC_ARM64_REG_X{i}'), 0x1000 + i)
            cpu.reg_write(arm.UC_ARM64_REG_X0, 9); cpu.reg_write(arm.UC_ARM64_REG_X1, 5)
            cpu.reg_write(arm.UC_ARM64_REG_Q0, 0x123456789abcdef)
            cpu.reg_write(arm.UC_ARM64_REG_SP, stack + 0xF00); cpu.reg_write(arm.UC_ARM64_REG_LR, stop)
            cpu.emu_start(at, stop, count=100)
            self.assertEqual(cpu.reg_read(arm.UC_ARM64_REG_PC), stop)
            self.assertEqual(cpu.reg_read(arm.UC_ARM64_REG_X0), 18 + 3 * wrapped)
            self.assertEqual(cpu.reg_read(arm.UC_ARM64_REG_Q0), 0x123456789abcdef)
            self.assertEqual(cpu.reg_read(arm.UC_ARM64_REG_SP), stack + 0xF00)
            self.assertEqual([cpu.reg_read(getattr(arm, f'UC_ARM64_REG_X{i}')) for i in range(19,30)], list(range(0x1013,0x101e)))

    @unittest.skipUnless(unicorn, 'needs Unicorn for ARM64 execution')
    def test_relocated_instructions_match_stock_execution(self):
        at, gateway = 0x100000, 0x7000000
        words = [0xd503201f, 0x91000400, 0xb1000400, 0xa9bf7bfd,
                 0x14000010, 0x17fffff0, 0x94000010, 0x10000207,
                 0x90000007, 0xb0000007, 0x54000200, 0x54000201,
                 0xb4000207, 0xb5000207, 0x36000207, 0x37000207,
                 0xd63f00e0, 0x18000207, 0x58000207, 0x98000207,
                 0x58000210, 0x5800021f, 0x1c000207, 0x5c000207,
                 0x9c000207, 0xd8000200]
        for word in words:
            for flags, x7 in ((0, 0), (0x60000000, 3)):
                with self.subTest(word=hex(word), flags=flags, x7=x7):
                    code = self.emitted('inline', word, at, gateway)
                    results = []
                    for relocated in (False, True):
                        cpu = unicorn.Uc(unicorn.UC_ARCH_ARM64, unicorn.UC_MODE_ARM)
                        cpu.mem_map(at - 0x1000, 0x3000)
                        cpu.mem_map(gateway, 0x1000)
                        cpu.mem_map(0x9000000, 0x4000)
                        cpu.mem_write(at, struct.pack('<I', word))
                        cpu.mem_write(at + 64, bytes(range(16)))
                        cpu.mem_write(gateway, code)
                        for index in range(31):
                            cpu.reg_write(getattr(arm, f'UC_ARM64_REG_X{index}'), 0x12340000 + index)
                        cpu.reg_write(arm.UC_ARM64_REG_X7, at + 64 if word == 0xd63f00e0 else x7)
                        for i in range(32):
                            cpu.reg_write(getattr(arm, f'UC_ARM64_REG_Q{i}'), 0x1234567800000000 + i)
                        cpu.reg_write(arm.UC_ARM64_REG_SP, 0x9003000)
                        cpu.reg_write(arm.UC_ARM64_REG_NZCV, flags)
                        stops = {at + 4, at + 64, at - 64}
                        cpu.hook_add(unicorn.UC_HOOK_CODE, lambda uc, address, size, user: uc.emu_stop() if address in stops else None)
                        cpu.emu_start(gateway + 12 if relocated else at, 0, count=12)
                        literal = (word & 0x3b000000) == 0x18000000
                        results.append(([cpu.reg_read(getattr(arm, f'UC_ARM64_REG_X{i}')) for i in range(31)],
                                        [cpu.reg_read(getattr(arm, f'UC_ARM64_REG_Q{i}')) for i in range(32)],
                                        cpu.reg_read(arm.UC_ARM64_REG_SP), cpu.reg_read(arm.UC_ARM64_REG_NZCV),
                                        cpu.reg_read(arm.UC_ARM64_REG_PC), b'' if literal else bytes(cpu.mem_read(0x9002fe0, 32))))
                    self.assertEqual(results[0], results[1])

    def test_unsupported_relocation_is_refused(self):
        for mode, word in [('function', 0x10000000), ('inline', 0xd63f03c0), ('inline', 0xdc000000)]:
            result = subprocess.run([str(self.dump), mode, hex(word), '0x100000', '0x7000000', '0x7001000'], capture_output=True)
            self.assertEqual(result.returncode, 1)

    @unittest.skipUnless(unicorn, 'needs Unicorn for ARM64 execution')
    def test_function_gateway_calls_wrapper_and_displaced_original(self):
        at, gateway, wrapper, stop = 0x100000, 0x7000000, 0x8000000, 0x8800000
        code = self.emitted('function', 0x91000400, at, gateway, wrapper)
        cpu = unicorn.Uc(unicorn.UC_ARCH_ARM64, unicorn.UC_MODE_ARM)
        for address, size in ((at, 4096), (gateway, 4096), (wrapper, 4096), (stop, 4096), (0x9000000, 0x4000)):
            cpu.mem_map(address, size)
        cpu.mem_write(at, struct.pack('<II', 0x14000000 | ((gateway - at) // 4), 0xd65f03c0))
        cpu.mem_write(gateway, code)
        # Save LR, call the gateway's displaced original, add three, and return.
        callback = struct.pack('<IIIIIIQ', 0xa9bf7bfd, 0x580000b0, 0xd63f0200,
                               0x91000c00, 0xa8c17bfd, 0xd65f03c0, gateway + 16)
        cpu.mem_write(wrapper, callback)
        cpu.reg_write(arm.UC_ARM64_REG_X0, 9)
        cpu.reg_write(arm.UC_ARM64_REG_SP, 0x9003000)
        cpu.reg_write(arm.UC_ARM64_REG_LR, stop)
        cpu.emu_start(at, stop, count=100)
        self.assertEqual(cpu.reg_read(arm.UC_ARM64_REG_X0), 13)
        self.assertEqual(cpu.reg_read(arm.UC_ARM64_REG_SP), 0x9003000)
        self.assertEqual(cpu.reg_read(arm.UC_ARM64_REG_PC), stop)

    @unittest.skipUnless(unicorn, 'needs Unicorn for ARM64 execution')
    def test_real_register_capture_and_absent_framework_fallback(self):
        # Compile the entry from start.S, not a reimplementation of its frame.
        source = (ROOT / 'framework/target/start.S').read_text(encoding='utf-8')
        capture = '.text\n' + source[source.index('.equ NATIVE_FRAME'):]
        assembly = self.work / 'capture.S'
        assembly.write_text(capture + '\n' + (ROOT / 'tests/core/native_frame_handler.S').read_text(encoding='utf-8'),
                            encoding='utf-8', newline='\n')
        obj, elf = self.work / 'capture.o', self.work / 'capture.elf'
        run([shutil.which('clang'), '--target=aarch64-none-elf', '-c', assembly, '-o', obj])
        run([shutil.which('ld.lld'), '-Ttext=0x8000000', '-e', 'tsuchi_native_jump', obj, '-o', elf])
        raw = elf.read_bytes()
        _, sections, symbols = build_module.read_elf(raw)
        records = build_patch.records(self.table, self.work / 'fixture-patch')
        main, status, stack = 0x100000, 0x200000, 0x9000000
        at = main + 0x1000
        # The shared capture accepts a return from the separately mapped arena;
        # the old patch reservation stays unused.
        gateway = 0x5000000
        shared = main + stubs.native_layout(self.table)[0]
        code = self.emitted('inline', 0x91000400, at, gateway, shared)
        for mode in ('present', 'absent', 'wrong-magic', 'short-status', 'stopped', 'no-entry'):
            with self.subTest(mode=mode):
                cpu = unicorn.Uc(unicorn.UC_ARCH_ARM64, unicorn.UC_MODE_ARM)
                for base, size in ((main, 0x4000), (status, 0x1000), (stack, 0x4000),
                                   (gateway, 0x4000), (0x8000000, 0x40000)):
                    cpu.mem_map(base, size)
                for name in ('.text', '.data'):
                    section = sections[name]
                    cpu.mem_write(section['address'], raw[section['offset']:section['offset'] + section['size']])
                for offset, data in records:
                    cpu.mem_write(main + offset, data)
                cpu.mem_write(gateway, code)
                cpu.mem_write(at, struct.pack('<I', 0x14000000 | ((gateway - at) // 4 & 0x3ffffff)))
                if mode != 'absent':
                    cpu.mem_write(main + 0x80, struct.pack('<QQ', stubs.STATUS_MAGIC, status))
                    cpu.mem_write(status, struct.pack('<Q', stubs.STATUS_MAGIC if mode != 'wrong-magic' else 0))
                    cpu.mem_write(status + 12, struct.pack('<I', 256 if mode == 'short-status' else 512))
                    cpu.mem_write(status + 32, struct.pack('<I', 2 if mode == 'stopped' else 1))
                    cpu.mem_write(status + 256, struct.pack('<Q', 0 if mode == 'no-entry' else symbols['tsuchi_native_jump']))
                integers = [0x123456789abcdef0 + i for i in range(31)]
                vectors = [(0xdeadbeef0123456789abcdef00000000 + i) for i in range(32)]
                for i, value in enumerate(integers):
                    cpu.reg_write(getattr(arm, f'UC_ARM64_REG_X{i}'), value)
                for i, value in enumerate(vectors):
                    cpu.reg_write(getattr(arm, f'UC_ARM64_REG_Q{i}'), value)
                cpu.reg_write(arm.UC_ARM64_REG_SP, stack + 0x3000)
                cpu.reg_write(arm.UC_ARM64_REG_NZCV, 0x60000000)
                cpu.reg_write(arm.UC_ARM64_REG_FPCR, 0x800000)
                cpu.reg_write(arm.UC_ARM64_REG_FPSR, 0)
                cpu.hook_add(unicorn.UC_HOOK_CODE, lambda uc, address, size, user: uc.emu_stop() if address == at + 4 else None)
                cpu.emu_start(at, 0, count=500)
                integers[0] += 1  # the original ADD executes once
                active = mode == 'present'
                if active:
                    integers[5] += 7
                    vectors[4] ^= 0xff
                self.assertEqual(cpu.reg_read(arm.UC_ARM64_REG_PC), at + 4)
                self.assertEqual([cpu.reg_read(getattr(arm, f'UC_ARM64_REG_X{i}')) for i in range(31)], integers)
                self.assertEqual([cpu.reg_read(getattr(arm, f'UC_ARM64_REG_Q{i}')) for i in range(32)], vectors)
                self.assertEqual(cpu.reg_read(arm.UC_ARM64_REG_SP), stack + 0x3000)
                self.assertEqual(cpu.reg_read(arm.UC_ARM64_REG_NZCV), 0x40000000 if active else 0x60000000)
                self.assertEqual(cpu.reg_read(arm.UC_ARM64_REG_FPCR), 0xc00000 if active else 0x800000)
                self.assertEqual(cpu.reg_read(arm.UC_ARM64_REG_FPSR), 1 if active else 0)
                self.assertEqual(struct.unpack('<I', cpu.mem_read(symbols['dispatch_calls'], 4))[0], int(active))

    @unittest.skipUnless(unicorn, 'needs Unicorn for ARM64 execution')
    def test_platform_system_call_abi_cache_and_permission_gate(self):
        for enabled in (False, True):
            objects = []
            for source in ('framework/target/code_memory.cpp', 'tests/core/native_platform_fixture.cpp'):
                obj = self.work / f'platform_{enabled}_{Path(source).stem}.o'
                args = [shutil.which('clang++'), '--target=aarch64-none-elf', '-std=c++17', '-O2',
                        '-ffreestanding', '-ffixed-x18', '-fno-builtin', '-fno-exceptions', '-fno-rtti']
                for path in (self.work / 'framework', self.gen / 'include', ROOT / 'sdk/libc',
                             ROOT / 'framework/base', ROOT / 'framework/core', ROOT / 'framework/target'):
                    args += ['-I', path]
                if not enabled:
                    args += ['-DTSUCHI_NATIVE_CODE_ACCESS=0']
                run([*args, '-c', ROOT / source, '-o', obj])
                objects.append(obj)
            elf = self.work / f'platform_{enabled}.elf'
            run([shutil.which('ld.lld'), '-Ttext=0x8000000', '-e', 'native_alias_test', *objects, '-o', elf])
            raw = elf.read_bytes()
            _, sections, symbols = build_module.read_elf(raw)
            self.assertEqual('tsuchi_native_code_access' in symbols, enabled)
            start, span = 0x6000000, 0x400000
            cases = [
                {'name': 'success'},
                # One QueryMemory free block spans both reserved regions.
                {'name': 'excluded-free', 'heap': (start, 0x20000),
                 'alias': (start + 0x20000, 0x20000), 'destination': start + 0x40000},
                {'name': 'first-map-refused', 'fail_maps': 1, 'maps': 2,
                 'destination': start + 0x1000},
                {'name': 'refused-before-exclusions', 'fail_maps': 1, 'maps': 2,
                 'heap': (start + 0x1000, 0x2000), 'alias': (start + 0x3000, 0x1000),
                 'destination': start + 0x4000},
                {'name': 'mapping-refused', 'fail_maps': 16, 'maps': 16, 'destination': None},
                {'name': 'handle-refused', 'destination': None, 'maps': 0},
                {'name': 'empty-reservations', 'heap': (0, 0), 'alias': (0, 0)},
                {'name': 'reserved-to-end', 'heap': (start, span), 'destination': None, 'maps': 0},
                {'name': 'occupied-prefix', 'query': 'occupied-prefix', 'destination': start + 0x4000},
                {'name': 'occupied-all', 'query': 'occupied', 'destination': None, 'maps': 0},
                {'name': 'query-limit', 'aslr': (start, 0x2000000), 'query': 'fragmented',
                 'destination': None, 'maps': 0, 'queries': 4096},
                {'name': 'empty-aslr', 'aslr': (start, 0), 'destination': None, 'maps': 0},
                {'name': 'unaligned-aslr', 'aslr': (start + 1, span), 'destination': None, 'maps': 0},
            ]
            for name in ('query-refused', 'query-zero', 'query-base-after', 'query-overflow',
                         'query-before', 'query-unaligned'):
                cases.append({'name': name, 'query': name, 'destination': None, 'maps': 0})
            for kind in (2, 3, 4, 5, 12, 13):
                cases.append({'name': f'information-refused-{kind}', 'fail_info': kind,
                              'destination': None, 'maps': 0})
            for name in ('aslr', 'heap', 'alias'):
                cases.append({'name': f'{name}-overflow', name: (0xfffffffffffff000, 0x2000),
                              'destination': None, 'maps': 0})
            for case in cases:
                with self.subTest(enabled=enabled, case=case['name']):
                    cpu = unicorn.Uc(unicorn.UC_ARCH_ARM64, unicorn.UC_MODE_ARM)
                    for at, size in ((0x8000000, 0x40000), (0x100000, 0x4000), (0x9000000, 0x4000), (0x8800000, 0x1000)):
                        cpu.mem_map(at, size)
                    for name in ('.text', '.rodata', '.data'):
                        if name in sections:
                            section = sections[name]
                            cpu.mem_write(section['address'], raw[section['offset']:section['offset'] + section['size']])
                    original = bytes(i % 251 for i in range(4096))
                    cpu.mem_write(0x101000, original)
                    calls, maps, queries = [], [], []
                    values = {65001: 0x44}
                    aslr = case.get('aslr', (start, span))
                    heap = case.get('heap', (0xa000000, 0x100000))
                    alias = case.get('alias', (0xb000000, 0x100000))
                    for kind, bounds in ((12, aslr), (4, heap), (2, alias)):
                        values[kind], values[kind + 1] = bounds

                    def system_call(uc, interrupt, user):
                        pc = uc.reg_read(arm.UC_ARM64_REG_PC)
                        instruction = struct.unpack('<I', uc.mem_read(pc - 4, 4))[0]
                        number = instruction >> 5 & 0xffff
                        calls.append(number)
                        x = lambda i: uc.reg_read(getattr(arm, f'UC_ARM64_REG_X{i}'))
                        if number == 0x29:
                            kind = x(1)
                            self.assertEqual((x(0), x(2), x(3)),
                                             (0, 0 if kind == 65001 else 0xffff8001, 0))
                            if kind == case.get('fail_info') or (kind == 65001 and case['name'] == 'handle-refused'):
                                uc.reg_write(arm.UC_ARM64_REG_X0, 0xf601)
                                return
                            self.assertIn(kind, values)
                            uc.reg_write(arm.UC_ARM64_REG_X1, values[kind])
                        elif number == 6:
                            address = x(2)
                            self.assertEqual(address % 4096, 0)
                            self.assertGreaterEqual(address, aslr[0])
                            self.assertLess(address, aslr[0] + aslr[1])
                            queries.append(address)
                            mode = case.get('query', 'free')
                            if mode == 'query-refused':
                                uc.reg_write(arm.UC_ARM64_REG_X0, 0xd401)
                                return
                            base, size, state = aslr[0], aslr[1], 0
                            if mode == 'occupied-prefix':
                                if address < start + 0x4000:
                                    size, state = 0x4000, 3
                                else:
                                    base, size = start + 0x4000, span - 0x4000
                            elif mode == 'occupied':
                                state = 3
                            elif mode == 'fragmented':
                                base, size, state = address, 4096, 3
                            elif mode == 'query-zero':
                                size = 0
                            elif mode == 'query-base-after':
                                base, size = address + 4096, 4096
                            elif mode == 'query-overflow':
                                base, size = address, 0xfffffffffffff000
                            elif mode == 'query-before':
                                base, size = address - 4096, 4096
                            elif mode == 'query-unaligned':
                                size = 4095
                            uc.mem_write(x(0), struct.pack('<QQIIIIII', base, size, state, 0, 0, 0, 0, 0))
                        elif number == 0x74:
                            destination = x(0)
                            self.assertEqual((x(1), x(2), x(3)), (0x44, 0x101000, 4096))
                            self.assertEqual(destination % 4096, 0)
                            self.assertGreaterEqual(destination, aslr[0])
                            self.assertLessEqual(destination + 4096, aslr[0] + aslr[1])
                            for base, size in (heap, alias):
                                self.assertFalse(size and destination < base + size and base < destination + 4096)
                            maps.append(destination)
                            if len(maps) <= case.get('fail_maps', 0):
                                uc.reg_write(arm.UC_ARM64_REG_X0, 0xd401)
                                return
                            uc.mem_map(destination, 4096)
                            uc.mem_write(destination, original)
                        elif number == 0x40 and case['name'] == 'handle-refused':
                            self.assertEqual((x(0), x(1), x(2), x(3)), (0, 0, 0, 0))
                            uc.reg_write(arm.UC_ARM64_REG_X0, 0xd401)
                            return
                        else:
                            self.fail(f'unexpected system call {number:#x}')
                        uc.reg_write(arm.UC_ARM64_REG_X0, 0)

                    cpu.hook_add(unicorn.UC_HOOK_INTR, system_call)

                    def call(name, *arguments):
                        for i, value in enumerate(arguments):
                            cpu.reg_write(getattr(arm, f'UC_ARM64_REG_X{i}'), value)
                        cpu.reg_write(arm.UC_ARM64_REG_SP, 0x9003000)
                        cpu.reg_write(arm.UC_ARM64_REG_LR, 0x8800000)
                        cpu.emu_start(symbols[name], 0x8800000, count=2000000)
                        self.assertEqual(cpu.reg_read(arm.UC_ARM64_REG_PC), 0x8800000)
                        return cpu.reg_read(arm.UC_ARM64_REG_X0)

                    pointer = call('native_alias_test', 0x101004, 4)
                    destination = case.get('destination', start)
                    if enabled and destination is not None:
                        self.assertEqual(pointer, destination + 4)
                        self.assertEqual(bytes(cpu.mem_read(pointer, 4)), original[4:8])
                        before = list(calls)
                        self.assertEqual(call('native_alias_test', 0x101004, 4), pointer)
                        self.assertEqual(call('native_alias_test', 0x101008, 4), destination + 8)
                        call('native_flush_test', pointer, 0x101004, 4)
                        self.assertEqual(calls, before)
                    else:
                        self.assertEqual(pointer, 0)
                    if not enabled:
                        self.assertEqual(calls, [])
                    else:
                        self.assertEqual(len(maps), case.get('maps', 1))
                        self.assertLessEqual(len(maps), 16)
                        self.assertLessEqual(len(queries), 4096)
                        if 'queries' in case:
                            self.assertEqual(len(queries), case['queries'])
                    # Rejected input never asks the kernel to map anything.
                    before = list(calls)
                    for at, size in ((0, 4), (0x104000, 4), (0x101ffc, 8), (0x101000, 0)):
                        self.assertEqual(call('native_alias_test', at, size), 0)
                    self.assertEqual(calls, before)
                    self.assertEqual(bytes(cpu.mem_read(0x101000, 4096)), original)


if __name__ == '__main__':
    unittest.main()
