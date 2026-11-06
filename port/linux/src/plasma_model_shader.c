/* The selected point light gets (1 - d*d/r*r)^2. All other contributions
   and the stock shader's alpha, cone and material terms are retained.
   From GulchCE's plasma_model_shader.c. Here each backend declares the
   uniforms in its own blocks and the samplers at its own bindings
   (nv2a_vsh_vk.c, nv2a_psh_vk.c, nv2a_vsh_dk.c), and a shader without
   XGPU_PLASMA_MODEL_OCCLUSION has no samplers to bind: plasma_visibility is 1. */
#include "xgpu.h"
#include "plasma_model_shader.h"

void plasma_model_emit_visibility(struct xgpu_text *text, unsigned int plasma)
{
	if (!(plasma & XGPU_PLASMA_MODEL_OCCLUSION))
	{
		xgpu_text_append(text, "float plasma_visibility(int slot, vec3 position) { return 1.0; }\n");
		return;
	}
	xgpu_text_append(text,
		"float plasma_visibility(int slot, vec3 position) {\n"
		" if (plasma_model_parameters[2 + slot] < 0.5) return 1.0;\n"
		" vec4 tr = plasma_occ_transform[slot];\n"
		" vec3 uv = (position-tr.xyz)*tr.w+vec3(0.5);\n"
		" if (any(lessThan(uv,vec3(0.0))) || any(greaterThan(uv,vec3(1.0)))) return 1.0;\n"
		" return slot == 0 ? textureLod(plasma_occ_texture0,uv,0.0).r : textureLod(plasma_occ_texture1,uv,0.0).r;\n}\n");
}

void plasma_model_emit_vertex(struct xgpu_text *text, const uint32_t *instruction,
	unsigned long index, const struct nv2a_vertex_lighting *lighting)
{
	unsigned long constant = (instruction[1] >> 13) & 255;
	int slot;
	if (index == lighting->position_instruction)
		xgpu_text_append(text, "\tvec3 plasma_model_position = r%lu.xyz;\n", lighting->position_register);
	/* Only the color multiply-add of a recognized two-point-light model
	   program: B is the unswizzled, positive color constant. The ILU reads C,
	   so changing B.xyz cannot affect the parallel scalar instruction. */
	if (((instruction[1] >> 21) & 15) != 4 || (instruction[3] & 2) ||
		((instruction[2] >> 11) & 3) != 3 || ((instruction[2] >> 25) & 1) ||
		((instruction[2] >> 19) & 63) != 6 || index <= lighting->position_instruction)
		return;
	for (slot = 0; slot < 2; slot++)
		if (constant == (unsigned long)(XGPU_VERTEX_CONSTANT_BIAS - 77 + slot * 3))
			xgpu_text_append(text,
				"\tif (plasma_model_parameters[%d] > 0.5) {\n"
				"\t\tvec3 delta = c[%d].xyz - plasma_model_position;\n"
				"\t\tB.xyz *= max(1.0 - dot(delta, delta) * c[%d].w, 0.0) * plasma_visibility(%d,plasma_model_position);\n\t}\n",
				slot, XGPU_VERTEX_CONSTANT_BIAS - 79 + slot * 3, XGPU_VERTEX_CONSTANT_BIAS - 79 + slot * 3, slot);
}

void plasma_model_emit_pixel(struct xgpu_text *text, unsigned int plasma)
{
	plasma_model_emit_visibility(text, plasma);
	xgpu_text_append(text,
		"float plasma_model_falloff(float attenuation, float selected, int slot, vec3 position) {\n"
		"\treturn selected > 0.5 ? attenuation * attenuation * plasma_visibility(slot,position) : attenuation;\n}\n");
}
