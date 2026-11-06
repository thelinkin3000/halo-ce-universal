/* Render-only plasma selection and shader additions. No tag data is changed. */
#ifndef PLASMA_MODEL_SHADER_H
#define PLASMA_MODEL_SHADER_H
#include <stdint.h>
struct xgpu_text;
struct nv2a_vertex_lighting;
void plasma_model_lighting_reset(void);
void plasma_model_lighting_slot(long light_index, short slot);
unsigned int plasma_model_lighting_mask(void);
/* what a variant has (xgpu.h's XGPU_PLASMA_MODEL_*) */
void plasma_model_emit_visibility(struct xgpu_text *text, unsigned int plasma);
void plasma_model_emit_vertex(struct xgpu_text *text, const uint32_t *instruction,
	unsigned long index, const struct nv2a_vertex_lighting *lighting);
void plasma_model_emit_pixel(struct xgpu_text *text, unsigned int plasma);
#endif
