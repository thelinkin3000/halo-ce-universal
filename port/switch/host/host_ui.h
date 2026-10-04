/*
HOST_UI.H

The screens before the game (host_ui.c), shown by the updater
(host_update.c) and while the disc image is unpacked (host_main.c). Each draws
the whole screen and shows it. A list of keys is pairs of a button and what it
does - "A", "Update now", "B", "Not now" - ending in NULL.
*/

#ifndef __HALO_SWITCH_HOST_UI_H
#define __HALO_SWITCH_HOST_UI_H

/* takes the default window for the screens, and gives it back for the game */
int host_ui_open(void);
void host_ui_close(void);

void host_ui_message(const char *title, const char *message, const char *const *keys);
void host_ui_progress(const char *title, const char *message, long long done, long long total,
	const char *const *keys);

#endif
