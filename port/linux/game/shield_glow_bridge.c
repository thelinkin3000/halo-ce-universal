/* What the shared lighting modules ask of the shield glow, until its
   shared-lighting version arrives (docs/dynamic_lighting_port.md, Phase 5): the
   current shield_glow.c stays as it is, and its lights use the stock falloff. */
#include "cseries.h"
#include "shield_glow.h"

boolean shield_glow_enabled(void) { return FALSE; }
void shield_glow_reset(void) {}
void shield_glow_frame_end(void) {}
void shield_glow_window_begin(void) {}
void shield_glow_submitted(long light, long rasterizer) {}
boolean shield_glow_selected(long rasterizer) { return FALSE; }
float shield_glow_radius(long light, float radius) { return radius; }
