/*
DK_TEXTURES.H

The deko3d renderer's texture cache (xbox_textures_dk.c, a copy of
port/linux/src/xbox_textures.c) and what it needs of the device
(d3d8_dk.c), whose command stream it writes into.
*/

#ifndef __DK_TEXTURES_H
#define __DK_TEXTURES_H

#include "xgpu.h"

/* The image number of the Xbox texture whose header is resource (and the
palette its stages' palettized textures use), sending its texels to the host
if it is new or has been written since; 0 if there is none to draw. *kind
receives DK_TEXTURE_2D, _3D or _CUBE, and description what xbox_textures
knows of it. */
uint32_t dk_texture_get(const DWORD *resource, const D3DCOLOR *palette, int *kind,
	struct xgpu_texture_description *description);

/* d3d8_dk.c: room for a command in the stream (dk_commands.h), its header
filled in */
void *dk_stream_command(uint32_t type, unsigned long size);
/* d3d8_dk.c: buffer is the guest's own copy of data a command names; it is
freed once the stream has been handed to the host, which reads it then. A
great deal of it waiting hands the stream over. Call after the command is
written: it may do that. */
void dk_stream_free_after_handover(void *buffer, unsigned long size);

#endif
