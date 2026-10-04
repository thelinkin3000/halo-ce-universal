/*
HOST_DK.C

The host half of the deko3d renderer (port/switch/DEKO3D.md): it runs the
commands the guest's half (port/switch/guest/d3d8_dk.c) writes over a frame
(port/switch/guest/dk_commands.h) and presents the frame.

It keeps:
- the device and one queue, and a ring of FRAMES slices of command memory,
  each with a fence, so a slice is reused only once the GPU is done with the
  frame recorded in it;
- the render targets and depth buffers, as images found by the guest
  address of their data, as d3d8_gl.c finds its GL textures;
- the swapchain, on the console's default window, which the host leaves
  without an EGL surface under deko3d (host_sdl2.c).

deko3d ends the program when it cannot make something it is asked for
(DEKO3D.md), so what is asked of it here is what the console is known to
give.
*/

#include "host.h"
#include "../guest/dk_commands.h"

#include <deko3d.h>
#include <switch.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FRAMES 3
#define COMMAND_MEMORY_SIZE (4 * 1024 * 1024)
#define IMAGE_BLOCK_SIZE (32 * 1024 * 1024)
#define IMAGE_BLOCK_LIMIT 16
#define TARGET_LIMIT 256
#define SCREEN_WIDTH 1280
#define SCREEN_HEIGHT 720

int host_dk_presenting;

struct image_block
{
	DkMemBlock memory;
	uint32_t used;
};

/* a render target or depth buffer */
struct target
{
	struct dk_surface surface;
	DkImage image;
};

static struct
{
	int ready;
	DkDevice device;
	DkQueue queue;
	DkCmdBuf commands;
	DkMemBlock command_memory;
	DkFence fences[FRAMES];
	int fence_pending[FRAMES];
	int frame;

	DkMemBlock screen_memory;
	DkImage screen_images[2];
	DkSwapchain swapchain;

	struct image_block image_blocks[IMAGE_BLOCK_LIMIT];
	int image_block_count;
	struct target targets[TARGET_LIMIT];
	int target_count;

	/* the targets bound now (NULL: none) */
	struct target *color, *depth;
	unsigned long frames_presented;
} dk;

static void debug_callback(void *user, const char *context, DkResult result, const char *message)
{
	(void)user;
	host_logf(HOST_LOG_ERROR, "deko3d: %s: result %d: %s", context ? context : "?", (int)result,
		message ? message : "");
}

static void command_memory_exhausted(void *user, DkCmdBuf commands, size_t needed)
{
	(void)user;
	(void)commands;
	host_fatal("deko3d: a frame needs more than %u bytes of commands (%zu more asked for)",
		(unsigned)COMMAND_MEMORY_SIZE, needed);
}

static DkMemBlock memory_block(uint32_t size, uint32_t flags)
{
	DkMemBlockMaker maker;

	dkMemBlockMakerDefaults(&maker, dk.device, (size + DK_MEMBLOCK_ALIGNMENT - 1) & ~(DK_MEMBLOCK_ALIGNMENT - 1));
	maker.flags = flags;
	return dkMemBlockCreate(&maker);
}

/* room for an image: offset in a block of image memory */
static int image_memory(const DkImageLayout *layout, DkMemBlock *block, uint32_t *offset)
{
	uint32_t size = (uint32_t)dkImageLayoutGetSize(layout);
	uint32_t alignment = dkImageLayoutGetAlignment(layout);
	int index;

	for (index = 0; index < dk.image_block_count; index++)
	{
		struct image_block *candidate = &dk.image_blocks[index];
		uint32_t start = (candidate->used + alignment - 1) & ~(alignment - 1);

		if (start + size <= IMAGE_BLOCK_SIZE)
		{
			candidate->used = start + size;
			*block = candidate->memory;
			*offset = start;
			return 1;
		}
	}
	if (dk.image_block_count == IMAGE_BLOCK_LIMIT || size > IMAGE_BLOCK_SIZE)
		return 0;
	dk.image_blocks[dk.image_block_count].memory = memory_block(IMAGE_BLOCK_SIZE,
		DkMemBlockFlags_GpuCached | DkMemBlockFlags_Image);
	dk.image_blocks[dk.image_block_count].used = size;
	*block = dk.image_blocks[dk.image_block_count].memory;
	*offset = 0;
	dk.image_block_count++;
	return 1;
}

static void layout_make(DkImageLayout *layout, DkImageFormat format, uint32_t flags, uint32_t width, uint32_t height)
{
	DkImageLayoutMaker maker;

	dkImageLayoutMakerDefaults(&maker, dk.device);
	maker.flags = flags;
	maker.format = format;
	maker.dimensions[0] = width;
	maker.dimensions[1] = height;
	dkImageLayoutInitialize(layout, &maker);
}

/* the target for a surface, made the first time it is drawn into */
static struct target *target_get(const struct dk_surface *surface)
{
	struct target *target;
	DkImageLayout layout;
	DkMemBlock block;
	uint32_t offset;
	int index;

	if (surface->kind == DK_SURFACE_NONE || !surface->width || !surface->height)
		return NULL;
	for (index = 0; index < dk.target_count; index++)
	{
		target = &dk.targets[index];
		if (!memcmp(&target->surface, surface, sizeof(*surface)))
			return target;
	}
	if (dk.target_count == TARGET_LIMIT)
	{
		host_logf(HOST_LOG_ERROR, "deko3d: more than %d render targets; %08x is not drawn into", TARGET_LIMIT,
			(unsigned)surface->data);
		return NULL;
	}
	layout_make(&layout, surface->kind == DK_SURFACE_DEPTH ? DkImageFormat_Z24S8 : DkImageFormat_RGBA8_Unorm,
		DkImageFlags_UsageRender | DkImageFlags_Usage2DEngine | DkImageFlags_HwCompression, surface->width,
		surface->height);
	if (!image_memory(&layout, &block, &offset))
	{
		host_logf(HOST_LOG_ERROR, "deko3d: no image memory for the %ux%u target at %08x", (unsigned)surface->width,
			(unsigned)surface->height, (unsigned)surface->data);
		return NULL;
	}
	target = &dk.targets[dk.target_count++];
	target->surface = *surface;
	dkImageInitialize(&target->image, &layout, block, offset);
	host_logf(HOST_LOG_INFO, "deko3d: %s target %08x, %ux%u", surface->kind == DK_SURFACE_DEPTH ? "depth" : "color",
		(unsigned)surface->data, (unsigned)surface->width, (unsigned)surface->height);
	return target;
}

/* starts recording into this frame's slice of command memory, once the GPU
is done with the frame last recorded there */
static void frame_begin(void)
{
	if (dk.fence_pending[dk.frame])
	{
		dkFenceWait(&dk.fences[dk.frame], -1);
		dk.fence_pending[dk.frame] = 0;
	}
	dkCmdBufClear(dk.commands);
	dkCmdBufAddMemory(dk.commands, dk.command_memory, (uint32_t)dk.frame * COMMAND_MEMORY_SIZE, COMMAND_MEMORY_SIZE);
	dk.color = dk.depth = NULL;
}

static int initialize(void)
{
	DkDeviceMaker device_maker;
	DkQueueMaker queue_maker;
	DkCmdBufMaker command_maker;
	DkSwapchainMaker swapchain_maker;
	DkImageLayout layout;
	DkImage const *screen_images[2];
	uint32_t image_size;
	int index;

	dkDeviceMakerDefaults(&device_maker);
	device_maker.cbDebug = debug_callback;
	/* Direct3D's conventions: depth from 0 to 1, the origin top left */
	device_maker.flags = DkDeviceFlags_DepthZeroToOne | DkDeviceFlags_OriginUpperLeft;
	dk.device = dkDeviceCreate(&device_maker);
	dkQueueMakerDefaults(&queue_maker, dk.device);
	queue_maker.flags = DkQueueFlags_Graphics;
	dk.queue = dkQueueCreate(&queue_maker);
	dk.command_memory = memory_block(FRAMES * COMMAND_MEMORY_SIZE, DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached);
	dkCmdBufMakerDefaults(&command_maker, dk.device);
	command_maker.cbAddMem = command_memory_exhausted;
	dk.commands = dkCmdBufCreate(&command_maker);

	layout_make(&layout, DkImageFormat_RGBA8_Unorm,
		DkImageFlags_UsageRender | DkImageFlags_UsagePresent | DkImageFlags_Usage2DEngine | DkImageFlags_HwCompression,
		SCREEN_WIDTH, SCREEN_HEIGHT);
	image_size = (uint32_t)((dkImageLayoutGetSize(&layout) + dkImageLayoutGetAlignment(&layout) - 1) &
		~(uint64_t)(dkImageLayoutGetAlignment(&layout) - 1));
	dk.screen_memory = memory_block(2 * image_size, DkMemBlockFlags_GpuCached | DkMemBlockFlags_Image);
	for (index = 0; index < 2; index++)
	{
		dkImageInitialize(&dk.screen_images[index], &layout, dk.screen_memory, (uint32_t)index * image_size);
		screen_images[index] = &dk.screen_images[index];
	}
	dkSwapchainMakerDefaults(&swapchain_maker, dk.device, nwindowGetDefault(), screen_images, 2);
	dk.swapchain = dkSwapchainCreate(&swapchain_maker);
	host_logf(HOST_LOG_INFO, "deko3d: device ready, presenting at %dx%d", SCREEN_WIDTH, SCREEN_HEIGHT);
	dk.ready = 1;
	frame_begin();
	return 1;
}

static void targets_bind(const struct dk_command_targets *command)
{
	DkImageView color_view, depth_view;
	DkImageView const *colors[1] = { &color_view };
	struct target *size_from;

	dk.color = target_get(&command->color);
	dk.depth = target_get(&command->depth);
	if (dk.color)
		dkImageViewDefaults(&color_view, &dk.color->image);
	if (dk.depth)
		dkImageViewDefaults(&depth_view, &dk.depth->image);
	dkCmdBufBindRenderTargets(dk.commands, colors, dk.color ? 1 : 0, dk.depth ? &depth_view : NULL);
	/* the queue keeps its viewport and scissor between commands: start
	each binding with the whole target */
	size_from = dk.color ? dk.color : dk.depth;
	if (size_from)
	{
		DkViewport viewport = { 0.0f, 0.0f, (float)size_from->surface.width, (float)size_from->surface.height,
			0.0f, 1.0f };
		DkScissor scissor = { 0, 0, size_from->surface.width, size_from->surface.height };

		dkCmdBufSetViewports(dk.commands, 0, &viewport, 1);
		dkCmdBufSetScissors(dk.commands, 0, &scissor, 1);
	}
}

static void clear(const struct dk_command_clear *command)
{
	uint32_t mask = ((command->flags & DK_CLEAR_RED) ? DkColorMask_R : 0) |
		((command->flags & DK_CLEAR_GREEN) ? DkColorMask_G : 0) |
		((command->flags & DK_CLEAR_BLUE) ? DkColorMask_B : 0) |
		((command->flags & DK_CLEAR_ALPHA) ? DkColorMask_A : 0);
	int depth = dk.depth && (command->flags & DK_CLEAR_DEPTH);
	int stencil = dk.depth && (command->flags & DK_CLEAR_STENCIL);
	uint32_t index;

	if (!dk.color)
		mask = 0;
	if (!mask && !depth && !stencil)
		return;
	for (index = 0; index < command->rectangle_count; index++)
	{
		const uint32_t *rectangle = command->rectangles[index];
		DkScissor scissor = { rectangle[0], rectangle[1], rectangle[2], rectangle[3] };

		dkCmdBufSetScissors(dk.commands, 0, &scissor, 1);
		if (mask)
			dkCmdBufClearColorFloat(dk.commands, 0, mask, command->color[0], command->color[1], command->color[2],
				command->color[3]);
		if (depth || stencil)
			dkCmdBufClearDepthStencil(dk.commands, depth, command->depth, stencil ? 0xff : 0,
				(uint8_t)command->stencil);
	}
}

static void present(const struct dk_command_present *command)
{
	struct target *back_buffer = target_get(&command->back_buffer);
	int slot = dkQueueAcquireImage(dk.queue, dk.swapchain);
	DkImageView screen_view;
	DkImageView const *screen_views[1] = { &screen_view };
	DkScissor scissor = { 0, 0, SCREEN_WIDTH, SCREEN_HEIGHT };
	DkViewport viewport = { 0.0f, 0.0f, (float)SCREEN_WIDTH, (float)SCREEN_HEIGHT, 0.0f, 1.0f };

	dkImageViewDefaults(&screen_view, &dk.screen_images[slot]);
	dkCmdBufBindRenderTargets(dk.commands, screen_views, 1, NULL);
	dkCmdBufSetViewports(dk.commands, 0, &viewport, 1);
	dkCmdBufSetScissors(dk.commands, 0, &scissor, 1);
	dkCmdBufClearColorFloat(dk.commands, 0, DkColorMask_RGBA, 0.0f, 0.0f, 0.0f, 1.0f);
	if (back_buffer)
	{
		/* letterboxed to the back buffer's shape */
		uint32_t width = SCREEN_WIDTH, height = SCREEN_WIDTH * back_buffer->surface.height / back_buffer->surface.width;
		DkImageView source;
		DkImageRect from = { 0, 0, 0, back_buffer->surface.width, back_buffer->surface.height, 1 };
		DkImageRect to;

		if (height > SCREEN_HEIGHT)
		{
			height = SCREEN_HEIGHT;
			width = SCREEN_HEIGHT * back_buffer->surface.width / back_buffer->surface.height;
		}
		to.x = (SCREEN_WIDTH - width) / 2;
		to.y = (SCREEN_HEIGHT - height) / 2;
		to.z = 0;
		to.width = width;
		to.height = height;
		to.depth = 1;
		dkImageViewDefaults(&source, &back_buffer->image);
		dkCmdBufBarrier(dk.commands, DkBarrier_Fragments, 0);
		dkCmdBufBlitImage(dk.commands, &source, &from, &screen_view, &to, DkBlitFlag_FilterLinear, 0);
	}
	dkQueueSubmitCommands(dk.queue, dkCmdBufFinishList(dk.commands));
	dkQueueSignalFence(dk.queue, &dk.fences[dk.frame], true);
	dk.fence_pending[dk.frame] = 1;
	dkQueuePresentImage(dk.queue, dk.swapchain, slot);
	host_dk_presenting = 1;
	if (++dk.frames_presented == 1)
		host_logf(HOST_LOG_INFO, "deko3d: first frame presented");
	dk.frame = (dk.frame + 1) % FRAMES;
	frame_begin();
}

/* runs size bytes of commands at commands (a guest address) */
void host_dk_submit(uint32_t commands, uint32_t size)
{
	const unsigned char *at = (const unsigned char *)(uintptr_t)commands;
	const unsigned char *end = at + size;
	int presented = 0;

	if (!dk.ready && !initialize())
		return;
	while (at + sizeof(struct dk_command_header) <= end)
	{
		const struct dk_command_header *header = (const struct dk_command_header *)at;

		if (header->size < sizeof(*header) || at + header->size > end)
		{
			host_logf(HOST_LOG_ERROR, "deko3d: a command of %u bytes runs past the stream's end; the rest is dropped",
				(unsigned)header->size);
			break;
		}
		switch (header->type)
		{
		case DK_COMMAND_TARGETS:
			targets_bind((const struct dk_command_targets *)header);
			break;
		case DK_COMMAND_CLEAR:
			clear((const struct dk_command_clear *)header);
			break;
		case DK_COMMAND_PRESENT:
			present((const struct dk_command_present *)header);
			presented = 1;
			break;
		default:
			host_logf(HOST_LOG_ERROR, "deko3d: unknown command %u", (unsigned)header->type);
			break;
		}
		at += header->size;
	}
	/* a stream handed over before the frame's end (it filled up) is run now,
	and the frame goes on in the same slice */
	if (!presented)
		dkQueueSubmitCommands(dk.queue, dkCmdBufFinishList(dk.commands));
}
