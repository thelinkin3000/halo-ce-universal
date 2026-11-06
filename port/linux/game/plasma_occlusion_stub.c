/* Occlusion is not linked yet (docs/dynamic_lighting_port.md, Phase 6): no rays
   and no field textures. With no occlusion texture, plasma_falloff_bind uses the
   plain falloff texture, and models read as fully visible. */
#include "cseries.h"
#include "plasma_occlusion.h"

void plasma_occlusion_reset(void) {}
void plasma_occlusion_window(void) {}
void plasma_occlusion_submit(long light, long rasterizer_index) {}
void plasma_occlusion_model_slot(long rasterizer_index, short slot) {}
void *plasma_occlusion_texture(long rasterizer_index, int specular) { return NULL; }
void *plasma_occlusion_model_texture(int slot, float transform[4]) { return NULL; }
unsigned long plasma_occlusion_view_serial(void) { return 0; }
