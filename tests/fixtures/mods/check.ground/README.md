# Physical ground-contact check

Select this group explicitly on a disposable Cave000 save:

```text
python tools/stage_test_mods.py ground
```

The fixture waits for 30 consecutive controlled steps with actor updates
unpaused and for the first captain, loads the stock
Chappy class, and makes its own Chappy 100cm along world X and 20cm above the
captain. It holds only that actor's movement and pauses its own
`CarrotAIComponent` through reflected `SetComponentTickEnabled`, after checking
ownership and recording the original component tick state. The actor's own
update remains available to consume native contact updates. A logic suspension
would stop that update and correctly make contact observations unavailable.
The fixture does not change the captain, a squad, steering, or a level actor.

It checks the first query's `not_ready` result, a completed supporting-contact
sample, immediate invalidation on an upward launch, false contact while rising,
across the vertical-speed sign change, and while falling, then support near the
original floor height. Position and velocity are independent motion readbacks;
neither is used to manufacture a contact result. No navigation query is used.
Physical support can be reported before the actor's cached velocity reflects
the landing. The fixture logs the first floor-support sample and waits for a
supported sample with settled vertical velocity before ending each flight.

It then suspends its actor's physics and collision, lifts it by 150cm, and checks
that 45 stopped-body queries return `not_ready` while the actor stays lifted.
After restoration, a controlled zero-speed launch keeps the body updating. The
fixture checks fresh false contact during that fall and supporting contact on
landing. This is a controlled physics-restoration route, not a claim that a
sleeping body wakes naturally. It finally releases its holds, restores its
brain's recorded component tick state when that component still belongs to its
actor, quietly removes its actor, and checks refusal of the retired handle.

Initial readiness has a 9000-world-step bound. Later phase budgets count only
steps with player control and actor updates unpaused; a menu does not consume a
flight or removal budget. The fixture never changes pause state or menus.
Each flight is bounded to 320 such active steps. Class, brain and body readiness
have 180-active-step bounds; removal has a 90-active-step bound. A timeout fails every remaining
motion check and attempts owned-actor cleanup. Logs contain phase changes and
the first sample of each motion phase, rather than a line for every update.

The planned checks are the fixed list in `mod.json`. The normal check runner can
read them without a separate completion watcher. A pass covers this floor and
owned-body route. Wall and ceiling filters, attachment, ordinary Engine actors,
and sleeping-body refusal require the separate native or stand-in checks; this
fixture does not claim those observations.
