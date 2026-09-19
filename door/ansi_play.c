/*
 * ansi_play.c -- playing Wolfenstein 3D in ANSI, for callers without TRACE. (The Doom door's ansi_play.c, with
 * Wolfenstein's status bar and menus.)
 *
 * The screen, 80x24:
 *   rows 1-22   the picture (ansi_screen.c), the whole view: the game's own status bar is switched off
 *   row 23      the status bar as text: floor, score, lives, health, ammo, weapons, keys
 *   row 24      the controls, and frames a second when ` is pressed. Its last cell is never written.
 *
 * The loop reads keys, lets go of keys whose time is up, and sends a frame when there is a new one and the link has
 * room for it. If the link is slow, frames are skipped rather than queued, so the picture never falls behind the
 * game: a slow connection gets fewer frames, not old ones.
 */

#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <time.h>
#include <unistd.h>

#include "ansi_host.h"
#include "ansi_hud.h"
#include "ansi_input.h"
#include "ansi_play.h"
#include "door.h"
#include "trace_wolf.h"

#define PICTURE_LAST_ROW 22
#define HUD_ROW          23
#define MESSAGE_ROW      24

#define MAX_FPS          35           /* the most there is to show: Wolfenstein runs at 70 tics, 35 is plenty */
#define OUTQ_LIMIT       4096         /* send the next frame only once the link has nearly caught up */
#define KEY_QUIT         17           /* Ctrl-Q: back to the BBS at once */
#define KEY_STATS        '`'

static long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

/* Bytes still waiting to go out to the caller, or 0 when that can't be told */
static int output_queued(void)
{
    int queued = 0;
#ifdef TIOCOUTQ
    if (ioctl(STDOUT_FILENO, TIOCOUTQ, &queued) != 0)
        queued = 0;
#endif
    return queued;
}

/* Everything, with nothing lost to a signal or a short write */
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

/* ---- the text status bar ---- */

static int put(int row, int col, const char *text, int fg)
{
    ansi_screen_text(row, col, text, fg, C_BLACK);
    return col + (int)strlen(text);
}

static int put_number(int row, int col, const char *format, int value, int fg)
{
    char text[16];
    snprintf(text, sizeof(text), format, value);
    return put(row, col, text, fg);
}

static void draw_status(const ansi_hud_t *hud)
{
    static const char *const WEAPON[4] = { "KNIFE", "PISTOL", "MACHINE GUN", "CHAINGUN" };
    int col = 1;

    ansi_screen_clear_to_eol(HUD_ROW, 1, C_BLACK);
    if (!hud->in_level)
    {
        col = put(HUD_ROW, col, " WOLFENSTEIN 3D", C_LRED);
        put(HUD_ROW, col, "   Arrows: choose   Enter: select   Esc: back   Ctrl-Q: back to the BBS", C_GREY);
        return;
    }

    col = put(HUD_ROW, col, " FLOOR ", C_GREY);
    col = put_number(HUD_ROW, col, "%d", hud->floor, C_WHITE);
    col = put(HUD_ROW, col, "  SCORE ", C_GREY);
    col = put_number(HUD_ROW, col, "%d", hud->score, C_WHITE);
    col = put(HUD_ROW, col, "  LIVES ", C_GREY);
    col = put_number(HUD_ROW, col, "%d", hud->lives, C_WHITE);
    col = put(HUD_ROW, col, "  HEALTH ", C_GREY);
    col = put_number(HUD_ROW, col, "%d%%", hud->health, hud->health <= 25 ? C_LRED : C_WHITE);
    col = put(HUD_ROW, col, "  AMMO ", C_GREY);
    col = put_number(HUD_ROW, col, "%d", hud->ammo, hud->ammo == 0 ? C_LRED : C_WHITE);
    col = put(HUD_ROW, col, "  ", C_GREY);
    col = put(HUD_ROW, col, hud->weapon >= 0 && hud->weapon < 4 ? WEAPON[hud->weapon] : "", C_YELLOW);
    col = put(HUD_ROW, col, "  KEYS ", C_GREY);
    col = put(HUD_ROW, col, hud->gold_key ? "\xFE" : "\xFA", hud->gold_key ? C_YELLOW : C_DARKGREY);
    col = put(HUD_ROW, col, " ", C_GREY);
    put(HUD_ROW, col, hud->silver_key ? "\xFE" : "\xFA", hud->silver_key ? C_LCYAN : C_DARKGREY);
}

/* The game's menus and questions, redrawn as text in a box over the picture: its own are pictures of text, which
 * can't be read at this size. The item the game's gun points at is highlighted; ones that can't be chosen are dim. */
static void draw_menu(const ansi_menu_t *m)
{
    char line[SCREEN_COLS + 1];
    int lines = m->message_lines > 0 ? m->message_lines : m->count + 2;   /* a title and a blank line before items */
    int width = 30, top, left;

    if (!m->show)
        return;
    for (int i = 0; i < m->message_lines; i++)
        if ((int)strlen(m->message[i]) + 6 > width)
            width = (int)strlen(m->message[i]) + 6;
    for (int i = 0; i < m->count; i++)
        if ((int)strlen(m->items[i]) + 10 > width)
            width = (int)strlen(m->items[i]) + 10;
    if (width > SCREEN_COLS - 4)
        width = SCREEN_COLS - 4;
    top = (PICTURE_LAST_ROW - (lines + 2)) / 2 + 1;
    left = (SCREEN_COLS - width) / 2 + 1;

    /* The frame: ╔═╗ ║ ║ ╚═╝ */
    memset(line, '\xCD', (size_t)width);
    line[0] = '\xC9';
    line[width - 1] = '\xBB';
    line[width] = '\0';
    ansi_screen_text(top, left, line, C_RED, C_BLACK);
    line[0] = '\xC8';
    line[width - 1] = '\xBC';
    ansi_screen_text(top + lines + 1, left, line, C_RED, C_BLACK);
    memset(line, ' ', (size_t)width);
    line[0] = line[width - 1] = '\xBA';
    for (int r = 1; r <= lines; r++)
        ansi_screen_text(top + r, left, line, C_RED, C_BLACK);

    if (m->message_lines > 0)
    {
        for (int i = 0; i < m->message_lines; i++)
            ansi_screen_text(top + 1 + i, left + (width - (int)strlen(m->message[i])) / 2, m->message[i],
                             C_WHITE, C_BLACK);
        return;
    }

    ansi_screen_text(top + 1, left + (width - (int)strlen(m->title)) / 2, m->title, C_YELLOW, C_BLACK);
    for (int i = 0; i < m->count; i++)
    {
        if (!m->items[i][0])
            continue;
        if (i == m->selected)
        {
            snprintf(line, sizeof(line), " > %-*s", width - 8, m->items[i]);
            ansi_screen_text(top + 3 + i, left + 2, line, C_WHITE, C_RED);
        }
        else
        {
            ansi_screen_text(top + 3 + i, left + 5, m->items[i], m->dim[i] ? C_DARKGREY : C_GREY, C_BLACK);
        }
    }
}

static void draw_message_row(const char *stats)
{
    int col;

    ansi_screen_clear_to_eol(MESSAGE_ROW, 1, C_BLACK);
    col = put(MESSAGE_ROW, 2, "Arrows/WS move  AD strafe  F fire  Space open  1-4 weapon  Esc menu  ", C_DARKGREY);
    put(MESSAGE_ROW, col, ansi_input_running() ? "R: RUN" : "R: walk", ansi_input_running() ? C_LCYAN : C_DARKGREY);
    if (stats[0])
        put(MESSAGE_ROW, SCREEN_COLS - (int)strlen(stats), stats, C_LCYAN);   /* ends in column 79 */
}

/* ---- the loop ---- */

play_result_t ansi_play(ansi_mode_t mode, bool utf8)
{
    uint32_t *frame = NULL;
    int frame_w = 0, frame_h = 0;
    unsigned frame_seq = 0;
    long last_sent = 0, stats_since;
    long frames = 0, bytes = 0;
    char stats[48] = "";
    bool show_stats = false;
    play_result_t result = PLAY_QUIT;
    ansi_hud_t hud;

    /* Colours reset, screen cleared, cursor hidden, and no wrapping at the right edge */
    door_write("\033[0m\033[2J\033[H\033[?25l\033[?7l");
    ansi_screen_init(mode, utf8);

    if (!ansi_host_start(trace_wolf_pak_data(), trace_wolf_pak_size(), trace_wolf_pak_hash()))
    {
        door_write("\033[0m\033[?7h\033[?25h");
        return PLAY_FAILED;
    }

    stats_since = now_ms();
    for (;;)
    {
        long now;
        int key;

        /* Keys: wait a few milliseconds for some, which is also what paces this loop */
        {
            struct timeval tv = { 0, 4000 };
            fd_set fds;
            FD_ZERO(&fds);
            FD_SET(STDIN_FILENO, &fds);
            if (select(STDIN_FILENO + 1, &fds, NULL, NULL, &tv) > 0)
            {
                unsigned char buf[256];
                ssize_t n = read(STDIN_FILENO, buf, sizeof(buf));
                if (n == 0 || (n < 0 && errno != EINTR && errno != EAGAIN))
                {
                    result = PLAY_HANGUP;
                    break;
                }
                if (n > 0)
                    ansi_input_feed(buf, (int)n, now_ms());
            }
        }
        now = now_ms();

        ansi_hud_read(&hud);
        ansi_input_auto_fire(hud.weapon >= 2);

        while ((key = ansi_input_next(now)) >= 0)
        {
            if (key == KEY_QUIT)
            {
                result = PLAY_LEFT;
                goto done;
            }
            if (key == KEY_STATS && !hud.typing)
            {
                show_stats = !show_stats;
                stats[0] = '\0';
                continue;
            }
            ansi_input_key(key, hud.menu, hud.typing, now);
        }
        ansi_input_release_due(now);

        if (ansi_host_finished())
            break;
        if (door_time_remaining() <= 0)
        {
            result = PLAY_LEFT;
            break;
        }

        /* A frame, when there's a new one, it's time, and the link has caught up */
        if (now - last_sent >= 1000 / MAX_FPS && output_queued() < OUTQ_LIMIT)
        {
            const char *out;
            size_t len;

            if (ansi_host_frame(&frame, &frame_w, &frame_h, &frame_seq))
                ansi_screen_picture(frame, frame_w, frame_h, 1, PICTURE_LAST_ROW);
            draw_menu(&hud.menu_view);
            draw_status(&hud);
            draw_message_row(stats);

            len = ansi_screen_update(&out);
            if (len > 0)
            {
                if (!write_all(out, len))
                {
                    result = PLAY_HANGUP;
                    break;
                }
                frames++;
                bytes += (long)len;
            }
            last_sent = now;
        }

        /* Frames and bytes a second, shown with ` for judging a connection */
        if (now - stats_since >= 1000)
        {
            if (show_stats)
                snprintf(stats, sizeof(stats), "%ld fps %ld KB/s", frames * 1000 / (now - stats_since),
                         bytes * 1000 / (now - stats_since) / 1024);
            frames = bytes = 0;
            stats_since = now;
        }
    }

done:
    ansi_input_release_all();
    free(frame);
    door_write("\033[0m\033[?7h\033[?25h\033[2J\033[H");
    return result;
}
