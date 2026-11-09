"""Finish a Tsuchi installation using main.npdm from your own game dump."""
import argparse
import os
from pathlib import Path
import tempfile

import native_hook_permissions as permissions


def install(source, exefs):
    source, exefs = Path(source), Path(exefs)
    if source.is_symlink() or not source.is_file() or source.stat().st_size > permissions.MAX_BYTES:
        raise ValueError('Choose a regular main.npdm from your own game dump')
    if exefs.is_symlink() or not exefs.is_dir():
        raise ValueError('Choose the installed Tsuchi exefs directory')
    if not any((exefs / ('subsdk' + str(slot))).is_file() for slot in range(10)):
        raise ValueError('Install the Tsuchi module before preparing its process metadata')
    original = source.read_bytes()
    prepared = permissions.grant(original)
    destination = exefs / 'main.npdm'
    if destination.is_symlink() or source.resolve() == destination.resolve():
        raise ValueError('Keep the game dump separate from the installed process metadata')
    if destination.exists():
        if not destination.is_file() or destination.read_bytes() != prepared:
            raise ValueError('An existing main.npdm differs; preserve it before installing Tsuchi metadata')
        return permissions.validate(prepared)
    # Publish a complete file without replacing a competing installer's destination.
    fd, name = tempfile.mkstemp(prefix='.tsuchi-npdm-', dir=exefs)
    pending = Path(name)
    try:
        with os.fdopen(fd, 'wb') as output:
            output.write(prepared)
            output.flush()
            os.fsync(output.fileno())
        try:
            os.link(pending, destination)
        except FileExistsError:
            raise ValueError('Another installer wrote main.npdm; preserve it and retry') from None
        except OSError:
            # FAT SD cards do not support hard links. Exclusive creation still preserves
            # a foreign file; remove only this attempt's incomplete file on write failure.
            with destination.open('xb') as output:
                try:
                    output.write(prepared)
                    output.flush()
                    os.fsync(output.fileno())
                except BaseException:
                    output.close()
                    destination.unlink()
                    raise
    finally:
        pending.unlink(missing_ok=True)
    return permissions.validate(prepared)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--npdm', type=Path, required=True, help='main.npdm from your own matching game dump')
    parser.add_argument('--exefs', type=Path, required=True, help='installed Tsuchi exefs folder')
    args = parser.parse_args()
    try:
        info = install(args.npdm, args.exefs)
    except (OSError, ValueError) as error:
        parser.exit(1, str(error) + '\n')
    print('Tsuchi code access installed for ' + info['program_id'] + '. Restart the game.')


if __name__ == '__main__':
    main()
