/*
 * ansi_hud.c -- reads what the ANSI mode draws as text, straight out of the game running in this process.
 *
 * The status bar comes from Wolfenstein's own gamestate. The menus come from the [wolf3d-trace] hooks the door's
 * build patches into the game (../patches/ansi-text-hooks.patch): which menu HandleMenu is running, where its gun
 * points, the text of the message box, and the name being typed. Built with the game's flags, so its headers are here.
 *
 * Reading from another thread without a lock is fine for this: every value is a plain int or a pointer to a menu
 * array that lives for the whole game, and at worst the door draws something a frame out of date.
 */

#include <stdio.h>
#include <string.h>

#include "wl_def.h"
#include "wl_menu.h"

/* ⚠️ wl_def.h turns on #pragma pack(1) for everything after it, which would lay ansi_hud_t out differently here than
 * in ansi_play.c (that mismatch crashed the door). Back to normal packing before our own header. */
#pragma pack()
#include "ansi_hud.h"

/* The hooks (patched into wl_menu.c and id_us.c) */
extern CP_iteminfo *trace_menu_info;
extern CP_itemtype *trace_menu_items;
extern volatile int trace_menu_which;
extern char trace_message[256];
extern volatile int trace_confirm;
extern char (*const trace_save_names)[MaxGameName];
extern volatile int trace_typing;
extern char trace_typed[MaxString];

/* The game's menus, to tell which one is up */
extern CP_itemtype MainMenu[], SndMenu[], CtlMenu[], NewEmenu[], NewMenu[], LSMenu[], CusMenu[];

/* Copies text into a line the door can print: CP437 only, no control characters */
static void copy_clean(char *out, size_t size, const char *in)
{
    size_t n = 0;
    for (; *in && n + 1 < size; in++)
        out[n++] = (unsigned char)*in < 32 ? ' ' : *in;
    out[n] = '\0';
}

/* A message box's text, split at the game's own line breaks */
static void split_message(ansi_menu_t *m, const char *text)
{
    const char *p = text;
    m->message_lines = 0;
    while (*p && m->message_lines < MESSAGE_LINES)
    {
        const char *end = strchr(p, '\n');
        size_t len = end != NULL ? (size_t)(end - p) : strlen(p);
        char line[72];
        if (len >= sizeof(line))
            len = sizeof(line) - 1;
        memcpy(line, p, len);
        line[len] = '\0';
        copy_clean(m->message[m->message_lines++], sizeof(m->message[0]), line);
        if (end == NULL)
            break;
        p = end + 1;
    }
}

static void read_menu(ansi_menu_t *m)
{
    CP_iteminfo *info = trace_menu_info;
    CP_itemtype *items = trace_menu_items;
    int count;

    memset(m, 0, sizeof(*m));

    /* A question (Quit? End the game?) or a notice covers the menu, as it does in the game */
    if (trace_message[0] && (trace_confirm || info != NULL))
    {
        m->show = true;
        split_message(m, trace_message);
        return;
    }
    if (trace_typing)
    {
        m->show = true;
        snprintf(m->message[0], sizeof(m->message[0]), "Name this saved game:");
        copy_clean(m->message[1], sizeof(m->message[1]), trace_typed);
        if (m->message[1][0] == '\0')
            snprintf(m->message[1], sizeof(m->message[1]), "_");
        snprintf(m->message[2], sizeof(m->message[2]), "Enter saves, Esc cancels");
        m->message_lines = 3;
        return;
    }
    if (info == NULL || items == NULL)
        return;

    count = info->amount < MENU_MAX_ITEMS ? info->amount : MENU_MAX_ITEMS;
    m->show = true;
    m->count = count;
    m->selected = trace_menu_which;

    if (items == MainMenu)          snprintf(m->title, sizeof(m->title), ingame ? "OPTIONS" : "WOLFENSTEIN 3D");
    else if (items == SndMenu)      snprintf(m->title, sizeof(m->title), "SOUND");
    else if (items == CtlMenu)      snprintf(m->title, sizeof(m->title), "CONTROL");
    else if (items == NewEmenu)     snprintf(m->title, sizeof(m->title), "WHICH EPISODE TO PLAY?");
    else if (items == NewMenu)      snprintf(m->title, sizeof(m->title), "HOW TOUGH ARE YOU?");
    else if (items == LSMenu)       snprintf(m->title, sizeof(m->title), "SAVED GAMES");
    else if (items == CusMenu)      snprintf(m->title, sizeof(m->title), "CUSTOMIZE CONTROLS");
    else                            snprintf(m->title, sizeof(m->title), "MENU");

    for (int i = 0; i < count; i++)
    {
        const char *name = items[i].string;
        char line[MENU_ITEM_LEN];

        if (items == LSMenu)
            name = trace_save_names[i][0] ? trace_save_names[i] : "- empty -";
        else if (items == SndMenu)
        {
            /* The sound menu's three headings are pictures; its "None"s only make sense under them */
            static const char *const heading[] = { "Effects: ", "Effects: ", "Effects: ", "", "",
                                                   "Digitized: ", "Digitized: ", "Digitized: ", "", "",
                                                   "Music: ", "Music: " };
            snprintf(line, sizeof(line), "%s%s", i < 12 ? heading[i] : "", name);
            copy_clean(m->items[i], sizeof(m->items[i]), line);
            m->dim[i] = items[i].active == 0;
            continue;
        }

        /* Episode names are two lines in the game */
        copy_clean(line, sizeof(line), name);
        for (char *p = line; *p; p++)
            if (*p == ' ' && p[1] == ' ')
                memmove(p, p + 1, strlen(p));
        if (items == NewEmenu)
        {
            const char *nl = strchr(name, '\n');
            if (nl != NULL)
                snprintf(line, sizeof(line), "%.*s - %s", (int)(nl - name), name, nl + 1);
        }
        snprintf(m->items[i], sizeof(m->items[i]), "%s", line);
        /* 0 is a spacer or not available yet (Save Game before a game); 3 is an episode shareware doesn't have */
        m->dim[i] = items[i].active == 0 || items[i].active == 3;
        if (items[i].active == 3)
            snprintf(m->items[i] + strlen(m->items[i]), sizeof(m->items[i]) - strlen(m->items[i]), " *");
    }
}

void ansi_hud_read(ansi_hud_t *hud)
{
    memset(hud, 0, sizeof(*hud));

    read_menu(&hud->menu_view);
    hud->typing = trace_typing != 0;
    hud->menu = hud->menu_view.show || trace_confirm;
    hud->in_level = ingame && !demoplayback && playstate == ex_stillplaying && !hud->menu;

    hud->floor = gamestate.mapon + 1;
    hud->score = gamestate.score;
    hud->lives = gamestate.lives;
    hud->health = gamestate.health;
    hud->ammo = gamestate.ammo;
    hud->weapon = gamestate.weapon;
    hud->best_weapon = gamestate.bestweapon;
    hud->gold_key = (gamestate.keys & 1) != 0;
    hud->silver_key = (gamestate.keys & 2) != 0;
}
