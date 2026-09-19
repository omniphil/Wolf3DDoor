/*
 * ansi_screen.c -- Doom's picture as ANSI, for callers without TRACE.
 *
 * The door keeps two grids of 80x24 cells: what the caller's screen shows now, and what it should show next. The
 * picture and the text status line are drawn into the second; ansi_screen_update() then sends only the cells that
 * differ, so a still picture costs nothing and a moving one only what moved.
 *
 * Three ways of drawing the picture, picked by the player (main.c):
 *
 *   16 colours   Every cell is fitted separately: each of  space █ ▀ ▄ ▌ ▐ ░ ▒ ▓  with every pair of colours, compared
 *                with the picture on a 2x4 grid of samples in CIELAB, so what looks closest wins. Doom is mostly dark,
 *                and the 16 colours have few dark shades, so the picture is brightened and its colour pushed up a
 *                little first, the way ANSI artists do. Tried out on stills before this was written: see
 *                ansi_test/ansify.py, which this follows.
 *   256 colours  Half-blocks: ▀ with the top pixel as the foreground and the bottom one as the background, so an
 *                80x22 area shows 80x44 pixels, each the nearest of xterm's 256 colours.
 *   24-bit       The same half-blocks in exact colour.
 */

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "ansi_screen.h"

/* ---- cells ---- */

/* A colour, tagged with the palette it belongs to, so text in the 16 colours can sit beside a 24-bit picture */
#define TAG_16   0x01000000u
#define TAG_256  0x02000000u
#define TAG_RGB  0x03000000u
#define TAG(c)   ((c) & 0xFF000000u)
#define NO_COLOR 0u

typedef struct
{
    uint8_t  ch;            /* CP437; 0 = unknown (forces a redraw) */
    uint32_t fg, bg;
} cell_t;

#define CH_SPACE  32
#define CH_FULL   219
#define CH_UPPER  223
#define CH_LOWER  220
#define CH_LEFT   221
#define CH_RIGHT  222
#define CH_LIGHT  176
#define CH_MEDIUM 177
#define CH_DARK   178

static cell_t g_shown[SCREEN_ROWS][SCREEN_COLS];      /* what the caller has */
static cell_t g_next[SCREEN_ROWS][SCREEN_COLS];       /* what they should have */

static ansi_mode_t g_mode;
static bool        g_utf8;

/* ---- colour ---- */

/* The 16 colours as a VGA card shows them, in PC order */
static const uint8_t VGA[16][3] = {
    {0,0,0}, {0,0,170}, {0,170,0}, {0,170,170}, {170,0,0}, {170,0,170}, {170,85,0}, {170,170,170},
    {85,85,85}, {85,85,255}, {85,255,85}, {85,255,255}, {255,85,85}, {255,85,255}, {255,255,85}, {255,255,255},
};

/* PC colour order to the digit ANSI uses (ANSI's order is black red green yellow blue magenta cyan white) */
static const int ANSI_DIGIT[8] = { 0, 4, 2, 6, 1, 5, 3, 7 };

typedef struct { float l, a, b; } lab_t;

static float srgb_to_linear(float c)
{
    c /= 255.0f;
    return c <= 0.04045f ? c / 12.92f : powf((c + 0.055f) / 1.055f, 2.4f);
}

static float lab_f(float t)
{
    return t > 0.008856f ? cbrtf(t) : 7.787f * t + 16.0f / 116.0f;
}

static lab_t linear_to_lab(float r, float g, float b)
{
    float x = (0.4124f * r + 0.3576f * g + 0.1805f * b) / 0.9505f;
    float y =  0.2126f * r + 0.7152f * g + 0.0722f * b;
    float z = (0.0193f * r + 0.1192f * g + 0.9505f * b) / 1.089f;
    float fx = lab_f(x), fy = lab_f(y), fz = lab_f(z);
    lab_t out = { 116.0f * fy - 16.0f, 500.0f * (fx - fy), 200.0f * (fy - fz) };
    return out;
}

static float dist2(lab_t p, lab_t q)
{
    float dl = p.l - q.l, da = p.a - q.a, db = p.b - q.b;
    return dl * dl + da * da + db * db;
}

/* ---- the 16-colour fit ---- */

#define GAMMA       0.65f       /* brightens: Doom's darks otherwise all become black */
#define SATURATION  1.25f       /* pushes colours towards the palette's, so browns and reds survive */
#define SHADE_PENALTY 0.04f     /* discourages ▒ mixes of very different colours, which read as noise */
#define KEEP_MARGIN 1.12f       /* keep a cell as it is unless the new fit is clearly better: less flicker, less data */

static const float SHADE_COVER[3] = { 0.25f, 0.5f, 0.75f };   /* ░ ▒ ▓ */

static lab_t g_pal_lab[16];
static lab_t g_mix_lab[3][16][8];      /* shade k, foreground, background */
static float g_mix_penalty[16][8];
static float g_gamma_lut[256];         /* 0..255 in, brightened 0..255 out */

static void init_16(void)
{
    float lin[16][3];

    for (int i = 0; i < 16; i++)
    {
        for (int c = 0; c < 3; c++)
            lin[i][c] = srgb_to_linear(VGA[i][c]);
        g_pal_lab[i] = linear_to_lab(lin[i][0], lin[i][1], lin[i][2]);
    }
    for (int k = 0; k < 3; k++)
        for (int f = 0; f < 16; f++)
            for (int b = 0; b < 8; b++)
            {
                float cov = SHADE_COVER[k];
                g_mix_lab[k][f][b] = linear_to_lab(lin[f][0] * cov + lin[b][0] * (1 - cov),
                                                   lin[f][1] * cov + lin[b][1] * (1 - cov),
                                                   lin[f][2] * cov + lin[b][2] * (1 - cov));
            }
    for (int f = 0; f < 16; f++)
        for (int b = 0; b < 8; b++)
            g_mix_penalty[f][b] = SHADE_PENALTY * 8.0f * dist2(g_pal_lab[f], g_pal_lab[b]);
    for (int v = 0; v < 256; v++)
        g_gamma_lut[v] = 255.0f * powf(v / 255.0f, GAMMA);
}

/* The average colour of a block of the frame, 0..255 per channel */
static void box_average(const uint32_t *bgra, int width, int x0, int x1, int y0, int y1, float rgb[3])
{
    unsigned long r = 0, g = 0, b = 0, n = 0;

    for (int y = y0; y < y1; y++)
    {
        const uint32_t *row = bgra + (size_t)y * (size_t)width;
        for (int x = x0; x < x1; x++)
        {
            uint32_t p = row[x];
            r += (p >> 16) & 0xFF;
            g += (p >> 8) & 0xFF;
            b += p & 0xFF;
            n++;
        }
    }
    if (n == 0)
        n = 1;
    rgb[0] = (float)r / n;
    rgb[1] = (float)g / n;
    rgb[2] = (float)b / n;
}

/* One sample for the 16-colour fit: brightened, colour pushed up, in CIELAB */
static lab_t prepared_sample(const float rgb[3])
{
    float grey = 0.299f * rgb[0] + 0.587f * rgb[1] + 0.114f * rgb[2];
    float lin[3];

    for (int c = 0; c < 3; c++)
    {
        float v = grey + (rgb[c] - grey) * SATURATION;
        int i = v < 0 ? 0 : v > 255 ? 255 : (int)v;
        lin[c] = srgb_to_linear(g_gamma_lut[i]);
    }
    return linear_to_lab(lin[0], lin[1], lin[2]);
}

/* A region of a cell's 2x4 samples: its mean and how far the samples spread from it */
typedef struct { lab_t mean; float spread; int n; } region_t;

static region_t region(const lab_t *s, const int *idx, int n)
{
    region_t r = { {0, 0, 0}, 0, n };
    for (int i = 0; i < n; i++)
    {
        r.mean.l += s[idx[i]].l;
        r.mean.a += s[idx[i]].a;
        r.mean.b += s[idx[i]].b;
    }
    r.mean.l /= n;
    r.mean.a /= n;
    r.mean.b /= n;
    for (int i = 0; i < n; i++)
        r.spread += dist2(s[idx[i]], r.mean);
    return r;
}

/* Samples are numbered row by row: 0 1 / 2 3 / 4 5 / 6 7 */
static const int ALL[8] = { 0, 1, 2, 3, 4, 5, 6, 7 };
static const int TOP[4] = { 0, 1, 2, 3 }, BOTTOM[4] = { 4, 5, 6, 7 };
static const int LEFT[4] = { 0, 2, 4, 6 }, RIGHT[4] = { 1, 3, 5, 7 };

typedef struct { region_t all, top, bottom, left, right; } cell_stats_t;

/* How far a cell drawn as (ch, fg, bg) is from the samples. Squared distances split exactly into mean + spread, so
 * this needs only each region's mean, never the samples themselves. */
static float region_cost(const region_t *r, int color)
{
    return r->n * dist2(r->mean, g_pal_lab[color]) + r->spread;
}

static float cell_cost(const cell_stats_t *st, int ch, int fg, int bg)
{
    switch (ch)
    {
    case CH_SPACE: return region_cost(&st->all, bg);
    case CH_FULL:  return region_cost(&st->all, fg);
    case CH_UPPER: return region_cost(&st->top, fg) + region_cost(&st->bottom, bg);
    case CH_LOWER: return region_cost(&st->bottom, fg) + region_cost(&st->top, bg);
    case CH_LEFT:  return region_cost(&st->left, fg) + region_cost(&st->right, bg);
    case CH_RIGHT: return region_cost(&st->right, fg) + region_cost(&st->left, bg);
    case CH_LIGHT: case CH_MEDIUM: case CH_DARK:
    {
        int k = ch - CH_LIGHT;
        return 8 * dist2(st->all.mean, g_mix_lab[k][fg][bg]) + st->all.spread + g_mix_penalty[fg][bg];
    }
    default:       return 1e30f;
    }
}

/* The best colour for one region, from the first `count` colours */
static int best_color(const region_t *r, int count, float *cost)
{
    int best = 0;
    float best_cost = 1e30f;
    for (int i = 0; i < count; i++)
    {
        float c = dist2(r->mean, g_pal_lab[i]);
        if (c < best_cost)
        {
            best_cost = c;
            best = i;
        }
    }
    *cost = r->n * best_cost + r->spread;
    return best;
}

static cell_t fit_16(const lab_t samples[8], const cell_t *shown)
{
    cell_stats_t st;
    cell_t best = { CH_SPACE, TAG_16 | 7, TAG_16 | 0 };
    float best_cost;

    st.all = region(samples, ALL, 8);
    st.top = region(samples, TOP, 4);
    st.bottom = region(samples, BOTTOM, 4);
    st.left = region(samples, LEFT, 4);
    st.right = region(samples, RIGHT, 4);

    /* One colour for the whole cell: a space on a background, or a full block for the bright colours */
    {
        float cost;
        int c = best_color(&st.all, 16, &cost);
        best_cost = cost;
        if (c < 8)
            best = (cell_t){ CH_SPACE, NO_COLOR, TAG_16 | (uint32_t)c };
        else
            best = (cell_t){ CH_FULL, TAG_16 | (uint32_t)c, NO_COLOR };
    }

    /* Half blocks: the two halves are independent, so each takes its own best colour */
    {
        static const struct { int ch; size_t fg_region, bg_region; } HALVES[4] = {
            { CH_UPPER, offsetof(cell_stats_t, top),    offsetof(cell_stats_t, bottom) },
            { CH_LOWER, offsetof(cell_stats_t, bottom), offsetof(cell_stats_t, top) },
            { CH_LEFT,  offsetof(cell_stats_t, left),   offsetof(cell_stats_t, right) },
            { CH_RIGHT, offsetof(cell_stats_t, right),  offsetof(cell_stats_t, left) },
        };
        for (int h = 0; h < 4; h++)
        {
            const region_t *fr = (const region_t *)((const char *)&st + HALVES[h].fg_region);
            const region_t *br = (const region_t *)((const char *)&st + HALVES[h].bg_region);
            float fc, bc;
            int f = best_color(fr, 16, &fc);
            int b = best_color(br, 8, &bc);
            if (fc + bc < best_cost && f != b)
            {
                best_cost = fc + bc;
                best = (cell_t){ (uint8_t)HALVES[h].ch, TAG_16 | (uint32_t)f, TAG_16 | (uint32_t)b };
            }
        }
    }

    /* Shades: every pair, since the mix is what counts */
    for (int k = 0; k < 3; k++)
        for (int f = 0; f < 16; f++)
            for (int b = 0; b < 8; b++)
            {
                float c;
                if (f == b)
                    continue;
                c = 8 * dist2(st.all.mean, g_mix_lab[k][f][b]) + st.all.spread + g_mix_penalty[f][b];
                if (c < best_cost)
                {
                    best_cost = c;
                    best = (cell_t){ (uint8_t)(CH_LIGHT + k), TAG_16 | (uint32_t)f, TAG_16 | (uint32_t)b };
                }
            }

    /* What the caller already has is kept when it is nearly as good: a still scene then stops flickering between two
     * equally good fits, and nothing is sent for it */
    if (shown->ch != 0 && (shown->ch == CH_SPACE || TAG(shown->fg) == TAG_16) &&
        (shown->ch == CH_FULL || TAG(shown->bg) == TAG_16))
    {
        int fg = (int)(shown->fg & 0xFF), bg = (int)(shown->bg & 0xFF);
        if (bg < 8 && fg < 16 && cell_cost(&st, shown->ch, fg, bg) <= best_cost * KEEP_MARGIN + 1.0f)
            return *shown;
    }
    return best;
}

/* ---- the 256-colour palette ---- */

static const int CUBE[6] = { 0, 95, 135, 175, 215, 255 };

static int cube_level(float v)
{
    int best = 0;
    for (int i = 1; i < 6; i++)
        if (fabsf(v - CUBE[i]) < fabsf(v - CUBE[best]))
            best = i;
    return best;
}

static uint32_t to_256(const float rgb[3])
{
    int r = cube_level(rgb[0]), g = cube_level(rgb[1]), b = cube_level(rgb[2]);
    float dc = 0, dg;
    float grey = (rgb[0] + rgb[1] + rgb[2]) / 3.0f;
    int gi = (int)lroundf((grey - 8.0f) / 10.0f);
    int gv;

    if (gi < 0) gi = 0;
    if (gi > 23) gi = 23;
    gv = 8 + gi * 10;
    dc = (rgb[0] - CUBE[r]) * (rgb[0] - CUBE[r]) + (rgb[1] - CUBE[g]) * (rgb[1] - CUBE[g])
       + (rgb[2] - CUBE[b]) * (rgb[2] - CUBE[b]);
    dg = (rgb[0] - gv) * (rgb[0] - gv) + (rgb[1] - gv) * (rgb[1] - gv) + (rgb[2] - gv) * (rgb[2] - gv);
    return TAG_256 | (uint32_t)(dg < dc ? 232 + gi : 16 + 36 * r + 6 * g + b);
}

static uint32_t to_rgb(const float rgb[3])
{
    return TAG_RGB | ((uint32_t)lroundf(rgb[0]) << 16) | ((uint32_t)lroundf(rgb[1]) << 8) | (uint32_t)lroundf(rgb[2]);
}

/* 24-bit colours this close count as the same, so a picture that barely moved isn't sent again */
#define RGB_TOLERANCE 10

static bool rgb_close(uint32_t p, uint32_t q)
{
    for (int shift = 0; shift <= 16; shift += 8)
    {
        int d = (int)((p >> shift) & 0xFF) - (int)((q >> shift) & 0xFF);
        if (d > RGB_TOLERANCE || d < -RGB_TOLERANCE)
            return false;
    }
    return true;
}

static bool color_same(uint32_t p, uint32_t q)
{
    if (p == q)
        return true;
    return TAG(p) == TAG_RGB && TAG(q) == TAG_RGB && rgb_close(p, q);
}

/* Does the caller's cell already look like this one? A space ignores its foreground and a full block its background. */
static bool cell_same(const cell_t *a, const cell_t *b)
{
    if (a->ch != b->ch)
        return false;
    if (a->ch != CH_SPACE && !color_same(a->fg, b->fg))
        return false;
    if (a->ch != CH_FULL && !color_same(a->bg, b->bg))
        return false;
    return true;
}

/* ---- drawing ---- */

void ansi_screen_init(ansi_mode_t mode, bool utf8)
{
    g_mode = mode;
    g_utf8 = utf8;
    init_16();
    ansi_screen_invalidate();
    for (int r = 0; r < SCREEN_ROWS; r++)
        for (int c = 0; c < SCREEN_COLS; c++)
            g_next[r][c] = (cell_t){ CH_SPACE, TAG_16 | 7, TAG_16 | 0 };
}

void ansi_screen_invalidate(void)
{
    memset(g_shown, 0, sizeof(g_shown));
}

void ansi_screen_picture(const uint32_t *bgra, int width, int height, int first_row, int last_row)
{
    int rows = last_row - first_row + 1;

    if (bgra == NULL || width <= 0 || height <= 0 || rows <= 0)
        return;

    if (g_mode == MODE_16)
    {
        /* 2 samples across and 4 down per cell */
        int sw = SCREEN_COLS * 2, sh = rows * 4;
        for (int r = 0; r < rows; r++)
            for (int c = 0; c < SCREEN_COLS; c++)
            {
                lab_t s[8];
                for (int i = 0; i < 8; i++)
                {
                    int sx = c * 2 + (i & 1), sy = r * 4 + (i >> 1);
                    int x0 = sx * width / sw, x1 = (sx + 1) * width / sw;
                    int y0 = sy * height / sh, y1 = (sy + 1) * height / sh;
                    float rgb[3];
                    if (x1 <= x0) x1 = x0 + 1;
                    if (y1 <= y0) y1 = y0 + 1;
                    box_average(bgra, width, x0, x1, y0, y1, rgb);
                    s[i] = prepared_sample(rgb);
                }
                g_next[first_row - 1 + r][c] = fit_16(s, &g_shown[first_row - 1 + r][c]);
            }
        return;
    }

    /* Half-blocks: a pixel for the top half of each cell and one for the bottom */
    {
        int ph = rows * 2;
        for (int r = 0; r < rows; r++)
            for (int c = 0; c < SCREEN_COLS; c++)
            {
                float top[3], bottom[3];
                int x0 = c * width / SCREEN_COLS, x1 = (c + 1) * width / SCREEN_COLS;
                int ya = (r * 2) * height / ph, yb = (r * 2 + 1) * height / ph, yc = (r * 2 + 2) * height / ph;
                uint32_t t, b;
                if (x1 <= x0) x1 = x0 + 1;
                if (yb <= ya) yb = ya + 1;
                if (yc <= yb) yc = yb + 1;
                box_average(bgra, width, x0, x1, ya, yb, top);
                box_average(bgra, width, x0, x1, yb, yc, bottom);
                if (g_mode == MODE_256)
                {
                    t = to_256(top);
                    b = to_256(bottom);
                }
                else
                {
                    t = to_rgb(top);
                    b = to_rgb(bottom);
                }
                if (color_same(t, b))
                    g_next[first_row - 1 + r][c] = (cell_t){ CH_SPACE, NO_COLOR, b };
                else
                    g_next[first_row - 1 + r][c] = (cell_t){ CH_UPPER, t, b };
            }
    }
}

void ansi_screen_text(int row, int col, const char *text, int fg, int bg)
{
    if (row < 1 || row > SCREEN_ROWS)
        return;
    for (const unsigned char *p = (const unsigned char *)text; *p && col <= SCREEN_COLS; p++, col++)
    {
        if (col < 1)
            continue;
        g_next[row - 1][col - 1] = (cell_t){ *p, TAG_16 | (uint32_t)fg, TAG_16 | (uint32_t)bg };
    }
}

void ansi_screen_clear_to_eol(int row, int col, int bg)
{
    if (row < 1 || row > SCREEN_ROWS)
        return;
    for (; col <= SCREEN_COLS; col++)
        if (col >= 1)
            g_next[row - 1][col - 1] = (cell_t){ CH_SPACE, NO_COLOR, TAG_16 | (uint32_t)bg };
}

/* ---- sending it ---- */

static char  *g_out;
static size_t g_out_len, g_out_cap;

static void emit(const char *s, size_t n)
{
    if (g_out_len + n > g_out_cap)
    {
        size_t cap = g_out_cap ? g_out_cap * 2 : 65536;
        char *grown;
        while (cap < g_out_len + n)
            cap *= 2;
        grown = realloc(g_out, cap);
        if (grown == NULL)
            return;
        g_out = grown;
        g_out_cap = cap;
    }
    memcpy(g_out + g_out_len, s, n);
    g_out_len += n;
}

static void emits(const char *s)
{
    emit(s, strlen(s));
}

/* CP437 to UTF-8, for terminals that expect it. Only what this door draws needs mapping. */
static const char *utf8_of(uint8_t ch)
{
    switch (ch)
    {
    case CH_LIGHT:  return "\xE2\x96\x91";   /* ░ */
    case CH_MEDIUM: return "\xE2\x96\x92";   /* ▒ */
    case CH_DARK:   return "\xE2\x96\x93";   /* ▓ */
    case CH_FULL:   return "\xE2\x96\x88";   /* █ */
    case CH_LOWER:  return "\xE2\x96\x84";   /* ▄ */
    case CH_LEFT:   return "\xE2\x96\x8C";   /* ▌ */
    case CH_RIGHT:  return "\xE2\x96\x90";   /* ▐ */
    case CH_UPPER:  return "\xE2\x96\x80";   /* ▀ */
    case 254:       return "\xE2\x96\xA0";   /* ■ */
    case 250:       return "\xC2\xB7";       /* · */
    case 196:       return "\xE2\x94\x80";   /* ─ */
    case 201:       return "\xE2\x95\x94";   /* ╔ */
    case 205:       return "\xE2\x95\x90";   /* ═ */
    case 187:       return "\xE2\x95\x97";   /* ╗ */
    case 186:       return "\xE2\x95\x91";   /* ║ */
    case 200:       return "\xE2\x95\x9A";   /* ╚ */
    case 188:       return "\xE2\x95\x9D";   /* ╝ */
    default:        return NULL;
    }
}

/* The terminal's current colours and cursor, so only changes are sent */
static uint32_t g_cur_fg, g_cur_bg;
static int      g_cur_bold;          /* -1 unknown */
static int      g_cur_row, g_cur_col;   /* 0 = unknown */

static void add_param(char *params, size_t size, const char *p)
{
    size_t len = strlen(params);
    snprintf(params + len, size - len, "%s%s", len ? ";" : "", p);
}

static void color_param(char *params, size_t size, uint32_t color, bool fg)
{
    char p[32];
    uint32_t v = color & 0xFFFFFFu;
    switch (TAG(color))
    {
    case TAG_16:  snprintf(p, sizeof(p), "%d", (fg ? 30 : 40) + ANSI_DIGIT[v & 7]); break;
    case TAG_256: snprintf(p, sizeof(p), "%d;5;%u", fg ? 38 : 48, v); break;
    case TAG_RGB: snprintf(p, sizeof(p), "%d;2;%u;%u;%u", fg ? 38 : 48, (v >> 16) & 0xFF, (v >> 8) & 0xFF, v & 0xFF); break;
    default: return;
    }
    add_param(params, size, p);
}

static void set_colors(const cell_t *cell)
{
    bool need_fg = cell->ch != CH_SPACE, need_bg = cell->ch != CH_FULL;
    bool want_bold = need_fg && TAG(cell->fg) == TAG_16 && (cell->fg & 0xFF) >= 8;
    char params[96] = "";

    /* Bright text is "bold" in the 16 colours. Turning it off is a full reset, which every terminal understands,
     * after which both colours are sent again. A 256/24-bit foreground also wants it off, so it isn't brightened. */
    if (need_fg && g_cur_bold != (int)want_bold)
    {
        if (want_bold && g_cur_bold == 0)
        {
            add_param(params, sizeof(params), "1");
        }
        else
        {
            add_param(params, sizeof(params), "0");
            if (want_bold)
                add_param(params, sizeof(params), "1");
            g_cur_fg = TAG_16 | 7;
            g_cur_bg = TAG_16 | 0;
        }
        g_cur_bold = want_bold;
    }
    if (need_fg && g_cur_fg != cell->fg)
    {
        color_param(params, sizeof(params), cell->fg, true);
        g_cur_fg = cell->fg;
    }
    if (need_bg && g_cur_bg != cell->bg)
    {
        color_param(params, sizeof(params), cell->bg, false);
        g_cur_bg = cell->bg;
    }
    if (params[0])
    {
        emits("\033[");
        emits(params);
        emits("m");
    }
}

static void move_to(int row, int col)
{
    char seq[24];
    if (g_cur_row == row && g_cur_col == col)
        return;
    if (g_cur_row == row && g_cur_col > 0 && col > g_cur_col)
    {
        if (col - g_cur_col == 1)
            emits("\033[C");
        else
        {
            snprintf(seq, sizeof(seq), "\033[%dC", col - g_cur_col);
            emits(seq);
        }
    }
    else
    {
        snprintf(seq, sizeof(seq), "\033[%d;%dH", row, col);
        emits(seq);
    }
    g_cur_row = row;
    g_cur_col = col;
}

size_t ansi_screen_update(const char **out)
{
    bool first = true;

    g_out_len = 0;
    for (int r = 0; r < SCREEN_ROWS; r++)
        for (int c = 0; c < SCREEN_COLS; c++)
        {
            cell_t *want = &g_next[r][c];
            cell_t *have = &g_shown[r][c];

            /* The bottom-right cell is never written: most terminals scroll the whole screen when it is */
            if (r == SCREEN_ROWS - 1 && c == SCREEN_COLS - 1)
                continue;
            if (have->ch != 0 && cell_same(want, have))
                continue;

            if (first)
            {
                /* Nothing is assumed about where the cursor or colours were left between updates */
                g_cur_row = g_cur_col = 0;
                g_cur_bold = -1;
                g_cur_fg = g_cur_bg = NO_COLOR;
                first = false;
            }
            move_to(r + 1, c + 1);
            set_colors(want);
            {
                const char *u = g_utf8 ? utf8_of(want->ch) : NULL;
                if (u != NULL)
                    emits(u);
                else
                    emit((const char *)&want->ch, 1);
            }
            *have = *want;
            if (c + 1 < SCREEN_COLS)
                g_cur_col = c + 2;
            else
                g_cur_row = g_cur_col = 0;   /* the last column: where the cursor goes depends on the terminal */
        }
    *out = g_out;
    return g_out_len;
}
