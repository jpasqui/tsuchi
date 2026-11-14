# Mod settings and native lifetime controls

Framework controls for mod development, 2026-11-09.

Settings keep the saved player preference separate from effective values. A mod
may declare settings_overrides for a direct required dependency. Visible settings
accept overrides unless overridable is false; hidden settings reject them.
Conflicting manifest values block the claimants, while equal values can coexist.
The generated form identifies the controlling mods and prevents preference edits
while the value is controlled. Dependency failure and restart release or reapply
controllers through the normal mod lifecycle.

For gameplay changes, settings.hold, update_hold and release_hold provide an
exclusive owned scalar controller. The owner may target itself or a direct
required dependency. Boolean, integer and number values must fit the declared
kind and bounds. Holds never write saved preferences and end on owner stop.
Manifest controllers retain priority, and acquisition or update while manifest
control is active reports busy. Content transactions cannot release or update
older ownership. Lua and JavaScript use their ordinary scalar values, including
safe integer Numbers or BigInts for JavaScript integer settings.

The setting owner may narrow an integer range with set_integer_range. Effective
reads and generated selectors use the narrowed range without rewriting the
saved preference or an override. Direct writes must fit both ranges. Changes
notify listeners and refresh an open form. Owner stop releases the range.
Numeric form rows use one arrow control, with optional positive manifest step;
step changes the menu increment and does not restrict API values to a grid.

pikmin.type_count counts named stock types, including Glow, plus active admitted
added types. It is independent of unlocks, world population and inactive save
allocations. Gameplay policy remains in the consuming mod.

reflection.hold_field, update_field and release_field own a scalar field change.
Restoration only occurs if the field still matches the value owned by the hold;
foreign changes, stale objects and unreadable state are preserved. Holds end on
mod stop. These operations do not assign gameplay meaning to arbitrary fields.

Compiled hook.wrap_call wraps one checked direct BL instruction without changing
other calls to its callee. Hook.original continues the same ordered chain, and an
active invocation remains pinned until it returns. Functions that inspect their
caller's return address require a different approach. All existing mainline ABI
table entries retain their positions; new entries are appended. An admitted
restart:game manifest prevents stopping a running context whose allocation or
pointer lifetime extends to process exit. This is enforced across dependent
restart groups, before any running context stops.

This framework change includes no gameplay packages or game data. Population
limit calculations and the handling of a lowered cap remain mod responsibilities.
