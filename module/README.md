# wolf3d.wasm: the whole of Wolfenstein 3D as a TRACE module

Wolf4SDL from `../third_party`, compiled to WebAssembly and run by TERMinator's sandbox (`gamesandbox`) on the player's
own machine. **The game's own code is untouched** apart from `../patches` (see `../README.md`); everything replaced
is the part that would talk to SDL, SDL_mixer or the file system. Modelled on the Tyrian module (`../../Tyrian`).

## Build

```
make            # needs wasi-sdk in ~/tools (same as the DOOM and TYRIAN modules)
```

The first build after a patch changes can fail at the `rm -rf obj/wolf4sdl` step while Dropbox holds the folder; the
Makefile retries once, and running `make` again always works.

Headless, without a BBS or a window (the pack goes in the assets folder named by its SHA-256):

```
gamesandbox_probe gamesandbox wolf3d.wasm -seconds 24 -assets <dir with <sha>.bin> -data pak=<sha> \
    -keys 57@2000,57@4000,57@6000,328@7500,328@8000,328@8500,328@9000,328@9500,28@11000,28@12500,28@14000 \
    -shot game.bmp
```

That is three keys past the sign-on to the menu, five Ups from "Read This!" (shareware's default) to "New Game"
("Save Game" is skipped while disabled), then Enter for New Game, episode 1 and the difficulty.

⚠️ The `gamesandbox_probe.exe` in `TERMinator-Windows/trace/bin` (checked 2026-09-19) is older than its source: it
sends E0 keys as 256 + the code with no extended flag, and ignores `:hold`. The module accepts both forms, and holds a
release that arrives in the same poll as its press (`MIN_HOLD_MS`), so the probe's taps still register.

## What's in here

| File | Replaces | What it does |
|---|---|---|
| `src/wolftrace.c` | `main` | The TRACE entry points. Wolf4SDL's `main()` runs on its own thread as `wolf_main` with `--res 640 400` (or what the door's `res=` asks for). Loads the data pack, receives the player's files, makes `exit()` wait for saves to reach the BBS and close TERMinator's picture. |
| `src/sdl_trace.c` | SDL2 | Clock, `SDL_Delay` (which keeps sound and saves flowing), keys (set-1 scancodes to SDL scancodes, the Tyrian module's tables), surfaces with palettes, the 8-bit to 32-bit blit that makes each frame, `SDL_RenderPresent` = `trace_present` at 4:3. |
| `src/mixer_trace.c` | SDL2_mixer | 8 channels with groups, panning and finished callbacks, the music hook (AdLib music *and* AdLib effects) and the post-mix (PC speaker), mixed on the game thread into TERMinator's queue. |
| `src/opl_trace.c` | the OPL chip | `YM3812Init/Write/UpdateOne` over Nuked OPL3. |
| `src/file_trace.c` | the file system | `fopen`/`fclose`/`stat`/`unlink` (redirected by `include/wolftrace_compat.h`) over memory: the data pack, and the player's `config.wl1` and `savegam0-9.wl1`, which go to the BBS when written. |
| `include/SDL.h`, `SDL_mixer.h` | SDL headers | Only what the game uses, with SDL's own scancode numbers (they end up in `config.wl1`). |
| `include/wolftrace_compat.h` | | Forced into every file: `exit`, the file calls, `mkdir`. |

## Door messages

The same as the Tyrian module's. Down (door to game): `file name=<n> off=<o> total=<t>\n<bytes>` (the player's files,
before the game starts), `pak=<sha256> [res=<w>x<h>]` (starts it; the ANSI door asks for 320x200), `quit`. Up:
`put name=<n> off=<o> total=<t>\n<bytes>`, pieces of 3000 bytes (`door/files.h` FILES_CHUNK).

## Known gaps

- No mouse or joystick (TERMinator doesn't pass them to modules yet); keyboard only.
- Pause: there is no set-1 code for the Pause key in the key table yet. Esc (the menu) pauses the game anyway.
- Screenshots and demo recording do nothing (there's nowhere to keep them).
