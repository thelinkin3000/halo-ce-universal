/*
DK_COMMANDS.H

What the deko3d renderer's guest half (d3d8_dk.c) asks of its host half
(port/switch/host/host_dk.c): a stream of commands, written into guest
memory over a frame and handed to the host in one call (host_dk_submit), so
that a frame costs one crossing from the guest to the host rather than one a
draw.

Shared by both halves, which are built for different ABIs (the guest's
pointers are 32 bits, the host's 64): every field is a 32-bit integer or
float, and an address is a guest one, which the host can read directly (the
guest's memory is below 4 GB).

Each command starts with a header naming its type and its size in bytes,
the header included; sizes are multiples of 4.
*/

#ifndef __DK_COMMANDS_H
#define __DK_COMMANDS_H

#include <stdint.h>
#include "dk_shaders.h"

enum
{
	DK_COMMAND_TARGETS = 1,
	DK_COMMAND_CLEAR,
	DK_COMMAND_PRESENT,
	DK_COMMAND_STATE,
	DK_COMMAND_SHADERS,
	DK_COMMAND_CONSTANTS,
	DK_COMMAND_VERTEX_PARAMETERS,
	DK_COMMAND_PIXEL_PARAMETERS,
	DK_COMMAND_VERTEX_FORMAT,
	DK_COMMAND_DRAW,
};

/* a surface's kind, as the host makes its image */
enum
{
	DK_SURFACE_NONE = 0,
	DK_SURFACE_COLOR, /* 8 bits a channel (every color target, as d3d8_gl.c makes them) */
	DK_SURFACE_DEPTH, /* 24-bit depth, 8-bit stencil */
};

/* what a clear clears */
enum
{
	DK_CLEAR_RED = 1 << 0,
	DK_CLEAR_GREEN = 1 << 1,
	DK_CLEAR_BLUE = 1 << 2,
	DK_CLEAR_ALPHA = 1 << 3,
	DK_CLEAR_DEPTH = 1 << 4,
	DK_CLEAR_STENCIL = 1 << 5,
};

struct dk_command_header
{
	uint32_t type;
	uint32_t size;
};

/* a render target or depth buffer, known by the physical address of its
data (Data in its D3DSurface), as d3d8_gl.c knows them; its size is in
pixels */
struct dk_surface
{
	uint32_t data;
	uint32_t width;
	uint32_t height;
	uint32_t kind;
};

/* the surfaces later commands draw into (either can be DK_SURFACE_NONE) */
struct dk_command_targets
{
	struct dk_command_header header;
	struct dk_surface color;
	struct dk_surface depth;
};

/* clears rectangles of the current targets: x, y, width, height in their
pixels */
struct dk_command_clear
{
	struct dk_command_header header;
	uint32_t flags;
	float color[4];
	float depth;
	uint32_t stencil;
	uint32_t rectangle_count;
	uint32_t rectangles[][4];
};

/* shows the back buffer, letterboxed to the display. A screenshot is asked
for by a guest address of width * height * 4 bytes (debug.screenshot_every),
which the host fills with the back buffer's pixels, R G B A in memory, once the
GPU has drawn them (the frame waits for it); 0 asks for none */
struct dk_command_present
{
	struct dk_command_header header;
	struct dk_surface back_buffer;
	uint32_t screenshot;
};

/* ---------- draws (DEKO3D.md, phase 6)

The host keeps what it is told: each of the commands below is written only
when its contents differ from what the host has, and applies at the next draw,
so the draws that share a state share one copy of it. Nothing here is the
guest's own bookkeeping: a target change or a clear, which the host answers
by resetting the viewport and the scissor, makes the host put the state it
holds back before the next draw. */

/* the Xbox's enumerants for these are OpenGL's (d3d8_gl.c hands them to GL as
they are), and cross as they are: blend factors (D3DBLEND_*), compare
functions (D3DCMP_*) and stencil operations (D3DSTENCILOP_*); the host maps
them to deko3d's in a table. The rest are neutral values, below. */

/* DK_CULL_*: which face to discard */
enum
{
	DK_CULL_NONE = 0,
	DK_CULL_FRONT,
	DK_CULL_BACK,
};

/* DK_FILL_*: the polygon mode */
enum
{
	DK_FILL_SOLID = 0,
	DK_FILL_LINE,
	DK_FILL_POINT,
};

/* DK_BLEND_*: the blend equation */
enum
{
	DK_BLEND_ADD = 0,
	DK_BLEND_SUBTRACT,
	DK_BLEND_REVERSE_SUBTRACT,
	DK_BLEND_MIN,
	DK_BLEND_MAX,
};

/* DK_PRIMITIVE_*: what the vertices make */
enum
{
	DK_PRIMITIVE_POINTS = 0,
	DK_PRIMITIVE_LINES,
	DK_PRIMITIVE_LINE_LOOP,
	DK_PRIMITIVE_LINE_STRIP,
	DK_PRIMITIVE_TRIANGLES,
	DK_PRIMITIVE_TRIANGLE_STRIP,
	DK_PRIMITIVE_TRIANGLE_FAN,
	DK_PRIMITIVE_QUADS,
};

/* the fixed-function state of a draw. Rectangles are x, y, width, height in
the bound targets' pixels; the scissor follows the viewport (the NV2A's
register defaults to it, which keeps split screen's windows apart) */
struct dk_draw_state
{
	int32_t viewport[4];
	float depth_range[2];
	int32_t scissor[4];

	uint32_t cull;
	/* 1: counterclockwise polygons are the front, 0: clockwise */
	uint32_t front_face_ccw;
	uint32_t fill_mode;
	/* the polygon offset, as SetRenderState_ZBias computes it */
	uint32_t offset_enable;
	float offset_slope;
	float offset_units;

	uint32_t depth_test;
	uint32_t depth_write;
	uint32_t depth_function;

	uint32_t stencil_test;
	uint32_t stencil_function;
	uint32_t stencil_reference;
	uint32_t stencil_mask;
	uint32_t stencil_write_mask;
	uint32_t stencil_fail;
	uint32_t stencil_depth_fail;
	uint32_t stencil_pass;

	uint32_t blend;
	uint32_t blend_source;
	uint32_t blend_destination;
	uint32_t blend_equation;
	float blend_color[4];

	/* bit 0 red ... bit 3 alpha */
	uint32_t color_mask;
};

struct dk_command_state
{
	struct dk_command_header header;
	struct dk_draw_state state;
};

/* the vertex and pixel shaders, as host_dk_shader_find gave them
(dk_shader_for_draw); the guest writes no draw for a shader that is not ready */
struct dk_command_shaders
{
	struct dk_command_header header;
	uint32_t vertex;
	uint32_t pixel;
};

/* some of the vertex constants, c[first] on, as dk_vertex_constants */
struct dk_command_constants
{
	struct dk_command_header header;
	uint32_t first;
	uint32_t count;
	float data[][4];
};

struct dk_command_vertex_parameters
{
	struct dk_command_header header;
	struct dk_vertex_parameters parameters;
};

struct dk_command_pixel_parameters
{
	struct dk_command_header header;
	struct dk_pixel_parameters parameters;
};

/* how an input register of the vertex shader is fed */
enum
{
	/* not fed by the declaration: the register reads its current value
	(SetVertexData), from the constants below */
	DK_ATTRIBUTE_UNFED = 0,
	/* the same for a NORMPACKED3 register, whose current value is the
	integer zero */
	DK_ATTRIBUTE_UNFED_PACKED,
	DK_ATTRIBUTE_FLOAT1,
	DK_ATTRIBUTE_FLOAT2,
	DK_ATTRIBUTE_FLOAT3,
	DK_ATTRIBUTE_FLOAT4,
	/* D3DCOLOR: four bytes, blue first, normalized */
	DK_ATTRIBUTE_COLOR,
	/* 16-bit integers, converted to floats as they are */
	DK_ATTRIBUTE_SHORT1,
	DK_ATTRIBUTE_SHORT2,
	DK_ATTRIBUTE_SHORT3,
	DK_ATTRIBUTE_SHORT4,
	/* 16-bit integers, normalized to -1 to 1 */
	DK_ATTRIBUTE_NORMSHORT1,
	DK_ATTRIBUTE_NORMSHORT2,
	DK_ATTRIBUTE_NORMSHORT3,
	DK_ATTRIBUTE_NORMSHORT4,
	/* bytes, normalized to 0 to 1 */
	DK_ATTRIBUTE_BYTE1,
	DK_ATTRIBUTE_BYTE2,
	DK_ATTRIBUTE_BYTE3,
	DK_ATTRIBUTE_BYTE4,
	/* NORMPACKED3: one 32-bit integer, which the shader unpacks */
	DK_ATTRIBUTE_PACKED,
};

#define DK_ATTRIBUTE_COUNT 16
#define DK_STREAM_COUNT 16

struct dk_attribute
{
	/* DK_ATTRIBUTE_* */
	uint32_t format;
	/* the Direct3D stream it comes from, and its offset in a vertex */
	uint32_t stream;
	uint32_t offset;
};

/* the declaration and streams of the draws that follow: the sixteen input
registers (the shaders declare all of them) and the stride of each stream */
struct dk_vertex_format
{
	struct dk_attribute attributes[DK_ATTRIBUTE_COUNT];
	uint32_t strides[DK_STREAM_COUNT];
	/* the streams the registers read, bit n for stream n */
	uint32_t stream_mask;
	/* the current values of the unfed registers */
	float constants[DK_ATTRIBUTE_COUNT][4];
};

struct dk_command_vertex_format
{
	struct dk_command_header header;
	struct dk_vertex_format format;
};

/* a draw. The streams follow, one pair (guest address, size in bytes) for
each stream in the format's stream_mask, lowest first: the vertices the draw
reads, from the first one it reads (the host reads them where they are,
cleaning the CPU's cache, or copies them if they are not in one chunk of the
window). If inline_bytes is not 0 the draw's one stream, immediate mode's, is
not at an address but follows the pairs, in the command itself. */
struct dk_command_draw
{
	struct dk_command_header header;
	/* DK_PRIMITIVE_* */
	uint32_t primitive;
	/* vertices, or indices */
	uint32_t count;
	/* 0: not indexed. Else the guest address of the draw's count 16-bit
	indices, and the number added to the vertex each names (the streams start
	at the lowest vertex the draw reads) */
	uint32_t index_address;
	int32_t vertex_offset;
	uint32_t inline_bytes;
	uint32_t stream_count;
	uint32_t streams[][2];
};

#endif
