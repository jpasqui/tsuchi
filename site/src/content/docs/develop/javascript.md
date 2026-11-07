---
title: Use JavaScript in your mod
description: Call Tsuchi's API, listen to events, and await a dialog answer.
---

:::caution[WIP]
JavaScript mods haven't been tested in the game yet.
:::

Start with [Write your first mod](/develop/first-mod/), then put your script in
`main.js`. Tsuchi runs JavaScript with QuickJS. You can import text modules from
your mod's folder with `import { double } from "./lib/helper.js"`.

Use either `main.js` or `main.lua` in a mod. If your mod also has `mod.nro`, Tsuchi
starts the script first, then the compiled code. If either entry fails at startup,
Tsuchi blocks the mod and releases its subscriptions and holds.

## Values

Call a service by its schema name, such as [`entity.list`](/reference/entity/#list).
Structs use objects with named fields, vectors use `{x, y, z}`, and rotations use
`{pitch, yaw, roll}`. Enums use their value names as strings. Flags use arrays of
names, such as `["logic", "movement"]` for
[`entity.hold_suspended`](/reference/entity/#hold_suspended).

Keep handles as the objects the API returns. A Number can't stand in for a handle.
The same handle compares equal within your mod, so you can use handles as Map keys.
An empty optional handle comes back as `null`. Calls with no returned value give
`true` when they succeed. Read and write bytes as `Uint8Array`.

## Integers

Every `u64` uses a *BigInt*, an integer that keeps all its bits. Use `0n` for zero,
`9007199254740993n` for 2^53 + 1, and `18446744073709551615n` for UINT64_MAX.
A `u64` argument refuses Numbers, strings, negative BigInts, and values above UINT64_MAX.
Signed `i64` values use BigInt too. Smaller integer types use Number with range checks.

For example, pass an event's placement number straight to
[`entity.find_placed`](/reference/entity/#find_placed):

```javascript
entity.on("spawned", (event) => {
    if (event.placement !== 0n) {
        const placed = entity.find_placed(event.placement);
    }
});
```

Converting a placement number to Number can make two placements become the same
number. Keep placement numbers as BigInts when you compare or pass them to the API.

## Errors

A failed call throws an `Error`. Its `name` is the result code, such as `not_found`
or `busy`, and its `message` is the schema's failure description.

For example, handle a missing placement and let other failures reach the log:

```javascript
try {
    const placed = entity.find_placed(9007199254740993n);
} catch (error) {
    if (error.name !== "not_found") throw error;
    print(error.message);
}
```

A wrong argument raises `TypeError` or `RangeError` before the framework calls the
service. An error in an event handler goes to your mod's log, and later events still run.

## Events

Use `service.on("event", handler)`. The handler receives one object with the event's
fields. Call `cancel()` on the returned subscription to stop listening:

```javascript
const steps = time.on("step", (event) => {
    if (event.number % 1800n === 0n) print("Another minute has passed");
});

// Later, when the mod no longer needs these events:
steps.cancel();
```

Callbacks run on the game thread. Your script's hook-originated events are queued
for the start of the next world step, so your handlers don't run inside a game hook. Tsuchi ends your
mod's subscriptions and resource holds before freeing its runtime. Restarting the
mod creates a fresh runtime and runs `main.js` again.

## Waiting

Use `await` for a dialog answer. `main.js` supports top-level `await`, and an event
handler can be `async`:

```javascript
const answer = await dialog.confirm("Keep playing?");
if (answer.accepted) print("The player accepted");
```

The answer has the same fields as
[`dialog.answered`](/reference/dialog/#answered). If you need the handle before the
player answers, keep the Promise and read its `dialog` property.

[`asset.load`](/reference/asset/#load) returns a Promise for the asset handle. The
asset load runs synchronously before that Promise resolves. Background asset loading
is a separate capability, and this Promise doesn't make `asset.load` a background call.
[`reflection.call`](/reference/reflection/#call) returns `[result, outputs]`, with
`null` for a function with no result.

## Time

`Date` doesn't know the real date or time. `Date.now()` counts from when Tsuchi
started, as if Tsuchi had started at midnight on 1 January 1970 UTC, so use it to
measure how long something takes. For game time, count
[`time.step`](/reference/time/#step) events, as a step is always 1/30 of a game second.

## Limits

Each call from Tsuchi lets your script run up to twenty million interpreter
instructions, and Promise jobs share that turn. Catching an error doesn't renew
the budget. A handler that exhausts its turn is disabled. One mod's runtime can
hold up to 128 MiB, including binding buffers and callbacks it keeps. JavaScript
gets a 256 KiB native stack allowance measured from the first entry into the runtime.
Reentering that runtime keeps the original allowance. Deep recursion raises stack
overflow independently of memory exhaustion.
Tsuchi evaluates text, and exposes no bytecode loader, OS library, or worker threads.

## Editor

For editor checks, use `sdk/tsuchi.d.ts` from the SDK. Put it beside your script and
tell your editor to check against ES2020 or later. Tsuchi offers JavaScript's language libraries, and browser and Node APIs
aren't available. Use `print` or `console.log` for your mod's log.