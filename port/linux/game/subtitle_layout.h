/* Language-neutral presentation in the engine's 480-line coordinates. */
#ifndef HALO_SUBTITLE_LAYOUT_H
#define HALO_SUBTITLE_LAYOUT_H
#include <stdint.h>
typedef struct subtitle_rect { int16_t y0,x0,y1,x1; } subtitle_rect;
/* Returns 0 for invalid dimensions. Layout matches the console-tested footer.
 * The renderer must measure/wrap text; expand upward if more lines are needed. */
int subtitle_footer_layout(int width, subtitle_rect *panel, subtitle_rect *text);
#endif
