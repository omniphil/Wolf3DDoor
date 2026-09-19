/*
 * mixer_trace.c -- SDL2_mixer for Wolf4SDL (include/SDL_mixer.h), mixing into TERMinator's sound queue.
 *
 * Each block, in the order SDL_mixer uses: silence, then the music hook (Wolf4SDL's AdLib player writes the OPL chip's
 * output here, music and AdLib effects both), then the channels (digitised sounds) added on top with their panning,
 * then the post-mix (the PC speaker). Stereo, signed 16-bit, 44.1 kHz: TERMinator's own format.
 *
 * It all runs on the game thread from wolftrace_pump_audio(), which SDL_Delay, SDL_PollEvent and every shown frame
 * call. The game's calls into this file come from the same thread, so there is nothing to lock. The AdLib player also
 * keeps the game's sound timing (a sound "finishes" when the player gets to its end), which is why the pump must keep
 * running while the game waits for a sound to end.
 */

#include <string.h>

#include "SDL_mixer.h"
#include "trace_api.h"
#include "wolftrace.h"

#define RATE        44100      /* TE_AUDIO_RATE */
#define MIX_FRAMES  512

/*
 * Levels, set so Wolfenstein sits beside the other doors through TERMinator (Quake ~-25, Tyrian -24.6, DOOM ~-30 dBFS
 * RMS, all measured the gamesandbox_probe way: RMS and peak over every sample sent). Measured natively 2026-09-19:
 *                               unscaled                      with these gains
 *   menu music                  -34.5 RMS, -19.5 peak          -26.5 RMS, -11.6 peak
 *   E1M1 music alone            -40.1 RMS, -21.4 peak          -32.1 RMS, -13.5 peak
 *   E1M1 + a pistol shot a sec  -16.4 RMS,   0.0 peak (clips)  -24.6 RMS,  -5.0 peak
 * Nuked OPL3 comes out quieter than the MAME chip Wolf4SDL was balanced against, and the digitised sounds are
 * full-scale 8-bit samples, so the music is raised and the effects lowered.
 */
#define MUSIC_GAIN  2.5f        /* AdLib: music and the AdLib sound effects */
#define DIGI_GAIN   0.35f       /* digitised sounds (guns, voices, doors) */

typedef struct
{
    Mix_Chunk *chunk;
    Uint32     pos;            /* samples played */
    int        playing;
    int        tag;
    Uint8      left, right;
    Uint32     started;        /* for Mix_GroupOldest */
} channel_t;

static channel_t g_channels[MIX_CHANNELS];
static int       g_reserved;
static Uint32    g_play_count;
static int       g_open;

static void (*g_finished)(int channel);
static void (*g_music)(void *, Uint8 *, int);
static void  *g_music_arg;
static void (*g_postmix)(void *, Uint8 *, int);
static void  *g_postmix_arg;

int Mix_OpenAudioDevice(int frequency, Uint16 format, int channels, int chunksize, const char *device,
                        int allowed_changes)
{
    (void)frequency; (void)format; (void)channels; (void)chunksize; (void)device; (void)allowed_changes;
    for (int i = 0; i < MIX_CHANNELS; i++)
    {
        memset(&g_channels[i], 0, sizeof(g_channels[i]));
        g_channels[i].tag = -1;
        g_channels[i].left = g_channels[i].right = 255;
    }
    g_open = 1;
    return 0;
}

int Mix_QuerySpec(int *frequency, Uint16 *format, int *channels)
{
    if (frequency) *frequency = RATE;
    if (format) *format = AUDIO_S16SYS;
    if (channels) *channels = 2;
    return 1;
}

const char *Mix_GetError(void) { return "not available"; }

int Mix_ReserveChannels(int num)
{
    g_reserved = num < 0 ? 0 : num > MIX_CHANNELS ? MIX_CHANNELS : num;
    return g_reserved;
}

int Mix_GroupChannels(int from, int to, int tag)
{
    int n = 0;
    for (int i = from; i <= to && i < MIX_CHANNELS; i++, n++)
        if (i >= 0)
            g_channels[i].tag = tag;
    return n;
}

int Mix_GroupAvailable(int tag)
{
    for (int i = 0; i < MIX_CHANNELS; i++)
        if ((tag == -1 || g_channels[i].tag == tag) && !g_channels[i].playing)
            return i;
    return -1;
}

int Mix_GroupOldest(int tag)
{
    int best = -1;
    for (int i = 0; i < MIX_CHANNELS; i++)
        if ((tag == -1 || g_channels[i].tag == tag) && g_channels[i].playing &&
            (best < 0 || g_channels[i].started < g_channels[best].started))
            best = i;
    return best;
}

/* id_sd.c's WAV: a 36-byte RIFF/fmt header, then the "data" chunk */
Mix_Chunk *Mix_LoadWAV_RW(SDL_RWops *src, int freesrc)
{
    Mix_Chunk *chunk = NULL;
    const Uint8 *p;
    size_t size;

    if (src == NULL)
        return NULL;
    p = src->data;
    size = src->size;
    for (size_t off = 12; off + 8 <= size; )
    {
        Uint32 len;
        memcpy(&len, p + off + 4, 4);
        if (memcmp(p + off, "data", 4) == 0)
        {
            if (off + 8 + len > size)
                len = (Uint32)(size - off - 8);
            chunk = (Mix_Chunk *)calloc(1, sizeof(*chunk));
            if (chunk != NULL)
            {
                chunk->abuf = (Uint8 *)malloc(len ? len : 1);
                if (chunk->abuf == NULL)
                {
                    free(chunk);
                    chunk = NULL;
                    break;
                }
                memcpy(chunk->abuf, p + off + 8, len);
                chunk->alen = len;
                chunk->allocated = 1;
                chunk->volume = MIX_MAX_VOLUME;
            }
            break;
        }
        off += 8 + len + (len & 1);
    }
    if (freesrc)
        free(src);
    return chunk;
}

void Mix_FreeChunk(Mix_Chunk *chunk)
{
    if (chunk == NULL)
        return;
    for (int i = 0; i < MIX_CHANNELS; i++)
        if (g_channels[i].chunk == chunk)
            g_channels[i].playing = 0, g_channels[i].chunk = NULL;
    free(chunk->abuf);
    free(chunk);
}

static void stop(int i)
{
    if (!g_channels[i].playing)
        return;
    g_channels[i].playing = 0;
    g_channels[i].chunk = NULL;
    if (g_finished != NULL)
        g_finished(i);
}

int Mix_PlayChannel(int channel, Mix_Chunk *chunk, int loops)
{
    (void)loops;    /* the game never loops a sound */
    if (chunk == NULL)
        return -1;
    if (channel < 0)
    {
        for (int i = g_reserved; i < MIX_CHANNELS; i++)
            if (!g_channels[i].playing)
            {
                channel = i;
                break;
            }
        if (channel < 0)
            return -1;
    }
    if (channel >= MIX_CHANNELS)
        return -1;
    stop(channel);
    g_channels[channel].chunk = chunk;
    g_channels[channel].pos = 0;
    g_channels[channel].playing = 1;
    g_channels[channel].started = ++g_play_count;
    return channel;
}

int Mix_HaltChannel(int channel)
{
    if (channel < 0)
        for (int i = 0; i < MIX_CHANNELS; i++)
            stop(i);
    else if (channel < MIX_CHANNELS)
        stop(channel);
    return 0;
}

int Mix_SetPanning(int channel, Uint8 left, Uint8 right)
{
    if (channel < 0 || channel >= MIX_CHANNELS)
        return 0;
    g_channels[channel].left = left;
    g_channels[channel].right = right;
    return 1;
}

void Mix_ChannelFinished(void (*channel_finished)(int channel)) { g_finished = channel_finished; }

void Mix_HookMusic(void (*mix_func)(void *, Uint8 *, int), void *arg)
{
    g_music = mix_func;
    g_music_arg = arg;
}

void Mix_SetPostMix(void (*mix_func)(void *, Uint8 *, int), void *arg)
{
    g_postmix = mix_func;
    g_postmix_arg = arg;
}

static void mix_block(Sint16 *out, int frames)
{
    static int acc[MIX_FRAMES * 2];

    memset(out, 0, (size_t)frames * 4);
    if (g_music != NULL)
        g_music(g_music_arg, (Uint8 *)out, frames * 4);

    for (int i = 0; i < frames * 2; i++)
        acc[i] = (int)(out[i] * MUSIC_GAIN);
    for (int c = 0; c < MIX_CHANNELS; c++)
    {
        channel_t *ch = &g_channels[c];
        const Sint16 *samples;
        Uint32 count;
        int n;

        if (!ch->playing || ch->chunk == NULL)
            continue;
        samples = (const Sint16 *)ch->chunk->abuf;
        count = ch->chunk->alen / 2;
        n = frames;
        if (ch->pos + (Uint32)n > count)
            n = (int)(count - ch->pos);
        for (int i = 0; i < n; i++)
        {
            int s = (int)(samples[ch->pos + i] * DIGI_GAIN);
            acc[2 * i]     += s * ch->left / 255;
            acc[2 * i + 1] += s * ch->right / 255;
        }
        ch->pos += (Uint32)n;
        if (ch->pos >= count)
            stop(c);
    }
    for (int i = 0; i < frames * 2; i++)
        out[i] = (Sint16)(acc[i] > 32767 ? 32767 : acc[i] < -32768 ? -32768 : acc[i]);

    if (g_postmix != NULL)
        g_postmix(g_postmix_arg, (Uint8 *)out, frames * 4);
}

void wolftrace_pump_audio(void)
{
    static Sint16 block[MIX_FRAMES * 2];
    static int busy;

    if (!g_open || busy)
        return;
    busy = 1;       /* the channel-finished callback could otherwise find its way back in here */
    while (trace_audio_room() >= MIX_FRAMES)
    {
        mix_block(block, MIX_FRAMES);
        if (trace_audio_write(block, MIX_FRAMES) <= 0)
            break;
    }
    busy = 0;
}
