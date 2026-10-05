# Installing the WOLFENSTEIN 3D door on the BBS

Three ways to play, picked by each caller on the start page (remembered in their `saves/<player>/display.cfg`; a choice
saved before the JPEG XL mode came in is moved up to the same mode's new number):

| Choice | What it is |
|---|---|
| 1. TRACE graphics (640x400 + Sound) | the door sends the game and TERMinator runs it on the caller's machine. Only offered to TERMinator with TRACE |
| 2. JPEG XL graphics (320x200 + Sound) | the game runs here; its real picture goes as JPEG XL and its sound plays from the terminal's cache (below) |
| 3. ANSI 24-bit | the game runs here and is sent as half-blocks in exact colour |
| 4. ANSI 256 | the same in xterm's 256 colours |
| 5. ANSI 16 | CP437 blocks and shades. Works in any BBS terminal |

This is the DOOM door's design (`../../Doom/door/INSTALL.md` has the details: detection, test strips, U for UTF-8
blocks, link pacing, and the JPEG XL mode's pacing and quality). The start page is the first thing a caller sees: the
menu shows what was detected.

## What the BBS needs

A Linux BBS box with `gcc`, `make`, `patch` and `python3`, and BBS software that writes a `door32.sys` drop file.

| File | Where it comes from | Size |
|---|---|---|
| `wolf3ddoor` | built here with `make` (the game is compiled in, for ANSI mode) | ~430 KB |
| `wolf3d.wasm` | ships in this folder (or `make` in `../module`, which needs wasi-sdk) | ~325 KB |
| `wolf3d.pak` | the shareware data, packed by `make install` from `../data` | 1.2 MB |
| `sound/` | the JPEG XL mode's sound, rendered from the game's data (below). Copied with the folder | 5.8 MB |
| `native/` | the game's sources, put here by `make bundle` on the development machine | |
| libjxl | the system's `libjxl.so` (0.7 or later), loaded at run time; without it the JPEG XL mode isn't offered | |

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
python3 test_ansi.py 3 out/     # plays ANSI 24-bit headless and writes screenshots to out/ (4 = 256, 5 = 16)
rm -rf saves out                # the tests leave these behind
```

`WOLFDOOR_LOG=/some/file ./wolf3ddoor` writes the game's own messages there, and in the JPEG XL mode its numbers
(frames a second, KB a second, quality, round trip) every 5 seconds. Without it a JPEG XL game keeps them in
`saves/<player>/jxl.log` (the last game only). A `saves/<player>/linktest.cfg` (`kbps=300`, `ping=80`) plays that
player's JPEG XL games through a link of that speed, for seeing how it does over the internet.

## JPEG XL mode (added 2026-09-27)

The DOOM door's JPEG XL mode (`../../Doom/door/INSTALL.md`, "JPEG XL mode") for Wolfenstein: offered to a terminal that
answers as CTerm and draws JPEG XL (`Q;JXL`), with sound if it also plays Ogg Vorbis and WAV files. The game runs here
with the player's own settings (TRACE's `config.wl1`: its status bar, view size and sound), each frame goes as a
320x200 JPEG XL the terminal scales up, at most 30 a second, sharper or softer to suit the link. Measured on a
modelled link: 30 fps at distance 0.5 on 1-2 MB/s, 26-27 fps at distance ~3 on 300 KB/s with a 120 ms ping.

Keys: a terminal that reports presses and releases (`CSI = 1 h`) gets the game's own keys, Shift to run, and F as well
as Ctrl fires; one that doesn't gets the ANSI mode's keys (above), R to run.

**Sound** plays on the caller's terminal from files in its cache, so almost nothing is streamed: the digitised sounds
(WAV, upsampled to 22 kHz as the game does) and every AdLib effect go up once, about 1.7 MB, checked by md5 on each call; music is
each track cut into 5-second Ogg Vorbis pieces, sent just before they're needed (or earlier while the link is idle).
The game's own sound code runs as in ANSI mode, and its calls are caught on the way (`pix_hooks.c`, linked with
`--wrap`), so its timing is untouched. Levels match TRACE (the mixer's gains, `module/src/mixer_trace.c`).

`sound/` is made on the development machine from `../data` (it's derived from id's data, so it ships with the door but
isn't in the public source):

```
make wolfrender && mkdir -p /tmp/wolfsound && ./wolfrender ../data /tmp/wolfsound && python3 tools/make_sound.py /tmp/wolfsound sound
```

`wolfrender` drives the Nuked OPL3 chip the way the game does (music at 700 Hz, effects every fifth tick). The
shareware data has placeholder chunks for the 16 songs only the full game has; they are skipped (11 tracks remain).

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
