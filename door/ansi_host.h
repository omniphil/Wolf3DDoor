/*
 * ansi_host.h -- runs the Wolfenstein 3D module here on the BBS, for players without TRACE. See ansi_host.c.
 */

#ifndef ANSI_HOST_H
#define ANSI_HOST_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* The JPEG XL mode (pix_play.c): the game's own view, status bar and sound instead of ANSI mode's, and keys held as
 * TRACE holds them when the terminal reports presses and releases (keys). Call before ansi_host_start. */
void ansi_host_set_pixel_mode(bool on, bool keys);

/* Starts the game on its own thread, with the data the door loaded and this player's saves. */
bool ansi_host_start(const unsigned char *pak, size_t pak_size, const char *pak_hash);

/* Copies the newest frame into *pixels (reallocated to fit) when there is one newer than *seq. BGRA, width x height,
 * shown at 4:3. Returns false when nothing new has been drawn. */
bool ansi_host_frame(uint32_t **pixels, int *width, int *height, unsigned *seq);

/* A key going down or up, as a set-1 scancode (the arrows and the block above them are sent as E0 keys). */
void ansi_host_key(int scancode, bool down);

/* A typed character, for naming a saved game */
void ansi_host_text(int ch);

/* True once the game has quit (the player chose Quit, or a fatal error). */
bool ansi_host_finished(void);

#endif
