/* A model never leaves the screen because a light touched it: while the dynamic
   lighting's variant compiles, or if it fails, the draw takes the stock shaders.
   The real draw_shaders and plasma_draw_prepare of the Vulkan device (d3d8_vk.c)
   and draw_prepare's choice in the deko3d device (d3d8_dk.c), over a fake shader service. */
#include "harness.h"
#include <stdint.h>
typedef int BOOL;
typedef unsigned long DWORD;
#define TRUE 1
#define FALSE 0
#define XGPU_PLASMA_MODEL_FALLOFF 1u
#define XGPU_PLASMA_MODEL_OCCLUSION 2u
#define VK_SHADER_STAGE_VERTEX 0
#define VK_SHADER_STAGE_PIXEL 1
#define VK_IMAGE_3D 2
#define DK_TEXTURE_3D 2

struct nv2a_vertex_lighting { int lights; };
struct nv2a_pixel_shader_key { unsigned char per_pixel_lighting, plasma_model; };
struct xgpu_texture_description { int unused; };
struct vertex_shader_object
{
	unsigned long id, packed_mask;
	DWORD *instructions;
	struct nv2a_vertex_lighting lighting;
	BOOL lighting_failed, plasma_failed, fixed_function;
};
static struct vertex_shader_object the_program;
static DWORD words[4];
static struct { struct vertex_shader_object *vertex_shader; BOOL fixed_function, video_ready; } device;

static unsigned int light_mask;
unsigned int plasma_model_lighting_mask(void) { return light_mask; }
static int field_ready;
void *plasma_occlusion_model_texture(int slot, float transform[4]) { return field_ready ? (void *)words : NULL; }
static uint32_t vk_texture_get(const void *resource, const void *palette, int *kind, struct xgpu_texture_description *description)
{ *kind = VK_IMAGE_3D; return 7; }

/* the fake service. A hash says what is asked for: 1 stock, 2 lit stock, 16+bits a variant's vertex shader; a pixel
   key's hash is 100 + per_pixel_lighting * 10 + plasma_model. */
static int variant_ready, variant_failed, stock_ready = 1, variant_asks, waiting_asks, lit_on;
static uint32_t shader_handle(uint32_t stage, uint64_t hash, const struct vertex_shader_object *program,
	unsigned long mask, BOOL lit, unsigned int plasma, const struct nv2a_pixel_shader_key *key, BOOL wait, BOOL *failed)
{
	int variant = stage == VK_SHADER_STAGE_PIXEL ? key->plasma_model != 0 : plasma != 0;
	if (variant)
	{
		variant_asks++;
		if (wait) waiting_asks++;
		if (variant_failed) { *failed = TRUE; return 0; }
		return variant_ready ? (uint32_t)hash : 0;
	}
	return stock_ready ? (uint32_t)hash : 0;
}
static uint64_t vertex_shader_hash(const struct vertex_shader_object *program, unsigned long mask, BOOL lit, unsigned int plasma)
{ return plasma ? 16 + plasma : lit ? 2 : 1; }
static uint64_t pixel_shader_hash(const struct nv2a_pixel_shader_key *key)
{ return 100 + key->per_pixel_lighting * 10 + key->plasma_model; }
static BOOL per_pixel_lighting(void) { return lit_on; }
static struct vertex_shader_object *current_program(void) { return &the_program; }
static struct vertex_shader_object *fixed_function_program(void) { return &the_program; }
static const char *dump_folder(void) { return NULL; }
static void dump_vertex_shader(const char *f, const struct vertex_shader_object *p, unsigned long m) {}
static void dump_pixel_shader(const char *f, const struct nv2a_pixel_shader_key *k) {}
static void pixel_key_make(struct nv2a_pixel_shader_key *key) { memset(key, 0, sizeof(*key)); }
#define platform_log(...) ((void)0)

#include "under_test.inc"

static void reset(void)
{
	memset(&the_program, 0, sizeof(the_program));
	memset(&plasma_draw, 0, sizeof(plasma_draw));
	the_program.instructions = words;
	the_program.lighting.lights = 2;
	device.vertex_shader = &the_program;
	device.video_ready = TRUE;
	light_mask = 1; variant_ready = variant_failed = variant_asks = waiting_asks = lit_on = field_ready = 0;
	stock_ready = 1;
}

static uint32_t vertex, pixel;
static struct nv2a_pixel_shader_key key;
static unsigned long mask;
static BOOL shaders(void) { return draw_shaders(FALSE, &key, &mask, &vertex, &pixel); }

int main(int argc, char **argv)
{
	const char *name = argv[1];
	reset();
	if (!strcmp(name, "no_light"))
	{
		light_mask = 0;
		CHECK(shaders() && vertex == 1 && pixel == 100, "a model with no selected light draws stock");
		CHECK(!variant_asks && !plasma_draw.bits, "and the variant is not even asked for");
		lit_on = 1;
		CHECK(shaders() && vertex == 2 && pixel == 120 && !variant_asks, "lit for each pixel too");
	}
	else if (!strcmp(name, "other_program"))
	{
		the_program.lighting.lights = 1;
		CHECK(shaders() && vertex == 1 && pixel == 100 && !variant_asks, "a program without both point lights draws stock");
	}
	else if (!strcmp(name, "compiling"))
	{
		CHECK(shaders() && vertex == 1 && pixel == 100, "while the variant compiles the draw takes the stock shaders");
		CHECK(variant_asks == 1 && !waiting_asks, "its compile is asked for, without spending the frame on it");
		CHECK(!plasma_draw.bits, "and the draw's parameters are stock");
		lit_on = 1;
		CHECK(shaders() && vertex == 2 && pixel == 120 && !key.plasma_model && !plasma_draw.bits, "lit for each pixel too");
	}
	else if (!strcmp(name, "ready"))
	{
		variant_ready = 1;
		CHECK(shaders() && vertex == 17 && pixel == 100 && plasma_draw.bits == 1, "the vertex lit draw takes the variant's vertex shader");
		CHECK(plasma_draw.parameters[0] == 1 && plasma_draw.parameters[1] == 0, "its slot is selected");
		lit_on = 1;
		CHECK(shaders() && vertex == 2 && pixel == 121 && key.plasma_model == 1, "lit for each pixel, the pixel shader's variant");
		light_mask = 2; field_ready = 1;
		CHECK(shaders() && pixel == 123 && plasma_draw.images[0] == 7 && plasma_draw.images[1] == 7, "occlusion when a field is bound");
		CHECK(plasma_draw.parameters[2] == 1 && plasma_draw.parameters[3] == 1 && plasma_draw.parameters[1] == 1, "its parameters");
	}
	else if (!strcmp(name, "failed"))
	{
		variant_failed = 1;
		CHECK(shaders() && vertex == 1 && pixel == 100 && the_program.plasma_failed, "a failed variant draws stock and is remembered");
		variant_asks = 0;
		CHECK(shaders() && vertex == 1 && pixel == 100 && !variant_asks, "and is not asked for again");
		the_program.plasma_failed = FALSE; lit_on = 1;
		CHECK(shaders() && vertex == 2 && pixel == 120 && the_program.plasma_failed && !the_program.lighting_failed,
			"lit for each pixel: stock per-pixel lighting stays");
	}
	else if (!strcmp(name, "stock_not_ready"))
	{
		stock_ready = 0;
		CHECK(shaders() && !vertex && !pixel, "stock not ready either: as stock does, the draw waits");
	}
	else
		CHECK(0, "unknown case");
	return 0;
}
