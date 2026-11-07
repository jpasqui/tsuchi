"""Edits the restart check's target in its development folder on an emulator's SD card while the game runs.

    python tools/drive_restart_check.py --sd <Eden user folder>/sdmc [--build <key>] [--out drive.json]

Start it before the game. It takes away the development folders of check.restart and check.restart-target, so both
start with what is installed in their mod folders; check.restart has none from then on, since the framework restarts a
mod whose development folder changes. Then it reads the framework's log on the same card. Each time check.restart
writes that it waits for a revision on the SD card, this puts tests/fixtures/restart/<letter>.lua in place as the
target's main.lua in its development folder, which the framework sees and restarts the target for. A file or folder
is made beside the development folders and moved in with one rename, so the framework never sees half of a change.
With the emulator's executable given (--emulator), it records the process that runs it at every step, so the record
shows that one process ran them all. It ends once check.restart's last check has ended, or at --timeout, and takes
the target's development folder away.

See DESIGN.md, "Restarting a mod", and tests/fixtures/mods/README.md.
"""
import argparse
import json
import re
import shutil
import subprocess
import sys
import time
from pathlib import Path

import yaml

ROOT = Path(__file__).resolve().parents[1]
REVISIONS = ROOT / 'tests' / 'fixtures' / 'restart'
TITLE = f"{yaml.safe_load((ROOT / 'game' / 'table.yaml').read_text(encoding='utf-8'))['constants']['program_id']:016X}"
WAITING = re.compile(r'^\[check\.restart\] check (\S+) waiting revision ([b-e]) on the SD card$', re.M)
LAST = re.compile(r'^\[check\.restart\] check recovered (pass|fail)', re.M)


def processes(executable):
    """The running processes of the executable, with their IDs and when they started."""
    if not executable:
        return []
    command = ("Get-CimInstance Win32_Process -Filter \"Name='" + executable.name + "'\" | "
               "Select-Object ProcessId,ExecutablePath,@{n='Created';e={$_.CreationDate.ToString('o')}} | ConvertTo-Json -Compress")
    done = subprocess.run(['powershell', '-NoProfile', '-Command', command], capture_output=True, text=True)
    found = json.loads(done.stdout) if done.stdout.strip() else []
    found = found if isinstance(found, list) else [found]
    return [p for p in found if p.get('ExecutablePath') and Path(p['ExecutablePath']).resolve() == executable.resolve()]


def put_in_place(folder, name, text):
    """Writes `name` in a mod's development folder with one rename: the file, or the whole folder when it is not
    there yet. What is made first lies beside the development folders, where no mod's folder is."""
    staging = folder.parent / ('.' + folder.name + '.new')
    shutil.rmtree(staging, ignore_errors=True)
    staging.mkdir(parents=True)
    (staging / name).write_text(text, encoding='utf-8', newline='\n')
    if folder.is_dir():
        (staging / name).replace(folder / name)
        shutil.rmtree(staging, ignore_errors=True)
    else:
        staging.replace(folder)


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('--sd', type=Path, required=True, help="the emulator's SD card folder")
    parser.add_argument('--emulator', type=Path, help='the executable of the emulator that runs the game')
    parser.add_argument('--out', type=Path, help='where to write the record, as JSON')
    parser.add_argument('--timeout', type=float, default=900)
    parser.add_argument('--build', help="a build's key in game/builds: the title whose log is read is that build's")
    args = parser.parse_args()
    dev = args.sd / 'tsuchi' / 'dev'
    target, controller = dev / 'check.restart-target', dev / 'check.restart'
    title = TITLE
    if args.build:
        own = yaml.safe_load((ROOT / 'game' / 'builds' / f'{args.build}.yaml').read_text(encoding='utf-8'))
        title = f"{own['constants']['program_id']:016X}"
    log = args.sd / 'atmosphere' / 'contents' / title / 'tsuchi.log'
    shutil.rmtree(target, ignore_errors=True)
    shutil.rmtree(controller, ignore_errors=True)
    began = time.time()
    record = {'sd': str(args.sd), 'began': began, 'steps': []}
    done = set()
    try:
        while time.time() - began < args.timeout:
            time.sleep(0.5)
            if not log.is_file() or log.stat().st_mtime < began:
                continue  # the log of an earlier run
            text = log.read_text(encoding='utf-8', errors='replace')
            for match in WAITING.finditer(text):
                letter = match.group(2)
                if letter in done:
                    continue
                source = (REVISIONS / f'{letter}.lua').read_text(encoding='utf-8')
                put_in_place(target, 'main.lua', source)
                done.add(letter)
                step = {'letter': letter, 'check': match.group(1), 'at': time.time(), 'bytes': len(source.encode('utf-8')),
                        'processes': processes(args.emulator)}
                record['steps'].append(step)
                print(json.dumps(step), flush=True)
            if LAST.search(text):
                record['ended'] = LAST.search(text).group(1)
                break
    finally:
        record['finished'] = time.time()
        shutil.rmtree(target, ignore_errors=True)
        if args.out:
            args.out.write_text(json.dumps(record, indent=2) + '\n', encoding='utf-8')
    print('ended:', record.get('ended', 'timeout'))
    return 0 if record.get('ended') == 'pass' else 1


if __name__ == '__main__':
    sys.exit(main())
