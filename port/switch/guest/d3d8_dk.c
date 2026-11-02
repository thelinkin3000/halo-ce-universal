/*
D3D8_DK.C

The Xbox Direct3D 8 device for the Switch's deko3d renderer
(port/switch/DEKO3D.md). Linked into the Switch's game image,
halo_guest.elf, in place of port/linux/src/d3d8_gl.c, which the other builds
keep unchanged.

The parts of d3d8_gl.c that are not OpenGL are copied here as they are - the
screen's width, the state the XDK's inline functions keep, the vertical
blank, the reserved viewport constants, render and texture stage state,
vertex shaders and their declarations, streams and immediate mode - and are
to be merged with it later. What d3d8_gl.c does with OpenGL, this file will
do through the host's deko3d backend (port/switch/host/host_dk.c), to which
it writes a stream of commands over a frame (dk_commands.h). So far clears
and presenting go there; a draw and a visibility test do nothing yet.
*/

#include "xgpu.h"
#include "halo_port_window.h"
#include "sdl_platform.h"
#include "halo_ui_pointer.h"
#include "port_config.h"
#include "posix.h"
#include "dk_commands.h"
#include "dk_shaders.h"
#include "dk_textures.h"
#include "../../linux/game/cache_file_formats.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

void d3d8_surface_initialize(D3DSurface *surface, D3DFORMAT format, unsigned long width, unsigned long height);
void d3d8_surface_resize(D3DSurface *surface, D3DFORMAT format, unsigned long width, unsigned long height);

/* what the context supports: xbox_textures.c reads it (as d3d8_gl.c
defines it on Android); nothing here is OpenGL, so nothing is supported */
struct xgpu_capabilities xgpu_capabilities;

/* ---------- the screen's width

The Xbox screen is 640x480. The native ports can draw a wider one: 480
lines, and as many columns as the display's shape gives. On Android that is
display.screen_width (port_config.c; 640 keeps 4:3); on the desktop, the
display's shape while the game is fullscreen, and 640 in a window. The
game's camera derives its horizontal field of view from the viewport, so the
3D view simply widens. The menus and full-screen overlays are laid out for
640 columns; while they draw (halo_screen_ui_offset), everything shifts right
to center them.

Fullscreen on the desktop also draws at the display's resolution: render
targets the size of the screen get that many pixels (screen_scale), and
viewports, clears and visibility counts are scaled to match, so the game
still works in its 480 lines. The width and the scale change only between
frames, after one is presented (halo_screen_commit). */

#define SCREEN_HEIGHT 480
#define SCREEN_MAXIMUM_WIDTH 1920

/* the width the game draws, 0 until first asked, and how many pixels a
render target the size of the screen has per unit of it */
static long screen_width;
static float screen_scale[2] = { 1.0f, 1.0f };
static long ui_offset;
#define UI_OFFSET ((GLint)ui_offset)

#ifdef HALO_SWITCH
/* the host's (host_dk.c): the height it presents at, 1080 docked and 720 in
handheld */
unsigned int host_dk_output_height(void);
unsigned int host_dk_gpu_frame_us(void);

/* ---------- display.dynamic_resolution

While the GPU's frame, averaged over the last 16 or so (host_dk_gpu_frame_us),
is over DYNAMIC_DROP_US for DYNAMIC_DROP_FRAMES frames in a row, the screen
is drawn one step lower (1080 to 900, 900 to 720, 720 to 480: never more);
it goes back once the frame at the chosen lines, reckoned from the lower
one's by their pixels, would be under DYNAMIC_RAISE_US for
DYNAMIC_RAISE_FRAMES frames. A change waits DYNAMIC_HOLD_FRAMES after the
last (the targets are made again at each), and the average its first
DYNAMIC_SETTLE_FRAMES (it is the old size's until then). Asked between
frames (screen_mode_choose, from halo_screen_commit), once a frame. */
/* (dropped under 55 frames a second, 1/55 s of the GPU, for half a second;
back up within 90% of a 60 Hz frame for a second) */
#define DYNAMIC_DROP_US 18182
#define DYNAMIC_RAISE_US 15000
#define DYNAMIC_DROP_FRAMES 30
#define DYNAMIC_RAISE_FRAMES 60
#define DYNAMIC_HOLD_FRAMES 120
#define DYNAMIC_SETTLE_FRAMES 20

static struct
{
	unsigned int chosen, lower;
	int dropped;
	unsigned long frames_since_change, over, under;
} dynamic;

/* the lines a step below these (the steps 480, 720, 900 and 1080) */
static unsigned int dynamic_step_below(unsigned int lines)
{
	return lines > 900 ? 900 : lines > 720 ? 720 : 480;
}

static unsigned int dynamic_resolution(unsigned int lines)
{
	unsigned int gpu_us;

	if (!config_boolean("display.dynamic_resolution") || lines <= SCREEN_HEIGHT)
	{
		dynamic.dropped = 0;
		dynamic.chosen = lines;
		return lines;
	}
	if (lines != dynamic.chosen)
	{
		/* (another choice of lines, or the console docked or undocked:
		begun again at it) */
		dynamic.chosen = lines;
		dynamic.dropped = 0;
		dynamic.frames_since_change = dynamic.over = dynamic.under = 0;
	}
	dynamic.lower = dynamic_step_below(lines);
	dynamic.frames_since_change++;
	gpu_us = host_dk_gpu_frame_us();
	if (dynamic.frames_since_change < DYNAMIC_SETTLE_FRAMES || !gpu_us)
		return dynamic.dropped ? dynamic.lower : lines;
	if (!dynamic.dropped)
	{
		dynamic.over = gpu_us > DYNAMIC_DROP_US ? dynamic.over + 1 : 0;
		if (dynamic.over >= DYNAMIC_DROP_FRAMES && dynamic.frames_since_change >= DYNAMIC_HOLD_FRAMES)
		{
			dynamic.dropped = 1;
			dynamic.frames_since_change = dynamic.over = dynamic.under = 0;
			platform_log("dynamic resolution: the GPU at %.1f ms a frame: %u lines, from %u", gpu_us / 1000.0,
				dynamic.lower, lines);
		}
	}
	else
	{
		float ratio = (float)lines / (float)dynamic.lower;
		float reckoned = gpu_us * ratio * ratio;

		dynamic.under = reckoned < DYNAMIC_RAISE_US ? dynamic.under + 1 : 0;
		if (dynamic.under >= DYNAMIC_RAISE_FRAMES && dynamic.frames_since_change >= DYNAMIC_HOLD_FRAMES)
		{
			dynamic.dropped = 0;
			dynamic.frames_since_change = dynamic.over = dynamic.under = 0;
			platform_log("dynamic resolution: the GPU at %.1f ms a frame at %u lines, %.1f at %u: back to %u",
				gpu_us / 1000.0, dynamic.lower, reckoned / 1000.0, lines, lines);
		}
	}
	return dynamic.dropped ? dynamic.lower : lines;
}

/* for debug.show_resolution's corner (main.c): whether the screen is a
step under the chosen lines now */
int halo_screen_dynamic_resolution_dropped(void)
{
	return dynamic.dropped;
}
#endif

static void screen_mode_choose(long *width, float scale[2])
{
#ifdef HALO_ANDROID
	/* display.screen_width, or 0 for the display's shape, which the app
	passes (port/android/host/host_main.c) */
	const char *display = getenv("HALO_DISPLAY_WIDTH");

	*width = config_integer("display.screen_width");
	if (*width <= 0)
		*width = display ? atol(display) : 640;
	if (*width < 640)
		*width = 640;
	if (*width > 1600)
		*width = 1600;
	*width &= ~1L;
	scale[0] = scale[1] = 1.0f;
#ifdef HALO_SWITCH
	/* display.resolution_limit: the lines the screen's targets are drawn at,
	"480" (the game's own), "720", "900", "1080" or any number of lines
	between, no more than the host presents at (1080 docked, 720 in handheld),
	which the host scales them up to: "900" docked is 69% of 1080p's pixels,
	the GPU's room for 60 a second in a heavy scene */
	{
		long lines = atol(config_string("display.resolution_limit"));
		unsigned int height = lines > SCREEN_HEIGHT ? (unsigned int)(lines < 1080 ? lines : 1080) : SCREEN_HEIGHT;
		unsigned int output = host_dk_output_height();

		/* (debug.resolution_uncapped: drawn at those lines all the same, and
		scaled down to the screen: a Lite's handheld 720p drawing docked
		1080p's load, for testing it) */
		if (height > output && !config_boolean("debug.resolution_uncapped"))
			height = output;
		height = dynamic_resolution(height);
		if (height > SCREEN_HEIGHT)
			scale[0] = scale[1] = (float)height / (float)SCREEN_HEIGHT;
	}
#endif
#else
	long display_width, display_height;

	*width = 640;
	scale[0] = scale[1] = 1.0f;
	if (platform_screen_mode(&display_width, &display_height) && display_width > 0 && display_height > 0)
	{
		long wanted = (SCREEN_HEIGHT * display_width + display_height / 2) / display_height;

		*width = wanted < 640 ? 640 : wanted > SCREEN_MAXIMUM_WIDTH ? SCREEN_MAXIMUM_WIDTH : wanted & ~1L;
		scale[0] = (float)display_width / (float)*width;
		scale[1] = (float)display_height / (float)SCREEN_HEIGHT;
		/* a display narrower or wider than the game can be: the picture
		keeps its shape and the display blit letterboxes it */
		if (*width != wanted && *width != (wanted & ~1L))
			scale[0] = scale[1] = scale[0] < scale[1] ? scale[0] : scale[1];
	}
#endif
}

long halo_screen_width(void)
{
	if (!screen_width)
	{
		screen_mode_choose(&screen_width, screen_scale);
		platform_log("screen: %ldx%d drawn at %.0fx%.0f", screen_width, SCREEN_HEIGHT,
			screen_width * screen_scale[0], SCREEN_HEIGHT * screen_scale[1]);
	}
	return screen_width;
}

/* the display's pixels for each of the 480 lines (text_hires.c) */
float halo_screen_pixel_scale(void)
{
	halo_screen_width();
	return screen_scale[1];
}

/* how many pixels the screen's targets draw to the Xbox's one, the larger
of the two ways (the screen effects' convolutions: rasterizer_xbox_screen_effect.c) */
float halo_screen_scale(void)
{
	halo_screen_width();
	return screen_scale[0] > screen_scale[1] ? screen_scale[0] : screen_scale[1];
}

/* display.model_detail (as d3d8_gl.c's): "stock" chooses a model's level of
detail by the Xbox's 480 lines, "native" by the display's, "highest" draws
the most detailed level of whatever is drawn at all */
enum
{
	_model_detail_stock,
	_model_detail_native,
	_model_detail_highest,
};

/* (read again only when a setting changes: every model asks, every frame) */
static int model_detail_setting(void)
{
	static unsigned long read_at = (unsigned long)-1;
	static int value;

	if (read_at != config_changes())
	{
		const char *setting = config_string("display.model_detail");

		read_at = config_changes();
		value = !strcmp(setting, "native") ? _model_detail_native :
			!strcmp(setting, "highest") ? _model_detail_highest : _model_detail_stock;
	}
	return value;
}

int halo_model_detail_stock(void)
{
	return model_detail_setting() == _model_detail_stock;
}

float halo_model_detail_scale(int level)
{
	float display_scale;

	if (model_detail_setting() == _model_detail_stock)
		return 1.0f;
	if (model_detail_setting() == _model_detail_highest && level)
		return 1.0e6f;
	display_scale = halo_screen_pixel_scale();
	return display_scale > 1.0f ? display_scale : 1.0f;
}

/* (the OpenGL renderer's alone, which the settings offer only there:
display.overshield_effect's fade up the body, which this renderer's vertex
shaders do not draw, and display.hdr's tone mapping) */
void d3d8_gl_set_height_fade(float inverse_softness, float height, float offset, float scale)
{
	(void)inverse_softness;
	(void)height;
	(void)offset;
	(void)scale;
}

void halo_screen_tone_map(short x0, short y0, short x1, short y1)
{
	(void)x0;
	(void)y0;
	(void)x1;
	(void)y1;
}

void halo_screen_ui_offset(unsigned char centered)
{
	ui_offset = centered ? (halo_screen_width() - 640) / 2 : 0;
}

/* (as d3d8_gl.c's: a split screen's view centres the menus' 640 columns in
it, a quarter's 320) */
void halo_screen_ui_offset_view(long view_width)
{
	long width = halo_screen_width();

	ui_offset = view_width > 0 && view_width < width ? (view_width > 320 ? (view_width - 320) / 2 : 0) : (width - 640) / 2;
}

long halo_screen_ui_offset_current(void)
{
	return ui_offset;
}

/* display.reversed_depth is the OpenGL renderer's (d3d8_gl.c): here the
depth is the Xbox's */
BOOL xgpu_reversed_depth;

/* HDR Calibration is the desktop's: no HDR display here */
int halo_hdr_calibration_available(void)
{
	return 0;
}

void halo_hdr_calibration_show(int pattern, int paper_white, int peak_brightness)
{
	(void)pattern;
	(void)paper_white;
	(void)peak_brightness;
}

/* the OpenGL renderer's static geometry (d3d8_gl.c's static buffers, an
Android fix): here the game's vertices and indices are drawn as before
(d3d8_resources.c forgets a renamed buffer's copy itself) */
void xgpu_static_buffer_add(const void *resource, unsigned long size, int indices)
{
	(void)resource;
	(void)size;
	(void)indices;
}

void xgpu_static_buffers_add(const void *buffers, long count, unsigned long stride, int indices, const void *end)
{
	(void)buffers;
	(void)count;
	(void)stride;
	(void)indices;
	(void)end;
}

void xgpu_static_buffers_remove(const void *buffers, long count, unsigned long stride, int indices)
{
	(void)buffers;
	(void)count;
	(void)stride;
	(void)indices;
}

void xgpu_static_range_add(const void *start, unsigned long size, int indices)
{
	(void)start;
	(void)size;
	(void)indices;
}

void xgpu_static_range_remove(const void *start, int indices)
{
	(void)start;
	(void)indices;
}

void xgpu_static_forget_all(void)
{
}

/* display.shadow_resolution, display.per_pixel_lighting and
display.anti_aliasing are the OpenGL renderer's (d3d8_gl.c): here the shadow
maps stay the Xbox's 128x128, every draw is lit as its vertex shader lights
it, and the 3D view is drawn as it is */
long halo_shadow_map_scale(void)
{
	return 1;
}

void halo_vertex_shader_lighting(unsigned long handle)
{
	(void)handle;
}

void halo_screen_anti_alias(short x0, short y0, short x1, short y1)
{
	(void)x0; (void)y0; (void)x1; (void)y1;
}

static void *stream_command(uint32_t type, unsigned long size);

/* texture coordinate 0's clip, for the draws that follow: the lowest and
highest u and v as the texture is sampled (0 to 1 across it), and whether
to clip (the pixel parameters' alpha_reference.y to .w) */
static float texture_clip[3];
/* the stage clipped, whether in three coordinates, and whether its black
texels add nothing (the shadows': halo_texture_clip) */
static int texture_clip_stage;
static int texture_clip_3d;
static int texture_clip_black;

/* The draws that follow shaded only where texture coordinate 0's u and v are
in [low, high], or everywhere: an object's shadow cast on the level
(rasterizer_xbox_shadows.c) changes nothing outside its map, and was shaded
over the whole of every triangle it falls on - most of a crowd's GPU time.
The pixel key has NV2A_PIXEL_KEY_CLIP while it is on, and that shader lets
the pixels outside go before it reads a texture (nv2a_psh_dk.c). (The vertex
shaders' clip distances were tried first: the clipper did not clip by the
values they were given.) */
void halo_texture_clip_stage(short stage, unsigned char three_dimensional, float low, float high);

void halo_texture_clip(unsigned char clip, float low, float high)
{
	halo_texture_clip_stage(clip ? 0 : -1, FALSE, low, high);
	/* (an object's shadow cast on the level darkens it by its map times
	its colour, corner fade and vertex alpha, each no more than 1: where the
	map is black it darkens nothing - rasterizer_xbox_shadows.c) */
	texture_clip_black = texture_clip[2] != 0.0f;
}

/* as halo_texture_clip, for a stage's coordinates (stage -1 for none), in
two or three: the dynamic lights' passes on the level
(rasterizer_xbox_environment.c) add nothing where their distance
attenuation, black past its border, is past the light's box */
void halo_texture_clip_stage(short stage, unsigned char three_dimensional, float low, float high)
{
	unsigned char clip = stage >= 0 && stage < 4;

	/* debug.gpu_texture_clip: "on", "off" (never clipped), "wide" (clipped
	by limits nothing reaches: the clip's own effect, apart from where), or
	"<low>,<high>": the limits to clip by, whatever the caller asks */
	static int mode = -1;
	static float set_low, set_high;

	if (mode < 0)
	{
		const char *setting = config_string("debug.gpu_texture_clip");

		mode = !strcmp(setting, "off") ? 0 : !strcmp(setting, "wide") ? 2 : 1;
		if (mode == 1 && sscanf(setting, "%f,%f", &set_low, &set_high) == 2)
			mode = 3;
		if (mode != 1)
			platform_log("texture clip: %s (debug.gpu_texture_clip)", setting);
	}
	if (mode == 0)
		clip = FALSE;
	else if (mode == 2)
	{
		low = -1000.0f;
		high = 1000.0f;
	}
	else if (mode == 3)
	{
		low = set_low;
		high = set_high;
	}
	texture_clip[0] = clip ? low : 0.0f;
	texture_clip[1] = clip ? high : 0.0f;
	texture_clip[2] = clip ? 1.0f : 0.0f;
	texture_clip_stage = clip ? stage : 0;
	texture_clip_3d = clip && three_dimensional;
	texture_clip_black = FALSE;
}

/* the rectangle the draws that follow are rasterized in, if on (in the
target's game pixels), as well as the viewport (halo_draw_scissor) */
static unsigned char draw_scissor_on;
static float draw_scissor[4];

/* A draw that changes nothing outside a box of the world - an object's
shadow cast on the level, a dynamic light's pass - rasterized only inside
that box's rectangle on the screen (rasterizer_xbox.c): their triangles,
the level's, are much bigger than what they change, and the pixels outside
were shaded only to be let go (halo_texture_clip). debug.gpu_draw_scissor =
false: never. */
void halo_draw_scissor(unsigned char on, float x0, float y0, float x1, float y1)
{
	static int enabled = -1;

	if (enabled < 0)
	{
		enabled = config_boolean("debug.gpu_draw_scissor") != 0;
		if (!enabled)
			platform_log("draw scissor: off (debug.gpu_draw_scissor)");
	}
	draw_scissor_on = on && enabled;
	draw_scissor[0] = x0;
	draw_scissor[1] = y0;
	draw_scissor[2] = x1;
	draw_scissor[3] = y1;
}

/* the zoom's blend of its blur over the screen, for the draws that follow
(rasterizer_xbox_screen_effect.c): the pixel key has NV2A_PIXEL_KEY_WARP_BLEND
while it is on */
static unsigned char warp_blend;

/* the zoom's blur drawn on a small copy of the screen and blended over it
once (rasterizer_xbox_screen_effect.c), instead of its passes over the whole
screen; debug.original_zoom draws it as the game does */
unsigned char halo_screen_fast_warp(void)
{
	static int on = -1;

	if (on < 0)
	{
		on = !config_boolean("debug.original_zoom");
		if (!on)
			platform_log("zoom: as the game draws it (debug.original_zoom)");
	}
	return (unsigned char)on;
}

void halo_screen_warp_blend(unsigned char on)
{
	warp_blend = on;
}

/* a blur drawn small (rasterizer_xbox_screen_effect.c); debug.original_blur
draws it as the game does */
unsigned char halo_screen_fast_blur(void)
{
	static int on = -1;

	if (on < 0)
	{
		on = !config_boolean("debug.original_blur");
		if (!on)
			platform_log("screen blur: as the game draws it (debug.original_blur)");
	}
	return (unsigned char)on;
}

/* the part of a frame the game is about to draw, for the host's record of
the commands it ran (DK_COMMAND_MARKER) */
int halo_render_defer_sun_glow(void)
{
	return 0;
}

int halo_render_defer_water(void)
{
	return 0;
}

void halo_render_marker(char const *name)
{
	struct dk_command_marker *command = stream_command(DK_COMMAND_MARKER, sizeof(*command));
	size_t length = 0;

	while (name[length] && length < sizeof(command->name) - 1)
	{
		command->name[length] = name[length];
		length++;
	}
	while (length < sizeof(command->name))
		command->name[length++] = 0;
}

/* ---------- state the XDK header's inline functions read and write */

DWORD D3D__RenderState[D3DRS_MAX];
DWORD D3D__TextureState[D3DTSS_MAXSTAGES][D3DTSS_MAX];
WORD *D3D__IndexData;
BYTE D3D__StateBlockDirty[1024];

/* ---------- vertex shaders */

#define VERTEX_SHADER_SIGNATURE 0x76736864UL /* 'vshd' */
#define VERTEX_PROGRAM_SLOTS 136

struct vertex_element
{
	unsigned char reg;
	unsigned char stream;
	unsigned char type;
	unsigned char bytes;
	unsigned short offset;
};

struct vertex_shader_object
{
	unsigned long signature;
	unsigned long id;
	DWORD *instructions;
	unsigned long instruction_count;
	/* FNV-1a 64 over the instruction count and words (dk_shaders.h's
	mixers): the program's hash, half of the shader key the deko3d
	renderer's cache knows a vertex shader by (dk_shaders.c) */
	uint64_t program_hash;
	struct vertex_element elements[XGPU_VERTEX_ATTRIBUTE_COUNT];
	unsigned long element_count;
	unsigned long packed_mask;
};

/* ---------- the device */

struct dk_device
{
	D3DPRESENT_PARAMETERS presentation;
	D3DSurface back_buffer;
	/* the frame shown last, as the loading screen reads it (GetBackBuffer
	with -1: front_buffer_ready); the frame it was last asked for in, and
	the frame its picture was last taken in (front_buffer_take), each + 1 */
	D3DSurface front_buffer;
	unsigned long front_buffer_asked;
	unsigned long front_buffer_taken;
	D3DSurface depth_buffer;
	D3DSurface *render_target;
	D3DSurface *depth_stencil;
	D3DVIEWPORT8 viewport;
	D3DMATRIX transforms[D3DTS_MAX];
	D3DBaseTexture *textures[D3DTSS_MAXSTAGES];
	D3DPalette *palettes[D3DTSS_MAXSTAGES];
	D3DSHADERCONSTANTMODE shader_constant_mode;

	struct vertex_shader_object *vertex_shader;
	/* SetVertexShader chose the fixed-function pipeline (with 0): the
	loading screen's draws (nv2a_dk_fixed_function_glsl) */
	BOOL fixed_function;
	struct vertex_shader_object *program_slots[VERTEX_PROGRAM_SLOTS];
	unsigned long program_address;
	float constants[XGPU_VERTEX_CONSTANT_COUNT][4];
	float viewport_scale[4];
	float viewport_offset[4];

	struct
	{
		DWORD data;
		UINT stride;
		/* the buffer itself, whose Lock records its last use */
		D3DVertexBuffer *buffer;
	} streams[16];
	/* SetIndices' base vertex (d3d8_gl.c) */
	UINT base_vertex_index;
	D3DIndexBuffer *index_buffer;

	/* the current value of each input register (SetVertexData) */
	float attributes[XGPU_VERTEX_ATTRIBUTE_COUNT][4];
	BOOL immediate_active;
	D3DPRIMITIVETYPE immediate_type;
	float *immediate_vertices;
	unsigned long immediate_count;
	unsigned long immediate_capacity;
	/* the registers written since Begin, the vertex each was first written
	at, and the values they had at Begin (for the vertices before that) */
	unsigned long immediate_mask;
	unsigned long immediate_first[XGPU_VERTEX_ATTRIBUTE_COUNT];
	float immediate_begin[XGPU_VERTEX_ATTRIBUTE_COUNT][4];

	BOOL visibility_test_active;

	unsigned long frame;
	unsigned long next_vertex_shader_id;
	BOOL created;
};

static struct dk_device device;

static D3DDevice *device_pointer(void)
{
	return (D3DDevice *)&device;
}

static float dword_to_float(DWORD value)
{
	union { DWORD d; float f; } u;

	u.d = value;
	return u.f;
}

/* ---------- the command stream (dk_commands.h)

Commands are written here over a frame and handed to the host half
(port/switch/host/host_dk.c) at its end, or sooner if the stream fills. */

void host_dk_submit(unsigned int commands, unsigned int size);
/* the highest submission the GPU has finished (host_dk.c) */
unsigned int host_dk_retired(void);
/* the queue drained (host_dk.c) */
void host_dk_idle(void);

void halo_switch_renderer_idle(void)
{
	host_dk_idle();
}
/* a visibility test slot's latest count (host_dk.c) */
unsigned int host_dk_visibility(unsigned int index);

#define STREAM_SIZE (1024 * 1024)

static uint32_t stream[STREAM_SIZE / 4];
static unsigned long stream_used;
/* Each handing over of the stream is one submission, numbered from 1 as the
host numbers them: this is the number of the one being written. */
static unsigned long submission = 1;

/* buffers of the guest's that commands name (a decoded texture's texels): the
host reads them when it runs the stream - on its render thread, after the
stream is handed over (host_dk.c) - so each is freed once the GPU has
finished its submission, which the host ran before */
#define PENDING_FREE_LIMIT 256
#define PENDING_FREE_BYTES (8 * 1024 * 1024)
/* the buffers of submissions handed over and not yet finished */
#define HANDED_FREE_LIMIT 2048

static void *pending_frees[PENDING_FREE_LIMIT];
static unsigned long pending_free_count;
static unsigned long pending_free_bytes;

static struct
{
	void *buffer;
	unsigned long submission;
} handed_frees[HANDED_FREE_LIMIT];
static unsigned long handed_free_first, handed_free_count;

/* the handed-over buffers whose submissions the GPU has finished, freed;
with wait, all of them, waiting as long as it takes */
static void handed_frees_release(BOOL wait)
{
	while (handed_free_count)
	{
		unsigned long index = handed_free_first;

		if (host_dk_retired() < handed_frees[index].submission)
		{
			if (!wait)
				return;
			usleep(200);
			continue;
		}
		free(handed_frees[index].buffer);
		handed_free_first = (handed_free_first + 1) % HANDED_FREE_LIMIT;
		handed_free_count--;
	}
}

static void stream_flush(void)
{
	if (!stream_used)
		return;
	host_dk_submit((unsigned int)(uintptr_t)stream, (unsigned int)stream_used);
	stream_used = 0;
	/* (this submission's buffers kept until the GPU has finished it) */
	while (pending_free_count)
	{
		if (handed_free_count == HANDED_FREE_LIMIT)
			handed_frees_release(TRUE);
		handed_frees[(handed_free_first + handed_free_count) % HANDED_FREE_LIMIT].buffer =
			pending_frees[--pending_free_count];
		handed_frees[(handed_free_first + handed_free_count) % HANDED_FREE_LIMIT].submission = submission;
		handed_free_count++;
	}
	pending_free_bytes = 0;
	submission++;
	handed_frees_release(FALSE);
}

/* room for a command of size bytes, its header filled in */
static void *stream_command(uint32_t type, unsigned long size)
{
	struct dk_command_header *header;

	size = (size + 3) & ~3UL;
	if (stream_used + size > STREAM_SIZE)
		stream_flush();
	header = (struct dk_command_header *)((unsigned char *)stream + stream_used);
	header->type = type;
	header->size = (uint32_t)size;
	stream_used += size;
	return header;
}

void *dk_stream_command(uint32_t type, unsigned long size)
{
	return stream_command(type, size);
}

void dk_stream_free_after_handover(void *buffer, unsigned long size)
{
	pending_frees[pending_free_count++] = buffer;
	pending_free_bytes += size;
	if (pending_free_count == PENDING_FREE_LIMIT || pending_free_bytes >= PENDING_FREE_BYTES)
		stream_flush();
}

static void color_to_vec4(D3DCOLOR color, float *out)
{
	out[0] = ((color >> 16) & 0xff) / 255.0f;
	out[1] = ((color >> 8) & 0xff) / 255.0f;
	out[2] = (color & 0xff) / 255.0f;
	out[3] = ((color >> 24) & 0xff) / 255.0f;
}

/* ---------- vertical blank emulation */

#define VERTICAL_BLANK_NANOSECONDS (1000000000L / 60)

static pthread_mutex_t vertical_blank_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t vertical_blank_condition = PTHREAD_COND_INITIALIZER;
static D3DCALLBACK vertical_blank_callback;
static unsigned long vertical_blank_count;
static volatile unsigned int flip_count;
static unsigned long pending_flips;
static BOOL vertical_blank_thread_started = FALSE;

static void *vertical_blank_thread(void *unused)
{
	struct timespec next;

	(void)unused;
	clock_gettime(CLOCK_MONOTONIC, &next);
	for (;;)
	{
		D3DCALLBACK callback;

		next.tv_nsec += VERTICAL_BLANK_NANOSECONDS;
		if (next.tv_nsec >= 1000000000L)
		{
			next.tv_nsec -= 1000000000L;
			next.tv_sec++;
		}
		clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, NULL);

		pthread_mutex_lock(&vertical_blank_lock);
		vertical_blank_count++;
		/* a presented frame becomes visible at the next vertical blank */
		if (pending_flips)
		{
			pending_flips--;
			flip_count++;
		}
		callback = vertical_blank_callback;
		pthread_cond_broadcast(&vertical_blank_condition);
		pthread_mutex_unlock(&vertical_blank_lock);

		if (callback)
			callback(0);
	}
	return NULL;
}

static void vertical_blank_start(void)
{
	pthread_mutex_lock(&vertical_blank_lock);
	if (!vertical_blank_thread_started)
	{
		pthread_t thread;

		if (pthread_create(&thread, NULL, vertical_blank_thread, NULL) == 0)
		{
			pthread_detach(thread);
			vertical_blank_thread_started = TRUE;
		}
		else
		{
			platform_log("cannot start the vertical blank thread");
		}
	}
	pthread_mutex_unlock(&vertical_blank_lock);
}

/* replaces main/d3d_intimacy.cpp, which reads the counter out of the Xbox
Direct3D runtime's private device structure */
volatile unsigned int *d3d_find_flipcount(void)
{
	return &flip_count;
}

void WINAPI D3DDevice_SetVerticalBlankCallback(D3DCALLBACK callback)
{
	pthread_mutex_lock(&vertical_blank_lock);
	vertical_blank_callback = callback;
	pthread_mutex_unlock(&vertical_blank_lock);
	vertical_blank_start();
}

static void front_buffer_take(void);
static void targets_used(void);

void WINAPI D3DDevice_BlockUntilVerticalBlank(void)
{
	unsigned long count;

	/* (the loading screen's last call: its frame, for the next frame's
	trails) */
	front_buffer_take();

	vertical_blank_start();
	pthread_mutex_lock(&vertical_blank_lock);
	count = vertical_blank_count;
	while (vertical_blank_count == count)
		pthread_cond_wait(&vertical_blank_condition, &vertical_blank_lock);
	pthread_mutex_unlock(&vertical_blank_lock);
}
/* ---------- render targets

The textures and render targets the OpenGL renderer keeps are the host
backend's to keep here. Until it does, there are none. */

static void surface_dimensions(const D3DSurface *surface, unsigned long *width, unsigned long *height, BOOL *depth)
{
	struct xgpu_texture_description description;
	DWORD format;

	xgpu_texture_describe(surface->Format, surface->Size, &description);
	*width = description.width;
	*height = description.height;
	format = description.format;
	*depth = format == D3DFMT_D24S8 || format == D3DFMT_F24S8 || format == D3DFMT_D16 || format == D3DFMT_F16 ||
		format == D3DFMT_LIN_D24S8 || format == D3DFMT_LIN_F24S8 || format == D3DFMT_LIN_D16 || format == D3DFMT_LIN_F16;
}

/* the host's pixels to a game pixel in the targets bound (targets_bind):
the screen's scale for the screen's targets, else 1. Viewports, clears and
visibility counts are scaled by it */
static float target_scale[2] = { 1.0f, 1.0f };

/* a surface's scale: the screen's, if it is the screen's size */
static void target_scale_of(unsigned long width, unsigned long height, float scale[2])
{
	scale[0] = scale[1] = 1.0f;
	if (width == (unsigned long)halo_screen_width() && height == SCREEN_HEIGHT)
	{
		scale[0] = screen_scale[0];
		scale[1] = screen_scale[1];
	}
}

/* a surface as the host knows it; depth_only: NONE unless it is a depth
format (a depth buffer that is not one is left unbound, as d3d8_gl.c does) */
static void surface_describe(const D3DSurface *surface, BOOL depth_only, struct dk_surface *out, float scale[2])
{
	unsigned long width, height;
	BOOL depth;

	memset(out, 0, sizeof(*out));
	scale[0] = scale[1] = 1.0f;
	if (!surface || !surface->Data)
		return;
	surface_dimensions(surface, &width, &height, &depth);
	if (depth_only && !depth)
		return;
	target_scale_of(width, height, scale);
	out->data = surface->Data;
	out->width = (uint32_t)width;
	out->height = (uint32_t)height;
	out->kind = depth ? DK_SURFACE_DEPTH : DK_SURFACE_COLOR;
	out->pixel_width = (uint32_t)(width * scale[0] + 0.5f);
	out->pixel_height = (uint32_t)(height * scale[1] + 0.5f);
}

/* the color surfaces drawn into, by their data, and the size each was last
drawn at: a texture whose data is one samples the host's target
(render-to-texture), as d3d8_gl.c's xgpu_render_target_find finds them */
#define RENDERED_LIMIT 128

static struct rendered
{
	DWORD data;
	unsigned long width, height;
} rendered[RENDERED_LIMIT];
static unsigned long rendered_count;

/* whether two copies of a state are the same, compared a word at a time:
musl's memcmp goes a byte at a time, and every draw compares several
hundred bytes of state against what the host was told (7% of the game
thread in a match's profile). The structs compared are words throughout. */
static int words_equal(const void *a, const void *b, size_t size)
{
	const uint32_t *x = a, *y = b;
	size_t index;

	for (index = 0; index < size / 4; index++)
	{
		if (x[index] != y[index])
			return 0;
	}
	return size % 4 ? !memcmp((const char *)a + size - size % 4, (const char *)b + size - size % 4, size % 4) : 1;
}

static void rendered_note(const struct dk_surface *surface)
{
	unsigned long index;

	if (surface->kind != DK_SURFACE_COLOR)
		return;
	for (index = 0; index < rendered_count && rendered[index].data != surface->data; index++)
		;
	if (index == rendered_count)
	{
		if (rendered_count == RENDERED_LIMIT)
			return;
		rendered_count++;
	}
	rendered[index].data = surface->data;
	rendered[index].width = surface->width;
	rendered[index].height = surface->height;
}

static const struct rendered *rendered_find(DWORD data)
{
	unsigned long index;

	for (index = 0; index < rendered_count; index++)
	{
		if (rendered[index].data == data)
			return &rendered[index];
	}
	return NULL;
}

/* the mip composites the host has been told of (DK_COMMAND_COMPOSITE) */
#define COMPOSITE_LIMIT 32

static struct dk_command_composite composites_told[COMPOSITE_LIMIT];
static unsigned long composites_told_count;

/* tells the host of a texture rendered a level at a time, unless it knows
it as it is */
static void composite_tell(const struct xgpu_texture_description *description, DWORD data)
{
	struct dk_command_composite composite;
	unsigned long index, level;

	memset(&composite, 0, sizeof(composite));
	composite.data = data;
	composite.width = (uint32_t)description->width;
	composite.height = (uint32_t)description->height;
	composite.levels = (uint32_t)description->levels;
	for (level = 0; level < description->levels; level++)
		composite.level_data[level] = (uint32_t)(data + xgpu_texture_level_offset(description, level));
	for (index = 0; index < composites_told_count; index++)
	{
		if (!memcmp(composites_told[index].level_data, composite.level_data, sizeof(composite.level_data)) &&
			composites_told[index].data == data && composites_told[index].width == composite.width &&
			composites_told[index].height == composite.height && composites_told[index].levels == composite.levels)
			return;
	}
	{
		struct dk_command_composite *command = stream_command(DK_COMMAND_COMPOSITE, sizeof(*command));

		memcpy((char *)command + sizeof(command->header), (char *)&composite + sizeof(composite.header),
			sizeof(composite) - sizeof(composite.header));
	}
	if (composites_told_count < COMPOSITE_LIMIT)
		composites_told[composites_told_count++] = composite;
}

/* the targets the host has bound, as last told it; cleared when a frame
starts there anew */
static struct dk_command_targets targets_told;
static BOOL targets_known;

/* tells the host the current targets, if they are not what it has; FALSE if
there is nothing to draw into. has_depth (if not NULL) says whether there is
a depth buffer. */
static BOOL targets_bind(BOOL *has_depth)
{
	struct dk_command_targets targets;
	float color_scale[2], depth_scale[2];

	memset(&targets, 0, sizeof(targets));
	surface_describe(device.render_target, FALSE, &targets.color, color_scale);
	surface_describe(device.depth_stencil, TRUE, &targets.depth, depth_scale);
	if (targets.color.kind == DK_SURFACE_NONE && targets.depth.kind == DK_SURFACE_NONE)
		return FALSE;
	target_scale[0] = targets.color.kind != DK_SURFACE_NONE ? color_scale[0] : depth_scale[0];
	target_scale[1] = targets.color.kind != DK_SURFACE_NONE ? color_scale[1] : depth_scale[1];
	if (has_depth)
		*has_depth = targets.depth.kind != DK_SURFACE_NONE;
	if (!targets_known || !words_equal(&targets.color, &targets_told.color, sizeof(targets.color)) ||
		!words_equal(&targets.depth, &targets_told.depth, sizeof(targets.depth)))
	{
		struct dk_command_targets *command = stream_command(DK_COMMAND_TARGETS, sizeof(*command));

		command->color = targets.color;
		command->depth = targets.depth;
		rendered_note(&targets.color);
		targets_told = targets;
		targets_known = TRUE;
	}
	return TRUE;
}

struct xgpu_render_target *xgpu_render_target_find(unsigned long data)
{
	(void)data;
	return NULL;
}

/* xbox_textures.c, hud_hires.c and text_hires.c call this after their own GL
calls, which under deko3d have no context and do nothing */
void xgpu_gl_state_invalidate(void)
{
}

/* (the dedicated server's question, whose renderer may be none: this one is
always up, and those callers' GL does nothing here, as before they asked) */
int xgpu_gl_ready(void)
{
	return 1;
}

/* ---------- device creation */

Direct3D *WINAPI Direct3DCreate8(UINT sdk_version)
{
	(void)sdk_version;
	return (Direct3D *)1;
}

void WINAPI Direct3D_SetPushBufferSize(DWORD push_buffer_size, DWORD segment_count)
{
	(void)push_buffer_size;
	(void)segment_count;
}

/* each vertex constant register's serial is the value constants_serial took
when the register last changed; a program's registers are current up to
the serial it recorded when it last uploaded them. The serials are 64-bit,
as in d3d8_gl.c: a skinned model changes up to 132 registers a draw, and
32 bits wrapped, after which a draw found none of its registers changed
and kept what it last uploaded (the other split-screen window's view) */
static unsigned long long constant_serials[XGPU_VERTEX_CONSTANT_COUNT];
static unsigned long long constants_serial;
/* set when a window's viewport changes: the next draw sends every constant,
so one split-screen window cannot draw its map with the other's view */
static BOOL constants_known;
/* the register each of the latest serials changed, so a program that is
only a little behind finds its changed registers without a full scan */
#define CONSTANT_LOG_SIZE 1024
static unsigned char constant_log[CONSTANT_LOG_SIZE];

static void constants_store(unsigned long first, const void *data, unsigned long count)
{
	const float (*values)[4] = data;
	unsigned long index;

	for (index = 0; index < count; index++)
	{
		if (!words_equal(device.constants[first + index], values[index], sizeof(device.constants[0])))
		{
			memcpy(device.constants[first + index], values[index], sizeof(device.constants[0]));
			constant_serials[first + index] = ++constants_serial;
			constant_log[constants_serial % CONSTANT_LOG_SIZE] = (unsigned char)(first + index);
		}
	}
}

static void viewport_update_constants(void)
{
	/* Direct3D's reserved constants c[-38] and c[-37] map clip space to
	the screen; zscale is the depth buffer's range */
	float zscale = 16777215.0f;
	unsigned long width, height;
	BOOL depth;

	if (device.depth_stencil)
	{
		struct xgpu_texture_description description;

		xgpu_texture_describe(device.depth_stencil->Format, device.depth_stencil->Size, &description);
		if (description.format == D3DFMT_D16 || description.format == D3DFMT_LIN_D16 ||
			description.format == D3DFMT_F16 || description.format == D3DFMT_LIN_F16)
		{
			zscale = 65535.0f;
		}
	}
	(void)width; (void)height; (void)depth;
	device.viewport_scale[0] = device.viewport.Width * 0.5f;
	device.viewport_scale[1] = -(float)device.viewport.Height * 0.5f;
	device.viewport_scale[2] = zscale * (device.viewport.MaxZ - device.viewport.MinZ);
	device.viewport_scale[3] = 0.0f;
	device.viewport_offset[0] = device.viewport.X + device.viewport.Width * 0.5f;
	device.viewport_offset[1] = device.viewport.Y + device.viewport.Height * 0.5f;
	device.viewport_offset[2] = zscale * device.viewport.MinZ;
	device.viewport_offset[3] = 0.0f;
	if (!(device.shader_constant_mode & D3DSCM_NORESERVEDCONSTANTS))
	{
		constants_store(XGPU_VERTEX_CONSTANT_BIAS - 38, device.viewport_scale, 1);
		constants_store(XGPU_VERTEX_CONSTANT_BIAS - 37, device.viewport_offset, 1);
	}
	/* The view (c[-96]) is set with this viewport, once per split-screen
	window. Sending every constant on the next draw keeps the map, which is
	drawn after the objects and is the world itself, from keeping the other
	window's view when the two cameras do not agree. */
	constants_known = FALSE;
}
HRESULT WINAPI Direct3D_CreateDevice(UINT adapter, D3DDEVTYPE device_type, void *unused, DWORD behavior_flags,
	D3DPRESENT_PARAMETERS *presentation_parameters, D3DDevice **returned_device)
{
	unsigned long width, height;
	int index;

	(void)adapter;
	(void)device_type;
	(void)unused;
	(void)behavior_flags;
	if (!device.created)
	{
		memset(&device, 0, sizeof(device));
		if (presentation_parameters)
			device.presentation = *presentation_parameters;
		width = device.presentation.BackBufferWidth ? device.presentation.BackBufferWidth : 640;
		height = device.presentation.BackBufferHeight ? device.presentation.BackBufferHeight : 480;
		d3d8_surface_initialize(&device.back_buffer, D3DFMT_LIN_A8R8G8B8, width, height);
		d3d8_surface_initialize(&device.depth_buffer, D3DFMT_LIN_D24S8, width, height);
		device.render_target = &device.back_buffer;
		device.depth_stencil = &device.depth_buffer;
		for (index = 0; index < D3DTS_MAX; index++)
		{
			device.transforms[index]._11 = 1.0f;
			device.transforms[index]._22 = 1.0f;
			device.transforms[index]._33 = 1.0f;
			device.transforms[index]._44 = 1.0f;
		}
		device.viewport.Width = width;
		device.viewport.Height = height;
		device.viewport.MaxZ = 1.0f;
		device.next_vertex_shader_id = 1;
		D3D__RenderState[D3DRS_ZENABLE] = TRUE;
		D3D__RenderState[D3DRS_ZWRITEENABLE] = TRUE;
		D3D__RenderState[D3DRS_ZFUNC] = D3DCMP_LESSEQUAL;
		D3D__RenderState[D3DRS_COLORWRITEENABLE] = D3DCOLORWRITEENABLE_ALL;
		D3D__RenderState[D3DRS_SRCBLEND] = D3DBLEND_ONE;
		D3D__RenderState[D3DRS_DESTBLEND] = D3DBLEND_ZERO;
		D3D__RenderState[D3DRS_BLENDOP] = D3DBLENDOP_ADD;
		D3D__RenderState[D3DRS_CULLMODE] = D3DCULL_CCW;
		D3D__RenderState[D3DRS_FRONTFACE] = D3DFRONT_CW;
		D3D__RenderState[D3DRS_FILLMODE] = D3DFILL_SOLID;
		D3D__RenderState[D3DRS_ALPHAFUNC] = D3DCMP_ALWAYS;
		D3D__RenderState[D3DRS_STENCILFUNC] = D3DCMP_ALWAYS;
		D3D__RenderState[D3DRS_STENCILMASK] = 0xff;
		D3D__RenderState[D3DRS_STENCILWRITEMASK] = 0xff;
		D3D__RenderState[D3DRS_STENCILFAIL] = D3DSTENCILOP_KEEP;
		D3D__RenderState[D3DRS_STENCILZFAIL] = D3DSTENCILOP_KEEP;
		D3D__RenderState[D3DRS_STENCILPASS] = D3DSTENCILOP_KEEP;
		for (index = 0; index < D3DTSS_MAXSTAGES; index++)
		{
			D3D__TextureState[index][D3DTSS_ADDRESSU] = D3DTADDRESS_WRAP;
			D3D__TextureState[index][D3DTSS_ADDRESSV] = D3DTADDRESS_WRAP;
			D3D__TextureState[index][D3DTSS_ADDRESSW] = D3DTADDRESS_WRAP;
			D3D__TextureState[index][D3DTSS_MAGFILTER] = D3DTEXF_POINT;
			D3D__TextureState[index][D3DTSS_MINFILTER] = D3DTEXF_POINT;
			D3D__TextureState[index][D3DTSS_MAXANISOTROPY] = 1;
		}
		viewport_update_constants();
		/* what tells the texture cache a texture has been rewritten
		(xbox_textures_dk.c) */
		memory_watch_initialize();

		/* The window is the host's stand-in (port/switch/host/host.h): the
		platform layer's event loop runs only while it has one, and its swap
		holds frames to the display's rate until deko3d presents them */
		if (!platform_video_initialize(width, height))
			platform_log("Direct3D: no window; the game's events will not be read");
		platform_log("Direct3D: the deko3d renderer");
		device.created = TRUE;
	}
	*returned_device = device_pointer();
	return S_OK;
}

/* ---------- the menus' pointer */

int halo_ui_pointer_update(int menus_active, struct halo_ui_pointer *pointer)
{
	(void)pointer;
	platform_menus_set_active(menus_active != 0);
	return 0;
}

/* menu_files.c says when it deletes a menu picture's GL texture, for the
OpenGL renderer's bindings (d3d8_gl.c); this renderer keeps images of its
own and has none of those */
void xgpu_gl_texture_deleted(unsigned int texture)
{
	(void)texture;
}

/* the scoreboard's: none, as on Android's OpenGL ES (d3d8_gl.c); a vote to
kick starts from the console (votekick) */
int halo_scoreboard_pointer_update(int offered, struct halo_ui_pointer *pointer)
{
	(void)offered;
	(void)pointer;
	return -1;
}

/* takes up the display's shape and resolution, or the window's, if they
have changed; between frames, since the game's layout and the targets must
agree for a whole frame. Returns the width the game draws. */
/* the host's (host_sdl2.c): the CPU's rate in MHz, 0 for the console's own */
void host_clock_cpu(unsigned int mhz);

/* performance.cpu_clock for the host, when it is first read and when it
changes: "1785", "1020" or "stock" */
static void cpu_clock_update(void)
{
	static unsigned long changes = (unsigned long)-1;
	const char *clock;

	if (changes == config_changes())
		return;
	changes = config_changes();
	clock = config_string("performance.cpu_clock");
	host_clock_cpu(!strcmp(clock, "stock") ? 0 : !strcmp(clock, "1020") ? 1020 : 1785);
}

long halo_screen_commit(void)
{
	long width;
	float scale[2];

	cpu_clock_update();

	if (!screen_width)
		return halo_screen_width();
	screen_mode_choose(&width, scale);
	if (width != screen_width || scale[0] != screen_scale[0] || scale[1] != screen_scale[1])
	{
		platform_log("screen: %ldx%d drawn at %.0fx%.0f", width, SCREEN_HEIGHT,
			width * scale[0], SCREEN_HEIGHT * scale[1]);
		screen_width = width;
		screen_scale[0] = scale[0];
		screen_scale[1] = scale[1];
#ifndef HALO_ANDROID
		if (device.created)
		{
			device.presentation.BackBufferWidth = (UINT)width;
			d3d8_surface_resize(&device.back_buffer, D3DFMT_LIN_A8R8G8B8, (unsigned long)width, SCREEN_HEIGHT);
			d3d8_surface_resize(&device.depth_buffer, D3DFMT_LIN_D24S8, (unsigned long)width, SCREEN_HEIGHT);
		}
#endif
	}
	return screen_width;
}
ULONG WINAPI D3DDevice_Release(void)
{
	return 1;
}

void WINAPI D3DDevice_GetDeviceCaps(D3DCAPS8 *caps)
{
	memset(caps, 0, sizeof(*caps));
	caps->DeviceType = D3DDEVTYPE_HAL;
	caps->MaxTextureWidth = 4096;
	caps->MaxTextureHeight = 4096;
	caps->MaxVolumeExtent = 512;
	caps->MaxTextureRepeat = 8192;
	caps->MaxTextureAspectRatio = 4096;
	caps->MaxAnisotropy = 4;
	caps->MaxTextureBlendStages = 4;
	caps->MaxSimultaneousTextures = 4;
	caps->MaxActiveLights = 8;
	caps->MaxVertexBlendMatrices = 4;
	caps->MaxPointSize = 64.0f;
	caps->MaxPrimitiveCount = 0xfffff;
	caps->MaxVertexIndex = 0xffff;
	caps->MaxStreams = 16;
	caps->MaxStreamStride = 255;
	caps->VertexShaderVersion = D3DVS_VERSION(1, 1);
	caps->MaxVertexShaderConst = 192;
	caps->PixelShaderVersion = D3DPS_VERSION(1, 1);
	caps->MaxPixelShaderValue = 1.0f;
}

/* ---------- the front buffer

The Xbox's front buffer is the frame on the screen, which the loading screen
reads back (GetBackBuffer with -1, progress_bar.c) and draws again blurred,
a little larger and fainter, under each new frame: the trails of its light.
As in d3d8_gl.c, it is a render target of the menus' 640x480, which takes the
640 columns in the middle of the frame (where the loading screen draws, as
the menus do on a wide screen) once the loading screen has drawn it, while
the screen asks for it: at its BlockUntilVerticalBlank, its last call. The
host copies it (DK_COMMAND_BLIT); a texture at its data samples the host's
image, as any render target's does (rendered_find). */

enum
{
	FRONT_BUFFER_WIDTH = 640,
	FRONT_BUFFER_HEIGHT = 480,
};

/* the front buffer, made the first time it is asked for, and black when the
frame before did not take its picture (a loading screen's first frame) */
static BOOL front_buffer_ready(void)
{
	/* (its memory could not be had: not asked for again each frame) */
	static BOOL failed;

	if (failed)
		return FALSE;
	if (!device.front_buffer.Data)
	{
		d3d8_surface_initialize(&device.front_buffer, D3DFMT_LIN_A8R8G8B8, FRONT_BUFFER_WIDTH, FRONT_BUFFER_HEIGHT);
		if (!device.front_buffer.Data)
		{
			failed = TRUE;
			return FALSE;
		}
	}
	if (device.front_buffer_taken != device.frame)
	{
		/* cleared as the bound target, which the next draw binds back (the
		targets the host was told differ from the device's again) */
		D3DSurface *color = device.render_target, *depth = device.depth_stencil;
		struct dk_command_clear *command;
		struct dk_surface front;
		float scale[2];

		device.render_target = &device.front_buffer;
		device.depth_stencil = NULL;
		if (targets_bind(NULL))
		{
			surface_describe(&device.front_buffer, FALSE, &front, scale);
			command = stream_command(DK_COMMAND_CLEAR, sizeof(*command) + 4 * sizeof(uint32_t));
			targets_used();
			command->flags = DK_CLEAR_RED | DK_CLEAR_GREEN | DK_CLEAR_BLUE | DK_CLEAR_ALPHA;
			command->color[0] = command->color[1] = command->color[2] = 0.0f;
			command->color[3] = 1.0f;
			command->depth = 1.0f;
			command->stencil = 0;
			command->rectangle_count = 1;
			command->rectangles[0][0] = 0;
			command->rectangles[0][1] = 0;
			command->rectangles[0][2] = front.pixel_width;
			command->rectangles[0][3] = front.pixel_height;
		}
		device.render_target = color;
		device.depth_stencil = depth;
	}
	device.front_buffer_asked = device.frame + 1;
	return TRUE;
}

/* the frame as the loading screen has drawn it, copied into the front
buffer, when this frame asked for it (D3DDevice_BlockUntilVerticalBlank) */
static void front_buffer_take(void)
{
	struct dk_command_blit *command;
	struct dk_surface back_buffer, front;
	float back_scale[2], front_scale[2];
	uint32_t width, left;

	if (!device.front_buffer.Data || device.front_buffer_asked != device.frame + 1)
		return;
	surface_describe(&device.back_buffer, FALSE, &back_buffer, back_scale);
	surface_describe(&device.front_buffer, FALSE, &front, front_scale);
	if (back_buffer.kind != DK_SURFACE_COLOR || front.kind != DK_SURFACE_COLOR)
		return;
	width = (uint32_t)(FRONT_BUFFER_WIDTH * back_scale[0] + 0.5f);
	if (width > back_buffer.pixel_width)
		width = back_buffer.pixel_width;
	left = (back_buffer.pixel_width - width) / 2;
	command = stream_command(DK_COMMAND_BLIT, sizeof(*command));
	command->source = back_buffer;
	command->destination = front;
	command->from[0] = left;
	command->from[1] = 0;
	command->from[2] = width;
	command->from[3] = back_buffer.pixel_height;
	command->to[0] = 0;
	command->to[1] = 0;
	command->to[2] = front.pixel_width;
	command->to[3] = front.pixel_height;
	rendered_note(&front);
	device.front_buffer_taken = device.frame + 1;
}

void WINAPI D3DDevice_GetBackBuffer(INT back_buffer, D3DBACKBUFFER_TYPE type, D3DSurface **result)
{
	(void)type;
	/* -1: the front buffer, the frame shown last, which the loading screen
	blurs into its light's trails (progress_bar.c) */
	if (back_buffer == -1 && front_buffer_ready())
	{
		device.front_buffer.Common++;
		*result = &device.front_buffer;
		return;
	}
	/* like Direct3D, the caller gets a reference it must release */
	device.back_buffer.Common++;
	*result = &device.back_buffer;
}

HRESULT WINAPI D3DDevice_GetDepthStencilSurface(D3DSurface **result)
{
	*result = device.depth_stencil;
	if (!*result)
		return D3DERR_NOTFOUND;
	(*result)->Common++;
	return S_OK;
}

void WINAPI D3DDevice_SetRenderTarget(D3DSurface *render_target, D3DSurface *depth_stencil)
{
	if (render_target)
		device.render_target = render_target;
	device.depth_stencil = depth_stencil;
	/* like Direct3D, reset the viewport to the whole new target */
	if (device.render_target)
	{
		unsigned long width, height;
		BOOL depth;

		surface_dimensions(device.render_target, &width, &height, &depth);
		device.viewport.X = 0;
		device.viewport.Y = 0;
		device.viewport.Width = width;
		device.viewport.Height = height;
		device.viewport.MinZ = 0.0f;
		device.viewport.MaxZ = 1.0f;
	}
	viewport_update_constants();
}

void WINAPI D3DDevice_SetViewport(CONST D3DVIEWPORT8 *viewport)
{
	device.viewport = *viewport;
	viewport_update_constants();
}

void WINAPI D3DDevice_SetTransform(D3DTRANSFORMSTATETYPE state, CONST D3DMATRIX *matrix)
{
	if ((unsigned long)state < D3DTS_MAX)
		device.transforms[state] = *matrix;
}

void WINAPI D3DDevice_GetTransform(D3DTRANSFORMSTATETYPE state, D3DMATRIX *matrix)
{
	if ((unsigned long)state < D3DTS_MAX)
		*matrix = device.transforms[state];
}

void WINAPI D3DDevice_SetFlickerFilter(DWORD filter) { (void)filter; }
void WINAPI D3DDevice_SetSoftDisplayFilter(BOOL enable) { (void)enable; }

void WINAPI D3DDevice_SetShaderConstantMode(D3DSHADERCONSTANTMODE mode)
{
	device.shader_constant_mode = mode;
	viewport_update_constants();
}
/* ---------- GPU synchronisation (DEKO3D.md, phase 3)

Draws read the game's memory where the game wrote it, after the draw, so a
resource is busy until the GPU is past the last submission that read it. As
the Xbox's runtime did, the resource's Lock field records that submission
(resource_used); the game sets it to 0, not busy, whenever it makes a
resource's header - but not for the headers a map file holds, which come
with what the file has there (halo_resource_busy). d3d8_resources.c asks halo_resource_busy and
halo_resource_wait for IsBusy, BlockUntilNotBusy and its locks; their
definitions there are empty for the OpenGL image, whose mirror copied a
draw's data at the draw. */

/* a draw or clear in the submission being written uses the resource; called
after the command is written (writing one can hand the stream over) */
static void resource_used(void *resource)
{
	if (resource)
		((D3DResource *)resource)->Lock = submission;
}

/* the resources the current state reads or writes: the bound render
targets, and for a draw its textures, palettes, vertex streams and index
buffer */
static void targets_used(void)
{
	resource_used(device.render_target);
	resource_used(device.depth_stencil);
}

static void draw_resources_used(BOOL indexed, BOOL streamed)
{
	unsigned long index;
	int stage;

	targets_used();
	for (stage = 0; stage < D3DTSS_MAXSTAGES; stage++)
	{
		if (!((D3D__RenderState[D3DRS_PSTEXTUREMODES] >> (5 * stage)) & 0x1f))
			continue;
		resource_used(device.textures[stage]);
		resource_used(device.palettes[stage]);
	}
	/* (immediate mode reads no buffer) */
	for (index = 0; streamed && device.vertex_shader && index < device.vertex_shader->element_count; index++)
		resource_used(device.streams[device.vertex_shader->elements[index].stream].buffer);
	if (indexed)
		resource_used(device.index_buffer);
}

BOOL halo_resource_busy(D3DResource *resource)
{
	unsigned long last = resource ? resource->Lock : 0;

	if (!last)
		return FALSE;
	/* past the submission being written: not a number this device wrote. A
	resource whose header is in a map file comes with whatever the file has
	there (a level's first lock waited for submission 153684298, 410 being
	written, and never came back), so it is not busy, and is now 0 */
	if (last > submission)
	{
		resource->Lock = 0;
		return FALSE;
	}
	/* used in the submission still being written: handed over now, as the
	Xbox's runtime kicks off its push buffer, or a caller spinning on IsBusy
	would wait for a submission that never comes */
	if (last == submission)
		stream_flush();
	return host_dk_retired() < last;
}

void halo_resource_wait(D3DResource *resource)
{
	unsigned long waits = 0;

	while (halo_resource_busy(resource))
	{
		/* (said once a wait, at two seconds: a lock that never comes back is
		otherwise a silent stop) */
		if (++waits == 4000)
			platform_log("deko3d: a lock of %p has waited two seconds for submission %lu (%u retired, %lu being "
				"written)", (void *)resource, (unsigned long)resource->Lock, host_dk_retired(), submission);
		usleep(500);
	}
}

BOOL WINAPI D3DDevice_IsBusy(void)
{
	stream_flush();
	return host_dk_retired() < submission - 1;
}

void WINAPI D3DDevice_KickPushBuffer(void)
{
	stream_flush();
}

void WINAPI D3DDevice_InsertCallback(D3DCALLBACKTYPE type, D3DCALLBACK callback, DWORD context)
{
	(void)type;
	/* the "GPU" reaches the callback immediately */
	if (callback)
		callback(context);
}

/* ---------- visibility (occlusion) tests: the GPU counts the pixels the
draws between a begin and an end pass, and writes the count to the test's
slot when it gets there (DK_COMMAND_VISIBILITY_*). A result is the slot's
latest count - while the GPU is behind, an earlier test's - as d3d8_gl.c's
query buffer gives: the game asks at the start of the next frame, and
waiting there would stop the CPU until the GPU caught up. A count is in
the host's pixels, made the game's (visibility_area): the screen's targets
can be drawn bigger (display.resolution_limit). */

/* the host's slot of the game's: 1 up, as the host has none 0 (dk_commands.h).
The game's first lens flare of a frame tests in its slot 0: made slot 1, as it
was, the first two flares were given the second's count */
static unsigned long visibility_slot(DWORD index)
{
	return index % (DK_VISIBILITY_SLOTS - 1) + 1;
}

static BOOL visibility_pending[DK_VISIBILITY_SLOTS];
/* the host's pixels to a game pixel where each test drew: its count is
divided by it, the game dividing by its own test's area (lens flares) */
static float visibility_area[DK_VISIBILITY_SLOTS];

void WINAPI D3DDevice_BeginVisibilityTest(void)
{
	if (device.visibility_test_active)
		return;
	device.visibility_test_active = TRUE;
	stream_command(DK_COMMAND_VISIBILITY_BEGIN, sizeof(struct dk_command_header));
}

HRESULT WINAPI D3DDevice_EndVisibilityTest(DWORD index)
{
	struct dk_command_visibility_end *command;

	if (!device.visibility_test_active)
		return S_OK;
	device.visibility_test_active = FALSE;
	index = visibility_slot(index);
	command = stream_command(DK_COMMAND_VISIBILITY_END, sizeof(*command));
	command->index = (uint32_t)index;
	visibility_pending[index] = TRUE;
	visibility_area[index] = target_scale[0] * target_scale[1];
	return S_OK;
}

HRESULT WINAPI D3DDevice_GetVisibilityTestResult(DWORD index, UINT *result, ULONGLONG *time_stamp)
{
	if (time_stamp)
		*time_stamp = 0;
	index = visibility_slot(index);
	if (result)
	{
		unsigned int count = visibility_pending[index] ? host_dk_visibility((unsigned int)index) : 0;

		/* (in the game's pixels) */
		*result = visibility_area[index] > 1.0f ? (UINT)(count / visibility_area[index] + 0.5f) : count;
	}
	return S_OK;
}

/* ---------- render and texture stage state */

void D3DFASTCALL D3DDevice_SetRenderState_Simple(DWORD method, DWORD value)
{
	/* callers also store the value in D3D__RenderState themselves */
	(void)method;
	(void)value;
}

void D3DFASTCALL D3DDevice_SetRenderState_Deferred(D3DRENDERSTATETYPE state, DWORD value)
{
	if ((unsigned long)state < D3DRS_MAX)
		D3D__RenderState[state] = value;
}

void WINAPI D3DDevice_SetRenderState_ZBias(DWORD value);

void WINAPI D3DDevice_SetRenderStateNotInline(D3DRENDERSTATETYPE state, DWORD value)
{
	if (state == D3DRS_ZBIAS)
		D3DDevice_SetRenderState_ZBias(value);
	else if ((unsigned long)state < D3DRS_MAX)
		D3D__RenderState[state] = value;
}

/* As the Xbox's D3D8 does it: a z bias is a polygon offset of -bias depth
units plus -bias/4 times the polygon's depth slope, enabled for every fill
mode. Without the slope term, decals (biased by 8) fight with the surface
under them wherever it is seen at an angle. */
void WINAPI D3DDevice_SetRenderState_ZBias(DWORD value)
{
	float offset = -(float)value;
	float slope = offset * 0.25f;
	DWORD enable = value != 0;

	memcpy(&D3D__RenderState[D3DRS_POLYGONOFFSETZSLOPESCALE], &slope, sizeof(slope));
	memcpy(&D3D__RenderState[D3DRS_POLYGONOFFSETZOFFSET], &offset, sizeof(offset));
	D3D__RenderState[D3DRS_POINTOFFSETENABLE] = enable;
	D3D__RenderState[D3DRS_WIREFRAMEOFFSETENABLE] = enable;
	D3D__RenderState[D3DRS_SOLIDOFFSETENABLE] = enable;
	D3D__RenderState[D3DRS_ZBIAS] = value;
}

#define COMPLEX_RENDER_STATE(name, state) \
	void WINAPI D3DDevice_SetRenderState_##name(DWORD value) { D3D__RenderState[state] = value; }

COMPLEX_RENDER_STATE(PSTextureModes, D3DRS_PSTEXTUREMODES)
COMPLEX_RENDER_STATE(VertexBlend, D3DRS_VERTEXBLEND)
COMPLEX_RENDER_STATE(FogColor, D3DRS_FOGCOLOR)
COMPLEX_RENDER_STATE(FillMode, D3DRS_FILLMODE)
COMPLEX_RENDER_STATE(BackFillMode, D3DRS_BACKFILLMODE)
COMPLEX_RENDER_STATE(TwoSidedLighting, D3DRS_TWOSIDEDLIGHTING)
COMPLEX_RENDER_STATE(NormalizeNormals, D3DRS_NORMALIZENORMALS)
COMPLEX_RENDER_STATE(ZEnable, D3DRS_ZENABLE)
COMPLEX_RENDER_STATE(StencilEnable, D3DRS_STENCILENABLE)
COMPLEX_RENDER_STATE(StencilFail, D3DRS_STENCILFAIL)
COMPLEX_RENDER_STATE(FrontFace, D3DRS_FRONTFACE)
COMPLEX_RENDER_STATE(CullMode, D3DRS_CULLMODE)
COMPLEX_RENDER_STATE(TextureFactor, D3DRS_TEXTUREFACTOR)
COMPLEX_RENDER_STATE(LogicOp, D3DRS_LOGICOP)
COMPLEX_RENDER_STATE(EdgeAntiAlias, D3DRS_EDGEANTIALIAS)
COMPLEX_RENDER_STATE(MultiSampleAntiAlias, D3DRS_MULTISAMPLEANTIALIAS)
COMPLEX_RENDER_STATE(MultiSampleMask, D3DRS_MULTISAMPLEMASK)
COMPLEX_RENDER_STATE(MultiSampleType, D3DRS_MULTISAMPLETYPE)
COMPLEX_RENDER_STATE(ShadowFunc, D3DRS_SHADOWFUNC)
COMPLEX_RENDER_STATE(LineWidth, D3DRS_LINEWIDTH)
COMPLEX_RENDER_STATE(Dxt1NoiseEnable, D3DRS_DXT1NOISEENABLE)
COMPLEX_RENDER_STATE(YuvEnable, D3DRS_YUVENABLE)
COMPLEX_RENDER_STATE(OcclusionCullEnable, D3DRS_OCCLUSIONCULLENABLE)
COMPLEX_RENDER_STATE(StencilCullEnable, D3DRS_STENCILCULLENABLE)
COMPLEX_RENDER_STATE(RopZCmpAlwaysRead, D3DRS_ROPZCMPALWAYSREAD)
COMPLEX_RENDER_STATE(RopZRead, D3DRS_ROPZREAD)
COMPLEX_RENDER_STATE(DoNotCullUncompressed, D3DRS_DONOTCULLUNCOMPRESSED)

void D3DFASTCALL D3DDevice_SetTextureState_Deferred(DWORD stage, D3DTEXTURESTAGESTATETYPE type, DWORD value)
{
	if (stage < D3DTSS_MAXSTAGES && (unsigned long)type < D3DTSS_MAX)
		D3D__TextureState[stage][type] = value;
}

void WINAPI D3DDevice_SetTextureState_TexCoordIndex(DWORD stage, DWORD value)
{
	if (stage < D3DTSS_MAXSTAGES)
		D3D__TextureState[stage][D3DTSS_TEXCOORDINDEX] = value;
}

void WINAPI D3DDevice_SetTextureState_BorderColor(DWORD stage, DWORD value)
{
	if (stage < D3DTSS_MAXSTAGES)
		D3D__TextureState[stage][D3DTSS_BORDERCOLOR] = value;
}

void WINAPI D3DDevice_SetTextureState_ColorKeyColor(DWORD stage, DWORD value)
{
	if (stage < D3DTSS_MAXSTAGES)
		D3D__TextureState[stage][D3DTSS_COLORKEYCOLOR] = value;
}

void WINAPI D3DDevice_SetTextureState_BumpEnv(DWORD stage, D3DTEXTURESTAGESTATETYPE type, DWORD value)
{
	if (stage < D3DTSS_MAXSTAGES && (unsigned long)type < D3DTSS_MAX)
		D3D__TextureState[stage][type] = value;
}

void WINAPI D3DDevice_SetTexture(DWORD stage, D3DBaseTexture *texture)
{
	if (stage < D3DTSS_MAXSTAGES)
		device.textures[stage] = texture;
}

void WINAPI D3DDevice_SetPalette(DWORD stage, D3DPalette *palette)
{
	if (stage < D3DTSS_MAXSTAGES)
		device.palettes[stage] = palette;
}

void WINAPI D3DDevice_SetPixelShaderProgram(D3DPIXELSHADERDEF *definition)
{
	/* the definition's members are the pixel shader render states */
	if (!definition)
		return;
	memcpy(&D3D__RenderState[D3DRS_PSALPHAINPUTS0], definition->PSAlphaInputs, sizeof(definition->PSAlphaInputs));
	D3D__RenderState[D3DRS_PSFINALCOMBINERINPUTSABCD] = definition->PSFinalCombinerInputsABCD;
	D3D__RenderState[D3DRS_PSFINALCOMBINERINPUTSEFG] = definition->PSFinalCombinerInputsEFG;
	memcpy(&D3D__RenderState[D3DRS_PSCONSTANT0_0], definition->PSConstant0, sizeof(definition->PSConstant0));
	memcpy(&D3D__RenderState[D3DRS_PSCONSTANT1_0], definition->PSConstant1, sizeof(definition->PSConstant1));
	memcpy(&D3D__RenderState[D3DRS_PSALPHAOUTPUTS0], definition->PSAlphaOutputs, sizeof(definition->PSAlphaOutputs));
	memcpy(&D3D__RenderState[D3DRS_PSRGBINPUTS0], definition->PSRGBInputs, sizeof(definition->PSRGBInputs));
	D3D__RenderState[D3DRS_PSCOMPAREMODE] = definition->PSCompareMode;
	D3D__RenderState[D3DRS_PSFINALCOMBINERCONSTANT0] = definition->PSFinalCombinerConstant0;
	D3D__RenderState[D3DRS_PSFINALCOMBINERCONSTANT1] = definition->PSFinalCombinerConstant1;
	memcpy(&D3D__RenderState[D3DRS_PSRGBOUTPUTS0], definition->PSRGBOutputs, sizeof(definition->PSRGBOutputs));
	D3D__RenderState[D3DRS_PSCOMBINERCOUNT] = definition->PSCombinerCount;
	D3D__RenderState[D3DRS_PSTEXTUREMODES] = definition->PSTextureModes;
	D3D__RenderState[D3DRS_PSDOTMAPPING] = definition->PSDotMapping;
	D3D__RenderState[D3DRS_PSINPUTTEXTURE] = definition->PSInputTexture;
}
/* ---------- vertex shaders */

static unsigned long vertex_type_bytes(unsigned long type)
{
	switch (type)
	{
	case D3DVSDT_FLOAT1: return 4;
	case D3DVSDT_FLOAT2: return 8;
	case D3DVSDT_FLOAT3: return 12;
	case D3DVSDT_FLOAT4: return 16;
	case D3DVSDT_D3DCOLOR: return 4;
	case D3DVSDT_SHORT1: return 2;
	case D3DVSDT_SHORT2: return 4;
	case D3DVSDT_SHORT3: return 6;
	case D3DVSDT_SHORT4: return 8;
	case D3DVSDT_NORMSHORT1: return 2;
	case D3DVSDT_NORMSHORT2: return 4;
	case D3DVSDT_NORMSHORT3: return 6;
	case D3DVSDT_NORMSHORT4: return 8;
	case D3DVSDT_NORMPACKED3: return 4;
	case D3DVSDT_PBYTE1: return 1;
	case D3DVSDT_PBYTE2: return 2;
	case D3DVSDT_PBYTE3: return 3;
	case D3DVSDT_PBYTE4: return 4;
	case D3DVSDT_FLOAT2H: return 12;
	default: return 0;
	}
}

static void parse_declaration(struct vertex_shader_object *object, const DWORD *declaration)
{
	unsigned long stream = 0;
	unsigned long offsets[16] = { 0 };

	for (; declaration && *declaration != D3DVSD_END(); declaration++)
	{
		DWORD token = *declaration;
		unsigned long token_type = (token & D3DVSD_TOKENTYPEMASK) >> D3DVSD_TOKENTYPESHIFT;

		switch (token_type)
		{
		case D3DVSD_TOKEN_STREAM:
			stream = token & D3DVSD_STREAMNUMBERMASK;
			break;
		case D3DVSD_TOKEN_STREAMDATA:
			if (token & D3DVSD_DATALOADTYPEMASK)
			{
				/* skip: the count is in dwords, or in bytes with bit 27 */
				unsigned long count = (token & D3DVSD_SKIPCOUNTMASK) >> D3DVSD_SKIPCOUNTSHIFT;

				offsets[stream] += (token & 0x08000000) ? count : count * 4;
			}
			else if (object->element_count < XGPU_VERTEX_ATTRIBUTE_COUNT)
			{
				struct vertex_element *element = &object->elements[object->element_count++];

				element->reg = (unsigned char)(token & D3DVSD_VERTEXREGMASK);
				element->stream = (unsigned char)stream;
				element->type = (unsigned char)((token & D3DVSD_DATATYPEMASK) >> D3DVSD_DATATYPESHIFT);
				element->bytes = (unsigned char)vertex_type_bytes(element->type);
				element->offset = (unsigned short)offsets[stream];
				offsets[stream] += element->bytes;
				if (element->type == D3DVSDT_NORMPACKED3)
					object->packed_mask |= 1UL << element->reg;
			}
			break;
		case D3DVSD_TOKEN_CONSTMEM:
			declaration += ((token & D3DVSD_CONSTCOUNTMASK) >> D3DVSD_CONSTCOUNTSHIFT) * 4;
			break;
		case D3DVSD_TOKEN_EXT:
			declaration += (token & D3DVSD_EXTCOUNTMASK) >> D3DVSD_EXTCOUNTSHIFT;
			break;
		default:
			break;
		}
	}
}
static void vertex_shader_note(struct vertex_shader_object *object);

HRESULT WINAPI D3DDevice_CreateVertexShader(CONST DWORD *declaration, CONST DWORD *function, DWORD *handle, DWORD usage)
{
	struct vertex_shader_object *object = calloc(1, sizeof(*object));

	(void)usage;
	if (!object)
		return E_OUTOFMEMORY;
	object->signature = VERTEX_SHADER_SIGNATURE;
	object->id = device.next_vertex_shader_id++;
	if (function)
	{
		/* header: program type in the low word, instruction count in the high */
		object->instruction_count = function[0] >> 16;
		object->instructions = malloc(object->instruction_count * 4 * sizeof(DWORD));
		memcpy(object->instructions, function + 1, object->instruction_count * 4 * sizeof(DWORD));
		/* the key the shader cache knows this program by, computed once,
	here, while the words are at hand */
		object->program_hash = dk_shader_hash_mix(
			dk_shader_hash_mix(dk_shader_hash_init(), &object->instruction_count, sizeof(object->instruction_count)),
			object->instructions, object->instruction_count * 4 * sizeof(DWORD));
	}
	parse_declaration(object, declaration);
	vertex_shader_note(object);
	/* odd values are FVF codes; programmable shader handles are even */
	*handle = (DWORD)object;
	return S_OK;
}

static struct vertex_shader_object *vertex_shader_from_handle(DWORD handle)
{
	struct vertex_shader_object *object = (struct vertex_shader_object *)handle;

	if (!handle || (handle & 1) || object->signature != VERTEX_SHADER_SIGNATURE)
		return NULL;
	return object;
}

void WINAPI D3DDevice_DeleteVertexShader(DWORD handle)
{
	/* programs stay cached; the object is small */
	(void)handle;
}

void WINAPI D3DDevice_SetVertexShader(DWORD handle)
{
	struct vertex_shader_object *object = vertex_shader_from_handle(handle);

	if (object)
	{
		device.vertex_shader = object;
		device.program_address = 0;
		device.program_slots[0] = object;
		device.fixed_function = FALSE;
	}
	/* 0: the fixed-function pipeline, which only the loading screen uses
	(progress_bar.c). Another handle that is no shader changes nothing, as
	before */
	else if (!handle)
		device.fixed_function = TRUE;
}

void WINAPI D3DDevice_LoadVertexShader(DWORD handle, DWORD address)
{
	if (address < VERTEX_PROGRAM_SLOTS)
		device.program_slots[address] = vertex_shader_from_handle(handle);
}

void WINAPI D3DDevice_SelectVertexShader(DWORD handle, DWORD address)
{
	struct vertex_shader_object *object = vertex_shader_from_handle(handle);

	/* (a program, loaded or packed at the address: never the fixed-function
	pipeline, which SetVertexShader with 0 sets) */
	device.fixed_function = FALSE;
	if (object)
		device.vertex_shader = object;
	if (address < VERTEX_PROGRAM_SLOTS)
		device.program_address = address;
}

void WINAPI D3DDevice_GetVertexShaderSize(DWORD handle, UINT *size)
{
	struct vertex_shader_object *object = vertex_shader_from_handle(handle);

	*size = object ? object->instruction_count : 0;
}

void WINAPI D3DDevice_SetVertexShaderConstant(INT reg, CONST void *constant_data, DWORD constant_count)
{
	long first = reg + XGPU_VERTEX_CONSTANT_BIAS;

	if (first < 0 || first >= XGPU_VERTEX_CONSTANT_COUNT)
		return;
	if (first + (long)constant_count > XGPU_VERTEX_CONSTANT_COUNT)
		constant_count = XGPU_VERTEX_CONSTANT_COUNT - first;
	constants_store((unsigned long)first, constant_data, constant_count);
}

/* the program that runs: the one loaded at the selected address, else the
current shader's own */
static struct vertex_shader_object *current_program(void)
{
	struct vertex_shader_object *program = device.program_slots[device.program_address];

	return program ? program : device.vertex_shader;
}

/* a position in immediate mode by the fixed-function pipeline's transforms
(d3d8_gl.c's fixed_function_transform): Direct3D's row vectors, times
world, view, then projection */
static void fixed_function_transform(float *position)
{
	static const D3DTRANSFORMSTATETYPE order[3] = { D3DTS_WORLD, D3DTS_VIEW, D3DTS_PROJECTION };
	int which, column;

	for (which = 0; which < 3; which++)
	{
		const D3DMATRIX *matrix = &device.transforms[order[which]];
		float in[4];

		memcpy(in, position, sizeof(in));
		for (column = 0; column < 4; column++)
		{
			position[column] = in[0] * matrix->m[0][column] + in[1] * matrix->m[1][column] +
				in[2] * matrix->m[2][column] + in[3] * matrix->m[3][column];
		}
	}
}

/* the game finished loading a map (scenario_load) */
/* d3d8_gl.c's program_record_map_hash: the map's name hashed, so the keys
met on it can be raised in the background compile's queue */
static uint32_t map_hash(const char *name)
{
	uint32_t hash = 2166136261UL;

	for (; *name; name++)
		hash = (hash ^ (unsigned char)*name) * 16777619UL;
	return hash ? hash : 1;
}

/* the loading screen is up for a map (game_precache_new_map) */
void d3d8_gl_map_loading(const char *name)
{
	dk_shader_map_loading(map_hash(name));
}

/* whether it stays up a frame more, for the map's shaders */
int d3d8_gl_map_waiting(void)
{
	return dk_shader_map_waiting();
}

/* how far the shaders' part of it has come, 0 to 1 */
float d3d8_gl_map_progress(void)
{
	return dk_shader_map_progress();
}

void d3d8_gl_map_loaded(const char *name)
{
	dk_shader_map_loaded(map_hash(name));
}

/* ---------- the shader dump (debug.gpu_dump_shaders; DEKO3D.md, phase 4)

The corpus the GLSL generators are proved with is the game's own: the
vertex programs this device holds (the game creates them from its table at
startup), and the pixel shader keys the OpenGL image recorded in
shader_programs.bin (d3d8_gl.c's program records), as GLSL from the deko3d
generators (nv2a_vsh_dk.c, nv2a_psh_dk.c), which UAM then compiles on the PC
and on the console. Written once, a second into the game
(D3DDevice_Present). */

/* the vertex shader objects by their id (d3d8_gl.c's list, kept for its
program records: the records name the programs by id) */
static struct vertex_shader_object **vertex_shaders_by_id;
static unsigned long vertex_shaders_by_id_count;

static void vertex_shader_note(struct vertex_shader_object *object)
{
	if (object->id >= vertex_shaders_by_id_count)
	{
		unsigned long count = object->id + 64;
		struct vertex_shader_object **grown = realloc(vertex_shaders_by_id, count * sizeof(*grown));

		if (!grown)
			return;
		memset(grown + vertex_shaders_by_id_count, 0, (count - vertex_shaders_by_id_count) * sizeof(*grown));
		vertex_shaders_by_id = grown;
		vertex_shaders_by_id_count = count;
	}
	vertex_shaders_by_id[object->id] = object;
}

/* ---------- for dk_shaders.c (the shader cache's guest half): the vertex
programs the game has made, which its keys name by hash and the OpenGL
records name by id. Programs stay cached (DeleteVertexShader keeps them),
so an id keeps naming the same program for the program's life */

unsigned long d3d8_dk_vertex_shader_count(void)
{
	return vertex_shaders_by_id_count;
}

int d3d8_dk_vertex_program_by_id(unsigned long id, uint64_t *program_hash, const uint32_t **instructions,
	unsigned long *instruction_count)
{
	struct vertex_shader_object *object = id < vertex_shaders_by_id_count ? vertex_shaders_by_id[id] : NULL;

	if (!object || !object->instructions)
		return 0;
	if (program_hash)
		*program_hash = object->program_hash;
	if (instructions)
		*instructions = (const uint32_t *)object->instructions;
	if (instruction_count)
		*instruction_count = object->instruction_count;
	return 1;
}


/* d3d8_gl.c's program records, whose keys the dump's pixel shaders are */
#define PROGRAM_RECORD_MAGIC 0x31435350UL /* "PSC1" */
#define PROGRAM_RECORD_PATH "z:\\shader_programs.bin"

struct program_record
{
	DWORD map_hash;
	DWORD vertex_id;
	DWORD variant;
	DWORD packed_mask;
	struct nv2a_pixel_shader_key key;
};

/* d3d8_gl.c's hash, which names the pixel files (size is a multiple of 4) */
static unsigned long hash_words(const void *data, unsigned long size)
{
	const DWORD *words = data;
	unsigned long hash = 2166136261UL;

	for (size /= 4; size; size--)
		hash = (hash ^ *words++) * 16777619UL;
	return hash;
}

/* the records from shader_programs.bin, or as many as memory holds; a file
from a build whose key differs in size is no use (the key struct is shared,
so a record is the same size here - checked anyway) */
static struct program_record *program_records_load(unsigned long *count_out)
{
	char path[512];
	FILE *file;
	DWORD header[2];
	struct program_record *records = NULL;
	unsigned long count = 0, capacity = 0;
	struct program_record record;

	*count_out = 0;
	platform_translate_path(PROGRAM_RECORD_PATH, path, sizeof(path));
	if ((file = fopen(path, "rb")) == NULL)
		return NULL;
	if (fread(header, sizeof(header), 1, file) == 1 && header[0] == PROGRAM_RECORD_MAGIC &&
		header[1] == sizeof(record))
	{
		while (fread(&record, sizeof(record), 1, file) == 1)
		{
			if (count == capacity)
			{
				unsigned long grown_capacity = capacity ? capacity * 2 : 256;
				struct program_record *grown = realloc(records, grown_capacity * sizeof(*grown));

				if (!grown)
					break;
				records = grown;
				capacity = grown_capacity;
			}
			records[count++] = record;
		}
	}
	else
	{
		platform_log("shader dump: %s is not a program records file of this build; its keys are not dumped", path);
	}
	fclose(file);
	*count_out = count;
	return records;
}

static BOOL shaders_dumped;

static void shaders_dump(void)
{
	const char *folder = config_string("debug.gpu_dump_shaders");
	char path[512], line[256];
	FILE *manifest;
	struct program_record *records;
	unsigned long record_count, index;
	struct nv2a_pixel_shader_key *keys = NULL;
	unsigned long key_count = 0, key_capacity = 0, vertex_files = 0, pixel_files = 0;
	unsigned long id;

	if (!*folder)
		return;
	records = program_records_load(&record_count);
	/* (exists already on every run but the first) */
	posix_make_directory(folder);
	snprintf(path, sizeof(path), "%s/manifest.txt", folder);
	manifest = fopen(path, "w");
	if (!manifest)
	{
		platform_log("shader dump: cannot write in %s", folder);
		free(records);
		return;
	}

	/* a vertex file per object and packed mask: mask 0 (the immediate-mode
	variant), and every mask the records show the program drawn with */
	for (id = 1; id < vertex_shaders_by_id_count; id++)
	{
		struct vertex_shader_object *object = vertex_shaders_by_id[id];
		unsigned long masks[16];
		unsigned long mask_count = 0, mask_index;

		if (!object || !object->instructions)
			continue;
		masks[mask_count++] = 0;
		for (index = 0; index < record_count; index++)
		{
			unsigned long mask;

			if (records[index].vertex_id != id)
				continue;
			mask = records[index].packed_mask;
			for (mask_index = 0; mask_index < mask_count; mask_index++)
			{
				if (masks[mask_index] == mask)
					break;
			}
			if (mask_index == mask_count)
			{
				if (mask_count < sizeof(masks) / sizeof(masks[0]))
					masks[mask_count++] = mask;
				else
					platform_log("shader dump: vertex shader %lu has more than %lu packed masks; the rest are not dumped",
						id, (unsigned long)(sizeof(masks) / sizeof(masks[0])));
			}
		}
		for (mask_index = 0; mask_index < mask_count; mask_index++)
		{
			char *source = nv2a_dk_vertex_shader_to_glsl((const uint32_t *)object->instructions,
				object->instruction_count, masks[mask_index]);
			FILE *file;

			snprintf(path, sizeof(path), "%s/vs_%lu_%08lx.vert", folder, id, masks[mask_index]);
			if ((file = fopen(path, "w")) != NULL)
			{
				fputs(source, file);
				fclose(file);
				snprintf(line, sizeof(line), "vs_%lu_%08lx.vert: vertex shader %lu (%lu instructions), packed mask 0x%08lx%s\n",
					id, masks[mask_index], id, object->instruction_count, masks[mask_index],
					masks[mask_index] ? "" : " (the immediate-mode variant)");
				fputs(line, manifest);
				vertex_files++;
			}
			free(source);
		}
	}

	/* a fragment file per distinct key, named by d3d8_gl.c's hash of it */
	for (index = 0; index < record_count; index++)
	{
		struct nv2a_pixel_shader_key *key = &records[index].key;
		unsigned long hash, found;
		char *source;
		FILE *file;

		for (found = 0; found < key_count; found++)
		{
			if (!memcmp(&keys[found], key, sizeof(*key)))
				break;
		}
		if (found < key_count)
			continue;
		if (key_count == key_capacity)
		{
			unsigned long grown_capacity = key_capacity ? key_capacity * 2 : 256;
			struct nv2a_pixel_shader_key *grown = realloc(keys, grown_capacity * sizeof(*grown));

			if (!grown)
				break;
			keys = grown;
			key_capacity = grown_capacity;
		}
		keys[key_count++] = *key;
		hash = hash_words(key, sizeof(*key));
		source = nv2a_dk_pixel_shader_to_glsl(key);
		snprintf(path, sizeof(path), "%s/ps_%08lx.frag", folder, hash);
		if ((file = fopen(path, "w")) != NULL)
		{
			fputs(source, file);
			fclose(file);
			snprintf(line, sizeof(line), "ps_%08lx.frag: the pixel shader key hashing to 0x%08lx, first in the records of map 0x%08lx\n",
				hash, hash, (unsigned long)records[index].map_hash);
			fputs(line, manifest);
			pixel_files++;
		}
		free(source);
	}
	fclose(manifest);
	platform_log("shader dump: %lu vertex and %lu pixel shaders (%lu records) written to %s",
		vertex_files, pixel_files, record_count, folder);
	free(keys);
	free(records);
}

/* ---------- vertex data and drawing */

void WINAPI D3DDevice_SetStreamSource(UINT stream_number, D3DVertexBuffer *stream_data, UINT stride)
{
	if (stream_number >= 16)
		return;
	device.streams[stream_number].data = stream_data ? stream_data->Data : 0;
	device.streams[stream_number].stride = stride;
	device.streams[stream_number].buffer = stream_data;
}

void WINAPI D3DDevice_SetIndices(D3DIndexBuffer *index_data, UINT base_vertex_index)
{
	device.base_vertex_index = base_vertex_index;
	device.index_buffer = index_data;
	/* an index buffer's Data is a window address, not an offset within one:
	from a map file it is the address the window was linked at, and from
	CreateIndexBuffer ordinary memory, which the move leaves alone */
	D3D__IndexData = index_data ? (WORD *)PORT_WINDOW_REBASE(index_data->Data) : NULL;
}
/* ---------- drawing (DEKO3D.md, phase 6)

The device reads Direct3D's state at each draw as d3d8_gl.c's prepare_draw
does and writes it to the command stream (dk_commands.h) - but only what the
host does not already hold: the host keeps the state, the shaders, the
uniforms and the vertex format it was last told, so a draw that changes one of
them writes one command, and a draw that changes nothing writes the draw. */

static struct
{
	unsigned long draws, immediate_draws, skipped_no_program, skipped_no_target, skipped_shader, skipped_size;
} stats;

/* what the host holds */
static struct dk_draw_state state_told;
static BOOL state_known;
static uint32_t shaders_told[2];
static BOOL shaders_known;
static struct dk_vertex_parameters vertex_parameters_told;
static struct dk_pixel_parameters pixel_parameters_told;
static BOOL parameters_known;
static struct dk_vertex_format format_told;
static BOOL format_known;
static struct dk_command_textures textures_told;
static BOOL textures_known;
static unsigned long long constants_told;

/* the state the uniform blocks above come from: when none of it has changed
they need no converting (most draws share it with the draw before) */
#define DRAW_UNIFORM_INPUT_COUNT (4 + 4 + 16 + 1 + 16 + 2 + 4 + 1 + 1 + 3 + 6 * D3DTSS_MAXSTAGES)

static DWORD draw_uniform_inputs[DRAW_UNIFORM_INPUT_COUNT];
static BOOL draw_uniform_inputs_known;

static unsigned long stage_texture_mode(int stage)
{
	return (D3D__RenderState[D3DRS_PSTEXTUREMODES] >> (5 * stage)) & 0x1f;
}

/* the host's pixel edge of a coordinate in the game's pixels of the targets
bound (target_scale: the screen's targets may be drawn bigger) */
static int target_pixel(float coordinate, int axis)
{
	return (int)floorf(coordinate * target_scale[axis] + 0.5f);
}

static uint32_t blend_equation(DWORD operation)
{
	switch (operation)
	{
	case D3DBLENDOP_SUBTRACT: return DK_BLEND_SUBTRACT;
	case D3DBLENDOP_REVSUBTRACT:
	case D3DBLENDOP_REVSUBTRACTSIGNED: return DK_BLEND_REVERSE_SUBTRACT;
	case D3DBLENDOP_MIN: return DK_BLEND_MIN;
	case D3DBLENDOP_MAX: return DK_BLEND_MAX;
	default: return DK_BLEND_ADD;
	}
}

/* d3d8_gl.c's apply_raster_state, as a description of what it would set */
static void draw_state_make(BOOL has_depth, struct dk_draw_state *state)
{
	DWORD *rs = D3D__RenderState;
	DWORD write = rs[D3DRS_COLORWRITEENABLE];
	BOOL depth_test = has_depth && rs[D3DRS_ZENABLE];

	memset(state, 0, sizeof(*state));
	state->viewport[0] = target_pixel((float)device.viewport.X, 0);
	state->viewport[1] = target_pixel((float)device.viewport.Y, 1);
	state->viewport[2] = target_pixel((float)(device.viewport.X + device.viewport.Width), 0) - state->viewport[0];
	state->viewport[3] = target_pixel((float)(device.viewport.Y + device.viewport.Height), 1) - state->viewport[1];
	state->depth_range[0] = device.viewport.MinZ;
	state->depth_range[1] = device.viewport.MaxZ;
	/* the game never issues a scissor rectangle, and the NV2A scissor register
	defaults to the viewport, so fragment clipping follows the viewport: this is
	what keeps a split-screen window's geometry from bleeding across the divider */
	memcpy(state->scissor, state->viewport, sizeof(state->scissor));
	/* (narrowed to the rectangle a draw changes anything in, if one is set:
	halo_draw_scissor) */
	if (draw_scissor_on)
	{
		int x0 = target_pixel(draw_scissor[0], 0), y0 = target_pixel(draw_scissor[1], 1);
		int x1 = target_pixel(draw_scissor[2], 0), y1 = target_pixel(draw_scissor[3], 1);
		int left = state->viewport[0] > x0 ? state->viewport[0] : x0;
		int top = state->viewport[1] > y0 ? state->viewport[1] : y0;
		int right = state->viewport[0] + state->viewport[2] < x1 ? state->viewport[0] + state->viewport[2] : x1;
		int bottom = state->viewport[1] + state->viewport[3] < y1 ? state->viewport[1] + state->viewport[3] : y1;

		state->scissor[0] = left;
		state->scissor[1] = top;
		state->scissor[2] = right > left ? right - left : 0;
		state->scissor[3] = bottom > top ? bottom - top : 0;
	}

	state->depth_test = depth_test;
	if (depth_test)
	{
		state->depth_function = rs[D3DRS_ZFUNC] ? rs[D3DRS_ZFUNC] : D3DCMP_NEVER;
		state->depth_write = rs[D3DRS_ZWRITEENABLE] != 0;
	}

	state->stencil_test = has_depth && rs[D3DRS_STENCILENABLE];
	if (state->stencil_test)
	{
		state->stencil_function = rs[D3DRS_STENCILFUNC] ? rs[D3DRS_STENCILFUNC] : D3DCMP_NEVER;
		state->stencil_reference = rs[D3DRS_STENCILREF];
		state->stencil_mask = rs[D3DRS_STENCILMASK];
		state->stencil_write_mask = rs[D3DRS_STENCILWRITEMASK];
		state->stencil_fail = rs[D3DRS_STENCILFAIL];
		state->stencil_depth_fail = rs[D3DRS_STENCILZFAIL];
		state->stencil_pass = rs[D3DRS_STENCILPASS];
	}

	state->blend = rs[D3DRS_ALPHABLENDENABLE] != 0;
	if (state->blend)
	{
		state->blend_source = rs[D3DRS_SRCBLEND];
		state->blend_destination = rs[D3DRS_DESTBLEND];
		state->blend_equation = blend_equation(rs[D3DRS_BLENDOP]);
		color_to_vec4(rs[D3DRS_BLENDCOLOR], state->blend_color);
	}
	state->color_mask = ((write & D3DCOLORWRITEENABLE_RED) ? 1 : 0) | ((write & D3DCOLORWRITEENABLE_GREEN) ? 2 : 0) |
		((write & D3DCOLORWRITEENABLE_BLUE) ? 4 : 0) | ((write & D3DCOLORWRITEENABLE_ALPHA) ? 8 : 0);

	/* the cull mode names the winding to discard; FRONTFACE names the front
	winding */
	if (rs[D3DRS_CULLMODE] != D3DCULL_NONE)
		state->cull = rs[D3DRS_CULLMODE] == rs[D3DRS_FRONTFACE] ? DK_CULL_FRONT : DK_CULL_BACK;
	state->front_face_ccw = rs[D3DRS_FRONTFACE] == D3DFRONT_CCW;
	state->fill_mode = rs[D3DRS_FILLMODE] == D3DFILL_WIREFRAME ? DK_FILL_LINE :
		rs[D3DRS_FILLMODE] == D3DFILL_POINT ? DK_FILL_POINT : DK_FILL_SOLID;

	/* D3DRS_ZBIAS is expressed in these states (D3DDevice_SetRenderState_ZBias) */
	state->offset_enable = rs[D3DRS_SOLIDOFFSETENABLE] != 0;
	if (state->offset_enable)
	{
		state->offset_slope = dword_to_float(rs[D3DRS_POLYGONOFFSETZSLOPESCALE]);
		state->offset_units = dword_to_float(rs[D3DRS_POLYGONOFFSETZOFFSET]);
	}
}

static void draw_state_send(BOOL has_depth)
{
	struct dk_draw_state state;

	draw_state_make(has_depth, &state);
	if (state_known && words_equal(&state, &state_told, sizeof(state)))
		return;
	{
		struct dk_command_state *command = stream_command(DK_COMMAND_STATE, sizeof(*command));

		command->state = state;
	}
	state_told = state;
	state_known = TRUE;
}

/* the vertex constants the host does not have yet: the registers changed since
the serial it was told, as one run (d3d8_gl.c's prepare_draw does the same
for a program's own registers) */
static void draw_constants_send(void)
{
	unsigned long first = XGPU_VERTEX_CONSTANT_COUNT, last = 0, index;
	struct dk_command_constants *command;

	if (constants_known && constants_told == constants_serial)
		return;
	if (!constants_known)
	{
		first = 0;
		last = XGPU_VERTEX_CONSTANT_COUNT - 1;
	}
	else if (constants_serial - constants_told <= CONSTANT_LOG_SIZE)
	{
		unsigned long long serial;

		for (serial = constants_told + 1; serial <= constants_serial; serial++)
		{
			index = constant_log[serial % CONSTANT_LOG_SIZE];
			if (first > index)
				first = index;
			if (last < index)
				last = index;
		}
	}
	else
	{
		for (index = 0; index < XGPU_VERTEX_CONSTANT_COUNT; index++)
		{
			if (constant_serials[index] > constants_told)
			{
				if (first > index)
					first = index;
				last = index;
			}
		}
	}
	constants_told = constants_serial;
	constants_known = TRUE;
	if (first >= XGPU_VERTEX_CONSTANT_COUNT)
		return;
	command = stream_command(DK_COMMAND_CONSTANTS, sizeof(*command) + (last - first + 1) * 4 * sizeof(float));
	command->first = (uint32_t)first;
	command->count = (uint32_t)(last - first + 1);
	memcpy(command->data, device.constants[first], (last - first + 1) * 4 * sizeof(float));
}

/* the uniforms that are not the vertex constants (d3d8_gl.c's
draw_uniforms), converted when the state they come from has changed, and
written when the host does not hold the result */
static void draw_parameters_send(float texture_scale[4][4])
{
	DWORD inputs[DRAW_UNIFORM_INPUT_COUNT];
	unsigned long count = 0;
	int stage;

	memcpy(&inputs[count], device.viewport_scale, sizeof(device.viewport_scale));
	count += 4;
	memcpy(&inputs[count], device.viewport_offset, sizeof(device.viewport_offset));
	count += 4;
	memcpy(&inputs[count], texture_scale, 16 * sizeof(float));
	count += 16;
	inputs[count++] = D3D__RenderState[D3DRS_POINTSIZE];
	for (stage = 0; stage < 8; stage++)
	{
		inputs[count++] = D3D__RenderState[D3DRS_PSCONSTANT0_0 + stage];
		inputs[count++] = D3D__RenderState[D3DRS_PSCONSTANT1_0 + stage];
	}
	inputs[count++] = D3D__RenderState[D3DRS_PSFINALCOMBINERCONSTANT0];
	inputs[count++] = D3D__RenderState[D3DRS_PSFINALCOMBINERCONSTANT1];
	inputs[count++] = D3D__RenderState[D3DRS_FOGCOLOR];
	inputs[count++] = D3D__RenderState[D3DRS_FOGSTART];
	inputs[count++] = D3D__RenderState[D3DRS_FOGEND];
	inputs[count++] = D3D__RenderState[D3DRS_FOGDENSITY];
	inputs[count++] = D3D__RenderState[D3DRS_ALPHAREF];
	inputs[count++] = (DWORD)UI_OFFSET;
	memcpy(&inputs[count], texture_clip, 3 * sizeof(float));
	count += 3;
	for (stage = 0; stage < D3DTSS_MAXSTAGES; stage++)
	{
		DWORD *state = D3D__TextureState[stage];

		inputs[count++] = state[D3DTSS_BUMPENVMAT00];
		inputs[count++] = state[D3DTSS_BUMPENVMAT01];
		inputs[count++] = state[D3DTSS_BUMPENVMAT10];
		inputs[count++] = state[D3DTSS_BUMPENVMAT11];
		inputs[count++] = state[D3DTSS_BUMPENVLSCALE];
		inputs[count++] = state[D3DTSS_BUMPENVLOFFSET];
	}
	if (draw_uniform_inputs_known && words_equal(inputs, draw_uniform_inputs, sizeof(inputs)))
		return;
	memcpy(draw_uniform_inputs, inputs, sizeof(inputs));
	draw_uniform_inputs_known = TRUE;
	{
		struct dk_vertex_parameters vertex;
		struct dk_pixel_parameters pixel;

		memset(&vertex, 0, sizeof(vertex));
		memset(&pixel, 0, sizeof(pixel));
		memcpy(vertex.viewport_scale, device.viewport_scale, sizeof(vertex.viewport_scale));
		memcpy(vertex.viewport_offset, device.viewport_offset, sizeof(vertex.viewport_offset));
		vertex.point_and_screen[0] = D3D__RenderState[D3DRS_POINTSIZE] ?
			dword_to_float(D3D__RenderState[D3DRS_POINTSIZE]) : 1.0f;
		vertex.point_and_screen[1] = (float)UI_OFFSET;


		for (stage = 0; stage < 8; stage++)
		{
			color_to_vec4(D3D__RenderState[D3DRS_PSCONSTANT0_0 + stage], pixel.ps_c0[stage]);
			color_to_vec4(D3D__RenderState[D3DRS_PSCONSTANT1_0 + stage], pixel.ps_c1[stage]);
		}
		color_to_vec4(D3D__RenderState[D3DRS_PSFINALCOMBINERCONSTANT0], pixel.ps_final_c0);
		color_to_vec4(D3D__RenderState[D3DRS_PSFINALCOMBINERCONSTANT1], pixel.ps_final_c1);
		color_to_vec4(D3D__RenderState[D3DRS_FOGCOLOR], pixel.fog_color);
		pixel.fog_parameters[0] = dword_to_float(D3D__RenderState[D3DRS_FOGSTART]);
		pixel.fog_parameters[1] = dword_to_float(D3D__RenderState[D3DRS_FOGEND]);
		pixel.fog_parameters[2] = dword_to_float(D3D__RenderState[D3DRS_FOGDENSITY]);
		pixel.alpha_reference[0] = (float)(D3D__RenderState[D3DRS_ALPHAREF] & 0xff);
		/* (the clip, which the pixel shader makes before anything else: its
		limits in the units the stage is sampled in, 0 to 1) */
		pixel.alpha_reference[1] = texture_clip[0];
		pixel.alpha_reference[2] = texture_clip[1];
		pixel.alpha_reference[3] = texture_clip[2];
		memcpy(pixel.texture_scale, texture_scale, sizeof(pixel.texture_scale));
		for (stage = 0; stage < D3DTSS_MAXSTAGES; stage++)
		{
			DWORD *state = D3D__TextureState[stage];

			pixel.bump_matrix[stage][0] = dword_to_float(state[D3DTSS_BUMPENVMAT00]);
			pixel.bump_matrix[stage][1] = dword_to_float(state[D3DTSS_BUMPENVMAT01]);
			pixel.bump_matrix[stage][2] = dword_to_float(state[D3DTSS_BUMPENVMAT10]);
			pixel.bump_matrix[stage][3] = dword_to_float(state[D3DTSS_BUMPENVMAT11]);
			pixel.bump_luminance[stage][0] = dword_to_float(state[D3DTSS_BUMPENVLSCALE]);
			pixel.bump_luminance[stage][1] = dword_to_float(state[D3DTSS_BUMPENVLOFFSET]);
		}
		if (!parameters_known || !words_equal(&vertex, &vertex_parameters_told, sizeof(vertex)))
		{
			struct dk_command_vertex_parameters *command = stream_command(DK_COMMAND_VERTEX_PARAMETERS, sizeof(*command));

			command->parameters = vertex;
			vertex_parameters_told = vertex;
		}
		if (!parameters_known || !words_equal(&pixel, &pixel_parameters_told, sizeof(pixel)))
		{
			struct dk_command_pixel_parameters *command = stream_command(DK_COMMAND_PIXEL_PARAMETERS, sizeof(*command));

			command->parameters = pixel;
			pixel_parameters_told = pixel;
		}
		parameters_known = TRUE;
	}
}

static void draw_shaders_send(uint32_t vertex, uint32_t pixel)
{
	if (shaders_known && shaders_told[0] == vertex && shaders_told[1] == pixel)
		return;
	{
		struct dk_command_shaders *command = stream_command(DK_COMMAND_SHADERS, sizeof(*command));

		command->vertex = vertex;
		command->pixel = pixel;
	}
	shaders_told[0] = vertex;
	shaders_told[1] = pixel;
	shaders_known = TRUE;
}

static void draw_format_send(const struct dk_vertex_format *format)
{
	if (format_known && words_equal(format, &format_told, sizeof(*format)))
		return;
	{
		struct dk_command_vertex_format *command = stream_command(DK_COMMAND_VERTEX_FORMAT, sizeof(*command));

		command->format = *format;
	}
	format_told = *format;
	format_known = TRUE;
}

static uint32_t address_mode(DWORD mode)
{
	switch (mode)
	{
	case D3DTADDRESS_MIRROR: return DK_WRAP_MIRROR;
	case D3DTADDRESS_CLAMP:
	case D3DTADDRESS_CLAMPTOEDGE: return DK_WRAP_CLAMP;
	case D3DTADDRESS_BORDER: return DK_WRAP_BORDER;
	default: return DK_WRAP_REPEAT;
	}
}

/* what d3d8_gl.c's configure_sampler decides for a stage, as values for the
host's sampler. mipmapped: the texture has more than one level. hires: it is
a high-res replacement (xbox_textures_dk.c), drawn smaller than it is, so
filtered and from its mip levels whatever the game asks. */
static void sampler_make(int stage, BOOL mipmapped, BOOL hires, struct dk_sampler *sampler)
{
	DWORD *state = D3D__TextureState[stage];
	DWORD min_filter = hires ? D3DTEXF_LINEAR : state[D3DTSS_MINFILTER];
	DWORD mip_filter = hires ? D3DTEXF_LINEAR : mipmapped ? state[D3DTSS_MIPFILTER] : D3DTEXF_NONE;

	memset(sampler, 0, sizeof(*sampler));
	sampler->min_linear = min_filter != D3DTEXF_POINT;
	sampler->mag_linear = hires || state[D3DTSS_MAGFILTER] != D3DTEXF_POINT;
	sampler->mip_filter = mip_filter == D3DTEXF_NONE ? DK_MIP_NONE : mip_filter == D3DTEXF_POINT ? DK_MIP_NEAREST :
		DK_MIP_LINEAR;
	sampler->wrap[0] = address_mode(state[D3DTSS_ADDRESSU]);
	sampler->wrap[1] = address_mode(state[D3DTSS_ADDRESSV]);
	sampler->wrap[2] = address_mode(state[D3DTSS_ADDRESSW]);
	sampler->lod_bias = hires ? 0.0f : dword_to_float(state[D3DTSS_MIPMAPLODBIAS]);
	sampler->lod_minimum = hires ? 0.0f : (float)state[D3DTSS_MAXMIPLEVEL];
	sampler->anisotropy = (min_filter == D3DTEXF_ANISOTROPIC && state[D3DTSS_MAXANISOTROPY] > 1) ?
		(float)state[D3DTSS_MAXANISOTROPY] : 1.0f;
	color_to_vec4(state[D3DTSS_BORDERCOLOR], sampler->border);
}

/* A blended draw whose pixels leave the target as it was wherever the
source adds nothing: particles and glows near the camera, many layers of
large sprites whose soft edges and empty corners are most of what they
cover, were up to 11 ms of a 1080p frame, blending each of those pixels in
for nothing. Their pixel shader lets such a pixel go (nv2a_psh_dk.c); the
picture is the same, as the blend would have rounded back to what was there.
Not while depth or stencil is written (a stencil only tested is not) or the
samples are counted (the lens flares' visibility tests), which a let-go
pixel would change.
debug.gpu_blend_discard = false: never. */
static unsigned long blend_discard(void)
{
	static int enabled = -1;
	DWORD source = D3D__RenderState[D3DRS_SRCBLEND];
	DWORD destination = D3D__RenderState[D3DRS_DESTBLEND];
	DWORD operation = D3D__RenderState[D3DRS_BLENDOP];
	DWORD writes = D3D__RenderState[D3DRS_COLORWRITEENABLE];
	unsigned long mode = 0;

	if (enabled < 0)
	{
		enabled = config_boolean("debug.gpu_blend_discard") != 0;
		if (!enabled)
			platform_log("blend discard: off (debug.gpu_blend_discard)");
	}
	if (!enabled || !D3D__RenderState[D3DRS_ALPHABLENDENABLE] || device.visibility_test_active ||
		!(writes & D3DCOLORWRITEENABLE_ALL))
		return 0;
	/* (a stencil only tested - the game's stencil modes that accept or
	reject, the most of a frame's - is not written, whatever is let go) */
	if ((D3D__RenderState[D3DRS_ZENABLE] && D3D__RenderState[D3DRS_ZWRITEENABLE]) ||
		(D3D__RenderState[D3DRS_STENCILENABLE] && D3D__RenderState[D3DRS_STENCILWRITEMASK] &&
		(D3D__RenderState[D3DRS_STENCILFAIL] != D3DSTENCILOP_KEEP ||
		D3D__RenderState[D3DRS_STENCILZFAIL] != D3DSTENCILOP_KEEP ||
		D3D__RenderState[D3DRS_STENCILPASS] != D3DSTENCILOP_KEEP)))
	{
		static int said;

		if (!said++)
			platform_log("blend discard: not for blended draws that write depth or stencil");
		return 0;
	}
	if (destination == D3DBLEND_ONE && (operation == D3DBLENDOP_ADD || operation == D3DBLENDOP_REVSUBTRACT))
		mode = source == D3DBLEND_SRCALPHA ? NV2A_PIXEL_DISCARD_ADD_ALPHA : NV2A_PIXEL_DISCARD_ADD;
	else if (operation == D3DBLENDOP_ADD && source == D3DBLEND_SRCALPHA && destination == D3DBLEND_INVSRCALPHA)
		mode = NV2A_PIXEL_DISCARD_OVER;
	else if (operation == D3DBLENDOP_ADD && ((source == D3DBLEND_ZERO && destination == D3DBLEND_SRCCOLOR) ||
		(source == D3DBLEND_DESTCOLOR && destination == D3DBLEND_ZERO)))
		mode = NV2A_PIXEL_DISCARD_MULTIPLY;
	else if (operation == D3DBLENDOP_ADD && source == D3DBLEND_ONE && destination == D3DBLEND_INVSRCALPHA)
		mode = NV2A_PIXEL_DISCARD_PREMULTIPLIED;
	/* (the destination times one less the source: kept where the source is
	black, as an added one) */
	else if (operation == D3DBLENDOP_ADD && source == D3DBLEND_ZERO && destination == D3DBLEND_INVSRCCOLOR)
		mode = NV2A_PIXEL_DISCARD_ADD;
	/* (min and max take no factors) */
	else if (operation == D3DBLENDOP_MAX)
		mode = NV2A_PIXEL_DISCARD_MAX;
	else if (operation == D3DBLENDOP_MIN)
		mode = NV2A_PIXEL_DISCARD_MIN;
	if (!mode)
	{
		/* (each blend no pixel is let go for, said once: what is left) */
		static DWORD said[16];
		static int said_count;
		DWORD blend = (source & 0xff) | (destination & 0xff) << 8 | (operation & 0xffff) << 16;
		int index;

		for (index = 0; index < said_count && said[index] != blend; index++)
			;
		if (index == said_count && said_count < 16)
		{
			said[said_count++] = blend;
			platform_log("blend discard: not for a blend of source %lu, destination %lu, operation %lu",
				(unsigned long)source, (unsigned long)destination, (unsigned long)operation);
		}
		return 0;
	}
	return (mode << NV2A_PIXEL_KEY_DISCARD_SHIFT) |
		((writes & D3DCOLORWRITEENABLE_ALPHA) ? NV2A_PIXEL_KEY_DISCARD_ALPHA_WRITTEN : 0);
}

/* The pixel shader's key as prepare_draw builds it, and with it what the
stages sample: every stage's texture is found here (finding one may send its
texels to the host), as bind_textures does. */
static void pixel_key_make(struct nv2a_pixel_shader_key *key, float texture_scale[4][4],
	struct dk_command_textures *textures)
{
	int stage;

	memset(key, 0, sizeof(*key));
	memset(textures, 0, sizeof(*textures));
	memcpy(key->combiner_state, D3D__RenderState, sizeof(key->combiner_state));
	/* constants are uniforms, not part of the program */
	memset(&key->combiner_state[D3DRS_PSCONSTANT0_0], 0, 16 * sizeof(DWORD));
	key->combiner_state[D3DRS_PSFINALCOMBINERCONSTANT0] = 0;
	key->combiner_state[D3DRS_PSFINALCOMBINERCONSTANT1] = 0;
	key->texture_modes = D3D__RenderState[D3DRS_PSTEXTUREMODES];
	if (texture_clip[2] != 0.0f)
		key->texture_modes |= NV2A_PIXEL_KEY_CLIP | ((unsigned long)texture_clip_stage << NV2A_PIXEL_KEY_CLIP_STAGE_SHIFT) |
			(texture_clip_3d ? NV2A_PIXEL_KEY_CLIP_3D : 0) | (texture_clip_black ? NV2A_PIXEL_KEY_CLIP_BLACK : 0);
	if (warp_blend)
		key->texture_modes |= NV2A_PIXEL_KEY_WARP_BLEND;
	for (stage = 0; stage < D3DTSS_MAXSTAGES; stage++)
	{
		D3DBaseTexture *texture = device.textures[stage];
		unsigned long mode = stage_texture_mode(stage);

		texture_scale[stage][0] = texture_scale[stage][1] = 1.0f;
		texture_scale[stage][2] = texture_scale[stage][3] = 1.0f;
		key->alpha_kill[stage] = D3D__TextureState[stage][D3DTSS_ALPHAKILL] == D3DTALPHAKILL_ENABLE;
		key->color_sign[stage] = (unsigned char)((D3D__TextureState[stage][D3DTSS_COLORSIGN] >> 28) & 0xf);
		if (!texture || !texture->Data || mode == 0 || mode == 0x04 || mode == 0x05 || mode == 0x11)
		{
			key->sampler_type[stage] = mode == 0x11 ? _xgpu_sampler_2d : _xgpu_sampler_none;
			continue;
		}
		{
			const D3DCOLOR *palette = device.palettes[stage] && device.palettes[stage]->Data ?
				(const D3DCOLOR *)PLATFORM_PHYSICAL_TO_VIRTUAL(device.palettes[stage]->Data) : NULL;
			struct xgpu_texture_description description;
			int kind = DK_TEXTURE_2D;

			const struct rendered *target = rendered_find(texture->Data);

			memset(&description, 0, sizeof(description));
			if (target)
			{
				/* a render target: the host's image of it */
				xgpu_texture_describe(texture->Format, texture->Size, &description);
				textures->stages[stage].target = texture->Data;
				/* (rendered a level at a time: the levels as one image, as
				bind_textures in d3d8_gl.c makes them) */
				if (!description.linear && !description.cube_map && description.levels > 1 &&
					description.levels <= DK_COMPOSITE_LEVELS && target->width == description.width &&
					target->height == description.height)
				{
					composite_tell(&description, texture->Data);
					textures->stages[stage].composite = (uint32_t)description.levels;
				}
				else
					description.levels = 1;
				if (description.linear)
				{
					texture_scale[stage][0] = 1.0f / (float)target->width;
					texture_scale[stage][1] = 1.0f / (float)target->height;
				}
			}
			else
			{
				textures->stages[stage].id = dk_texture_get((const DWORD *)texture, palette, &kind, &description);
				if (description.linear)
				{
					texture_scale[stage][0] = 1.0f / (float)description.width;
					texture_scale[stage][1] = 1.0f / (float)description.height;
				}
			}
			if (stage == 0)
				key->coverage_alpha = description.hires_coverage != FALSE;
			key->sampler_type[stage] = kind == DK_TEXTURE_CUBE ? _xgpu_sampler_cube : kind == DK_TEXTURE_3D ?
				_xgpu_sampler_3d : _xgpu_sampler_2d;
			sampler_make(stage, description.levels > 1, description.hires, &textures->stages[stage].sampler);
		}
	}
	/* (only with the meter's blend: hud_hires.h, nv2a_pixel_shader_key) */
	key->coverage_alpha = key->coverage_alpha && D3D__RenderState[D3DRS_ALPHABLENDENABLE] &&
		D3D__RenderState[D3DRS_SRCBLEND] == D3DBLEND_CONSTANTCOLOR &&
		D3D__RenderState[D3DRS_DESTBLEND] == D3DBLEND_SRCALPHA;
	key->alpha_test_function = D3D__RenderState[D3DRS_ALPHATESTENABLE] ? D3D__RenderState[D3DRS_ALPHAFUNC] : 0;
	key->fog_enable = D3D__RenderState[D3DRS_FOGENABLE] != 0;
	key->fog_table_mode = (unsigned char)D3D__RenderState[D3DRS_FOGTABLEMODE];
	key->texture_modes |= blend_discard();
	/* a 3D clip (a dynamic light's distance attenuation): the pixels past the
	texture's reach go too, if it is known (dk_texture_reach) */
	if (texture_clip[2] != 0.0f && texture_clip_3d)
	{
		D3DBaseTexture *texture = device.textures[texture_clip_stage];
		float reach = texture && texture->Data ?
			dk_texture_reach((unsigned long)PLATFORM_PHYSICAL_TO_VIRTUAL(texture->Data)) : -1.0f;

		if (reach >= 0.0f)
		{
			texture_scale[texture_clip_stage][3] = reach;
			key->texture_modes |= NV2A_PIXEL_KEY_CLIP_SPHERE;
		}
	}
}

static void draw_textures_send(const struct dk_command_textures *textures)
{
	if (textures_known && words_equal(&textures->stages, &textures_told.stages, sizeof(textures->stages)))
		return;
	{
		struct dk_command_textures *command = stream_command(DK_COMMAND_TEXTURES, sizeof(*command));

		memcpy(command->stages, textures->stages, sizeof(command->stages));
	}
	textures_told = *textures;
	textures_known = TRUE;
}

/* d3d8_gl.c's prepare_draw: everything a draw needs but its vertices, written
to the stream. FALSE if there is nothing to draw into, or a shader the draw
needs is not ready (skipped, as decided: DEKO3D.md, "Decisions"). */
static BOOL draw_prepare(BOOL immediate)
{
	struct vertex_shader_object *program = current_program();
	struct dk_vertex_key vertex_key;
	struct nv2a_pixel_shader_key pixel_key;
	struct dk_command_textures textures;
	float texture_scale[4][4];
	uint32_t vertex_shader, pixel_shader;
	BOOL has_depth = FALSE;

	/* (the fixed-function pipeline's draws: immediate mode's only, as in
	d3d8_gl.c; draws from vertex buffers have no declaration to read them
	by, and no game draw makes one) */
	if (device.fixed_function ? !immediate : !program || !device.vertex_shader || !program->instructions)
	{
		stats.skipped_no_program++;
		return FALSE;
	}
	if (!targets_bind(&has_depth))
	{
		stats.skipped_no_target++;
		return FALSE;
	}
	pixel_key_make(&pixel_key, texture_scale, &textures);
	memset(&vertex_key, 0, sizeof(vertex_key));
	if (device.fixed_function)
		vertex_key.program_hash = DK_FIXED_FUNCTION_PROGRAM_HASH;
	else
	{
		vertex_key.program_hash = program->program_hash;
		vertex_key.packed_mask = immediate ? 0 : (uint32_t)device.vertex_shader->packed_mask;
	}
	/* (both are asked for, so that both are queued if neither is ready) */
	vertex_shader = dk_shader_for_draw(DK_SHADER_STAGE_VERTEX, &vertex_key);
	pixel_shader = dk_shader_for_draw(DK_SHADER_STAGE_PIXEL, &pixel_key);
	if (!vertex_shader || !pixel_shader)
	{
		stats.skipped_shader++;
		return FALSE;
	}
	if (immediate)
		stats.immediate_draws++;
	else
		stats.draws++;
	draw_state_send(has_depth);
	draw_shaders_send(vertex_shader, pixel_shader);
	draw_constants_send();
	draw_parameters_send(texture_scale);
	draw_textures_send(&textures);
	return TRUE;
}

static uint32_t attribute_format(const struct vertex_element *element)
{
	switch (element->type)
	{
	case D3DVSDT_FLOAT1: return DK_ATTRIBUTE_FLOAT1;
	case D3DVSDT_FLOAT2: return DK_ATTRIBUTE_FLOAT2;
	case D3DVSDT_FLOAT3:
	case D3DVSDT_FLOAT2H: return DK_ATTRIBUTE_FLOAT3;
	case D3DVSDT_FLOAT4: return DK_ATTRIBUTE_FLOAT4;
	case D3DVSDT_D3DCOLOR: return DK_ATTRIBUTE_COLOR;
	case D3DVSDT_SHORT1: return DK_ATTRIBUTE_SHORT1;
	case D3DVSDT_SHORT2: return DK_ATTRIBUTE_SHORT2;
	case D3DVSDT_SHORT3: return DK_ATTRIBUTE_SHORT3;
	case D3DVSDT_SHORT4: return DK_ATTRIBUTE_SHORT4;
	case D3DVSDT_NORMSHORT1: return DK_ATTRIBUTE_NORMSHORT1;
	case D3DVSDT_NORMSHORT2: return DK_ATTRIBUTE_NORMSHORT2;
	case D3DVSDT_NORMSHORT3: return DK_ATTRIBUTE_NORMSHORT3;
	case D3DVSDT_NORMSHORT4: return DK_ATTRIBUTE_NORMSHORT4;
	case D3DVSDT_PBYTE1: return DK_ATTRIBUTE_BYTE1;
	case D3DVSDT_PBYTE2: return DK_ATTRIBUTE_BYTE2;
	case D3DVSDT_PBYTE3: return DK_ATTRIBUTE_BYTE3;
	case D3DVSDT_PBYTE4: return DK_ATTRIBUTE_BYTE4;
	case D3DVSDT_NORMPACKED3: return DK_ATTRIBUTE_PACKED;
	default: return DK_ATTRIBUTE_FLOAT4;
	}
}

/* the declaration's registers and the streams' strides, with the unfed
registers at the values SetVertexData last gave them (setup_streams in
d3d8_gl.c); only those values are kept, so that a change to a register the
declaration feeds does not make a new format */
static void format_from_declaration(struct dk_vertex_format *format)
{
	struct vertex_shader_object *declaration = device.vertex_shader;
	unsigned long index;

	memset(format, 0, sizeof(*format));
	for (index = 0; index < XGPU_VERTEX_ATTRIBUTE_COUNT; index++)
	{
		if (declaration->packed_mask & (1UL << index))
		{
			format->attributes[index].format = DK_ATTRIBUTE_UNFED_PACKED;
		}
		else
		{
			format->attributes[index].format = DK_ATTRIBUTE_UNFED;
			memcpy(format->constants[index], device.attributes[index], sizeof(format->constants[index]));
		}
	}
	for (index = 0; index < declaration->element_count; index++)
	{
		const struct vertex_element *element = &declaration->elements[index];

		if (!device.streams[element->stream].data || element->type == D3DVSDT_NONE)
			continue;
		memset(format->constants[element->reg], 0, sizeof(format->constants[element->reg]));
		format->attributes[element->reg].format = attribute_format(element);
		format->attributes[element->reg].stream = element->stream;
		format->attributes[element->reg].offset = element->offset;
		format->strides[element->stream] = device.streams[element->stream].stride;
		format->stream_mask |= 1UL << element->stream;
	}
}

static uint32_t primitive_of(D3DPRIMITIVETYPE type)
{
	switch (type)
	{
	case D3DPT_POINTLIST: return DK_PRIMITIVE_POINTS;
	case D3DPT_LINELIST: return DK_PRIMITIVE_LINES;
	case D3DPT_LINELOOP: return DK_PRIMITIVE_LINE_LOOP;
	case D3DPT_LINESTRIP: return DK_PRIMITIVE_LINE_STRIP;
	case D3DPT_TRIANGLESTRIP:
	case D3DPT_QUADSTRIP: return DK_PRIMITIVE_TRIANGLE_STRIP;
	case D3DPT_TRIANGLEFAN:
	case D3DPT_POLYGON: return DK_PRIMITIVE_TRIANGLE_FAN;
	case D3DPT_QUADLIST: return DK_PRIMITIVE_QUADS;
	default: return DK_PRIMITIVE_TRIANGLES;
	}
}

/* what the draws of a stream read: the vertices [first, first + count), and
the bytes a stride-0 stream (one value for every vertex) is given */
#define STRIDELESS_STREAM_BYTES 64

/* writes the draw of vertices [first, first + count) of the streams the
format reads (a lowest vertex of first, so a stream's data starts there);
indices, if not NULL, is the guest address of the draw's indices */
/* the most a draw's indices copied into its command take (DK_INDEX_INLINE):
a larger list is read where it is, as before */
#define INLINE_INDEX_LIMIT (256 * 1024)

/* whether a draw's indices stay as they are until the GPU reads them: those
in the Xbox window - the map's own index buffers, where the game keeps them,
and Custom Edition's converted geometry (custom_edition_geometry.c) - which
nothing writes while the map is loaded, or in Custom Edition's tag cache.
The others are the lists the game remakes as it draws, its dynamic index
buffer's (D3DDevice_CreateIndexBuffer's, in the C heap: d3d8_resources.c):
split screen remakes them for the second view where the first's were. Only
those are copied as the draw is made (DK_INDEX_INLINE); copying every list
not in the tag cache, as at first, copied the map's own each draw and slowed
every game */
static BOOL indices_fixed(const WORD *indices, unsigned long count)
{
	unsigned long start = (unsigned long)(uintptr_t)indices, end = start + count * sizeof(WORD);
	unsigned long custom = (unsigned long)(uintptr_t)halo_custom_edition_tag_cache();

	if (platform_is_contiguous(indices) && platform_is_contiguous((const char *)indices + count * sizeof(WORD) - 1))
		return TRUE;
	return custom && start >= custom && end <= custom + CUSTOM_EDITION_TAG_CACHE_BYTES;
}

static void draw_write(D3DPRIMITIVETYPE type, unsigned long count, unsigned long first, unsigned long vertex_count,
	const WORD *indices, long vertex_offset)
{
	struct dk_command_draw *command;
	unsigned long stream, streams = 0, slot = 0;
	unsigned long index_bytes = indices && count * sizeof(WORD) <= INLINE_INDEX_LIMIT && !indices_fixed(indices, count) ?
		(count * sizeof(WORD) + 3) & ~3UL : 0;

	for (stream = 0; stream < DK_STREAM_COUNT; stream++)
		streams += (format_told.stream_mask >> stream) & 1;
	command = stream_command(DK_COMMAND_DRAW, sizeof(*command) + streams * 2 * sizeof(uint32_t) + index_bytes);
	command->primitive = primitive_of(type);
	command->count = (uint32_t)count;
	command->index_address = index_bytes ? DK_INDEX_INLINE : (uint32_t)(uintptr_t)indices;
	if (index_bytes)
		memcpy(&command->streams[streams], indices, count * sizeof(WORD));
	command->vertex_offset = (int32_t)vertex_offset;
	command->inline_bytes = 0;
	command->stream_count = (uint32_t)streams;
	for (stream = 0; stream < DK_STREAM_COUNT; stream++)
	{
		unsigned long stride = device.streams[stream].stride;

		if (!((format_told.stream_mask >> stream) & 1))
			continue;
		command->streams[slot][0] = (uint32_t)(uintptr_t)PLATFORM_PHYSICAL_TO_VIRTUAL(device.streams[stream].data) +
			(uint32_t)(first * stride);
		command->streams[slot][1] = (uint32_t)(stride ? stride * vertex_count : STRIDELESS_STREAM_BYTES);
		slot++;
	}
	/* (after the command: writing it may have handed the stream over, and the
	draw is in the submission it is written to) */
	draw_resources_used(indices != NULL, TRUE);
}

void WINAPI D3DDevice_DrawVertices(D3DPRIMITIVETYPE primitive_type, UINT start_vertex, UINT vertex_count)
{
	struct dk_vertex_format format;

	if (primitive_type == D3DPT_QUADLIST)
		vertex_count &= ~3U;
	if (!vertex_count || !draw_prepare(FALSE))
		return;
	format_from_declaration(&format);
	draw_format_send(&format);
	draw_write(primitive_type, vertex_count, start_vertex, vertex_count, NULL, 0);
}

void WINAPI D3DDevice_DrawIndexedVertices(D3DPRIMITIVETYPE primitive_type, UINT vertex_count, CONST WORD *index_data)
{
	struct dk_vertex_format format;
	unsigned long index, minimum = 0xffff, maximum = 0;

	if (primitive_type == D3DPT_QUADLIST)
		vertex_count &= ~3U;
	if (!vertex_count || !index_data || !draw_prepare(FALSE))
		return;
	/* the streams are read from the lowest vertex the indices name, to the
	highest (index i is vertex base + i) */
	for (index = 0; index < vertex_count; index++)
	{
		if (index_data[index] < minimum)
			minimum = index_data[index];
		if (index_data[index] > maximum)
			maximum = index_data[index];
	}
	format_from_declaration(&format);
	draw_format_send(&format);
	draw_write(primitive_type, vertex_count, device.base_vertex_index + minimum, maximum - minimum + 1, index_data,
		-(long)minimum);
}

/* ---------- immediate mode */

void WINAPI D3DDevice_Begin(D3DPRIMITIVETYPE primitive_type)
{
	device.immediate_active = TRUE;
	device.immediate_type = primitive_type;
	device.immediate_count = 0;
	device.immediate_mask = 0;
	memcpy(device.immediate_begin, device.attributes, sizeof(device.immediate_begin));
}

/* a vertex: the registers written since Begin, each in its own place of
the vertex's sixteen (the others are the same for every vertex of the draw,
and go with it as constants) */
static void immediate_emit(void)
{
	unsigned long floats = XGPU_VERTEX_ATTRIBUTE_COUNT * 4, mask = device.immediate_mask;
	float *vertex;

	if (device.immediate_count == device.immediate_capacity)
	{
		device.immediate_capacity = device.immediate_capacity ? device.immediate_capacity * 2 : 256;
		device.immediate_vertices = realloc(device.immediate_vertices,
			device.immediate_capacity * floats * sizeof(float));
	}
	vertex = device.immediate_vertices + device.immediate_count * floats;
	while (mask)
	{
		unsigned long reg = (unsigned long)__builtin_ctzl(mask);

		memcpy(vertex + reg * 4, device.attributes[reg], 4 * sizeof(float));
		mask &= mask - 1;
	}
	device.immediate_count++;
}

void WINAPI D3DDevice_End(void)
{
	/* the registers written since Begin, four floats each, a vertex at a
	time; the rest are the draw's constants, their values throughout */
	unsigned long floats = XGPU_VERTEX_ATTRIBUTE_COUNT * 4, mask = device.immediate_mask;
	unsigned long count = device.immediate_count, index, fed = 0, stride, vertex;
	D3DPRIMITIVETYPE type = device.immediate_type;
	struct dk_vertex_format format;
	struct dk_command_draw *command;
	float *out;

	device.immediate_active = FALSE;
	device.immediate_count = 0;
	device.immediate_mask = 0;
	if (type == D3DPT_QUADLIST)
		count &= ~3UL;
	if (!count)
		return;
	stride = (unsigned long)__builtin_popcountl(mask) * 4 * sizeof(float);
	/* the vertices go in the stream with the draw: the next Begin reuses
	the buffer they are in before the stream is handed over */
	if (count * stride + 4096 > STREAM_SIZE)
	{
		stats.skipped_size++;
		return;
	}
	if (!draw_prepare(TRUE))
		return;
	/* (the fixed-function pipeline's: positions into clip space) */
	if (device.fixed_function)
	{
		for (vertex = 0; vertex < count; vertex++)
			fixed_function_transform(device.immediate_vertices + vertex * floats);
	}
	memset(&format, 0, sizeof(format));
	for (index = 0; index < XGPU_VERTEX_ATTRIBUTE_COUNT; index++)
	{
		if (mask & (1UL << index))
		{
			format.attributes[index].format = DK_ATTRIBUTE_FLOAT4;
			format.attributes[index].stream = 0;
			format.attributes[index].offset = (uint32_t)(fed++ * 4 * sizeof(float));
		}
		else
		{
			format.attributes[index].format = DK_ATTRIBUTE_UNFED;
			memcpy(format.constants[index], device.attributes[index], sizeof(format.constants[index]));
		}
	}
	format.strides[0] = (uint32_t)stride;
	format.stream_mask = 1;
	draw_format_send(&format);
	command = stream_command(DK_COMMAND_DRAW, sizeof(*command) + 2 * sizeof(uint32_t) + count * stride);
	command->primitive = primitive_of(type);
	command->count = (uint32_t)count;
	command->index_address = 0;
	command->vertex_offset = 0;
	command->inline_bytes = (uint32_t)(count * stride);
	command->stream_count = 1;
	command->streams[0][0] = 0;
	command->streams[0][1] = (uint32_t)(count * stride);
	out = (float *)&command->streams[1];
	for (vertex = 0; vertex < count; vertex++)
	{
		const float *in = device.immediate_vertices + vertex * floats;
		unsigned long left = mask;

		while (left)
		{
			unsigned long reg = (unsigned long)__builtin_ctzl(left);

			/* a vertex made before the register was first written has its
			value from before Begin */
			memcpy(out, vertex < device.immediate_first[reg] ? device.immediate_begin[reg] : in + reg * 4,
				4 * sizeof(float));
			out += 4;
			left &= left - 1;
		}
	}
	draw_resources_used(FALSE, FALSE);
}

static void set_attribute(INT reg, float a, float b, float c, float d)
{
	BOOL emit = FALSE;

	if (reg == D3DVSDE_VERTEX)
	{
		reg = 0;
		emit = TRUE;
	}
	if (reg < 0 || reg >= XGPU_VERTEX_ATTRIBUTE_COUNT)
		return;
	if (device.immediate_active && !(device.immediate_mask & (1UL << reg)))
	{
		device.immediate_mask |= 1UL << reg;
		device.immediate_first[reg] = device.immediate_count;
	}
	device.attributes[reg][0] = a;
	device.attributes[reg][1] = b;
	device.attributes[reg][2] = c;
	device.attributes[reg][3] = d;
	/* like the hardware, writing register 0 completes a vertex */
	if (device.immediate_active && (emit || reg == 0))
		immediate_emit();
}

void WINAPI D3DDevice_SetVertexData2f(INT reg, FLOAT a, FLOAT b)
{
	set_attribute(reg, a, b, 0.0f, 1.0f);
}

void WINAPI D3DDevice_SetVertexData4f(INT reg, FLOAT a, FLOAT b, FLOAT c, FLOAT d)
{
	set_attribute(reg, a, b, c, d);
}

void WINAPI D3DDevice_SetVertexData2s(INT reg, SHORT a, SHORT b)
{
	set_attribute(reg, (float)a, (float)b, 0.0f, 1.0f);
}

void WINAPI D3DDevice_SetVertexData4ub(INT reg, BYTE a, BYTE b, BYTE c, BYTE d)
{
	set_attribute(reg, a / 255.0f, b / 255.0f, c / 255.0f, d / 255.0f);
}

void WINAPI D3DDevice_SetVertexDataColor(INT reg, D3DCOLOR color)
{
	float value[4];

	color_to_vec4(color, value);
	set_attribute(reg, value[0], value[1], value[2], value[3]);
}
/* ---------- clearing */

void WINAPI D3DDevice_Clear(DWORD count, CONST D3DRECT *rectangles, DWORD flags, D3DCOLOR color, float z, DWORD stencil)
{
	struct dk_command_clear *command;
	uint32_t clear_flags = 0;
	DWORD index, kept = 0;

	if (flags & D3DCLEAR_TARGET)
	{
		/* the Xbox clears the channels named (D3DCLEAR_TARGET_R, _G, _B, _A):
		the fog screen clears only alpha, leaving the picture under the fog */
		clear_flags |= ((flags & D3DCLEAR_TARGET_R) ? DK_CLEAR_RED : 0) | ((flags & D3DCLEAR_TARGET_G) ? DK_CLEAR_GREEN : 0) |
			((flags & D3DCLEAR_TARGET_B) ? DK_CLEAR_BLUE : 0) | ((flags & D3DCLEAR_TARGET_A) ? DK_CLEAR_ALPHA : 0);
	}
	if (flags & D3DCLEAR_ZBUFFER)
		clear_flags |= DK_CLEAR_DEPTH;
	if (flags & D3DCLEAR_STENCIL)
		clear_flags |= DK_CLEAR_STENCIL;
	if (!clear_flags || !targets_bind(NULL))
		return;
	command = stream_command(DK_COMMAND_CLEAR, sizeof(*command) + (count && rectangles ? count : 1) * 4 * sizeof(uint32_t));
	/* (after the command: writing it may have handed the stream over, and the
	clear is in the submission it is written to) */
	targets_used();
	command->flags = clear_flags;
	color_to_vec4(color, command->color);
	command->depth = z;
	command->stencil = stencil;
	if (!count || !rectangles)
	{
		/* the NV2A clips a viewport-less clear to the viewport, which is what
		keeps a split-screen window's clear from wiping the other window */
		int x0 = target_pixel((float)device.viewport.X, 0), y0 = target_pixel((float)device.viewport.Y, 1);

		command->rectangles[0][0] = (uint32_t)x0;
		command->rectangles[0][1] = (uint32_t)y0;
		command->rectangles[0][2] = (uint32_t)(target_pixel((float)(device.viewport.X + device.viewport.Width), 0) - x0);
		command->rectangles[0][3] = (uint32_t)(target_pixel((float)(device.viewport.Y + device.viewport.Height), 1) - y0);
		command->rectangle_count = 1;
		return;
	}
	for (index = 0; index < count; index++)
	{
		INT left = rectangles[index].x1 > (INT)device.viewport.X ? rectangles[index].x1 : (INT)device.viewport.X;
		INT top = rectangles[index].y1 > (INT)device.viewport.Y ? rectangles[index].y1 : (INT)device.viewport.Y;
		INT right = rectangles[index].x2 < (INT)(device.viewport.X + device.viewport.Width) ?
			rectangles[index].x2 : (INT)(device.viewport.X + device.viewport.Width);
		INT bottom = rectangles[index].y2 < (INT)(device.viewport.Y + device.viewport.Height) ?
			rectangles[index].y2 : (INT)(device.viewport.Y + device.viewport.Height);

		if (left >= right || top >= bottom)
			continue;
		{
			int x0 = target_pixel((float)(left + UI_OFFSET), 0), y0 = target_pixel((float)top, 1);

			command->rectangles[kept][0] = (uint32_t)x0;
			command->rectangles[kept][1] = (uint32_t)y0;
			command->rectangles[kept][2] = (uint32_t)(target_pixel((float)(right + UI_OFFSET), 0) - x0);
			command->rectangles[kept][3] = (uint32_t)(target_pixel((float)bottom, 1) - y0);
		}
		kept++;
	}
	command->rectangle_count = kept;
}

/* ---------- presentation */

/* debug.screenshot_every: every that many frames the host reads the back
buffer back into a buffer the guest gives (dk_command_present), and the
frame is written as a BMP in debug.screenshot_directory, as d3d8_gl.c does */
static long screenshot_every = -1;
static int statistics_enabled = -1;
static unsigned char *screenshot_pixels;
static uint32_t screenshot_width, screenshot_height;

static void screenshot_write(void)
{
	const char *directory = config_string("debug.screenshot_directory");
	unsigned long width = screenshot_width, height = screenshot_height;
	unsigned long image_size = width * height * 4, pixel;
	unsigned char header[54] = { 'B', 'M' };
	char path[512];
	FILE *file;

	/* the host's RGBA, as BGRA; the display ignores destination alpha, which
	the game uses as scratch, and image viewers would show it as transparency */
	for (pixel = 0; pixel < width * height; pixel++)
	{
		unsigned char red = screenshot_pixels[pixel * 4];

		screenshot_pixels[pixel * 4] = screenshot_pixels[pixel * 4 + 2];
		screenshot_pixels[pixel * 4 + 2] = red;
		screenshot_pixels[pixel * 4 + 3] = 0xff;
	}
	snprintf(path, sizeof(path), "%s/frame%05lu.bmp", directory, device.frame);
	file = fopen(path, "wb");
	if (!file)
		return;
	*(unsigned int *)(header + 2) = (unsigned int)(54 + image_size);
	*(unsigned int *)(header + 10) = 54;
	*(unsigned int *)(header + 14) = 40;
	*(int *)(header + 18) = (int)width;
	*(int *)(header + 22) = -(int)height; /* rows from the top, as read */
	*(unsigned short *)(header + 26) = 1;
	*(unsigned short *)(header + 28) = 32;
	*(unsigned int *)(header + 34) = (unsigned int)image_size;
	fwrite(header, 1, sizeof(header), file);
	fwrite(screenshot_pixels, 1, image_size, file);
	fclose(file);
}

/* debug.gpu_stats: what the draws of the last second came to */
static void draw_statistics_log(void)
{
	platform_log("frame %lu: %lu draws, %lu immediate; skipped %lu no program, %lu no target, %lu shader not ready, "
		"%lu too big", device.frame, stats.draws, stats.immediate_draws, stats.skipped_no_program,
		stats.skipped_no_target, stats.skipped_shader, stats.skipped_size);
	memset(&stats, 0, sizeof(stats));
}

void WINAPI D3DDevice_Present(CONST RECT *source_rectangle, CONST RECT *destination_rectangle,
	void *unused, void *unused2)
{
	(void)source_rectangle;
	(void)destination_rectangle;
	(void)unused;
	(void)unused2;
	/* Not at the first frame: the rasterizer presents once as it starts,
	before it makes the game's vertex shaders (rasterizer_initialize), and a
	dump then has none. A second's frames on, it has made them all - which
	is also when the shader cache's pass can start (dk_shaders.c) */
	if (!shaders_dumped && device.frame >= 60)
	{
		shaders_dumped = TRUE;
		shaders_dump();
		dk_shader_start();
	}
	/* the startup pass, a few keys a frame (dk_shaders.c) */
	dk_shader_frame();
	if (screenshot_every < 0)
		screenshot_every = config_integer("debug.screenshot_every");
	if (statistics_enabled < 0)
		statistics_enabled = config_boolean("debug.gpu_stats");
	if (statistics_enabled > 0 && device.frame % 60 == 0)
		draw_statistics_log();
	{
		struct dk_command_present *command = stream_command(DK_COMMAND_PRESENT, sizeof(*command));
		float scale[2];

		surface_describe(&device.back_buffer, FALSE, &command->back_buffer, scale);
		command->screenshot = 0;
		if (screenshot_every > 0 && device.frame % (unsigned long)screenshot_every == 0 && command->back_buffer.width &&
			*config_string("debug.screenshot_directory"))
		{
			/* (the back buffer's pixels: the host reads them as it has them) */
			screenshot_width = command->back_buffer.pixel_width;
			screenshot_height = command->back_buffer.pixel_height;
			screenshot_pixels = malloc((unsigned long)screenshot_width * screenshot_height * 4);
			command->screenshot = (uint32_t)(uintptr_t)screenshot_pixels;
		}
	}
	stream_flush();
	if (screenshot_pixels)
	{
		/* (the host writes the pixels as it runs the present, on its render
		thread: all of it run, and the GPU done) */
		host_dk_idle();
		screenshot_write();
		free(screenshot_pixels);
		screenshot_pixels = NULL;
	}
	/* the host starts the next frame with nothing bound */
	targets_known = FALSE;
	/* the stand-in window's swap: holds the frame to the display's rate
	until the host presents, after which its swapchain does */
	platform_video_swap();
	xgpu_texture_cache_begin_frame();
	device.frame++;
	platform_pump_events();

	pthread_mutex_lock(&vertical_blank_lock);
	/* the Xbox keeps at most two frames queued behind its 60 Hz display;
	with interpolation, frames come at the real display's rate instead */
	if (halo_interpolation_enabled())
	{
		flip_count++;
	}
	else
	{
		/* (port: the vertical blanks that retire the flips counted, started
		here if the game has not yet: the boot's loading screen presents
		before it sets its callback or waits for one, and with
		display.interpolation off its third frame waited for ever, a black
		screen) */
		if (!vertical_blank_thread_started)
		{
			pthread_mutex_unlock(&vertical_blank_lock);
			vertical_blank_start();
			pthread_mutex_lock(&vertical_blank_lock);
		}
		while (pending_flips >= 2)
			pthread_cond_wait(&vertical_blank_condition, &vertical_blank_lock);
		pending_flips++;
	}
	pthread_mutex_unlock(&vertical_blank_lock);
}

HRESULT WINAPI D3DDevice_PersistDisplay(void)
{
	return S_OK;
}
