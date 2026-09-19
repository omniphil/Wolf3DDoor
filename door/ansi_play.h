/*
 * ansi_play.h -- playing Wolfenstein 3D in ANSI, for callers without TRACE. See ansi_play.c.
 */

#ifndef ANSI_PLAY_H
#define ANSI_PLAY_H

#include <stdbool.h>

#include "ansi_screen.h"

typedef enum
{
    PLAY_QUIT,          /* the player quit the game */
    PLAY_LEFT,          /* the player left with Ctrl-Q, or their time ran out */
    PLAY_HANGUP,        /* the caller went away */
    PLAY_FAILED,        /* the game couldn't start */
} play_result_t;

/* Runs the game here and shows it on the caller's 80x24 screen until they quit. */
play_result_t ansi_play(ansi_mode_t mode, bool utf8);

#endif
