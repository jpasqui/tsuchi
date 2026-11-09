"""ARM64 execution of arena allocation and rollback against a stand-in kernel."""
import ctypes
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
import generate  # noqa: E402
from gen import game_table  # noqa: E402
import build_module  # noqa: E402
import yaml  # noqa: E402
try:
    import unicorn
    from unicorn import arm64_const as arm
except ImportError:
    unicorn = None

PAGE, SIZE, MAIN, CODE = 4096, 16384, 0x40000000, 0x62f1000
SOURCE, BUFFER = 0x90001000, 0x90000000
PHASES = 'idle disabled handle allocate backing placement map_code set_rx map_alias verify_rx verify_rw rollback_alias rollback_code ready'.split()


def run(command):
    result = subprocess.run(list(map(str, command)), capture_output=True, text=True)
    if result.returncode:
        raise AssertionError(result.stdout + result.stderr)


class Kernel:
    def __init__(self, test, image, case=None):
        self.test, self.case = test, case or {}
        self.cpu = unicorn.Uc(unicorn.UC_ARCH_ARM64, unicorn.UC_MODE_ARM)
        raw, sections, self.symbols = image
        self.cpu.mem_map(0x8000000, 0x80000)
        self.cpu.mem_map(0x8800000, PAGE)
        self.cpu.mem_map(0x8900000, 0x8000)
        for name in ('.text', '.rodata', '.data'):
            if name in sections:
                row = sections[name]
                self.cpu.mem_write(row['address'], raw[row['offset']:row['offset'] + row['size']])
        self.heap = (ctypes.c_ubyte * 0x6000)()
        self.main = (ctypes.c_ubyte * 0x400000)()
        self.other = (ctypes.c_ubyte * SIZE)()
        self.cpu.mem_map_ptr(BUFFER, 0x6000, unicorn.UC_PROT_ALL, ctypes.addressof(self.heap))
        self.cpu.mem_map_ptr(MAIN, 0x400000, unicorn.UC_PROT_READ | unicorn.UC_PROT_EXEC, ctypes.addressof(self.main))
        self.events, self.queries, self.code_attempts, self.alias_attempts = [], [], [], []
        self.rx = self.rw = 0
        self.rx_permission = 0
        self.main_aliases = []
        self.occupied = list(self.case.get('occupied', []))
        self.aslr = self.case.get('aslr', (0x30000000, 0x30000000))
        self.reserved = [self.case.get('heap', (0x90000000, 0x10000000)),
                         self.case.get('alias', (0x50000000, 0x10000000))]
        self.write_symbol('native_alloc_pointer', self.case.get('allocation', 0x90000100))
        self.write_symbol('native_main_address', self.case.get('main', MAIN))
        self.write_symbol('native_nested_on', self.case.get('nested', 0))
        self.cpu.hook_add(unicorn.UC_HOOK_INTR, self.svc)
        self.cpu.hook_add(unicorn.UC_HOOK_CODE, self.allocator)

    def write_symbol(self, name, value):
        self.cpu.mem_write(self.symbols[name], struct.pack('<Q', value))

    def value(self, name):
        return struct.unpack('<Q', self.cpu.mem_read(self.symbols[name], 8))[0]

    def allocator(self, cpu, address, size, user):
        if address == self.symbols['native_engine_allocate']:
            self.events.append('allocate')
        elif address == self.symbols['native_engine_free']:
            self.test.assertFalse(self.rx or self.rw, 'never free mapped backing')
            self.events.append('free')
        instruction = struct.unpack('<I', cpu.mem_read(address, 4))[0]
        operation = instruction & 0xffffffe0
        if operation in (0xd50b7b20, 0xd50b7520):  # DC CVAU, IC IVAU
            register = instruction & 31
            at = cpu.reg_read(getattr(arm, f'UC_ARM64_REG_X{register}'))
            if at in (SOURCE, self.rx, self.rw):
                value = struct.unpack('<I', cpu.mem_read(at, 4))[0]
                self.events.append(('dc' if operation == 0xd50b7b20 else 'ic', at, value))

    def call(self, name, *args):
        for i, value in enumerate(args):
            self.cpu.reg_write(getattr(arm, f'UC_ARM64_REG_X{i}'), value)
        self.cpu.reg_write(arm.UC_ARM64_REG_SP, 0x8907000)
        self.cpu.reg_write(arm.UC_ARM64_REG_LR, 0x8800000)
        self.cpu.emu_start(self.symbols[name], 0x8800000, count=5000000)
        self.test.assertEqual(self.cpu.reg_read(arm.UC_ARM64_REG_PC), 0x8800000)
        return self.cpu.reg_read(arm.UC_ARM64_REG_X0)

    def admit(self):
        address = 0x8900000
        self.cpu.mem_write(address, b'\xff' * 24)
        result = self.call('native_arena_test', address)
        return result, struct.unpack('<QQQ', self.cpu.mem_read(address, 24))

    def stats(self):
        self.call('native_arena_stats', 0x8900100)
        values = struct.unpack('<IIIIIIQQQ', self.cpu.mem_read(0x8900100, 48))
        keys = 'alias_count alias_limit state phase result rollback rx rw bytes'.split()
        return dict(zip(keys, values))

    def info(self, at):
        case = self.case
        if BUFFER <= at < BUFFER + 0x6000:
            state = case.get('backing_state', 5)
            permission = case.get('backing_permission', 3)
            attribute = case.get('backing_attribute', 0)
            return BUFFER, 0x6000, state, attribute, permission, case.get('backing_ipc', 0), case.get('backing_device', 0), 0
        if self.rx and self.rx <= at < self.rx + SIZE:
            return (self.rx, SIZE, case.get('rx_state', 8), case.get('rx_attribute', 0),
                    case.get('rx_permission', self.rx_permission), 0, 0, 0)
        for base, size in [(self.rw, SIZE)] if self.rw else []:
            if base <= at < base + size:
                return base, size, case.get('rw_state', 15), case.get('rw_attribute', 0), case.get('rw_permission', 3), 0, 0, 0
        occupied = [(MAIN, MAIN + CODE), *self.occupied,
                    *[(address, address + PAGE) for address in self.main_aliases]]
        if self.rx:
            occupied.append((self.rx, self.rx + SIZE))
        if self.rw:
            occupied.append((self.rw, self.rw + SIZE))
        for base, end in occupied:
            if base <= at < end:
                return base, end - base, 3, 0, 5, 0, 0, 0
        if case.get('fragmented'):
            return at, PAGE, 3, 0, 5, 0, 0, 0
        low, high = self.aslr[0], sum(self.aslr)
        for base, end in occupied:
            if end <= at:
                low = max(low, end)
            if base > at:
                high = min(high, base)
        return low, high - low, 0, 0, 0, 0, 0, 0

    def guarded(self, address):
        low, high = address - PAGE, address + SIZE + PAGE
        self.test.assertGreaterEqual(low, self.aslr[0])
        self.test.assertLessEqual(high, sum(self.aslr))
        for base, size in self.reserved:
            self.test.assertFalse(size and low < base + size and base < high)

    def svc(self, cpu, interrupt, user):
        pc = cpu.reg_read(arm.UC_ARM64_REG_PC)
        word = struct.unpack('<I', cpu.mem_read(pc - 4, 4))[0]
        number = word >> 5 & 0xffff
        x = lambda i: cpu.reg_read(getattr(arm, f'UC_ARM64_REG_X{i}'))
        args = tuple(x(i) for i in range(4))
        self.events.append(number)
        result = 0
        if number == 0x29:
            kind = x(1)
            self.test.assertEqual((x(0), x(2), x(3)), (0, 0 if kind == 65001 else 0xffff8001, 0))
            values = {65001: 0x44, 12: self.aslr[0], 13: self.aslr[1],
                      4: self.reserved[0][0], 5: self.reserved[0][1],
                      2: self.reserved[1][0], 3: self.reserved[1][1]}
            self.test.assertIn(kind, values)
            if kind == self.case.get('fail_info'):
                result = 0xfa29
            else:
                cpu.reg_write(arm.UC_ARM64_REG_X1, values[kind])
        elif number == 6:
            at = x(2)
            self.queries.append(at)
            if (self.case.get('query_fail') == 'backing' and BUFFER <= at < BUFFER + 0x6000) or \
                    (self.case.get('query_fail') == 'placement' and at < BUFFER and not self.rx) or \
                    (self.case.get('query_fail') == 'verify_rx' and self.rw and self.rx <= at < self.rx + SIZE):
                result = 0xfa06
            else:
                info = list(self.info(at))
                if self.case.get('malformed') and at < BUFFER and not self.rx:
                    mode = self.case['malformed']
                    if mode == 'zero': info[1] = 0
                    elif mode == 'overflow': info[0], info[1] = at, 0xfffffffffffff000
                    elif mode == 'after': info[0], info[1] = at + PAGE, PAGE
                    elif mode == 'unaligned': info[1] = 1
                cpu.mem_write(x(0), struct.pack('<QQIIIIII', *info))
        elif number == 0x77:
            self.test.assertEqual((x(0), x(2), x(3)), (0x44, SOURCE, SIZE))
            self.guarded(x(1))
            for source, destination in ((MAIN, x(1) + SIZE - 4), (MAIN + CODE - 4, x(1)),
                                        (x(1), MAIN + CODE), (x(1) + SIZE - 4, MAIN)):
                self.test.assertGreaterEqual(destination - source, -(1 << 27))
                self.test.assertLess(destination - source, 1 << 27)
            self.code_attempts.append(x(1))
            if len(self.code_attempts) <= self.case.get('fail_code', 0):
                result = 0xca77
                if self.case.get('race'):
                    self.occupied.append((x(1), x(1) + PAGE))
            else:
                self.rx = x(1)
                cpu.mem_map_ptr(self.rx, SIZE, unicorn.UC_PROT_NONE, ctypes.addressof(self.heap) + PAGE)
                cpu.mem_protect(SOURCE, SIZE, unicorn.UC_PROT_NONE)
        elif number == 0x73:
            self.test.assertEqual(args, (0x44, self.rx, SIZE, 5))
            if self.case.get('fail_set_rx'):
                result = 0xca73
            else:
                self.rx_permission = 5
                cpu.mem_protect(self.rx, SIZE, unicorn.UC_PROT_READ | unicorn.UC_PROT_EXEC)
        elif number == 0x74:
            if x(3) == PAGE:
                self.test.assertEqual(x(1), 0x44)
                self.test.assertGreaterEqual(x(2), MAIN)
                self.test.assertLess(x(2), MAIN + 0x400000)
                self.main_aliases.append(x(0))
                cpu.mem_map_ptr(x(0), PAGE, unicorn.UC_PROT_READ | unicorn.UC_PROT_WRITE,
                                ctypes.addressof(self.main) + x(2) - MAIN)
            else:
                self.test.assertEqual((x(1), x(2), x(3)), (0x44, self.rx, SIZE))
                self.guarded(x(0))
                self.test.assertFalse(x(0) - PAGE < self.rx + SIZE + PAGE and self.rx - PAGE < x(0) + SIZE + PAGE)
                self.alias_attempts.append(x(0))
                if len(self.alias_attempts) <= self.case.get('fail_alias', 0):
                    result = 0xca74
                else:
                    self.rw = x(0)
                    pointer = ctypes.addressof(self.other) if self.case.get('not_mirrored') else ctypes.addressof(self.heap) + PAGE
                    cpu.mem_map_ptr(self.rw, SIZE, unicorn.UC_PROT_READ | unicorn.UC_PROT_WRITE, pointer)
        elif number == 0x75:
            self.test.assertEqual(args, (self.rw, 0x44, self.rx, SIZE))
            if self.case.get('fail_unmap_alias'):
                result = 0xfa75
            else:
                cpu.mem_unmap(self.rw, SIZE)
                self.rw = 0
        elif number == 0x78:
            self.test.assertEqual(args, (0x44, self.rx, SOURCE, SIZE))
            self.test.assertEqual(self.rw, 0, 'unmap writable view before code')
            if self.case.get('fail_unmap_code'):
                result = 0xfa78
            else:
                cpu.mem_unmap(self.rx, SIZE)
                cpu.mem_protect(SOURCE, SIZE, unicorn.UC_PROT_READ | unicorn.UC_PROT_WRITE)
                self.rx = 0
        elif number == 0x40 and self.case.get('fail_info') == 65001:
            self.test.assertEqual(args, (0, 0, 0, 0))
            result = 0xfa40
        else:
            self.test.fail(f'unexpected SVC {number:#x}: {args}')
        cpu.reg_write(arm.UC_ARM64_REG_X0, result)


@unittest.skipUnless(unicorn and shutil.which('clang++') and shutil.which('ld.lld'), 'needs LLVM and Unicorn')
class NativeArena(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.work = ROOT / 'build/native-arena'
        cls.work.mkdir(parents=True, exist_ok=True)
        generate.write(cls.work / 'gen')
        raw = yaml.safe_load((ROOT / 'tests/fixtures/native_hooks.yaml').read_text(encoding='utf-8'))
        raw['constants'].update(code_size=CODE, stub_base=0x62f0890, stub_end=CODE)
        path = cls.work / 'game.yaml'
        path.write_text(yaml.safe_dump(raw), encoding='utf-8')
        for name, text in game_table.generate(path).items():
            output = cls.work / name
            output.parent.mkdir(parents=True, exist_ok=True)
            output.write_text(text, encoding='utf-8', newline='\n')
        cls.images = {}
        for enabled in (False, True):
            objects = []
            for source in ('framework/target/code_memory.cpp', 'tests/core/native_arena_platform_fixture.cpp'):
                output = cls.work / f'{enabled}-{Path(source).stem}.o'
                flags = ['--target=aarch64-none-elf', '-std=c++17', '-O2', '-ffreestanding', '-ffixed-x18',
                         '-fno-builtin', '-fno-exceptions', '-fno-rtti', '-Wall', '-Wextra', '-Werror']
                for folder in (cls.work / 'framework', cls.work / 'gen/include', ROOT / 'sdk/libc',
                               ROOT / 'framework/base', ROOT / 'framework/core', ROOT / 'framework/target'):
                    flags += ['-I', folder]
                if not enabled: flags += ['-DTSUCHI_NATIVE_CODE_ACCESS=0']
                run([shutil.which('clang++'), *flags, '-c', ROOT / source, '-o', output])
                objects.append(output)
            output = cls.work / f'platform-{enabled}.elf'
            run([shutil.which('ld.lld'), '-Ttext=0x8000000', '-e', 'native_arena_test', *objects, '-o', output])
            raw = output.read_bytes()
            _, sections, symbols = build_module.read_elf(raw)
            cls.images[enabled] = raw, sections, symbols

    def machine(self, case=None, enabled=True):
        return Kernel(self, self.images[enabled], case)

    def refused(self, case, phase, result=0, retained=False, rollback=0):
        machine = self.machine(case)
        self.assertEqual(machine.admit(), (0, (0, 0, 0)))
        stats = machine.stats()
        self.assertEqual((stats['state'], stats['phase'], stats['result'], stats['rollback']),
                         (3 if retained else 2, PHASES.index(phase), result, rollback))
        self.assertLessEqual(len(machine.code_attempts) + len(machine.alias_attempts), 16)
        self.assertLessEqual(len(machine.queries), 4096)
        self.assertEqual(machine.value('native_free_count'), int(machine.value('native_alloc_count') > 0 and case.get('allocation', 1) != 0 and not retained))
        if retained:
            before = list(machine.events)
            self.assertEqual(machine.admit(), (0, (0, 0, 0)))
            self.assertEqual(machine.events, before)
        return machine

    def test_admission_uses_owned_aliases_and_reuses_the_arena(self):
        machine = self.machine()
        self.assertEqual(machine.stats()['state'], 0)
        success, arena = machine.admit()
        self.assertEqual((success, arena[2]), (1, SIZE))
        self.assertEqual(arena[:2], (machine.rx, machine.rw))
        self.assertEqual(bytes(machine.cpu.mem_read(machine.rx, SIZE)), b'\0' * SIZE)
        self.assertEqual(machine.value('native_alloc_bytes'), SIZE + PAGE - 1)
        self.assertEqual(machine.value('native_alloc_count'), 1)
        self.assertEqual(machine.value('native_free_count'), 0)
        self.assertIn(('dc', SOURCE, 0), machine.events[:machine.events.index(0x77)])
        written = machine.events.index(('dc', machine.rw, 0xd503201f))
        invalidated = machine.events.index(('ic', machine.rx, 0xd503201f))
        cleared = machine.events.index(('dc', machine.rw, 0), invalidated)
        self.assertLess(written, invalidated)
        self.assertIn(('ic', machine.rx, 0), machine.events[cleared:])
        before = list(machine.events)
        self.assertEqual(machine.admit(), (success, arena))
        self.assertEqual(machine.events, before)
        stats = machine.stats()
        self.assertEqual((stats['state'], stats['phase'], stats['rx'], stats['rw'], stats['bytes']),
                         (1, PHASES.index('ready'), *arena))
        machine.cpu.mem_write(machine.rw + SIZE - 4, b'gate')
        self.assertEqual(bytes(machine.cpu.mem_read(machine.rx + SIZE - 4, 4)), b'gate')

    def test_normal_gate_and_null_requests_make_no_system_calls(self):
        machine = self.machine(enabled=False)
        self.assertEqual(machine.admit(), (0, (0, 0, 0)))
        self.assertEqual(machine.stats()['state'], 4)
        self.assertEqual(machine.stats()['phase'], PHASES.index('disabled'))
        self.assertEqual(machine.call('native_arena_alias', MAIN + PAGE), 0)
        self.assertEqual(machine.events, [])
        machine = self.machine()
        self.assertEqual(machine.call('native_arena_test', 0), 0)
        self.assertEqual(machine.events, [])

    def test_allocator_and_backing_fail_before_mapping(self):
        self.refused({'allocation': 0}, 'allocate')
        self.refused({'allocation': 0xfffffffffffffff0}, 'backing')
        for case in ({'backing_state': 4}, {'backing_permission': 1}, {'backing_attribute': 1},
                     {'backing_ipc': 1}, {'backing_device': 1}):
            with self.subTest(case=case):
                machine = self.refused(case, 'backing')
                self.assertNotIn(0x77, machine.events)
        self.refused({'query_fail': 'backing'}, 'backing', 0xfa06)
        self.refused({'fail_info': 65001}, 'handle', 0xfa40)

    def test_regions_and_query_results_fail_closed(self):
        for kind in (2, 3, 4, 5, 12, 13):
            with self.subTest(kind=kind):
                self.refused({'fail_info': kind}, 'placement', 0xfa29)
        for key in ('aslr', 'heap', 'alias'):
            with self.subTest(key=key):
                self.refused({key: (0xfffffffffffff000, 0x2000)}, 'placement')
        for case in ({'aslr': (0x30000000, 0)}, {'aslr': (0x60000000, 0x10000)},
                     {'aslr': (0x3f000000, SIZE)}, {'main': 0xffffffffff000000}):
            with self.subTest(case=case):
                self.refused(case, 'placement')
        for mode in ('zero', 'overflow', 'after', 'unaligned'):
            with self.subTest(mode=mode):
                self.refused({'malformed': mode}, 'placement')
        self.refused({'query_fail': 'placement'}, 'placement', 0xfa06)
        machine = self.refused({'fragmented': True}, 'placement')
        self.assertEqual(len(machine.queries), 4096)

    def test_reservations_guards_branch_edges_and_raced_candidates(self):
        for case in ({'heap': (0x30000000, 0xf000000), 'alias': (0x3f000000, 0x100000)},
                     {'fail_code': 2, 'race': True}, {'fail_alias': 2}):
            with self.subTest(case=case):
                machine = self.machine(case)
                self.assertEqual(machine.admit()[0], 1)
                self.assertLessEqual(len(machine.code_attempts) + len(machine.alias_attempts), 16)
        machine = self.machine({'aslr': (0x47ff0000, 0x20000), 'occupied': [(0x47ff0000, 0x47ffb000)]})
        self.assertEqual(machine.admit()[0], 1)
        self.assertEqual(machine.rx, 0x47ffc000)
        self.assertEqual(machine.rx + SIZE - 4 - MAIN, (1 << 27) - 4)
        self.refused({'aslr': (0x3f000000, SIZE + 2 * PAGE)}, 'map_alias')

    def test_every_mapped_failure_rolls_back_in_order(self):
        machine = self.refused({'fail_code': 16}, 'map_code', 0xca77)
        self.assertEqual(len(machine.code_attempts), 16)
        self.assertNotIn(0x78, machine.events)
        machine = self.refused({'fail_set_rx': True}, 'set_rx', 0xca73)
        self.assertLess(machine.events.index(0x78), machine.events.index('free'))
        machine = self.refused({'fail_alias': 16}, 'map_alias', 0xca74)
        self.assertEqual(len(machine.alias_attempts), 15)
        for case, phase in (({'rx_permission': 3}, 'verify_rx'), ({'rx_state': 3}, 'verify_rx'),
                            ({'rx_attribute': 8}, 'verify_rx'), ({'rw_permission': 5}, 'verify_rw'),
                            ({'rw_state': 5}, 'verify_rw'), ({'rw_attribute': 1}, 'verify_rw'),
                            ({'not_mirrored': True}, 'verify_rw')):
            with self.subTest(case=case):
                machine = self.refused(case, phase)
                self.assertLess(machine.events.index(0x75), machine.events.index(0x78))
                self.assertLess(machine.events.index(0x78), machine.events.index('free'))
        self.refused({'query_fail': 'verify_rx'}, 'verify_rx', 0xfa06)

    def test_failed_rollback_keeps_backing_and_disables_new_attempts(self):
        machine = self.refused({'fail_set_rx': True, 'fail_unmap_code': True},
                               'rollback_code', 0xca73, retained=True, rollback=0xfa78)
        self.assertNotEqual(machine.stats()['rx'], 0)
        machine = self.refused({'rx_permission': 3, 'fail_unmap_alias': True},
                               'rollback_alias', retained=True, rollback=0xfa75)
        self.assertNotIn(0x78, machine.events)
        self.assertNotEqual(machine.stats()['rw'], 0)
        machine = self.refused({'rw_permission': 5, 'fail_unmap_code': True},
                               'rollback_code', retained=True, rollback=0xfa78)
        self.assertEqual(machine.stats()['rw'], 0)

    def test_clean_refusal_can_retry_after_permissions_become_available(self):
        machine = self.refused({'fail_set_rx': True}, 'set_rx', 0xca73)
        machine.case['fail_set_rx'] = False
        self.assertEqual(machine.admit()[0], 1)
        self.assertEqual(machine.value('native_alloc_count'), 2)
        self.assertEqual(machine.value('native_free_count'), 1)
        self.assertEqual(machine.stats()['state'], 1)

    def test_nested_allocator_and_release_callbacks_preserve_the_outer_attempt(self):
        machine = self.machine({'nested': 1})
        self.assertEqual(machine.admit()[0], 1)
        self.assertEqual(machine.value('native_nested_count'), 1)
        self.assertEqual(machine.value('native_nested_ok_count'), 1)
        self.assertEqual(machine.value('native_nested_allocate_result'), 0)
        self.assertEqual(machine.value('native_alloc_count'), 1)
        self.assertEqual(machine.stats()['state'], 1)
        machine = self.refused({'nested': 3, 'fail_set_rx': True}, 'set_rx', 0xca73)
        self.assertEqual(machine.value('native_nested_count'), 2)
        self.assertEqual(machine.value('native_nested_ok_count'), 2)
        self.assertEqual(machine.value('native_nested_free_result'), 0)
        self.assertEqual(machine.value('native_alloc_count'), 1)
        self.assertEqual(machine.value('native_free_count'), 1)
        # The scope guard has unwound after refusal, so an outer retry can work.
        machine.case['fail_set_rx'] = False
        self.assertEqual(machine.admit()[0], 1)
        self.assertEqual(machine.value('native_alloc_count'), 2)
        self.assertEqual(machine.value('native_nested_count'), 3)
        self.assertEqual(machine.value('native_nested_ok_count'), 3)
        self.assertEqual(machine.stats()['state'], 1)

    def test_main_alias_budget_and_arena_guards_remain_separate(self):
        machine = self.machine()
        self.assertEqual(machine.admit()[0], 1)
        for i in range(512):
            address = machine.call('native_arena_alias', MAIN + i * PAGE + 4)
            self.assertNotEqual(address, 0)
            for arena in (machine.rx, machine.rw):
                self.assertFalse(arena - PAGE <= address - 4 < arena + SIZE + PAGE)
        self.assertEqual(machine.stats()['alias_count'], 512)
        self.assertEqual(machine.stats()['alias_limit'], 512)
        before = list(machine.events)
        self.assertEqual(machine.call('native_arena_alias', MAIN + 512 * PAGE + 4), 0)
        self.assertNotEqual(machine.call('native_arena_alias', MAIN + 4), 0)
        self.assertEqual(machine.events, before)
        self.assertEqual(machine.value('native_alloc_count'), 1)


if __name__ == '__main__':
    unittest.main()
