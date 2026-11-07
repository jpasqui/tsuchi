---
title: Add shader sets
description: Ship compiled shaders for materials made from scratch.
---

:::caution[WIP]
Shader sets haven't been tested in the game yet.
:::

Before you start, [create your mod](/develop/first-mod/), then make your material and
shader files with Kadan or your own tools. You'll need shaders compiled for Pikmin 4
version 1.1.1.

In Kadan, use a game master material as the parent of your own instance, or create
your own material graph. Game master graphs stay read-only, and instance switches
offer the variants the game can draw. Kadan shows unsupported nodes, blend modes,
and mesh kinds while you author a custom material. Add the material's asset or folder
to your mod's selection on the **Mods** page, then export it. Kadan includes the assets
it needs and its compiled shader set, and leaves game assets out of the package.

## Manifest

Put your *shader set*, the binary file of compiled shaders, in your mod's folder and
give its relative path in `shaders`. For example:

```json
"shaders": "shaders.bin"
```

Keep that file with the material it was made for. Make your material instance with a
game material as its parent, then add your textures and parameter values. Tsuchi uses
the set's compiled pixel shaders on a separate copy of that parent in the running game.

## Format

Make sure your tool writes the shader-set binary format. Each entry names your material
instance and its parent game material, and identifies the game's shader map, shader
count, and slots that will use your pixel shaders. If your material changes the shader
map's resource description, include the original description's size and the edits that
make the new description.

Use a set compiled for the game's shader map and version, as Tsuchi will refuse a
mismatch. It also refuses duplicate entries or slots, out-of-range slots, non-pixel
shaders, and malformed files. Check the log for the reason if your mod doesn't load.

## Materials

If your instance only changes an existing game material's parameters, you won't need a
shader set. For a material made from scratch, keep your instance and shader set together.
Compile its shaders for the game, as editor shaders and desktop cooked materials won't
work.

Choose a game material that matches the way you'll use your material. Meshes, particle
emitters, projected decals, and Image widgets need pixel shaders compiled for their own
renderer paths. Translucent meshes need passes compiled for their renderer path
too. The game's material still supplies the vertex shaders and renderer state.
Shader sets don't replace vertex shaders, arbitrary Slate brushes, or Border brushes.
For a world-space widget, its outer mesh material and the Image material inside the
widget use separate paths.

A dynamic instance whose parent chain reaches your material gets its own copy of the
material.
Tsuchi copies its scalar, vector and texture parameters and keeps them current while
the instance is used. The copy starts from the game material's defaults, and then each
instance down the chain puts in its own values, so your shipped instance's values
replace the game's. Static switch and font parameters aren't copied. Separate instances keep separate values. If a
parameter copy fails, Tsuchi leaves the original material in place.

Keep each shader set in the mod that uses it. Tsuchi makes a separate material copy for
each mod and assigns dynamic instances where that mod's material is used. The game's
original parent and other mods' copies keep their own shaders. Until the copy of the
material is ready, or if Tsuchi can't make it, the material uses the donor's shaders with your
instance's textures and values. Choose a donor with the matching material domain,
blend mode, and usage flags so it supplies the renderer passes your material needs.

## Check

Install your mod, visit the map, and compare the result with the editor using the same
camera, lights, exposure, and material parameters. Check each place that uses your
material, then leave and revisit. If the material doesn't appear as you expect, check
the log for a mismatch or a material copy that couldn't be made.

When your mod stops, Tsuchi restores the materials it replaced, including later
assignments the game made. Tsuchi restores pooled components before reuse and widgets
before releasing a departing world's material copies. Original materials and their
copies stay held while they're used. Shader copies stay in memory until the game ends because
the renderer can still use them.

## Kadan archives

Kadan exports creator pixel programs in a UE4 shader archive and puts the material
metadata in mod.json. Keep the archive beside mod.json. Tsuchi reads the archive
and supplies each material with its compiled pixel programs. The player's game
supplies the vertex programs, so the mod carries no stock shader code.

The archive's name is in the shaders object's archive field. The shaders object
also records each creator material, its game parent, and the places of its pixel
programs in the parent's map. Kadan writes these fields when you save the mod.
