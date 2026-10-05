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

enum
{
	VK_COMMAND_TARGETS = 1,
	VK_COMMAND_CLEAR,
	VK_COMMAND_PRESENT,
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

#endif
