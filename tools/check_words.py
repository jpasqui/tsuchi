"""Fails when a file that would be committed contains a path from someone's computer.

More patterns can be listed in local/terms.txt, one regular expression per line.

    python tools/check_words.py
"""
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
MACHINE_PATHS = [
    r'\b[A-Za-z]:[\\/](?![\\/])\w',          # C:\folder or C:/folder
    r'(?<![\w.])/(?:Users|home)/\w',          # /Users/name, /home/name
    r'(?<![\w.])/[a-z]/Users/',               # /c/Users/...
    r'\\Users\\',
]
SKIP = ('third_party/', 'tools/check_words.py')  # this file holds the patterns themselves


def pattern(root):
    terms = list(MACHINE_PATHS)
    extra = Path(root) / 'local' / 'terms.txt'
    if extra.exists():
        terms += [line.strip() for line in extra.read_text(encoding='utf-8').splitlines()
                  if line.strip() and not line.startswith('#')]
    return re.compile('|'.join(f'(?:{t})' for t in terms), re.IGNORECASE)


def candidates(root):
    """Tracked files, and new ones not ignored."""
    out = subprocess.run(['git', 'ls-files', '--cached', '--others', '--exclude-standard'],
                         cwd=root, capture_output=True, text=True, check=True).stdout
    return [line for line in out.splitlines() if line and not line.startswith(SKIP)]


def find(root):
    """[(path, line number, line)] for every match, file names included."""
    found = pattern(root)
    hits = []
    for relative in candidates(root):
        if found.search(relative):
            hits.append((relative, 0, relative))
        try:
            text = (Path(root) / relative).read_text(encoding='utf-8')
        except (UnicodeDecodeError, OSError):
            continue
        for number, line in enumerate(text.splitlines(), 1):
            if found.search(line):
                hits.append((relative, number, line.strip()))
    return hits


def main():
    hits = find(ROOT)
    for path, number, line in hits:
        print(f'{path}:{number}: {line[:120]}')
    if hits:
        sys.exit(f'{len(hits)} lines that do not belong in this repository')


if __name__ == '__main__':
    main()
