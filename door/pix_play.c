/*
 * pix_play.c -- playing Wolfenstein 3D as JPEG XL pictures with sound: the mode between TRACE and ANSI. (The DOOM
 * door's pix_play.c, for Wolfenstein: only the game-specific calls differ.)
 *
 * The game runs here on the BBS, as in ANSI mode (ansi_host.c), but the caller sees its real 320x200 picture: each frame
 * goes out as JPEG XL through the CTerm APC picture commands and their terminal scales it up, and its sound plays on
 * their terminal from files kept in its cache (pix_sound.c). Worked out on recorded frames before it was written:
 *
 *   Pictures   Only what changed is sent. The picture is cut into 32x8 tiles; when most of them changed (nearly
 *              always while moving) the whole frame goes, otherwise each run of changed tiles goes as its own small
 *              picture. Standing still that is about 0.2 KB a frame instead of 3-7.
 *   Pacing     A cursor-position request follows every frame, and its answer says the frame has arrived. As many
 *              frames may be on their way at once as the link's speed times its round trip allows: waiting for each
 *              answer before sending the next (as others do) caps the frame rate at one frame per round trip, 7 fps
 *              with a 120 ms ping however fast the line.
 *   Quality    JPEG XL's distance follows the link: sharper while every frame goes with no queue building up,
 *              softer as soon as frames start waiting or the round trip grows.
 *              Tiles that settle after being sent soft are sent once more, sharp, while the link is idle. At most
 *              TARGET_FPS frames go a second; the game's others go unsent, and their share of the link goes on
 *              quality.
 *   Keys       Real presses and releases (CSI = 1 h) where the terminal reports them; otherwise the ANSI mode's
 *              timed holds (ansi_input.c).
 *
 * Set WOLFDOOR_LOG=<file> to have the frame rate, bandwidth, quality and round trip written there every 5 seconds.
 */

#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <time.h>
#include <unistd.h>

#include "ansi_host.h"
#include "ansi_hud.h"
#include "ansi_input.h"
#include "apc.h"
#include "door.h"
#include "files.h"
#include "jxl_enc.h"
#include "pix_hooks.h"
#include "pix_play.h"
#include "pix_sound.h"
#include "trace_wolf.h"

#define W 320
#define H 200
#define TW 32                       /* tiles: 10 across, 25 down (the status bar starts on a tile edge, row 160) */
#define TH 8
#define TX (W / TW)
#define TY (H / TH)

#define MAX_IN_FLIGHT    16         /* frames on their way at once, at most (the window below decides) */
#define MIN_WINDOW       4          /* frames always allowed on their way, so the link's speed can be found */
#define QUEUE_ALLOWED_MS 50         /* how much queueing the window allows on top of the round trip */
#define UPLOAD_GAP_MS    200        /* music sent ahead of need goes at most this often, so it never crowds play */
#define STALL_MS         3000       /* no answer for this long: forget what's outstanding rather than stop */
#define FULL_FRACTION    0.5        /* more than this changed: send the whole frame */
#define SETTLE_FRAMES    5          /* frames a tile must stay unchanged before it's sharpened */
#define SHARP_DISTANCE   0.5f       /* what a settled tile is sharpened to */
#define TARGET_FPS       30         /* frames sent a second at most: the rest of the link goes on quality */
#define MIN_DISTANCE     0.5f
#define MAX_DISTANCE     8.0f
#define START_DISTANCE   3.0f
#define CONTROL_MS       250        /* how often the quality is reconsidered */
#define KEY_QUIT_SC      0x10       /* Q, with Ctrl: back to the BBS at once */

static long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

static bool write_all(const char *data, size_t len)
{
    while (len > 0)
    {
        ssize_t n = write(STDOUT_FILENO, data, len);
        if (n < 0)
        {
            if (errno == EINTR)
                continue;
            if (errno == EAGAIN)
            {
                struct timeval tv = { 0, 10000 };
                fd_set fds;
                FD_ZERO(&fds);
                FD_SET(STDOUT_FILENO, &fds);
                select(STDOUT_FILENO + 1, NULL, &fds, NULL, &tv);
                continue;
            }
            return false;
        }
        data += n;
        len -= (size_t)n;
    }
    return true;
}

/* ---- what's on its way ----
 *
 * The window: how many bytes may be on their way at once. Enough to keep the link busy for one round trip plus a
 * little queue (its speed times QUEUE_ALLOWED_MS), and never less than a few frames, so a fast link is found. Too
 * small and a long ping caps the frame rate however fast the link is; too big and frames queue up and arrive late.
 */

static struct
{
    long   sent[MAX_IN_FLIGHT];
    size_t bytes[MAX_IN_FLIGHT];
    bool   frame[MAX_IN_FLIGHT];        /* a picture, rather than a music upload */
    size_t total;
    int    head, count;
} g_fly;

static long   g_rtt_sum, g_rtt_n;                 /* round trips in this control period */
static long   g_rtt_floor[10];                    /* the shortest round trip in each of the last 10 seconds */
static long   g_rtt_floor_sec = -1;
static size_t g_acked;                            /* bytes answered for in this control period */
static double g_rate;                             /* the link's speed as measured, bytes a millisecond */
static double g_frame_avg = 8000;                 /* a frame's size, averaged */


static void fly_push(long now, size_t bytes, bool frame)
{
    int i = (g_fly.head + g_fly.count) % MAX_IN_FLIGHT;
    g_fly.sent[i] = now;
    g_fly.bytes[i] = bytes;
    g_fly.frame[i] = frame;
    g_fly.total += bytes;
    g_fly.count++;
}

/* Answers still owed for frames that were given up on: when they do come, they belong to nothing in flight now */
static int g_orphans;

static void fly_reset(void)
{
    g_orphans += g_fly.count;
    g_fly.count = 0;
    g_fly.total = 0;
}

/* The link with no queue in it: the shortest round trip of the last 10 seconds */
static long rtt_min(void)
{
    long m = 0;
    for (int i = 0; i < 10; i++)
        if (g_rtt_floor[i] > 0 && (m == 0 || g_rtt_floor[i] < m))
            m = g_rtt_floor[i];
    return m;
}

static void fly_ack(long now)
{
    long rtt, sec = now / 1000;
    if (g_orphans > 0)
    {
        g_orphans--;
        return;
    }
    if (g_fly.count == 0)
        return;
    rtt = now - g_fly.sent[g_fly.head];
    g_acked += g_fly.bytes[g_fly.head];
    g_fly.total -= g_fly.bytes[g_fly.head];
    g_fly.head = (g_fly.head + 1) % MAX_IN_FLIGHT;
    g_fly.count--;
    g_rtt_sum += rtt;
    g_rtt_n++;
    if (sec != g_rtt_floor_sec)
    {
        g_rtt_floor_sec = sec;
        g_rtt_floor[sec % 10] = rtt;
    }
    else if (rtt < g_rtt_floor[sec % 10])
    {
        g_rtt_floor[sec % 10] = rtt;
    }
}

static bool window_open(void)
{
    double window = g_rate * (double)(rtt_min() + QUEUE_ALLOWED_MS);
    if (window < MIN_WINDOW * g_frame_avg)
        window = MIN_WINDOW * g_frame_avg;
    return g_fly.count < MAX_IN_FLIGHT && (double)g_fly.total < window;
}

/* ---- keys ---- */

static const pix_caps_t *g_caps;
static bool g_held[128];
static bool g_quit_asked;

/* Fire is Ctrl, and F or J fire too, as they do in the ANSI mode: a phone's soft keyboard and a terminal's button
 * bar have no Ctrl to hold. The letter still goes to the game as well, so typing a savegame name works. */
#define SC_CTRL 0x1D
#define SC_F    0x21
#define SC_J    0x24
static bool g_fire_down;        /* Ctrl as the game has it */

static void sync_fire(void)
{
    bool want = g_held[SC_CTRL] || g_held[SC_F] || g_held[SC_J];

    if (want != g_fire_down)
    {
        g_fire_down = want;
        ansi_host_key(SC_CTRL, want);
    }
}

/* A Linux key code (what CSI = 1 h reports) as the set-1 scancode the game's input takes (ansi_host_key) */
static int evdev_to_set1(int code)
{
    switch (code)
    {
    case 96:  return 0x1C;      /* keypad Enter */
    case 97:  return 0x1D;      /* right Ctrl */
    case 100: return 0x38;      /* right Alt */
    case 102: return 0x47;      /* Home */
    case 103: return 0x48;      /* Up */
    case 104: return 0x49;      /* Page Up */
    case 105: return 0x4B;      /* Left */
    case 106: return 0x4D;      /* Right */
    case 107: return 0x4F;      /* End */
    case 108: return 0x50;      /* Down */
    case 109: return 0x51;      /* Page Down */
    case 110: return 0x52;      /* Insert */
    case 111: return 0x53;      /* Delete */
    case 119: return 0x45;      /* Pause */
    default:  return code >= 1 && code <= 88 ? code : 0;   /* the main block is the same numbers in both */
    }
}

static void key_report(const char *params, bool down)
{
    /* CSI = a;b;c K (down) or k (up): usually one code, several only when the terminal lets go of everything */
    for (const char *p = params; *p != '\0'; )
    {
        int sc = evdev_to_set1(atoi(p));
        if (sc > 0 && sc < 128 && g_held[sc] != down)
        {
            g_held[sc] = down;
            if (sc != SC_CTRL)
                ansi_host_key(sc, down);
            if (sc == SC_CTRL || sc == SC_F || sc == SC_J)
                sync_fire();
        }
        if (down && sc == KEY_QUIT_SC && g_held[SC_CTRL])
            g_quit_asked = true;
        p = strchr(p, ';');
        if (p == NULL)
            break;
        p++;
    }
}

static void release_all(void)
{
    for (int sc = 1; sc < 128; sc++)
        if (g_held[sc])
        {
            g_held[sc] = false;
            if (sc != SC_CTRL)
                ansi_host_key(sc, false);
        }
    sync_fire();
    ansi_input_release_all();
}

/*
 * The caller's bytes: answers to the door's own questions (cursor position, key reports, sound notices) are taken
 * out here; anything else is typing, for the ANSI mode's key handling. An escape sequence can be split across reads,
 * so it is gathered until complete.
 */
static unsigned char g_seq[64];
static int  g_seq_len;
static long g_seq_at;

static void finish_sequence(long now)
{
    unsigned char final = g_seq[g_seq_len - 1];
    char params[64];
    int n = g_seq_len - 3;

    memcpy(params, g_seq + 2, (size_t)n);
    params[n] = '\0';

    if (final == 'R' && params[0] >= '0' && params[0] <= '9')
        fly_ack(now);                                   /* ESC [ row ; col R: a frame has arrived */
    else if (params[0] == '=' && (final == 'K' || final == 'k'))
        key_report(params + 1, final == 'K');
    else if (params[0] == '=' || params[0] == '<' || params[0] == '?')
        ;                                               /* sound notices and other answers: nothing to do */
    else if (!g_caps->keys)
        ansi_input_feed(g_seq, g_seq_len, now);
    g_seq_len = 0;
}

static void take_input(const unsigned char *data, int len, long now)
{
    for (int i = 0; i < len; i++)
    {
        unsigned char c = data[i];

        if (g_seq_len == 1)
        {
            if (c == '[')
            {
                g_seq[g_seq_len++] = c;
                continue;
            }
            /* ESC and something else: not ours, so the typing handler has both */
            if (!g_caps->keys)
                ansi_input_feed(g_seq, 1, now);
            g_seq_len = 0;
        }
        else if (g_seq_len >= 2)
        {
            if (g_seq_len < (int)sizeof(g_seq))
                g_seq[g_seq_len++] = c;
            if (c >= 0x40 && c <= 0x7E)
                finish_sequence(now);
            else if (g_seq_len == (int)sizeof(g_seq))
                g_seq_len = 0;                          /* garbage: drop it */
            continue;
        }

        if (c == 27)
        {
            g_seq[0] = c;
            g_seq_len = 1;
            g_seq_at = now;
            continue;
        }
        if (g_caps->keys)
            continue;                                   /* keys come as reports; this is just the typed copy */
        if (c == 17)
            g_quit_asked = true;                        /* Ctrl-Q */
        else
            ansi_input_feed(&c, 1, now);
    }
    /* A lone Escape that nothing followed is the Escape key */
    if (g_seq_len == 1 && now - g_seq_at > 50)
    {
        if (!g_caps->keys)
            ansi_input_feed(g_seq, 1, now);
        g_seq_len = 0;
    }
}

/* ---- the picture ---- */

static uint8_t g_rgb[W * H * 3];        /* the newest frame */
static uint8_t g_ref[W * H * 3];        /* what the caller was last sent, as the game drew it */
static bool    g_have_ref;
static uint8_t g_scaled[W * 2 * H * 2 * 3];
static float   g_tile_d[TY][TX];        /* the quality each tile was last sent at */
static int     g_tile_still[TY][TX];    /* frames since each tile last changed */
static int     g_zoom, g_ox, g_oy;      /* how much the terminal scales it up, and where it goes */

static void to_rgb(const uint32_t *bgra, int w, int h)
{
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++)
        {
            uint32_t p = bgra[(y * h / H) * w + (x * w / W)];
            uint8_t *d = g_rgb + (y * W + x) * 3;
            d[0] = (uint8_t)(p >> 16);
            d[1] = (uint8_t)(p >> 8);
            d[2] = (uint8_t)p;
        }
}

static bool tile_changed(int tx, int ty)
{
    for (int y = ty * TH; y < (ty + 1) * TH; y++)
        if (memcmp(g_rgb + (y * W + tx * TW) * 3, g_ref + (y * W + tx * TW) * 3, TW * 3) != 0)
            return true;
    return false;
}

/* One rectangle of the frame as a picture command. Returns the bytes added. */
static size_t draw_rect(outbuf_t *o, int x, int y, int w, int h, float distance)
{
    const uint8_t *src = g_rgb + (y * W + x) * 3;
    int stride = W * 3, zoom = g_zoom;
    const uint8_t *jxl;
    size_t n, before = o->len;
    char head[96];

    /* A terminal that can't scale pictures itself gets them at the size they're shown */
    if (!g_caps->zoom && g_zoom > 1)
    {
        int sw = w * g_zoom;
        for (int yy = 0; yy < h * g_zoom; yy++)
            for (int xx = 0; xx < sw; xx++)
                memcpy(g_scaled + (yy * sw + xx) * 3, src + (yy / g_zoom) * stride + (xx / g_zoom) * 3, 3);
        src = g_scaled;
        stride = sw * 3;
        w *= g_zoom;
        h *= g_zoom;
        x *= g_zoom;
        y *= g_zoom;
        zoom = 1;
    }
    else
    {
        x *= g_zoom;
        y *= g_zoom;
    }

    n = jxl_enc_rgb(src, stride, w, h, distance, &jxl);
    if (n == 0)
        return 0;
    if (zoom > 1)
        snprintf(head, sizeof(head), "DX=%d;DY=%d;ZX=%d;ZY=%d;", g_ox + x, g_oy + y, zoom, zoom);
    else
        snprintf(head, sizeof(head), "DX=%d;DY=%d;", g_ox + x, g_oy + y);

    if (g_caps->blob)
    {
        char cmd[128];
        snprintf(cmd, sizeof(cmd), "C;DrawJXLBlob;%s", head);
        apc_blob(o, cmd, jxl, n);
    }
    else
    {
        /* Older terminals: through the cache, a file overwritten each time */
        apc_blob(o, "C;S;wolfdoor/frame.jxl;", jxl, n);
        apc_cmd(o, "C;DrawJXL;%swolfdoor/frame.jxl", head);
    }
    return o->len - before;
}

/*
 * A set of tiles as picture commands: the whole frame when it's most of it, otherwise each run of tiles along a row
 * of tiles, runs spanning the same columns in the rows below merged into one taller rectangle. Returns false if the
 * set was empty.
 */
static bool draw_tiles(outbuf_t *o, bool tiles[TY][TX], float distance)
{
    int count = 0;

    for (int ty = 0; ty < TY; ty++)
        for (int tx = 0; tx < TX; tx++)
            count += tiles[ty][tx];
    if (count == 0)
        return false;

    if (count > FULL_FRACTION * TX * TY)
    {
        draw_rect(o, 0, 0, W, H, distance);
        for (int ty = 0; ty < TY; ty++)
            for (int tx = 0; tx < TX; tx++)
                g_tile_d[ty][tx] = distance;
        return true;
    }
    for (int ty = 0; ty < TY; ty++)
        for (int tx = 0; tx < TX; )
        {
            int x0, x1, ty1;
            if (!tiles[ty][tx])
            {
                tx++;
                continue;
            }
            x0 = tx;
            while (tx < TX && tiles[ty][tx])
                tx++;
            x1 = tx;
            /* grow downwards while the row below has the same run */
            for (ty1 = ty + 1; ty1 < TY; ty1++)
            {
                bool same = (x0 == 0 || !tiles[ty1][x0 - 1]) && (x1 == TX || !tiles[ty1][x1]);
                for (int k = x0; k < x1 && same; k++)
                    same = tiles[ty1][k];
                if (!same)
                    break;
                for (int k = x0; k < x1; k++)
                    tiles[ty1][k] = false;
            }
            for (int yy = ty; yy < ty1; yy++)
                for (int k = x0; k < x1; k++)
                    g_tile_d[yy][k] = distance;
            draw_rect(o, x0 * TW, ty * TH, (x1 - x0) * TW, (ty1 - ty) * TH, distance);
        }
    return true;
}

/* What changed since the caller's copy, at the current quality. Returns false if nothing did. */
static bool draw_changes(outbuf_t *o, float distance)
{
    bool changed[TY][TX];

    for (int ty = 0; ty < TY; ty++)
        for (int tx = 0; tx < TX; tx++)
        {
            changed[ty][tx] = !g_have_ref || tile_changed(tx, ty);
            g_tile_still[ty][tx] = changed[ty][tx] ? 0 : g_tile_still[ty][tx] + 1;
        }
    memcpy(g_ref, g_rgb, sizeof(g_ref));
    g_have_ref = true;
    return draw_tiles(o, changed, distance);
}

/* Tiles that have stayed put since they were sent soft, sent again sharp: once each, so a picture that has settled
 * sharpens, while a blinking face on the status bar only ever costs its own few tiles */
static bool draw_sharper(outbuf_t *o)
{
    bool soft[TY][TX];

    for (int ty = 0; ty < TY; ty++)
        for (int tx = 0; tx < TX; tx++)
            soft[ty][tx] = g_tile_still[ty][tx] >= SETTLE_FRAMES && g_tile_d[ty][tx] > SHARP_DISTANCE + 0.01f;
    return draw_tiles(o, soft, SHARP_DISTANCE);
}

/* ---- a slower link, for testing ----
 *
 * On a fast local connection everything is easy. To see how the mode plays over the internet, put a file
 * linktest.cfg in a player's own folder (saves/<player>/), for example
 *     kbps=300
 *     ping=80
 * and that player's games (nobody else's) go through a link of that speed, in KB a second, and round trip, in ms:
 * what the door sends is released no faster than kbps and half the ping late, and what the player sends reaches the
 * game half the ping late. The door's pacing and quality then react exactly as they would to a real link.
 */

#define SHAPE_CHUNKS 4096
#define SHAPE_IN     256

static bool   g_shape;
static double g_shape_rate;             /* bytes a millisecond */
static long   g_shape_half_ping;
static double g_shape_free;             /* when the modelled link is next free */
static outbuf_t g_shape_out;            /* bytes waiting for their time */
static size_t g_shape_sent;             /* how many of those have gone */
static struct { long due; size_t end; } g_shape_chunks[SHAPE_CHUNKS];
static int    g_shape_head, g_shape_count;
static struct { long due; int len; unsigned char data[512]; } g_shape_in[SHAPE_IN];
static int    g_in_head, g_in_count;

static void shape_load(FILE *log)
{
    char path[700], line[64];
    int kbps = 0, ping = 0;
    FILE *f;

    snprintf(path, sizeof(path), "%s/linktest.cfg", files_folder());
    f = fopen(path, "r");
    if (f == NULL)
        return;
    while (fgets(line, sizeof(line), f) != NULL)
    {
        sscanf(line, "kbps=%d", &kbps);
        sscanf(line, "ping=%d", &ping);
    }
    fclose(f);
    if (kbps <= 0)
        return;
    g_shape = true;
    g_shape_rate = kbps * 1024.0 / 1000.0;
    g_shape_half_ping = ping / 2;
    if (log != NULL)
        fprintf(log, "jxl: link test: %d KB/s, %d ms ping\n", kbps, ping);
}

static void shape_out(const char *data, size_t len, long now)
{
    int i;
    if (g_shape_count == SHAPE_CHUNKS)
        return;                         /* can't happen at these rates; the frame is just lost */
    if (g_shape_free < now)
        g_shape_free = now;
    g_shape_free += (double)len / g_shape_rate;
    out_bytes(&g_shape_out, data, len);
    i = (g_shape_head + g_shape_count) % SHAPE_CHUNKS;
    g_shape_chunks[i].due = (long)g_shape_free + g_shape_half_ping;
    g_shape_chunks[i].end = g_shape_out.len;
    g_shape_count++;
}

/* Sends what's due (everything, when all is true). False if the caller has gone. */
static bool shape_flush(long now, bool all)
{
    while (g_shape_count > 0 && (all || g_shape_chunks[g_shape_head].due <= now))
    {
        size_t end = g_shape_chunks[g_shape_head].end;
        if (!write_all(g_shape_out.data + g_shape_sent, end - g_shape_sent))
            return false;
        g_shape_sent = end;
        g_shape_head = (g_shape_head + 1) % SHAPE_CHUNKS;
        g_shape_count--;
    }
    if (g_shape_count == 0)
        g_shape_out.len = g_shape_sent = 0;
    return true;
}

static void shape_in(const unsigned char *data, int len, long now)
{
    int i;
    if (g_in_count == SHAPE_IN)
        return;
    i = (g_in_head + g_in_count) % SHAPE_IN;
    g_shape_in[i].due = now + g_shape_half_ping;
    g_shape_in[i].len = len;
    memcpy(g_shape_in[i].data, data, (size_t)len);
    g_in_count++;
}

static void shape_in_release(long now)
{
    while (g_in_count > 0 && g_shape_in[g_in_head].due <= now)
    {
        take_input(g_shape_in[g_in_head].data, g_shape_in[g_in_head].len, now);
        g_in_head = (g_in_head + 1) % SHAPE_IN;
        g_in_count--;
    }
}

/* ---- if the door itself falls over ----
 *
 * The caller's terminal is left in this mode's state otherwise: key reports on and typed keys held back (so the BBS
 * seems not to answer the keyboard at all), and seconds of sound still queued (2026-09-27). So on a crash the terminal
 * is put back first -- one prepared string, all a signal handler may safely do -- and then the crash goes on as it
 * would have.
 */

#define STOP_SOUND(ch) APC_PREFIX "A;Flush;C=" #ch APC_END
static const char CRASH_RESET[] = "\033[=2l\033[=1l"
    STOP_SOUND(2) STOP_SOUND(3) STOP_SOUND(4) STOP_SOUND(5) STOP_SOUND(6) STOP_SOUND(7) STOP_SOUND(8)
    STOP_SOUND(9) STOP_SOUND(10) STOP_SOUND(11) STOP_SOUND(12) STOP_SOUND(13) STOP_SOUND(14)
    "\033[0m\033[2J\033[H\033[?25h";
static const int CRASH_SIGNALS[] = { SIGSEGV, SIGBUS, SIGFPE, SIGILL, SIGABRT };

static void crash_reset(int sig)
{
    ssize_t n = write(STDOUT_FILENO, CRASH_RESET, sizeof(CRASH_RESET) - 1);
    (void)n;
    signal(sig, SIG_DFL);
    raise(sig);
}

static void crash_guard(bool on)
{
    for (size_t i = 0; i < sizeof(CRASH_SIGNALS) / sizeof(CRASH_SIGNALS[0]); i++)
        signal(CRASH_SIGNALS[i], on ? crash_reset : SIG_DFL);
}

/* ---- the game ---- */

play_result_t pix_play(const pix_caps_t *caps)
{
    uint32_t *frame = NULL;
    int frame_w = 0, frame_h = 0;
    unsigned frame_seq = 0, sent_seq = 0, skipped_seq = 0;
    float distance = START_DISTANCE;
    long control_at, stats_at, last_ack_check, upload_at = 0;
    long frames = 0, bytes = 0, skipped = 0, period_frames = 0, period_skipped = 0;
    long next_frame_at = 0;
    play_result_t result = PLAY_QUIT;
    outbuf_t out = { 0 };
    FILE *log = NULL;
    const char *log_path = getenv("WOLFDOOR_LOG");
    ansi_hud_t hud;

    g_caps = caps;
    if (log_path != NULL && *log_path)
        log = fopen(log_path, "a");
    else
    {
        /* Otherwise the player's own folder keeps their last game's numbers, for looking into how it went */
        char path[700];
        snprintf(path, sizeof(path), "%s/jxl.log", files_folder());
        log = fopen(path, "w");
    }
    shape_load(log);

    /* The biggest whole scale that fits, centred */
    g_zoom = caps->px_w / W < caps->px_h / H ? caps->px_w / W : caps->px_h / H;
    if (g_zoom < 1)
        g_zoom = 1;
    if (g_zoom > 2 && !caps->zoom)
        g_zoom = 2;                     /* sending them pre-scaled beyond 2x costs too much */
    g_ox = (caps->px_w - W * g_zoom) / 2;
    g_oy = (caps->px_h - H * g_zoom) / 2;
    if (g_ox < 0) g_ox = 0;
    if (g_oy < 0) g_oy = 0;
    if (log != NULL)
        fprintf(log, "jxl: screen %dx%d; frames %dx%d, %s at %dx; key reports %s, sound %s, pictures %s\n",
                caps->px_w, caps->px_h, W * (caps->zoom ? 1 : g_zoom), H * (caps->zoom ? 1 : g_zoom),
                caps->zoom ? "scaled by the terminal" : "pre-scaled here", g_zoom, caps->keys ? "on" : "off",
                caps->sound ? "on" : "off", caps->blob ? "in the command" : "through the cache");

    /* Colours reset, screen cleared, cursor hidden; key reports on (and the typed copies of keys off) */
    out_str(&out, "\033[0m\033[2J\033[H\033[?25l");
    if (caps->keys)
        out_str(&out, "\033[=1h\033[=2h");
    if (caps->sound)
        pix_sound_start(&out);
    write_all(out.data, out.len);

    crash_guard(true);
    pix_hooks_enable(caps->sound);
    /* The player's own settings (TRACE's, not ANSI mode's whole-screen, silent ones), with the game's status bar and
     * sound; keys held as TRACE holds them where the terminal reports them (ansi_host.c) */
    files_use_ansi_config(false);
    ansi_host_set_pixel_mode(true, caps->keys);
    if (!ansi_host_start(trace_wolf_pak_data(), trace_wolf_pak_size(), trace_wolf_pak_hash()))
    {
        door_write("\033[=2l\033[=1l\033[0m\033[?25h");
        crash_guard(false);
        free(out.data);
        return PLAY_FAILED;
    }

    control_at = stats_at = last_ack_check = now_ms();
    for (;;)
    {
        long now;
        int key;

        /* Input, waiting a couple of milliseconds for it, which is also what paces this loop */
        {
            struct timeval tv = { 0, 2000 };
            fd_set fds;
            FD_ZERO(&fds);
            FD_SET(STDIN_FILENO, &fds);
            if (select(STDIN_FILENO + 1, &fds, NULL, NULL, &tv) > 0)
            {
                unsigned char buf[512];
                ssize_t n = read(STDIN_FILENO, buf, sizeof(buf));
                if (n == 0 || (n < 0 && errno != EINTR && errno != EAGAIN))
                {
                    result = PLAY_HANGUP;
                    break;
                }
                if (n > 0 && g_shape)
                    shape_in(buf, (int)n, now_ms());
                else if (n > 0)
                    take_input(buf, (int)n, now_ms());
            }
            else
            {
                take_input(NULL, 0, now_ms());
            }
        }
        now = now_ms();
        if (g_shape)
        {
            shape_in_release(now);
            if (!shape_flush(now, false))
            {
                result = PLAY_HANGUP;
                break;
            }
        }

        if (!caps->keys)
        {
            ansi_hud_read(&hud);
            ansi_input_auto_fire(hud.weapon >= 2);
            while ((key = ansi_input_next(now)) >= 0)
            {
                if (key == 17)
                    g_quit_asked = true;
                else
                    ansi_input_key(key, hud.menu, hud.typing, now);
            }
            ansi_input_release_due(now);
        }
        if (g_quit_asked)
        {
            result = PLAY_LEFT;
            break;
        }
        if (ansi_host_finished())
            break;
        if (door_time_remaining() <= 0)
        {
            result = PLAY_LEFT;
            break;
        }

        /* An answer that never came (a terminal that dropped a question) mustn't stop the picture for good */
        if (g_fly.count > 0 && now - g_fly.sent[g_fly.head] > STALL_MS)
            fly_reset();

        out.len = 0;
        if (caps->sound)
            pix_sound_update(&out, now);

        /* The newest frame, TARGET_FPS times a second, when the link has room for it */
        ansi_host_frame(&frame, &frame_w, &frame_h, &frame_seq);
        if (frame_seq != sent_seq && now >= next_frame_at)
        {
            next_frame_at += 1000 / TARGET_FPS;
            if (next_frame_at < now)
                next_frame_at = now;
            if (window_open())
            {
                size_t before = out.len;
                bool sent;

                to_rgb(frame, frame_w, frame_h);
                sent_seq = frame_seq;
                sent = draw_changes(&out, distance);
                /* and, while the link is idle, the settled parts sharper */
                if (g_fly.count == 0 && period_skipped == 0 && distance > SHARP_DISTANCE)
                    sent |= draw_sharper(&out);
                if (sent)
                {
                    out_str(&out, "\033[6n");
                    g_frame_avg = g_frame_avg * 0.9 + (double)(out.len - before) * 0.1;
                    fly_push(now, out.len - before, true);
                    frames++;
                    period_frames++;
                }
            }
            else if (frame_seq != sent_seq && frame_seq != skipped_seq)
            {
                /* counted once per frame the game drew and the link had no room for */
                skipped_seq = frame_seq;
                skipped++;
                period_skipped++;
            }
        }

        /* Music pieces: one that's needed soon goes now; others only while the link has room to spare */
        if (caps->sound && g_fly.count < MAX_IN_FLIGHT)
        {
            int want = pix_sound_upload_wanted(now);
            if (want == 2 || (want == 1 && g_fly.count == 0 && distance <= MIN_DISTANCE && period_skipped == 0 &&
                              now - upload_at >= UPLOAD_GAP_MS))
            {
                size_t n = pix_sound_upload(&out, now);
                out_str(&out, "\033[6n");
                fly_push(now, n, false);
                upload_at = now;
            }
        }

        if (out.len > 0)
        {
            if (g_shape)
            {
                shape_out(out.data, out.len, now);
            }
            else if (!write_all(out.data, out.len))
            {
                result = PLAY_HANGUP;
                break;
            }
            bytes += (long)out.len;
        }

        /*
         * The quality: softer when frames had to be skipped for want of room, or answers are coming back well behind
         * the link's own round trip (a queue is building); sharper when every frame goes and there's no queue.
         */
        if (now - control_at >= CONTROL_MS)
        {
            long rtt = g_rtt_n > 0 ? g_rtt_sum / g_rtt_n : 0;
            long queue = g_rtt_n > 0 ? rtt - rtt_min() : 0;
            double sample = (double)g_acked / (double)(now - control_at);

            /* The link's speed: what was answered for, trusted fully when it's more than before (the link was
             * busier) and only slowly when less (maybe there was just less to send) */
            g_rate = sample > g_rate ? sample : g_rate * 0.95 + sample * 0.05;
            g_acked = 0;
            bool behind = period_skipped > period_frames / 6 || queue > 80;
            bool keeping_up = period_skipped <= 1 && queue < 40;

            if (behind)
                distance *= 1.2f;
            else if (keeping_up)
                distance *= 0.93f;
            if (distance < MIN_DISTANCE) distance = MIN_DISTANCE;
            if (distance > MAX_DISTANCE) distance = MAX_DISTANCE;
            g_rtt_sum = g_rtt_n = 0;
            period_frames = period_skipped = 0;
            control_at = now;
        }

        if (log != NULL && now - stats_at >= 5000)
        {
            long ms = now - stats_at;
            fprintf(log, "jxl: %ld fps, %ld KB/s, distance %.2f, rtt floor %ld ms, link %.0f KB/s, skipped %ld\n",
                    frames * 1000 / ms, bytes * 1000 / ms / 1024, distance, rtt_min(), g_rate * 1000 / 1024, skipped);
            fflush(log);
            frames = bytes = skipped = 0;
            stats_at = now;
        }
    }

    release_all();
    pix_hooks_enable(false);
    ansi_host_set_pixel_mode(false, false);
    if (g_shape)
        shape_flush(now_ms(), true);
    out.len = 0;
    if (caps->sound)
        pix_sound_stop(&out);
    if (caps->keys)
        out_str(&out, "\033[=2l\033[=1l");
    out_str(&out, "\033[0m\033[2J\033[H\033[?25h");
    write_all(out.data, out.len);
    crash_guard(false);
    free(out.data);
    free(frame);
    if (log != NULL)
        fclose(log);
    return result;
}
