"""Compiles an object only when something it is made from changed.

An object is kept while three things are as they were when it was made: the command that made it, the compiler, and
every file the compiler read for it (clang lists those in a dependency file beside the object). Anything else
compiles it again. The checks use this so that a run after a small change compiles only what the change touched;
tools/build_module.py uses it only when asked, so a module made for a release is always made whole.
"""
import json
import os
import re
import shutil
import subprocess
from pathlib import Path

_TOKEN = re.compile(r'(?:\\.|[^\s\\])+')
_compilers = {}


def _compiler_identity(program):
    """The compiler's path, size and time: a new compiler makes every object again."""
    path = shutil.which(str(program)) or str(program)
    if path not in _compilers:
        try:
            stat = os.stat(path)
            _compilers[path] = [path, stat.st_size, stat.st_mtime_ns]
        except OSError:
            _compilers[path] = [path, 0, 0]
    return _compilers[path]


def _inputs(depfile, cwd):
    """The files a dependency file names, as paths; None when it cannot be read."""
    try:
        text = depfile.read_text(encoding='utf-8', errors='replace')
    except OSError:
        return None
    text = text.replace('\\\r\n', ' ').replace('\\\n', ' ')
    head, colon, rest = text.partition(': ')
    if not colon:
        return None
    files = []
    for token in _TOKEN.findall(rest):
        name = re.sub(r'\\([ #])', r'\1', token)
        path = Path(name)
        files.append(path if path.is_absolute() else Path(cwd) / path)
    return files


def _output(command):
    return Path(command[command.index('-o') + 1])


def current(command, cwd=None):
    """Whether the object the command makes is there and was made by this command from files that have not changed."""
    command = [str(part) for part in command]
    obj = _output(command)
    stamp, depfile = obj.with_name(obj.name + '.cmd'), obj.with_name(obj.name + '.d')
    expected = json.dumps({'command': command, 'cwd': str(cwd or os.getcwd()), 'compiler': _compiler_identity(command[0])})
    try:
        if stamp.read_text(encoding='utf-8') != expected:
            return False
        made = obj.stat().st_mtime_ns
    except OSError:
        return False
    inputs = _inputs(depfile, cwd or os.getcwd())
    if not inputs:
        return False
    try:
        return all(path.stat().st_mtime_ns <= made for path in inputs)
    except OSError:  # a file it was made from is gone
        return False


def compile(command, cwd=None):
    """Runs the compile command unless its object is current. Returns the completed process, or None when it was kept.

    A failed compile raises subprocess.CalledProcessError with the compiler's output, and leaves no record, so the
    next call compiles again."""
    command = [str(part) for part in command]
    if current(command, cwd):
        return None
    obj = _output(command)
    stamp, depfile = obj.with_name(obj.name + '.cmd'), obj.with_name(obj.name + '.d')
    stamp.unlink(missing_ok=True)  # first: an object half made by a stopped run is never taken for current
    obj.parent.mkdir(parents=True, exist_ok=True)
    result = subprocess.run([*command, '-MMD', '-MF', str(depfile)], capture_output=True, text=True, cwd=cwd)
    if result.returncode:
        raise subprocess.CalledProcessError(result.returncode, command, result.stdout, result.stderr)
    stamp.write_text(json.dumps({'command': command, 'cwd': str(cwd or os.getcwd()), 'compiler': _compiler_identity(command[0])}),
                     encoding='utf-8')
    return result
