/*
 * ansi_pace.c -- how many ANSI frames may be on their way to the caller at once, told by the caller's terminal.
 *
 * The window: enough bytes on their way to keep the link busy for one round trip plus a little queue (its speed times
 * QUEUE_ALLOWED_MS). The link's speed is timed on each big frame: one that had to wait behind the frame before it
 * went through in the time between their two answers; one that didn't, in its round trip less the ping. (What was
 * answered for a second would be how much was sent, not how much the link could take.) Below MIN_WINDOW frames a
 * frame may go anyway, so a faster link is found and a long ping doesn't cap the frame rate -- but not past
 * FLOOR_QUEUE_MS of queue: ANSI frames run from a few hundred bytes to tens of KB, and four big ones on a slow link
 * would be a second behind. (pix_play.c's window, without its picture-quality control.)
 */

#include "ansi_pace.h"

#include <string.h>

#define MAX_IN_FLIGHT    8
#define MIN_WINDOW       4
#define QUEUE_ALLOWED_MS 50
#define FLOOR_QUEUE_MS   200
#define SPEED_FRAME      2048       /* bytes: smaller frames go through too quickly to time */
#define STALL_MS         3000       /* no answer for this long: forget what's outstanding rather than stop */
#define HELD_MS          40         /* a part-answer is typing after all if nothing more comes in this long */

static struct
{
    long   sent[MAX_IN_FLIGHT];
    size_t bytes[MAX_IN_FLIGHT];
    size_t total;
    int    head, count;
} g_fly;

static int    g_orphans;            /* answers still owed for frames given up on */
static bool   g_answered;           /* the terminal has answered at least once */
static bool   g_off;                /* it never answers: no pacing, the caller's own output check only */
static long   g_rtt_floor[10];      /* the shortest round trip in each of the last 10 seconds */
static long   g_rtt_floor_sec;
static double g_rate;               /* bytes a millisecond; 0 until measured */
static long   g_last_answer;

static unsigned char g_held[ANSI_PACE_HELD];
static int  g_held_len;
static long g_held_at;

void ansi_pace_reset(void)
{
    memset(&g_fly, 0, sizeof(g_fly));
    memset(g_rtt_floor, 0, sizeof(g_rtt_floor));
    g_rtt_floor_sec = -1;
    g_orphans = 0;
    g_answered = g_off = false;
    g_rate = 0;
    g_last_answer = 0;
    g_held_len = 0;
}

long ansi_pace_rtt(void)
{
    long m = 0;
    for (int i = 0; i < 10; i++)
        if (g_rtt_floor[i] > 0 && (m == 0 || g_rtt_floor[i] < m))
            m = g_rtt_floor[i];
    return m;
}

static void answered(long now)
{
    long rtt, sec = now / 1000;

    g_answered = true;
    if (g_orphans > 0)
    {
        g_orphans--;
        return;
    }
    if (g_fly.count == 0)
        return;
    rtt = now - g_fly.sent[g_fly.head];
    if (rtt < 1)
        rtt = 1;
    if (sec != g_rtt_floor_sec || rtt < g_rtt_floor[sec % 10])
        g_rtt_floor[sec % 10] = rtt;
    g_rtt_floor_sec = sec;

    if (g_fly.bytes[g_fly.head] >= SPEED_FRAME)
    {
        bool waited = g_last_answer - g_fly.sent[g_fly.head] > ansi_pace_rtt();
        long through = waited ? now - g_last_answer : rtt - ansi_pace_rtt();
        /* Two answers read together say nothing about the time between them; and one wild sample mustn't count
         * for much: the guess grows at most by half at a time */
        if (through >= 3)
        {
            double sample = (double)g_fly.bytes[g_fly.head] / (double)through;
            if (g_rate > 0 && sample > g_rate * 1.5)
                sample = g_rate * 1.5;
            g_rate = g_rate == 0 ? sample : g_rate * 0.8 + sample * 0.2;
        }
    }
    g_last_answer = now;
    g_fly.total -= g_fly.bytes[g_fly.head];
    g_fly.head = (g_fly.head + 1) % MAX_IN_FLIGHT;
    g_fly.count--;
}

bool ansi_pace_open(long now)
{
    double total;

    if (g_off)
        return true;


    /* An answer that never came mustn't stop the picture for good; a terminal that has never answered doesn't */
    if (g_fly.count > 0 && now - g_fly.sent[g_fly.head] > STALL_MS)
    {
        if (!g_answered)
        {
            g_off = true;
            return true;
        }
        g_orphans += g_fly.count;
        g_fly.count = 0;
        g_fly.total = 0;
    }

    if (g_fly.count >= MAX_IN_FLIGHT)
        return false;
    total = (double)g_fly.total;
    if (total < g_rate * (double)(ansi_pace_rtt() + QUEUE_ALLOWED_MS))
        return true;
    if (g_rate == 0)
        return g_fly.count == 0;                        /* one at a time until the link's speed is known */
    return g_fly.count < MIN_WINDOW && total < g_rate * (double)(ansi_pace_rtt() + FLOOR_QUEUE_MS);
}

void ansi_pace_sent(long now, size_t bytes)
{
    int i;

    if (g_off || g_fly.count >= MAX_IN_FLIGHT)
        return;
    i = (g_fly.head + g_fly.count) % MAX_IN_FLIGHT;
    g_fly.sent[i] = now;
    g_fly.bytes[i] = bytes;
    g_fly.total += bytes;
    g_fly.count++;
}

/* Whether c carries on an answer (ESC [ row ; col R) from what's held; 2 when it finishes one */
static int carries_on(unsigned char c)
{
    const unsigned char *semi;

    if (g_held_len == 0)
        return c == 0x1B;
    if (g_held_len == 1)
        return c == '[';
    if (g_held_len >= ANSI_PACE_HELD - 1)
        return 0;
    semi = memchr(g_held, ';', (size_t)g_held_len);
    bool after_digit = g_held[g_held_len - 1] >= '0' && g_held[g_held_len - 1] <= '9';
    if (c >= '0' && c <= '9')
        return 1;
    if (c == ';')
        return semi == NULL && after_digit;
    if (c == 'R')
        return semi != NULL && after_digit ? 2 : 0;
    return 0;
}

int ansi_pace_take(const unsigned char *data, int len, unsigned char *out, long now)
{
    int n = 0;

    for (int i = 0; i < len; i++)
    {
        unsigned char c = data[i];
        int r = carries_on(c);

        if (r == 0 && g_held_len > 0)
        {
            /* Not an answer after all: what was held is typing, and c starts afresh */
            memcpy(out + n, g_held, (size_t)g_held_len);
            n += g_held_len;
            g_held_len = 0;
            r = carries_on(c);
        }
        if (r == 2)
        {
            g_held_len = 0;
            answered(now);
        }
        else if (r == 1)
        {
            if (g_held_len == 0)
                g_held_at = now;
            g_held[g_held_len++] = c;
        }
        else
        {
            out[n++] = c;
        }
    }
    return n;
}

int ansi_pace_stale(unsigned char *out, long now)
{
    int n = 0;

    if (g_held_len > 0 && now - g_held_at >= HELD_MS)
    {
        memcpy(out, g_held, (size_t)g_held_len);
        n = g_held_len;
        g_held_len = 0;
    }
    return n;
}
