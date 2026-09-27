/*
 * pix_sound.c -- Wolfenstein's sound effects and music on the caller's own terminal, for the JPEG XL graphics mode.
 * (The DOOM door's pix_sound.c, for Wolfenstein.)
 *
 * The terminal plays sound files it keeps in a cache of its own, one per BBS, so nothing is streamed as raw audio.
 * Everything comes ready made in door/sound/ (tools/wolfrender.c and tools/make_sound.py, from the game's data):
 *
 *   Effects  the digitised sounds (guns, voices, doors: 8-bit WAV as they are in VSWAP) and every AdLib sound effect
 *            (rendered on the game's own OPL chip, Ogg Vorbis). Uploaded once (checked by md5 on every call) and
 *            loaded into the terminal's sound slots at the start. Playing one is then a few dozen bytes: stop that
 *            channel, set its left/right volume, copy the effect into the channel's own slot, queue it.
 *   Music    each track rendered on the OPL chip and cut into 5-second Ogg Vorbis pieces. A track plays as its pieces
 *            queued back to back on one channel, which the terminal joins seamlessly; each piece is uploaded only
 *            when it is about to be needed, or earlier while the link has room to spare, and stays in the cache.
 *
 * Levels follow the game's own mixer (module/src/mixer_trace.c), so the balance matches TRACE: the AdLib chip's
 * output (music and AdLib effects) is rendered with its gain already in, and the digitised sounds play at DIGI_GAIN,
 * panned as the game pans them (0-255 a side).
 *
 * Terminal resources used: slots 0-63 digitised sounds, 64-150 AdLib effects, 200-207 one per mixer channel, 208 the
 * AdLib effect, 250 music; channel 2 music, 3-10 the game's eight mixer channels, 11 the AdLib effect (the chip plays
 * one effect at a time).
 */

#define _POSIX_C_SOURCE 200809L

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "door.h"
#include "pix_hooks.h"
#include "pix_sound.h"
#include "trace_door.h"

#define SFX_DIR      "wolfdoor/sfx/"
#define MUS_DIR      "wolfdoor/mus/"
#define MAX_DIGI     64
#define MAX_ADLIB    100
#define ADLIB_SLOT0  64
#define MAX_TRACKS   32
#define MAX_PIECES   64
#define PIECE_MS     5000
#define MUSIC_CH     2
#define DIGI_CH0     3
#define DIGI_CHANNELS 8             /* the game's mixer channels (SDL_mixer.h MIX_CHANNELS) */
#define ADLIB_CH     11
#define SCRATCH_SLOT 200            /* + mixer channel */
#define ADLIB_SCRATCH 208
#define MUSIC_SLOT   250
#define DIGI_GAIN    0.35           /* mixer_trace.c */

#define QUEUE_AHEAD_MS  8000        /* music queued this far ahead of what's playing */
#define URGENT_MS       3000        /* a piece needed sooner than this is sent even if the picture has to wait */

typedef struct
{
    bool     have;
    char     file[24];
    uint8_t *data;
    size_t   size;
} sfx_t;

typedef struct
{
    int      number;                /* the game's track number (chunk - STARTMUSIC) */
    int      pieces, last_ms;
    uint8_t *data[MAX_PIECES];
    size_t   size[MAX_PIECES];
    bool     cached[MAX_PIECES];
} track_t;

static sfx_t   g_digi[MAX_DIGI];
static sfx_t   g_adlib[MAX_ADLIB];
static track_t g_tracks[MAX_TRACKS];
static int     g_track_count;
static bool    g_ready;

/* The music as it stands (Wolfenstein's tracks always loop) */
static struct
{
    int  track;                     /* index into g_tracks, -1 none */
    int  next;                      /* the next piece to queue */
    long queued_until;              /* when what's queued runs out */
} M = { .track = -1 };

/* Each mixer channel's panning, as the game last set it */
static int g_left[DIGI_CHANNELS], g_right[DIGI_CHANNELS];

/* ---- the files ---- */

static bool load_file(sfx_t *s, const char *name)
{
    char path[64];
    tdoor_blob_t blob;

    snprintf(path, sizeof(path), "sound/%s", name);
    if (!tdoor_load_blob(path, &blob, 1024 * 1024))
        return false;
    snprintf(s->file, sizeof(s->file), "%s", name);
    s->data = blob.data;
    s->size = blob.size;
    s->have = true;
    return true;
}

static void load_sounds(void)
{
    tdoor_blob_t index;
    char *text, *line, *save = NULL;

    if (!tdoor_load_blob("sound/index.txt", &index, 64 * 1024))
        return;
    text = malloc(index.size + 1);
    if (text == NULL)
        return;
    memcpy(text, index.data, index.size);
    text[index.size] = '\0';

    for (line = strtok_r(text, "\n", &save); line != NULL; line = strtok_r(NULL, "\n", &save))
    {
        char kind[8], name[32];
        int n, pieces, last_ms;

        if (sscanf(line, "%7s %d", kind, &n) != 2)
            continue;
        if (strcmp(kind, "digi") == 0 && n >= 0 && n < MAX_DIGI)
        {
            snprintf(name, sizeof(name), "digi_%02d.wav", n);
            load_file(&g_digi[n], name);
        }
        else if (strcmp(kind, "adlib") == 0 && n >= 0 && n < MAX_ADLIB)
        {
            snprintf(name, sizeof(name), "adlib_%02d.ogg", n);
            load_file(&g_adlib[n], name);
        }
        else if (strcmp(kind, "music") == 0 && g_track_count < MAX_TRACKS &&
                 sscanf(line, "%*s %d %d %d", &n, &pieces, &last_ms) == 3 && pieces > 0 && pieces <= MAX_PIECES)
        {
            track_t *t = &g_tracks[g_track_count];
            bool all = true;

            t->number = n;
            t->pieces = pieces;
            t->last_ms = last_ms;
            for (int k = 0; k < pieces && all; k++)
            {
                char path[64];
                tdoor_blob_t piece;
                snprintf(path, sizeof(path), "sound/music_%02d_%02d.ogg", n, k);
                if (tdoor_load_blob(path, &piece, 1024 * 1024))
                {
                    t->data[k] = piece.data;
                    t->size[k] = piece.size;
                }
                else
                {
                    all = false;
                }
            }
            if (all)
                g_track_count++;
        }
    }
    free(text);
}

/* ---- what the terminal already has ---- */

/* Asks for the cache listing of one folder and reads the reply: lines of name TAB md5, between APC markers.
 * Returns the reply text (static), or NULL if none came. */
static const char *list_cache(const char *dir)
{
    static char reply[65536];
    size_t n = 0;
    int c;

    door_write(APC_PREFIX "C;L;");
    door_write(dir);
    door_write("*" APC_END);

    while (n < sizeof(reply) - 1)
    {
        c = door_read_char_timeout(n == 0 ? 3000 : 1000);
        if (c < 0)
            return NULL;
        reply[n++] = (char)c;
        if (n >= 2 && reply[n - 2] == '\033' && reply[n - 1] == '\\')
            break;
    }
    reply[n] = '\0';
    return strstr(reply, "C;L") != NULL ? reply : NULL;
}

/* Does the listing have this file with this content? */
static bool listed(const char *listing, const char *name, const void *data, size_t size)
{
    char want[128], md5[33];
    md5_hex(data, size, md5);
    snprintf(want, sizeof(want), "\n%s\t%s", name, md5);
    return listing != NULL && strstr(listing, want) != NULL;
}

/*
 * Waits for one answer to a cursor-position request (ESC [ row ; col R), which says everything sent before it has
 * reached the terminal. The upload below keeps a few of these outstanding, so its progress is what has arrived, not
 * what has been handed to the network, and the game doesn't start with the tail of it still queued in front.
 */
static bool wait_arrived(void)
{
    int c;
    while ((c = door_read_char_timeout(30000)) >= 0)
        if (c == 'R')
            return true;
    return false;
}

#define UPLOAD_AHEAD 65536          /* bytes sent before waiting for them to arrive */
#define MAX_UPLOADS  (MAX_DIGI + MAX_ADLIB)

bool pix_sound_prepare(void (*progress)(int percent))
{
    const char *listing;
    sfx_t *all[MAX_UPLOADS];
    size_t queued[MAX_UPLOADS], total = 0, sent = 0, arrived = 0;
    int count = 0, ahead = 0, first = 0;

    load_sounds();
    for (int i = 0; i < MAX_DIGI; i++)
        if (g_digi[i].have)
            all[count++] = &g_digi[i];
    for (int i = 0; i < MAX_ADLIB; i++)
        if (g_adlib[i].have)
            all[count++] = &g_adlib[i];
    if (count == 0)
        return false;

    /* Only what the terminal doesn't have yet, or has in another version */
    listing = list_cache(SFX_DIR);
    for (int i = 0; i < count; i++)
    {
        if (listed(listing, all[i]->file, all[i]->data, all[i]->size))
            all[i] = NULL;
        else
            total += all[i]->size;
    }
    for (int i = 0; i < count; i++)
    {
        outbuf_t o = { 0 };
        char head[64];
        if (all[i] == NULL)
            continue;
        snprintf(head, sizeof(head), "C;S;" SFX_DIR "%s;", all[i]->file);
        apc_blob(&o, head, all[i]->data, all[i]->size);
        out_str(&o, "\033[6n");
        door_write_raw(o.data, o.len);
        free(o.data);
        queued[ahead++] = all[i]->size;
        sent += all[i]->size;
        /* keep no more than UPLOAD_AHEAD on its way */
        while (sent - arrived > UPLOAD_AHEAD && first < ahead && wait_arrived())
        {
            arrived += queued[first++];
            if (progress != NULL)
                progress((int)(arrived * 100 / total));
        }
    }
    while (first < ahead && wait_arrived())
    {
        arrived += queued[first++];
        if (progress != NULL)
            progress((int)(arrived * 100 / total));
    }

    listing = list_cache(MUS_DIR);
    for (int t = 0; t < g_track_count; t++)
        for (int k = 0; k < g_tracks[t].pieces; k++)
        {
            char file[32];
            snprintf(file, sizeof(file), "music_%02d_%02d.ogg", g_tracks[t].number, k);
            g_tracks[t].cached[k] = listed(listing, file, g_tracks[t].data[k], g_tracks[t].size[k]);
        }

    for (int c = 0; c < DIGI_CHANNELS; c++)
        g_left[c] = g_right[c] = 255;
    g_ready = true;
    return true;
}

/* ---- playing ---- */

static int db(double gain)
{
    return gain <= 0.001 ? -60 : (int)lround(20.0 * log10(gain));
}

void pix_sound_start(outbuf_t *o)
{
    if (!g_ready)
        return;
    for (int i = 0; i < MAX_DIGI; i++)
        if (g_digi[i].have)
            apc_cmd(o, "A;Load;S=%d;" SFX_DIR "%s", i, g_digi[i].file);
    for (int i = 0; i < MAX_ADLIB; i++)
        if (g_adlib[i].have)
            apc_cmd(o, "A;Load;S=%d;" SFX_DIR "%s", ADLIB_SLOT0 + i, g_adlib[i].file);
    apc_cmd(o, "A;Volume;C=%d;V=0dB", MUSIC_CH);
    apc_cmd(o, "A;Volume;C=%d;V=0dB", ADLIB_CH);
}

static void digi_volume(outbuf_t *o, int ch)
{
    apc_cmd(o, "A;Volume;C=%d;VL=%ddB;VR=%ddB", DIGI_CH0 + ch, db(DIGI_GAIN * g_left[ch] / 255.0),
            db(DIGI_GAIN * g_right[ch] / 255.0));
}

/* At once, with no fade: a fade (O=) only fades the last piece queued, and the several seconds queued in front of it
 * would play on (after the game ended, or over the next track) */
static void music_stop(outbuf_t *o)
{
    apc_cmd(o, "A;Flush;C=%d", MUSIC_CH);
    M.track = -1;
}

static void handle(outbuf_t *o, const pix_event_t *ev, long now)
{
    int ch = ev->channel;

    switch (ev->kind)
    {
    case PIX_DIGI_START:
        if (ch < 0 || ch >= DIGI_CHANNELS || ev->sound < 0 || ev->sound >= MAX_DIGI || !g_digi[ev->sound].have)
            return;
        g_left[ch] = ev->left;
        g_right[ch] = ev->right;
        apc_cmd(o, "A;Flush;C=%d", DIGI_CH0 + ch);
        digi_volume(o, ch);
        apc_cmd(o, "A;Copy;S=%d;D=%d", ev->sound, SCRATCH_SLOT + ch);
        apc_cmd(o, "A;Queue;C=%d;S=%d", DIGI_CH0 + ch, SCRATCH_SLOT + ch);
        return;
    case PIX_DIGI_PAN:
        /* The game re-pans every sound it's tracking each frame, mostly to where it already is */
        if (ch < 0 || ch >= DIGI_CHANNELS || (g_left[ch] == ev->left && g_right[ch] == ev->right))
            return;
        g_left[ch] = ev->left;
        g_right[ch] = ev->right;
        digi_volume(o, ch);
        return;
    case PIX_DIGI_STOP:
        for (int c = 0; c < DIGI_CHANNELS; c++)
            if (ch < 0 || c == ch)
                apc_cmd(o, "A;Flush;C=%d;O=10", DIGI_CH0 + c);
        return;

    case PIX_ADLIB_START:
        if (ev->sound < 0 || ev->sound >= MAX_ADLIB || !g_adlib[ev->sound].have)
            return;
        apc_cmd(o, "A;Flush;C=%d", ADLIB_CH);
        apc_cmd(o, "A;Copy;S=%d;D=%d", ADLIB_SLOT0 + ev->sound, ADLIB_SCRATCH);
        apc_cmd(o, "A;Queue;C=%d;S=%d", ADLIB_CH, ADLIB_SCRATCH);
        return;
    case PIX_ADLIB_STOP:
        apc_cmd(o, "A;Flush;C=%d;O=10", ADLIB_CH);
        return;

    case PIX_MUS_PLAY:
        music_stop(o);
        for (int t = 0; t < g_track_count; t++)
            if (g_tracks[t].number == ev->sound)
                M.track = t;
        if (M.track >= 0)
        {
            /* Part way in (back from the menu): from the start of the piece that was playing */
            M.next = (int)(ev->start_ms / PIECE_MS);
            if (M.next >= g_tracks[M.track].pieces)
                M.next = 0;
        }
        M.queued_until = now;
        return;
    case PIX_MUS_STOP:
        music_stop(o);
        return;
    }
}

static int piece_ms(const track_t *t, int k)
{
    return k == t->pieces - 1 ? t->last_ms : PIECE_MS;
}

void pix_sound_update(outbuf_t *o, long now)
{
    pix_event_t ev;

    if (!g_ready)
    {
        while (pix_hooks_next(&ev))
            ;
        return;
    }
    while (pix_hooks_next(&ev))
        handle(o, &ev, now);

    /* Keep the music queued ahead, as far as its pieces have reached the terminal. It loops. */
    while (M.track >= 0 && M.queued_until - now < QUEUE_AHEAD_MS)
    {
        track_t *t = &g_tracks[M.track];
        if (!t->cached[M.next])
            break;
        if (M.queued_until < now)
            M.queued_until = now;       /* it ran dry (a piece arrived late): start again from now */
        apc_cmd(o, "A;Load;S=%d;" MUS_DIR "music_%02d_%02d.ogg", MUSIC_SLOT, t->number, M.next);
        apc_cmd(o, "A;Queue;C=%d;S=%d", MUSIC_CH, MUSIC_SLOT);
        M.queued_until += piece_ms(t, M.next);
        M.next = (M.next + 1) % t->pieces;
    }
}

/* The next piece to upload: the one the music will reach first, then the rest of this track, then other tracks */
static bool next_upload(int *track, int *piece, long now, bool *urgent)
{
    if (M.track >= 0)
    {
        track_t *t = &g_tracks[M.track];
        int k = M.next;
        long due = M.queued_until;
        for (int i = 0; i < t->pieces; i++, k = (k + 1) % t->pieces, due += PIECE_MS)
            if (!t->cached[k])
            {
                *track = M.track;
                *piece = k;
                *urgent = due - now < URGENT_MS;
                return true;
            }
    }
    for (int tr = 0; tr < g_track_count; tr++)
        for (int k = 0; k < g_tracks[tr].pieces; k++)
            if (!g_tracks[tr].cached[k])
            {
                *track = tr;
                *piece = k;
                *urgent = false;
                return true;
            }
    return false;
}

int pix_sound_upload_wanted(long now)
{
    int t, k;
    bool urgent;
    if (!g_ready || !next_upload(&t, &k, now, &urgent))
        return 0;
    return urgent ? 2 : 1;
}

size_t pix_sound_upload(outbuf_t *o, long now)
{
    int t, k;
    bool urgent;
    char head[64];
    size_t before = o->len;

    if (!g_ready || !next_upload(&t, &k, now, &urgent))
        return 0;
    snprintf(head, sizeof(head), "C;S;" MUS_DIR "music_%02d_%02d.ogg;", g_tracks[t].number, k);
    apc_blob(o, head, g_tracks[t].data[k], g_tracks[t].size[k]);
    g_tracks[t].cached[k] = true;
    return o->len - before;
}

void pix_sound_stop(outbuf_t *o)
{
    if (!g_ready)
        return;
    music_stop(o);
    for (int c = 0; c < DIGI_CHANNELS; c++)
        apc_cmd(o, "A;Flush;C=%d", DIGI_CH0 + c);
    apc_cmd(o, "A;Flush;C=%d", ADLIB_CH);
}
