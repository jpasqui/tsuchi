# Action bindings and control hints

Character input hooks are mapped for Switch 1 1.1.1 and 2.0.0, Switch 2 2.0.0,
and the Switch 2 demo. Each build supplies the 23 original reads, seven additional
isolated-action reads, four controller virtual entries and the controller layouts.
The additional reads cover both throw-release paths, the alternate throw result,
dismount, Rush charge maintenance and both Jump-to-Rush paths. They retain their
existing rule: an extra read changes only when an isolated action state owns it.

The ports were checked against each original executable, including the instruction
at every hook, its controller/character register, controller slots and press-age
loads. Switch 2's alternate throw result is intercepted before its first saved
character register is restored, rather than copying the Switch 1 epilogue offset.
Static port checks and live game tests are distinct; in-game qualification currently
covers 1.1.1.

`ui.hold_control_hint(space, key, glyph)` owns the glyph beside a localized action
label. For example:

```lua
ui.hold_control_hint("ActionMarker", "ThrowPikmin", ui.glyph("r"))
ui.hold_control_hint("ActionMarker", "Attack", ui.glyph("x"))
```

The HUD, floating actor prompts and stock control diagrams use separate icon and
label fields. The adapter pairs their reflected Guide/Text, Button/Message and
Button/Guide members and resolves each line independently. Localized metadata
identities distinguish identical captions such as Previous and Next Pikmin type.
Plain labels use exact normalized text only when neither side has an identity.
A single icon keeps its original scale. Key names, Unbound and multiple bindings
flow with the caption, retaining hold indicators and avoiding the fixed-width
icon column. Both original fields are restored together.

Native SetText calls update affected hints before returning, including actor
prompts rewritten during each world tick. Acquisition and explicit refresh perform
one batched discovery; idle frames do not scan the
object table. Generation-checked widget handles and retained original FTexts allow
release or mod stop to restore localized text. A foreign text replacement is
preserved. `ui.release_control_hint` ends one ownership; `ui.refresh_control_hints`
also rebuilds existing rich text after a batch of `text.override` changes.

Better Settings uses these holds for action columns and replaces only audited
action tokens in the current localized instructions. It preserves menu confirmation,
photo controls and tablet navigation. Its Jump / Rush setting drives both isolated
states from one input, retaining the game's tap-to-jump, hold-to-charge behavior.
The new UI calls are appended to the ABI lock without moving existing slots.
