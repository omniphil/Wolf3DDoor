/* wolftrace.h -- what the TRACE platform layer's files share with each other (see wolftrace.c). */

#ifndef WOLFTRACE_H
#define WOLFTRACE_H

#include <stddef.h>
#include <stdint.h>

/* One event from TERMinator (engine contract TE_IN_*), queued for the game thread */
typedef struct
{
    int32_t type, flags, a, b, c;
} wolftrace_event_t;

#define TE_IN_KEY   1   /* flags bit0 pressed, bit1 extended (E0); a = set-1 scancode */
#define TE_IN_TEXT  2   /* a = UTF-16 code unit of a typed character */
#define TE_IN_FOCUS 5   /* flags bit0 focused */
#define TE_IN_QUIT  6   /* the player closed the picture, or the door went away */

void wolftrace_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* Events, oldest first; 0 when there are none. peek leaves it queued. */
int  wolftrace_next_event(wolftrace_event_t *out);
int  wolftrace_event_waiting(void);

/* The game's data: one packed asset the door sent (tools/mkpak.py). NULL when there is no such file. */
const uint8_t *wolftrace_data_file(const char *name, size_t *size);

/* The player's files (config and saves), which live on the BBS (file_trace.c) */
void wolftrace_user_file_received(const char *name, size_t off, size_t total, const uint8_t *data, size_t len);
const uint8_t *wolftrace_user_file(const char *name, size_t *size);
void wolftrace_user_file_written(const char *name, const uint8_t *data, size_t size);
int  wolftrace_user_files_pump(void);   /* sends what it can; returns 1 while anything is still waiting to go */

/* Everything that has to happen regularly on the game thread: sound, and saves going up to the BBS. Called from
 * SDL_Delay, SDL_PollEvent and every frame, so it runs whatever the game is waiting for. */
void wolftrace_pump(void);
void wolftrace_pump_audio(void);

void wolftrace_exit(int code) __attribute__((noreturn));

#endif
