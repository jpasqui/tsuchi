# Tsuchi SDK

You can write a Lua or JavaScript mod with a text editor. To build a C or C++ mod, install
Python 3.11 or later and Clang with LLD and AArch64 support. Put `clang` and
`ld.lld` on your PATH.

Extract the whole archive into one folder. Keep these files together:

```text
sdk/
  build_mod.py
  support.c
  start.S
  mod.ld
  libc/string.h
  include/tsuchi/*.h
  tsuchi.d.ts
  check/check.lua
  check/tsuchi_check.h
  examples/olimar.clock/
    mod.json
    main.lua
    main.js
    main.c
  inventory.json
```

The headers describe the API of the matching Tsuchi release. The small support
files provide the compiled mod's entry point, linker layout and memory functions.
The check helpers are optional tools for checks you write in your mod.

## Your first JavaScript mod

Create an `olimar.clock` folder. Copy `mod.json` and `main.js` from
`sdk/examples/olimar.clock` into it, and install that folder as your mod. Choose
one script entry, `main.lua` or `main.js`; Tsuchi refuses a mod with both.

Use `sdk/tsuchi.d.ts` for editor type checks against ES2020 or later. Every `u64`
and `i64` uses BigInt. Events use `service.on("event", handler)`, and dialog answers
use `await`. A failed service call throws an Error with the result code as its name.
See the JavaScript guide in the matching docs for handles, flags, bytes, and cleanup.

## Your first Lua mod

Create an `olimar.clock` folder. Copy `mod.json` and `main.lua` from
`sdk/examples/olimar.clock` into it. Install that folder as your mod.

The example logs its name and version when it starts. It subscribes to world
steps and logs the elapsed minutes every 1800 steps. World steps stop when the
world pauses.

## Your first C mod

Create a separate `olimar.clock` folder. Copy the example's `mod.json` into it.
From the folder where you extracted the SDK, run:

```sh
python sdk/build_mod.py sdk/examples/olimar.clock/main.c --out olimar.clock/mod.nro
```

Install the `olimar.clock` folder containing `mod.json` and `mod.nro`. The C
example logs when it starts and every 1800 world steps. Choose one example for
this folder. If you include both `main.lua` and `mod.nro`, both run.

Your source includes `tsuchi/tsuchi.h` and defines `tsuchi_mod_start`. Add source
paths to the command to build more files. Use `-I folder` for your own headers
and `-D NAME=VALUE` for a definition. C++ uses C++17 without exceptions or run-time
type information. Your mod cannot link against a system library.

## Archive contents

`examples/native-hooks` provides two inline callbacks and checks both target
locations before installation. Copy `targets.example.h` to `targets.h` and supply
your mod's offsets and stock instructions. The example contains no game targets.
Runtime code writes need code access, which a release installation includes
through its `main.npdm`. Read **Port native hooks** in the matching docs.

`inventory.json` records the API version, source input hashes and the size and
SHA-256 hash of every other archive member. It excludes its own hash.

This SDK contains generated C headers, the Python build script, first-party SDK
support files, check helpers and the first-mod examples. It contains no framework
runtime implementation, third-party source or game data.
