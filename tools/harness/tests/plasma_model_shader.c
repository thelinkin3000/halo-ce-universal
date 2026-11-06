/* The Vulkan and deko3d translators with the dynamic lighting's model variant,
   over the game's real vertex programs. Adapted from GulchCE's test of its GL
   translators. test_plasma_model_shader.py compiles what this prints. */
#include "harness.h"
#include <stdarg.h>
#include "under_test.inc"

const char *config_string(const char *name) { return ""; }
int config_boolean(const char *name) { return 0; }

void xgpu_text_append(struct xgpu_text *text, const char *format, ...)
{
	char buffer[16384];
	va_list args;
	int length;
	va_start(args, format); length = vsnprintf(buffer, sizeof(buffer), format, args); va_end(args);
	CHECK(length >= 0 && (unsigned)length < sizeof(buffer), "bounded shader append");
	text->buffer = realloc(text->buffer, text->length + length + 1);
	memcpy(text->buffer + text->length, buffer, length + 1);
	text->length += length;
}

static char *vertex(unsigned program, unsigned plasma, unsigned long packed, int lit)
{
	struct nv2a_vertex_lighting lighting;
	int model = nv2a_vertex_shader_lighting(programs[program], lengths[program], &lighting);
#ifdef TEST_VULKAN
	return nv2a_vk_vertex_shader_to_glsl((const uint32_t *)programs[program], lengths[program], packed,
		lit && model ? &lighting : NULL, plasma);
#else
	(void)lit; (void)model;
	return nv2a_dk_vertex_shader_to_glsl((const uint32_t *)programs[program], lengths[program],
		packed | ((unsigned long)plasma << DK_VERTEX_KEY_PLASMA_SHIFT));
#endif
}

int main(int argc, char **argv)
{
	unsigned i, recognized = 0;
	if (argc == 6 && !strcmp(argv[1], "vertex"))
	{
		fputs(vertex(atoi(argv[2]), atoi(argv[3]), atoi(argv[4]), atoi(argv[5])), stdout);
		return 0;
	}
#ifdef TEST_VULKAN
	if (argc == 4 && !strcmp(argv[1], "pixel"))
	{
		static struct nv2a_pixel_shader_key key;
		key.per_pixel_lighting = (unsigned char)atoi(argv[2]);
		key.plasma_model = (unsigned char)atoi(argv[3]);
		fputs(nv2a_vk_pixel_shader_to_glsl(&key), stdout);
		return 0;
	}
#endif
	if (argc == 2 && !strcmp(argv[1], "programs"))
	{
		for (i = 0; i < sizeof(lengths) / sizeof(lengths[0]); i++)
		{
			struct nv2a_vertex_lighting lighting;
			if (nv2a_vertex_shader_lighting(programs[i], lengths[i], &lighting) && lighting.lights == 2)
				printf("%u\n", i);
		}
		return 0;
	}
	/* the checks */
	for (i = 0; i < sizeof(lengths) / sizeof(lengths[0]); i++)
	{
		struct nv2a_vertex_lighting lighting;
		int point = nv2a_vertex_shader_lighting(programs[i], lengths[i], &lighting) && lighting.lights == 2;
		char *stock = vertex(i, 0, 2, 0), *falloff = vertex(i, 1, 2, 0), *occluded = vertex(i, 3, 2, 0);
		CHECK(!strstr(stock, "plasma_") && !strstr(stock, "sampler3D"), "stock program %u has nothing of the variant", i);
		if (!point)
		{
			CHECK(!strcmp(stock, falloff) && !strcmp(stock, occluded), "program %u without both point lights stays stock", i);
			continue;
		}
		recognized++;
		CHECK(strstr(falloff, "if (plasma_model_parameters[0] > 0.5)"), "first point light's colour reached in program %u", i);
		CHECK(strstr(falloff, "if (plasma_model_parameters[1] > 0.5)"), "second point light's colour reached in program %u", i);
		CHECK(strstr(falloff, "vec3 plasma_model_position") < strstr(falloff, "B.xyz *="), "position captured before colour use");
		CHECK(!strstr(falloff, "sampler3D"), "falloff alone binds no sampler");
		CHECK(strstr(falloff, "return 1.0; }"), "falloff alone is fully visible");
		CHECK(strstr(occluded, "uniform sampler3D plasma_occ_texture0") && strstr(occluded, "textureLod(plasma_occ_texture1"),
			"occlusion samples both fields");
#ifdef TEST_VULKAN
		{
			char *lit = vertex(i, 1, 2, 1), *lit_stock = vertex(i, 0, 2, 1);
			CHECK(!strcmp(lit, lit_stock), "a program lit for each pixel leaves the variant to the pixel shader");
		}
#endif
	}
	CHECK(recognized >= 2, "real two-point-light programs tested");
#ifdef TEST_VULKAN
	{
		static struct nv2a_pixel_shader_key key;
		char *text;
		key.per_pixel_lighting = 2;
		text = nv2a_vk_pixel_shader_to_glsl(&key);
		CHECK(!strstr(text, "plasma_") && !strstr(text, "sampler3D"), "stock per-pixel lighting has nothing of the variant");
		CHECK(strstr(text, "light += max(1.0 - distance_squared * model_lights[1].w, 0.0)"), "first stock point light");
		CHECK(strstr(text, "light += max(1.0 - distance_squared * model_lights[4].w, 0.0)"), "second stock point light");
		key.plasma_model = 1;
		text = nv2a_vk_pixel_shader_to_glsl(&key);
		CHECK(strstr(text, "plasma_model_falloff(max(1.0 - distance_squared * model_lights[1].w, 0.0), plasma_model_parameters[0], 0"),
			"first selected point light squares its falloff");
		CHECK(strstr(text, "plasma_model_parameters[1], 1"), "second selected point light squares its falloff");
		CHECK(!strstr(text, "sampler3D"), "falloff alone binds no sampler");
		key.plasma_model = 3;
		text = nv2a_vk_pixel_shader_to_glsl(&key);
		CHECK(strstr(text, "set = 1, binding = 0) uniform sampler3D plasma_occ_texture0"), "occlusion samples its own set");
	}
#endif
	return 0;
}
