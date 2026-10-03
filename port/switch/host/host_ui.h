/*
HOST_UI.H

The screens before the game (host_ui.c), shown by the menu (host_lobby.c) and
the updater (host_update.c). Each draws the whole screen and shows it. A list
of keys is pairs of a button and what it does - "A", "Join", "B", "Back" -
ending in NULL.
*/

#ifndef __HALO_SWITCH_HOST_UI_H
#define __HALO_SWITCH_HOST_UI_H

/* a row of the internet games list: its columns' text */
struct host_ui_row
{
	const char *columns[4];
};

/* takes the default window for the screens, and gives it back for the game */
int host_ui_open(void);
void host_ui_close(void);

void host_ui_choice(const char *title, const char *const *items, int count, int selected, const char *note,
	const char *const *keys);
void host_ui_message(const char *title, const char *message, const char *const *keys);
void host_ui_progress(const char *title, const char *message, long long done, long long total,
	const char *const *keys);
/* how many rows the list shows at once */
int host_ui_list_rows(void);
/* columns are each column's left edge, from the list's */
void host_ui_list(const char *title, const char *aside, const char *const *headings, const int *columns,
	int column_count, const struct host_ui_row *rows, int count, int selected, int first, const char *empty,
	const char *status, const char *credit, const char *const *keys);

#endif
