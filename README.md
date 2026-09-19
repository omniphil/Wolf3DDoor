# WOLFENSTEIN 3D for TERMinator: a BBS door

id Software's Wolfenstein 3D (1992), shareware episode 1 "Escape from Wolfenstein" (v1.4), playable from a BBS. It
works the same way as the DOOM door (https://github.com/omniphil/DOOMDoor):

- **TRACE** (TERMinator 1.1.2+): the whole game runs on the caller's PC inside TERMinator's sandbox at 640x400, with
  AdLib music and digitised sound. The BBS sends it once; after that almost nothing crosses the wire.
- **ANSI 24-bit / 256 / 16** (every other terminal): the door runs the same game on the BBS and sends it as ANSI
  half-blocks. The menus are redrawn as text boxes and the status bar as a text line, because Wolfenstein's own are
  pictures of text that can't be read at 80x44. It is silent.

The start page lets each caller pick, and remembers the choice.

**This repository exists so that anyone who plays the door can have the source of the game they were sent**, which is
what the GNU GPL asks for (see `door/LICENSE.md`). It lives at https://github.com/omniphil/Wolf3DDoor, which is the
address the door itself gives out.

| Folder | What |
|---|---|
| `third_party/Wolf4SDL` | Wolf4SDL (github.com/KS-Presto/Wolf4SDL, commit dc8b250, 2024-05-20), unmodified. id's code under the GPL (license-gpl.txt) |
| `third_party/nuked-opl3` | Nuked OPL3 1.8 (GPL-2+), the AdLib chip, copied unmodified from Crispy Doom 7.1 |
| `patches/` | every change made to Wolf4SDL, applied to a copy at build time (see below) |
| `data/` | the shareware v1.4 data (`*.WL1`, 8 files). **Not in the repository** (id's, not GPL): `tools/get_shareware.sh` fetches it from archive.org item `wolf3dsw` (`wolf3dsw.zip`, md5 44729c473432d11b9194f52c648ea19d) |
| `module/` | `wolf3d.wasm`, the game as a TRACE module (see `module/README.md`) |
| `door/` | `wolf3ddoor` (see `door/INSTALL.md`) |
| `tools/mkpak.py` | packs `data/` into `door/wolf3d.pak`, the one asset the door sends |
| `module/trace/trace_api.h` | the engine API the module is written against, copied from TERMinator so this source builds on its own |

Build (Linux or WSL; the module needs [wasi-sdk](https://github.com/WebAssembly/wasi-sdk) 34 in `~/tools`):

```
sh tools/get_shareware.sh                  # the shareware data, into data/
make -C module && cp module/wolf3d.wasm door/ && python3 tools/mkpak.py data door/wolf3d.pak && make -C door
make -C door bundle      # before copying door/ to the BBS: puts the game's sources in door/native/
```

## The patches

`third_party/` stays exactly as released. Both builds copy Wolf4SDL into their `obj/` (module) or `game/` (door)
folder and apply `patches/*.patch` there. Every patched line is marked `[wolf3d-trace]`.

- `wl_def-itoa.patch`: a **Wolf4SDL bug**. Its non-Windows `itoa`/`ltoa` size their output with `strlen()` of the
  caller's *uninitialised* buffer. Native builds usually get lucky with stack junk; WebAssembly's stack starts
  zeroed, so every number (the whole status bar) came out empty. Now `sprintf`, like DOS's unbounded `itoa`.
- `wasd-keys.patch`: W/S move and A/D strafe (the game's own strafe buttons, which only a joystick could press), so
  TRACE has the same keys as the ANSI mode. The arrows, Alt-strafe and custom bindings all still work.
- `steady-frame-rate.patch`: `CalcTics` gives every frame at least 2 tics (`trace_min_tics`), so the game runs at a
  steady 35 fps like DOOM. Wolf4SDL's adaptive timing at ~49 fps moved the view 1 tic, then 2, then 1: judder while a
  key is held (user report, 2026-09-19).
- `ansi-text-hooks.patch`: a few globals the ANSI mode reads to draw text instead of pictures: the menu `HandleMenu`
  is running and where its gun points, `Message()`'s text, `Confirm()` waiting, the name `US_LineInput` is typing,
  and `ReadConfig` overrides (whole-screen view, sound off) that only the ANSI door sets. The TRACE module ignores
  them.

## Why not Wolf4SDL's own sound chips

Wolf4SDL offers MAME's OPL emulator (a non-commercial licence, not GPL-compatible) or DOSBox's (`USE_GPL`), and the
DOSBox one no longer builds since Wolf4SDL moved from C++ to C (`id_sd.c` still calls it as a C++ class). So `id_sd.c`
is built on its MAME path, which only *declares* `YM3812Init/Write/UpdateOne`, and `module/src/opl_trace.c` answers
them with Nuked OPL3. No MAME code is compiled.
