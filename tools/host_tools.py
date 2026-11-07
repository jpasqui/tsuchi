"""Locate host tools across local and hosted runner installations."""
import os
from pathlib import Path
import shutil
import sys


def find_openssl():
    """Return an existing OpenSSL file, respecting an explicit override.

    Git for Windows distributions put OpenSSL in different subdirectories.
    Probe only paths beside Git or supplied through PATH; never scan a drive.
    An invalid explicit override is an error for the caller, not a reason to
    silently select a different installation.
    """
    override = os.environ.get('TSUCHI_OPENSSL')
    if override:
        candidate = Path(override).expanduser()
        return candidate.resolve() if candidate.is_file() else None
    git = shutil.which('git')
    if sys.platform == 'win32' and git:
        for folder in Path(git).resolve().parents:
            for relative in ('mingw64/bin/openssl.exe', 'usr/bin/openssl.exe', 'bin/openssl.exe'):
                candidate = folder / relative
                if candidate.is_file():
                    return candidate
    executable = shutil.which('openssl')
    candidate = Path(executable) if executable else None
    return candidate.resolve() if candidate is not None and candidate.is_file() else None
