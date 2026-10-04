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
- a deko3d memory block on each committed 16 MB chunk of the game's memory
  window (host_memory.c says which; DEKO3D.md, phase 3), so the GPU reads
  the game's vertex and index data where the game keeps it;
- a ring slice of upload memory for vertex and index data that is not in
  the window (CreateIndexBuffer's, immediate mode's);
- the swapchain, on the console's default window, which the host leaves
  without an EGL surface under deko3d (host_sdl2.c).

The GPU reads the window a moment after the game wrote it, and the CPU's
writes may still be in its cache: each range a draw reads is cleaned from
the CPU's cache as the draw is recorded (window_read), and every submission
ends with a queue flush, after which deko3d invalidates the GPU's own caches
(its Queue::postSubmitFlush), so the next submission reads memory as the CPU
left it.

Every call of host_dk_submit is one submission, numbered as the guest
numbers it, and ends with a fence. The guest records in each resource the
submission that last read it (its Lock field, as the Xbox's runtime did) and
asks host_dk_retired which submissions the GPU has finished, to answer
IsBusy and wait in its locks.

deko3d ends the program when it cannot make something it is asked for
(DEKO3D.md), so what is asked of it here is what the console is known to
give.
*/

#include "host.h"
#include "../guest/dk_commands.h"

#include <deko3d.h>
#include <switch.h>
#include <pthread.h>
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

/* the window is made real a chunk at a time (host_memory.c);
HALO_GUEST_WINDOW_SIZE (128 MB) is 8 of them */
#define WINDOW_CHUNK_SIZE (16 * 1024 * 1024)
#define WINDOW_CHUNKS (HALO_GUEST_WINDOW_SIZE / WINDOW_CHUNK_SIZE)

/* what a frame can upload (CreateIndexBuffer's index data, immediate-mode
vertices, quad lists' indices); one slice per ring frame */
#define UPLOAD_SLICE_SIZE (4 * 1024 * 1024)
#define UPLOAD_ALIGNMENT 256

/* the window ranges a submission's draws read, cleaned from the CPU's cache
once each (window_read); past this many, a range is cleaned but not kept */
#define CLEANED_LIMIT 256

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

/* one committed 16 MB chunk of the window, GPU-mapped on the game thread
once deko3d is up; block NULL and gpu 0 until then */
struct window_chunk
{
	uint64_t address;
	DkMemBlock block;
	DkGpuAddr gpu;
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

	/* the window's chunks, in commit order */
	struct window_chunk window_chunks[WINDOW_CHUNKS];
	int window_chunk_count;
	/* deko3d ends the program on a memory block it cannot make (DEKO3D.md),
	so each chunk is tried as the probe tries it first, into a private
	address space made for the asking */
	int nv_ready;
	NvAddressSpace test_address_space;

	/* a frame's slice of upload memory and its ends; CpuUncached, so the
	CPU's copies into it need no flushing */
	DkMemBlock upload_memory;
	void *upload_cpu;
	DkGpuAddr upload_gpu;
	uint32_t upload_used;
	int upload_overflowed;

	/* the window ranges cleaned for the submission being recorded */
	struct
	{
		uint64_t address, size;
	} cleaned[CLEANED_LIMIT];
	int cleaned_count;

	/* submissions, numbered as the guest numbers them: the one being
	recorded, the last one ended with a fence, the highest the GPU has
	finished, and each fence's */
	uint32_t serial;
	uint32_t serial_fenced;
	uint32_t serial_retired;
	uint32_t fence_serial[FRAMES];
	/* commands recorded since the last submission */
	int recorded;

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

static DkMemBlock memory_block(uint32_t size, uint32_t flags, void *storage)
{
	DkMemBlockMaker maker;

	dkMemBlockMakerDefaults(&maker, dk.device, (size + DK_MEMBLOCK_ALIGNMENT - 1) & ~(DK_MEMBLOCK_ALIGNMENT - 1));
	maker.flags = flags;
	maker.storage = storage;
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
		DkMemBlockFlags_GpuCached | DkMemBlockFlags_Image, NULL);
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
static pthread_mutex_t dk_lock = PTHREAD_MUTEX_INITIALIZER;

static void frame_begin(void)
{
	/* (under the lock: the guest's other threads read the fences through
	host_dk_retired) */
	pthread_mutex_lock(&dk_lock);
	if (dk.fence_pending[dk.frame])
	{
		dkFenceWait(&dk.fences[dk.frame], -1);
		dk.fence_pending[dk.frame] = 0;
		if (dk.fence_serial[dk.frame] > dk.serial_retired)
			dk.serial_retired = dk.fence_serial[dk.frame];
	}
	pthread_mutex_unlock(&dk_lock);
	dkCmdBufClear(dk.commands);
	dkCmdBufAddMemory(dk.commands, dk.command_memory, (uint32_t)dk.frame * COMMAND_MEMORY_SIZE, COMMAND_MEMORY_SIZE);
	/* the upload slice is reused the same way, behind the same fence */
	dk.upload_used = 0;
	dk.color = dk.depth = NULL;
}

/* ---------- the game's memory, where the game keeps it (DEKO3D.md, phase 3)

The chunk table is filled by whichever guest thread commits a chunk
(host_memory.c), so it is behind dk_lock; the chunks' memory blocks are made
on the game thread only (chunks_map), as the rest of deko3d's objects are. */

/* the memory block for a window chunk: the chunk's storage, GPU-cached, CPU
writes cleaned before the draws that read them (window_read). Tried first
the way the probe tries it, because deko3d ends the program when it cannot
make one. */
static void chunk_block_make(struct window_chunk *chunk)
{
	NvMap map;
	iova_t address = 0;
	Result result;

	result = nvMapCreate(&map, (void *)(uintptr_t)chunk->address, WINDOW_CHUNK_SIZE, 0x1000, NvKind_Pitch, true);
	if (R_FAILED(result))
	{
		host_logf(HOST_LOG_ERROR, "deko3d: nvmap refuses the window chunk at %p: 0x%08x (module %u, "
			"description %u)", (void *)(uintptr_t)chunk->address, (unsigned)result, (unsigned)R_MODULE(result),
			(unsigned)R_DESCRIPTION(result));
		return;
	}
	if (!dk.nv_ready)
	{
		host_logf(HOST_LOG_ERROR, "deko3d: no test address space; the window chunk at %p stays unmapped",
			(void *)(uintptr_t)chunk->address);
		nvMapClose(&map);
		return;
	}
	result = nvAddressSpaceMap(&dk.test_address_space, nvMapGetHandle(&map), true, NvKind_Pitch, &address);
	if (R_FAILED(result))
	{
		host_logf(HOST_LOG_ERROR, "deko3d: the GPU refuses the window chunk at %p: 0x%08x (module %u, "
			"description %u)", (void *)(uintptr_t)chunk->address, (unsigned)result, (unsigned)R_MODULE(result),
			(unsigned)R_DESCRIPTION(result));
		nvMapClose(&map);
		return;
	}
	nvAddressSpaceUnmap(&dk.test_address_space, address);
	nvMapClose(&map);
	chunk->block = memory_block(WINDOW_CHUNK_SIZE, DkMemBlockFlags_CpuCached | DkMemBlockFlags_GpuCached,
		(void *)(uintptr_t)chunk->address);
	if (!chunk->block)
	{
		host_logf(HOST_LOG_ERROR, "deko3d: no memory block for the window chunk at %p",
			(void *)(uintptr_t)chunk->address);
		return;
	}
	chunk->gpu = dkMemBlockGetGpuAddr(chunk->block);
	host_logf(HOST_LOG_INFO, "deko3d: window chunk %p is GPU address %010llx", (void *)(uintptr_t)chunk->address,
		(unsigned long long)chunk->gpu);
}

/* host_memory.c says a chunk of the window has been committed: noted here,
mapped by the game thread at its next submission (chunks_map) */
void host_dk_window_chunk_committed(uint64_t address, uint64_t size)
{
	struct window_chunk *chunk;

	(void)size; /* always WINDOW_CHUNK_SIZE */
	if (!host_renderer_deko3d)
		return;
	pthread_mutex_lock(&dk_lock);
	if (dk.window_chunk_count == WINDOW_CHUNKS)
	{
		host_logf(HOST_LOG_ERROR, "deko3d: more than %u window chunks committed; %p is not GPU-mapped",
			(unsigned)WINDOW_CHUNKS, (void *)(uintptr_t)address);
		pthread_mutex_unlock(&dk_lock);
		return;
	}
	chunk = &dk.window_chunks[dk.window_chunk_count++];
	chunk->address = address;
	chunk->block = NULL;
	chunk->gpu = 0;
	pthread_mutex_unlock(&dk_lock);
}

/* makes the memory blocks of the chunks committed since the last time; on
the game thread. A chunk the console refuses is tried no more. */
static void chunks_map(void)
{
	static int tried;
	int count, index;

	pthread_mutex_lock(&dk_lock);
	count = dk.window_chunk_count;
	pthread_mutex_unlock(&dk_lock);
	/* (entries below count are not touched by other threads again) */
	for (index = tried; index < count; index++)
		chunk_block_make(&dk.window_chunks[index]);
	tried = count;
}

/* The GPU address of [address, address + size) of the window, for a draw
that reads it, and the range cleaned from the CPU's cache - each range once
a submission. 0 if the range is not inside one mapped chunk: two chunks'
GPU addresses are not contiguous, so a range crossing from one into the
next goes through the upload buffer (upload_copy), as data outside the
window does. On the game thread. */
static DkGpuAddr window_read(uint64_t address, uint64_t size)
{
	uint64_t first = address & ~(uint64_t)63, last = (address + size + 63) & ~(uint64_t)63;
	DkGpuAddr gpu = 0;
	int index;

	for (index = 0; index < dk.window_chunk_count; index++)
	{
		struct window_chunk *chunk = &dk.window_chunks[index];

		if (chunk->block && address >= chunk->address && address + size <= chunk->address + WINDOW_CHUNK_SIZE)
		{
			gpu = chunk->gpu + (address - chunk->address);
			break;
		}
	}
	if (!gpu || !size)
		return gpu;
	for (index = 0; index < dk.cleaned_count; index++)
	{
		if (first >= dk.cleaned[index].address && last <= dk.cleaned[index].address + dk.cleaned[index].size)
			return gpu;
	}
	/* cleaned, not invalidated: the CPU keeps reading what it wrote */
	armDCacheClean((void *)(uintptr_t)first, last - first);
	if (dk.cleaned_count < CLEANED_LIMIT)
	{
		dk.cleaned[dk.cleaned_count].address = first;
		dk.cleaned[dk.cleaned_count].size = last - first;
		dk.cleaned_count++;
	}
	return gpu;
}

/* the per-frame upload buffer, for vertex and index data outside the
window; the draw is skipped if a frame's copies do not fit, which a frame's
size is not expected to reach */
static DkGpuAddr upload_copy(const void *data, uint32_t size)
{
	uint32_t offset = (dk.upload_used + UPLOAD_ALIGNMENT - 1) & ~(UPLOAD_ALIGNMENT - 1);

	if (!dk.upload_memory)
		return 0;
	if (offset + size > UPLOAD_SLICE_SIZE)
	{
		if (!dk.upload_overflowed)
		{
			host_logf(HOST_LOG_ERROR, "deko3d: a frame needs more than %u bytes of uploads; some draws will "
				"be missing", (unsigned)UPLOAD_SLICE_SIZE);
			dk.upload_overflowed = 1;
		}
		return 0;
	}
	memcpy((char *)dk.upload_cpu + dk.frame * UPLOAD_SLICE_SIZE + offset, data, size);
	dk.upload_used = offset + size;
	return dk.upload_gpu + dk.frame * UPLOAD_SLICE_SIZE + offset;
}

/* ---------- submissions and which of them the GPU has finished */

/* submits what has been recorded and ends the submission with a fence. The
fence's flush matters as much as the fence: after a flush deko3d
invalidates the GPU's caches, so the next submission reads what the CPU has
written (and cleaned) since. */
static void commands_submit(void)
{
	dkQueueSubmitCommands(dk.queue, dkCmdBufFinishList(dk.commands));
	pthread_mutex_lock(&dk_lock);
	dkQueueSignalFence(dk.queue, &dk.fences[dk.frame], true);
	dk.fence_pending[dk.frame] = 1;
	dk.fence_serial[dk.frame] = dk.serial;
	dk.serial_fenced = dk.serial;
	pthread_mutex_unlock(&dk_lock);
	dk.cleaned_count = 0;
	dk.recorded = 0;
}

/* (under dk_lock) */
static void fences_poll(void)
{
	int index;

	for (index = 0; index < FRAMES; index++)
	{
		if (dk.fence_pending[index] && dkFenceWait(&dk.fences[index], 0) == DkResult_Success)
		{
			dk.fence_pending[index] = 0;
			if (dk.fence_serial[index] > dk.serial_retired)
				dk.serial_retired = dk.fence_serial[index];
		}
	}
}

/* the highest submission the GPU has finished, every one before it finished
too (the queue runs them in order). The deko3d renderer's guest half
answers IsBusy and waits in its locks against it (DEKO3D.md, phase 3); any
guest thread may ask. */
uint32_t host_dk_retired(void)
{
	uint32_t retired;

	pthread_mutex_lock(&dk_lock);
	if (dk.ready)
		fences_poll();
	retired = dk.serial_retired;
	pthread_mutex_unlock(&dk_lock);
	return retired;
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
	dk.command_memory = memory_block(FRAMES * COMMAND_MEMORY_SIZE, DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached, NULL);
	dkCmdBufMakerDefaults(&command_maker, dk.device);
	command_maker.cbAddMem = command_memory_exhausted;
	dk.commands = dkCmdBufCreate(&command_maker);

	layout_make(&layout, DkImageFormat_RGBA8_Unorm,
		DkImageFlags_UsageRender | DkImageFlags_UsagePresent | DkImageFlags_Usage2DEngine | DkImageFlags_HwCompression,
		SCREEN_WIDTH, SCREEN_HEIGHT);
	image_size = (uint32_t)((dkImageLayoutGetSize(&layout) + dkImageLayoutGetAlignment(&layout) - 1) &
		~(uint64_t)(dkImageLayoutGetAlignment(&layout) - 1));
	dk.screen_memory = memory_block(2 * image_size, DkMemBlockFlags_GpuCached | DkMemBlockFlags_Image, NULL);
	for (index = 0; index < 2; index++)
	{
		dkImageInitialize(&dk.screen_images[index], &layout, dk.screen_memory, (uint32_t)index * image_size);
		screen_images[index] = &dk.screen_images[index];
	}
	dkSwapchainMakerDefaults(&swapchain_maker, dk.device, nwindowGetDefault(), screen_images, 2);
	dk.swapchain = dkSwapchainCreate(&swapchain_maker);
	host_logf(HOST_LOG_INFO, "deko3d: device ready, presenting at %dx%d", SCREEN_WIDTH, SCREEN_HEIGHT);

	/* the window's committed chunks, GPU-mapped (host_memory.c reports the
	chunks; the GPU checks go as the probe's did) */
	nvMapInit();
	if (R_SUCCEEDED(nvAddressSpaceCreate(&dk.test_address_space, 0x10000)))
		dk.nv_ready = 1;
	else
		host_logf(HOST_LOG_ERROR, "deko3d: no test address space for the window's chunks");
	dk.ready = 1;
	chunks_map();

	/* the upload buffer: CpuUncached, so the copies into it are coherent
	without a flush, and reused once a frame's fence has passed */
	dk.upload_memory = memory_block(FRAMES * UPLOAD_SLICE_SIZE, DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached,
		NULL);
	if (dk.upload_memory)
	{
		dk.upload_cpu = dkMemBlockGetCpuAddr(dk.upload_memory);
		dk.upload_gpu = dkMemBlockGetGpuAddr(dk.upload_memory);
	}
	else
		host_logf(HOST_LOG_ERROR, "deko3d: no upload buffer; data outside the window will not draw");

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
	commands_submit();
	dkQueuePresentImage(dk.queue, dk.swapchain, slot);
	host_dk_presenting = 1;
	if (++dk.frames_presented == 1)
		host_logf(HOST_LOG_INFO, "deko3d: first frame presented");
	dk.frame = (dk.frame + 1) % FRAMES;
	frame_begin();
}

/* runs size bytes of commands at commands (a guest address): one
submission, which the guest numbers as this does (host_dk_retired) */
void host_dk_submit(uint32_t commands, uint32_t size)
{
	const unsigned char *at = (const unsigned char *)(uintptr_t)commands;
	const unsigned char *end = at + size;

	if (!dk.ready && !initialize())
		return;
	dk.serial++;
	chunks_map();
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
			dk.recorded = 1;
			break;
		case DK_COMMAND_CLEAR:
			clear((const struct dk_command_clear *)header);
			dk.recorded = 1;
			break;
		case DK_COMMAND_PRESENT:
			present((const struct dk_command_present *)header);
			break;
		default:
			host_logf(HOST_LOG_ERROR, "deko3d: unknown command %u", (unsigned)header->type);
			break;
		}
		at += header->size;
	}
	/* Every submission ends with its fence, so the guest's count and this
	one agree: a stream handed over before the frame's end (it filled up, or
	a busy check needed what it holds submitted) is run now, and the frame
	goes on in the same slice of command memory. */
	if (dk.recorded || dk.serial_fenced != dk.serial)
		commands_submit();
}

/* the deko3d device, on the game thread only: the shader cache's code
memory (host_dk_shaders.c) needs it. Starts the backend if the guest has not
handed a frame over yet, so a shader asked for before the first Present
still finds it ready; NULL if the device would not come up. */
struct tag_DkDevice *host_dk_device(void)
{
	if (!dk.ready && !initialize())
		return NULL;
	return dk.device;
}
