# Builds wolf3d.wasm: the whole of Wolfenstein 3D (game, graphics, AdLib music, digitised sound) as a TRACE module
# the door sends to TERMinator.
#
# Wolf4SDL comes from ../third_party unmodified, built for the shareware episode (v1.4: CARMACIZED + UPLOAD). The SDL
# and SDL_mixer calls it makes are answered by include/SDL*.h + src/*.c, and its files by src/file_trace.c. The AdLib
# chip is Nuked OPL3 (src/opl_trace.c explains why neither of Wolf4SDL's own is used). Run in WSL/Linux with wasi-sdk.
# No built-in rules: with the patched-copy rule below they would chain into nonsense (x.d from x.d.o from x.d.c)
MAKEFLAGS += -r
.SUFFIXES:

WASI_SDK ?= $(HOME)/tools/wasi-sdk-34.0-x86_64-linux
CC  = $(WASI_SDK)/bin/clang

# Built from a patched copy (obj/wolf4sdl): ../third_party stays exactly as released, and ../patches holds every
# change made to it (one so far: the itoa/ltoa fix in wl_def.h, see the patch)
WOLF4SDL_SRC = ../third_party/Wolf4SDL
WOLF4SDL     = obj/wolf4sdl
PATCHES      = $(wildcard ../patches/*.patch)
NUKED    = ../third_party/nuked-opl3
# trace_api.h: the client's own tree when building here, or the copy bundled with the published source
TRACE_INCLUDE = $(firstword $(wildcard ../../../TERMinator-Windows/trace/include) trace)

# The edition: Wolfenstein 3D shareware v1.4 (see Wolf4SDL's version.h)
EDITION = -DVERSIONALREADYCHOSEN -DCARMACIZED -DUPLOAD

COMMON = --target=wasm32-wasip1-threads -O2 -pthread -DNDEBUG $(EDITION) \
         -D_WASI_EMULATED_SIGNAL -D_WASI_EMULATED_PROCESS_CLOCKS \
         -include wolftrace_compat.h -I include -I src -I $(TRACE_INCLUDE) -I $(WOLF4SDL) -I $(NUKED)
CFLAGS   = $(COMMON) -std=gnu99 -w
LDFLAGS = -mexec-model=reactor -Wl,--import-memory,--export-memory,--shared-memory,--max-memory=268435456 \
          -Wl,--export=wasi_thread_start -Wl,--strip-all -Wl,-z,stack-size=1048576 \
          -lwasi-emulated-signal -lwasi-emulated-process-clocks

GAME_SRC = id_ca.c id_in.c id_pm.c id_sd.c id_us.c id_vh.c id_vl.c signon.c \
           wl_act1.c wl_act2.c wl_agent.c wl_atmos.c wl_cloudsky.c wl_debug.c wl_draw.c wl_game.c wl_inter.c \
           wl_main.c wl_menu.c wl_parallax.c wl_plane.c wl_play.c wl_scale.c wl_shade.c wl_state.c wl_text.c \
           wl_utils.c
TRACE_SRC = $(notdir $(wildcard src/*.c)) opl3.c

OBJ = $(patsubst %.c, obj/%.o, $(GAME_SRC) $(TRACE_SRC))

wolf3d.wasm: $(OBJ)
	$(CC) $(CFLAGS) $(OBJ) $(LDFLAGS) -o $@

obj/%.o: $(WOLF4SDL)/%.c | obj
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

obj/%.o: src/%.c $(WOLF4SDL)/.patched | obj
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

obj/opl3.o: $(NUKED)/opl3.c | obj
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

# Wolf4SDL's main() runs on the module's game thread (src/wolftrace.c), under another name
obj/wl_main.o: $(WOLF4SDL)/wl_main.c | obj
	$(CC) $(CFLAGS) -Dmain=wolf_main -MMD -MP -c $< -o $@

# The patched copy: made (or remade, when a patch or the source changes) before anything is compiled from it
$(WOLF4SDL)/%.c: $(WOLF4SDL)/.patched ;

# ⚠️ Without this make counts the patched copy's .c files as intermediate (they come from a pattern rule) and deletes
# them at the end of every build, leaving the copy empty but for wl_main.c
.PRECIOUS: $(WOLF4SDL)/%.c

$(WOLF4SDL)/.patched: $(PATCHES) $(wildcard $(WOLF4SDL_SRC)/*.c $(WOLF4SDL_SRC)/*.h) | obj
	@# Dropbox can hold a just-written folder for a moment, so the first rm may fail: try again after a second
	rm -rf $(WOLF4SDL) || (sleep 2 && rm -rf $(WOLF4SDL))
	mkdir -p $(WOLF4SDL)
	cp -r $(WOLF4SDL_SRC)/*.c $(WOLF4SDL_SRC)/*.h $(WOLF4SDL_SRC)/*.inc $(WOLF4SDL_SRC)/mame $(WOLF4SDL)/
	for p in $(PATCHES); do patch -s -d $(WOLF4SDL) -p1 < $$p || exit 1; done
	touch $@

-include $(wildcard obj/*.d)

obj:
	mkdir -p obj

clean:
	rm -rf obj wolf3d.wasm

.PHONY: clean
