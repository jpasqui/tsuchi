---
title: Use Lua in your mod
description: Load modules, call the API, and handle events and errors in Lua.
---

Start with [Write your first mod](/develop/first-mod/) if you haven't made a mod folder
yet. Put your code in `main.lua`, and Tsuchi will run it when your mod starts. Each mod
gets its own Lua 5.4 state, so its globals won't replace another mod's globals.

## Libraries

You can use Lua's base functions, `coroutine`, `table`, `string`, `math`, and `utf8`.
There is no `io`, `os`, `package`, or `debug`. Read your mod's files with
[`file`](/reference/file/), and save your own data with
[`storage`](/reference/storage/). `print` writes to the [Tsuchi log](/develop/logs/).

You can use `load` to run Lua source text. It won't load compiled Lua chunks, and
`dofile` and `loadfile` aren't available.

## Modules

Put shared code in another `.lua` file and load it with `require`. Dots in the module
name become folders, so `require("lib.clock")` loads `lib/clock.lua` from your mod.
Tsuchi runs the file once and remembers its return value. If it returns nothing,
`require` returns `true`.

For example, put this in `lib/clock.lua`:

```lua title="lib/clock.lua"
local clock = {}
function clock.seconds(steps)
    return steps / 30
end
return clock
```

Then load it from `main.lua`:

```lua title="main.lua"
local clock = require("lib.clock")
print(clock.seconds(90))
```

You should see `3.0` in the log. `require` reads only your mod's files. To share data
with another mod, use the API that the other mod documents.

## Values

Call services as global tables, like `player.captain(0)`. Numbers and text use Lua's
own types. Lists come back as tables, and structs come back as tables with named
fields. Flags use named values set to `true`, like `{actors = true}` for a
pause request.

A *handle* is an opaque identity for an object or resource. Keep the handle itself
rather than turning it into a number. Repeated handles for the same identity compare
equal within your state, so you can use them as table keys. An actor can leave the
world or return to a pool while you still have its handle. When that happens, calls using the old handle return `nil` and
`"not_found"`. Keeping a handle doesn't keep its actor alive.

## Results

When an API call fails, it returns `nil` followed by a lowercase result name. When a
call succeeds without a return value, it returns `true`. Check against `nil`, as a
successful query can return `false`.

For example, wait until the first player's captain is available:

```lua
local captain_actor, why = player.captain(0)
if captain_actor == nil then
    print("Can't find the captain: " .. why)
else
    print("Found the captain")
end
```

The reference for each call lists its results. A wrong Lua argument, such as text
where a number belongs or an unknown closed enum name, raises a Lua error instead.
Use `pcall` if you need to catch a Lua error. Fix wrong arguments rather than retrying
them every step.

## Events

Subscribe with an `on_*` function and pass a callback. The callback receives a table
with the event's fields. Keep the returned subscription if you want to cancel it
before your mod stops:

```lua
local steps = assert(time.on_step(function(event)
    if event.number % 30 == 0 then
        print("Another game second")
    end
end))
```

Call `steps:cancel()` to stop listening. Tsuchi cancels your subscriptions when your
mod stops. Read [Events and cleanup](/develop/events-and-cleanup/) before keeping
resources between callbacks.

## Limits

Each call into your script, including startup and each callback, has a budget of
20 million Lua instructions. When it runs out, the script raises an error. Catching
the error won't give the call more time. A callback that runs out of instructions
won't run again.

Your mod's Lua state can hold up to 128 MiB. Going past that raises Lua's memory
error. Deep recursion can raise `stack overflow` before the memory limit. Lua tail
calls don't grow the stack, so an endless tail call can reach the instruction limit
instead. Split long work across steps and keep only the data you need.

## 64-bit IDs

A `u64` uses every bit of a Lua integer. Values with the high bit set appear
negative, so `0xffffffffffffffff` represents the unsigned value
18446744073709551615. Keep placement numbers as IDs and pass them back unchanged:

```lua
entity.on_spawned(function(event)
    if event.placement ~= 0 then
        local placed, why = entity.find_placed(event.placement)
        if placed == nil then print(why) end
    end
end)
```

Write high IDs as hexadecimal integer literals. A `u64` argument also accepts a
floating-point whole number from 0 through 2^53 - 1. Other floats and numeric strings
are refused. Arithmetic and comparisons still follow Lua's signed integer rules.
Smaller unsigned arguments, such as `u32`, need a nonnegative value in their range.
