"""Builds the AI ordering fixture's files from the game table.

The P4AI file is authored here. No game asset or captured serializer bytes are used.
The host version changes the target and layout, rather than compiling game offsets
into the observer.
"""
import json
import struct

OBSERVER = 'check.ai-order'
VALUES = 'helped.ai-order'
TERRITORY = (11.25, -22.5, 33.75, 87.5, 731.25)


def files(table):
    start = table['slot_groups']['ai_part_start']
    layout = table['structs']['ai_start_observation']['fields']
    placement = table['constants']['ai_order_placement']
    offsets = (table['tables'][start['tables']][2] + start['offset'], start['stock'][2],
               table['structs']['scene_component']['fields']['owner'],
               layout['game_owner'], table['structs']['game_actor']['fields']['ai'],
               table['structs']['game_actor']['fields']['placement'],
               layout['territory'], layout['patrol'], layout['actions'], placement,
               layout['tree_context'], layout['patrol_reference'])
    targets = ''.join(f'{value:#x}\n' for value in offsets).encode('utf-8')
    # P4AI v1: map string, one placement, kind 3 (BabyAI), two replacement groups.
    def text(value):
        raw = value.encode('utf-8') + b'\0'
        return struct.pack('<H', len(raw)) + raw
    map_path = table['assets']['ai_order_map']
    body = text(map_path) + struct.pack('<Q', placement) + text(table['assets']['ai_order_class'])
    body += bytes((3, 2, 0)) + struct.pack('<I5f', 20, *TERRITORY)
    body += bytes((22,)) + struct.pack('<II', 4, 0)
    ai_file = struct.pack('<4sIII', b'P4AI', 1, 16 + len(body), 1) + body
    manifest = {'id': VALUES, 'name': 'AI values for the ordering check', 'version': '1.0.0',
                'requires': {OBSERVER: '*'}, 'maps': [{'map': map_path, 'ai': 'maps/cave.ai'}]}
    class_name = table['assets']['ai_order_class'].rsplit('.', 1)[-1]
    return {OBSERVER: {'targets.txt': targets, 'class.txt': class_name.encode('utf-8')},
            VALUES: {'mod.json': (json.dumps(manifest, indent=2) + '\n').encode('utf-8'),
                     'maps/cave.ai': ai_file}}
