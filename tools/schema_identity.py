"""Bounded identity of the exact schema inputs used by a build or archive."""
import hashlib
import json
from pathlib import Path
import re

import yaml

MAX_FILE = 2 * 1024 * 1024
MAX_TOTAL = 8 * 1024 * 1024
MAX_FILES = 128


def capture(root):
    folder = Path(root) / 'schema'
    paths = sorted(folder.glob('*.yaml')) + [folder / 'abi.lock']
    if folder / 'core.yaml' not in paths or len(paths) > MAX_FILES:
        raise ValueError('API schema inputs are missing or exceed their bound')
    raw = {}
    for path in paths:
        if not path.is_file() or path.is_symlink() or path.stat().st_size > MAX_FILE:
            raise ValueError('API schema input is missing, linked, or too large: ' + path.name)
        data = path.read_bytes()
        if len(data) > MAX_FILE:
            raise ValueError('API schema input grew beyond its bound: ' + path.name)
        raw[path] = data
    if sum(map(len, raw.values())) > MAX_TOTAL:
        raise ValueError('API schema inputs exceed their total bound')
    if sorted(folder.glob('*.yaml')) + [folder / 'abi.lock'] != paths or any(
            path.read_bytes() != data for path, data in raw.items()):
        raise ValueError('API schema changed while reading its identity')
    core = yaml.safe_load(raw[folder / 'core.yaml'])
    if not isinstance(core, dict):
        raise ValueError('Core API schema must be a mapping')
    version = core.get('api_version')
    if type(version) is not int or version < 0:
        raise ValueError('API version must be a nonnegative integer')
    return {'api_version': version,
            'schema_inputs': {'schema/' + path.name: hashlib.sha256(data).hexdigest()
                              for path, data in sorted(raw.items())}}


def release_version(root):
    """The release number people see, such as 0.1.0: `release_version` in schema/abi.lock, or None.
    It is separate from `api_version`, which is what mods see."""
    lock = Path(root) / 'schema' / 'abi.lock'
    try:
        value = json.loads(lock.read_text(encoding='utf-8')).get('release_version')
    except (OSError, ValueError, AttributeError):
        return None
    if value is not None and (not isinstance(value, str) or
                              not re.fullmatch(r'(0|[1-9][0-9]*)(\.(0|[1-9][0-9]*)){2}', value)):
        raise ValueError('release_version in schema/abi.lock must look like 0.1.0')
    return value


def archive_path(root, archive, prerelease):
    """Where a package goes. An unreleased ABI lock is packaged only as a prerelease, and a
    prerelease says so in its name."""
    archive = Path(archive)
    if prerelease:
        return archive if 'prerelease' in archive.name else archive.with_name(archive.stem + '-prerelease' + archive.suffix)
    lock = Path(root) / 'schema' / 'abi.lock'
    try:
        released = json.loads(lock.read_text(encoding='utf-8')).get('released') is True
    except (OSError, ValueError, AttributeError):
        released = False
    if not released:
        raise ValueError('schema/abi.lock is not released. Pass --prerelease to package a prerelease.')
    return archive


def require_current(root, identity):
    if capture(root) != identity:
        raise ValueError('API schema changed during module build')
