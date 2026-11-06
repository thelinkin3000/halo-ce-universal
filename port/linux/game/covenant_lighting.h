#ifndef COVENANT_LIGHTING_H
#define COVENANT_LIGHTING_H

void covenant_lighting_update(void);
void covenant_lighting_forget(void);
void covenant_lighting_frame_end(void);
void covenant_lighting_window_begin(void);
void covenant_lighting_submitted(long light_index, long rasterizer_index);
int covenant_lighting_selected(long rasterizer_index);

float covenant_lighting_radius(long light_index, long object_index, float radius);

float covenant_lighting_scale(const char *name);
void covenant_lighting_color(long object_index, long definition_index, real_rgb_color *color);

int covenant_lighting_owns(long light_index, long object_index);

#endif
