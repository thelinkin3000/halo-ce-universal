/*
HOST_UI.C

The screens the Switch port shows before the game - the updater's offer and
progress, the disc image being unpacked, and messages - drawn into the console's default window as pictures
rather than as text in libnx's console.

Everything is drawn in software into a 1280x720 picture, which goes to a libnx
framebuffer on the default window when a screen is shown. That is the window
the console used, handed back the same way (framebufferClose, as consoleExit
does) before SDL takes it for the game, so the change of who draws there is
the one the port already makes. A screen is drawn only when it changes, and is
a few milliseconds of the CPU's time.

The panels are the menus' own: port/switch/art/menu_panel.svg, the item
background from the game's shell (a rounded box with a 4-unit blue border),
rendered with nanosvg and stretched as nine pieces - the corners as drawn, the
edges and the middle stretched - so its corners meet the screen's corners, or a
row's, with the border and the rounding unchanged. The text is stb_truetype's:
OpenCE for titles and Overpass for the rest (port/assets/fonts).

Built with HOST_UI_PREVIEW on a desktop, the same drawing writes each screen to
a .ppm file instead, so the screens can be looked at without a console.
*/

#ifndef HOST_UI_PREVIEW
#include "host.h"

#include <switch.h>
#else
#include "host_ui_preview.h"
#endif
#include "host_ui.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#include "../../third_party/stb/stb_truetype.h"

#define NANOSVG_IMPLEMENTATION
#include "../../third_party/nanosvg/nanosvg.h"
#define NANOSVGRAST_IMPLEMENTATION
#include "../../third_party/nanosvg/nanosvgrast.h"

enum
{
	SCREEN_WIDTH = 1280,
	SCREEN_HEIGHT = 720,
	MARGIN = 80,
	/* the panel's slice, in SVG units: past its corner's radius and border */
	PANEL_SLICE = 16,
	GLYPHS = 95,
};

/* the colours: the panel's own blues, and text that reads on them */
#define COLOUR_TEXT 0xffe8f2ffu
#define COLOUR_DIM 0xff9ab4d4u
#define COLOUR_ACCENT 0xff2895ffu

/* ---------- the assets (host_ui_assets.S, or files in a preview) */

#ifndef HOST_UI_PREVIEW
extern const unsigned char host_ui_title_font[], host_ui_title_font_end[];
extern const unsigned char host_ui_body_font[], host_ui_body_font_end[];
extern const unsigned char host_ui_panel_svg[], host_ui_panel_svg_end[];
#endif

/* ---------- the picture */

static uint32_t *canvas;
static int dirty;

static uint32_t argb_to_rgba(uint32_t colour)
{
	/* colours are written 0xAARRGGBB; the picture is R, G, B, A in memory */
	uint32_t a = colour >> 24, r = (colour >> 16) & 0xff, g = (colour >> 8) & 0xff, b = colour & 0xff;

	return r | (g << 8) | (b << 16) | (a << 24);
}

static void blend(uint32_t *pixel, uint32_t r, uint32_t g, uint32_t b, uint32_t alpha)
{
	uint32_t old = *pixel;
	uint32_t inverse = 255 - alpha;

	if (!alpha)
		return;
	*pixel = ((r * alpha + (old & 0xff) * inverse) / 255) |
		(((g * alpha + ((old >> 8) & 0xff) * inverse) / 255) << 8) |
		(((b * alpha + ((old >> 16) & 0xff) * inverse) / 255) << 16) | 0xff000000u;
}

static void fill_rectangle(int x, int y, int width, int height, uint32_t colour)
{
	uint32_t rgba = argb_to_rgba(colour);
	int row, column;

	for (row = y < 0 ? 0 : y; row < y + height && row < SCREEN_HEIGHT; row++)
		for (column = x < 0 ? 0 : x; column < x + width && column < SCREEN_WIDTH; column++)
			canvas[row * SCREEN_WIDTH + column] = rgba;
}

/* an anti-aliased disc, for a button's mark */
static void fill_disc(float centre_x, float centre_y, float radius, uint32_t colour)
{
	int row, column;

	for (row = (int)(centre_y - radius - 1); row <= (int)(centre_y + radius + 1); row++)
	{
		for (column = (int)(centre_x - radius - 1); column <= (int)(centre_x + radius + 1); column++)
		{
			float dx = column + 0.5f - centre_x, dy = row + 0.5f - centre_y;
			float coverage = radius + 0.5f - sqrtf(dx * dx + dy * dy);

			if (row < 0 || row >= SCREEN_HEIGHT || column < 0 || column >= SCREEN_WIDTH || coverage <= 0.0f)
				continue;
			if (coverage > 1.0f)
				coverage = 1.0f;
			blend(&canvas[row * SCREEN_WIDTH + column], (colour >> 16) & 0xff, (colour >> 8) & 0xff, colour & 0xff,
				(uint32_t)(coverage * 255.0f));
		}
	}
}

/* ---------- the panel, in nine pieces */

struct panel
{
	unsigned char *pixels;
	int width, height, slice;
};

/* the background's, at twice the SVG's size */
static struct panel background_panel;

static int panel_render(struct panel *panel, float scale)
{
	char *text;
	size_t size;
	NSVGimage *image;
	NSVGrasterizer *rasterizer;
	NSVGshape *shape;
	float bounds[4] = { 1e9f, 1e9f, -1e9f, -1e9f };

#ifndef HOST_UI_PREVIEW
	size = (size_t)(host_ui_panel_svg_end - host_ui_panel_svg);
	text = malloc(size + 1);
	if (!text)
		return 0;
	memcpy(text, host_ui_panel_svg, size);
#else
	text = host_ui_preview_file("port/switch/art/menu_panel.svg", &size);
	if (!text)
		return 0;
#endif
	text[size] = 0;
	/* nanosvg parses the text in place */
	image = nsvgParse(text, "px", 96.0f);
	free(text);
	if (!image)
		return 0;
	/* the shape's own bounds, not the canvas's: the panel sits in the corner
	of a larger texture */
	for (shape = image->shapes; shape; shape = shape->next)
	{
		if (shape->bounds[0] < bounds[0])
			bounds[0] = shape->bounds[0];
		if (shape->bounds[1] < bounds[1])
			bounds[1] = shape->bounds[1];
		if (shape->bounds[2] > bounds[2])
			bounds[2] = shape->bounds[2];
		if (shape->bounds[3] > bounds[3])
			bounds[3] = shape->bounds[3];
	}
	panel->width = (int)ceilf((bounds[2] - bounds[0]) * scale);
	panel->height = (int)ceilf((bounds[3] - bounds[1]) * scale);
	panel->slice = (int)ceilf(PANEL_SLICE * scale);
	panel->pixels = calloc((size_t)panel->width * panel->height, 4);
	rasterizer = nsvgCreateRasterizer();
	if (!panel->pixels || !rasterizer)
	{
		nsvgDelete(image);
		return 0;
	}
	nsvgRasterize(rasterizer, image, -bounds[0] * scale, -bounds[1] * scale, scale, panel->pixels, panel->width,
		panel->height, panel->width * 4);
	nsvgDeleteRasterizer(rasterizer);
	nsvgDelete(image);
	return 1;
}

/* the source coordinate for a destination one, across one axis of nine */
static int slice_source(int at, int length, int source_length, int slice)
{
	if (at < slice)
		return at;
	if (at >= length - slice)
		return source_length - (length - at);
	return slice + (int)((long long)(at - slice) * (source_length - 2 * slice) / (length - 2 * slice));
}

static void draw_panel(const struct panel *panel, int x, int y, int width, int height)
{
	int row, column;

	if (!panel->pixels || width < 2 * panel->slice || height < 2 * panel->slice)
		return;
	for (row = 0; row < height; row++)
	{
		int source_row = slice_source(row, height, panel->height, panel->slice);

		if (y + row < 0 || y + row >= SCREEN_HEIGHT)
			continue;
		for (column = 0; column < width; column++)
		{
			const unsigned char *source;
			int source_column;

			if (x + column < 0 || x + column >= SCREEN_WIDTH)
				continue;
			source_column = slice_source(column, width, panel->width, panel->slice);
			source = panel->pixels + ((size_t)source_row * panel->width + source_column) * 4;
			blend(&canvas[(y + row) * SCREEN_WIDTH + x + column], source[0], source[1], source[2], source[3]);
		}
	}
}

/* ---------- text */

struct glyph
{
	unsigned char *bitmap;
	int x, y, width, height;
	float advance;
};

struct font
{
	stbtt_fontinfo *face;
	float scale;
	int ascent;
	struct glyph glyphs[GLYPHS];
};

static stbtt_fontinfo title_face, body_face;
static struct font title_font, body_font, small_font;

/* a font whose capitals are capitals pixels tall (0: one whose ascent to
descent is pixels), OpenCE's being much taller than its metrics say */
static int font_make(struct font *font, stbtt_fontinfo *face, float pixels, float capitals)
{
	int ascent, descent, gap, index;

	font->face = face;
	font->scale = stbtt_ScaleForPixelHeight(face, pixels);
	if (capitals > 0.0f)
	{
		int x0, y0, x1, y1;

		if (stbtt_GetCodepointBox(face, 'H', &x0, &y0, &x1, &y1) && y1 > y0)
			font->scale = capitals / (float)(y1 - y0);
	}
	stbtt_GetFontVMetrics(face, &ascent, &descent, &gap);
	font->ascent = (int)roundf(ascent * font->scale);
	for (index = 0; index < GLYPHS; index++)
	{
		struct glyph *glyph = &font->glyphs[index];
		int advance, bearing;

		stbtt_GetCodepointHMetrics(face, index + 32, &advance, &bearing);
		glyph->advance = advance * font->scale;
		glyph->bitmap = stbtt_GetCodepointBitmap(face, font->scale, font->scale, index + 32, &glyph->width,
			&glyph->height, &glyph->x, &glyph->y);
	}
	return 1;
}

/* the printable ASCII the screens are written in */
static const struct glyph *glyph_of(const struct font *font, char character)
{
	unsigned char code = (unsigned char)character;

	return &font->glyphs[(code >= 32 && code < 127 ? code : '?') - 32];
}

static float text_width(const struct font *font, const char *text)
{
	float width = 0.0f;

	for (; *text; text++)
		width += glyph_of(font, *text)->advance;
	return width;
}

/* text with its top at y, cut with "..." if it is wider than maximum (0: any
width); returns the width drawn */
static float draw_text(const struct font *font, float x, int y, const char *text, uint32_t colour, float maximum)
{
	float start = x;
	float ellipsis = text_width(font, "...");
	int cut = maximum > 0.0f && text_width(font, text) > maximum;
	uint32_t r = (colour >> 16) & 0xff, g = (colour >> 8) & 0xff, b = colour & 0xff;

	for (; *text; text++)
	{
		const struct glyph *glyph = glyph_of(font, *text);
		int row, column;

		if (cut && x + glyph->advance > start + maximum - ellipsis)
		{
			draw_text(font, x, y, "...", colour, 0.0f);
			return x + ellipsis - start;
		}
		for (row = 0; row < glyph->height; row++)
		{
			int screen_y = y + font->ascent + glyph->y + row;

			if (screen_y < 0 || screen_y >= SCREEN_HEIGHT)
				continue;
			for (column = 0; column < glyph->width; column++)
			{
				int screen_x = (int)roundf(x) + glyph->x + column;

				if (screen_x < 0 || screen_x >= SCREEN_WIDTH)
					continue;
				blend(&canvas[screen_y * SCREEN_WIDTH + screen_x], r, g, b,
					glyph->bitmap[row * glyph->width + column]);
			}
		}
		x += glyph->advance;
	}
	return x - start;
}

/* text broken into lines of at most width; returns the next line's top */
static int draw_wrapped(const struct font *font, int x, int y, int width, int line, const char *text, uint32_t colour)
{
	char buffer[512];
	const char *cursor = text;

	while (*cursor)
	{
		size_t length = 0, fits = 0;

		/* as many words as fit */
		while (cursor[length])
		{
			size_t next = length;

			while (cursor[next] == ' ')
				next++;
			while (cursor[next] && cursor[next] != ' ')
				next++;
			if (next >= sizeof(buffer))
				break;
			memcpy(buffer, cursor, next);
			buffer[next] = 0;
			if (fits && text_width(font, buffer) > width)
				break;
			fits = next;
			length = next;
		}
		if (!fits)
			fits = strlen(cursor) < sizeof(buffer) - 1 ? strlen(cursor) : sizeof(buffer) - 1;
		memcpy(buffer, cursor, fits);
		buffer[fits] = 0;
		draw_text(font, (float)x, y, buffer, colour, (float)width);
		y += line;
		cursor += fits;
		while (*cursor == ' ')
			cursor++;
	}
	return y;
}

/* ---------- what every screen has */

/* the screen's panel, its title and the keys along its foot. keys is pairs
of a button and what it does, "A", "Update now", "B", "Not now", ..., ending
in NULL */
static void draw_frame(const char *title, const char *const *keys)
{
	float x = SCREEN_WIDTH - MARGIN;
	int count, index;

	fill_rectangle(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT, 0xff000814u);
	draw_panel(&background_panel, 0, 0, SCREEN_WIDTH, SCREEN_HEIGHT);
	/* the title's baseline just above the line under it */
	draw_text(&title_font, MARGIN, 108 - title_font.ascent, title, COLOUR_TEXT, (float)(SCREEN_WIDTH - 2 * MARGIN - 200));
	fill_rectangle(MARGIN, 128, SCREEN_WIDTH - 2 * MARGIN, 2, COLOUR_ACCENT);
	/* the keys, right to left */
	for (count = 0; keys && keys[count]; count += 2)
	{
	}
	for (index = count - 2; index >= 0; index -= 2)
	{
		float label = text_width(&small_font, keys[index + 1]);

		x -= label;
		draw_text(&small_font, x, 652, keys[index + 1], COLOUR_TEXT, 0.0f);
		x -= 34;
		fill_disc(x + 14, 666, 14, COLOUR_ACCENT);
		{
			const struct glyph *mark = glyph_of(&small_font, keys[index][0]);

			draw_text(&small_font, x + 14 - mark->x - mark->width / 2.0f,
				666 - small_font.ascent - mark->y - mark->height / 2, keys[index], 0xff021836u, 0.0f);
		}
		x -= 36;
	}
	dirty = 1;
}

/* ---------- showing it */

#ifndef HOST_UI_PREVIEW
static Framebuffer framebuffer;
static int framebuffer_open;

static void present(void)
{
	u32 stride;
	unsigned char *pixels;
	int row;

	if (!framebuffer_open || !dirty)
		return;
	pixels = framebufferBegin(&framebuffer, &stride);
	for (row = 0; row < SCREEN_HEIGHT; row++)
		memcpy(pixels + (size_t)row * stride, canvas + (size_t)row * SCREEN_WIDTH, SCREEN_WIDTH * 4);
	framebufferEnd(&framebuffer);
	dirty = 0;
}
#else
static void present(void)
{
	host_ui_preview_write(canvas, SCREEN_WIDTH, SCREEN_HEIGHT);
	dirty = 0;
}
#endif

int host_ui_open(void)
{
	const unsigned char *title_data, *body_data;

	/* open again after a close (the disc image is unpacked after the menu has
	gone): the fonts and panels are kept, and only the window is taken back */
	if (canvas)
	{
#ifndef HOST_UI_PREVIEW
		if (!framebuffer_open)
		{
			if (R_FAILED(framebufferCreate(&framebuffer, nwindowGetDefault(), SCREEN_WIDTH, SCREEN_HEIGHT,
				PIXEL_FORMAT_RGBA_8888, 2)))
				return 0;
			framebufferMakeLinear(&framebuffer);
			framebuffer_open = 1;
		}
#endif
		return 1;
	}
	canvas = calloc((size_t)SCREEN_WIDTH * SCREEN_HEIGHT, 4);
	if (!canvas)
		return 0;
#ifndef HOST_UI_PREVIEW
	title_data = host_ui_title_font;
	body_data = host_ui_body_font;
#else
	title_data = (const unsigned char *)host_ui_preview_file("port/assets/fonts/OpenCE-Regular.ttf", NULL);
	body_data = (const unsigned char *)host_ui_preview_file("port/assets/fonts/Overpass-750.ttf", NULL);
#endif
	if (!stbtt_InitFont(&title_face, title_data, stbtt_GetFontOffsetForIndex(title_data, 0)) ||
		!stbtt_InitFont(&body_face, body_data, stbtt_GetFontOffsetForIndex(body_data, 0)))
		return 0;
	font_make(&title_font, &title_face, 0.0f, 40.0f);
	font_make(&body_font, &body_face, 25.0f, 0.0f);
	font_make(&small_font, &body_face, 21.0f, 0.0f);
	if (!panel_render(&background_panel, 2.0f))
		return 0;
#ifndef HOST_UI_PREVIEW
	if (R_FAILED(framebufferCreate(&framebuffer, nwindowGetDefault(), SCREEN_WIDTH, SCREEN_HEIGHT,
		PIXEL_FORMAT_RGBA_8888, 2)))
		return 0;
	framebufferMakeLinear(&framebuffer);
	framebuffer_open = 1;
#endif
	return 1;
}

void host_ui_close(void)
{
#ifndef HOST_UI_PREVIEW
	if (framebuffer_open)
	{
		/* a black screen while the game loads, rather than the last menu */
		memset(canvas, 0, (size_t)SCREEN_WIDTH * SCREEN_HEIGHT * 4);
		dirty = 1;
		present();
		framebufferClose(&framebuffer);
		framebuffer_open = 0;
	}
#endif
}

/* ---------- the screens */

void host_ui_message(const char *title, const char *message, const char *const *keys)
{
	draw_frame(title, keys);
	draw_wrapped(&body_font, MARGIN, 186, SCREEN_WIDTH - 2 * MARGIN, 36, message, COLOUR_TEXT);
	present();
}

void host_ui_progress(const char *title, const char *message, long long done, long long total,
	const char *const *keys)
{
	int width = SCREEN_WIDTH - 2 * MARGIN;
	int filled = total > 0 ? (int)((long long)width * (done < total ? done : total) / total) : 0;
	char amount[64];

	draw_frame(title, keys);
	draw_wrapped(&body_font, MARGIN, 186, width, 36, message, COLOUR_TEXT);
	fill_rectangle(MARGIN, 300, width, 20, 0xff0a2547u);
	fill_rectangle(MARGIN, 300, filled, 20, COLOUR_ACCENT);
	if (total > 0)
		snprintf(amount, sizeof(amount), "%lld of %lld MB", done / (1024 * 1024), total / (1024 * 1024));
	else
		snprintf(amount, sizeof(amount), "%lld MB", done / (1024 * 1024));
	draw_text(&small_font, MARGIN, 336, amount, COLOUR_DIM, 0.0f);
	present();
}
