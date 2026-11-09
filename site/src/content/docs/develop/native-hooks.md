---
title: Port native hooks
description: Port compiled function hooks, register callbacks, and instruction checks.
---

Start with [a compiled mod](/develop/compiled-mod/) and the SDK that matches the
runtime. Native hooks require the stock instructions and signatures for the game
version you support. Your mod's source can remain private. The offsets in these calls, and any
your mod reads game memory by, belong to one build of the game, so list the builds it supports
with `game` in its manifest. The same mod then runs on no other build.

The ordinary runtime includes native code access as well as table hooks and named
sites described in [Hook game functions](/develop/hooks/). Complete the process
metadata step in the normal installation guide before running native hooks. The
SDK alone does not grant launch permissions.

## Function entries

Use `hook.wrap_function` with a main-code offset, the expected first instruction,
and a wrapper with the game's function signature. The displaced instruction must
be position-independent. The returned `Hook.original` continues through older
wrappers and then the game's function.

Tsuchi checks the instruction before installation. A mismatch returns `busy`.
An existing named function-entry site reuses its chain and consumes no dynamic
gateway. Named call sites must use `wrap_site` instead.

## Inline callbacks

Use `hook.wrap_inline` with an instruction offset, its expected word, and a handler
whose C signature is `void handler(TSUCHI_InlineContext *context)`.

The context provides:

- `registers`: 31 writable `uint64_t` values, x0 through x30.
- `vectors`: 64 writable `uint64_t` values, the low and high halves of q0 through q31.
- `nzcv`, `fpcr`, and `fpsr`: the condition flags and floating-point registers.
- `sp`: the game's stack pointer, which you can read. Changes to this field have no effect.

Call the returned `Hook.original` with the context to continue through other mods'
handlers. The displaced stock instruction executes once after the chain returns.
Keep the returned hook record available while its callbacks can run. Don't reuse
that record's storage until callbacks from the previous installation have returned.
The gateway preserves SP and returns through its replay sequence. It offers no
program-counter override. Changes to registers still affect instructions that
use those registers, including indirect branches and returns.

Replay supports position-independent instructions, direct branches and calls,
conditional branches, ADR, ADRP, and integer and vector literal loads. Indirect
BLR calls retain the stock return address, except BLR x30, which is refused because
its source and link register alias. A refusal happens before code changes.

Use the SDK's `examples/native-hooks` folder for a two-hook starting point. It
contains no game addresses or behavior. Enter your existing targets in
`targets.h`, then port the callbacks' register logic. A callback that writes a W
register must zero the upper 32 bits of its corresponding X value.

## Instruction checks

Use `hook.check_code` with an offset and expected bytes. It compares 4 through
4096 bytes in whole instructions and needs no code-write permissions. A changed
byte returns `busy`. Check every target before you install the first hook when
your mod needs several hooks. These comparisons read the running bytes, including
code already changed by Tsuchi or another mod.
If an existing checker hashes the whole executable, account for Tsuchi's own
patch or narrow the checks to the stock regions your mod requires.

The checks and installations are separate operations. They don't stop other game
threads. Return a failed result from `tsuchi_mod_start` so Tsuchi releases hooks
already installed during that startup.

## Instruction patches

Use `hook.patch_instruction` to replace one aligned word after checking the
original. Remove the returned ID with `hook.remove`. Tsuchi also restores the
instruction when the mod stops, if the location still holds the replacement.
An overlapping hook or patch returns `busy`. A foreign code change is never
overwritten during restoration. Explicit removal returns `busy` while a foreign
change remains at the location.

Choose locations where replacing one instruction while other threads run is
valid. Multi-instruction patch transactions are not offered.

## Runtime

The native runtime allocates one 16 KiB executable arena for up to 256 distinct
dynamic function or inline targets. Each target keeps one gateway until the game
closes. Removing and reinstalling
hooks at that target reuses its gateway. With no live handlers, the gateway runs
the stock instruction. Instruction patches have 256 ownership records and need
no gateway. Framework sites and the patch region are excluded from dynamic targets.
A target also retains its hook kind. Switching it between function and inline
hooks, or replacing it with an instruction patch, requires a new process.
The experimental write backend caches aliases for up to 512 distinct main-code
pages until the process closes. Removing a patch frees its ownership record but
keeps that page mapping. A later write can therefore run out of alias space even
when fewer than 256 instruction patches remain active.

Targets and handlers consume the hook engine's shared pool of 1,024 thunks. A
dynamic target keeps one entry thunk, and each installed handler needs another.
One handler on each of 256 dynamic targets therefore uses 512 thunks before other
hooks are counted. The engine also has 1,024 handler records and 1,024 concurrent
callback frames. A removed handler retains its continuation until calls already
running through that handler return.

The runtime's status block reports assigned and published targets, active patches,
cached aliases, shared thunks, handler records, and active and peak callback frames.
Arena setup logs its state, failure phase and kernel result. If mapping fails, the
runtime leaves the target instruction unchanged. If rollback fails, the runtime
keeps the backing memory and refuses another arena allocation.

The standard module includes the code-write backend; no experimental build flag
is needed. Release packaging generates `main.npdm` from the authored policy in
`tools/process_metadata.py` and includes it beside the module. Players copy the
files together; no dump or Python step is required. Separate descriptors cover
the Switch game, Switch 2 Edition, and Switch 2 demo, including their process
IDs, core limits and ACID layouts. Signatures and key slots are zero-filled;
no game metadata file is read or redistributed.

The descriptor grants the game's system calls plus the backend's required calls
in both ACI0 and ACID, permits code reads, and allows filesystem and service
clients for mods. It grants no service-host wildcard. This requires a loader that
accepts replacement ExeFS descriptors, as Skyline does. Console qualification
remains outstanding.

For local runtime development, `python tools/build_module.py --native-hooks-npdm
path/to/prepared/main.npdm` can stage already-prepared metadata with the standard
module in `build/native-module/exefs`. This option validates rather than modifies
its input. Install the module, patch and metadata together, then restart the game.
A process started with different metadata has different permissions; runtime
mapping failure handling cannot make an unauthorized system call safe. Release
packaging accepts the ordinary code-access backend but refuses any local `.npdm`
input file or report recording supplied metadata. Only descriptors freshly
generated from the authored policy enter the archive.

The SDK hook service layout is unchanged. API 0 remains unfrozen; this change does
not promise compatibility across every development build.

The host checks exercise ownership, chaining, and continuation reclamation after
failed startups. ARM64 emulator checks exercise register capture, replay, and
absent-framework fallback. The external-arena build also passed 17 inline-hook
checks at two math import veneers in an isolated Pikmin 4 1.1.1 game-emulator run
with private development metadata. The fixture checked that both installed
branches use adjacent slots in one 16 KiB RX mapping outside main text. The
256-target capacity check is separate host/ARM64 evidence. The standard code-access build and local metadata installer also passed a live
Eden 1.1.1 cheat toggle check: an authored patch changed an unused code word and
turning it off restored the exact original bytes. Console support, function-entry
hooks, and other replay forms still need their own game qualification.

Three SDK fixture binaries built for the earlier static-gateway runtime also
passed the same 17 game checks unchanged on the arena runtime. Their installed
hashes matched the earlier build. This checks compatibility for those binaries
across this runtime change.
