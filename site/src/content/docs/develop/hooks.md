---
title: Hook game functions
description: Wrap a game entry from compiled code and call the next function.
---

[Build a compiled mod](/develop/compiled-mod/) before adding a hook. Lua and JavaScript don't
expose [`hook`](/reference/hook/). You'll need the game's function signature as well
as its entry, because your wrapper must take the same arguments and return the same
value.

For runtime function targets, inline register callbacks, instruction checks, and
code-write build requirements, read [Port native hooks](/develop/native-hooks/).

## Entries

Use `hook.wrap_slot` for a function-table entry named in the game table. Use
`hook.wrap_site` for a named function that the game calls directly. A site hook
needs the matching Tsuchi patch installed.

If the game table doesn't name a function-table entry, use `hook.wrap_address` with
the entry's offset from the start of the game's code. Pass the stock function's
offset too if you know it, so Tsuchi can check that the entry still holds the
function you expect. Don't pass the address of the function's instructions as a
table-entry offset.

Find the signature and offsets for the game version you support before writing the
wrapper. A similar name or a matching pointer size doesn't prove the signature.

## Several builds of the game

Tsuchi supports several builds of the game with one module, and `hook.wrap_slot` and
`hook.wrap_site` find their entry by name on each. An offset belongs to one build. A mod that
calls `hook.wrap_address` works only on the builds whose offsets its author wrote, so name those
builds with `game` in its manifest. A hook on an entry a build doesn't have returns `unsupported`.

## Wrappers

For example, this wrapper passes every engine tick through to the next function.
`engine_tick` takes the engine pointer, elapsed seconds, and an idle flag:

```c title="main.c"
#include "tsuchi/tsuchi.h"

typedef void (*EngineTick)(void *engine, float seconds, bool idle);
static TSUCHI_Hook tick;

static void on_tick(void *engine, float seconds, bool idle) {
    EngineTick next = (EngineTick)tick.original;
    next(engine, seconds, idle);
}

TSUCHI_Result tsuchi_mod_start(const TSUCHI_Host *host, TSUCHI_Mod *mod) {
    tick = (TSUCHI_Hook){0};
    return host->hook->wrap_slot(mod, "engine_tick", (void *)on_tick, &tick);
}
```

If the entry isn't available, startup returns the refusal and Tsuchi blocks the
mod. Use [time events](/develop/time/) if you only need to act once per step, as you
won't need to know a native signature.

## Chaining

When several mods hook one entry, the newest hook runs first. Its `original` points
to the next hook, or to the game's function if no other hook follows. Call that
pointer to continue the chain. Calling a saved game address directly would skip
other mods' wrappers.

Remove your hook with `hook.remove(mod, tick.id)` when you don't need it. Tsuchi
also removes it when your mod stops. Calls already running finish through their
existing chain. Don't keep using `original` after those calls return.

## Threads

Your wrapper runs on the thread that called the game's function. On a thread other
than the game thread, Tsuchi calls return `invalid`. Use only the wrapper's
arguments and its `original` there. If you need to call the API, pass the work to a
step callback with synchronization appropriate for the threads involved.
