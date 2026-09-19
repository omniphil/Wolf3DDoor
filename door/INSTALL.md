# Installing the WOLFENSTEIN 3D door on the BBS

Two ways to play, picked by each caller on the start page (remembered in their `saves/<player>/display.cfg`):

| Choice | What it is |
|---|---|
| 1. TRACE graphics (640x400 + Sound) | the door sends the game and TERMinator runs it on the caller's machine. Only offered to TERMinator with TRACE |
| 2. ANSI 24-bit | the game runs here and is sent as half-blocks in exact colour |
| 3. ANSI 256 | the same in xterm's 256 colours |
| 4. ANSI 16 | CP437 blocks and shades. Works in any BBS terminal |

This is the DOOM door's design (`../../Doom/door/INSTALL.md` has the ANSI details: detection, test strips, U for
UTF-8 blocks, link pacing).

## What the BBS needs

A Linux BBS box with `gcc`, `make`, `patch` and `python3`, and BBS software that writes a `door32.sys` drop file.

| File | Where it comes from | Size |
|---|---|---|
| `wolf3ddoor` | built here with `make` (the game is compiled in, for ANSI mode) | ~430 KB |
| `wolf3d.wasm` | ships in this folder (or `make` in `../module`, which needs wasi-sdk) | ~325 KB |
| `wolf3d.pak` | the shareware data, packed by `make install` from `../data` | 1.2 MB |
| `native/` | the game's sources, put here by `make bundle` on the development machine | |

## Steps

1. On the development machine: `make bundle` in `door/`, so the folder builds on its own.
2. Copy `door/` to the BBS and run `make && make install` there.
3. **Add a door entry** that runs `wolf3ddoor` with the folder holding `door32.sys` as its one argument. In Mystic:
   `(D3) Exec DOOR32 program` with Data `./doors/wolf3d/wolf3ddoor /path/to/mystic/temp%3`. Without a drop file every
   caller shares one `saves/player/` folder.

## Checking it works

```
python3 test_door.py            # a fake TERMinator: game and data arrive intact, config first, saves kept
python3 test_door.py --plain    # no TRACE: it is marked NOT FOUND and the ANSI modes are offered
python3 test_ansi.py 2 out/     # plays ANSI 24-bit headless and writes screenshots to out/ (3 = 256, 4 = 16)
rm -rf saves out                # the tests leave these behind
```

`WOLFDOOR_LOG=/some/file ./wolf3ddoor` writes the game's own messages there in ANSI mode.

## Saved games

Kept per player in `saves/<handle>-<user number>/`: `config.wl1` (settings, keys, high scores) and `savegam0-9.wl1`.
ANSI mode uses `ansi-config.wl1` as its config, because it starts the game with a whole-screen view and sound off, and
that mustn't follow the player into TRACE. Saved games are shared by both modes. A file is written beside the old one
and renamed over it when whole; the one it replaces is kept as `.bak`.

## ANSI controls

Arrows or W/S move, A/D strafe, R toggles running (off to start), F fires (hold it for rapid fire), Space opens doors, 1-4 weapons, Esc the menu, Ctrl-Q straight
back to the BBS, ` frames per second. Menus, questions and the save-name prompt are drawn as text boxes; the item the
game's gun points at is highlighted, and episodes 2-6 (not in the shareware) are dim with a `*`.

**Not redrawn as text yet:** "Read This!", View Scores, the tally between floors, and Change View. They show as the
game's own pictures, which can't be read at this size.

CPU: about 3% of one core per ANSI player (measured locally, Core Ultra 9 285H). TRACE players cost nothing after
the first upload.

### How held keys work (`ansi_input.c`)

A terminal never says a key was let go, and after the first press it waits for the keyboard's repeat delay (about
half a second) before repeating. The door can't tell a tap from the start of a hold until those repeats arrive, so
whatever it does in that half second is also what a tap does. It:
- moves or turns for exactly one frame on the press (the game's copy in this door has no minimum key hold);
- through the repeat delay, creeps with a few more one-frame steps (turns every 200 ms, moves once mid-way), so a
  hold starts slowly instead of pausing;
- holds the key steadily once the repeats arrive; any press within the repeat delay of the last one counts as held,
  so slow or bunched repeats over the network don't break a hold up.

Measured in the game: a tap turns 14-21 degrees or moves about 0.6 tile; holding 1.5 s turns ~140 degrees or walks
2.5-4 tiles. Holding walks; R adds Shift for running. Every F press is its own shot for the pistol and knife (they
only fire again after a release), held for the machine gun and chaingun.

The numbers are at the top of that section of `ansi_input.c`: `HOLD_STEP_MS`/`HOLD_TURN_MS` (a tap),
`PULSE_OFF_MS`/`PULSE_OFF_MOVE_MS` (how sparse the creep is: bigger = smaller taps but a slower start), and `CREEP 0`
for a plain nudge-then-pause. A caller can make holds start sooner by lowering their keyboard's repeat delay.
