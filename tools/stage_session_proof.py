"""Stage the session proof mod for a game run with two emulators, on one computer or on two.

    python tools/stage_session_proof.py host                    build/session-proof-host
    python tools/stage_session_proof.py guest                   build/session-proof-guest
    python tools/stage_session_proof.py guest 203.0.113.10     build/session-proof-guest, joining that host

Each folder holds romfs/tsuchi/mods/proof.session with the script and its role. The host listens on
UDP port 47710 and the guest joins it at the address given, 127.0.0.1 without one. role.txt holds the role
and, for a guest given an address, the address on a second line. tests/game/session_proof.lua says what each
logs.
"""
import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def stage(role, address=None):
    if role not in ('host', 'guest'):
        raise ValueError('the role is host or guest')
    if address is not None and (role != 'guest' or not re.fullmatch(r'[A-Za-z0-9.:-]{1,64}', address)):
        raise ValueError('only a guest takes an address, a host name or IP address')
    out = ROOT / 'build' / ('session-proof-' + role)
    mod = out / 'romfs/tsuchi/mods/proof.session'
    mod.mkdir(parents=True, exist_ok=True)
    (mod / 'mod.json').write_text(json.dumps({'id': 'proof.session', 'name': 'Session proof', 'version': '1.0.0',
                                              'framework': 0}, indent=2) + '\n', encoding='utf-8', newline='\n')
    (mod / 'main.lua').write_bytes((ROOT / 'tests/game/session_proof.lua').read_bytes())
    (mod / 'role.txt').write_text(role + '\n' + (address + '\n' if address else ''), encoding='utf-8', newline='\n')
    return out


if __name__ == '__main__':
    print(stage(sys.argv[1] if len(sys.argv) > 1 else '', sys.argv[2] if len(sys.argv) > 2 else None))
