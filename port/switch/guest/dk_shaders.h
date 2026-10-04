/*
DK_SHADERS.H

The deko3d renderer's GLSL generators (nv2a_vsh_dk.c, nv2a_psh_dk.c; copies
of the OpenGL renderer's nv2a_vsh.c and nv2a_psh.c, port/linux/src): their
prototypes, the binding numbers and interface locations the generated
shaders use, and the uniform blocks' contents as C structs.

UAM links nothing: each stage is compiled alone, so a vertex output and the
pixel input it feeds meet only by their location, and every uniform lives
in a block with an explicit binding (UAM rejects loose uniforms). The
structs are the blocks' std140 layout byte for byte - vec4 members only,
which is what keeps a C struct and a std140 block from drifting apart - and
phase 6 fills one per draw. As dk_commands.h, every field is fixed-width,
so the host (LP64) can include this too.
*/

#ifndef __DK_SHADERS_H
#define __DK_SHADERS_H

#include <stdint.h>

/* uniform block bindings, per stage */
#define DK_BINDING_VERTEX_CONSTANTS 0
#define DK_BINDING_VERTEX_PARAMETERS 1
#define DK_BINDING_PIXEL_PARAMETERS 0

/* sampler bindings (the pixel stage's; texN is at binding N) */
#define DK_BINDING_TEXTURE0 0
#define DK_BINDING_TEXTURE1 1
#define DK_BINDING_TEXTURE2 2
#define DK_BINDING_TEXTURE3 3

/* the locations the vertex and pixel stages meet at: every vertex shader
writes all nine, and every pixel shader declares all nine, read or not, so
that any one vertex shader goes with any one pixel shader */
#define DK_LOCATION_D0 0
#define DK_LOCATION_D1 1
#define DK_LOCATION_B0 2
#define DK_LOCATION_B1 3
#define DK_LOCATION_T0 4
#define DK_LOCATION_T1 5
#define DK_LOCATION_T2 6
#define DK_LOCATION_T3 7
#define DK_LOCATION_FOG 8

/* the pixel shader's output */
#define DK_LOCATION_FRAGMENT_COLOR 0

/* layout(std140, binding = 0) uniform vertex_constants { vec4 c[192]; }:
Direct3D's constant registers c[-96..95], as the OpenGL renderer's uniform
array */
struct dk_vertex_constants
{
	float c[192][4];
};
typedef char dk_vertex_constants_size_check[sizeof(struct dk_vertex_constants) == 3072 ? 1 : -1];

/* layout(std140, binding = 1) uniform vertex_parameters {
vec4 viewport_scale; vec4 viewport_offset; vec4 point_and_screen; } */
struct dk_vertex_parameters
{
	float viewport_scale[4];
	float viewport_offset[4];
	/* x: the point size; y: the columns the menus shift by to center on a
	wide screen (the OpenGL renderer's screen_offset) */
	float point_and_screen[4];
};
typedef char dk_vertex_parameters_size_check[sizeof(struct dk_vertex_parameters) == 48 ? 1 : -1];

/* layout(std140, binding = 0) uniform pixel_parameters: the combiner
registers that live outside the program, and the texture stages' constants
(the OpenGL renderer's ps_c0, bump_matrix and so on) */
struct dk_pixel_parameters
{
	float ps_c0[8][4];
	float ps_c1[8][4];
	float ps_final_c0[4];
	float ps_final_c1[4];
	float fog_color[4];
	float fog_parameters[4];
	/* the alpha test's reference value, in x */
	float alpha_reference[4];
	float bump_matrix[4][4];
	float bump_luminance[4][4];
	float texture_scale[4][4];
};
typedef char dk_pixel_parameters_size_check[sizeof(struct dk_pixel_parameters) == 528 ? 1 : -1];

/* GLSL for UAM for an NV2A vertex program (the instruction words after the
program header). Attributes whose bit is set in packed_attribute_mask are
fed as NORMPACKED3 32-bit integers and unpacked in the shader. Returns a
malloc'd string. */
char *nv2a_dk_vertex_shader_to_glsl(const uint32_t *instructions, unsigned long instruction_count,
	unsigned long packed_attribute_mask);

struct nv2a_pixel_shader_key;

/* GLSL for UAM for an Xbox pixel shader (the key is xgpu.h's); the key's
count_samples is ignored - under deko3d the visibility tests count with
DkCounter_SamplesPassed. Returns a malloc'd string. */
char *nv2a_dk_pixel_shader_to_glsl(const struct nv2a_pixel_shader_key *key);

#endif
