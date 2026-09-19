/*
 * ansi_screen.h -- the caller's screen as a grid of cells, and the ANSI that brings it up to date. See ansi_screen.c.
 */

#ifndef ANSI_SCREEN_H
#define ANSI_SCREEN_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SCREEN_COLS 80
#define SCREEN_ROWS 24

typedef enum
{
    MODE_16,        /* 16 colours: CP437 blocks and shades, best fit per cell. Works everywhere */
    MODE_256,       /* half-blocks in the xterm 256-colour palette */
    MODE_24BIT,     /* half-blocks in 24-bit colour */
} ansi_mode_t;

/* The classic 16 colours, in PC (VGA) order, for text drawn with ansi_screen_text */
enum { C_BLACK, C_BLUE, C_GREEN, C_CYAN, C_RED, C_MAGENTA, C_BROWN, C_GREY,
       C_DARKGREY, C_LBLUE, C_LGREEN, C_LCYAN, C_LRED, C_LMAGENTA, C_YELLOW, C_WHITE };

void ansi_screen_init(ansi_mode_t mode, bool utf8);

/* Draws a frame (BGRA, shown at 4:3) into rows first_row..last_row (1-based), all 80 columns. */
void ansi_screen_picture(const uint32_t *bgra, int width, int height, int first_row, int last_row);

/* Text in two of the 16 colours at row, col (1-based); stops at the edge of the screen. CP437 bytes. */
void ansi_screen_text(int row, int col, const char *text, int fg, int bg);

/* Fills from row, col to the end of that row with blanks in bg. */
void ansi_screen_clear_to_eol(int row, int col, int bg);

/* The ANSI that turns what the caller has into what was drawn since, in *out (valid until the next call).
 * Only changed cells are sent. Returns its length (0 when nothing changed). */
size_t ansi_screen_update(const char **out);

/* Forget what the caller's screen shows, so the next update redraws all of it (after a screen was drawn over). */
void ansi_screen_invalidate(void);

#endif
