/*
 * pix_hooks.h -- Wolfenstein's sound and music calls, caught for the JPEG XL graphics mode. See pix_hooks.c.
 */

#ifndef PIX_HOOKS_H
#define PIX_HOOKS_H

#include <stdbool.h>
#include <stddef.h>

/* The game's wl_def.h packs every struct after it (#pragma pack(1)), and pix_hooks.c includes it: without this the
 * event would be laid out one way where it's written and another where it's read (the crash of 2026-09-27: the music
 * start position came out of the wrong bytes). */
#pragma pack(push, 8)

typedef enum
{
    PIX_DIGI_START,     /* channel, sound (digitised sound number), left, right */
    PIX_DIGI_PAN,       /* channel, left, right: a sound moved while playing */
    PIX_DIGI_STOP,      /* channel (-1: all of them) */
    PIX_ADLIB_START,    /* sound (the game's sound number) */
    PIX_ADLIB_STOP,
    PIX_MUS_PLAY,       /* sound (track number, chunk - STARTMUSIC), start_ms (where in it to start) */
    PIX_MUS_STOP,
} pix_event_kind_t;

typedef struct
{
    pix_event_kind_t kind;
    int  channel, sound;
    int  left, right;   /* 0-255, as Mix_SetPanning has them */
    long start_ms;
} pix_event_t;

#pragma pack(pop)

/* Both sides must agree: a packed layout would put start_ms off its alignment. Checked wherever this is included. */
_Static_assert(offsetof(pix_event_t, start_ms) % _Alignof(long) == 0, "pix_event_t is packed here");

/* While on, the game's sound calls are also turned into events here. Its own mixer runs on regardless (its output is
 * thrown away, as in ANSI mode), so its sound timing stays exactly as the game expects. Off: nothing is queued. */
void pix_hooks_enable(bool on);

/* The next event, oldest first. False when there are none. Called from the door's thread. */
bool pix_hooks_next(pix_event_t *ev);

#endif
