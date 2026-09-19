/*
 * SDL.h -- the part of SDL2 that Wolf4SDL uses, answered by TRACE instead (src/sdl_trace.c, src/mixer_trace.c).
 *
 * Wolf4SDL is written against SDL2 and SDL2_mixer. There is no SDL in the sandbox, so this header declares just the
 * types, constants and calls the game's own files use, with SDL's own values where the game stores them (scancodes
 * end up in config.wl1, so they must be SDL's numbers). Anything else won't compile, which is deliberate: a new SDL
 * call in a future Wolf4SDL shows up here rather than failing quietly at run time.
 *
 * The keyboard block is the same as the Tyrian module's (BBSGames/Tyrian/module/include/SDL.h).
 */

#ifndef WOLFTRACE_SDL_H
#define WOLFTRACE_SDL_H

#include <math.h>          /* the real SDL_stdinc.h brings these in, and the game relies on it */
#include <stdarg.h>
#include <stdio.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- SDL_types.h / SDL_stdinc.h ---- */

typedef int8_t   Sint8;
typedef uint8_t  Uint8;
typedef int16_t  Sint16;
typedef uint16_t Uint16;
typedef int32_t  Sint32;
typedef uint32_t Uint32;
typedef int64_t  Sint64;
typedef uint64_t Uint64;

typedef enum { SDL_FALSE = 0, SDL_TRUE = 1 } SDL_bool;

#define SDL_MAIN_HANDLED 1

/* ---- init, errors, messages ---- */

#define SDL_INIT_AUDIO    0x00000010u
#define SDL_INIT_VIDEO    0x00000020u
#define SDL_INIT_JOYSTICK 0x00000200u

int  SDL_Init(Uint32 flags);
void SDL_Quit(void);
const char *SDL_GetError(void);
int  SDL_SetHint(const char *name, const char *value);
#define SDL_HINT_RENDER_SCALE_QUALITY "SDL_RENDER_SCALE_QUALITY"

#define SDL_MESSAGEBOX_ERROR       0x10
#define SDL_MESSAGEBOX_INFORMATION 0x40
typedef struct SDL_Window SDL_Window;
int SDL_ShowSimpleMessageBox(Uint32 flags, const char *title, const char *message, SDL_Window *window);

/* ---- time ---- */

Uint32 SDL_GetTicks(void);
void   SDL_Delay(Uint32 ms);

/* ---- surfaces: in memory. The game draws into an 8-bit one and copies it to a 32-bit one to show it ---- */

typedef struct SDL_Color { Uint8 r, g, b, a; } SDL_Color;
typedef struct SDL_Rect { int x, y, w, h; } SDL_Rect;
typedef struct SDL_Palette { int ncolors; SDL_Color colors[256]; } SDL_Palette;

typedef struct SDL_PixelFormat
{
    Uint32 format;
    SDL_Palette *palette;
    Uint8  BitsPerPixel;
    Uint8  BytesPerPixel;
} SDL_PixelFormat;

typedef struct SDL_Surface
{
    Uint32 flags;
    SDL_PixelFormat *format;
    int w, h;
    int pitch;
    void *pixels;
} SDL_Surface;

#define SDL_MUSTLOCK(s) 0
#define SDL_ALPHA_OPAQUE 255
#define SDL_PIXELFORMAT_ARGB8888 0x16362004u

SDL_bool SDL_PixelFormatEnumToMasks(Uint32 format, int *bpp, Uint32 *rmask, Uint32 *gmask, Uint32 *bmask,
                                    Uint32 *amask);
SDL_Surface *SDL_CreateRGBSurface(Uint32 flags, int width, int height, int depth,
                                  Uint32 rmask, Uint32 gmask, Uint32 bmask, Uint32 amask);
void SDL_FreeSurface(SDL_Surface *surface);
static inline int  SDL_LockSurface(SDL_Surface *s) { (void)s; return 0; }
static inline void SDL_UnlockSurface(SDL_Surface *s) { (void)s; }
int  SDL_FillRect(SDL_Surface *dst, const SDL_Rect *rect, Uint32 color);
/* 8-bit to 8-bit copies as is; 8-bit to 32-bit goes through the source's palette (that is how a frame is shown) */
int  SDL_BlitSurface(SDL_Surface *src, const SDL_Rect *srcrect, SDL_Surface *dst, SDL_Rect *dstrect);
int  SDL_SetPaletteColors(SDL_Palette *palette, const SDL_Color *colors, int first, int ncolors);
/* TRACE's frames are BGRA (0xAARRGGBB), the same as SDL's ARGB8888 on a little-endian machine */
Uint32 SDL_MapRGBA(const SDL_PixelFormat *format, Uint8 r, Uint8 g, Uint8 b, Uint8 a);
/* Screenshots (a debug key): there is nowhere to keep one */
static inline int SDL_SaveBMP(SDL_Surface *s, const char *file) { (void)s; (void)file; return -1; }

/* ---- memory "files" (the game builds each digitised sound as a WAV in memory) ---- */

typedef struct SDL_RWops { const Uint8 *data; size_t size; } SDL_RWops;
SDL_RWops *SDL_RWFromMem(void *mem, int size);

/* ---- the window: there is one picture, TERMinator's. The renderer's Present is what shows a frame. ---- */

typedef struct SDL_Renderer SDL_Renderer;
typedef struct SDL_Texture SDL_Texture;

#define SDL_WINDOWPOS_UNDEFINED 0x1FFF0000
#define SDL_WINDOW_FULLSCREEN   0x00000001u
#define SDL_WINDOW_OPENGL       0x00000002u
#define SDL_RENDERER_ACCELERATED  0x00000002u
#define SDL_RENDERER_PRESENTVSYNC 0x00000004u
#define SDL_TEXTUREACCESS_STREAMING 1
#define SDL_BLENDMODE_BLEND 1

SDL_Window   *SDL_CreateWindow(const char *title, int x, int y, int w, int h, Uint32 flags);
void          SDL_DestroyWindow(SDL_Window *window);
SDL_Renderer *SDL_CreateRenderer(SDL_Window *window, int index, Uint32 flags);
void          SDL_DestroyRenderer(SDL_Renderer *renderer);
static inline int SDL_SetRenderDrawBlendMode(SDL_Renderer *r, int mode) { (void)r; (void)mode; return 0; }
SDL_Texture  *SDL_CreateTexture(SDL_Renderer *renderer, Uint32 format, int access, int w, int h);
void          SDL_DestroyTexture(SDL_Texture *texture);
int           SDL_UpdateTexture(SDL_Texture *texture, const SDL_Rect *rect, const void *pixels, int pitch);
static inline int SDL_RenderCopy(SDL_Renderer *r, SDL_Texture *t, const SDL_Rect *s, const SDL_Rect *d)
{ (void)r; (void)t; (void)s; (void)d; return 0; }
void          SDL_RenderPresent(SDL_Renderer *renderer);
static inline void SDL_SetWindowGrab(SDL_Window *w, SDL_bool grabbed) { (void)w; (void)grabbed; }
static inline void SDL_WarpMouseInWindow(SDL_Window *w, int x, int y) { (void)w; (void)x; (void)y; }

#define SDL_ENABLE  1
#define SDL_DISABLE 0
#define SDL_IGNORE  0
#define SDL_QUERY  -1
static inline int SDL_ShowCursor(int toggle) { (void)toggle; return 0; }
static inline int SDL_SetRelativeMouseMode(SDL_bool enabled) { (void)enabled; return 0; }

/* ---- keyboard ---- */

typedef enum
{
    SDL_SCANCODE_UNKNOWN = 0,

    SDL_SCANCODE_A = 4, SDL_SCANCODE_B, SDL_SCANCODE_C, SDL_SCANCODE_D, SDL_SCANCODE_E, SDL_SCANCODE_F,
    SDL_SCANCODE_G, SDL_SCANCODE_H, SDL_SCANCODE_I, SDL_SCANCODE_J, SDL_SCANCODE_K, SDL_SCANCODE_L,
    SDL_SCANCODE_M, SDL_SCANCODE_N, SDL_SCANCODE_O, SDL_SCANCODE_P, SDL_SCANCODE_Q, SDL_SCANCODE_R,
    SDL_SCANCODE_S, SDL_SCANCODE_T, SDL_SCANCODE_U, SDL_SCANCODE_V, SDL_SCANCODE_W, SDL_SCANCODE_X,
    SDL_SCANCODE_Y, SDL_SCANCODE_Z,

    SDL_SCANCODE_1 = 30, SDL_SCANCODE_2, SDL_SCANCODE_3, SDL_SCANCODE_4, SDL_SCANCODE_5,
    SDL_SCANCODE_6, SDL_SCANCODE_7, SDL_SCANCODE_8, SDL_SCANCODE_9, SDL_SCANCODE_0,

    SDL_SCANCODE_RETURN = 40, SDL_SCANCODE_ESCAPE = 41, SDL_SCANCODE_BACKSPACE = 42, SDL_SCANCODE_TAB = 43,
    SDL_SCANCODE_SPACE = 44, SDL_SCANCODE_MINUS = 45, SDL_SCANCODE_EQUALS = 46, SDL_SCANCODE_LEFTBRACKET = 47,
    SDL_SCANCODE_RIGHTBRACKET = 48, SDL_SCANCODE_BACKSLASH = 49, SDL_SCANCODE_NONUSHASH = 50,
    SDL_SCANCODE_SEMICOLON = 51, SDL_SCANCODE_APOSTROPHE = 52, SDL_SCANCODE_GRAVE = 53, SDL_SCANCODE_COMMA = 54,
    SDL_SCANCODE_PERIOD = 55, SDL_SCANCODE_SLASH = 56, SDL_SCANCODE_CAPSLOCK = 57,

    SDL_SCANCODE_F1 = 58, SDL_SCANCODE_F2, SDL_SCANCODE_F3, SDL_SCANCODE_F4, SDL_SCANCODE_F5, SDL_SCANCODE_F6,
    SDL_SCANCODE_F7, SDL_SCANCODE_F8, SDL_SCANCODE_F9, SDL_SCANCODE_F10, SDL_SCANCODE_F11, SDL_SCANCODE_F12,

    SDL_SCANCODE_PRINTSCREEN = 70, SDL_SCANCODE_SCROLLLOCK = 71, SDL_SCANCODE_PAUSE = 72, SDL_SCANCODE_INSERT = 73,
    SDL_SCANCODE_HOME = 74, SDL_SCANCODE_PAGEUP = 75, SDL_SCANCODE_DELETE = 76, SDL_SCANCODE_END = 77,
    SDL_SCANCODE_PAGEDOWN = 78, SDL_SCANCODE_RIGHT = 79, SDL_SCANCODE_LEFT = 80, SDL_SCANCODE_DOWN = 81,
    SDL_SCANCODE_UP = 82,

    SDL_SCANCODE_NUMLOCKCLEAR = 83, SDL_SCANCODE_KP_DIVIDE = 84, SDL_SCANCODE_KP_MULTIPLY = 85,
    SDL_SCANCODE_KP_MINUS = 86, SDL_SCANCODE_KP_PLUS = 87, SDL_SCANCODE_KP_ENTER = 88,
    SDL_SCANCODE_KP_1 = 89, SDL_SCANCODE_KP_2, SDL_SCANCODE_KP_3, SDL_SCANCODE_KP_4, SDL_SCANCODE_KP_5,
    SDL_SCANCODE_KP_6, SDL_SCANCODE_KP_7, SDL_SCANCODE_KP_8, SDL_SCANCODE_KP_9, SDL_SCANCODE_KP_0,
    SDL_SCANCODE_KP_PERIOD = 99, SDL_SCANCODE_NONUSBACKSLASH = 100,

    SDL_SCANCODE_LCTRL = 224, SDL_SCANCODE_LSHIFT = 225, SDL_SCANCODE_LALT = 226, SDL_SCANCODE_LGUI = 227,
    SDL_SCANCODE_RCTRL = 228, SDL_SCANCODE_RSHIFT = 229, SDL_SCANCODE_RALT = 230, SDL_SCANCODE_RGUI = 231,

    SDL_NUM_SCANCODES = 512
} SDL_Scancode;

typedef Sint32 SDL_Keycode;
#define SDLK_SCANCODE_MASK (1 << 30)
#define SDL_SCANCODE_TO_KEYCODE(x) ((SDL_Keycode)(x) | SDLK_SCANCODE_MASK)
enum
{
    SDLK_UNKNOWN = 0,
    SDLK_RIGHTBRACKET = ']',
    SDLK_a = 'a', SDLK_d = 'd', SDLK_g = 'g', SDLK_l = 'l', SDLK_o = 'o', SDLK_r = 'r', SDLK_s = 's',
};

typedef enum
{
    KMOD_NONE = 0x0000,
    KMOD_LSHIFT = 0x0001, KMOD_RSHIFT = 0x0002,
    KMOD_LCTRL = 0x0040, KMOD_RCTRL = 0x0080,
    KMOD_LALT = 0x0100, KMOD_RALT = 0x0200,
    KMOD_LGUI = 0x0400, KMOD_RGUI = 0x0800,
    KMOD_NUM = 0x1000, KMOD_CAPS = 0x2000,
    KMOD_CTRL = KMOD_LCTRL | KMOD_RCTRL,
    KMOD_SHIFT = KMOD_LSHIFT | KMOD_RSHIFT,
    KMOD_ALT = KMOD_LALT | KMOD_RALT,
    KMOD_GUI = KMOD_LGUI | KMOD_RGUI,
} SDL_Keymod;

typedef struct SDL_Keysym
{
    SDL_Scancode scancode;
    SDL_Keycode sym;
    Uint16 mod;
} SDL_Keysym;

SDL_Keymod SDL_GetModState(void);

/* ---- events ---- */

#define SDL_RELEASED 0
#define SDL_PRESSED  1

typedef enum
{
    SDL_QUIT = 0x100,
    SDL_WINDOWEVENT = 0x200,
    SDL_KEYDOWN = 0x300, SDL_KEYUP, SDL_TEXTEDITING, SDL_TEXTINPUT,
    SDL_MOUSEMOTION = 0x400, SDL_MOUSEBUTTONDOWN, SDL_MOUSEBUTTONUP, SDL_MOUSEWHEEL,
    SDL_JOYBUTTONDOWN = 0x603, SDL_JOYBUTTONUP,
} SDL_EventType;

#define SDL_TEXTINPUTEVENT_TEXT_SIZE 32

typedef struct SDL_KeyboardEvent { Uint32 type; Uint8 state; Uint8 repeat; SDL_Keysym keysym; } SDL_KeyboardEvent;
typedef struct SDL_TextInputEvent { Uint32 type; char text[SDL_TEXTINPUTEVENT_TEXT_SIZE]; } SDL_TextInputEvent;
typedef struct SDL_JoyButtonEvent { Uint32 type; Uint8 button, state; } SDL_JoyButtonEvent;

typedef union SDL_Event
{
    Uint32 type;
    SDL_KeyboardEvent key;
    SDL_TextInputEvent text;
    SDL_JoyButtonEvent jbutton;
} SDL_Event;

int SDL_PollEvent(SDL_Event *event);
/* Waits until an event is there; with NULL it is left for the next SDL_PollEvent */
int SDL_WaitEvent(SDL_Event *event);
static inline Uint8 SDL_EventState(Uint32 type, int state) { (void)type; (void)state; return 0; }

/* ---- mouse: none (TERMinator doesn't pass one to modules yet) ---- */

#define SDL_BUTTON(x)      (1 << ((x) - 1))
#define SDL_BUTTON_LEFT    1
#define SDL_BUTTON_MIDDLE  2
#define SDL_BUTTON_RIGHT   3
static inline Uint32 SDL_GetMouseState(int *x, int *y) { if (x) *x = 0; if (y) *y = 0; return 0; }
static inline Uint32 SDL_GetRelativeMouseState(int *x, int *y) { if (x) *x = 0; if (y) *y = 0; return 0; }

/* ---- joysticks: none ---- */

typedef struct SDL_Joystick SDL_Joystick;
#define SDL_HAT_CENTERED 0x00
#define SDL_HAT_UP       0x01
#define SDL_HAT_RIGHT    0x02
#define SDL_HAT_DOWN     0x04
#define SDL_HAT_LEFT     0x08

static inline int SDL_NumJoysticks(void) { return 0; }
static inline SDL_Joystick *SDL_JoystickOpen(int index) { (void)index; return NULL; }
static inline void SDL_JoystickClose(SDL_Joystick *j) { (void)j; }
static inline int SDL_JoystickNumButtons(SDL_Joystick *j) { (void)j; return 0; }
static inline int SDL_JoystickNumHats(SDL_Joystick *j) { (void)j; return 0; }
static inline Sint16 SDL_JoystickGetAxis(SDL_Joystick *j, int n) { (void)j; (void)n; return 0; }
static inline Uint8 SDL_JoystickGetButton(SDL_Joystick *j, int n) { (void)j; (void)n; return 0; }
static inline Uint8 SDL_JoystickGetHat(SDL_Joystick *j, int n) { (void)j; (void)n; return 0; }
static inline void SDL_JoystickUpdate(void) { }

/* ---- audio formats (SDL_mixer.h has the rest) ---- */

typedef Uint16 SDL_AudioFormat;
#define AUDIO_S16LSB 0x8010
#define AUDIO_S16    AUDIO_S16LSB
#define AUDIO_S16SYS AUDIO_S16LSB
#define SDL_AUDIO_ALLOW_FREQUENCY_CHANGE 0x01

#ifdef __cplusplus
}
#endif

#endif /* WOLFTRACE_SDL_H */
