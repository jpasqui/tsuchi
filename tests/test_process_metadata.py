"""Release descriptors are authored, edition-specific, and usable without inputs."""
from pathlib import Path
import struct
import sys
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
from gen import game_table
import native_hook_permissions as permissions
import process_metadata


class ProcessMetadata(unittest.TestCase):
    def test_every_supported_build_has_the_right_identity_and_layout(self):
        seen = set()
        for table in game_table.load_builds(ROOT / 'game/table.yaml'):
            program = table['constants']['program_id']
            raw = process_metadata.generate(program)
            self.assertEqual(process_metadata.generate(program), raw)
            seen.add(program)
            info = permissions.validate(raw)
            self.assertEqual(info['program_id'], f'{program:016X}')
            aci_at, aci_size, acid_at, acid_size = struct.unpack_from('<IIII', raw, 0x70)
            aci, acid = raw[aci_at:aci_at + aci_size], raw[acid_at:acid_at + acid_size]
            switch2 = program >> 56 == 4
            header = 0x300 if switch2 else 0x200
            self.assertEqual(acid[:header], bytes(header))  # No copied signature/key
            self.assertEqual(acid[header:header + 4], b'ACID')
            self.assertEqual(struct.unpack_from('<I', acid, header + 4)[0], acid_size - header // 2)
            self.assertEqual(struct.unpack_from('<QQ', acid, header + 0x10), (program, program))
            self.assertEqual(raw[0x0c:0x10], bytes([0x37 if switch2 else 0x17, 0, 44, 0]))
            self.assertEqual(struct.unpack_from('<III', raw, 0x14), (83886080, 0, 65536))
            for section, base in ((aci, 0), (acid, header)):
                spans = []
                for field in (0x20, 0x28, 0x30):
                    offset, size = struct.unpack_from('<II', section, base + field)
                    self.assertGreaterEqual(offset, base + 0x40)
                    self.assertLessEqual(offset + size, len(section))
                    self.assertFalse(any(offset < end and start < offset + size for start, end in spans))
                    spans.append((offset, offset + size))
                calls = permissions.syscalls(section, base + 0x30, base + 0x40)
                self.assertTrue(permissions.REQUIRED <= calls)
                self.assertNotIn(0x60, calls)  # DebugActiveProcess is not required
                self.assertEqual(0x80 in calls, switch2)
                self.assertEqual(0x5f in calls, not switch2)
                offset, _ = struct.unpack_from('<II', section, base + 0x30)
                thread = struct.unpack_from('<I', section, offset)[0]
                self.assertEqual(thread, 0x050073b7 if switch2 else 0x020073b7)
                offset, size = struct.unpack_from('<II', section, base + 0x28)
                self.assertEqual(section[offset:offset + size], b'\x00*')
        self.assertEqual(seen, set(process_metadata.PROFILES))

    def test_save_data_owners_match_each_edition(self):
        demo, switch, switch2, switch2_demo = 0x0100E0B019974000, 0x0100B7C00933A000, 0x0400B7C00933A000, 0x0400D60027FE0000
        expected = {
            switch: '0100000000000000000000001c000000000000001c00000010000000'
                    '0100000001000000' + struct.pack('<Q', demo).hex(),
            switch2: '0100000000000000000000001c000000000000001c00000020000000'
                     '0300000003010100' + b''.join(struct.pack('<Q', owner) for owner in (switch, demo, switch2_demo)).hex(),
            switch2_demo: '0100000000000000000000001c000000000000001c00000000000000',
        }
        for program, layout in expected.items():
            raw = process_metadata.generate(program)
            aci_at, aci_size = struct.unpack_from('<II', raw, 0x70)
            aci = raw[aci_at:aci_at + aci_size]
            offset, size = struct.unpack_from('<II', aci, 0x20)
            fah = aci[offset:offset + size].hex()
            # Permissions differ from the game's (all bits); the owner lists are the game's.
            self.assertEqual(fah[:8] + '0' * 16 + fah[24:], layout)

    def test_unknown_program_is_not_given_a_generic_descriptor(self):
        with self.assertRaisesRegex(ValueError, 'No authored process metadata'):
            process_metadata.generate(0x0100000000000000)

    def test_each_edition_has_a_separate_emulator_folder(self):
        folders = [process_metadata.profile(program)[0] for program in process_metadata.PROFILES]
        self.assertEqual(len(folders), len(set(folders)))
        self.assertEqual(folders[0], 'emulator')
