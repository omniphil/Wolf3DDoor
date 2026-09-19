/*
 * ansi_input.c -- a terminal's keystrokes turned into Wolfenstein's key presses and releases. See ansi_input.h.
 * Part 1 is the Doom door's, unchanged; part 2 is Wolfenstein's own keys.
 *
 * Two jobs:
 *   1. Bytes to keys: arrows, function keys and so on arrive as escape sequences, in whichever of the common forms
 *      the terminal uses (ESC [ A, ESC O A, ESC [ 11 ~ ...). A lone ESC is the Escape key once nothing follows it.
 *   2. Keys to the game: it wants to know when a key goes down and when it comes up, and a terminal only ever says
 *      "pressed". So a key is held from its first press until a little after its last repeat: tap an arrow and you
 *      step, hold it and the terminal's key repeat keeps you walking.
 */

#include <ctype.h>
#include <string.h>

#include "ansi_host.h"
#include "ansi_input.h"

/* ---- 1. bytes to keys ---- */

#define ESC_WAIT_MS 60          /* how long a lone ESC waits for the rest of a sequence */

static unsigned char g_seq[16];
static int           g_seq_len;
static long          g_seq_started;
static bool          g_after_cr;    /* telnet sends Enter as CR LF or CR NUL: the second byte isn't a key */

static int  g_keys[64];
static int  g_key_head, g_key_tail;

static void push_key(int key)
{
    int next = (g_key_tail + 1) % 64;
    if (next != g_key_head)
    {
        g_keys[g_key_tail] = key;
        g_key_tail = next;
    }
}

/* A finished CSI (ESC [ ...) or SS3 (ESC O x) sequence */
static void decode_sequence(void)
{
    unsigned char final = g_seq[g_seq_len - 1];
    int number = 0;

    if (g_seq_len >= 2 && g_seq[1] == 'O')
    {
        switch (final)
        {
        case 'A': push_key(KEY_T_UP); break;
        case 'B': push_key(KEY_T_DOWN); break;
        case 'C': push_key(KEY_T_RIGHT); break;
        case 'D': push_key(KEY_T_LEFT); break;
        case 'H': push_key(KEY_T_HOME); break;
        case 'F': push_key(KEY_T_END); break;
        case 'P': push_key(KEY_T_F1); break;
        case 'Q': push_key(KEY_T_F2); break;
        case 'R': push_key(KEY_T_F3); break;
        case 'S': push_key(KEY_T_F4); break;
        default: break;
        }
        return;
    }

    for (int i = 2; i < g_seq_len && isdigit(g_seq[i]); i++)
        number = number * 10 + (g_seq[i] - '0');

    switch (final)
    {
    case 'A': push_key(KEY_T_UP); break;
    case 'B': push_key(KEY_T_DOWN); break;
    case 'C': push_key(KEY_T_RIGHT); break;
    case 'D': push_key(KEY_T_LEFT); break;
    case 'H': push_key(KEY_T_HOME); break;
    case 'F': push_key(KEY_T_END); break;
    case 'K': push_key(KEY_T_END); break;     /* some BBS terminals send End as ESC [ K */
    case '~':
        switch (number)
        {
        case 1: case 7:  push_key(KEY_T_HOME); break;
        case 2:          push_key(KEY_T_INSERT); break;
        case 3:          push_key(KEY_T_DELETE); break;
        case 4: case 8:  push_key(KEY_T_END); break;
        case 5:          push_key(KEY_T_PGUP); break;
        case 6:          push_key(KEY_T_PGDN); break;
        case 11: push_key(KEY_T_F1); break;  case 12: push_key(KEY_T_F2); break;
        case 13: push_key(KEY_T_F3); break;  case 14: push_key(KEY_T_F4); break;
        case 15: push_key(KEY_T_F5); break;  case 17: push_key(KEY_T_F6); break;
        case 18: push_key(KEY_T_F7); break;  case 19: push_key(KEY_T_F8); break;
        case 20: push_key(KEY_T_F9); break;  case 21: push_key(KEY_T_F10); break;
        case 23: push_key(KEY_T_F11); break; case 24: push_key(KEY_T_F12); break;
        default: break;
        }
        break;
    default:
        break;      /* anything else (a terminal's reply to a query, say) is dropped */
    }
}

void ansi_input_feed(const unsigned char *data, int len, long now_ms)
{
    for (int i = 0; i < len; i++)
    {
        unsigned char c = data[i];

        if (g_seq_len > 0)
        {
            if (g_seq_len == 1)
            {
                if (c == '[' || c == 'O')
                {
                    g_seq[g_seq_len++] = c;
                    continue;
                }
                /* ESC then an ordinary key: Escape, then that key */
                g_seq_len = 0;
                push_key(KEY_T_ESC);
            }
            else
            {
                if (g_seq_len < (int)sizeof(g_seq))
                    g_seq[g_seq_len++] = c;
                /* SS3 is always one byte after the O; CSI ends at its first byte in @..~ */
                if (g_seq[1] == 'O' || (c >= 0x40 && c <= 0x7E))
                {
                    decode_sequence();
                    g_seq_len = 0;
                }
                continue;
            }
        }

        if (c == 27)
        {
            g_seq[0] = c;
            g_seq_len = 1;
            g_seq_started = now_ms;
            g_after_cr = false;
            continue;
        }
        if (g_after_cr && (c == '\n' || c == 0))
        {
            g_after_cr = false;
            continue;
        }
        g_after_cr = c == '\r';
        if (c == '\n')
            c = '\r';
        if (c == 127)
            c = 8;
        push_key(c);
    }
}

int ansi_input_next(long now_ms)
{
    int key;

    if (g_seq_len == 1 && now_ms - g_seq_started >= ESC_WAIT_MS)
    {
        g_seq_len = 0;
        push_key(KEY_T_ESC);
    }
    if (g_key_head == g_key_tail)
        return -1;
    key = g_keys[g_key_head];
    g_key_head = (g_key_head + 1) % 64;
    return key;
}

/* ---- 2. keys to Wolfenstein ---- */

/* Set-1 scancodes: what the module's input layer (module/src/sdl_trace.c) takes. ansi_host.c marks the arrow block
 * as E0 keys. */
enum
{
    SC_ESC = 0x01, SC_MINUS = 0x0C, SC_EQUALS = 0x0D, SC_BACKSPACE = 0x0E, SC_TAB = 0x0F, SC_ENTER = 0x1C,
    SC_CTRL = 0x1D, SC_LSHIFT = 0x2A, SC_ALT = 0x38, SC_SPACE = 0x39, SC_A = 0x1E, SC_D = 0x20,
    SC_UP = 0x48, SC_LEFT = 0x4B, SC_RIGHT = 0x4D, SC_DOWN = 0x50,
    SC_HOME = 0x47, SC_END = 0x4F, SC_PGUP = 0x49, SC_PGDN = 0x51, SC_INSERT = 0x52, SC_DELETE = 0x53,
};

static const unsigned char LETTER_SC[26] = {
    0x1E, 0x30, 0x2E, 0x20, 0x12, 0x21, 0x22, 0x23, 0x17, 0x24, 0x25, 0x26, 0x32,   /* a..m */
    0x31, 0x18, 0x19, 0x10, 0x13, 0x1F, 0x14, 0x16, 0x2F, 0x11, 0x2D, 0x15, 0x2C,   /* n..z */
};

/*
 * Holding keys down without a key-up. A terminal says a key was pressed and nothing else: hold an arrow and it sends
 * one press, waits for the keyboard's repeat delay (about half a second), then repeats quickly. Wolfenstein reads
 * keys as held-down state, so the door has to decide how long each one stays down:
 *
 *   moving   a tap is a nudge. Nothing happens through the rest of the repeat delay, then once the repeats arrive
 *   turning  the key is held steadily. The door can't tell a tap from the start of a hold until the repeats come, so
 *            whatever it does in that half second IS the tap: holding through it (first version) or creeping through
 *            it (second) made a tap move 1-3 tiles or turn ~55 degrees in the game (measured), far too sensitive
 *            (user, 2026-09-19). CREEP brings the creep back. Holding walks; R toggles running (Shift with every
 *            move), off to start.
 *   firing   every press is its own press and release: the pistol and knife only fire again once the key has been let
 *            go, so a held-down fire key fired once and then nothing. The machine gun and chaingun, which fire
 *            while held, are held instead.
 *
 * The repeat delay is learned from the caller's own first held key. The numbers are here to tune by feel.
 */
#define REPEAT_DELAY_MS     520     /* until one is measured */
#define BRIDGE_MARGIN_MS    80      /* pulsed past the delay, for the network bunching repeats up */
#define HOLD_STEP_MS        30      /* a tapped move: one frame (35 a second) */
#define HOLD_TURN_MS        30      /* a tapped turn: one frame */
#define CREEP               1       /* 1: one-frame steps through the repeat delay (see above); 0: wait instead */
#define PULSE_ON_MS         30      /* the creep through the repeat delay: one frame on... */
#define PULSE_OFF_MS        170     /* ...a turn steps every 200 ms: a slow creep, and a tap stays about 3 frames */
#define PULSE_OFF_MOVE_MS   300     /* moving covers ~1/4 tile a frame: one step mid-delay, a tap stays ~2 frames */
#define HOLD_FIRE_MS        70      /* long enough for two frames to see it */
#define FIRE_GAP_MS         50      /* let go this long before the next shot */
#define HOLD_TAP_MS         90      /* use, weapons: one press */
#define HOLD_MENU_MS        60      /* menus: Wolfenstein's read keys as held, so a press must last a frame or two */
#define HOLD_REPEAT_MS      150     /* after each repeat: longer than the gap between a terminal's repeats */

typedef struct
{
    bool down;
    long release_at;
    long first_at;          /* when the current hold began */
    bool repeating;         /* the terminal has started repeating it: it is being held */
    bool pulsing;           /* a turn between its tap and the repeats: pulsed on and off */
    long pulse_end;         /* when the repeat delay is over: stop pulsing */
    long next_pulse;        /* when the pulse goes on again */
    long last_press;        /* the last press or repeat of this key */
    int  pulse_off;         /* ms between creep steps: turns and moves differ */
} held_t;

static held_t g_held[128];
static long   g_repeat_delay = REPEAT_DELAY_MS;
static bool   g_run;                    /* R: Shift held with every move */
static bool   g_auto_fire;              /* the weapon in hand fires while held */
static bool   g_fire_again;             /* a fire press arrived while fire was down: shoot again after the gap */
static long   g_fire_up_at = -100000;   /* no shot yet */

static void key_down(int sc)
{
    if (!g_held[sc].down)
    {
        g_held[sc].down = true;
        ansi_host_key(sc, true);
    }
}

static void key_up(int sc)
{
    if (g_held[sc].down)
    {
        g_held[sc].down = false;
        ansi_host_key(sc, false);
    }
}

/* Forget a key completely: up, and no hold or pulse in progress */
static void key_drop(int sc)
{
    key_up(sc);
    g_held[sc].pulsing = false;
    g_held[sc].repeating = false;
    g_held[sc].last_press = 0;
}

/* A repeat of a key already being held: learn the terminal's repeat delay from the first one */
static void note_repeat(held_t *h, long now_ms)
{
    if (!h->repeating)
    {
        long d = now_ms - h->first_at;
        if (d >= 200 && d <= 900)
            g_repeat_delay = d;
        h->repeating = true;
    }
}

static bool in_hold(const held_t *h)
{
    return h->down || h->pulsing;
}

/* A plain press: down for first_ms, and kept down by repeats */
static void press(int sc, int first_ms, long now_ms)
{
    held_t *h;

    if (sc <= 0 || sc >= 128)
        return;
    h = &g_held[sc];
    if (h->down)
    {
        note_repeat(h, now_ms);
        if (h->release_at < now_ms + HOLD_REPEAT_MS)
            h->release_at = now_ms + HOLD_REPEAT_MS;
        return;
    }
    h->first_at = now_ms;
    h->repeating = false;
    h->release_at = now_ms + first_ms;
    key_down(sc);
}

/*
 * Moving and turning: a short press for a tap, steady once the repeats arrive. A press within the repeat delay of the
 * last one is a repeat even though the tap's nudge has already let go: the first repeat comes ~half a second after
 * the press, and over a network later ones can come 50-100 ms apart, so treating each as a new tap turned a held key
 * into a string of nudges (user: "holding doesn't really work too well", 2026-09-19).
 */
static void press_bridged(int sc, int opposite, int tap_ms, int pulse_off_ms, long now_ms)
{
    held_t *h = &g_held[sc];
    bool again = h->last_press != 0 && now_ms - h->last_press <= g_repeat_delay + BRIDGE_MARGIN_MS;

    key_drop(opposite);
    h->last_press = now_ms;
    if (in_hold(h) || again)
    {
        note_repeat(h, now_ms);
        h->pulsing = false;
        key_down(sc);
        if (h->release_at < now_ms + HOLD_REPEAT_MS)
            h->release_at = now_ms + HOLD_REPEAT_MS;
        return;
    }
    h->first_at = now_ms;
    h->repeating = false;
    h->pulsing = false;
    h->pulse_end = CREEP ? now_ms + g_repeat_delay + BRIDGE_MARGIN_MS : now_ms;
    h->pulse_off = pulse_off_ms;
    h->release_at = now_ms + tap_ms;
    key_down(sc);
}

/* Running, when R has it on: Shift held for as long as the move */
static void run_with(int sc, long now_ms)
{
    if (!g_run)
        return;
    press(SC_LSHIFT, 0, now_ms);
    if (g_held[SC_LSHIFT].release_at < g_held[sc].release_at)
        g_held[SC_LSHIFT].release_at = g_held[sc].release_at;
    if (g_held[SC_LSHIFT].release_at < g_held[sc].pulse_end)
        g_held[SC_LSHIFT].release_at = g_held[sc].pulse_end;
}

bool ansi_input_running(void)
{
    return g_run;
}

/* Firing: one press and release per keypress for the pistol and knife; held for the automatic weapons */
static void press_fire(long now_ms)
{
    held_t *h = &g_held[SC_CTRL];

    if (g_auto_fire)
    {
        press(SC_CTRL, HOLD_REPEAT_MS, now_ms);
        return;
    }
    if (h->down || now_ms < g_fire_up_at + FIRE_GAP_MS)
    {
        g_fire_again = true;        /* shoot again as soon as the game has seen this shot's release */
        return;
    }
    h->first_at = now_ms;
    h->release_at = now_ms + HOLD_FIRE_MS;
    key_down(SC_CTRL);
}

void ansi_input_auto_fire(bool on)
{
    g_auto_fire = on;
}

static int function_scancode(int key)
{
    if (key >= KEY_T_F1 && key <= KEY_T_F10)
        return 0x3B + (key - KEY_T_F1);
    if (key == KEY_T_F11)
        return 0x57;
    if (key == KEY_T_F12)
        return 0x58;
    return 0;
}

/* The key as itself, for menus */
static int plain_scancode(int key)
{
    switch (key)
    {
    case KEY_T_UP: return SC_UP;         case KEY_T_DOWN: return SC_DOWN;
    case KEY_T_LEFT: return SC_LEFT;     case KEY_T_RIGHT: return SC_RIGHT;
    case KEY_T_HOME: return SC_HOME;     case KEY_T_END: return SC_END;
    case KEY_T_PGUP: return SC_PGUP;     case KEY_T_PGDN: return SC_PGDN;
    case KEY_T_INSERT: return SC_INSERT; case KEY_T_DELETE: return SC_DELETE;
    case KEY_T_ESC: return SC_ESC;
    case '\r': return SC_ENTER;
    case 8: case 127: return SC_BACKSPACE;
    case '\t': return SC_TAB;
    case ' ': return SC_SPACE;
    case '-': case '_': return SC_MINUS;
    case '=': case '+': return SC_EQUALS;
    default: break;
    }
    if (key >= '1' && key <= '9')
        return 0x02 + (key - '1');
    if (key == '0')
        return 0x0B;
    if (key < 128 && isalpha(key))
        return LETTER_SC[tolower(key) - 'a'];
    return function_scancode(key);
}

void ansi_input_key(int key, bool menu, bool typing, long now_ms)
{
    if (typing)
    {
        /* A saved game's name: letters arrive as typed text; Enter, Backspace and Esc as keys */
        if (key >= 32 && key < 127)
            ansi_host_text(key);
        else
            press(plain_scancode(key), HOLD_MENU_MS, now_ms);
        return;
    }
    if (menu)
    {
        press(plain_scancode(key), HOLD_MENU_MS, now_ms);
        /* The menus also jump to an item by its first letter, which they read as typed text */
        if (key < 128 && isalpha(key))
            ansi_host_text(key);
        return;
    }

    switch (key)
    {
    /* Moving: arrows or W/S */
    case KEY_T_UP: case 'w': case 'W':
        press_bridged(SC_UP, SC_DOWN, HOLD_STEP_MS, PULSE_OFF_MOVE_MS, now_ms);
        run_with(SC_UP, now_ms);
        return;
    case KEY_T_DOWN: case 's': case 'S':
        press_bridged(SC_DOWN, SC_UP, HOLD_STEP_MS, PULSE_OFF_MOVE_MS, now_ms);
        run_with(SC_DOWN, now_ms);
        return;

    /* Turning */
    case KEY_T_LEFT:
        press_bridged(SC_LEFT, SC_RIGHT, HOLD_TURN_MS, PULSE_OFF_MS, now_ms);
        return;
    case KEY_T_RIGHT:
        press_bridged(SC_RIGHT, SC_LEFT, HOLD_TURN_MS, PULSE_OFF_MS, now_ms);
        return;

    /* Strafing: A and D, which the game itself now takes (../patches/wasd-keys.patch) */
    case 'a': case 'A': case ',': case '<':
        press_bridged(SC_A, SC_D, HOLD_STEP_MS, PULSE_OFF_MOVE_MS, now_ms);
        run_with(SC_A, now_ms);
        return;
    case 'd': case 'D': case '.': case '>':
        press_bridged(SC_D, SC_A, HOLD_STEP_MS, PULSE_OFF_MOVE_MS, now_ms);
        run_with(SC_D, now_ms);
        return;

    /* R: running on or off */
    case 'r': case 'R':
        g_run = !g_run;
        return;

    /* Fire is Ctrl, which a terminal can't send on its own */
    case 'f': case 'F': case 'j': case 'J':
        press_fire(now_ms);
        return;

    /* Open doors, push walls, flip the level's switch */
    case ' ': case 'e': case 'E':
        press(SC_SPACE, HOLD_TAP_MS, now_ms);
        return;

    default:
        press(plain_scancode(key), HOLD_TAP_MS, now_ms);
        return;
    }
}

void ansi_input_release_due(long now_ms)
{
    for (int sc = 1; sc < 128; sc++)
    {
        held_t *h = &g_held[sc];

        if (h->down && now_ms >= h->release_at)
        {
            key_up(sc);
            if (sc == SC_CTRL)
                g_fire_up_at = now_ms;
            /* A move or turn let go before the repeat delay is over: keep it creeping until the repeats can arrive */
            if (!h->repeating && now_ms < h->pulse_end)
            {
                h->pulsing = true;
                h->next_pulse = now_ms + (h->pulse_off > 0 ? h->pulse_off : PULSE_OFF_MS);
            }
            else
            {
                h->pulsing = false;
            }
        }
        else if (h->pulsing && !h->down)
        {
            if (now_ms >= h->pulse_end)
                h->pulsing = false;         /* no repeats came: it was a tap */
            else if (now_ms >= h->next_pulse)
            {
                h->release_at = now_ms + PULSE_ON_MS;
                key_down(sc);
            }
        }
    }

    /* The next shot of a quick burst on the pistol or knife, once the game has seen the last one let go */
    if (g_fire_again && !g_held[SC_CTRL].down && now_ms >= g_fire_up_at + FIRE_GAP_MS)
    {
        g_fire_again = false;
        g_held[SC_CTRL].release_at = now_ms + HOLD_FIRE_MS;
        key_down(SC_CTRL);
    }
}

void ansi_input_release_all(void)
{
    for (int sc = 1; sc < 128; sc++)
        key_drop(sc);
    g_fire_again = false;
}
