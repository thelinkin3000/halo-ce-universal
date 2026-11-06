/* Feature-owned attenuation for the existing additive world-light pass. */
#include "cseries.h"
#include <xtl.h>
#include "game/game_globals.h"
#include "rasterizer/common/rasterizer_common.h"
#include "rasterizer/xbox/rasterizer_xbox.h"
#include "rasterizer/rasterizer_swizzle.h"
#include "rasterizer/rasterizer.h"
#include "rasterizer/rasterizer_lights.h"
#include "plasma_lighting.h"
#include "plasma_occlusion.h"
#include <stdlib.h>

enum { PLASMA_FALLOFF_SIZE = 64 };
static IDirect3DVolumeTexture8 *plasma_falloff_texture;
static boolean plasma_falloff_bound;
static int plasma_falloff_stage;
static int plasma_falloff_filter;
static unsigned long plasma_specular_tint = 0x00FFFFFFUL;

void plasma_specular_begin(long rasterizer_index)
{
	real_rgb_color tint;
	real peak;
	plasma_specular_tint = 0x00FFFFFFUL;
	if (rasterizer_index < 0 || rasterizer_index >= rasterizer_lights.light_count ||
		!plasma_lighting_selected(rasterizer_index))
		return;
	tint = rasterizer_lights.lights[rasterizer_index].color;
	peak = MAX(tint.red, MAX(tint.green, tint.blue));
	/* The stock pass already applies scalar brightness. Preserve that peak,
	   but retain the source hue in both material reflection colours. */
	if (peak > 0.0f)
	{
		tint.red /= peak;
		tint.green /= peak;
		tint.blue /= peak;
	}
	plasma_specular_tint = real_rgb_color_to_pixel32(&tint);
}

unsigned long plasma_specular_color(unsigned long material)
{
	unsigned long result = material & 0xFF000000UL;
	int shift;
	if (plasma_specular_tint == 0x00FFFFFFUL)
		return material;
	for (shift = 0; shift < 24; shift += 8)
		result |= ((((material >> shift) & 255) *
			((plasma_specular_tint >> shift) & 255) + 127) / 255) << shift;
	return result;
}

/* Zero value and zero slope at the radius, without a hard luminous rim. */
static real plasma_falloff_value(real distance_squared)
{
	real remaining = MAX(0.0f, 1.0f - distance_squared);
	return remaining * remaining;
}

static boolean plasma_falloff_create(void)
{
	D3DLOCKED_BOX locked;
	unsigned long *pixels;
	int x, y, z;
	boolean success = FALSE;

	pixels = malloc(PLASMA_FALLOFF_SIZE * PLASMA_FALLOFF_SIZE * PLASMA_FALLOFF_SIZE * sizeof(*pixels));
	if (!pixels)
		return FALSE;
	for (z = 0; z < PLASMA_FALLOFF_SIZE; z++)
		for (y = 0; y < PLASMA_FALLOFF_SIZE; y++)
			for (x = 0; x < PLASMA_FALLOFF_SIZE; x++)
			{
				real dx = 2.0f * (x + 0.5f) / PLASMA_FALLOFF_SIZE - 1.0f;
				real dy = 2.0f * (y + 0.5f) / PLASMA_FALLOFF_SIZE - 1.0f;
				real dz = 2.0f * (z + 0.5f) / PLASMA_FALLOFF_SIZE - 1.0f;
				unsigned long value = (unsigned long)(255.0f * plasma_falloff_value(dx*dx + dy*dy + dz*dz) + 0.5f);
				pixels[(z * PLASMA_FALLOFF_SIZE + y) * PLASMA_FALLOFF_SIZE + x] = value * 0x01010101UL;
			}
	if (D3DDevice_CreateVolumeTexture(PLASMA_FALLOFF_SIZE, PLASMA_FALLOFF_SIZE, PLASMA_FALLOFF_SIZE,
		1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &plasma_falloff_texture) >= 0)
	{
		if (IDirect3DVolumeTexture8_LockBox(plasma_falloff_texture, 0, &locked, NULL, 0) >= 0 && locked.pBits)
		{
			rasterizer_xbox_bitmap_swizzle3d_long(locked.pBits, pixels,
				PLASMA_FALLOFF_SIZE, PLASMA_FALLOFF_SIZE, PLASMA_FALLOFF_SIZE);
			success = IDirect3DVolumeTexture8_UnlockBox(plasma_falloff_texture, 0) >= 0;
		}
	}
	free(pixels);
	if (!success)
		plasma_falloff_dispose();
	return success;
}

static void plasma_falloff_bind(long rasterizer_index, int stage, int stock_filter)
{
	D3DBaseTexture *occlusion;
	if (!plasma_lighting_selected(rasterizer_index))
		return;
	if (!plasma_falloff_texture && !plasma_falloff_create())
		return; /* allocation failure leaves the stock pass intact */
	occlusion = plasma_occlusion_texture(rasterizer_index, stage == 1);
	D3DDevice_SetTexture(stage, occlusion ? occlusion : (D3DBaseTexture *)plasma_falloff_texture);
	D3DDevice_SetTextureState_Deferred(stage, D3DTSS_MINFILTER, D3DTEXF_LINEAR);
	plasma_falloff_stage = stage;
	plasma_falloff_filter = stock_filter;
	plasma_falloff_bound = TRUE;
}

void plasma_falloff_begin(long rasterizer_index)
{
	plasma_falloff_bind(rasterizer_index, 2, D3DTEXF_POINT);
}

void plasma_falloff_specular_begin(long rasterizer_index)
{
	plasma_falloff_bind(rasterizer_index, 1, D3DTEXF_LINEAR);
}

void plasma_falloff_end(void)
{
	plasma_specular_tint = 0x00FFFFFFUL;
	if (!plasma_falloff_bound)
		return;
	rasterizer_set_texture_direct(plasma_falloff_stage, global_rasterizer_data->distance_attenuation.index, 0);
	D3DDevice_SetTextureState_Deferred(plasma_falloff_stage, D3DTSS_MINFILTER, plasma_falloff_filter);
	plasma_falloff_bound = FALSE;
}

void plasma_falloff_dispose(void)
{
	plasma_occlusion_reset();
	plasma_falloff_end();
	if (plasma_falloff_texture)
		D3DResource_Release((D3DResource *)plasma_falloff_texture);
	plasma_falloff_texture = NULL;
}
