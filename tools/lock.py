"""Brings schema/abi.lock up to date with the schema.

    python tools/lock.py            writes the lock
    python tools/lock.py --release  marks it released: from then on it only grows

The lock records where each service is in the table a compiled mod is handed, and where each
function and event is in its service's table. See tools/gen/lock.py.
"""
import argparse
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))

from gen import lock, schema  # noqa: E402


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('--release', action='store_true')
    args = parser.parse_args()
    folder = ROOT / 'schema'
    before = lock.read(folder)
    # Read the schema without the lock's say: it is the lock that is being made.
    if (folder / lock.NAME).exists():
        (folder / lock.NAME).unlink()
    try:
        described = schema.load(folder)
        after = lock.grown(described, before)
        if args.release:
            after['released'] = True
    except Exception:
        if before is not None:
            lock.write(folder, before)
        raise
    lock.write(folder, after)
    members = sum(len(members) for members in after['tables'].values())
    print(f"schema/{lock.NAME}: {len(after['services'])} services, {members} members, "
          f"{'released' if after['released'] else 'not released'}")


if __name__ == '__main__':
    main()
