"""The script hook gateways and call routine from start.S, executed on an ARM64 emulator."""
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
import build_module  # noqa: E402

try:
    import unicorn
    from unicorn import arm64_const as arm
except ImportError:
    unicorn = None

# A dispatch that records what the gateway stored and calls a game-like function through
# tsuchi_dynamic_call, and that function: it writes through x8, reads an argument from q7's
# upper half, and returns values in x0, x1 and q0 to q3.
HARNESS = r"""
.text
.global tsuchi_dynamic_dispatch
tsuchi_dynamic_dispatch:
    stp x29, x30, [sp, #-32]!
    mov x29, sp
    str x19, [sp, #16]
    mov x19, x1
    adrp x9, seen
    add x9, x9, :lo12:seen
    str w0, [x9]
    ldr x10, [x19, #64]
    str x10, [x9, #8]
    ldr x10, [x19, #88]
    str x10, [x9, #16]
    ldr x10, [x19, #16]
    str x10, [x9, #24]
    adr x0, original
    mov x1, x19
    bl tsuchi_dynamic_call
    ldr x19, [sp, #16]
    ldp x29, x30, [sp], #32
    ret

original:
    add x9, x0, x2
    str x9, [x8]
    str q7, [x8, #16]
    ldr x0, =0x1111111111111111
    ldr x1, =0x2222222222222222
    ldr q0, result0
    ldr q1, result1
    ldr q2, result2
    ldr q3, result3
    mov x8, #0
    ret
.ltorg
.balign 16
result0: .quad 0x0a0a0a0a0a0a0a0a, 0x0b0b0b0b0b0b0b0b
result1: .quad 0x1a1a1a1a1a1a1a1a, 0x1b1b1b1b1b1b1b1b
result2: .quad 0x2a2a2a2a2a2a2a2a, 0x2b2b2b2b2b2b2b2b
result3: .quad 0x3a3a3a3a3a3a3a3a, 0x3b3b3b3b3b3b3b3b

.data
.balign 8
.global seen
seen: .quad 0, 0, 0, 0
"""


@unittest.skipUnless(unicorn and shutil.which('clang') and shutil.which('ld.lld'), 'needs Unicorn, clang and ld.lld')
class DynamicGateways(unittest.TestCase):
    def setUp(self):
        temp = tempfile.TemporaryDirectory()
        self.addCleanup(temp.cleanup)
        work = Path(temp.name)
        source = (ROOT / 'framework/target/start.S').read_text(encoding='utf-8')
        begin = source.index('// The supply of script hook gateways')
        end = source.index("// Where the patch's native routes enter the framework")
        assembly = work / 'gateways.S'
        assembly.write_text('.text\n' + source[begin:end] + HARNESS, encoding='utf-8', newline='\n')
        obj, elf = work / 'gateways.o', work / 'gateways.elf'
        for command in ([shutil.which('clang'), '--target=aarch64-none-elf', '-c', assembly, '-o', obj],
                        [shutil.which('ld.lld'), '-Ttext=0x8000000', '-e', 'tsuchi_dynamic_gateways', obj, '-o', elf]):
            subprocess.run(command, check=True, capture_output=True)
        self.raw = elf.read_bytes()
        _, self.sections, self.symbols = build_module.read_elf(self.raw)

    def test_gateway_keeps_x8_and_every_result_register(self):
        stack, buffer, stop = 0x9000000, 0x9800000, 0xa000000
        cpu = unicorn.Uc(unicorn.UC_ARCH_ARM64, unicorn.UC_MODE_ARM)
        for base, size in ((0x8000000, 0x40000), (stack, 0x4000), (buffer, 0x1000), (stop, 0x1000)):
            cpu.mem_map(base, size)
        for name in ('.text', '.data'):
            section = self.sections[name]
            cpu.mem_write(section['address'], self.raw[section['offset']:section['offset'] + section['size']])
        gateways = self.symbols['tsuchi_dynamic_gateways']
        self.assertEqual(self.symbols['tsuchi_dynamic_gateways_end'] - gateways, 256 * 8)
        cpu.reg_write(arm.UC_ARM64_REG_X0, 7)
        cpu.reg_write(arm.UC_ARM64_REG_X2, 5)
        cpu.reg_write(arm.UC_ARM64_REG_X8, buffer)
        cpu.reg_write(arm.UC_ARM64_REG_X19, 0x1919)
        cpu.reg_write(arm.UC_ARM64_REG_X29, 0x2929)
        cpu.reg_write(arm.UC_ARM64_REG_Q0, 0xfeedface00000000_0000000012345678)
        q7 = 0x7777000077770000_1234567812345678
        cpu.reg_write(arm.UC_ARM64_REG_Q7, q7)
        cpu.reg_write(arm.UC_ARM64_REG_SP, stack + 0x3000)
        cpu.reg_write(arm.UC_ARM64_REG_LR, stop)
        cpu.emu_start(gateways + 5 * 8, stop, count=200)

        self.assertEqual(cpu.reg_read(arm.UC_ARM64_REG_PC), stop)
        self.assertEqual(cpu.reg_read(arm.UC_ARM64_REG_SP), stack + 0x3000)
        self.assertEqual(cpu.reg_read(arm.UC_ARM64_REG_X19), 0x1919)
        self.assertEqual(cpu.reg_read(arm.UC_ARM64_REG_X29), 0x2929)
        # The game-like function wrote its structure where the caller's x8 pointed, with q7 whole.
        self.assertEqual(struct.unpack('<Q', cpu.mem_read(buffer, 8))[0], 12)
        self.assertEqual(int.from_bytes(cpu.mem_read(buffer + 16, 16), 'little'), q7)
        # Its results reach the caller in x0, x1 and q0 to q3, and x8 is the caller's again.
        self.assertEqual(cpu.reg_read(arm.UC_ARM64_REG_X0), 0x1111111111111111)
        self.assertEqual(cpu.reg_read(arm.UC_ARM64_REG_X1), 0x2222222222222222)
        self.assertEqual(cpu.reg_read(arm.UC_ARM64_REG_X8), buffer)
        for i in range(4):
            low = int(f'{i}a' * 8, 16)
            high = int(f'{i}b' * 8, 16)
            self.assertEqual(cpu.reg_read(getattr(arm, f'UC_ARM64_REG_Q{i}')), high << 64 | low)
        # The dispatch got the gateway's number and the frame at start.S's offsets.
        number, x8, q0_high, x2 = struct.unpack('<QQQQ', cpu.mem_read(self.symbols['seen'], 32))
        self.assertEqual((number & 0xffffffff, x8, q0_high, x2), (5, buffer, 0xfeedface00000000, 5))


if __name__ == '__main__':
    unittest.main()
