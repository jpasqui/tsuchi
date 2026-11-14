# Mod settings and native lifetime controls

Owner: Codex, 2026-11-09. Branch codex/live-settings-controls, from main e473a9e.
The repository owner requested a PR for these outstanding framework changes.
This isolated worktree contains framework source, schema, author documentation
and authored tests only. Gameplay packages and unfinished population reduction
behavior remain in the selected-mods worktree. Merged PRs6/7/9 remain intact.

Scope: dependency settings overrides, owned runtime scalar holds, integer runtime
bounds, numeric arrow controls and steps, registered Pikmin type count, owned
reflection fields, checked direct-call hooks and process-lifetime restart guards.
All mainline compiled table prefixes are preserved, including settings.on_changed.
Design: docs/MOD_CONTROL_DESIGN.md. No game data, local research or captures ship.

Validation is being rerun against this isolated branch before publication.
