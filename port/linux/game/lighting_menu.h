#ifndef LIGHTING_MENU_H
#define LIGHTING_MENU_H

/* Group controls stage ON/OFF normally in the menu; no legacy CUSTOM state.
   Return -1 for an unrelated setting, otherwise the operation's success. */
int lighting_menu_text(const char *name, char *text, unsigned int size, int defaults);
int lighting_menu_write(const char *name, const char *value);
/* Whether this image has the feature: the GL images' stub says no, and the
   Dynamic Lighting row is hidden (menu_functions.c). */
int lighting_menu_available(void);

#endif
