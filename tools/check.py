"""Runs every check in the repository. This is the one command to run before a commit.

    python tools/check.py
    python tools/check.py --ci --report build/ci/checks.json
"""
import argparse
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--ci', action='store_true', help='run the strict CI gate; accepts --report PATH')
    args, remaining = parser.parse_known_args(argv)
    if args.ci:
        import ci
        return ci.main(remaining)
    if remaining:
        parser.error('unrecognized arguments: ' + ' '.join(remaining))
    return subprocess.run([sys.executable, '-m', 'unittest', 'discover', '-s', 'tests', '-p', 'test_*.py', '-v'],
                          cwd=ROOT).returncode


if __name__ == '__main__':
    sys.exit(main())
