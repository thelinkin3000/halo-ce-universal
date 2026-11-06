#ifndef MUZZLE_LIGHTING_H
#define MUZZLE_LIGHTING_H

void muzzle_lighting_shot(long weapon_index, short trigger_index);
void muzzle_lighting_update(void);
void muzzle_lighting_frame_end(void);
void muzzle_lighting_forget(void);
void muzzle_lighting_window_begin(void);
void muzzle_lighting_submitted(long light_index, long rasterizer_index);
int muzzle_lighting_selected(long rasterizer_index);
float muzzle_lighting_radius(long light_index, float radius);

int muzzle_lighting_vehicle_occlusion(long light);

#endif
