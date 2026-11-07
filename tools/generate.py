"""Generates the C headers, the Lua bindings and the reference pages from schema/.

    python tools/generate.py              writes build/gen
    python tools/generate.py --out DIR
"""
import argparse
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))

from gen import SchemaError, generate_all  # noqa: E402


def write(out):
    files = generate_all(ROOT / 'schema', ROOT / 'game' / 'table.yaml')
    for relative, text in files.items():
        path = out / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        if not path.exists() or path.read_text(encoding='utf-8') != text:
            path.write_text(text, encoding='utf-8', newline='\n')
    return files


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('--out', type=Path, default=ROOT / 'build' / 'gen')
    args = parser.parse_args()
    try:
        files = write(args.out)
    except SchemaError as error:
        sys.exit(f'schema error: {error}')
    print(f'{len(files)} files in {args.out}')


if __name__ == '__main__':
    main()
