/*
MENU_CHIEF.H

The Multiplayer Color screen's Spartan, the model the game plays
(menu_chief.c).
*/

#ifndef __MENU_CHIEF_H
#define __MENU_CHIEF_H
#pragma once

struct bitmap_data;

void menu_chief_tags_loaded(char const *map_name);
void menu_chief_tags_unloaded(void);
/* the kept pixels of one of his bitmaps: FALSE if it is not one */
boolean menu_chief_bitmap_read(struct bitmap_data const *bitmap, void *buffer);
/* him, in these bounds of the screen, in that color of the profiles': FALSE
if he was not read */
boolean menu_chief_draw(short x0, short y0, short x1, short y1, short color_index);

#endif
