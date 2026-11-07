# QuickJS

Bellard QuickJS 2025-09-13, from
https://bellard.org/quickjs/quickjs-2025-09-13.tar.xz, under the MIT license in LICENSE.

The framework builds quickjs.c, cutils.c, libregexp.c, libunicode.c, and dtoa.c.
It does not build quickjs-libc or expose OS operations or bytecode loading.
TSUCHI_QUICKJS guards CONFIG_ATOMICS since the game thread has no pthread
library. It uses switch dispatch and polls before each interpreter opcode so
straight-line code shares the instruction budget with loops. Existing internal
polls also charge the budget. Stack checking remains enabled. The same build
configuration is used in host tests.

Only the license, the version, and the files the build uses are kept. The release's
command-line tools, quickjs-libc, tests, examples, and documentation are left out.
