#include "subtitle_layout.h"
int subtitle_footer_layout(int width, subtitle_rect *panel, subtitle_rect *text)
{
    if (!panel || !text || width < 320 || width > 4000) return 0;
    panel->y0=419; panel->y1=478; panel->x0=28; panel->x1=(int16_t)(width-28);
    *text=*panel; text->y0=444; text->x0=44; text->x1=(int16_t)(width-44);
    return 1;
}
