/*
 * pix_sound.h -- Wolfenstein's sound effects and music on the caller's own terminal, for the JPEG XL graphics mode. See
 * pix_sound.c.
 */

#ifndef PIX_SOUND_H
#define PIX_SOUND_H

#include <stdbool.h>

#include "apc.h"

/* Before the game: builds the sound files, asks the terminal which it already has, and uploads the sound effects
 * it's missing (music waits until it's wanted). progress gets 0-100 while uploading. False if sound is unusable. */
bool pix_sound_prepare(void (*progress)(int percent));

/* At the start of play: loads the effects into the terminal's sound slots */
void pix_sound_start(outbuf_t *o);

/* Turns the game's sound events into commands, and keeps the music queued ahead */
void pix_sound_update(outbuf_t *o, long now_ms);

/* Music pieces not yet on the terminal. 2 = one is needed within a few seconds, 1 = one will be needed later
 * (worth sending while the link has room to spare), 0 = nothing to send. */
int  pix_sound_upload_wanted(long now_ms);

/* Adds the next wanted upload. Returns its size in bytes. */
size_t pix_sound_upload(outbuf_t *o, long now_ms);

/* At the end: silences everything */
void pix_sound_stop(outbuf_t *o);

#endif
