# The WOLFENSTEIN 3-D door. For TERMinator it sends wolf3d.wasm and wolf3d.pak, which run on the player's own machine
# (see ../module). For every other terminal it runs the same game itself and sends it as ANSI (ansi_*.c), so the
# game's sources are built in here too. Built for the BBS box; the same C builds anywhere POSIX. (The DOOM door's
# Makefile, for Wolfenstein.)
CC = gcc
CFLAGS = -Wall -Wextra -std=c11 -O2
LDFLAGS = -pthread -lm

# No built-in rules: with the patched-copy rule below they would chain into nonsense
MAKEFLAGS += -r
.SUFFIXES:

# The game's sources: the development tree when there is one (so edits there are always what gets built), otherwise
# the copy `make bundle` puts in native/ for the BBS
MODULE        = $(firstword $(wildcard ../module native/module))
WOLF4SDL_SRC  = $(firstword $(wildcard ../third_party/Wolf4SDL native/Wolf4SDL))
NUKED         = $(firstword $(wildcard ../third_party/nuked-opl3 native/nuked-opl3))
PATCH_DIR     = $(firstword $(wildcard ../patches native/patches))
TRACE_INCLUDE = $(firstword $(wildcard ../../../TERMinator-Windows/trace/include ../module/trace native/trace))

# Where the game is built (game/ beside the door unless GAME=<folder> is given)
GAME ?= game

# Wolf4SDL is built from a patched copy, the same patches as the module (see ../module/Makefile)
WOLF4SDL = $(GAME)/wolf4sdl
PATCHES  = $(wildcard $(PATCH_DIR)/*.patch)

DOOR_OBJ = main.o door.o trace_wolf.o files.o sha256.o ansi_host.o ansi_screen.o ansi_input.o ansi_play.o

# ---- the game, compiled for this machine ----
# The same files and flags ../module/Makefile builds into wolf3d.wasm, only for a normal CPU rather than wasm32
GAME_CFLAGS = -O2 -pthread -std=gnu99 -w -D_GNU_SOURCE -DVERSIONALREADYCHOSEN -DCARMACIZED -DUPLOAD \
              -include wolftrace_compat.h -I $(MODULE)/include -I $(MODULE)/src -I $(TRACE_INCLUDE) \
              -I $(WOLF4SDL) -I $(NUKED)

WOLF_SRC = id_ca.c id_in.c id_pm.c id_sd.c id_us.c id_vh.c id_vl.c signon.c \
           wl_act1.c wl_act2.c wl_agent.c wl_atmos.c wl_cloudsky.c wl_debug.c wl_draw.c wl_game.c wl_inter.c \
           wl_main.c wl_menu.c wl_parallax.c wl_plane.c wl_play.c wl_scale.c wl_shade.c wl_state.c wl_text.c \
           wl_utils.c
TRACE_SRC = wolftrace.c sdl_trace.c mixer_trace.c file_trace.c opl_trace.c
GAME_OBJ  = $(patsubst %.c, $(GAME)/%.o, $(WOLF_SRC) $(TRACE_SRC) opl3.c ansi_hud.c)

wolf3ddoor: $(DOOR_OBJ) $(GAME_OBJ)
	$(CC) $(DOOR_OBJ) $(GAME_OBJ) -o $@ $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

# trace_api.h marks its functions for WebAssembly, which gcc doesn't know and would warn about
ansi_host.o: ansi_host.c
	$(CC) $(CFLAGS) -Wno-attributes -I $(TRACE_INCLUDE) -c $< -o $@

$(GAME)/%.o: $(WOLF4SDL)/%.c | $(GAME)
	$(CC) $(GAME_CFLAGS) -c $< -o $@

$(GAME)/%.o: $(MODULE)/src/%.c $(WOLF4SDL)/.patched | $(GAME)
	$(CC) $(GAME_CFLAGS) -c $< -o $@

$(GAME)/opl3.o: $(NUKED)/opl3.c | $(GAME)
	$(CC) $(GAME_CFLAGS) -c $< -o $@

# Reads the game's state, so it is built with the game's headers and flags
$(GAME)/ansi_hud.o: ansi_hud.c $(WOLF4SDL)/.patched | $(GAME)
	$(CC) $(GAME_CFLAGS) -I . -c $< -o $@

# Wolf4SDL's main() runs on the game thread (the module's wolftrace.c), under another name
$(GAME)/wl_main.o: $(WOLF4SDL)/wl_main.c | $(GAME)
	$(CC) $(GAME_CFLAGS) -Dmain=wolf_main -c $< -o $@

$(WOLF4SDL)/%.c: $(WOLF4SDL)/.patched ;

# ⚠️ Without this make counts the patched copy's .c files as intermediate (they come from a pattern rule) and deletes
# them at the end of every build, leaving the copy empty but for wl_main.c
.PRECIOUS: $(WOLF4SDL)/%.c

$(WOLF4SDL)/.patched: $(PATCHES) | $(GAME)
	rm -rf $(WOLF4SDL) || (sleep 2 && rm -rf $(WOLF4SDL))
	mkdir -p $(WOLF4SDL)
	cp -r $(WOLF4SDL_SRC)/*.c $(WOLF4SDL_SRC)/*.h $(WOLF4SDL_SRC)/*.inc $(WOLF4SDL_SRC)/mame $(WOLF4SDL)/
	for p in $(PATCHES); do patch -s -d $(WOLF4SDL) -p1 < $$p || exit 1; done
	touch $@

$(GAME):
	mkdir -p $(GAME)

# Everything the sysop needs in one folder: the door, the game and its data. On the BBS these two files have usually
# come along with the folder already, and there is no ../module to copy from, so this only fills in what's missing.
install: wolf3ddoor
	@if [ -f wolf3d.wasm ]; then echo "wolf3d.wasm: already here"; \
	elif [ -f ../module/wolf3d.wasm ]; then cp ../module/wolf3d.wasm . && echo "wolf3d.wasm: copied"; \
	else echo "wolf3d.wasm: MISSING -- copy it in from the development machine"; fi
	@if [ -f wolf3d.pak ]; then echo "wolf3d.pak: already here"; \
	elif [ -d ../data ]; then python3 ../tools/mkpak.py ../data wolf3d.pak; \
	else echo "wolf3d.pak: MISSING -- copy it in from the development machine"; fi
	@mkdir -p saves && echo "saves/: ready"

# Copies the game's sources into native/, so this folder builds on the BBS box on its own (run on the development
# machine before copying the folder over). Only what the build uses.
bundle:
	rm -rf native
	mkdir -p native/module native/Wolf4SDL native/nuked-opl3 native/patches native/trace
	cp -r ../module/src ../module/include native/module/
	cp -r ../third_party/Wolf4SDL/*.c ../third_party/Wolf4SDL/*.h ../third_party/Wolf4SDL/*.inc \
	      ../third_party/Wolf4SDL/mame ../third_party/Wolf4SDL/license-gpl.txt ../third_party/Wolf4SDL/README.TXT \
	      native/Wolf4SDL/
	cp ../third_party/nuked-opl3/opl3.c ../third_party/nuked-opl3/opl3.h native/nuked-opl3/
	cp ../patches/*.patch native/patches/
	cp ../../../TERMinator-Windows/trace/include/trace_api.h native/trace/
	@du -sh native

clean:
	rm -rf $(DOOR_OBJ) $(GAME) wolf3ddoor

.PHONY: clean install bundle
