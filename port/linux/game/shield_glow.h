#ifndef SHIELD_GLOW_H
#define SHIELD_GLOW_H
boolean shield_glow_enabled(void);
void shield_glow_reset(void);
void shield_glow_update(void);
void shield_glow_frame_end(void);
void shield_glow_window_begin(void);
void shield_glow_submitted(long light, long rasterizer);
boolean shield_glow_selected(long rasterizer);
float shield_glow_radius(long light, float radius);
#endif
