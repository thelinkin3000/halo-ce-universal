/*
VK_COMMANDS.H

What the Vulkan renderer's guest half (d3d8_vk.c) asks of its host half
(port/android/host/host_vk*.c): a stream of commands, written into guest
memory over a frame and handed to the host in one call (host_vk_submit), so
that a frame costs one crossing from the guest to the host rather than one a
draw. port/android/VULKAN.md ("The command stream") says what each means.

Shared by both halves, which are built for different ABIs (the guest's
pointers are 32 bits, the host's 64): every field is a 32-bit integer or
float, and an address is a guest one, which the host can read directly (the
guest's memory is below 4 GB; it reads what a command names during the
hand-over and never afterwards).

Each command starts with a header naming its type and its size in bytes, the
header included; sizes are multiples of 4.
*/

#ifndef __VK_COMMANDS_H
#define __VK_COMMANDS_H

#include <stdint.h>

#include "vk_shaders.h"

enum
{
	VK_COMMAND_TARGETS = 1,
	VK_COMMAND_CLEAR,
	VK_COMMAND_PRESENT,
	VK_COMMAND_DATA,
	VK_COMMAND_TEST_DRAW,
	VK_COMMAND_PIPELINE,
	VK_COMMAND_DRAW,
};

/* a surface's kind, as the host makes its image */
enum
{
	VK_SURFACE_NONE = 0,
	VK_SURFACE_COLOR, /* 8 bits a channel (every colour target, as d3d8_gl.c makes them) */
	VK_SURFACE_DEPTH, /* 24-bit depth, 8-bit stencil */
};

/* what a clear clears */
enum
{
	VK_CLEAR_RED = 1 << 0,
	VK_CLEAR_GREEN = 1 << 1,
	VK_CLEAR_BLUE = 1 << 2,
	VK_CLEAR_ALPHA = 1 << 3,
	VK_CLEAR_DEPTH = 1 << 4,
	VK_CLEAR_STENCIL = 1 << 5,
};

struct vk_command_header
{
	uint32_t type;
	uint32_t size;
};

/* a render target or depth buffer, known by the physical address of its data
(Data in its D3DSurface), as d3d8_gl.c knows them. width and height are in the
game's units, pixel_width and pixel_height are what it is drawn at (the
screen's targets at the screen's scale: render_target_get in d3d8_gl.c), which
is the size of its image */
struct vk_surface
{
	uint32_t data;
	uint32_t width;
	uint32_t height;
	uint32_t pixel_width;
	uint32_t pixel_height;
	uint32_t kind;
};

/* the surfaces later commands draw into (either can be VK_SURFACE_NONE) */
struct vk_command_targets
{
	struct vk_command_header header;
	struct vk_surface color;
	struct vk_surface depth;
};

/* clears rectangles of the current targets: x, y, width, height in their
pixels, row 0 at the top, already clipped to the viewport and scaled by the
guest as d3d8_gl.c's Clear does */
struct vk_command_clear
{
	struct vk_command_header header;
	uint32_t flags;
	float color[4];
	float depth;
	uint32_t stencil;
	uint32_t rectangle_count;
	uint32_t rectangles[][4];
};

/* shows the back buffer, letterboxed to the display */
struct vk_command_present
{
	struct vk_command_header header;
	struct vk_surface back_buffer;
};

/* ---------- data (phase 3)

What a draw reads is copied into the stream by the guest at the draw
(vk_data_put in d3d8_vk.c), and by the host into an upload ring at the hand-over;
later commands name it by the id vk_data_put returned and an offset in it. An id is
the frame's running number of the data put, from 1, and is reset at each PRESENT
(and at a TEST_DRAW that says so) */

/* a place in the data of this frame */
struct vk_data_ref
{
	uint32_t id;
	uint32_t offset;
};

/* part of the data of an id: parts of one id come in order (part_offset 0, then the
next, until total_size), possibly in different hand-overs, and the host places all of
an id's parts contiguously. Followed by part_size bytes, the record padded to a
multiple of 4 */
struct vk_command_data
{
	struct vk_command_header header;
	uint32_t id;
	uint32_t part_offset;
	uint32_t total_size;
	uint32_t part_size;
	uint32_t payload[];
};

/* the self-test's vertex: a position and a colour */
struct vk_test_vertex
{
	float position[3];
	float color[4];
};

/* only for debug.vk_self_test: draws the three vertices (struct vk_test_vertex) at offset
in the data of id into a 16x16 target of the host's own, reads it back and logs the
colour seen against the one in expected (R, G, B, 0 to 255). With last set, the frame's
data is let go afterwards, as PRESENT does */
struct vk_command_test_draw
{
	struct vk_command_header header;
	struct vk_data_ref data;
	uint32_t expected[3];
	uint32_t last;
};

/* the pipeline a draw asks for (phase 5): the handles host_vk_shader_find gave (0: not ready yet, which the host counts
as a draw skipped for a shader) and the state it is made with. A pipeline not made yet is queued for the compile
thread and the draw would be skipped; phase 6's draw binds the pipeline when it is ready */
struct vk_command_pipeline
{
	struct vk_command_header header;
	uint32_t vertex_shader;
	uint32_t pixel_shader;
	struct vk_pipeline_state state;
};

/* ---------- draws (phase 6)

Each draw is one self-contained record: the host keeps no draw state between draws apart from what it has bound.
Everything that is an enumeration is the Vulkan enumerant's number, which the guest writes without Vulkan's headers. */

#define VK_DRAW_TEXTURE_STAGES 4
#define VK_DRAW_STREAMS VK_PIPELINE_VERTEX_BINDINGS

/* a stage's texture: none (the host binds a dummy), an image the guest made (id from VK_COMMAND_TEXTURE), or the
render target last bound at an address (data) */
enum
{
	VK_TEXTURE_NONE = 0,
	VK_TEXTURE_IMAGE,
	VK_TEXTURE_TARGET,
};

/* a sampler's state: Vulkan's filter (0 nearest, 1 linear), mipmap mode (0 none, 1 nearest, 2 linear) and address mode
numbers; the border colour is a Direct3D colour (the host picks the nearest of Vulkan's fixed ones, or a custom one) */
struct vk_sampler_state
{
	uint32_t min_filter, mag_filter, mip_mode;
	uint32_t address[3];
	uint32_t border_color;
	float lod_bias, min_lod, anisotropy;
};

struct vk_draw_texture
{
	uint32_t kind;
	/* VK_TEXTURE_IMAGE: the image's id; VK_TEXTURE_TARGET: the target's physical address */
	uint32_t id;
	/* the pixel shader's sampler: 1 2D, 2 3D, 3 cube (the dummy has this type when kind is NONE) */
	uint32_t sampler_type;
	struct vk_sampler_state sampler;
};

struct vk_command_draw
{
	struct vk_command_header header;
	/* the handles host_vk_shader_find gave (both made: a draw with either 0 is not sent) */
	uint32_t vertex_shader, pixel_shader;
	struct vk_pipeline_state state;
	/* x, y, width, height in the target's pixels (row 0 at the top), minimum and maximum depth */
	float viewport[6];
	int32_t scissor[4];
	float depth_bias_constant, depth_bias_slope;
	float blend_constants[4];
	uint32_t stencil_compare_mask, stencil_write_mask, stencil_reference;
	struct vk_data_ref vertex_constants, vertex_parameters, pixel_parameters;
	/* one for each of state.binding_count bindings */
	struct vk_data_ref vertex_buffers[VK_DRAW_STREAMS];
	struct vk_draw_texture textures[VK_DRAW_TEXTURE_STAGES];
	/* indexed: 16-bit indices, count of them, added to each index (negative: the streams start at the lowest vertex) */
	uint32_t indexed;
	struct vk_data_ref index_data;
	int32_t vertex_offset;
	uint32_t count;
	/* the visibility test the draw is in (1 + the game's slot), 0 for none (phase 6, step 6) */
	uint32_t visibility;
};

/* Vulkan's formats, the numbers the vertex input uses */
#define VK_VERTEX_FORMAT_R8_UNORM 9
#define VK_VERTEX_FORMAT_R8G8_UNORM 16
#define VK_VERTEX_FORMAT_R8G8B8_UNORM 23
#define VK_VERTEX_FORMAT_R8G8B8A8_UNORM 37
#define VK_VERTEX_FORMAT_B8G8R8A8_UNORM 44
#define VK_VERTEX_FORMAT_R16_SNORM 71
#define VK_VERTEX_FORMAT_R16_SSCALED 73
#define VK_VERTEX_FORMAT_R16G16_SNORM 78
#define VK_VERTEX_FORMAT_R16G16_SSCALED 80
#define VK_VERTEX_FORMAT_R16G16B16_SNORM 85
#define VK_VERTEX_FORMAT_R16G16B16_SSCALED 87
#define VK_VERTEX_FORMAT_R16G16B16A16_SNORM 92
#define VK_VERTEX_FORMAT_R16G16B16A16_SSCALED 94
#define VK_VERTEX_FORMAT_R32_UINT 98
#define VK_VERTEX_FORMAT_R32_SFLOAT 100
#define VK_VERTEX_FORMAT_R32G32_SFLOAT 103
#define VK_VERTEX_FORMAT_R32G32B32_SFLOAT 106
#define VK_VERTEX_FORMAT_R32G32B32A32_SFLOAT 109

#endif
