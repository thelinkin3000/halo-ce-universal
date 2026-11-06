#ifndef ROCKET_LIGHTING_H
#define ROCKET_LIGHTING_H

void rocket_lighting_update(void);
void rocket_lighting_forget(void);
void rocket_lighting_frame_end(void);
void rocket_lighting_window_begin(void);
void rocket_lighting_submitted(long light_index, long rasterizer_index);
int rocket_lighting_selected(long rasterizer_index);

float rocket_lighting_radius(long light_index, long object_index, float radius);

#endif
