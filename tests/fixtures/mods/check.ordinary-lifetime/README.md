# Ordinary actor lifetime observer

This is a game-only native SDK fixture. It does not run in the host model or the default game check set. No game result is recorded by these sources.

Copy the example to `targets.txt` only in the staged private mod after qualifying its inputs. The twelve nonempty lines name the route (`unload` or `destroy`), exact world, loaded class, exact actor name (or `-`), component name, native owner vtable, three stock leaf addresses, replacement material (or `-`), matching clip (or `-`), and a hexadecimal step deadline. The example's names and vtable are captured candidates. Their current live visibility, material, mobility, and lifetime behavior remain unobserved.

The observer wraps ProcessEvent, EndPlay, UnregisterAllComponents, and Destroyed before the first hold. It arms one reflected getter, disarms the native receiver locator before its continuation, and keeps the public Actor handle, native receiver, component owner, vtable, and object index together. The three stock comparisons reject an unexpected native owner table. The runtime's later outer wrappers restore or forget holds before this observer continues to the native leaf.

The fixture requires a visible original component. If a material path is given, the replacement must differ from the material read from the native mesh. If a clip path is given, the native component must have mode 0, a nonempty AnimClass and animation instance, and a matching mesh/clip Skeleton. A default mode 0 with an empty AnimClass is refused as animation evidence. The fixture holds a visible component hidden, optionally holds a distinct material and a stationary clip, and adds its own SceneComponent.

For `unload`, wait for `held-ready` before leaving through the normal game route. The fixture never requests travel itself. It copies restored native resources and ended bookkeeping before the original unregister or EndPlay. Temporary unregister with a remaining visibility hold is ignored. The result waits for this owner to leave the Engine actor world list after the original returns. The public Actor handle can remain alive after unload. A stale-handle fallback does not satisfy this observation.

For `destroy`, the fixture calls reflected K2_DestroyActor after 30 held steps. Invocation success is logged without treating it as accepted destruction. The qualified native Destroyed callback and the engine's BeingDestroyed flag establish acceptance. The fixture expects forgotten holds. It does not expect restoration on parts the engine is destroying.

The added-component check proves native absence before surviving owner parts unregister and the end of its lease. It does not count native DestroyComponent invocations. On accepted destruction, a later `remove` returning `not_found` can also mean the component became stale, so that return alone does not prove private added-component bookkeeping ended before teardown. The ordinary-owner event check verifies that this owner's handle received no gameplay birth or removal event. It does not qualify the timing or counts of unrelated gameplay events.

## Remaining prerequisites

- The selected observer must install before any other mod takes an ordinary hold on this native owner table. A preexisting runtime outer wrapper reverses the desired observation order. Use a cold boot with this explicit group and the matching module and patch.
- Animation on an ordinary owner needs a live surviving Blueprint-driven mesh with a compatible clip. Cave005's fan is a candidate. Its native AnimationMode, AnimClass, instance, Skeleton equality, and route remain unqualified.
- Mod stop needs an observer-owned surviving component and a separate producer that takes holds and refuses startup. The current loader starts mods at the first begun world, which can be Title. This fixture does not add a private mod-stop call or delay framework startup. A late Cave001 donor cannot by itself qualify producer startup cleanup.
- Added skeletal components that the observer destroys before clip cleanup cannot prove Blueprint reactivation on a surviving mesh. An added mesh with an empty Blueprint class proves only mode restoration. Neither substitutes for the animation prerequisite.
- An accepted native destruction, temporary unregister/register, and callback-triggered reentrant destruction need their own measured game observations. This fixture does not manufacture native flags, invoke native leaf addresses, or fabricate destruction results.
- No game fixture is claimed by creating or compiling these sources. The parent must qualify the donor, route, hook ordering, and current runtime source before a game launch.
