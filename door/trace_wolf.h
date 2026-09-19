/* trace_wolf.h -- sending Wolfenstein 3D to the player's terminal and starting it; see trace_wolf.c. */

#ifndef TRACE_WOLF_H
#define TRACE_WOLF_H

#include <stdbool.h>
#include <stddef.h>

/* Reads wolf3d.wasm and wolf3d.pak from beside the door binary. False means the door isn't installed properly. */
bool trace_wolf_load_files(void);

/* The game data, for the ANSI mode (which runs the game here instead of sending it) */
const unsigned char *trace_wolf_pak_data(void);
size_t trace_wolf_pak_size(void);
const char *trace_wolf_pak_hash(void);

/* Does this terminal have TRACE, with everything the game needs (its own module, assets, sound, sending back)? */
bool trace_wolf_detect(void);

/* Makes sure the terminal has the game data, uploading it if this is the player's first game. */
bool trace_wolf_send_data(void (*progress)(int));

/* Starts the game on the player's machine (uploading it the first time), with the player's own files. */
bool trace_wolf_open(void);

/* Waits until the player quits, or their time runs out, keeping the files the game sends back. */
void trace_wolf_wait(void);

/* Sends a message to the game: a line of text, and optionally a payload after it. */
void trace_wolf_send(const char *head, const void *payload, size_t len);

/* A message from the game (a piece of a saved file), from TRACE or from the ANSI mode's own copy of the game */
void trace_wolf_module_message(const unsigned char *data, size_t len);

/* Stops the game if it's still running. */
void trace_wolf_close(void);

#endif
