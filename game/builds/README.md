# Other builds of the game

Each `<key>.yaml` here holds one build's values for the names in `../table.yaml`. The key is
lower-case letters, digits, `.` and `-`, and it names the build's patch file and what a mod's
manifest can name. The generator refuses a file that adds a name, leaves one out, or changes a
list's length. See DESIGN.md, "Several builds of the game".

```yaml
version: "1.2.0"              # what the log and a mod's manifest call it
build_id: <32 hex digits>
identity:                     # the framework picks this build when all of these match; at least two
  - {offset: 0x1000, hash: 0x...}
  - {offset: 0x...., hash: 0x...}
data: {world: 0x..., old_global: absent}
constants: {...}              # every constant not under `fixed`
structs:
  game_actor: {size: 0x..., fields: {team: 0x...}}   # every field; a struct with neither is left out
functions: {spawn_now: 0x..., inlined_away: absent}
slots: {engine_tick: {table: 0x..., offset: 0x..., stock: 0x...}}
tables: {free_areas: [0x..., 0x...]}                 # the same length as the table's
slot_groups: {actor_register: {offset: 0x..., stock: [0x...]}}
code_sites: {update: {address: 0x..., instruction: 0x...}}
conditional_sites: {name: {address: 0x..., instruction: 0x..., next_instruction: 0x..., register: 1, threshold: 17}}
native_routes: {name: {address: 0x..., instruction: 0x...}}
route_frames: {package_store_entry: {slot: 8, entry: 24}}
names: {platform: Switch, shader_format: GLSL_SWITCH}
patch_space: [[0x..., 12], [0x..., 8]]               # written by tools/patch_space.py find
patch_checked: "<16 hex digits>"                     # written by tools/patch_space.py check --write
```

The patch goes in one block in the spare space after the code when it fits and every site reaches
it. Otherwise it goes in pieces in `patch_space`, a list `tools/patch_space.py find` makes from the
build's program. `tools/patch_space.py check` tests every word the patch will write against that
program, each site's instruction and `status_words`, and with `--write` records that it passed. The
patch builder refuses a build with no such record or one from an earlier layout. See DESIGN.md,
"The patch's space".

`absent` marks an entry the build does not have. It disables each API family in `../families.yaml`
whose source files use that entry. The generator refuses it for an entry a core file uses, for a
field of a structure the framework keeps in a buffer of its own, for a function that returns a
struct by value, for a constant not under `address_constants`, and for a route's frame while the
route stays.
