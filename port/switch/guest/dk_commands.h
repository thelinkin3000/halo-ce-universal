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

enum
{
	DK_COMMAND_TARGETS = 1,
	DK_COMMAND_CLEAR,
	DK_COMMAND_PRESENT,
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

/* shows the back buffer, letterboxed to the display */
struct dk_command_present
{
	struct dk_command_header header;
	struct dk_surface back_buffer;
};

#endif
