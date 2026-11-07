---
title: Use IDs and kinds
description: Name mods, content, kinds, and placed actors without losing identity.
---

Choose your mod's ID before you [publish it](/develop/publish/), as content and saved
data use that ID too. For example, use `example.gems` for the mod and
`example.gems:ruby` for the treasure it adds.

## Mod IDs

Use 1 to 47 ASCII lowercase letters, digits, `.`, `_`, or `-`. Start and end with a letter
or digit. Put your name at the start, like `olimar.clock`, to avoid another author's
ID. The folder name and the manifest's `id` must match. `tsuchi` and IDs beginning
with `tsuchi.` belong to the framework.

## Content IDs

To name something your mod adds, use `<mod id>:<name>`. The name after the colon
follows the same spelling rules as a mod ID. In `example.gems`' manifest, declare
the treasure under the short name `ruby`. In a call or another mod's declaration,
use `example.gems:ruby`. A full content ID is at most 95 bytes: 47, a colon, and
47. In C, `TSUCHI_ID_SIZE` is 96 to include the terminator.

The game's own IDs have no colon, such as `red`, `OTADARTS`, or `Area001_P`.
Plain names use 1 to 95 ASCII letters, digits, `.`, `_`, or `-`, and match without
regard to letter case. Keep the spelling the game or reference returns.

If the game has a number that Tsuchi hasn't named, you'll get `#` and a decimal
number from 0 through 4294967295, without leading zeros, such as `#8`. An open
vocabulary accepts that placeholder for the same number. A game name outside the
accepted spelling is returned unchanged, but calls refuse it as input.

## Kinds

A *vocabulary* is the set of names a call accepts. Open vocabularies can include
things mods add, such as Pikmin types, treasure IDs, entity kinds, and worlds. They
use text in Lua, JavaScript, and C. A malformed ID fails with `invalid`, and a valid ID
that names nothing fails with `not_found`. Lua returns `nil` and that result name,
JavaScript throws an Error with that name, and C returns the result code.

Closed vocabularies come from the game's code, such as attack kinds and leaf stages.
Lua and JavaScript use names like `"normal"`, and C uses named constants from the
header. The reference lists the values each one accepts. If you pass a value that
isn't listed, a script raises an argument error, and C returns `invalid`. If the game
reports a value Tsuchi has no name for, scripts get a name like `#40` and C gets the
game's number. You can use that name to tell values apart, but you can't pass it back.
Some vocabularies, such as `game.Mode`, are categories Tsuchi sorts the game's values
into, so a value that fits none of them comes back as `other`.

Don't choose a number for new content. Tsuchi assigns the game's numbers when it
registers your mod's content.

## Placements

A *placement number* is the 64-bit number that identifies an actor placed in a map.
It isn't the actor's kind or its handle. Find the current actor with
[`entity.find_placed`](/reference/entity/#find_placed), and check for `not_found`
because the actor might not be loaded or might have been removed.

Keep placement numbers unchanged in Lua, even if they appear negative. See
[64-bit IDs](/develop/lua/#64-bit-ids). In JavaScript, keep every placement number
as a BigInt, including `0n`, and Number arguments are refused for `u64`. See
[JavaScript integers](/develop/javascript/#integers). After the actor leaves play, its handle stops
working. If it returns, find it again by its placement number to get a fresh handle.
