/*
 * wolftrace.c -- the TRACE side of the Wolfenstein 3D module: what TERMinator talks to.
 *
 * The whole game runs here, in the sandbox on the player's PC: Wolf4SDL from ../third_party, unmodified, built with
 * its GPL sound chip (USE_GPL), playing the shareware episode 1 data (v1.4, *.wl1). The door sends this module and
 * the data once; after that almost nothing crosses the wire. Same shape as the Tyrian module (BBSGames/Tyrian).
 *
 *   - The door sends the player's config and saves (file ...), then "pak=<sha256>", the packed game data. That
 *     starts the game.
 *   - Wolf4SDL runs on its own thread, exactly as it always does: its main() never returns until the player quits,
 *     and it waits with SDL_Delay. The game thread presents frames and mixes sound itself (sdl_trace.c,
 *     mixer_trace.c).
 *   - Keyboard events arrive on TERMinator's thread and are queued here for the game thread to read.
 */

#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "trace_api.h"
#include "wolftrace.h"

#undef exit     /* this file is where exit() is answered, not redirected */

extern int wolf_main(int argc, char *argv[]);   /* Wolf4SDL's main(), renamed by the Makefile */
extern void WriteConfig(void);

/* ---- what the door told us ---- */
static char      g_pak_hash[65];
static uint8_t  *g_pak;
static size_t    g_pak_size;
static int       g_started;
static int       g_width = 640, g_height = 400;   /* Wolf4SDL draws at any multiple of 320x200 */
static pthread_t g_game_thread;

/* ---- events from TERMinator, drained by the game thread ---- */
#define EVENT_MAX 256
static wolftrace_event_t g_events[EVENT_MAX];
static int               g_event_head, g_event_tail;
static pthread_mutex_t   g_event_lock = PTHREAD_MUTEX_INITIALIZER;

void wolftrace_log(const char *fmt, ...)
{
    char text[512];
    va_list args;
    va_start(args, fmt);
    vsnprintf(text, sizeof(text), fmt, args);
    va_end(args);
    trace_log(text, (int32_t)strlen(text));
}

int wolftrace_next_event(wolftrace_event_t *out)
{
    int got = 0;
    pthread_mutex_lock(&g_event_lock);
    if (g_event_head != g_event_tail)
    {
        *out = g_events[g_event_head];
        g_event_head = (g_event_head + 1) % EVENT_MAX;
        got = 1;
    }
    pthread_mutex_unlock(&g_event_lock);
    return got;
}

int wolftrace_event_waiting(void)
{
    int waiting;
    pthread_mutex_lock(&g_event_lock);
    waiting = g_event_head != g_event_tail;
    pthread_mutex_unlock(&g_event_lock);
    return waiting;
}

static void queue_event(const wolftrace_event_t *ev)
{
    int next;
    pthread_mutex_lock(&g_event_lock);
    next = (g_event_tail + 1) % EVENT_MAX;
    if (next != g_event_head)       /* full: drop the newest rather than block the terminal */
    {
        g_events[g_event_tail] = *ev;
        g_event_tail = next;
    }
    pthread_mutex_unlock(&g_event_lock);
}

void wolftrace_pump(void)
{
    wolftrace_pump_audio();
    wolftrace_user_files_pump();
}

static void sleep_ms(int ms)
{
    struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

/*
 * Wolf4SDL quits by calling exit(): from Quit(), which has already written config.wl1 on a normal quit, or on a
 * fatal error. Ending this thread isn't enough: the sandbox would keep running and TERMinator would be left showing
 * the last frame, so it's told to stop, which closes the picture and tells the door. Before that, everything still
 * on its way up to the BBS is given a few seconds to get there.
 */
void wolftrace_exit(int code)
{
    static int exiting;

    if (!exiting)
    {
        exiting = 1;
        if (g_started)
            for (int waited = 0; wolftrace_user_files_pump() && waited < 5000; waited += 10)
                sleep_ms(10);
    }
    wolftrace_log("wolf3d: quitting (%d)", code);
    trace_quit(code >= 0 && code < 126 ? code : 0);
    _Exit(0);   /* not reached */
}

/* ---- the game data: one file, packed by tools/mkpak.py ----
 *
 *   "W3DPAK1\0", u32 count, then count entries of { char name[24]; u32 offset; u32 size }, then the files.
 *   Names are lower case. All little-endian.
 */
typedef struct { char name[24]; uint32_t offset, size; } pak_entry_t;
static const pak_entry_t *g_pak_entries;
static uint32_t           g_pak_count;

static int load_pak(void)
{
    int32_t size = trace_asset_size(g_pak_hash);
    uint32_t count;

    if (size < 12)
    {
        wolftrace_log("wolf3d: the door's game data isn't here (%s)", g_pak_hash);
        return 0;
    }
    g_pak = (uint8_t *)malloc((size_t)size);
    if (!g_pak)
        return 0;
    for (int32_t off = 0; off < size; )
    {
        int32_t got = trace_asset_read(g_pak_hash, off, g_pak + off, size - off);
        if (got <= 0)
        {
            wolftrace_log("wolf3d: couldn't read the game data at %d", off);
            return 0;
        }
        off += got;
    }
    g_pak_size = (size_t)size;

    memcpy(&count, g_pak + 8, 4);
    if (memcmp(g_pak, "W3DPAK1", 8) != 0 || 12 + (size_t)count * sizeof(pak_entry_t) > g_pak_size)
    {
        wolftrace_log("wolf3d: the game data isn't a Wolfenstein 3D pack");
        return 0;
    }
    g_pak_entries = (const pak_entry_t *)(g_pak + 12);
    g_pak_count = count;
    for (uint32_t i = 0; i < count; i++)
        if ((size_t)g_pak_entries[i].offset + g_pak_entries[i].size > g_pak_size)
        {
            wolftrace_log("wolf3d: the game data is damaged (%.24s)", g_pak_entries[i].name);
            return 0;
        }
    wolftrace_log("wolf3d: %u data files, %zu bytes", count, g_pak_size);
    return 1;
}

const uint8_t *wolftrace_data_file(const char *name, size_t *size)
{
    for (uint32_t i = 0; i < g_pak_count; i++)
        if (strncmp(g_pak_entries[i].name, name, sizeof(g_pak_entries[i].name)) == 0)
        {
            *size = g_pak_entries[i].size;
            return g_pak + g_pak_entries[i].offset;
        }
    return NULL;
}

/* ---- the game thread ---- */

static void *game_thread(void *arg)
{
    static char w[8], h[8];
    static char *args[] = { "wolf4sdl", "--res", w, h, "--samplerate", "44100", "--configdir", "user", NULL };
    (void)arg;
    snprintf(w, sizeof(w), "%d", g_width);
    snprintf(h, sizeof(h), "%d", g_height);
    wolftrace_exit(wolf_main(8, args));
    return NULL;
}

static void start_game(void)
{
    pthread_attr_t attr;

    if (g_started || !g_pak_hash[0])
        return;
    if (!load_pak())
        return;
    g_started = 1;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 4 * 1024 * 1024);
    if (pthread_create(&g_game_thread, &attr, game_thread, NULL) != 0)
        wolftrace_log("wolf3d: couldn't start the game thread");
    pthread_attr_destroy(&attr);
}

/* ---- TRACE entry points ---- */

int32_t trace_init(void)
{
    trace_set_tick(0);      /* nothing happens until the door says which data to use */
    return 0;
}

void *trace_alloc(int32_t size)
{
    return size > 0 ? malloc((size_t)size) : NULL;
}

void trace_free(void *ptr)
{
    free(ptr);
}

void trace_on_resize(int32_t width, int32_t height)
{
    (void)width; (void)height;   /* the game draws at its own size and TERMinator scales it */
}

static long field(const char *head, const char *name)
{
    const char *p = strstr(head, name);
    return p != NULL ? strtol(p + strlen(name), NULL, 10) : 0;
}

/*
 * What the door sends. Everything up to the first newline is the message; anything after it is its payload.
 *   file name=<n> off=<o> total=<t>\n...   part of one of the player's files (config, saves)
 *   pak=<sha256> [res=<w>x<h>]             the game data, and optionally the size to draw at (a multiple of
 *                                          320x200; 640x400 when left out). Everything the player has was sent
 *                                          before it, so this starts the game
 *   quit                                   the player's time on the BBS is up: save the config, and close
 * A door can add fields of its own without breaking this one.
 */
void trace_on_data(const char *data, int32_t length)
{
    char head[256];
    const char *payload = NULL;
    int32_t payload_len = 0;
    int head_len;
    const char *newline;

    if (length <= 0)
        return;

    newline = (const char *)memchr(data, '\n', (size_t)length);
    head_len = newline != NULL ? (int)(newline - data) : length;
    if (head_len >= (int)sizeof(head))
        head_len = (int)sizeof(head) - 1;
    memcpy(head, data, (size_t)head_len);
    head[head_len] = 0;
    if (newline != NULL)
    {
        payload = newline + 1;
        payload_len = length - (int32_t)(payload - data);
    }

    if (!strncmp(head, "pak=", 4))
    {
        const char *res = strstr(head, "res=");
        int w, h;
        if (res != NULL && sscanf(res, "res=%dx%d", &w, &h) == 2 && w >= 320 && w <= 1280 && w % 320 == 0 &&
            h == w / 320 * 200)
        {
            g_width = w;
            g_height = h;
        }
        if (strspn(head + 4, "0123456789abcdef") == 64 && !g_pak_hash[0])
        {
            memcpy(g_pak_hash, head + 4, 64);
            g_pak_hash[64] = 0;
            start_game();
        }
    }
    else if (!strcmp(head, "quit"))
    {
        wolftrace_event_t ev = { TE_IN_QUIT, 0, 0, 0, 0 };
        if (g_started)
            queue_event(&ev);      /* the game sees its window close: Quit() writes the config on the way out */
        else
            trace_quit(0);
    }
    else if (!strncmp(head, "file ", 5) && payload != NULL)
    {
        char name[32] = "";
        const char *p = strstr(head, "name=");
        if (p != NULL)
            sscanf(p, "name=%31s", name);
        if (name[0])
            wolftrace_user_file_received(name, (size_t)field(head, "off="), (size_t)field(head, "total="),
                                         (const uint8_t *)payload, (size_t)payload_len);
    }
}

void trace_on_input(int32_t type, int32_t flags, int32_t a, int32_t b, int32_t c)
{
    wolftrace_event_t ev = { type, flags, a, b, c };

    /* Closing the picture before the game has started has no one to ask: just go */
    if (type == TE_IN_QUIT && !g_started)
    {
        trace_quit(0);
        return;
    }
    queue_event(&ev);
}

void trace_update(void)
{
    /* The game thread does the work; there is nothing to do here between events. */
}
