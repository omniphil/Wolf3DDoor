/*
 * main.c -- WOLFENSTEIN 3D, as a BBS door. (The DOOM door's main.c, for Wolfenstein.)
 *
 * Two ways to play, picked by the player on the start page:
 *   TRACE  the door sends the game and TERMinator runs the whole of Wolfenstein in its sandbox on the player's own
 *          machine, so there are no pictures on the wire and it plays at full speed with its AdLib music and sound.
 *   ANSI   for every other terminal: the game runs here on the BBS and the door sends it as ANSI pictures, in 24-bit,
 *          256 or 16 colours (ansi_play.c). Blocky and silent, but it's Wolfenstein in a BBS terminal.
 *
 * The shareware episode ("Escape from Wolfenstein", v1.4), which id Software allowed to be passed on unchanged. The
 * game is Wolf4SDL, built from id's source release.
 */

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "ansi_play.h"
#include "door.h"
#include "files.h"
#include "trace_wolf.h"

#define CSI "\033["

static void cls(void)
{
    door_write(CSI "0m" CSI "2J" CSI "H");
}

static void title(void)
{
    cls();
    door_write(CSI "1;31m"
               "        ===============================================\r\n"
               "          W O L F E N S T E I N   3 D   -   episode 1\r\n"
               "        ===============================================\r\n" CSI "0m\r\n");
}

/* The name in its own colours, the way the client writes it: TERM in magenta, inator in cyan. */
static void write_terminator(void)
{
    door_write(CSI "1;35m" "TERM" CSI "1;36m" "inator" CSI "0m");
}

static void press_any_key(void)
{
    door_write(CSI "0;37m\r\n  Press any key to return to the BBS...\r\n" CSI "0m");
    door_read_char();
}

static long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

static void sleep_ms(int ms)
{
    struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

/*
 * "Detecting TRACE graphics...", centred, with a dot appearing every quarter second for a couple of seconds.
 * The look-and-see itself only takes a moment, so this is mostly for the player's benefit: something is happening,
 * and the screen isn't about to sit there silently. Returns what the terminal turned out to be.
 */
static bool detect_with_animation(void)
{
    static const char message[] = "Detecting TRACE graphics";   /* written in pieces below, TRACE in its own colour */
    const int dots = 8;                          /* 8 quarter-seconds: about two seconds in all */
    const int width = (int)sizeof(message) - 1 + dots;
    int column = (80 - width) / 2 + 1;           /* an 80-column screen, which every BBS terminal has */
    bool found;

    cls();
    door_write(CSI "12;1H");                     /* half way down */
    {
        char at[16];
        snprintf(at, sizeof(at), CSI "%dC", column - 1);
        door_write(at);
    }
    door_write(CSI "0;37m" "Detecting " CSI "1;35m" "TRACE" CSI "0;37m" " graphics.");

    {
        long start = now_ms();
        int shown = 1;

        sleep_ms(250);

        /* The actual question to the terminal, which answers in milliseconds, or not at all when there's no TRACE */
        found = trace_wolf_detect();

        /* Keep the dots going to the two-second mark, however long that answer took */
        while (shown < dots)
        {
            long elapsed = now_ms() - start;
            if (elapsed >= 2000)
                break;
            if (elapsed >= (long)shown * 250)
            {
                door_write(".");
                shown++;
            }
            else
            {
                sleep_ms(25);
            }
        }
    }
    door_write(CSI "0m");
    return found;
}

/* Drawn while the game data goes up the first time, so the wait doesn't look like a hung door. */
static void data_progress(int percent)
{
    char bar[80];
    int filled = percent * 40 / 100;

    memset(bar, ' ', sizeof(bar));
    memcpy(bar, "  [", 3);
    for (int i = 0; i < 40; i++)
        bar[3 + i] = i < filled ? '#' : '.';
    snprintf(bar + 43, sizeof(bar) - 43, "] %3d%%", percent);
    door_write(CSI "s");                 /* remember where we are */
    door_write("\r");
    door_write(bar);
    door_write(CSI "u");
}

/* ---- choosing how to play ---- */

/* What the caller's terminal turned out to be */
typedef struct
{
    bool trace;             /* TERMinator with TRACE: the real game on their own machine */
    bool cterm;             /* SyncTERM's terminal (SyncTERM, TERMinator): answers "who are you" with its version */
    int  cterm_major, cterm_minor;
} terminal_t;

/* CTerm from this version on understands 256 and 24-bit colour. TERMinator reports 1.324; SyncTERM 1.2 and later
 * report a 1.3xx version too. Older, or anything else, is marked UNKNOWN rather than hidden: the test strips on the
 * menu let the player see for themselves. */
#define CTERM_COLOR_MAJOR 1
#define CTERM_COLOR_MINOR 300

/*
 * Asks the terminal who it is (ESC [ c). SyncTERM's terminal answers ESC [ = 67;84;101;114;109;<major>;<minor> c,
 * "CTerm" in ASCII followed by its version. Others answer differently or not at all.
 */
static void detect_cterm(terminal_t *term)
{
    char reply[64];
    int n = 0, c;
    const char *p;

    door_write(CSI "c");
    c = door_read_char_timeout(500);
    while (c >= 0 && n < (int)sizeof(reply) - 1)
    {
        reply[n++] = (char)c;
        if (c == 'c' && n > 2)
            break;
        c = door_read_char_timeout(200);
    }
    reply[n] = '\0';

    p = strstr(reply, "[=67;84;101;114;109;");
    if (p != NULL && sscanf(p + strlen("[=67;84;101;114;109;"), "%d;%d", &term->cterm_major, &term->cterm_minor) == 2)
        term->cterm = true;
}

static bool cterm_has_color(const terminal_t *term)
{
    return term->cterm && (term->cterm_major > CTERM_COLOR_MAJOR ||
                           (term->cterm_major == CTERM_COLOR_MAJOR && term->cterm_minor >= CTERM_COLOR_MINOR));
}

enum { CHOICE_TRACE = 1, CHOICE_24BIT, CHOICE_256, CHOICE_16 };

/* The player's last choice, kept in their own folder beside their savegames */
static void choice_path(char *out, size_t size)
{
    snprintf(out, size, "%s/display.cfg", files_folder());
}

static void load_choice(int *choice, bool *utf8)
{
    char path[640];
    FILE *fp;
    int c = 0, u = 0;

    choice_path(path, sizeof(path));
    fp = fopen(path, "r");
    if (fp == NULL)
        return;
    if (fscanf(fp, "display=%d utf8=%d", &c, &u) >= 1 && c >= CHOICE_TRACE && c <= CHOICE_16)
    {
        *choice = c;
        *utf8 = u != 0;
    }
    fclose(fp);
}

static void save_choice(int choice, bool utf8)
{
    char path[640];
    FILE *fp;

    choice_path(path, sizeof(path));
    fp = fopen(path, "w");
    if (fp == NULL)
        return;
    fprintf(fp, "display=%d utf8=%d\n", choice, utf8 ? 1 : 0);
    fclose(fp);
}

/* ▀ in CP437, or in UTF-8 for a terminal that expects it */
static const char *upper_half(bool utf8)
{
    return utf8 ? "\xE2\x96\x80" : "\xDF";
}

/* A row of half-blocks: a rainbow on top and a grey ramp underneath, in 24-bit or 256 colours. If the terminal
 * understands them, it's a smooth gradient; if it doesn't, it's garbage or flat bands. */
static void test_strip(bool truecolor, bool utf8)
{
    char seq[64];

    for (int i = 0; i < 48; i++)
    {
        float h = i / 48.0f * 6.0f;
        int sector = (int)h;
        float f = h - (float)sector;
        int up = (int)(255 * f), down = 255 - up;
        int r, g, b, grey = 16 + i * 230 / 47;
        switch (sector)
        {
        case 0:  r = 255;  g = up;   b = 0;    break;
        case 1:  r = down; g = 255;  b = 0;    break;
        case 2:  r = 0;    g = 255;  b = up;   break;
        case 3:  r = 0;    g = down; b = 255;  break;
        case 4:  r = up;   g = 0;    b = 255;  break;
        default: r = 255;  g = 0;    b = down; break;
        }
        if (truecolor)
            snprintf(seq, sizeof(seq), CSI "38;2;%d;%d;%d;48;2;%d;%d;%dm", r, g, b, grey, grey, grey);
        else
            snprintf(seq, sizeof(seq), CSI "38;5;%d;48;5;%dm", 16 + 36 * (r * 5 / 255) + 6 * (g * 5 / 255) + (b * 5 / 255),
                     232 + i * 23 / 47);
        door_write(seq);
        door_write(upper_half(utf8));
    }
    door_write(CSI "0m");
}

/* The 16-colour blocks and shades the ANSI picture is made of */
static void block_strip(bool utf8)
{
    static const char *cp437[] = { "\xB0", "\xB1", "\xB2", "\xDB", "\xDC", "\xDF", "\xDD", "\xDE" };
    static const char *utf[] = { "\xE2\x96\x91", "\xE2\x96\x92", "\xE2\x96\x93", "\xE2\x96\x88",
                                 "\xE2\x96\x84", "\xE2\x96\x80", "\xE2\x96\x8C", "\xE2\x96\x90" };
    static const char *colors[] = { "0;31", "0;33", "0;32", "0;36", "0;34", "0;35", "1;31", "1;33", "1;32", "1;36",
                                    "1;34", "1;35" };
    char seq[24];

    for (int i = 0; i < 48; i++)
    {
        snprintf(seq, sizeof(seq), CSI "%s;40m", colors[(i / 8) % 12]);
        door_write(seq);
        door_write(utf8 ? utf[i % 8] : cp437[i % 8]);
    }
    door_write(CSI "0m");
}

static void draw_menu(const terminal_t *term, int recommended, bool utf8)
{
    bool color = cterm_has_color(term);
    char line[32];

    title();
    door_write(CSI "0;37m  Choose how to play:\r\n\r\n");

    if (term->trace)
        door_write(CSI "1;37m  [1] " CSI "1;35mTRACE" CSI "1;37m graphics   " CSI "0;37m(640x400 + Sound)      "
                   CSI "1;32mDETECTED\r\n");
    else
        door_write(CSI "1;30m  [1] TRACE graphics   (640x400 + Sound)      NOT FOUND (needs " CSI "1;35mTERM"
                   CSI "1;36minator" CSI "1;30m)\r\n");
    door_write(CSI "1;37m  [2] ANSI 24-bit      " CSI "0;37m(best look)            ");
    door_write(color ? CSI "1;32mDETECTED\r\n" : CSI "1;33mUNKNOWN\r\n");
    door_write(CSI "1;37m  [3] ANSI 256         " CSI "0;37m(faster, near 24-bit)  ");
    door_write(color ? CSI "1;32mDETECTED\r\n" : CSI "1;33mUNKNOWN\r\n");
    door_write(CSI "1;37m  [4] ANSI 16          " CSI "0;37m(works everywhere)     " CSI "1;32mSUPPORTED\r\n\r\n");

    door_write(CSI "0;37m    24-bit  ");
    test_strip(true, utf8);
    door_write("\r\n    256     ");
    test_strip(false, utf8);
    door_write("\r\n    16      ");
    block_strip(utf8);
    door_write("\r\n\r\n");

    snprintf(line, sizeof(line), "[%d]", recommended);
    door_write(CSI "0;37m  Enter = " CSI "1;37m");
    door_write(line);
    door_write(CSI "0;37m     Q = back to the BBS " CSI "0m");
}

/* Returns the choice, or 0 to go back to the BBS. */
static int choose_display(const terminal_t *term, bool *utf8)
{
    int recommended = term->trace ? CHOICE_TRACE : cterm_has_color(term) ? CHOICE_24BIT : CHOICE_16;
    int remembered = 0;

    load_choice(&remembered, utf8);
    if (remembered != 0 && (remembered != CHOICE_TRACE || term->trace))
        recommended = remembered;

    for (;;)
    {
        int c;

        draw_menu(term, recommended, *utf8);
        c = door_read_char();
        if (c < 0 || c == 'q' || c == 'Q')
            return 0;
        /* Not shown on the menu any more (2026-09-19), but still there for a terminal that wants UTF-8 blocks */
        if (c == 'u' || c == 'U')
        {
            *utf8 = !*utf8;
            continue;
        }
        if (c == '\r' || c == '\n')
            c = '0' + recommended;
        if (c == '1' && !term->trace)
            continue;
        if (c >= '1' && c <= '4')
        {
            save_choice(c - '0', *utf8);
            return c - '0';
        }
    }
}

/* Where the source of what the player was sent lives (GPL-2) */
#define WOLF_SOURCE_URL "https://github.com/omniphil/Wolf3DDoor"

static void goodbye(void)
{
    cls();
    door_write(CSI "1;31m\r\n  Thanks for playing WOLFENSTEIN 3D.\r\n\r\n" CSI "0m");

    /* The game here is Wolf4SDL (id's GPL source release) with the Nuked OPL3 chip: players are entitled to its
     * source */
    door_write(CSI "0;37m  The game here is Wolf4SDL, free software under the GNU GPL v2,\r\n");
    door_write("  with id Software's shareware episode.\r\n");
    door_write("  Source: " CSI "1;37m" WOLF_SOURCE_URL "\r\n" CSI "0m");
    press_any_key();
}

/* TRACE: the whole game goes to TERMinator and runs on the player's own machine. */
static int play_trace(void)
{
    title();
    door_write(CSI "1;32m  ");
    write_terminator();
    door_write(CSI "1;32m found. Sending the game...\r\n" CSI "0m");

    /* Whose saved games these are. "player" means the BBS didn't tell us who is calling (no drop file), and everyone
     * would then share one set of saves, so it's worth the sysop seeing it. */
    door_write(CSI "0;37m  Saved games for: ");
    door_write(files_player());
    door_write("\r\n");

    /* GPL-2: players are entitled to the source of what they were just sent */
    door_write("  Wolf4SDL, GPL-2: " WOLF_SOURCE_URL "\r\n\r\n" CSI "0m");

    /* The data is 1.2 MB and only travels once: after that it's cached on the player's machine for good. */
    door_write(CSI "0;37m  Checking whether you already have the game data...\r\n" CSI "0m");
    if (!trace_wolf_send_data(data_progress)) {
        door_write(CSI "1;33m\r\n  The game data couldn't be sent. Please try again later.\r\n" CSI "0m");
        press_any_key();
        return 1;
    }

    door_write(CSI "0;37m\r\n  Starting WOLFENSTEIN 3D. Arrows or W/S move, A/D strafe, Ctrl fires, Space opens,\r\n"
               "  Shift runs. ESC for the menu, and Quit from there to come back.\r\n" CSI "0m");

    if (!trace_wolf_open()) {
        door_write(CSI "1;33m\r\n  WOLFENSTEIN 3D couldn't be started on your terminal.\r\n" CSI "0m");
        press_any_key();
        return 1;
    }

    /*
     * The game now covers the terminal, so clear what's underneath it. Otherwise, the moment the player quits, the
     * text from a moment ago ("Sending the game...") flashes up before this door can draw its closing screen.
     */
    cls();

    /* From here the player's terminal has the keyboard and the screen; we wait for them to quit. */
    trace_wolf_wait();
    trace_wolf_close();
    goodbye();
    return 0;
}

/* ANSI: the game runs here on the BBS, and the player sees it as text-mode pictures. */
static int play_ansi(int choice, bool utf8)
{
    ansi_mode_t mode = choice == CHOICE_24BIT ? MODE_24BIT : choice == CHOICE_256 ? MODE_256 : MODE_16;
    play_result_t result;

    title();
    door_write(CSI "0;37m  Saved games for: ");
    door_write(files_player());
    door_write("\r\n\r\n");
    door_write("  Arrows or W/S move, A/D strafe, F fires, Space opens doors, 1-4 pick\r\n");
    door_write("  a weapon, Esc brings up the menu. Quit from the menu, or press Ctrl-Q,\r\n");
    door_write("  to come back. ` shows frames per second.\r\n\r\n");
    door_write(CSI "1;37m  Press any key to start." CSI "0m");
    if (door_read_char() < 0)
        return 0;

    /* ANSI keeps its own settings, so its whole-screen view and silence don't follow the player into TRACE */
    files_use_ansi_config(true);
    result = ansi_play(mode, utf8);
    if (result == PLAY_HANGUP)
        return 0;
    if (result == PLAY_FAILED)
    {
        title();
        door_write(CSI "1;33m  WOLFENSTEIN 3D couldn't be started. Please tell the sysop.\r\n" CSI "0m");
        press_any_key();
        return 1;
    }
    goodbye();
    return 0;
}

int main(int argc, char *argv[])
{
    terminal_t term = { 0 };
    bool utf8 = false;
    int choice, status;

    door_init(argc > 1 ? argv[1] : NULL);

    /* Saved games belong to the player, not to the machine they called from */
    files_init(door_info.handle, door_info.user_record);

    title();

    if (!trace_wolf_load_files()) {
        door_write(CSI "1;33m  This door isn't installed properly: wolf3d.wasm or wolf3d.pak is missing.\r\n" CSI "0m");
        door_write("  Please tell the sysop.\r\n");
        press_any_key();
        door_cleanup();
        return 1;
    }

    term.trace = detect_with_animation();
    detect_cterm(&term);

    choice = choose_display(&term, &utf8);
    if (choice == 0)
        status = 0;
    else if (choice == CHOICE_TRACE)
        status = play_trace();
    else
        status = play_ansi(choice, utf8);

    door_cleanup();
    return status;
}
