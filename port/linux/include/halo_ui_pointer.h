/*
HALO_UI_POINTER.H

The mouse in the menus of the desktop builds. While a menu is up, the mouse
is released and its pointer shows (port/linux/src/sdl_platform.c); each frame
the menus (source/interface/ui_widget.c) ask where it is, in their own 640x480
coordinates (port/linux/src/d3d8_gl.c undoes the letterbox, the scale and the
widescreen centering), and what it did since.
*/

#ifndef HALO_UI_POINTER_H
#define HALO_UI_POINTER_H

struct halo_ui_pointer
{
	short x, y;
	/* where the latest left click was */
	short click_x, click_y;
	unsigned char moved;
	unsigned char left_clicks;
	unsigned char right_clicks;
	/* whole wheel notches, away from the user positive */
	signed char wheel_steps;
};

/* frees the mouse for the menus while menus_active, and captures it again
for aiming when not; while menus are active returns nonzero and what the
pointer did since the last call. Always 0 on Android. */
int halo_ui_pointer_update(int menus_active, struct halo_ui_pointer *pointer);

/* the open scoreboard's pointer (game_engine.c), offered (a network game's)
or not: 1 while a right click has freed it, with where it is and what it
did since the last call, in the screen's coordinates (the game's drawing,
not the menus' centered 640); 0 while it is not; -1 where there is none
(Android) */
int halo_scoreboard_pointer_update(int offered, struct halo_ui_pointer *pointer);

/* the open scoreboard picked from with a controller (game_engine.c): set to
the controller picking (-1: none), whose right stick and A the game then
does not see; read gives them, the stick's up and right positive, and 0
when no controller is at that port (port/linux/src/xinput_sdl.c) */
void halo_scoreboard_controller_set(int controller_index);
int halo_scoreboard_controller_read(int controller_index, short *stick_x, short *stick_y, int *a_down);

#endif
