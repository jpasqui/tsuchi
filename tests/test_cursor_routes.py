"""Cursor inline branch replay against ARM stand-ins; not native game execution."""
from pathlib import Path
import shutil
import subprocess
import sys
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
from gen import game_table, stubs
import build_module
try:
    from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM, UC_HOOK_CODE
    from unicorn import arm64_const as R
except ImportError:
    Uc = None


class CursorRoutes(unittest.TestCase):
    def test_source_bound_sites_and_branch_destinations(self):
        table = game_table.load(ROOT / 'game/table.yaml')
        for name, address, word in [('cursor_unchanged', 0x21C084C, 0x540003A0),
                                    ('cursor_written', 0x21C0864, 0xB40002F4)]:
            site = table['native_routes'][name]
            self.assertEqual((site['address'], site['instruction']), (address, word))
            self.assertEqual(address + 4 * stubs.signed((word >> 5) & 0x7FFFF, 19), 0x21C08C0)
            self.assertFalse(site['has_resume'])

    @unittest.skipUnless(Uc and shutil.which('clang') and shutil.which('ld.lld'), 'Unicorn/ARM tools unavailable')
    def test_both_decisions_preserve_every_register_fp_flags_and_stack(self):
        # Assemble only the generated replay instructions, not a production module or a game image.
        table = game_table.load(ROOT / 'game/table.yaml')
        out = ROOT / 'build/cursor-replay'
        out.mkdir(parents=True, exist_ok=True)
        for name in ['cursor_unchanged', 'cursor_written']:
            site = table['native_routes'][name]
            target, next_ = 0x21C08C0, site['address'] + 4
            source = '.text\n.global entry\nentry:\n' + '\n'.join(stubs.native_replay(site, 'cursor'))
            source += '\n'
            (out / 'replay.S').write_text(source, encoding='utf-8')
            (out / 'replay.ld').write_text('SECTIONS { . = 0x40000; .text : { *(.text) } }\n' +
                                          f'cursor_target = {target:#x}; cursor_next = {next_:#x};', encoding='utf-8')
            for command in [[shutil.which('clang'), '--target=aarch64-none-elf', '-c', out/'replay.S', '-o', out/'replay.o'],
                            [shutil.which('ld.lld'), '-T', out/'replay.ld', '-e', 'entry', '-o', out/'replay.elf', out/'replay.o']]:
                ran = subprocess.run(command, capture_output=True, text=True)
                self.assertEqual(ran.returncode, 0, ran.stdout + ran.stderr)
            raw = (out / 'replay.elf').read_bytes()
            _, sections, _ = build_module.read_elf(raw)
            section = sections['.text']
            code = raw[section['offset']:section['offset'] + section['size']]
            for base in [0, 0x71000000, 0x8000000000]:
                for taken in [False, True]:
                    with self.subTest(site=name, base=base, taken=taken):
                        u = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
                        u.mem_map(base + 0x40000, 0x1000)
                        u.mem_map(base + 0x21C0000, 0x1000)
                        u.mem_map(0x70000000, 0x4000)
                        u.mem_write(base + 0x40000, code)
                        registers = [0xABC0000000000000 + i for i in range(31)]
                        registers[20] = 0 if taken else 0x12345678
                        vectors = [0x123456789ABCDEF00000000000000000 + i for i in range(32)]
                        flags = 0xB0000000 | (int(taken) << 30)
                        for i, value in enumerate(registers): u.reg_write(getattr(R, f'UC_ARM64_REG_X{i}'), value)
                        for i, value in enumerate(vectors): u.reg_write(getattr(R, f'UC_ARM64_REG_Q{i}'), value)
                        u.reg_write(R.UC_ARM64_REG_SP, 0x70002000)
                        u.reg_write(R.UC_ARM64_REG_NZCV, flags)
                        u.reg_write(R.UC_ARM64_REG_FPCR, 0x400000)
                        u.reg_write(R.UC_ARM64_REG_FPSR, 0x11)
                        canary = bytes(range(256)) * 4
                        u.mem_write(0x70001E00, canary)
                        stopped = []
                        def stop(engine, address, size, user):
                            if address in [base + target, base + next_]:
                                stopped.append(address)
                                engine.emu_stop()
                        u.hook_add(UC_HOOK_CODE, stop)
                        u.emu_start(base + 0x40000, 0, count=8)
                        self.assertEqual(stopped, [base + (target if taken else next_)])
                        self.assertEqual([u.reg_read(getattr(R, f'UC_ARM64_REG_X{i}')) for i in range(31)], registers)
                        self.assertEqual([u.reg_read(getattr(R, f'UC_ARM64_REG_Q{i}')) for i in range(32)], vectors)
                        self.assertEqual(u.reg_read(R.UC_ARM64_REG_SP), 0x70002000)
                        self.assertEqual(u.reg_read(R.UC_ARM64_REG_NZCV), flags)
                        self.assertEqual((u.reg_read(R.UC_ARM64_REG_FPCR), u.reg_read(R.UC_ARM64_REG_FPSR)), (0x400000, 0x11))
                        self.assertEqual(bytes(u.mem_read(0x70001E00, len(canary))), canary)


if __name__ == '__main__':
    unittest.main()
