"""Every inline system call says which registers it loses.

A Switch kernel, on both consoles, returns from a system call with x0 to x17 zeroed except
for the call's results. Emulators leave them alone, so a value the compiler keeps in one of
them across the call survives in Eden and Eglantine and is lost on a console: the 0.3 builds
lost the game table's address in find_game and crashed on every console. Each `svc` in code
that runs in the game therefore binds its arguments as read-and-written and lists every other
register from x0 to x17, and the flags, as clobbered.
"""
import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SOURCES = ('framework', 'runtime', 'sdk', 'tests/fixtures')
SUFFIXES = {'.c', '.cc', '.cpp', '.h', '.hpp'}
STATEMENT = re.compile(r'(?:asm|__asm__)\s+volatile\s*\(\s*"svc[^"]*"')
DECLARATION = re.compile(r'register\s+[\w:]+\s+(\w+)\s+(?:asm|__asm__)\("x(\d+)"\)')


def statements(text):
    """(start, the whole statement) for each inline svc in the text."""
    for match in STATEMENT.finditer(text):
        at, depth = match.end(), 1
        while depth:
            depth += {'(': 1, ')': -1}.get(text[at], 0)
            at += 1
        yield match.start(), text[match.start():at]


def sections(statement):
    """The statement's colon-separated sections after its template."""
    body = statement[statement.index('"', statement.index('"') + 1) + 1:-1]
    parts, depth, current, quoted = [], 0, '', False
    for character in body:
        quoted ^= character == '"'
        if not quoted:
            depth += {'(': 1, ')': -1}.get(character, 0)
            if character == ':' and depth == 0:
                parts.append(current)
                current = ''
                continue
        current += character
    return parts + [current]


class InlineSystemCalls(unittest.TestCase):
    def test_every_svc_loses_x0_to_x17_but_its_results(self):
        seen = 0
        for path in sorted(p for folder in SOURCES for p in (ROOT / folder).rglob('*') if p.suffix in SUFFIXES):
            text = path.read_text(encoding='utf-8', errors='replace')
            for start, statement in statements(text):
                seen += 1
                where = f'{path.relative_to(ROOT).as_posix()}:{text[:start].count(chr(10)) + 1}'
                parts = sections(statement)
                outputs = parts[1] if len(parts) > 1 else ''
                inputs = parts[2].strip() if len(parts) > 2 else ''
                clobbers = set(re.findall(r'"([^"]+)"', parts[3])) if len(parts) > 3 else set()
                self.assertEqual(inputs, '', f'{where}: bind arguments as "+r", since the kernel may zero them')
                bound = set()
                for name in re.findall(r'\((\w+)\)', outputs):
                    declared = [d for d in DECLARATION.finditer(text[:start]) if d.group(1) == name]
                    self.assertTrue(declared, f'{where}: {name} is not a register variable')
                    bound.add(f'x{declared[-1].group(2)}')
                lost = {f'x{n}' for n in range(18)} - bound
                self.assertLessEqual(lost, clobbers, f'{where}: also list {sorted(lost - clobbers)} as clobbered')
                self.assertIn('cc', clobbers, f'{where}: list "cc" as clobbered')
                self.assertIn('memory', clobbers, f'{where}: list "memory" as clobbered')
        self.assertGreater(seen, 20, 'the scan found too few system calls to be looking in the right place')


if __name__ == '__main__':
    unittest.main()
