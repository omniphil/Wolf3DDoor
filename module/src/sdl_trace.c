/*
 * sdl_trace.c -- the SDL2 calls Wolf4SDL makes (include/SDL.h), answered by TRACE.
 *
 * Time comes from trace_time_ms, keys from TERMinator's events, and a frame goes to TERMinator when the game presents
 * its renderer. Surfaces are plain memory: the game draws into an 8-bit one and copies it through the palette into a
 * 32-bit one (SDL_BlitSurface), which is exactly the BGRA picture trace_present takes. Sound is mixer_trace.c.
 *
 * The keyboard tables are the Tyrian module's (BBSGames/Tyrian/module/src/sdl_trace.c).
 */

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "SDL.h"
#include "trace_api.h"
#include "wolftrace.h"

/* ---- the little things ---- */

int  SDL_Init(Uint32 flags) { (void)flags; return 0; }
void SDL_Quit(void) { }
const char *SDL_GetError(void) { return "not available"; }
int  SDL_SetHint(const char *name, const char *value) { (void)name; (void)value; return 1; }

int SDL_ShowSimpleMessageBox(Uint32 flags, const char *title, const char *message, SDL_Window *window)
{
    (void)flags; (void)window;
    wolftrace_log("wolf3d: %s: %s", title, message);   /* Quit()'s error message: there is no window to show it in */
    return 0;
}

/* ---- time ---- */

Uint32 SDL_GetTicks(void)
{
    return (Uint32)trace_time_ms();
}

/*
 * Every wait in the game ends up here, so this is where sound is kept flowing and saves go up to the BBS. Long
 * waits are cut into short sleeps so neither runs dry while the game sits on a menu.
 */
void SDL_Delay(Uint32 ms)
{
    Uint32 end = SDL_GetTicks() + ms;

    for (;;)
    {
        Sint32 left;
        struct timespec ts;

        wolftrace_pump();
        left = (Sint32)(end - SDL_GetTicks());
        if (left <= 0)
            break;
        if (left > 5)
            left = 5;
        ts.tv_sec = 0;
        ts.tv_nsec = (long)left * 1000000L;
        nanosleep(&ts, NULL);
    }
}

/* ---- surfaces ---- */

SDL_bool SDL_PixelFormatEnumToMasks(Uint32 format, int *bpp, Uint32 *rmask, Uint32 *gmask, Uint32 *bmask,
                                    Uint32 *amask)
{
    (void)format;   /* only ever ARGB8888 */
    *bpp = 32;
    *rmask = 0x00FF0000u;
    *gmask = 0x0000FF00u;
    *bmask = 0x000000FFu;
    *amask = 0xFF000000u;
    return SDL_TRUE;
}

SDL_Surface *SDL_CreateRGBSurface(Uint32 flags, int width, int height, int depth,
                                  Uint32 rmask, Uint32 gmask, Uint32 bmask, Uint32 amask)
{
    SDL_Surface *s;
    (void)flags; (void)rmask; (void)gmask; (void)bmask; (void)amask;

    if ((depth != 8 && depth != 32) || width <= 0 || height <= 0)
        return NULL;
    s = (SDL_Surface *)calloc(1, sizeof(*s));
    if (s == NULL)
        return NULL;
    s->format = (SDL_PixelFormat *)calloc(1, sizeof(*s->format));
    if (s->format != NULL)
        s->format->palette = (SDL_Palette *)calloc(1, sizeof(*s->format->palette));
    if (s->format == NULL || s->format->palette == NULL)
    {
        if (s->format != NULL)
            free(s->format);
        free(s);
        return NULL;
    }
    s->format->format = depth == 8 ? 0 : SDL_PIXELFORMAT_ARGB8888;
    s->format->BitsPerPixel = (Uint8)depth;
    s->format->BytesPerPixel = (Uint8)(depth / 8);
    s->format->palette->ncolors = 256;
    s->w = width;
    s->h = height;
    s->pitch = width * s->format->BytesPerPixel;
    s->pixels = calloc((size_t)s->pitch, (size_t)height);
    if (s->pixels == NULL)
    {
        free(s->format->palette);
        free(s->format);
        free(s);
        return NULL;
    }
    return s;
}

void SDL_FreeSurface(SDL_Surface *surface)
{
    if (surface != NULL)
    {
        free(surface->pixels);
        free(surface->format->palette);
        free(surface->format);
        free(surface);
    }
}

int SDL_SetPaletteColors(SDL_Palette *palette, const SDL_Color *colors, int first, int ncolors)
{
    if (palette == NULL || first < 0 || first + ncolors > 256)
        return -1;
    memcpy(palette->colors + first, colors, sizeof(SDL_Color) * (size_t)ncolors);
    return 0;
}

Uint32 SDL_MapRGBA(const SDL_PixelFormat *format, Uint8 r, Uint8 g, Uint8 b, Uint8 a)
{
    (void)format;
    return ((Uint32)a << 24) | ((Uint32)r << 16) | ((Uint32)g << 8) | b;
}

/* Clip a rectangle to a surface; false when nothing is left */
static int clip(const SDL_Surface *s, SDL_Rect *r)
{
    int x2 = r->x + r->w, y2 = r->y + r->h;
    if (r->x < 0) r->x = 0;
    if (r->y < 0) r->y = 0;
    if (x2 > s->w) x2 = s->w;
    if (y2 > s->h) y2 = s->h;
    r->w = x2 - r->x;
    r->h = y2 - r->y;
    return r->w > 0 && r->h > 0;
}

int SDL_FillRect(SDL_Surface *dst, const SDL_Rect *rect, Uint32 color)
{
    SDL_Rect r = rect != NULL ? *rect : (SDL_Rect){ 0, 0, dst->w, dst->h };

    if (!clip(dst, &r))
        return 0;
    for (int y = r.y; y < r.y + r.h; y++)
    {
        Uint8 *row = (Uint8 *)dst->pixels + (size_t)y * dst->pitch;
        if (dst->format->BytesPerPixel == 1)
            memset(row + r.x, (int)(color & 0xFF), (size_t)r.w);
        else
            for (int x = r.x; x < r.x + r.w; x++)
                ((Uint32 *)row)[x] = color;
    }
    return 0;
}

/* The game only ever copies one whole screen to another: 8-bit to 8-bit, or 8-bit through its palette to 32-bit */
int SDL_BlitSurface(SDL_Surface *src, const SDL_Rect *srcrect, SDL_Surface *dst, SDL_Rect *dstrect)
{
    SDL_Rect s = srcrect != NULL ? *srcrect : (SDL_Rect){ 0, 0, src->w, src->h };
    int dx = dstrect != NULL ? dstrect->x : 0, dy = dstrect != NULL ? dstrect->y : 0;
    int sbpp = src->format->BytesPerPixel, dbpp = dst->format->BytesPerPixel;
    Uint32 lut[256];

    if (!clip(src, &s) || (sbpp != dbpp && !(sbpp == 1 && dbpp == 4)))
        return -1;
    if (sbpp == 1 && dbpp == 4)
        for (int i = 0; i < 256; i++)
        {
            const SDL_Color *c = &src->format->palette->colors[i];
            lut[i] = 0xFF000000u | ((Uint32)c->r << 16) | ((Uint32)c->g << 8) | c->b;
        }
    for (int y = 0; y < s.h; y++)
    {
        int ty = dy + y, tx = dx, w = s.w, sx = s.x;
        const Uint8 *from;
        if (ty < 0 || ty >= dst->h)
            continue;
        if (tx < 0) { sx -= tx; w += tx; tx = 0; }
        if (tx + w > dst->w) w = dst->w - tx;
        if (w <= 0)
            continue;
        from = (const Uint8 *)src->pixels + (size_t)(s.y + y) * src->pitch + (size_t)sx * sbpp;
        if (sbpp == dbpp)
            memcpy((Uint8 *)dst->pixels + (size_t)ty * dst->pitch + (size_t)tx * dbpp, from, (size_t)w * dbpp);
        else
        {
            Uint32 *to = (Uint32 *)((Uint8 *)dst->pixels + (size_t)ty * dst->pitch) + tx;
            for (int x = 0; x < w; x++)
                to[x] = lut[from[x]];
        }
    }
    return 0;
}

SDL_RWops *SDL_RWFromMem(void *mem, int size)
{
    SDL_RWops *rw = (SDL_RWops *)malloc(sizeof(*rw));
    if (rw != NULL)
    {
        rw->data = (const Uint8 *)mem;
        rw->size = size > 0 ? (size_t)size : 0;
    }
    return rw;
}

/* ---- the window: one picture, TERMinator's ---- */

struct SDL_Window { int w, h; };
struct SDL_Renderer { int unused; };
struct SDL_Texture { int w, h; uint32_t *pixels; };

static SDL_Texture *g_texture;      /* the one the game presents */

SDL_Window *SDL_CreateWindow(const char *title, int x, int y, int w, int h, Uint32 flags)
{
    SDL_Window *win = (SDL_Window *)calloc(1, sizeof(*win));
    (void)title; (void)x; (void)y; (void)flags;
    if (win != NULL)
    {
        win->w = w;
        win->h = h;
    }
    return win;
}

void SDL_DestroyWindow(SDL_Window *window) { free(window); }

SDL_Renderer *SDL_CreateRenderer(SDL_Window *window, int index, Uint32 flags)
{
    (void)window; (void)index; (void)flags;
    return (SDL_Renderer *)calloc(1, sizeof(SDL_Renderer));
}

void SDL_DestroyRenderer(SDL_Renderer *renderer) { free(renderer); }

SDL_Texture *SDL_CreateTexture(SDL_Renderer *renderer, Uint32 format, int access, int w, int h)
{
    SDL_Texture *t = (SDL_Texture *)calloc(1, sizeof(*t));
    (void)renderer; (void)format; (void)access;
    if (t == NULL)
        return NULL;
    t->pixels = (uint32_t *)calloc((size_t)w * (size_t)h, sizeof(uint32_t));
    if (t->pixels == NULL)
    {
        free(t);
        return NULL;
    }
    t->w = w;
    t->h = h;
    g_texture = t;
    return t;
}

void SDL_DestroyTexture(SDL_Texture *texture)
{
    if (texture == NULL)
        return;
    if (texture == g_texture)
        g_texture = NULL;
    free(texture->pixels);
    free(texture);
}

int SDL_UpdateTexture(SDL_Texture *texture, const SDL_Rect *rect, const void *pixels, int pitch)
{
    (void)rect;     /* always the whole screen */
    for (int y = 0; y < texture->h; y++)
        memcpy(texture->pixels + (size_t)y * texture->w, (const Uint8 *)pixels + (size_t)y * pitch,
               (size_t)texture->w * 4);
    g_texture = texture;
    return 0;
}

/* The frame goes to TERMinator with the 4:3 flag: the tall pixels Wolfenstein had on a VGA monitor */
void SDL_RenderPresent(SDL_Renderer *renderer)
{
    (void)renderer;
    if (g_texture != NULL)
        trace_present(g_texture->pixels, g_texture->w, g_texture->h, TRACE_PRESENT_ASPECT_4_3);
    wolftrace_pump();       /* sound and saves are kept going from here too, right after the picture */
}

/* ---- keyboard ----
 *
 * TERMinator sends physical keys as set-1 scancodes, with a flag for the E0-prefixed ones (the arrow block, right
 * Ctrl/Alt, keypad Enter). They become SDL's scancodes, which is what Wolf4SDL keeps in its key settings.
 */

static const Uint8 g_set1[128] =
{
    [0x01] = SDL_SCANCODE_ESCAPE,
    [0x02] = SDL_SCANCODE_1, [0x03] = SDL_SCANCODE_2, [0x04] = SDL_SCANCODE_3, [0x05] = SDL_SCANCODE_4,
    [0x06] = SDL_SCANCODE_5, [0x07] = SDL_SCANCODE_6, [0x08] = SDL_SCANCODE_7, [0x09] = SDL_SCANCODE_8,
    [0x0A] = SDL_SCANCODE_9, [0x0B] = SDL_SCANCODE_0, [0x0C] = SDL_SCANCODE_MINUS, [0x0D] = SDL_SCANCODE_EQUALS,
    [0x0E] = SDL_SCANCODE_BACKSPACE, [0x0F] = SDL_SCANCODE_TAB,
    [0x10] = SDL_SCANCODE_Q, [0x11] = SDL_SCANCODE_W, [0x12] = SDL_SCANCODE_E, [0x13] = SDL_SCANCODE_R,
    [0x14] = SDL_SCANCODE_T, [0x15] = SDL_SCANCODE_Y, [0x16] = SDL_SCANCODE_U, [0x17] = SDL_SCANCODE_I,
    [0x18] = SDL_SCANCODE_O, [0x19] = SDL_SCANCODE_P, [0x1A] = SDL_SCANCODE_LEFTBRACKET,
    [0x1B] = SDL_SCANCODE_RIGHTBRACKET, [0x1C] = SDL_SCANCODE_RETURN, [0x1D] = SDL_SCANCODE_LCTRL,
    [0x1E] = SDL_SCANCODE_A, [0x1F] = SDL_SCANCODE_S, [0x20] = SDL_SCANCODE_D, [0x21] = SDL_SCANCODE_F,
    [0x22] = SDL_SCANCODE_G, [0x23] = SDL_SCANCODE_H, [0x24] = SDL_SCANCODE_J, [0x25] = SDL_SCANCODE_K,
    [0x26] = SDL_SCANCODE_L, [0x27] = SDL_SCANCODE_SEMICOLON, [0x28] = SDL_SCANCODE_APOSTROPHE,
    [0x29] = SDL_SCANCODE_GRAVE, [0x2A] = SDL_SCANCODE_LSHIFT, [0x2B] = SDL_SCANCODE_BACKSLASH,
    [0x2C] = SDL_SCANCODE_Z, [0x2D] = SDL_SCANCODE_X, [0x2E] = SDL_SCANCODE_C, [0x2F] = SDL_SCANCODE_V,
    [0x30] = SDL_SCANCODE_B, [0x31] = SDL_SCANCODE_N, [0x32] = SDL_SCANCODE_M, [0x33] = SDL_SCANCODE_COMMA,
    [0x34] = SDL_SCANCODE_PERIOD, [0x35] = SDL_SCANCODE_SLASH, [0x36] = SDL_SCANCODE_RSHIFT,
    [0x37] = SDL_SCANCODE_KP_MULTIPLY, [0x38] = SDL_SCANCODE_LALT, [0x39] = SDL_SCANCODE_SPACE,
    [0x3A] = SDL_SCANCODE_CAPSLOCK,
    [0x3B] = SDL_SCANCODE_F1, [0x3C] = SDL_SCANCODE_F2, [0x3D] = SDL_SCANCODE_F3, [0x3E] = SDL_SCANCODE_F4,
    [0x3F] = SDL_SCANCODE_F5, [0x40] = SDL_SCANCODE_F6, [0x41] = SDL_SCANCODE_F7, [0x42] = SDL_SCANCODE_F8,
    [0x43] = SDL_SCANCODE_F9, [0x44] = SDL_SCANCODE_F10, [0x45] = SDL_SCANCODE_NUMLOCKCLEAR,
    [0x46] = SDL_SCANCODE_SCROLLLOCK,
    [0x47] = SDL_SCANCODE_KP_7, [0x48] = SDL_SCANCODE_KP_8, [0x49] = SDL_SCANCODE_KP_9,
    [0x4A] = SDL_SCANCODE_KP_MINUS, [0x4B] = SDL_SCANCODE_KP_4, [0x4C] = SDL_SCANCODE_KP_5,
    [0x4D] = SDL_SCANCODE_KP_6, [0x4E] = SDL_SCANCODE_KP_PLUS, [0x4F] = SDL_SCANCODE_KP_1,
    [0x50] = SDL_SCANCODE_KP_2, [0x51] = SDL_SCANCODE_KP_3, [0x52] = SDL_SCANCODE_KP_0,
    [0x53] = SDL_SCANCODE_KP_PERIOD, [0x56] = SDL_SCANCODE_NONUSBACKSLASH,
    [0x57] = SDL_SCANCODE_F11, [0x58] = SDL_SCANCODE_F12,
};

/* The E0 keys: the same scancodes, but a different key */
static SDL_Scancode extended_key(int set1)
{
    switch (set1)
    {
    case 0x1C: return SDL_SCANCODE_KP_ENTER;
    case 0x1D: return SDL_SCANCODE_RCTRL;
    case 0x35: return SDL_SCANCODE_KP_DIVIDE;
    case 0x37: return SDL_SCANCODE_PRINTSCREEN;
    case 0x38: return SDL_SCANCODE_RALT;
    case 0x47: return SDL_SCANCODE_HOME;
    case 0x48: return SDL_SCANCODE_UP;
    case 0x49: return SDL_SCANCODE_PAGEUP;
    case 0x4B: return SDL_SCANCODE_LEFT;
    case 0x4D: return SDL_SCANCODE_RIGHT;
    case 0x4F: return SDL_SCANCODE_END;
    case 0x50: return SDL_SCANCODE_DOWN;
    case 0x51: return SDL_SCANCODE_PAGEDOWN;
    case 0x52: return SDL_SCANCODE_INSERT;
    case 0x53: return SDL_SCANCODE_DELETE;
    case 0x5B: return SDL_SCANCODE_LGUI;
    case 0x5C: return SDL_SCANCODE_RGUI;
    default:   return SDL_SCANCODE_UNKNOWN;
    }
}

static Uint16 modifier_for(SDL_Scancode sc)
{
    switch (sc)
    {
    case SDL_SCANCODE_LSHIFT: return KMOD_LSHIFT;
    case SDL_SCANCODE_RSHIFT: return KMOD_RSHIFT;
    case SDL_SCANCODE_LCTRL:  return KMOD_LCTRL;
    case SDL_SCANCODE_RCTRL:  return KMOD_RCTRL;
    case SDL_SCANCODE_LALT:   return KMOD_LALT;
    case SDL_SCANCODE_RALT:   return KMOD_RALT;
    case SDL_SCANCODE_LGUI:   return KMOD_LGUI;
    case SDL_SCANCODE_RGUI:   return KMOD_RGUI;
    default:                  return 0;
    }
}

static Uint16 g_mod;

SDL_Keymod SDL_GetModState(void)
{
    return (SDL_Keymod)g_mod;
}

/* ---- events ---- */

static Uint8 g_down[SDL_NUM_SCANCODES];

/* A UTF-16 code unit to UTF-8. Surrogates are dropped: the game only has code page 437 to show anyway. */
static int utf8(unsigned cp, char *out)
{
    if (cp < 0x80) { out[0] = (char)cp; return 1; }
    if (cp < 0x800) { out[0] = (char)(0xC0 | (cp >> 6)); out[1] = (char)(0x80 | (cp & 0x3F)); return 2; }
    if (cp >= 0xD800 && cp < 0xE000) return 0;
    out[0] = (char)(0xE0 | (cp >> 12));
    out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
    out[2] = (char)(0x80 | (cp & 0x3F));
    return 3;
}

static int g_releasing;

/* Releases held back so a quick tap is seen (see TE_IN_KEY below) */
#define MIN_HOLD_MS 50
/* The ANSI door's copy of the game sets this to 0 (door/ansi_host.c): it paces its own key presses to the frame and
 * needs a one-frame press to stay one frame */
int wolftrace_min_hold_ms = MIN_HOLD_MS;
static Uint32 g_down_at[SDL_NUM_SCANCODES];
static Uint32 g_up_due[SDL_NUM_SCANCODES];
static int    g_ups_pending;

static int held_release_due(SDL_Event *event)
{
    Uint32 now = SDL_GetTicks();
    for (int sc = 0; g_ups_pending > 0 && sc < SDL_NUM_SCANCODES; sc++)
        if (g_up_due[sc] && (Sint32)(now - g_up_due[sc]) >= 0)
        {
            g_up_due[sc] = 0;
            g_ups_pending--;
            g_down[sc] = 0;
            g_mod &= (Uint16)~modifier_for((SDL_Scancode)sc);
            memset(event, 0, sizeof(*event));
            event->type = SDL_KEYUP;
            event->key.state = SDL_RELEASED;
            event->key.keysym.scancode = (SDL_Scancode)sc;
            event->key.keysym.mod = g_mod;
            return 1;
        }
    return 0;
}

/* One key-up for a key still down, while letting go of everything after focus was lost */
static int release_one(SDL_Event *event)
{
    for (int sc = 0; sc < SDL_NUM_SCANCODES; sc++)
        if (g_down[sc])
        {
            g_down[sc] = 0;
            memset(event, 0, sizeof(*event));
            event->type = SDL_KEYUP;
            event->key.state = SDL_RELEASED;
            event->key.keysym.scancode = (SDL_Scancode)sc;
            return 1;
        }
    g_releasing = 0;
    return 0;
}

int SDL_PollEvent(SDL_Event *event)
{
    wolftrace_event_t in;

    if (g_releasing && release_one(event))
        return 1;
    if (held_release_due(event))
        return 1;

    wolftrace_pump();

    while (wolftrace_next_event(&in))
    {
        memset(event, 0, sizeof(*event));
        switch (in.type)
        {
        case TE_IN_KEY:
        {
            /* E0 keys come as the flag (TERMinator) or as 256 + the code (older test probes) */
            int code = in.a, ext = in.flags & 2;
            SDL_Scancode sc;
            int pressed = in.flags & 1;
            Uint16 mod;

            if (code >= 256)
            {
                code -= 256;
                ext = 2;
            }
            sc = ext ? extended_key(code) : ((unsigned)code < 128 ? g_set1[code] : 0);
            if (sc == SDL_SCANCODE_UNKNOWN)
                continue;

            if (pressed)
            {
                if (g_up_due[sc])
                {
                    g_up_due[sc] = 0;       /* pressed again before a held-over release: it never let go */
                    g_ups_pending--;
                }
                g_down_at[sc] = SDL_GetTicks();
            }
            else if (g_down[sc] && SDL_GetTicks() - g_down_at[sc] < (Uint32)wolftrace_min_hold_ms)
            {
                /* Wolf4SDL reads keys as held-down state, so a press and release that arrive in the same poll
                 * would never be seen. Hold the release back until the key has been down a moment. */
                if (!g_up_due[sc])
                    g_ups_pending++;
                g_up_due[sc] = g_down_at[sc] + wolftrace_min_hold_ms;
                continue;
            }

            mod = modifier_for(sc);
            if (pressed)
                g_mod |= mod;
            else
                g_mod &= (Uint16)~mod;

            event->type = pressed ? SDL_KEYDOWN : SDL_KEYUP;
            event->key.state = pressed ? SDL_PRESSED : SDL_RELEASED;
            event->key.repeat = (Uint8)(pressed && g_down[sc]);   /* a held key auto-repeating */
            event->key.keysym.scancode = sc;
            event->key.keysym.mod = g_mod;
            g_down[sc] = (Uint8)pressed;
            return 1;
        }

        case TE_IN_TEXT:
        {
            /* Typed characters: the game reads these when a save is being named. Control characters come as keys. */
            int n;
            if (in.a < 32 || in.a == 127)
                continue;
            n = utf8((unsigned)in.a, event->text.text);
            if (n == 0)
                continue;
            event->text.text[n] = '\0';
            event->type = SDL_TEXTINPUT;
            return 1;
        }

        case TE_IN_FOCUS:
            /* Keys held while the picture lost focus will never see their release: let them all go (below) */
            if (!(in.flags & 1))
            {
                g_releasing = 1;
                g_mod = 0;
                if (release_one(event))
                    return 1;
            }
            continue;

        case TE_IN_QUIT:
            event->type = SDL_QUIT;
            return 1;

        default:
            continue;
        }
    }
    return 0;
}

int SDL_WaitEvent(SDL_Event *event)
{
    while (!wolftrace_event_waiting())
        SDL_Delay(5);
    return event != NULL ? SDL_PollEvent(event) : 1;
}
