---
title: Hook game functions
description: Wrap a game entry from scripts or compiled code and call the next function.
---

Lua, JavaScript, and [compiled mods](/develop/compiled-mod/) can use
[`hook`](/reference/hook/). Find the game's function signature and entry before
adding a wrapper, because its arguments and result must match the game's function.

For runtime function targets, inline register callbacks, instruction checks, and
code-write build requirements, read [Port native hooks](/develop/native-hooks/).

## Scripts

Use `hook.wrap(target, fn)` to run a synchronous callback in place of a function.
Your mod supplies `target`: `entry_at` and `stock` for a function-table entry,
`function_at` and `expected` for a function's first instruction, or `slot` for an
entry the game table names, like `engine_tick`. Offsets are relative to the game's
main module. Leave out `stock`, or set it to zero, when you don't know it. A `slot`
names the same entry on every build of the game that the table knows, so it needs
no offsets.
Every target needs a `signature`, such as `"bool(pointer, i32)"`, verified for the
game builds listed in your mod's manifest.

```lua
local id, why = hook.wrap({slot = "engine_tick", signature = "void(pointer, f32, bool)"}, function(original, engine, seconds, idle)
    return original(engine, seconds, idle)
end)
```

Scripts can also check and patch single instructions with `hook.check_code` and
`hook.patch_instruction`, the same calls compiled mods use. They need code access,
and their offsets belong to one build of the game.

For example, a callback can pass the arguments and result through unchanged:

```lua
local id, why = hook.wrap(target, function(original, ...)
    return original(...)
end)
```

```javascript
const id = hook.wrap(target, (original, ...args) => original(...args));
```

`original` works only during that callback. If the callback raises before calling
it, Tsuchi calls it with the game's arguments. If the callback raises after calling
it, Tsuchi returns the last completed original call's result. Service calls inside
the callback fail with `invalid`; keep the values it needs in script variables
updated by event handlers. Lua callbacks cannot yield, and JavaScript callbacks
cannot return promises or thenables. Promise jobs queued during a hook run when
your mod next handles an event.

If you pass a `text16` argument to `original` unchanged, the game gets its own text
back. If you change it, the game gets a copy that lasts only for that call, so use a
`pointer` signature for a function that keeps the address.

Use `hook.watch(target, fn)` to hear calls at the next frame without changing the
game's call. Each callback receives `args`, `result`, `thread`, and `dropped`.
Lua's `args` starts at 1; JavaScript's starts at 0. Pointer arguments are numbers,
not references that keep game objects alive. Text arguments are copied before
the original runs. A watch holds 256 calls between frames, and a mod can have 16 watches. `dropped` counts the
calls your callback didn't hear since the one before, because the watch was full
or your callback couldn't run.

Use `hook.instruction(offset, expected, fn)` for one instruction. Its callback
receives a copy of registers `x[0]` through `x[30]` and boolean flags `n`, `z`,
`c`, and `v`. The copy is committed only if the callback succeeds. Tsuchi then
continues the chain and replays the displaced instruction once. For example,
`function(r) r.z = true end` in Lua or `r => { r.z = true; }` in JavaScript sets
the zero flag. A displaced compare instruction can overwrite that flag again.

Lua carries 64-bit values as integer bit patterns; JavaScript uses `BigInt`.
Wrap text is limited to 256 UTF-16 units. Longer incoming text falls through to
the original. A callback runs only on the known game thread, with a bounded turn
and a shared per-frame allowance; other calls continue to the original.

Remove any script hook with `hook.remove(id)`. Stopping the mod also removes it,
and the runtime stays alive until calls already using its hooks have returned.
Function and instruction targets need code access, which the release's `main.npdm`
gives every installation (see [Port native hooks](/develop/native-hooks/)). Without
it they fail with `not_ready`. Table-entry hooks need no code access.

## Engine functions

`hook.engine_function(path, handlers)` hooks one of the engine's reflected
functions by its path. It needs no offsets and no code access, so it works on every
install, and it keeps working across game updates while the function keeps its path
and arguments. This is the same model as UE4SS's `RegisterHook`.

The path is the class's path, a colon, and the function's name, as
`reflection.path` gives it for the function. Case doesn't matter.

```lua
local id, why = hook.engine_function('/Script/Engine.PlayerController:ClientRestart', {
    before = function(call)
        -- call.object is the controller; call.args.NewPawn is the pawn
    end,
    after = function(call)
        -- call.result is the result, or nil for a function without one
    end,
})
```

```javascript
const id = hook.engine_function('/Script/Engine.PlayerController:ClientRestart', {
    before(call) { /* call.object, call.args.NewPawn */ },
});
```

`handlers` has `before`, `after`, or both. Each handler gets one table, `call`:

- `call.object` is the object the function runs on.
- `call.args` has the function's arguments by name. Values are the same as
  `reflection.get` gives: objects are handles, structs are tables, and names and
  strings are text.
- `call.result` is the function's result, in `after` only.

A change to `call.args` in `before` changes what the function gets. In `after`, a
value that the handler returns replaces the result, and a change to `call.args`
changes an argument that the function writes. Tsuchi writes a handler's changes
after it returns, all of them or none. If the handler raises an error, or a value
doesn't fit, none of its changes apply and the call goes on.

When several hooks are on one function, `before` handlers run newest first and
`after` handlers run oldest first.

The rules of `hook.wrap` apply to the handlers. They run on the game thread only,
with the same instruction budget. Every other Tsuchi call fails with `invalid`
inside them. Lua handlers can't yield, and JavaScript handlers can't return a
Promise.

### What reaches the handlers

The engine runs a reflected function through an entry in its description. Tsuchi
puts its own function in that entry. The entry is data, so this needs no code
access.

These calls reach your handlers:

- Calls that C++ code makes through `ProcessEvent`: events such as
  `ReceiveBeginPlay`, input bindings, timers, delegates, network calls, and
  `reflection.call`.
- A Blueprint's calls of C++ functions. A Blueprint passes the arguments as
  script code. Tsuchi runs that code first, the way the function's own code
  would, so your handlers get the values. An argument passed by reference is the
  Blueprint's own variable, so a change to it changes that variable.

These calls skip your handlers:

- A C++ function that other C++ code calls directly. UE4SS has the same limit.
- A Blueprint function that another Blueprint function calls.

Some C++ functions take a value of any type, such as the array functions. They
read the type from the Blueprint's script code as it runs. Don't hook those.

A Blueprint event without arguments runs as an entry point into its class's event
graph. Tsuchi hooks the event graph for that event and runs your handlers for its
entry point only.

The function keeps Tsuchi's entry until the game closes. With no hooks left, the
entry goes straight to the engine's own function. If the game unloads a class and
loads it again, its functions are new objects, so hook them again.

### Call a function by its path

To call a function, use `reflection.call`. Pass an object, or for a function that
belongs to the class, pass the class from `reflection.find_class`:

```lua
local class = reflection.find_class('GameplayStatics')
local result = reflection.call(class, 'GetTimeSeconds', {WorldContextObject = some_actor})
```

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
A script names an entry with `slot` in its target to get the same help. For offsets of its own,
a mod can check `mod.game_build()` and pick the ones it keeps for that build.

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
