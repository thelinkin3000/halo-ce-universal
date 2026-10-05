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

#endif
