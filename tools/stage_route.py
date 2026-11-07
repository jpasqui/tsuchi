"""Adds an explicitly selected setup route to an existing staged SDK run.

    python tools/stage_route.py chappy

The default checks and their receipt are left as they were staged. Each route is
outside tests/fixtures/mods, so ordinary staging does not install it.
"""
import argparse
import json
import shutil
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def stage(route, target=ROOT / 'build/test-mods'):
    source = ROOT / 'tests/routes' / route
    if route not in {'chappy', 'captain', 'dayplay', 'motiontrace'} or not source.is_dir():
        raise ValueError('no such setup route')
    if not (target / 'checks.json').is_file():
        raise ValueError('stage the requested SDK checks first')
    identity = json.loads((source / 'mod.json').read_text(encoding='utf-8'))['id']
    destination = target / 'romfs/tsuchi/mods' / identity
    destination.mkdir(parents=True, exist_ok=True)
    for path in source.iterdir():
        if path.is_file():
            shutil.copyfile(path, destination / path.name)
    # A mounted ROMFS file can be immutable; keepers use their public saved data.
    marker = destination / 'finish.txt'
    if marker.exists():
        marker.unlink()
    return destination


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('route', choices=['chappy', 'captain', 'dayplay', 'motiontrace'])
    parser.add_argument('--target', type=Path, default=ROOT / 'build/test-mods')
    args = parser.parse_args()
    print(stage(args.route, args.target))
