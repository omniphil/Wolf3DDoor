/*
 * pix_play.h -- playing Wolfenstein 3D as JPEG XL pictures with sound, for terminals that speak the CTerm APC graphics and
 * audio commands but not TRACE. See pix_play.c.
 */

#ifndef PIX_PLAY_H
#define PIX_PLAY_H

#include <stdbool.h>

#include "ansi_play.h"

typedef struct
{
    bool blob;              /* pictures can travel inside the command (CTerm 1.329+), not only through the cache */
    bool zoom;              /* the terminal scales pictures up itself (ZX/ZY, CTerm 1.332+) */
    bool keys;              /* the terminal reports key presses and releases (CSI = 1 h) */
    bool sound;             /* it plays sound files (and has Ogg Vorbis and WAV) */
    int  px_w, px_h;        /* the screen in pixels */
} pix_caps_t;

play_result_t pix_play(const pix_caps_t *caps);

#endif
