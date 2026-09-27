/*
 * wolfrender.c -- renders Wolfenstein's sound for the JPEG XL graphics mode, which plays it on the caller's own
 * terminal from files in its cache (pix_sound.c). Development only: the results are in door/sound/.
 *
 *     make wolfrender && ./wolfrender ../data /tmp/wolfsound && python3 tools/make_sound.py /tmp/wolfsound sound
 *
 * Writes into the output folder:
 *   digi_NN.wav    each digitised sound (guns, voices, doors), 16-bit mono at 22050 Hz: VSWAP's 8-bit 7042 Hz samples
 *                  upsampled the way the game does it (id_sd.c GetSample, cubic). Sent as they are, the terminal
 *                  stretched them 6x with straight lines, which left them scratchy; the originals hold nothing
 *                  above 3.5 kHz, so 22050 Hz loses nothing and the terminal's last 2x step is harmless
 *   adlib_NN.wav   each AdLib sound effect by its sound number, rendered from AUDIOT
 *   music_NN.wav   each music track by its number (chunk - STARTMUSIC), one pass of its loop
 *   sounds.txt     what was written: "digi N", "adlib N <ms>", "music N <ms>"
 *
 * The AdLib chip is Nuked OPL3, the same emulator and calls the game uses (module/src/opl_trace.c), driven the way
 * Wolf4SDL's id_sd.c drives it: the music sequencer at 700 Hz, sound effects stepped every fifth tick (140 Hz) on
 * channel 0 with their own instrument. Rendered at 44.1 kHz with the game's music gain (module/src/mixer_trace.c,
 * MUSIC_GAIN 2.5), so the terminal plays them at unity and they sit where they do in TRACE. The digitised sounds
 * keep their own level; the door plays them at the mixer's DIGI_GAIN.
 *
 * The chip's raw output is harsh where a real AdLib card's wasn't: some effects use a waveform that rises and then drops
 * straight to zero (a click a cycle), all of it sits on one side of zero (a thump when a sound starts and stops), and
 * the chip fires one short blip after an effect's key-off (a pop at its end). The card's output stage smoothed the
 * first two, so everything rendered here goes through the same (card_output): the offset blocked (20 Hz high-pass) and
 * the edges rounded off (7 kHz low-pass). Effects also lose the blip (drop_blip) and fade over their last 8 ms
 * (fade_end).
 * Chosen by ear, 2026-09-27 ("nothing to use" crackled and popped before).
 *
 * The data layout is the shareware v1.4 one Wolf4SDL builds for (audiowl6.h: 87 sounds, 27 tracks).
 */

#define _USE_MATH_DEFINES
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "opl3.h"

#define LASTSOUND        87
#define LASTMUSIC        27
#define STARTADLIBSOUNDS LASTSOUND
#define STARTMUSIC       (3 * LASTSOUND)
#define NUMSNDCHUNKS     (STARTMUSIC + LASTMUSIC)

#define RATE             44100
#define TICK_SAMPLES     (RATE / 700)       /* id_sd.c: samplesPerMusicTick = param_samplerate / 700 */
#define MUSIC_GAIN       2.5
#define TAIL_MS          400                /* an effect's release after its last note, before trimming silence */
#define ORIGSAMPLERATE   7042
#define DC_HZ            20.0               /* card_output: the offset blocked below this */
#define SOFT_HZ          7000.0             /* card_output: rounded off above this */
#define FADE_MS          8                  /* effect_ending */
#define DIGI_RATE        22050

/* id_sd.h's AdLib registers */
#define alChar    0x20
#define alScale   0x40
#define alAttack  0x60
#define alSus     0x80
#define alWave    0xe0
#define alFreqL   0xa0
#define alFreqH   0xb0
#define alFeedCon 0xc0
#define alEffects 0xbd

static opl3_chip g_chip;

static uint8_t *load(const char *dir, const char *name, size_t *size)
{
    char path[1024];
    FILE *f;
    uint8_t *buf;
    long n;

    snprintf(path, sizeof(path), "%s/%s", dir, name);
    f = fopen(path, "rb");
    if (f == NULL)
    {
        fprintf(stderr, "%s: can't open\n", path);
        exit(1);
    }
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    buf = malloc((size_t)n);
    if (buf == NULL || fread(buf, 1, (size_t)n, f) != (size_t)n)
    {
        fprintf(stderr, "%s: can't read\n", path);
        exit(1);
    }
    fclose(f);
    *size = (size_t)n;
    return buf;
}

static uint16_t u16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t u32(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }

/* ---- output ---- */

typedef struct
{
    int16_t *s;         /* mono */
    size_t   n, cap;
} pcm_t;

static void pcm_add(pcm_t *p, int16_t v)
{
    if (p->n == p->cap)
    {
        p->cap = p->cap ? p->cap * 2 : 65536;
        p->s = realloc(p->s, p->cap * sizeof(int16_t));
        if (p->s == NULL)
            exit(1);
    }
    p->s[p->n++] = v;
}

/* One 700 Hz tick of the chip, mixed to mono with the game's music gain */
static void render_tick(pcm_t *out)
{
    int16_t frames[TICK_SAMPLES * 2];

    OPL3_GenerateStream(&g_chip, frames, TICK_SAMPLES);
    for (int i = 0; i < TICK_SAMPLES; i++)
    {
        double v = (frames[2 * i] + frames[2 * i + 1]) * 0.5 * MUSIC_GAIN;
        pcm_add(out, (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v));
    }
}

static void write_wav(const char *path, const void *data, uint32_t bytes, uint32_t rate, int bits)
{
    uint8_t h[44];
    uint32_t block = (uint32_t)bits / 8, byterate = rate * block, riff = 36 + bytes;
    FILE *f = fopen(path, "wb");

    if (f == NULL)
    {
        fprintf(stderr, "%s: can't write\n", path);
        exit(1);
    }
    memcpy(h, "RIFF", 4);
    memcpy(h + 4, &riff, 4);
    memcpy(h + 8, "WAVEfmt ", 8);
    memcpy(h + 16, "\x10\0\0\0\x01\0\x01\0", 8);           /* 16-byte fmt, PCM, 1 channel */
    memcpy(h + 24, &rate, 4);
    memcpy(h + 28, &byterate, 4);
    h[32] = (uint8_t)block;
    h[33] = 0;
    h[34] = (uint8_t)bits;
    h[35] = 0;
    memcpy(h + 36, "data", 4);
    memcpy(h + 40, &bytes, 4);
    fwrite(h, 1, 44, f);
    fwrite(data, 1, bytes, f);
    fclose(f);
}

/* ---- the card's output stage ---- */

static int16_t clamp16(double v)
{
    return (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : lrint(v));
}

/* The offset blocked and the edges rounded, as a real card's output did. `extra` samples of silence are appended
 * first, so the high-pass settles back to nothing after the sound instead of stopping on a step. */
static void card_output(pcm_t *p, size_t extra)
{
    double r = exp(-2 * M_PI * DC_HZ / RATE), a = exp(-2 * M_PI * SOFT_HZ / RATE);
    double y = 0, px = 0, l1 = 0, l2 = 0;

    for (size_t i = 0; i < extra; i++)
        pcm_add(p, 0);
    for (size_t i = 0; i < p->n; i++)
    {
        double x = p->s[i];
        y = x - px + r * y;
        px = x;
        l1 = (1 - a) * y + a * l1;
        l2 = (1 - a) * l1 + a * l2;
        p->s[i] = clamp16(l2);
    }
}

/* The lone blip the chip fires after an effect's key-off: a last run of sound shorter than 4 ms, after at least 4 ms
 * of silence. Cut off at the start of that silence. On the raw chip output (the filter smears the silence). */
static void drop_blip(pcm_t *p)
{
    size_t gap = RATE * 4 / 1000, last, k, quiet = 0;

    if (p->n == 0)
        return;
    for (last = p->n - 1; last > 0 && abs(p->s[last]) <= 2; last--)
        ;
    for (k = last; k > 0; k--)
    {
        if (abs(p->s[k]) <= 2)
        {
            if (++quiet >= gap)
                break;
        }
        else
        {
            quiet = 0;
        }
    }
    /* s[k .. k+quiet-1] is the silence; the blip runs from k+quiet to last */
    if (quiet >= gap && last - (k + quiet) + 1 < gap)
        p->n = k;
}

/* An effect's ending, after the filter: faded out over FADE_MS from where it was last above 2% of its peak, and
 * nothing after */
static void fade_end(pcm_t *p)
{
    size_t n = RATE * FADE_MS / 1000, last = 0, start;
    int peak = 0;

    for (size_t i = 0; i < p->n; i++)
        if (abs(p->s[i]) > peak)
            peak = abs(p->s[i]);
    for (size_t i = 0; i < p->n; i++)
        if (abs(p->s[i]) > peak / 50)
            last = i;
    start = last > n ? last - n : 0;
    for (size_t i = start; i < p->n && i - start < n; i++)
        p->s[i] = clamp16(p->s[i] * (0.5 + 0.5 * cos(M_PI * (double)(i - start) / (double)n)));
    if (start + n < p->n)
        p->n = start + n;
}

/* ---- the chip, as id_sd.c sets it up ---- */

static void chip_reset(void)
{
    OPL3_Reset(&g_chip, RATE);
    for (int i = 1; i < 0xf6; i++)
        OPL3_WriteReg(&g_chip, (uint16_t)i, 0);
    OPL3_WriteReg(&g_chip, 1, 0x20);                   /* WSE=1 */
    OPL3_WriteReg(&g_chip, alEffects, 0);
}

/* ---- AdLib sound effects (id_sd.c SDL_ALPlaySound and the effect half of SDL_IMFMusicPlayer) ---- */

static bool render_adlib(const uint8_t *d, size_t len, pcm_t *out)
{
    uint32_t length;
    uint8_t block;
    const uint8_t *data, *inst;
    int counter = 5;
    uint32_t left;
    const uint8_t *ptr;

    /* SoundCommon (u32 length, u16 priority), Instrument (16 bytes), block, data */
    if (len < 24)
        return false;
    length = u32(d);
    inst = d + 6;
    block = (uint8_t)(((d[22] & 7) << 2) | 0x20);
    data = d + 23;
    if (length == 0 || 23 + (size_t)length > len || !(inst[6] | inst[7]))
        return false;

    chip_reset();
    /* SDL_AlSetFXInst: modulator slot 0, carrier slot 3 */
    OPL3_WriteReg(&g_chip, alFreqH, 0);
    OPL3_WriteReg(&g_chip, 0 + alChar, inst[0]);
    OPL3_WriteReg(&g_chip, 0 + alScale, inst[2]);
    OPL3_WriteReg(&g_chip, 0 + alAttack, inst[4]);
    OPL3_WriteReg(&g_chip, 0 + alSus, inst[6]);
    OPL3_WriteReg(&g_chip, 0 + alWave, inst[8]);
    OPL3_WriteReg(&g_chip, 3 + alChar, inst[1]);
    OPL3_WriteReg(&g_chip, 3 + alScale, inst[3]);
    OPL3_WriteReg(&g_chip, 3 + alAttack, inst[5]);
    OPL3_WriteReg(&g_chip, 3 + alSus, inst[7]);
    OPL3_WriteReg(&g_chip, 3 + alWave, inst[9]);
    OPL3_WriteReg(&g_chip, alFeedCon, 0);

    ptr = data;
    left = length;
    for (;;)
    {
        if (--counter == 0)
        {
            counter = 5;
            if (left == 0)
                break;
            if (*ptr)
            {
                OPL3_WriteReg(&g_chip, alFreqL, *ptr);
                OPL3_WriteReg(&g_chip, alFreqH, block);
            }
            else
            {
                OPL3_WriteReg(&g_chip, alFreqH, 0);
            }
            ptr++;
            left--;
        }
        render_tick(out);
    }
    OPL3_WriteReg(&g_chip, alFreqH, 0);
    for (int t = 0; t < TAIL_MS * 700 / 1000; t++)
        render_tick(out);

    /* Trim the silence the release fades into, then the card's output stage and a clean ending */
    while (out->n > 0 && abs(out->s[out->n - 1]) < 8)
        out->n--;
    if (out->n == 0)
        return false;
    drop_blip(out);
    card_output(out, RATE / 20);
    fade_end(out);
    return out->n > 0;
}

/* ---- music (the sequencer half of SDL_IMFMusicPlayer), one pass of the loop ---- */

static bool render_music(const uint8_t *d, size_t len, pcm_t *out)
{
    uint32_t seqlen;
    const uint8_t *p;
    uint32_t pos = 0, time = 0, count = 0;

    if (len < 4)
        return false;
    /* The shareware data keeps an 88-byte placeholder for each song only the full game has: no length word, and
     * nothing the game ever plays. Played as a song it would be minutes of noise. */
    if (u16(d) == 0 && len < 1024)
        return false;
    /* SD_StartMusic: a leading word is the length in bytes, unless it's 0 (then the whole chunk is the song) */
    if (u16(d) == 0)
    {
        seqlen = (uint32_t)len;
        p = d;
    }
    else
    {
        seqlen = u16(d);
        p = d + 2;
        if (seqlen > len - 2)
            seqlen = (uint32_t)(len - 2);
    }
    seqlen &= ~3u;
    if (seqlen == 0)
        return false;

    chip_reset();
    for (int i = 0; i < 9; i++)                        /* SD_MusicOff */
        OPL3_WriteReg(&g_chip, (uint16_t)(alFreqH + i), 0);

    /* Each entry: register, value, then a 16-bit delay in 700 Hz ticks before the next */
    while (pos < seqlen)
    {
        while (pos < seqlen && time <= count)
        {
            OPL3_WriteReg(&g_chip, p[pos], p[pos + 1]);
            time = count + u16(p + pos + 2);
            pos += 4;
        }
        render_tick(out);
        count++;
    }
    /* the last entry's delay, before the song starts over */
    while (count < time)
    {
        render_tick(out);
        count++;
    }
    card_output(out, 0);      /* no silence added: the song loops straight back to its start */
    return out->n > 0;
}

/* ---- digitised sounds (id_pm.c's page file, id_sd.c SDL_SetupDigi) ----
 *
 * A sound's pages lie one after another in VSWAP, so its bytes are one run of the file from its first page's offset.
 * Its length is the 16-bit one in the sound info page (the last page), with the high bits from how far its pages
 * reach, exactly as SDL_SetupDigi works it out. */

static int write_digi(const uint8_t *vs, size_t vs_size, const char *outdir, FILE *list)
{
    uint16_t chunks, sound_start;
    const uint8_t *offsets, *info;
    uint32_t info_size;
    int num, written = 0;

    if (vs_size < 6)
        return 0;
    chunks = u16(vs);
    sound_start = u16(vs + 4);
    offsets = vs + 6;
    if (vs_size < 6 + 6 * (size_t)chunks)
        return 0;
    info = vs + u32(offsets + 4 * (chunks - 1));
    info_size = u16(vs + 6 + 4 * chunks + 2 * (chunks - 1));
    num = (int)(info_size / 4);

    for (int i = 0; i < num; i++)
    {
        uint32_t start = u16(info + i * 4) + sound_start, next, first_off, span, size;
        char path[1024];

        if (start >= (uint32_t)chunks - 1)
            break;
        next = i < num - 1 ? u16(info + i * 4 + 4) : 0;
        next = (next == 0 || next + sound_start > (uint32_t)chunks - 1) ? (uint32_t)chunks - 1 : next + sound_start;
        first_off = u32(offsets + 4 * start);
        span = u32(offsets + 4 * next) - first_off;
        if ((span & 0xffff0000) != 0 && (span & 0xffff) < u16(info + i * 4 + 2))
            span -= 0x10000;
        size = (span & 0xffff0000) | u16(info + i * 4 + 2);
        if (size == 0 || first_off == 0 || (size_t)first_off + size > vs_size)
            continue;

        snprintf(path, sizeof(path), "%s/digi_%02d.wav", outdir, i);
        {
            /* id_sd.c SD_PrepareSound: each output sample from GetSample's cubic through the 8-bit originals */
            const uint8_t *src = vs + first_off;
            int dest = (int)((float)size * (float)DIGI_RATE / (float)ORIGSAMPLERATE);
            int16_t *out = malloc((size_t)dest * sizeof(int16_t));
            if (out == NULL)
                exit(1);
            for (int j = 0; j < dest; j++)
            {
                float  pos = (float)size * (float)j / (float)dest;
                int    cur = (int)pos;
                float  sf = pos - (float)cur;
                float  s0 = cur - 1 >= 0 ? (float)(src[cur - 1] - 128) : 0.0f;
                float  s1 = (float)(src[cur] - 128);
                float  s2 = cur + 1 < (int)size ? (float)(src[cur + 1] - 128) : 0.0f;
                float  val = s0 * sf * (sf - 1) / 2 - s1 * (sf * sf - 1) + s2 * (sf + 1) * sf / 2;
                int32_t v = (int32_t)(val * 256);
                out[j] = (int16_t)(v < -32768 ? -32768 : v > 32767 ? 32767 : v);
            }
            write_wav(path, out, (uint32_t)dest * 2, DIGI_RATE, 16);
            free(out);
        }
        fprintf(list, "digi %d\n", i);
        written++;
    }
    return written;
}

int main(int argc, char **argv)
{
    size_t head_size, audio_size, vswap_size;
    uint8_t *head, *audio, *vswap;
    FILE *list;
    char path[1024];
    int adlib = 0, music = 0, digi;

    if (argc != 3)
    {
        fprintf(stderr, "usage: wolfrender <data folder with the .WL1 files> <output folder>\n");
        return 2;
    }
    head = load(argv[1], "AUDIOHED.WL1", &head_size);
    audio = load(argv[1], "AUDIOT.WL1", &audio_size);
    vswap = load(argv[1], "VSWAP.WL1", &vswap_size);
    if (head_size < 4 * (NUMSNDCHUNKS + 1))
    {
        fprintf(stderr, "AUDIOHED.WL1: too short for %d chunks\n", NUMSNDCHUNKS);
        return 1;
    }

    snprintf(path, sizeof(path), "%s/sounds.txt", argv[2]);
    list = fopen(path, "w");
    if (list == NULL)
    {
        fprintf(stderr, "%s: can't write\n", path);
        return 1;
    }

    for (int chunk = 0; chunk < NUMSNDCHUNKS; chunk++)
    {
        uint32_t start = u32(head + 4 * chunk), end = u32(head + 4 * (chunk + 1));
        pcm_t pcm = { 0 };
        bool ok;
        bool is_adlib = chunk >= STARTADLIBSOUNDS && chunk < STARTADLIBSOUNDS + LASTSOUND;
        bool is_music = chunk >= STARTMUSIC;

        if ((!is_adlib && !is_music) || end <= start || end > audio_size)
            continue;
        ok = is_adlib ? render_adlib(audio + start, end - start, &pcm) : render_music(audio + start, end - start, &pcm);
        if (ok)
        {
            int n = is_adlib ? chunk - STARTADLIBSOUNDS : chunk - STARTMUSIC;
            long ms = (long)pcm.n * 1000 / RATE;
            snprintf(path, sizeof(path), "%s/%s_%02d.wav", argv[2], is_adlib ? "adlib" : "music", n);
            write_wav(path, pcm.s, (uint32_t)(pcm.n * 2), RATE, 16);
            fprintf(list, "%s %d %ld\n", is_adlib ? "adlib" : "music", n, ms);
            if (is_adlib)
                adlib++;
            else
                music++;
        }
        free(pcm.s);
    }
    digi = write_digi(vswap, vswap_size, argv[2], list);
    fclose(list);
    printf("wolfrender: %d AdLib effects, %d tracks, %d digitised sounds\n", adlib, music, digi);
    return 0;
}
