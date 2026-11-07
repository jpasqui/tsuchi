"""SDK tick observation targets from the same table as the runtime.

The host table uses a real C function address, so stock=0 explicitly denotes
host evidence. A staged game fixture requires and validates the native stock.
"""
from gen import game_table


def targets(path, fixtures=None, table=None):
    table = table or game_table.load(path, fixtures)
    fields = {name: body['fields'] for name, body in table['structs'].items()}
    values = [table['data']['tick_state_actor_table'] + fields['actor_table']['request_tick'],
              table['functions']['actor_request_tick']['address'],
              table['data']['tick_state_actor_table'],
              fields['actor_table']['request_tick'], fields['game_actor']['tick_requested'],
              fields['game_actor']['ai'], fields['game_actor']['movement'],
              fields['game_component']['tick_requested'], fields['component_table']['set_tick_enabled']]
    return table['assets']['tick_state_actor'] + '\n' + ''.join(f'{value:#x}\n' for value in values)
