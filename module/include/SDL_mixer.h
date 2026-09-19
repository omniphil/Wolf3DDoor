/*
 * SDL_mixer.h -- the part of SDL2_mixer that Wolf4SDL uses (id_sd.c), answered by src/mixer_trace.c.
 *
 * Wolf4SDL plays its digitised sounds on mixer channels, its AdLib music and sound effects through a music hook (an
 * emulated OPL2 chip), and the PC speaker through a post-mix callback. mixer_trace.c does all three on the game's own
 * thread, whenever TERMinator's sound queue has room, so nothing here needs a lock.
 */

#ifndef WOLFTRACE_SDL_MIXER_H
#define WOLFTRACE_SDL_MIXER_H

#include "SDL.h"

#define MIX_CHANNELS 8
#define MIX_MAX_VOLUME 128

typedef struct Mix_Chunk
{
    int    allocated;
    Uint8 *abuf;        /* mono signed 16-bit at the output rate */
    Uint32 alen;        /* bytes */
    Uint8  volume;
} Mix_Chunk;

int  Mix_OpenAudioDevice(int frequency, Uint16 format, int channels, int chunksize, const char *device,
                         int allowed_changes);
int  Mix_QuerySpec(int *frequency, Uint16 *format, int *channels);
const char *Mix_GetError(void);

int  Mix_ReserveChannels(int num);
int  Mix_GroupChannels(int from, int to, int tag);
int  Mix_GroupAvailable(int tag);
int  Mix_GroupOldest(int tag);

/* Only the one kind of WAV the game makes itself: PCM, mono, 16-bit, at the output rate */
Mix_Chunk *Mix_LoadWAV_RW(SDL_RWops *src, int freesrc);
void Mix_FreeChunk(Mix_Chunk *chunk);

int  Mix_PlayChannel(int channel, Mix_Chunk *chunk, int loops);
int  Mix_HaltChannel(int channel);
int  Mix_SetPanning(int channel, Uint8 left, Uint8 right);
void Mix_ChannelFinished(void (*channel_finished)(int channel));

void Mix_HookMusic(void (*mix_func)(void *udata, Uint8 *stream, int len), void *arg);
void Mix_SetPostMix(void (*mix_func)(void *udata, Uint8 *stream, int len), void *arg);

#endif
