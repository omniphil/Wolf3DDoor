# Licensing for the WOLFENSTEIN 3D door

Two separate things travel to the player, under two different licences.

## `wolf3d.wasm` and `wolf3ddoor`: the game, GNU GPL version 2

The game is **Wolf4SDL** (github.com/KS-Presto/Wolf4SDL), built from id Software's own release of the Wolfenstein 3D
source, which id later put under the GPL. Wolf4SDL offers it under id's licence *or* the GPL
(`third_party/Wolf4SDL/license-gpl.txt`); this door takes the GPL. The AdLib chip is **Nuked OPL3**, GPL-2 or later.
Wolf4SDL's MAME chip (a non-commercial licence, not GPL-compatible) is **not** compiled: its source only travels because
Wolf4SDL is kept unmodified in `third_party/`.

The door sends the compiled game (`wolf3d.wasm`) to every TRACE player, and compiles the same game into `wolf3ddoor`
for ANSI mode, so both are GPL-2 and the obligation is **source**: players are entitled to the source of what they
were sent.

**The source is published at https://github.com/omniphil/Wolf3DDoor**: the module, this door, the patches, and the
unmodified Wolf4SDL and Nuked OPL3 they are built from. Keep that repository up to date with the `wolf3d.wasm` the
door actually hands out; a player is entitled to the source of the game they were sent, not an older one.

Keep this file beside the door, so the licence travels with the game.

## The game data (`*.WL1`, packed into `wolf3d.pak`): id Software's shareware terms

The shareware episode ("Escape from Wolfenstein", v1.4) was given away by id and may be passed on **unchanged and not
for profit**, which is what this door does: `tools/mkpak.py` only packs the eight files, byte for byte. It is not GPL
and never became free software (only the engine did), so it is not in the repository; `tools/get_shareware.sh`
fetches it.

**Never put the registered game's data (`*.WL6`) or Spear of Destiny's (`*.SOD`) in this folder.** Those may not be
distributed, and the door would send a copy to every player.
