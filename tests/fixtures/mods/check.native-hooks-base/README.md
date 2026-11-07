# Native inline hook qualification

Select `native-hooks` explicitly when staging the test mods. This group requires
the experimental native-code-access runtime, its matching game patch, and process
metadata with the validated code-alias permissions. The ordinary runtime cannot
write these instructions.

The base, chain and observer are independent SDK-built modules. Both import-veneer
targets and all original words are generated from the game table and check-only
overlay. The fixture discovers main code through bounded memory queries and the
runtime's identity anchors. It hooks the final `BR x17` in each math veneer.

Only calls carrying a noncanonical private marker on the startup thread change
registers. Other calls continue without edits. Probes invoke pure cosine/sine and
fixture-owned helpers; they do not modify actors, assets or saves.

The chain joins both targets, proves B-to-A continuation order, asks the base to
remove its own hooks during an active callback, proves B-only behavior, removes
and reinstalls B, then deliberately refuses startup after installing one hook.
The observer depends only on base and sorts after chain. It checks that the loader
stopped chain and its handler no longer changes probe calls before reusing both targets.
`startup_cleanup` needs all three pieces of evidence: chain's
`startup_cleanup_armed` pass, the loader's `not_ready` startup rejection, and the
observer's stock-output pass. A missing chain mod alone is not proof. The hook
engine also skips records owned by stopped mods, so stock output does not prove
that hook records, trampoline slots or native module memory were reclaimed.
Independent core/loader accounting is needed for that assertion.

All declared checks must pass. Inactive target gateways remain assigned and their
branch words remain installed until the process exits; stock behavior, rather than
restored instruction bytes, is the callback-inactivity assertion. Host stand-in execution and
SDK compilation are separate from genuine in-game qualification.

Read this suite's log with its exact staged receipt:

```text
python tools/native_hook_fixture.py LOG --staged build/test-mods/checks.json --json qualification.json
```

The generic reader returns 1 for the intentionally blocked chain mod. This suite's
qualifier preserves that raw verdict and accepts its exact expected rejection only
when all seventeen checks, all three mod states, and the complete receipt agree.
It rejects missing checks, other failures, duplicate reports and incomplete logs.
