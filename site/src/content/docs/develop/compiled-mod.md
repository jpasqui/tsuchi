---
title: Build a compiled mod
description: Build C or C++ code into a mod that Tsuchi can load.
---

To build a compiled mod, you'll need the SDK, Python 3.11 or later,
and LLVM's Clang and LLD with AArch64 support. Put `clang` and `ld.lld` on your PATH.
These tools build code for the game's processor. If you don't have
any code to build yet, then you can use the C example from
[Write your first mod](/develop/first-mod/).

Start with Lua or JavaScript if your mod only needs the Tsuchi API, as you can write either
with a text editor. Use compiled code if your mod needs better performance or has to hook
the game's functions.

## SDK

Create a work folder and put `Tsuchi-SDK.zip` in it.
From that folder, extract the whole SDK:

```sh
python -m zipfile -e Tsuchi-SDK.zip .
```

Keep this layout together:

```text
sdk/
  build_mod.py
  support.c
  start.S
  mod.ld
  libc/string.h
  include/tsuchi/
    tsuchi.h
    core.h
    ...
  check/
  examples/olimar.clock/
    mod.json
    main.c
    main.lua
    main.js
  tsuchi.d.ts
  inventory.json
```

The player archive installs Tsuchi. The SDK builds mods and doesn't install the
framework. Use the SDK package that matches your Tsuchi release.

## Headers

In a compiled mod, you can only use the Tsuchi headers. This means that you can't use the C
or C++ standard library, exceptions, or RTTI. The SDK does include the functions that
compilers rely on, like `memcpy`.

The first step is to include the Tsuchi header:

```c title="main.c"
#include "tsuchi/tsuchi.h"
```

Your mod also has to define this function:

```c title="main.c"
TSUCHI_Result tsuchi_mod_start(const TSUCHI_Host *host, TSUCHI_Mod *mod);
```

Tsuchi will call this function when it loads your mod. Make sure to save both arguments, as
you'll need `mod` for every API call and `host` to reach the services. If everything went
well, then return `TSUCHI_OK`. If you return anything else, then Tsuchi won't load the mod.

## Build

The SDK includes the exact C, Lua, and JavaScript examples from [Write your first mod](/develop/first-mod/).
Create an `olimar.clock` folder next to `sdk` and copy the example's `mod.json` into it.
For the C example, leave both `main.lua` and `main.js` out of that folder, then
run this command from your work folder:

```sh
python sdk/build_mod.py sdk/examples/olimar.clock/main.c --out olimar.clock/mod.nro
```

For the Lua or JavaScript example, copy `sdk/examples/olimar.clock/main.lua` or
`sdk/examples/olimar.clock/main.js` into `olimar.clock` instead. Choose one script for the
folder. If you also include `mod.nro`, both the script and compiled code run. Next,
[install your mod](/players/install-a-mod/).

To build your mod, run `build_mod.py` with your source files and the path for the output
file:

```sh
python sdk/build_mod.py main.c --out olimar.clock/mod.nro
```

You can also add include directories with `-I` and defines with `-D`:

```sh
python sdk/build_mod.py main.cpp squad.cpp -I include -D DEBUG --out olimar.clock/mod.nro
```

## Cleanup

Read [Events and cleanup](/develop/events-and-cleanup/) for the changes Tsuchi
releases when your mod stops and the globals you need to reset on a restart.

## Scripts

You can include `main.lua` or `main.js` alongside `mod.nro`. Tsuchi starts the script first,
and if either entry fails to start, the mod won't load and Tsuchi cleans up its
startup changes. Use one
script language per mod, as Tsuchi refuses a folder containing both script entries.
