"""Build and reuse the vendored QuickJS for host checks, on both C libraries."""
from pathlib import Path
import shutil
import subprocess
import sys
ROOT = Path(__file__).resolve().parents[1]
QUICKJS = ROOT / 'third_party/quickjs-2025-09-13'
PARTS = ('quickjs', 'cutils', 'libregexp', 'libunicode', 'dtoa')


def objects(own=False):
    libc = ROOT / 'runtime/lua/libc'
    out = ROOT / 'build/obj' / ('quickjs-own' if own else 'quickjs-host')
    out.mkdir(parents=True, exist_ok=True)
    config = ['-std=c99', '-O1', '-w', '-DTSUCHI_QUICKJS', '-Dalloca=__builtin_alloca', '-DCONFIG_VERSION="2025-09-13"']
    if own:
        config += ['-ffreestanding', '-nostdlibinc', '-U_WIN32', '-I', libc / 'include', '-include', libc / 'host_names.h']
    elif sys.platform == 'win32': config += ['-I', ROOT / 'runtime/javascript/host']
    sources = [QUICKJS / (p + '.c') for p in PARTS] + ([libc / 'libc.c'] if own else [])
    newest = max(p.stat().st_mtime for p in (*QUICKJS.glob('*.h'), *libc.rglob('*.h')))
    result = []
    for source in sources:
        target = out / (source.stem + '.o')
        if not target.exists() or target.stat().st_mtime < max(source.stat().st_mtime, newest):
            ran = subprocess.run([str(c) for c in [shutil.which('clang'), *config, '-c', source, '-o', target]], capture_output=True, text=True)
            if ran.returncode: raise AssertionError(ran.stdout + ran.stderr)
        result.append(target)
    return result