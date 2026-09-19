/*
 * ansi_hud.h -- what the ANSI mode shows as text instead of Wolfenstein's own pictures of text: the status bar, the
 * menus, and the game's questions. Read straight from the game (ansi_hud.c), which runs in this process in ANSI mode.
 */

#ifndef ANSI_HUD_H
#define ANSI_HUD_H

#include <stdbool.h>

#define MENU_MAX_ITEMS  12
#define MENU_ITEM_LEN   44
#define MESSAGE_LINES   6

typedef struct
{
    bool show;                              /* a menu is up */
    char title[32];
    int  count;                             /* items, including the blanks the game uses as spacers */
    char items[MENU_MAX_ITEMS][MENU_ITEM_LEN];  /* an empty name is a spacer */
    bool dim[MENU_MAX_ITEMS];               /* can't be chosen (shareware episodes, Save Game before a game) */
    int  selected;
    int  message_lines;                     /* a question or notice from the game, shown instead of the items */
    char message[MESSAGE_LINES][72];
} ansi_menu_t;

typedef struct
{
    bool in_level;          /* playing a level, rather than the title, a menu or the tally between floors */
    bool menu;              /* a menu or question is up: keys go to it as they are */
    bool typing;            /* naming a saved game: letters go as typed text */
    int  floor, score, lives, health, ammo;
    int  weapon;            /* 0 knife, 1 pistol, 2 machine gun, 3 chaingun */
    int  best_weapon;
    bool gold_key, silver_key;
    ansi_menu_t menu_view;
} ansi_hud_t;

/* Takes a snapshot of the game. Called from the door's thread while the game runs on its own; a value can be a frame
 * old, which is fine for a status line. */
void ansi_hud_read(ansi_hud_t *hud);

#endif
