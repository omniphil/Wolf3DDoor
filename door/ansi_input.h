/*
 * ansi_input.h -- a terminal's keystrokes turned into Wolfenstein's key presses and releases. See ansi_input.c.
 */

#ifndef ANSI_INPUT_H
#define ANSI_INPUT_H

#include <stdbool.h>

/* Keys a terminal sends as escape sequences, numbered above the byte range */
enum
{
    KEY_T_UP = 256, KEY_T_DOWN, KEY_T_LEFT, KEY_T_RIGHT,
    KEY_T_HOME, KEY_T_END, KEY_T_INSERT, KEY_T_DELETE, KEY_T_PGUP, KEY_T_PGDN,
    KEY_T_F1, KEY_T_F2, KEY_T_F3, KEY_T_F4, KEY_T_F5, KEY_T_F6,
    KEY_T_F7, KEY_T_F8, KEY_T_F9, KEY_T_F10, KEY_T_F11, KEY_T_F12,
    KEY_T_ESC,
};

/* Feeds bytes from the caller. Complete keys come out of ansi_input_next(). */
void ansi_input_feed(const unsigned char *data, int len, long now_ms);

/* The next complete key (a byte or KEY_T_*), or -1. A lone ESC is only reported once it's clear it isn't the start
 * of a sequence, so call this every loop even when no bytes arrived. */
int ansi_input_next(long now_ms);

/*
 * Holding keys down. A terminal only says a key was pressed, never that it was let go, so a key is held from its first
 * press until a moment after its last repeat. Holding an arrow therefore walks steadily once the terminal starts
 * repeating it. `menu` says a menu or question is up (keys are tapped as themselves), `typing` that a saved game's
 * name is being typed (letters go as typed text).
 */
void ansi_input_key(int key, bool menu, bool typing, long now_ms);

/* The weapon in hand fires while its key is held (machine gun, chaingun), rather than once per press */
void ansi_input_auto_fire(bool on);

/* R toggles running (Shift held with every move); the door shows it on the controls line */
bool ansi_input_running(void);

/* Lets go of keys whose time is up. Call every loop. */
void ansi_input_release_due(long now_ms);

/* Lets go of everything (when the game is paused by the door, or ends). */
void ansi_input_release_all(void);

#endif
