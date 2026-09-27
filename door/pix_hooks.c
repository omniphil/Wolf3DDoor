/*
 * pix_hooks.c -- Wolfenstein's sound and music calls, caught for the JPEG XL graphics mode.
 *
 * In that mode the sound plays on the caller's terminal, from files it keeps in its cache (pix_sound.c), so what the
 * door needs from the game is not the mixed sound but the events: this digitised sound started on that channel,
 * panned so; this AdLib effect began; this track started, so far in. Wolf4SDL's id_sd.c and the module's
 * mixer_trace.c are left alone: the door is linked with --wrap for each of these functions (Makefile), so the game's
 * calls land here first. Every call is passed on to the real function, in this mode too, so the game's own sound
 * state and timing (the AdLib player decides when an effect has finished) stay exactly as they are in ANSI mode,
 * where the mixed sound is thrown away (ansi_host.c).
 *
 * --wrap only catches calls between files, which is what these are: the game (wl_*.c) calls id_sd.c, and id_sd.c
 * calls the mixer (mixer_trace.c). Where the game uses:
 *   digitised sounds   SD_PlayDigitized -> Mix_SetPanning + Mix_PlayChannel on a Mix_Chunk made by SD_PrepareSound
 *                      (via Mix_LoadWAV_RW), moved with Mix_SetPanning, stopped with Mix_HaltChannel
 *   AdLib effects      SD_PlaySound when the sound has no digitised version (or digitised sound is off),
 *                      SD_StopSound
 *   music              SD_StartMusic, SD_ContinueMusic (back from the menu, part way in), SD_MusicOff,
 *                      SD_FadeOutMusic, SD_SetMusicMode(off)
 *
 * Runs on the game's thread; the door reads the events from its own, hence the lock.
 */

#include <pthread.h>
#include <stdatomic.h>
#include <string.h>

#include "wl_def.h"
#include <SDL_mixer.h>

#include "pix_hooks.h"      /* after wl_def.h's #pragma pack(1): the header sets its own packing and checks it */

#define QUEUE 256
#define MAX_CHUNKS (STARTMUSIC - STARTDIGISOUNDS)

static atomic_int      g_on;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static pix_event_t     g_queue[QUEUE];
static int             g_head, g_tail;

/* Which digitised sound each of the game's Mix_Chunks is, found as SD_PrepareSound makes them */
static Mix_Chunk *g_chunks[MAX_CHUNKS];
static int        g_preparing = -1;

/* Each mixer channel's panning, as last set */
static int g_left[MIX_CHANNELS], g_right[MIX_CHANNELS];

void pix_hooks_enable(bool on)
{
    atomic_store(&g_on, on ? 1 : 0);
}

bool pix_hooks_next(pix_event_t *ev)
{
    bool got = false;
    pthread_mutex_lock(&g_lock);
    if (g_head != g_tail)
    {
        *ev = g_queue[g_head];
        g_head = (g_head + 1) % QUEUE;
        got = true;
    }
    pthread_mutex_unlock(&g_lock);
    return got;
}

static void push(const pix_event_t *ev)
{
    if (!atomic_load(&g_on))
        return;
    pthread_mutex_lock(&g_lock);
    if ((g_tail + 1) % QUEUE != g_head)
    {
        g_queue[g_tail] = *ev;
        g_tail = (g_tail + 1) % QUEUE;
    }
    pthread_mutex_unlock(&g_lock);
}

static void push_simple(pix_event_kind_t kind)
{
    pix_event_t ev = { 0 };
    ev.kind = kind;
    push(&ev);
}

/* ---- digitised sounds ---- */

void       __real_SD_PrepareSound(int which);
Mix_Chunk *__real_Mix_LoadWAV_RW(SDL_RWops *src, int freesrc);
int        __real_Mix_PlayChannel(int channel, Mix_Chunk *chunk, int loops);
int        __real_Mix_HaltChannel(int channel);
int        __real_Mix_SetPanning(int channel, Uint8 left, Uint8 right);

void __wrap_SD_PrepareSound(int which)
{
    g_preparing = which;
    __real_SD_PrepareSound(which);
    g_preparing = -1;
}

Mix_Chunk *__wrap_Mix_LoadWAV_RW(SDL_RWops *src, int freesrc)
{
    Mix_Chunk *chunk = __real_Mix_LoadWAV_RW(src, freesrc);
    if (g_preparing >= 0 && g_preparing < MAX_CHUNKS)
        g_chunks[g_preparing] = chunk;
    return chunk;
}

int __wrap_Mix_SetPanning(int channel, Uint8 left, Uint8 right)
{
    int ret = __real_Mix_SetPanning(channel, left, right);
    if (channel >= 0 && channel < MIX_CHANNELS)
    {
        pix_event_t ev = { 0 };
        g_left[channel] = left;
        g_right[channel] = right;
        ev.kind = PIX_DIGI_PAN;
        ev.channel = channel;
        ev.left = left;
        ev.right = right;
        push(&ev);
    }
    return ret;
}

int __wrap_Mix_PlayChannel(int channel, Mix_Chunk *chunk, int loops)
{
    int ret = __real_Mix_PlayChannel(channel, chunk, loops);
    if (ret >= 0 && ret < MIX_CHANNELS && chunk != NULL)
        for (int i = 0; i < MAX_CHUNKS; i++)
            if (g_chunks[i] == chunk)
            {
                pix_event_t ev = { 0 };
                ev.kind = PIX_DIGI_START;
                ev.channel = ret;
                ev.sound = i;
                ev.left = g_left[ret];
                ev.right = g_right[ret];
                push(&ev);
                break;
            }
    return ret;
}

int __wrap_Mix_HaltChannel(int channel)
{
    pix_event_t ev = { 0 };
    int ret = __real_Mix_HaltChannel(channel);
    ev.kind = PIX_DIGI_STOP;
    ev.channel = channel;
    push(&ev);
    return ret;
}

/* ---- AdLib effects ---- */

boolean __real_SD_PlaySound(int sound);
void    __real_SD_StopSound(void);
boolean __real_SD_SetSoundMode(byte mode);

boolean __wrap_SD_PlaySound(int sound)
{
    boolean ret = __real_SD_PlaySound(sound);

    /* The AdLib half of SD_PlaySound: no digitised version (or digitised sound off), AdLib on, and it wasn't turned
     * down for a sound of higher priority already playing (then the playing one is still the one playing) */
    if (sound >= 0 && sound < LASTSOUND && SoundMode == sdm_AdLib && (DigiMode == sds_Off || DigiMap[sound] == -1) &&
        (int)SD_SoundPlaying() == sound)
    {
        pix_event_t ev = { 0 };
        ev.kind = PIX_ADLIB_START;
        ev.sound = sound;
        push(&ev);
    }
    return ret;
}

void __wrap_SD_StopSound(void)
{
    __real_SD_StopSound();
    push_simple(PIX_ADLIB_STOP);
}

boolean __wrap_SD_SetSoundMode(byte mode)
{
    boolean ret = __real_SD_SetSoundMode(mode);
    if (mode != sdm_AdLib)
        push_simple(PIX_ADLIB_STOP);
    return ret;
}

/* ---- music ---- */

void    __real_SD_StartMusic(int chunk);
void    __real_SD_ContinueMusic(int chunk, int startoffs);
int     __real_SD_MusicOff(void);
void    __real_SD_FadeOutMusic(void);
boolean __real_SD_SetMusicMode(byte mode);

static void music_event(int chunk, long start_ms)
{
    pix_event_t ev = { 0 };
    if (MusicMode != smm_AdLib || chunk < STARTMUSIC || chunk >= STARTMUSIC + LASTMUSIC)
        return;
    ev.kind = PIX_MUS_PLAY;
    ev.sound = chunk - STARTMUSIC;
    ev.start_ms = start_ms;
    push(&ev);
}

void __wrap_SD_StartMusic(int chunk)
{
    __real_SD_StartMusic(chunk);
    music_event(chunk, 0);
}

/* Where startoffs (in words, as SD_MusicOff returned it) is in the song, in ms: the sum of the 700 Hz delays before
 * it. The song is cached by now (the real call just did it). */
static long music_offset_ms(int chunk, int startoffs)
{
    const word *song = (const word *)(const void *)audiosegs[chunk];
    long ticks = 0;
    int words;

    if (song == NULL || startoffs <= 0 || song[0] == 0)
        return 0;
    words = song[0] / 2;
    song++;
    if (startoffs >= words)
        return 0;           /* SD_ContinueMusic starts over then too */
    for (int i = 0; i + 1 < startoffs; i += 2)
        ticks += song[i + 1];
    return ticks * 1000 / 700;
}

void __wrap_SD_ContinueMusic(int chunk, int startoffs)
{
    __real_SD_ContinueMusic(chunk, startoffs);
    music_event(chunk, music_offset_ms(chunk, startoffs));
}

int __wrap_SD_MusicOff(void)
{
    int ret = __real_SD_MusicOff();
    push_simple(PIX_MUS_STOP);
    return ret;
}

void __wrap_SD_FadeOutMusic(void)
{
    __real_SD_FadeOutMusic();
    push_simple(PIX_MUS_STOP);
}

boolean __wrap_SD_SetMusicMode(byte mode)
{
    boolean ret = __real_SD_SetMusicMode(mode);
    if (mode != smm_AdLib)
        push_simple(PIX_MUS_STOP);
    return ret;
}
