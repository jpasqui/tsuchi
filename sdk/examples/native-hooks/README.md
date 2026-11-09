# Port two inline hooks

Use this example to port two native callbacks while keeping your source private.
It contains no game offsets or mod behavior and refuses startup until you add them.

1. Copy this folder into your mod's work folder.
2. Copy `targets.example.h` to `targets.h`. Enter the two main-code offsets and
   expected stock instructions from your mod for the game version you support.
   Each expected array can hold more words for your edit checker.
3. Port each callback's body into `on_delivered` or `on_model`. Cast `registers`
   to `uint64_t*` for x0 through x30. Cast `vectors` to `uint64_t*` for the low and
   high halves of q0 through q31. Use a 32-bit cast for the W view of an integer
   register. The stock instruction executes after the handler chain returns.
4. Keep the call through each hook's `original` to run other mods' handlers.
5. Build with the matching SDK:

   ```sh
   python sdk/build_mod.py native-hooks/main.c --out native-hooks/mod.nro
   ```

The example checks both locations before it installs either hook. It removes
the first hook if the second installation fails. Tsuchi also releases installed
hooks when startup fails or the mod stops. This is not an atomic installation
across game threads; choose hook locations whose callbacks tolerate startup.

The runtime must offer native code access. A release installation includes it:
its `main.npdm` grants the native-hook system calls. Read **Port native hooks**
in the matching documentation.
Passing the SDK and emulator checks does not qualify a mod in the game.
