/*
 * ansi_pace.h -- how many ANSI frames may be on their way to the caller at once, told by the caller's terminal.
 *
 * Each frame ends with a cursor-position question (ESC [ 6 n); the terminal answers only once it has taken in and drawn
 * everything before it, so an answer means that frame has arrived. The next frame goes only while few enough are
 * still unanswered: a slow link, or a terminal slow to draw, gets fewer frames rather than a queue of old ones.
 * (The JPEG XL mode's window, pix_play.c, for the ANSI mode.)
 */

#ifndef ANSI_PACE_H
#define ANSI_PACE_H

#include <stdbool.h>
#include <stddef.h>

#define ANSI_PACE_QUESTION "\033[6n"

void ansi_pace_reset(void);

/* True when another frame may go now */
bool ansi_pace_open(long now);

/* A frame of this many bytes (its question included) went out */
void ansi_pace_sent(long now, size_t bytes);

/*
 * The caller's bytes: answers are taken out and counted, and the rest -- typing -- is put in out (room for
 * len + ANSI_PACE_HELD bytes), its length returned. An answer split across reads is held until the rest comes;
 * anything held longer than a moment is let go as typing by ansi_pace_stale (an Escape key at the end of a read must
 * still arrive).
 */
#define ANSI_PACE_HELD 16
int ansi_pace_take(const unsigned char *data, int len, unsigned char *out, long now);
int ansi_pace_stale(unsigned char *out, long now);

/* The round trip with no queue in it, in ms, for the stats line (0 before any answer) */
long ansi_pace_rtt(void);

#endif
