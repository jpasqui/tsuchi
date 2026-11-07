# Input proof

Run this mod alone in a played level, with `sdk/check/check.lua` copied into its
folder. Don't touch the controller while it runs.

After the player has had control for ten seconds, the mod hides **+** and presses it for three world
steps. The game shouldn't open its menu, and `hidden-ignored` should pass. Then
the mod shows **+** again and presses it once more. The menu should open, and
`shown-opened` should pass. Take a screenshot while the menu is open. Ten seconds
later the mod presses **+** to close the menu, and `closed` should pass.

Each `proof input` line in the log has the step, whether the player has control,
and what the game has paused.
