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
/* the swapchain's images: three, so a frame that misses the display's
refresh starts the next one rather than waiting for the refresh after (a
17 ms frame cost 33 with two; 10.8% of the game thread in a match was the
wait for an image) */
#define SCREEN_IMAGES 3
#define COMMAND_MEMORY_SIZE (8 * 1024 * 1024)
/* a frame's command memory is added to the command buffer a piece at a time,
so that the host knows how much of it a frame has used (command_memory_added) */
#define COMMAND_PIECE_SIZE (1024 * 1024)
#define COMMAND_PIECES (COMMAND_MEMORY_SIZE / COMMAND_PIECE_SIZE)
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

/* the uniform buffers the shaders read (dk_shaders.h's blocks), in one block
of memory: each at an offset aligned as deko3d wants uniform buffers. Their
contents are written by the command stream (dkCmdBufPushConstants), in order
with the draws that read them, so one copy of each does for every frame in
flight. */
#define UNIFORM_VERTEX_CONSTANTS 0x000
#define UNIFORM_VERTEX_PARAMETERS 0xc00
#define UNIFORM_PIXEL_PARAMETERS 0xd00
#define UNIFORM_MEMORY_SIZE 0x1000

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
	/* when it was last bound to be drawn into (dk.target_clock), and
	whether it has been since a barrier made its pixels visible to the
	draws that sample it */
	uint32_t bound;
	int drawn;
	/* its image descriptor's slot, once it has been sampled (render-to-texture) */
	uint32_t slot;
	int has_slot;
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
	/* how many pieces of the frame's command memory the buffer has been given
	(the last one given is the one it is recording into), the bytes of the
	frame's earlier rollovers (command_memory_roll), and the largest a frame
	has needed since the last log line */
	uint32_t command_pieces;
	uint32_t command_rolled_bytes;
	uint32_t command_peak_bytes;
	/* the GPU's time (gpu_timing_*) */
	DkCmdBuf timing_commands;
	DkMemBlock timing_memory;
	uint32_t timing_count[FRAMES];
	/* the timing list has its memory (the first frame_begin gives it) */
	int timing_armed;
	uint64_t gpu_busy_total_ns, gpu_busy_peak_ns;
	uint32_t gpu_frames;
	uint32_t command_rollovers;
	/* set when the last piece is given: the rest of the frame's commands will
	not fit unless the host submits and starts over (host_dk_submit does, between
	commands) */
	int command_memory_low;

	DkMemBlock screen_memory;
	DkImage screen_images[SCREEN_IMAGES];
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
	uint32_t target_clock;

	/* ---------- draws: what the guest has told the host (dk_commands.h), which
	the host holds until it says otherwise and puts into the queue at the next
	draw whenever it needs to (a frame's start, a clear and a change of targets
	all leave the queue's own state not what the draws were told) */
	DkMemBlock uniform_memory;
	DkGpuAddr uniform_gpu;
	struct dk_draw_state state;
	int state_valid;
	int state_dirty;
	uint32_t vertex_shader, pixel_shader;
	int shaders_dirty;
	struct dk_vertex_format format;
	int format_valid;
	int format_dirty;
	/* the format as deko3d takes it: the streams the registers read are
	numbered from 0 in the order of their numbers, then the constants' */
	struct
	{
		DkVtxAttribState attributes[DK_ATTRIBUTE_COUNT];
		DkVtxBufferState buffers[DK_STREAM_COUNT];
		uint32_t buffer_count;
		int has_constants;
	} vertex;
	/* the constants block's place in the upload buffer, good for the
	upload generation (a frame's) it was copied in */
	DkGpuAddr constants_gpu;
	uint32_t constants_generation;
	uint32_t upload_generation;
	int uniforms_bound;
	int said_draw_problem;

	/* a screenshot's readback: CPU-visible memory the size of the back
	buffer's pixels */
	DkMemBlock readback_memory;
	uint32_t readback_size;
	/* the screenshot's image: pitch-linear, in readback_memory, so its rows
	are plain memory the CPU reads */
	DkImage readback_image;
	uint32_t readback_width, readback_height, readback_pitch;
} dk;

static void debug_callback(void *user, const char *context, DkResult result, const char *message)
{
	(void)user;
	host_logf(HOST_LOG_ERROR, "deko3d: %s: result %d: %s", context ? context : "?", (int)result,
		message ? message : "");
}

/* gives the command buffer the next piece of this frame's command memory */
static void command_piece_add(void)
{
	dkCmdBufAddMemory(dk.commands, dk.command_memory,
		(uint32_t)dk.frame * COMMAND_MEMORY_SIZE + dk.command_pieces * COMMAND_PIECE_SIZE, COMMAND_PIECE_SIZE);
	dk.command_pieces++;
	/* (a command is a few KB, so the last piece, 1 MB, has room for the
	commands up to the next time host_dk_submit looks) */
	if (dk.command_pieces == COMMAND_PIECES)
		dk.command_memory_low = 1;
}

/* starts recording at the beginning of this frame's command memory. The
caller has made sure the GPU has finished with it. */
static void command_memory_begin(void)
{
	uint32_t used = dk.command_rolled_bytes + dk.command_pieces * COMMAND_PIECE_SIZE;

	if (used > dk.command_peak_bytes)
		dk.command_peak_bytes = used;
	dkCmdBufClear(dk.commands);
	dk.command_pieces = 0;
	dk.command_rolled_bytes = 0;
	dk.command_memory_low = 0;
	command_piece_add();
}

/* the bytes of command memory the frame has used, to the piece */
static uint32_t command_memory_used(void)
{
	return dk.command_rolled_bytes + dk.command_pieces * COMMAND_PIECE_SIZE;
}

/* the command buffer wants more room: the next piece, while the frame's
region has one. The host never lets the region run out (host_dk_submit rolls
over when the last piece is given), so this ends the program only if one
command asks for a megabyte. */
static void command_memory_exhausted(void *user, DkCmdBuf commands, size_t needed)
{
	(void)user;
	(void)commands;
	if (dk.command_pieces == COMMAND_PIECES || needed > COMMAND_PIECE_SIZE)
		host_fatal("deko3d: a frame needs more than %u bytes of commands (%zu more asked for)",
			(unsigned)COMMAND_MEMORY_SIZE, needed);
	command_piece_add();
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

static void textures_frame_begin(void);
static void textures_initialize(void);
static void visibility_initialize(void);

/* waits for frame slot index's fence (under dk_lock), saying so in the log
if the GPU takes more than two seconds: a stop with nothing said is what a
faulted GPU otherwise looks like */
static void fence_wait_said(int index, const char *what)
{
	int said = 0;

	while (dkFenceWait(&dk.fences[index], 2000000000LL) != DkResult_Success)
	{
		if (!said)
		{
			said = 1;
			host_logf(HOST_LOG_ERROR, "deko3d: %s has waited two seconds for the GPU (submission %u, %u retired, "
				"the queue %s)", what, (unsigned)dk.fence_serial[index], (unsigned)dk.serial_retired,
				dkQueueIsInErrorState(dk.queue) ? "in an error state" : "not in an error state");
		}
	}
	if (said)
		host_logf(HOST_LOG_ERROR, "deko3d: %s: the GPU finished after all", what);
}

/* ---------- the GPU's time a frame

Each submission is bracketed by two timestamps: the first in a small command
list of its own submitted just before it, the second at the end of the
submission itself. The GPU's busy time in a frame is the sum of each pair's
difference - the time it waits between submissions for the CPU to hand
over more is not in it. A frame slot's reports are read when its fence has
passed (frame_begin), and the average and the longest go into the log every
60 frames. Reports are 16 bytes, the timestamp in the second half. */

#define TIMING_PAIRS 32
#define TIMING_COMMAND_SIZE (16 * 1024)

static void gpu_timing_initialize(void)
{
	DkCmdBufMaker maker;

	dk.timing_memory = memory_block(FRAMES * (TIMING_PAIRS * 2 * 16 + TIMING_COMMAND_SIZE),
		DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached, NULL);
	if (!dk.timing_memory)
		return;
	dkCmdBufMakerDefaults(&maker, dk.device);
	dk.timing_commands = dkCmdBufCreate(&maker);
}

static uint32_t gpu_timing_region(int frame)
{
	return (uint32_t)frame * (TIMING_PAIRS * 2 * 16 + TIMING_COMMAND_SIZE);
}

/* (frame_begin, once the slot's fence has passed) the slot's last frame
counted, and its command memory given back for this one */
static void gpu_timing_frame_begin(void)
{
	const uint64_t *reports;
	uint64_t busy = 0;
	uint32_t pair;

	if (!dk.timing_commands)
		return;
	reports = (const uint64_t *)((const char *)dkMemBlockGetCpuAddr(dk.timing_memory) + gpu_timing_region(dk.frame));
	for (pair = 0; pair < dk.timing_count[dk.frame]; pair++)
	{
		uint64_t start = reports[pair * 4 + 1], end = reports[pair * 4 + 3];

		if (end > start)
			busy += dkTimestampToNs(end - start);
	}
	if (dk.timing_count[dk.frame])
	{
		dk.gpu_busy_total_ns += busy;
		if (busy > dk.gpu_busy_peak_ns)
			dk.gpu_busy_peak_ns = busy;
		dk.gpu_frames++;
	}
	dk.timing_count[dk.frame] = 0;
	dkCmdBufClear(dk.timing_commands);
	dkCmdBufAddMemory(dk.timing_commands, dk.timing_memory, gpu_timing_region(dk.frame) + TIMING_PAIRS * 2 * 16,
		TIMING_COMMAND_SIZE);
	dk.timing_armed = 1;
}

/* (commands_submit_serial, before the submission) the start's list
submitted, the end's report recorded in the submission; 0 if not timed */
static int gpu_timing_bracket(void)
{
	DkGpuAddr reports;
	uint32_t pair = dk.timing_count[dk.frame];

	if (!dk.timing_commands || !dk.timing_armed || pair >= TIMING_PAIRS)
		return 0;
	reports = dkMemBlockGetGpuAddr(dk.timing_memory) + gpu_timing_region(dk.frame) + pair * 2 * 16;
	dkCmdBufReportCounter(dk.timing_commands, DkCounter_Timestamp, reports);
	dkQueueSubmitCommands(dk.queue, dkCmdBufFinishList(dk.timing_commands));
	dkCmdBufReportCounter(dk.commands, DkCounter_Timestamp, reports + 16);
	dk.timing_count[dk.frame] = pair + 1;
	return 1;
}

static void frame_begin(void)
{
	/* (under the lock: the guest's other threads read the fences through
	host_dk_retired) */
	pthread_mutex_lock(&dk_lock);
	if (dk.fence_pending[dk.frame])
	{
		fence_wait_said(dk.frame, "the next frame");
		dk.fence_pending[dk.frame] = 0;
		if (dk.fence_serial[dk.frame] > dk.serial_retired)
			dk.serial_retired = dk.fence_serial[dk.frame];
	}
	pthread_mutex_unlock(&dk_lock);
	gpu_timing_frame_begin();
	command_memory_begin();
	/* the upload slice is reused the same way, behind the same fence */
	dk.upload_used = 0;
	dk.upload_generation++;
	textures_frame_begin();
	dk.color = dk.depth = NULL;
	/* the queue is not trusted to be as the draws were told: put it back */
	dk.state_dirty = dk.shaders_dirty = dk.format_dirty = 1;
	dk.uniforms_bound = 0;
}

/* The frame has filled its command memory (command_memory_low): submit what
is recorded, wait for the GPU to finish it, and record on from the start of the
region. A stall, but a rare one, and the alternative ends the program. The
upload and staging slices go on as they were: what the GPU has read of them is
read by now, and what it has not is still ahead of the frame's own offsets. */
static void commands_submit_partial(void);

static void command_memory_roll(void)
{
	uint32_t used = command_memory_used();

	if (dk.recorded)
		commands_submit_partial();
	pthread_mutex_lock(&dk_lock);
	if (dk.fence_pending[dk.frame])
	{
		fence_wait_said(dk.frame, "a command memory rollover");
		dk.fence_pending[dk.frame] = 0;
		if (dk.fence_serial[dk.frame] > dk.serial_retired)
			dk.serial_retired = dk.fence_serial[dk.frame];
	}
	pthread_mutex_unlock(&dk_lock);
	if (!dk.command_rollovers++)
		host_logf(HOST_LOG_WARN, "deko3d: a frame filled its %u bytes of command memory; it is submitted and "
			"waited for, and the frame goes on from the start", (unsigned)COMMAND_MEMORY_SIZE);
	command_memory_begin();
	dk.command_rolled_bytes = used;
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
static void commands_submit_serial(uint32_t finished)
{
	static int fault_said;

	gpu_timing_bracket();
	dkQueueSubmitCommands(dk.queue, dkCmdBufFinishList(dk.commands));
	if (!fault_said && dkQueueIsInErrorState(dk.queue))
	{
		fault_said = 1;
		host_logf(HOST_LOG_ERROR, "deko3d: the queue is in an error state at submission %u (frame %lu)",
			(unsigned)dk.serial, dk.frames_presented);
	}
	pthread_mutex_lock(&dk_lock);
	dkQueueSignalFence(dk.queue, &dk.fences[dk.frame], true);
	dk.fence_pending[dk.frame] = 1;
	dk.fence_serial[dk.frame] = finished;
	dk.serial_fenced = finished;
	pthread_mutex_unlock(&dk_lock);
	dk.cleaned_count = 0;
	dk.recorded = 0;
}

static void commands_submit(void)
{
	commands_submit_serial(dk.serial);
}

/* a submission in the middle of a stream, with more of the stream still to
come: its fence must not say the stream's serial is finished, because the
guest's locks and busy checks take that to mean every draw the stream makes
is, and the rest of them are not even recorded yet. The serial before it is
the last one that is complete. (serial_fenced is left alone, so that the
stream's end still submits and numbers.) */
static void commands_submit_partial(void)
{
	uint32_t fenced = dk.serial_fenced;

	commands_submit_serial(dk.serial - 1);
	dk.serial_fenced = fenced;
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
	DkImage const *screen_images[SCREEN_IMAGES];
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
	gpu_timing_initialize();

	layout_make(&layout, DkImageFormat_RGBA8_Unorm,
		DkImageFlags_UsageRender | DkImageFlags_UsagePresent | DkImageFlags_Usage2DEngine | DkImageFlags_HwCompression,
		SCREEN_WIDTH, SCREEN_HEIGHT);
	image_size = (uint32_t)((dkImageLayoutGetSize(&layout) + dkImageLayoutGetAlignment(&layout) - 1) &
		~(uint64_t)(dkImageLayoutGetAlignment(&layout) - 1));
	dk.screen_memory = memory_block(SCREEN_IMAGES * image_size, DkMemBlockFlags_GpuCached | DkMemBlockFlags_Image, NULL);
	for (index = 0; index < SCREEN_IMAGES; index++)
	{
		dkImageInitialize(&dk.screen_images[index], &layout, dk.screen_memory, (uint32_t)index * image_size);
		screen_images[index] = &dk.screen_images[index];
	}
	dkSwapchainMakerDefaults(&swapchain_maker, dk.device, nwindowGetDefault(), screen_images, SCREEN_IMAGES);
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

	textures_initialize();
	visibility_initialize();
	dk.uniform_memory = memory_block(UNIFORM_MEMORY_SIZE, DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached, NULL);
	if (dk.uniform_memory)
		dk.uniform_gpu = dkMemBlockGetGpuAddr(dk.uniform_memory);
	else
		host_logf(HOST_LOG_ERROR, "deko3d: no uniform buffer; nothing will be drawn");

	frame_begin();
	return 1;
}

static void textures_targets_changing(void);

static void targets_bind(const struct dk_command_targets *command)
{
	DkImageView color_view, depth_view;
	DkImageView const *colors[1] = { &color_view };
	struct target *size_from;

	textures_targets_changing();
	dk.color = target_get(&command->color);
	dk.depth = target_get(&command->depth);
	if (dk.color)
	{
		dk.color->bound = ++dk.target_clock;
		dk.color->drawn = 1;
	}
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
	/* (the draws' viewport and scissor go back at the next draw) */
	dk.state_dirty = 1;
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
	/* (a clear sets the scissor to each rectangle) */
	dk.state_dirty = 1;
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

/* A screenshot's destination: a pitch-linear image in CPU-visible memory,
which the back buffer is blitted into (by the 2D engine, as the present's
blit is). Not a copy of the back buffer into a buffer: the back buffer is
made with hardware compression (target_get), and copying a compressed image
with dkCmdBufCopyImageToBuffer is what stopped the first frame of phase 6's
first console run - the GPU never reached the frame's fence. Kept for the
next screenshot of the same size. */
static int readback_make(uint32_t width, uint32_t height)
{
	DkImageLayoutMaker maker;
	DkImageLayout layout;
	uint32_t pitch = (width * 4 + DK_IMAGE_LINEAR_STRIDE_ALIGNMENT - 1) & ~(uint32_t)(DK_IMAGE_LINEAR_STRIDE_ALIGNMENT - 1);
	uint32_t size;

	if (dk.readback_memory && dk.readback_width == width && dk.readback_height == height)
		return 1;
	dkImageLayoutMakerDefaults(&maker, dk.device);
	maker.flags = DkImageFlags_PitchLinear | DkImageFlags_Usage2DEngine;
	maker.format = DkImageFormat_RGBA8_Unorm;
	maker.dimensions[0] = width;
	maker.dimensions[1] = height;
	maker.pitchStride = pitch;
	dkImageLayoutInitialize(&layout, &maker);
	size = (uint32_t)dkImageLayoutGetSize(&layout);
	if (dk.readback_memory)
		dkMemBlockDestroy(dk.readback_memory);
	dk.readback_memory = memory_block(size, DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached |
		DkMemBlockFlags_Image, NULL);
	dk.readback_size = dk.readback_memory ? size : 0;
	dk.readback_width = dk.readback_height = 0;
	if (!dk.readback_memory)
		return 0;
	dkImageInitialize(&dk.readback_image, &layout, dk.readback_memory, 0);
	dk.readback_width = width;
	dk.readback_height = height;
	dk.readback_pitch = pitch;
	return 1;
}

static void present(const struct dk_command_present *command)
{
	struct target *back_buffer = target_get(&command->back_buffer);
	struct target *screenshot = NULL;
	int slot;

	/* A GPU fault puts the queue in an error state, and deko3d then ends the
	program at the next call that needs the queue - the swapchain's acquire,
	here - with nothing said about the fault. Say it, with the frame, and
	stop presenting: the game goes on, the picture stops, the log has the
	frame the GPU faulted in */
	if (dkQueueIsInErrorState(dk.queue))
	{
		static int said;

		if (!said)
		{
			said = 1;
			host_logf(HOST_LOG_ERROR, "deko3d: the GPU faulted (the queue is in an error state) by frame %lu; "
				"nothing more is presented", dk.frames_presented);
		}
		/* what was recorded is thrown away, so the command memory does not
		fill with frames that are never submitted (frame_begin would wait for
		a fence the faulted GPU never reaches) */
		command_memory_begin();
		dk.recorded = 0;
		return;
	}
	slot = dkQueueAcquireImage(dk.queue, dk.swapchain);
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
		if (command->screenshot && readback_make(back_buffer->surface.width, back_buffer->surface.height))
		{
			DkImageView readback;

			dkImageViewDefaults(&readback, &dk.readback_image);
			dkCmdBufBlitImage(dk.commands, &source, &from, &readback, &from, 0, 0);
			screenshot = back_buffer;
		}
		dkCmdBufBlitImage(dk.commands, &source, &from, &screen_view, &to, DkBlitFlag_FilterLinear, 0);
	}
	commands_submit();
	if (screenshot)
	{
		/* a screenshot is a debugging aid: the frame waits for the GPU to
		have drawn it - two seconds at most, so that a GPU that has stopped
		(as the first screenshot's copy once made it) is said in the log
		rather than freezing the game with nothing said */
		if (dkFenceWait(&dk.fences[dk.frame], 2000000000LL) == DkResult_Success)
		{
			const unsigned char *rows = (const unsigned char *)dkMemBlockGetCpuAddr(dk.readback_memory);
			unsigned char *out = (unsigned char *)(uintptr_t)command->screenshot;
			uint32_t row, row_bytes = screenshot->surface.width * 4;

			for (row = 0; row < screenshot->surface.height; row++)
				memcpy(out + (size_t)row * row_bytes, rows + (size_t)row * dk.readback_pitch, row_bytes);
		}
		else
		{
			host_logf(HOST_LOG_ERROR, "deko3d: the GPU did not finish the frame in two seconds; no screenshot (and "
				"the GPU may have stopped)");
		}
	}
	dkQueuePresentImage(dk.queue, dk.swapchain, slot);
	host_dk_presenting = 1;
	if (++dk.frames_presented == 1)
		host_logf(HOST_LOG_INFO, "deko3d: first frame presented");
	/* (the largest frame's command memory, to a piece, for choosing its size
	from what maps take) */
	if (dk.frames_presented % 60 == 0)
	{
		uint32_t used = command_memory_used();

		if (used > dk.command_peak_bytes)
			dk.command_peak_bytes = used;
		host_logf(HOST_LOG_INFO, "deko3d: command memory, the most a frame used in the last 60: %u KB of %u KB "
			"(%u rollovers in all)", (unsigned)(dk.command_peak_bytes / 1024), (unsigned)(COMMAND_MEMORY_SIZE / 1024),
			(unsigned)dk.command_rollovers);
		if (dk.gpu_frames)
			host_logf(HOST_LOG_INFO, "deko3d: GPU busy %.2f ms a frame on average, %.2f ms the longest (%u frames)",
				(double)dk.gpu_busy_total_ns / dk.gpu_frames / 1e6, (double)dk.gpu_busy_peak_ns / 1e6,
				(unsigned)dk.gpu_frames);
		dk.gpu_busy_total_ns = dk.gpu_busy_peak_ns = 0;
		dk.gpu_frames = 0;
		dk.command_peak_bytes = 0;
	}
	dk.frame = (dk.frame + 1) % FRAMES;
	frame_begin();
}

/* ---------- textures (DEKO3D.md, phase 6, step 2)

The guest's texture cache numbers the images (dk_commands.h) and says when
one's texels are new; the host keeps the images, each with a slot in the
image descriptor set (deko3d binds textures by descriptor), and the samplers,
each with a slot in the sampler descriptor set, found by their contents.

An image the guest drops, or remakes in another shape, may still be read by
draws recorded earlier: its memory and descriptor slot are held back until the
GPU has finished the submission they were let go in (releases_collect). */

/* where each image's memory comes from: blocks of image memory with a list of
the ranges in them that are free, since images come and go (a render target's
image, which stays, comes out of image_memory) */
#define TEXTURE_BLOCK_SIZE (32 * 1024 * 1024)
#define TEXTURE_BLOCK_LIMIT 12
#define FREE_RANGES 256
#define RELEASE_LIMIT 1024
#define IMAGE_SLOTS 8192
#define SAMPLER_SLOTS 1024
#define DESCRIPTOR_SIZE 32
/* texels waiting to be copied to an image, for the draws of one frame; one
slice a frame, reused behind the frame's fence like the upload buffer's */
#define STAGING_SLICE_SIZE (8 * 1024 * 1024)
#define STAGING_ALIGNMENT 256
#define TEXTURE_SIZE_LIMIT 4096
#define TEXTURE_DEPTH_LIMIT 512

struct texture_block
{
	DkMemBlock memory;
	struct
	{
		uint32_t offset, size;
	} free[FREE_RANGES];
	uint32_t free_count;
};

struct texture
{
	int present;
	DkImage image;
	uint32_t slot;
	uint32_t block, offset, size;
	uint32_t kind, format, width, height, depth, levels;
};

/* what is waiting for the GPU to be done with a submission */
struct release
{
	uint32_t serial;
	uint32_t block, offset, size;
	uint32_t slot;
};

static struct
{
	struct texture_block blocks[TEXTURE_BLOCK_LIMIT];
	uint32_t block_count;
	struct texture table[DK_TEXTURE_LIMIT];
	struct texture dummy;

	struct release releases[RELEASE_LIMIT];
	uint32_t release_count;
	uint32_t slot_next;
	uint32_t slots_free[IMAGE_SLOTS];
	uint32_t slots_free_count;

	DkMemBlock image_descriptors, sampler_descriptors;
	DkGpuAddr image_descriptors_gpu, sampler_descriptors_gpu;
	struct dk_sampler samplers[SAMPLER_SLOTS];
	uint32_t sampler_count;

	DkMemBlock staging_memory;
	void *staging_cpu;
	DkGpuAddr staging_gpu;
	uint32_t staging_used;

	/* the stages' images and samplers as told, and as handles for the next draw */
	uint32_t stage_target[4];
	uint32_t stage_composite[4];
	int targets_sampled;
	/* a composite's copies read targets since the last barrier: the 2D
	engine's reads end before a target is drawn into again */
	int targets_copied;
	uint32_t stage_id[4];
	uint32_t stage_sampler[4];
	int stages_dirty;
	int descriptors_bound;
	/* texels were written since the last barrier, or draws were recorded
	since the last one (a texture rewritten in place waits for them) */
	int written_unfenced;
	int draws_unfenced;
	int said_problem;
} tex;

static void texture_problem(const char *format, ...) __attribute__((format(printf, 1, 2)));

static void texture_problem(const char *format, ...)
{
	char message[256];
	va_list arguments;

	if (tex.said_problem >= 16)
		return;
	tex.said_problem++;
	va_start(arguments, format);
	vsnprintf(message, sizeof(message), format, arguments);
	va_end(arguments);
	host_logf(HOST_LOG_ERROR, "deko3d: %s", message);
}

/* ---------- image memory */

static void block_free_range(struct texture_block *block, uint32_t offset, uint32_t size)
{
	uint32_t index, insert = block->free_count;

	for (index = 0; index < block->free_count; index++)
	{
		if (block->free[index].offset > offset)
		{
			insert = index;
			break;
		}
	}
	/* joined to the range before it and the one after, if they touch */
	if (insert > 0 && block->free[insert - 1].offset + block->free[insert - 1].size == offset)
	{
		block->free[insert - 1].size += size;
		if (insert < block->free_count && block->free[insert - 1].offset + block->free[insert - 1].size ==
			block->free[insert].offset)
		{
			block->free[insert - 1].size += block->free[insert].size;
			memmove(&block->free[insert], &block->free[insert + 1], (block->free_count - insert - 1) * sizeof(block->free[0]));
			block->free_count--;
		}
		return;
	}
	if (insert < block->free_count && offset + size == block->free[insert].offset)
	{
		block->free[insert].offset = offset;
		block->free[insert].size += size;
		return;
	}
	if (block->free_count == FREE_RANGES)
		return; /* (the range is lost: the table is full of small holes) */
	memmove(&block->free[insert + 1], &block->free[insert], (block->free_count - insert) * sizeof(block->free[0]));
	block->free[insert].offset = offset;
	block->free[insert].size = size;
	block->free_count++;
}

static int texture_memory_take(uint32_t size, uint32_t alignment, uint32_t *block_out, uint32_t *offset_out)
{
	uint32_t block_index, index;

	if (size > TEXTURE_BLOCK_SIZE)
		return 0;
	for (block_index = 0; block_index <= tex.block_count; block_index++)
	{
		struct texture_block *block;

		if (block_index == tex.block_count)
		{
			if (tex.block_count == TEXTURE_BLOCK_LIMIT)
				return 0;
			block = &tex.blocks[tex.block_count];
			block->memory = memory_block(TEXTURE_BLOCK_SIZE, DkMemBlockFlags_GpuCached | DkMemBlockFlags_Image, NULL);
			if (!block->memory)
				return 0;
			block->free[0].offset = 0;
			block->free[0].size = TEXTURE_BLOCK_SIZE;
			block->free_count = 1;
			tex.block_count++;
		}
		block = &tex.blocks[block_index];
		for (index = 0; index < block->free_count; index++)
		{
			uint32_t start = (block->free[index].offset + alignment - 1) & ~(alignment - 1);
			uint32_t end = block->free[index].offset + block->free[index].size;

			if (start + size <= end)
			{
				uint32_t range_offset = block->free[index].offset, range_size = block->free[index].size;

				/* what is left on either side stays free */
				memmove(&block->free[index], &block->free[index + 1], (block->free_count - index - 1) * sizeof(block->free[0]));
				block->free_count--;
				if (start > range_offset)
					block_free_range(block, range_offset, start - range_offset);
				if (start + size < range_offset + range_size)
					block_free_range(block, start + size, range_offset + range_size - (start + size));
				*block_out = block_index;
				*offset_out = start;
				return 1;
			}
		}
	}
	return 0;
}

/* ---------- descriptor slots and what is waiting for the GPU */

/* lets go of what the GPU has finished with; the game thread */
static void releases_collect(void)
{
	uint32_t retired = host_dk_retired();
	uint32_t index, kept = 0;

	for (index = 0; index < tex.release_count; index++)
	{
		struct release *release = &tex.releases[index];

		if (release->serial > retired)
		{
			tex.releases[kept++] = *release;
			continue;
		}
		if (release->size)
			block_free_range(&tex.blocks[release->block], release->offset, release->size);
		if (release->slot != (uint32_t)-1 && tex.slots_free_count < IMAGE_SLOTS)
			tex.slots_free[tex.slots_free_count++] = release->slot;
	}
	tex.release_count = kept;
}

static int slot_take(uint32_t *slot)
{
	if (!tex.slots_free_count)
		releases_collect();
	if (tex.slots_free_count)
	{
		*slot = tex.slots_free[--tex.slots_free_count];
		return 1;
	}
	if (tex.slot_next == IMAGE_SLOTS)
		return 0;
	*slot = tex.slot_next++;
	return 1;
}

/* an image's memory and slot are let go once the submission being recorded
has run */
static void texture_release(struct texture *texture)
{
	if (!texture->present)
		return;
	if (tex.release_count == RELEASE_LIMIT)
	{
		/* too many waiting: the GPU is waited for, which is rare and lets them all go */
		dkQueueWaitIdle(dk.queue);
		releases_collect();
	}
	if (tex.release_count < RELEASE_LIMIT)
	{
		struct release *release = &tex.releases[tex.release_count++];

		release->serial = dk.serial;
		release->block = texture->block;
		release->offset = texture->offset;
		release->size = texture->size;
		release->slot = texture->slot;
	}
	texture->present = 0;
}

/* ---------- staging */

/* texels outside the window, on their way to an image: copied into the frame's
slice. A slice too small is made empty by waiting for the GPU - rare, and a
texture of a map's load is the likeliest to do it. 0 if the texels are bigger
than a slice. */
static DkGpuAddr staging_copy(const void *data, uint32_t size)
{
	uint32_t offset = (tex.staging_used + STAGING_ALIGNMENT - 1) & ~(STAGING_ALIGNMENT - 1);

	if (!tex.staging_memory || size > STAGING_SLICE_SIZE)
		return 0;
	if (offset + size > STAGING_SLICE_SIZE)
	{
		if (dk.recorded)
			commands_submit_partial();
		dkQueueWaitIdle(dk.queue);
		offset = 0;
	}
	memcpy((char *)tex.staging_cpu + (size_t)dk.frame * STAGING_SLICE_SIZE + offset, data, size);
	tex.staging_used = offset + size;
	return tex.staging_gpu + (size_t)dk.frame * STAGING_SLICE_SIZE + offset;
}

/* ---------- images */

static DkImageFormat image_format(uint32_t format)
{
	switch (format)
	{
	case DK_TEXTURE_BC1: return DkImageFormat_RGBA_BC1;
	case DK_TEXTURE_BC2: return DkImageFormat_RGBA_BC2;
	case DK_TEXTURE_BC3: return DkImageFormat_RGBA_BC3;
	default: return DkImageFormat_BGRA8_Unorm;
	}
}

static uint32_t level_size(uint32_t value, uint32_t level)
{
	value >>= level;
	return value ? value : 1;
}

/* the bytes of one level, in the command's source */
static uint32_t level_bytes(const struct dk_command_texture *command, uint32_t level)
{
	uint32_t width = level_size(command->width, level), height = level_size(command->height, level);
	uint32_t depth = command->kind == DK_TEXTURE_3D ? level_size(command->depth, level) : 1;

	if (command->format == DK_TEXTURE_BGRA)
		return width * height * depth * 4;
	return ((width + 3) / 4) * ((height + 3) / 4) * (command->format == DK_TEXTURE_BC1 ? 8 : 16) * depth;
}

/* makes the image for a texture command, in the table's entry */
static int texture_make(struct texture *texture, const struct dk_command_texture *command)
{
	DkImageLayoutMaker maker;
	DkImageLayout layout;
	DkImageView view;
	uint32_t size, alignment;
	DkImageDescriptor *descriptors = (DkImageDescriptor *)dkMemBlockGetCpuAddr(tex.image_descriptors);

	if (!slot_take(&texture->slot))
	{
		texture_problem("no descriptor slot for texture %u", (unsigned)command->id);
		return 0;
	}
	dkImageLayoutMakerDefaults(&maker, dk.device);
	maker.type = command->kind == DK_TEXTURE_CUBE ? DkImageType_Cubemap : command->kind == DK_TEXTURE_3D ?
		DkImageType_3D : DkImageType_2D;
	maker.flags = 0;
	maker.format = image_format(command->format);
	maker.dimensions[0] = command->width;
	maker.dimensions[1] = command->height;
	maker.dimensions[2] = command->kind == DK_TEXTURE_CUBE ? 6 : command->kind == DK_TEXTURE_3D ? command->depth : 0;
	maker.mipLevels = command->levels;
	dkImageLayoutInitialize(&layout, &maker);
	size = (uint32_t)dkImageLayoutGetSize(&layout);
	alignment = dkImageLayoutGetAlignment(&layout);
	if (!texture_memory_take(size, alignment, &texture->block, &texture->offset))
	{
		/* what the GPU has finished with may make room */
		releases_collect();
		if (!texture_memory_take(size, alignment, &texture->block, &texture->offset))
		{
			if (tex.slots_free_count < IMAGE_SLOTS)
				tex.slots_free[tex.slots_free_count++] = texture->slot;
			texture_problem("no image memory for texture %u (%ux%u, %u levels, %u bytes)", (unsigned)command->id,
				(unsigned)command->width, (unsigned)command->height, (unsigned)command->levels, (unsigned)size);
			return 0;
		}
	}
	texture->size = size;
	dkImageInitialize(&texture->image, &layout, tex.blocks[texture->block].memory, texture->offset);
	dkImageViewDefaults(&view, &texture->image);
	dkImageDescriptorInitialize(&descriptors[texture->slot], &view, false, false);
	texture->kind = command->kind;
	texture->format = command->format;
	texture->width = command->width;
	texture->height = command->height;
	texture->depth = command->depth;
	texture->levels = command->levels;
	texture->present = 1;
	return 1;
}

/* the barrier the images' writes and reads are ordered by: the writes done
since the last one are made visible to the draws that follow */
static void textures_fence(int before_writes)
{
	if (before_writes)
	{
		if (tex.draws_unfenced)
			dkCmdBufBarrier(dk.commands, DkBarrier_Full, 0);
		tex.draws_unfenced = 0;
		return;
	}
	if (!tex.written_unfenced)
		return;
	dkCmdBufBarrier(dk.commands, DkBarrier_Full, DkInvalidateFlags_Image | DkInvalidateFlags_Descriptors);
	tex.written_unfenced = 0;
	tex.draws_unfenced = 0;
}

/* copies the command's texels into the image: every face, every level */
/* host_texels: the texels in the host's own memory (the dummy's), or NULL
for the command's source, which is a guest address. They must not cross in
the command: a host address does not fit its 32 bits, and cut short it named
memory that is not there (the first textured draw on a console faulted in
staging_copy on the dummy's four bytes) */
static void texture_write(struct texture *texture, const struct dk_command_texture *command, const void *host_texels)
{
	uint32_t faces = command->kind == DK_TEXTURE_CUBE ? 6 : 1;
	uint32_t face, level, face_bytes = 0;
	DkGpuAddr source;

	for (level = 0; level < command->levels; level++)
		face_bytes += level_bytes(command, level);
	if (command->source_bytes < command->face_bytes * (faces - 1) + face_bytes || command->face_bytes < face_bytes)
	{
		texture_problem("texture %u names %u bytes of texels and needs %u", (unsigned)command->id,
			(unsigned)command->source_bytes, (unsigned)(command->face_bytes * (faces - 1) + face_bytes));
		return;
	}
	/* in the window, where the game keeps it, for the compressed formats; the
	decoded ones the guest made elsewhere are copied to the staging buffer */
	if (host_texels)
		source = staging_copy(host_texels, command->source_bytes);
	else
	{
		source = window_read(command->source, command->source_bytes);
		if (!source)
			source = staging_copy((const void *)(uintptr_t)command->source, command->source_bytes);
	}
	if (!source)
	{
		texture_problem("texture %u (%u bytes) could not be read", (unsigned)command->id,
			(unsigned)command->source_bytes);
		return;
	}
	/* (a texture written in place waits for the draws that read it) */
	textures_fence(1);
	for (face = 0; face < faces; face++)
	{
		uint32_t offset = face * command->face_bytes;

		for (level = 0; level < command->levels; level++)
		{
			DkImageView view;
			DkImageRect rectangle;
			DkCopyBuf copy;

			dkImageViewDefaults(&view, &texture->image);
			view.mipLevelOffset = (uint8_t)level;
			view.mipLevelCount = 1;
			if (command->kind == DK_TEXTURE_CUBE)
			{
				view.layerOffset = (uint16_t)face;
				view.layerCount = 1;
			}
			rectangle.x = rectangle.y = rectangle.z = 0;
			rectangle.width = level_size(command->width, level);
			rectangle.height = level_size(command->height, level);
			rectangle.depth = command->kind == DK_TEXTURE_3D ? level_size(command->depth, level) : 1;
			copy.addr = source + offset;
			copy.rowLength = 0;
			copy.imageHeight = 0;
			dkCmdBufCopyBufferToImage(dk.commands, &copy, &view, &rectangle, 0);
			offset += level_bytes(command, level);
		}
	}
	tex.written_unfenced = 1;
	dk.recorded = 1;
}

static int texture_command_valid(const struct dk_command_texture *command)
{
	uint32_t largest = command->width > command->height ? command->width : command->height;
	uint32_t levels_possible = 1;

	if (command->id == 0 || command->id >= DK_TEXTURE_LIMIT || command->kind > DK_TEXTURE_CUBE ||
		command->format > DK_TEXTURE_BC3 || !command->width || !command->height || !command->depth ||
		command->width > TEXTURE_SIZE_LIMIT || command->height > TEXTURE_SIZE_LIMIT ||
		command->depth > TEXTURE_DEPTH_LIMIT || !command->levels)
		return 0;
	if (command->kind == DK_TEXTURE_CUBE && command->width != command->height)
		return 0;
	if (command->kind != DK_TEXTURE_3D && command->depth != 1)
		return 0;
	if (command->kind == DK_TEXTURE_3D && command->depth > largest)
		largest = command->depth;
	while ((largest >> levels_possible) != 0)
		levels_possible++;
	return command->levels <= levels_possible;
}

static void texture_receive(const struct dk_command_texture *command)
{
	struct texture *texture;

	if (!texture_command_valid(command))
	{
		texture_problem("a bad texture command: image %u, kind %u, format %u, %ux%ux%u, %u levels",
			(unsigned)command->id, (unsigned)command->kind, (unsigned)command->format, (unsigned)command->width,
			(unsigned)command->height, (unsigned)command->depth, (unsigned)command->levels);
		return;
	}
	if (!dk.ready || !tex.image_descriptors)
		return;
	texture = &tex.table[command->id];
	/* the same shape: written in place; else remade */
	if (texture->present && (texture->kind != command->kind || texture->format != command->format ||
		texture->width != command->width || texture->height != command->height || texture->depth != command->depth ||
		texture->levels != command->levels))
		texture_release(texture);
	if (!texture->present && !texture_make(texture, command))
		return;
	/* (made only, for DK_COMMAND_TEXTURE_ROWS to fill) */
	if (command->source_bytes)
		texture_write(texture, command, NULL);
	tex.stages_dirty = 1;
}

/* writes some rows of one level of a 2D BGRA image (the text's atlas and
the high-res art: xbox_textures_dk.c) */
static void texture_rows_receive(const struct dk_command_texture_rows *command)
{
	struct texture *texture;
	uint32_t width, height;
	DkGpuAddr source;
	DkImageView view;
	DkImageRect rectangle;
	DkCopyBuf copy;

	if (!dk.ready || !tex.image_descriptors || !command->id || command->id >= DK_TEXTURE_LIMIT)
		return;
	texture = &tex.table[command->id];
	if (!texture->present || texture->kind != DK_TEXTURE_2D || texture->format != DK_TEXTURE_BGRA ||
		command->level >= texture->levels)
	{
		texture_problem("rows for image %u, which is not a 2D BGRA one with level %u", (unsigned)command->id,
			(unsigned)command->level);
		return;
	}
	width = level_size(texture->width, command->level);
	height = level_size(texture->height, command->level);
	if (!command->rows || command->top >= height || command->rows > height - command->top ||
		command->source_bytes / 4 / width < command->rows)
	{
		texture_problem("rows %u to %u of image %u level %u (%ux%u, %u bytes) are not in it", (unsigned)command->top,
			(unsigned)(command->top + command->rows), (unsigned)command->id, (unsigned)command->level,
			(unsigned)width, (unsigned)height, (unsigned)command->source_bytes);
		return;
	}
	source = window_read(command->source, command->source_bytes);
	if (!source)
		source = staging_copy((const void *)(uintptr_t)command->source, command->source_bytes);
	if (!source)
	{
		texture_problem("rows of image %u (%u bytes) could not be read", (unsigned)command->id,
			(unsigned)command->source_bytes);
		return;
	}
	textures_fence(1);
	dkImageViewDefaults(&view, &texture->image);
	view.mipLevelOffset = (uint8_t)command->level;
	view.mipLevelCount = 1;
	rectangle.x = rectangle.z = 0;
	rectangle.y = command->top;
	rectangle.width = width;
	rectangle.height = command->rows;
	rectangle.depth = 1;
	copy.addr = source;
	copy.rowLength = 0;
	copy.imageHeight = 0;
	dkCmdBufCopyBufferToImage(dk.commands, &copy, &view, &rectangle, 0);
	tex.written_unfenced = 1;
	tex.stages_dirty = 1;
	dk.recorded = 1;
}

static void texture_free_receive(const struct dk_command_texture_free *command)
{
	if (command->id && command->id < DK_TEXTURE_LIMIT && dk.ready)
	{
		texture_release(&tex.table[command->id]);
		tex.stages_dirty = 1;
	}
}

/* ---------- the dummy a stage with no image samples: opaque black, as OpenGL's
unbound texture is */

static void dummy_make(void)
{
	static const unsigned char black[4] = { 0, 0, 0, 255 };
	struct dk_command_texture command;

	if (tex.dummy.present)
		return;
	memset(&command, 0, sizeof(command));
	command.id = 0;
	command.kind = DK_TEXTURE_2D;
	command.format = DK_TEXTURE_BGRA;
	command.width = command.height = command.depth = command.levels = 1;
	command.source = 0;
	command.face_bytes = command.source_bytes = 4;
	/* (the host's own four bytes: handed to the write as such, through the
	staging buffer, not as the command's guest address) */
	if (texture_make(&tex.dummy, &command))
		texture_write(&tex.dummy, &command, black);
}

/* ---------- samplers */

static DkWrapMode wrap_mode(uint32_t wrap)
{
	switch (wrap)
	{
	case DK_WRAP_MIRROR: return DkWrapMode_MirroredRepeat;
	case DK_WRAP_CLAMP: return DkWrapMode_ClampToEdge;
	case DK_WRAP_BORDER: return DkWrapMode_ClampToBorder;
	default: return DkWrapMode_Repeat;
	}
}

/* the slot of a sampler with these contents, made if there is none */
static uint32_t sampler_slot(const struct dk_sampler *sampler)
{
	DkSamplerDescriptor *descriptors = (DkSamplerDescriptor *)dkMemBlockGetCpuAddr(tex.sampler_descriptors);
	DkSampler made;
	uint32_t index;

	for (index = 0; index < tex.sampler_count; index++)
	{
		if (!memcmp(&tex.samplers[index], sampler, sizeof(*sampler)))
			return index;
	}
	if (tex.sampler_count == SAMPLER_SLOTS)
	{
		texture_problem("more than %u samplers; the first is used instead", SAMPLER_SLOTS);
		return 0;
	}
	dkSamplerDefaults(&made);
	made.minFilter = sampler->min_linear ? DkFilter_Linear : DkFilter_Nearest;
	made.magFilter = sampler->mag_linear ? DkFilter_Linear : DkFilter_Nearest;
	made.mipFilter = sampler->mip_filter == DK_MIP_LINEAR ? DkMipFilter_Linear :
		sampler->mip_filter == DK_MIP_NEAREST ? DkMipFilter_Nearest : DkMipFilter_None;
	made.wrapMode[0] = wrap_mode(sampler->wrap[0]);
	made.wrapMode[1] = wrap_mode(sampler->wrap[1]);
	made.wrapMode[2] = wrap_mode(sampler->wrap[2]);
	made.lodClampMin = sampler->lod_minimum;
	made.lodBias = sampler->lod_bias;
	made.maxAnisotropy = sampler->anisotropy < 1.0f ? 1.0f : sampler->anisotropy > 16.0f ? 16.0f : sampler->anisotropy;
	for (index = 0; index < 4; index++)
		made.borderColor[index].value_f = sampler->border[index];
	index = tex.sampler_count++;
	tex.samplers[index] = *sampler;
	dkSamplerDescriptorInitialize(&descriptors[index], &made);
	return index;
}

static void textures_receive(const struct dk_command_textures *command)
{
	uint32_t stage;

	if (!tex.sampler_descriptors)
		return;
	for (stage = 0; stage < 4; stage++)
	{
		tex.stage_id[stage] = command->stages[stage].id;
		tex.stage_target[stage] = command->stages[stage].target;
		tex.stage_composite[stage] = command->stages[stage].composite;
		tex.stage_sampler[stage] = sampler_slot(&command->stages[stage].sampler);
	}
	tex.stages_dirty = 1;
}

/* the targets are about to change: a target sampled since the last barrier
is drawn into only once the draws that sampled it are done, and a stage that
samples a target looks again (textures_apply) */
static void textures_targets_changing(void)
{
	if (tex.targets_copied)
	{
		dkCmdBufBarrier(dk.commands, DkBarrier_Full, 0);
		tex.targets_copied = tex.targets_sampled = 0;
	}
	else if (tex.targets_sampled)
	{
		dkCmdBufBarrier(dk.commands, DkBarrier_Fragments, 0);
		tex.targets_sampled = 0;
	}
	tex.stages_dirty = 1;
}

/* the color target a stage samples (render-to-texture): of those whose
surface is at data, the one drawn into last, as xgpu_render_target_find in
d3d8_gl.c chooses; its descriptor made the first time, and a barrier between
the draws into it and the draws that sample it. NULL if there is none, and
the stage samples the dummy. */
static struct target *target_sampled(uint32_t data)
{
	struct target *best = NULL;
	int index;

	for (index = 0; index < dk.target_count; index++)
	{
		struct target *target = &dk.targets[index];

		if (target->surface.data == data && target->surface.kind == DK_SURFACE_COLOR &&
			(!best || target->bound > best->bound))
			best = target;
	}
	if (!best)
		return NULL;
	if (!best->has_slot)
	{
		DkImageDescriptor *descriptors = (DkImageDescriptor *)dkMemBlockGetCpuAddr(tex.image_descriptors);
		DkImageView view;

		if (!slot_take(&best->slot))
			return NULL;
		dkImageViewDefaults(&view, &best->image);
		dkImageDescriptorInitialize(&descriptors[best->slot], &view, false, false);
		best->has_slot = 1;
		dkCmdBufBarrier(dk.commands, DkBarrier_None, DkInvalidateFlags_Descriptors);
	}
	if (best->drawn)
	{
		dkCmdBufBarrier(dk.commands, DkBarrier_Fragments, DkInvalidateFlags_Image);
		best->drawn = 0;
	}
	tex.targets_sampled = 1;
	return best;
}

/* ---------- mip composites (DK_COMMAND_COMPOSITE) */

#define COMPOSITE_LIMIT 32

static struct composite
{
	struct dk_command_composite defined;
	DkImage image;
	uint32_t slot;
	/* dk.target_clock when last copied, and how many levels came from targets */
	uint32_t stamp;
	uint32_t rendered_levels;
	int copied;
} composites[COMPOSITE_LIMIT];
static int composite_count;

static void composite_receive(const struct dk_command_composite *command)
{
	struct composite *composite = NULL;
	DkImageLayoutMaker maker;
	DkImageLayout layout;
	DkImageView view;
	DkMemBlock block;
	uint32_t offset;
	int index;

	if (!dk.ready || !tex.image_descriptors || !command->width || !command->height || command->levels < 2 ||
		command->levels > DK_COMPOSITE_LEVELS || command->width > TEXTURE_SIZE_LIMIT ||
		command->height > TEXTURE_SIZE_LIMIT)
		return;
	for (index = 0; index < composite_count; index++)
	{
		struct dk_command_composite *defined = &composites[index].defined;

		if (defined->data == command->data && defined->width == command->width &&
			defined->height == command->height && defined->levels == command->levels)
		{
			/* (the levels' addresses, which may be new) */
			memcpy(defined->level_data, command->level_data, sizeof(defined->level_data));
			composites[index].copied = 0;
			return;
		}
	}
	if (composite_count == COMPOSITE_LIMIT)
	{
		texture_problem("more than %d mip composites; %08x is not made", COMPOSITE_LIMIT, (unsigned)command->data);
		return;
	}
	dkImageLayoutMakerDefaults(&maker, dk.device);
	maker.flags = DkImageFlags_Usage2DEngine;
	maker.format = DkImageFormat_RGBA8_Unorm;
	maker.dimensions[0] = command->width;
	maker.dimensions[1] = command->height;
	maker.mipLevels = command->levels;
	dkImageLayoutInitialize(&layout, &maker);
	if (!image_memory(&layout, &block, &offset))
	{
		texture_problem("no image memory for the %ux%u mip composite at %08x", (unsigned)command->width,
			(unsigned)command->height, (unsigned)command->data);
		return;
	}
	composite = &composites[composite_count];
	memset(composite, 0, sizeof(*composite));
	if (!slot_take(&composite->slot))
		return;
	composite->defined = *command;
	dkImageInitialize(&composite->image, &layout, block, offset);
	dkImageViewDefaults(&view, &composite->image);
	dkImageDescriptorInitialize(&((DkImageDescriptor *)dkMemBlockGetCpuAddr(tex.image_descriptors))[composite->slot],
		&view, false, false);
	dkCmdBufBarrier(dk.commands, DkBarrier_None, DkInvalidateFlags_Descriptors);
	dk.recorded = 1;
	composite_count++;
}

/* the color target at data of exactly this size, the one bound last */
static struct target *target_exact(uint32_t data, uint32_t width, uint32_t height)
{
	struct target *best = NULL;
	int index;

	for (index = 0; index < dk.target_count; index++)
	{
		struct target *target = &dk.targets[index];

		if (target->surface.data == data && target->surface.kind == DK_SURFACE_COLOR &&
			target->surface.width == width && target->surface.height == height && (!best || target->bound > best->bound))
			best = target;
	}
	return best;
}

/* the composite a stage samples, its levels copied from their targets if one
has been drawn into since the last copy; NULL if there is none, or no level
has been drawn */
static struct composite *composite_sampled(uint32_t data, uint32_t levels)
{
	struct composite *composite = NULL;
	struct target *sources[DK_COMPOSITE_LEVELS];
	uint32_t level, rendered = 0, newest = 0;
	int index;

	for (index = 0; index < composite_count; index++)
	{
		if (composites[index].defined.data == data && composites[index].defined.levels == levels)
		{
			composite = &composites[index];
			break;
		}
	}
	if (!composite)
		return NULL;
	/* the levels the game drew, from the top, up to the first it did not */
	for (level = 0; level < levels; level++)
	{
		sources[level] = target_exact(composite->defined.level_data[level],
			level_size(composite->defined.width, level), level_size(composite->defined.height, level));
		if (!sources[level])
			break;
		if (sources[level]->bound > newest)
			newest = sources[level]->bound;
		rendered++;
	}
	if (!rendered)
		return NULL;
	if (composite->copied && composite->rendered_levels == rendered && newest <= composite->stamp)
		return composite;
	/* the targets' draws done, then each level copied (the 2D engine), the
	ones not drawn halved from the one above, and the copies done before
	the draws that sample them */
	dkCmdBufBarrier(dk.commands, DkBarrier_Fragments, 0);
	for (level = 0; level < levels; level++)
	{
		DkImageView source, destination;
		DkImageRect from, to;

		dkImageViewDefaults(&destination, &composite->image);
		destination.mipLevelOffset = (uint8_t)level;
		destination.mipLevelCount = 1;
		to.x = to.y = to.z = 0;
		to.width = level_size(composite->defined.width, level);
		to.height = level_size(composite->defined.height, level);
		to.depth = 1;
		from = to;
		if (level < rendered)
		{
			dkImageViewDefaults(&source, &sources[level]->image);
			dkCmdBufBlitImage(dk.commands, &source, &from, &destination, &to, 0, 0);
		}
		else
		{
			dkCmdBufBarrier(dk.commands, DkBarrier_Full, 0);
			dkImageViewDefaults(&source, &composite->image);
			source.mipLevelOffset = (uint8_t)(level - 1);
			source.mipLevelCount = 1;
			from.width = level_size(composite->defined.width, level - 1);
			from.height = level_size(composite->defined.height, level - 1);
			dkCmdBufBlitImage(dk.commands, &source, &from, &destination, &to, DkBlitFlag_FilterLinear, 0);
		}
	}
	dkCmdBufBarrier(dk.commands, DkBarrier_Full, DkInvalidateFlags_Image);
	composite->copied = 1;
	composite->rendered_levels = rendered;
	composite->stamp = dk.target_clock;
	tex.targets_copied = 1;
	dk.recorded = 1;
	return composite;
}

/* ---------- visibility tests (DK_COMMAND_VISIBILITY_*) */

static DkMemBlock visibility_memory;

static void visibility_initialize(void)
{
	visibility_memory = memory_block(DK_VISIBILITY_SLOTS * 16, DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached,
		NULL);
	if (visibility_memory)
		memset(dkMemBlockGetCpuAddr(visibility_memory), 0, DK_VISIBILITY_SLOTS * 16);
}

static void visibility_begin(void)
{
	if (!visibility_memory)
		return;
	dkCmdBufResetCounter(dk.commands, DkCounter_SamplesPassed);
	dk.recorded = 1;
}

/* (a report is 16 bytes: the count, 64 bits, then a timestamp) */
static void visibility_end(const struct dk_command_visibility_end *command)
{
	if (!visibility_memory || !command->index || command->index >= DK_VISIBILITY_SLOTS)
		return;
	dkCmdBufReportCounter(dk.commands, DkCounter_SamplesPassed,
		dkMemBlockGetGpuAddr(visibility_memory) + (DkGpuAddr)command->index * 16);
	dk.recorded = 1;
}

/* the latest count the GPU has written to a slot: from the slot's latest
test, or while the GPU is behind, an earlier one (as d3d8_gl.c's query
buffer). Any guest thread. */
uint32_t host_dk_visibility(uint32_t index)
{
	uint64_t count;

	if (!visibility_memory || !index || index >= DK_VISIBILITY_SLOTS)
		return 0;
	count = *(volatile uint64_t *)((char *)dkMemBlockGetCpuAddr(visibility_memory) + (size_t)index * 16);
	return count > 0xffffffffu ? 0xffffffffu : (uint32_t)count;
}

/* the stages' textures bound for the next draw: the descriptor sets once a
frame (frame_begin), the handles when they change */
static int textures_apply(void)
{
	DkResHandle handles[4];
	uint32_t stage;

	if (!tex.image_descriptors || !tex.sampler_descriptors)
		return 0;
	if (!tex.descriptors_bound)
	{
		dkCmdBufBindImageDescriptorSet(dk.commands, tex.image_descriptors_gpu, IMAGE_SLOTS);
		dkCmdBufBindSamplerDescriptorSet(dk.commands, tex.sampler_descriptors_gpu, SAMPLER_SLOTS);
		tex.descriptors_bound = 1;
		tex.stages_dirty = 1;
	}
	dummy_make();
	/* what was written since the last barrier is made visible to this draw */
	textures_fence(0);
	if (!tex.stages_dirty)
		return 1;
	for (stage = 0; stage < 4; stage++)
	{
		uint32_t id = tex.stage_id[stage];
		const struct texture *texture = id && tex.table[id].present ? &tex.table[id] : &tex.dummy;
		struct target *target;

		if (tex.stage_target[stage] && tex.stage_composite[stage])
		{
			struct composite *composite = composite_sampled(tex.stage_target[stage], tex.stage_composite[stage]);

			if (composite)
			{
				handles[stage] = dkMakeTextureHandle(composite->slot, tex.stage_sampler[stage]);
				continue;
			}
		}
		target = tex.stage_target[stage] ? target_sampled(tex.stage_target[stage]) : NULL;
		if (target)
		{
			handles[stage] = dkMakeTextureHandle(target->slot, tex.stage_sampler[stage]);
			continue;
		}
		if (!texture->present)
			return 0;
		handles[stage] = dkMakeTextureHandle(texture->slot, tex.stage_sampler[stage]);
	}
	dkCmdBufBindTextures(dk.commands, DkStage_Fragment, DK_BINDING_TEXTURE0, handles, 4);
	tex.stages_dirty = 0;
	return 1;
}

/* the start of a frame: the staging slice is free again behind the frame's
fence, the descriptor sets are bound anew, and what the GPU has finished with
is let go */
static void textures_frame_begin(void)
{
	tex.staging_used = 0;
	tex.descriptors_bound = 0;
	if (dk.ready)
		releases_collect();
}

/* the memory textures need, made when the device is */
static void textures_initialize(void)
{
	tex.image_descriptors = memory_block(IMAGE_SLOTS * DESCRIPTOR_SIZE, DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached,
		NULL);
	tex.sampler_descriptors = memory_block(SAMPLER_SLOTS * DESCRIPTOR_SIZE, DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached,
		NULL);
	tex.staging_memory = memory_block(FRAMES * STAGING_SLICE_SIZE, DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached,
		NULL);
	if (!tex.image_descriptors || !tex.sampler_descriptors || !tex.staging_memory)
	{
		host_logf(HOST_LOG_ERROR, "deko3d: no memory for textures' descriptors or staging; nothing textured will draw");
		tex.image_descriptors = tex.sampler_descriptors = NULL;
		return;
	}
	tex.image_descriptors_gpu = dkMemBlockGetGpuAddr(tex.image_descriptors);
	tex.sampler_descriptors_gpu = dkMemBlockGetGpuAddr(tex.sampler_descriptors);
	tex.staging_cpu = dkMemBlockGetCpuAddr(tex.staging_memory);
	tex.staging_gpu = dkMemBlockGetGpuAddr(tex.staging_memory);
	/* (the dummy takes slot 0 of the descriptors; the guest's images follow) */
}

/* ---------- draws (DEKO3D.md, phase 6) */

/* the Xbox's enumerants, which are OpenGL's (dk_commands.h), as deko3d's */
static DkCompareOp compare_op(uint32_t function)
{
	return function >= 0x200 && function <= 0x207 ? (DkCompareOp)(function - 0x200 + DkCompareOp_Never) :
		DkCompareOp_Always;
}

static DkStencilOp stencil_op(uint32_t operation)
{
	switch (operation)
	{
	case 0x0000: return DkStencilOp_Zero;
	case 0x1e01: return DkStencilOp_Replace;
	case 0x1e02: return DkStencilOp_Incr;
	case 0x1e03: return DkStencilOp_Decr;
	case 0x150a: return DkStencilOp_Invert;
	case 0x8507: return DkStencilOp_IncrWrap;
	case 0x8508: return DkStencilOp_DecrWrap;
	default: return DkStencilOp_Keep;
	}
}

static DkBlendFactor blend_factor(uint32_t factor)
{
	switch (factor)
	{
	case 0x0000: return DkBlendFactor_Zero;
	case 0x0300: return DkBlendFactor_SrcColor;
	case 0x0301: return DkBlendFactor_InvSrcColor;
	case 0x0302: return DkBlendFactor_SrcAlpha;
	case 0x0303: return DkBlendFactor_InvSrcAlpha;
	case 0x0304: return DkBlendFactor_DstAlpha;
	case 0x0305: return DkBlendFactor_InvDstAlpha;
	case 0x0306: return DkBlendFactor_DstColor;
	case 0x0307: return DkBlendFactor_InvDstColor;
	case 0x0308: return DkBlendFactor_SrcAlphaSaturate;
	case 0x8001: return DkBlendFactor_ConstColor;
	case 0x8002: return DkBlendFactor_InvConstColor;
	case 0x8003: return DkBlendFactor_ConstAlpha;
	case 0x8004: return DkBlendFactor_InvConstAlpha;
	default: return DkBlendFactor_One;
	}
}

static DkBlendOp blend_op(uint32_t equation)
{
	switch (equation)
	{
	case DK_BLEND_SUBTRACT: return DkBlendOp_Sub;
	case DK_BLEND_REVERSE_SUBTRACT: return DkBlendOp_RevSub;
	case DK_BLEND_MIN: return DkBlendOp_Min;
	case DK_BLEND_MAX: return DkBlendOp_Max;
	default: return DkBlendOp_Add;
	}
}

static DkPrimitive primitive_of(uint32_t primitive)
{
	switch (primitive)
	{
	case DK_PRIMITIVE_POINTS: return DkPrimitive_Points;
	case DK_PRIMITIVE_LINES: return DkPrimitive_Lines;
	case DK_PRIMITIVE_LINE_LOOP: return DkPrimitive_LineLoop;
	case DK_PRIMITIVE_LINE_STRIP: return DkPrimitive_LineStrip;
	case DK_PRIMITIVE_TRIANGLE_STRIP: return DkPrimitive_TriangleStrip;
	case DK_PRIMITIVE_TRIANGLE_FAN: return DkPrimitive_TriangleFan;
	case DK_PRIMITIVE_QUADS: return DkPrimitive_Quads;
	default: return DkPrimitive_Triangles;
	}
}

/* how deko3d reads a register of each kind (DK_ATTRIBUTE_*); a kind of 0 or
past the end is not one */
static int attribute_format_make(uint32_t format, DkVtxAttribSize *size, DkVtxAttribType *type, int *bgra)
{
	static const struct
	{
		DkVtxAttribSize size;
		DkVtxAttribType type;
	} table[] =
	{
		[DK_ATTRIBUTE_UNFED] = { DkVtxAttribSize_4x32, DkVtxAttribType_Float },
		[DK_ATTRIBUTE_UNFED_PACKED] = { DkVtxAttribSize_1x32, DkVtxAttribType_Uint },
		[DK_ATTRIBUTE_FLOAT1] = { DkVtxAttribSize_1x32, DkVtxAttribType_Float },
		[DK_ATTRIBUTE_FLOAT2] = { DkVtxAttribSize_2x32, DkVtxAttribType_Float },
		[DK_ATTRIBUTE_FLOAT3] = { DkVtxAttribSize_3x32, DkVtxAttribType_Float },
		[DK_ATTRIBUTE_FLOAT4] = { DkVtxAttribSize_4x32, DkVtxAttribType_Float },
		[DK_ATTRIBUTE_COLOR] = { DkVtxAttribSize_4x8, DkVtxAttribType_Unorm },
		[DK_ATTRIBUTE_SHORT1] = { DkVtxAttribSize_1x16, DkVtxAttribType_Sscaled },
		[DK_ATTRIBUTE_SHORT2] = { DkVtxAttribSize_2x16, DkVtxAttribType_Sscaled },
		[DK_ATTRIBUTE_SHORT3] = { DkVtxAttribSize_3x16, DkVtxAttribType_Sscaled },
		[DK_ATTRIBUTE_SHORT4] = { DkVtxAttribSize_4x16, DkVtxAttribType_Sscaled },
		[DK_ATTRIBUTE_NORMSHORT1] = { DkVtxAttribSize_1x16, DkVtxAttribType_Snorm },
		[DK_ATTRIBUTE_NORMSHORT2] = { DkVtxAttribSize_2x16, DkVtxAttribType_Snorm },
		[DK_ATTRIBUTE_NORMSHORT3] = { DkVtxAttribSize_3x16, DkVtxAttribType_Snorm },
		[DK_ATTRIBUTE_NORMSHORT4] = { DkVtxAttribSize_4x16, DkVtxAttribType_Snorm },
		[DK_ATTRIBUTE_BYTE1] = { DkVtxAttribSize_1x8, DkVtxAttribType_Unorm },
		[DK_ATTRIBUTE_BYTE2] = { DkVtxAttribSize_2x8, DkVtxAttribType_Unorm },
		[DK_ATTRIBUTE_BYTE3] = { DkVtxAttribSize_3x8, DkVtxAttribType_Unorm },
		[DK_ATTRIBUTE_BYTE4] = { DkVtxAttribSize_4x8, DkVtxAttribType_Unorm },
		[DK_ATTRIBUTE_PACKED] = { DkVtxAttribSize_1x32, DkVtxAttribType_Uint },
	};

	if (format >= sizeof(table) / sizeof(table[0]))
		return 0;
	*size = table[format].size;
	*type = table[format].type;
	*bgra = format == DK_ATTRIBUTE_COLOR;
	return 1;
}

static void state_receive(const struct dk_command_state *command)
{
	dk.state = command->state;
	dk.state_valid = 1;
	dk.state_dirty = 1;
}

static void shaders_receive(const struct dk_command_shaders *command)
{
	dk.vertex_shader = command->vertex;
	dk.pixel_shader = command->pixel;
	dk.shaders_dirty = 1;
}

/* the uniform buffers' contents, written with the commands of the draws
that read them */
static void constants_receive(const struct dk_command_constants *command)
{
	if (!dk.uniform_memory || command->first >= 192 || command->count > 192 - command->first)
		return;
	dkCmdBufPushConstants(dk.commands, dk.uniform_gpu + UNIFORM_VERTEX_CONSTANTS, sizeof(struct dk_vertex_constants),
		command->first * 4 * sizeof(float), command->count * 4 * sizeof(float), command->data);
}

static void vertex_parameters_receive(const struct dk_command_vertex_parameters *command)
{
	if (!dk.uniform_memory)
		return;
	dkCmdBufPushConstants(dk.commands, dk.uniform_gpu + UNIFORM_VERTEX_PARAMETERS, sizeof(struct dk_vertex_parameters),
		0, sizeof(command->parameters), &command->parameters);
}

static void pixel_parameters_receive(const struct dk_command_pixel_parameters *command)
{
	if (!dk.uniform_memory)
		return;
	dkCmdBufPushConstants(dk.commands, dk.uniform_gpu + UNIFORM_PIXEL_PARAMETERS, sizeof(struct dk_pixel_parameters),
		0, sizeof(command->parameters), &command->parameters);
}

/* deko3d's form of a vertex format, ready for the draws that follow */
static void format_receive(const struct dk_command_vertex_format *command)
{
	uint32_t binding_of_stream[DK_STREAM_COUNT];
	uint32_t stream, index, constants_binding;

	dk.format = command->format;
	dk.format_valid = 0;
	dk.format_dirty = 1;
	/* the unfed registers' values may be new (a format is sent again when a
	SetVertexData changes one): copied again at the next draw, not taken
	from the copy the frame made for an earlier format */
	dk.constants_gpu = 0;
	memset(&dk.vertex, 0, sizeof(dk.vertex));
	for (stream = 0; stream < DK_STREAM_COUNT; stream++)
	{
		binding_of_stream[stream] = 0;
		if (!((dk.format.stream_mask >> stream) & 1))
			continue;
		binding_of_stream[stream] = dk.vertex.buffer_count;
		dk.vertex.buffers[dk.vertex.buffer_count].stride = dk.format.strides[stream];
		dk.vertex.buffers[dk.vertex.buffer_count].divisor = 0;
		dk.vertex.buffer_count++;
	}
	for (index = 0; index < DK_ATTRIBUTE_COUNT; index++)
	{
		if (dk.format.attributes[index].format == DK_ATTRIBUTE_UNFED ||
			dk.format.attributes[index].format == DK_ATTRIBUTE_UNFED_PACKED)
			dk.vertex.has_constants = 1;
	}
	/* the constants are a stream of their own, with a stride of 0 */
	constants_binding = dk.vertex.buffer_count;
	if (dk.vertex.has_constants)
	{
		if (dk.vertex.buffer_count == DK_STREAM_COUNT)
		{
			host_logf(HOST_LOG_ERROR, "deko3d: a vertex format reads %u streams and has unfed registers; its draws are skipped",
				(unsigned)dk.vertex.buffer_count);
			return;
		}
		dk.vertex.buffers[constants_binding].stride = 0;
		dk.vertex.buffers[constants_binding].divisor = 0;
		dk.vertex.buffer_count++;
	}
	for (index = 0; index < DK_ATTRIBUTE_COUNT; index++)
	{
		const struct dk_attribute *attribute = &dk.format.attributes[index];
		DkVtxAttribState *state = &dk.vertex.attributes[index];
		DkVtxAttribSize size;
		DkVtxAttribType type;
		int bgra, unfed = attribute->format == DK_ATTRIBUTE_UNFED || attribute->format == DK_ATTRIBUTE_UNFED_PACKED;

		if (!attribute_format_make(attribute->format, &size, &type, &bgra) ||
			(!unfed && (attribute->stream >= DK_STREAM_COUNT || !((dk.format.stream_mask >> attribute->stream) & 1))))
		{
			host_logf(HOST_LOG_ERROR, "deko3d: register %u of a vertex format is of kind %u, stream %u; its draws are skipped",
				(unsigned)index, (unsigned)attribute->format, (unsigned)attribute->stream);
			return;
		}
		memset(state, 0, sizeof(*state));
		state->bufferId = unfed ? constants_binding : binding_of_stream[attribute->stream];
		state->offset = unfed ? index * 4 * sizeof(float) : attribute->offset;
		state->size = size;
		state->type = type;
		state->isBgra = bgra;
	}
	dk.format_valid = 1;
}

/* puts the queue's state as the draws were told, if it is not: the viewport
and scissor are clamped to the target they are for, which the guest does
not know the size of from here. 0 if nothing can be drawn with this state (an
empty viewport). */
static int state_apply(void)
{
	const struct dk_draw_state *state = &dk.state;
	const struct target *size_from = dk.color ? dk.color : dk.depth;
	DkViewport viewport;
	DkScissor scissor;
	DkRasterizerState rasterizer;
	DkColorState color;
	DkColorWriteState color_write;
	DkBlendState blend;
	DkDepthStencilState depth_stencil;
	int32_t left, top, right, bottom;

	if (!dk.state_dirty)
		return 1;
	if (state->viewport[2] <= 0 || state->viewport[3] <= 0 || !size_from)
		return 0;
	dk.state_dirty = 0;

	viewport.x = (float)state->viewport[0];
	viewport.y = (float)state->viewport[1];
	viewport.width = (float)state->viewport[2];
	viewport.height = (float)state->viewport[3];
	viewport.near = state->depth_range[0];
	viewport.far = state->depth_range[1];
	dkCmdBufSetViewports(dk.commands, 0, &viewport, 1);
	left = state->scissor[0] < 0 ? 0 : state->scissor[0];
	top = state->scissor[1] < 0 ? 0 : state->scissor[1];
	right = state->scissor[0] + state->scissor[2];
	bottom = state->scissor[1] + state->scissor[3];
	if (right > (int32_t)size_from->surface.width)
		right = (int32_t)size_from->surface.width;
	if (bottom > (int32_t)size_from->surface.height)
		bottom = (int32_t)size_from->surface.height;
	scissor.x = (uint32_t)left;
	scissor.y = (uint32_t)top;
	scissor.width = right > left ? (uint32_t)(right - left) : 0;
	scissor.height = bottom > top ? (uint32_t)(bottom - top) : 0;
	dkCmdBufSetScissors(dk.commands, 0, &scissor, 1);

	dkRasterizerStateDefaults(&rasterizer);
	rasterizer.cullMode = state->cull == DK_CULL_FRONT ? DkFace_Front : state->cull == DK_CULL_BACK ? DkFace_Back :
		DkFace_None;
	rasterizer.frontFace = state->front_face_ccw ? DkFrontFace_CCW : DkFrontFace_CW;
	rasterizer.polygonModeFront = rasterizer.polygonModeBack = state->fill_mode == DK_FILL_LINE ? DkPolygonMode_Line :
		state->fill_mode == DK_FILL_POINT ? DkPolygonMode_Point : DkPolygonMode_Fill;
	rasterizer.depthBiasEnableMask = state->offset_enable ? (DkPolygonFlag_Fill | DkPolygonFlag_Line) : 0;
	dkCmdBufBindRasterizerState(dk.commands, &rasterizer);
	if (state->offset_enable)
		dkCmdBufSetDepthBias(dk.commands, state->offset_units, 0.0f, state->offset_slope);

	dkColorStateDefaults(&color);
	dkColorStateSetBlendEnable(&color, 0, state->blend != 0);
	dkCmdBufBindColorState(dk.commands, &color);
	dkBlendStateDefaults(&blend);
	if (state->blend)
	{
		DkBlendFactor source = blend_factor(state->blend_source), destination = blend_factor(state->blend_destination);

		dkBlendStateSetOps(&blend, blend_op(state->blend_equation), blend_op(state->blend_equation));
		/* (glBlendFunc: the alpha channel's factors are the color's) */
		dkBlendStateSetFactors(&blend, source, destination, source, destination);
		dkCmdBufSetBlendConst(dk.commands, state->blend_color[0], state->blend_color[1], state->blend_color[2],
			state->blend_color[3]);
	}
	dkCmdBufBindBlendStates(dk.commands, 0, &blend, 1);
	dkColorWriteStateDefaults(&color_write);
	dkColorWriteStateSetMask(&color_write, 0, state->color_mask);
	dkCmdBufBindColorWriteState(dk.commands, &color_write);

	dkDepthStencilStateDefaults(&depth_stencil);
	depth_stencil.depthTestEnable = state->depth_test != 0;
	depth_stencil.depthWriteEnable = state->depth_write != 0;
	depth_stencil.depthCompareOp = compare_op(state->depth_function);
	depth_stencil.stencilTestEnable = state->stencil_test != 0;
	if (state->stencil_test)
	{
		/* one set of operations for both faces, as glStencilOp's */
		depth_stencil.stencilFrontFailOp = depth_stencil.stencilBackFailOp = stencil_op(state->stencil_fail);
		depth_stencil.stencilFrontDepthFailOp = depth_stencil.stencilBackDepthFailOp = stencil_op(state->stencil_depth_fail);
		depth_stencil.stencilFrontPassOp = depth_stencil.stencilBackPassOp = stencil_op(state->stencil_pass);
		depth_stencil.stencilFrontCompareOp = depth_stencil.stencilBackCompareOp = compare_op(state->stencil_function);
		dkCmdBufSetStencil(dk.commands, DkFace_FrontAndBack, (uint8_t)state->stencil_write_mask,
			(uint8_t)state->stencil_reference, (uint8_t)state->stencil_mask);
	}
	dkCmdBufBindDepthStencilState(dk.commands, &depth_stencil);
	return 1;
}

/* the shaders and the uniform buffers, which are bound again at the start of
each frame (frame_begin) */
static int shaders_apply(void)
{
	const DkShader *shaders[2];

	if (!dk.uniforms_bound && dk.uniform_memory)
	{
		dkCmdBufBindUniformBuffer(dk.commands, DkStage_Vertex, DK_BINDING_VERTEX_CONSTANTS,
			dk.uniform_gpu + UNIFORM_VERTEX_CONSTANTS, sizeof(struct dk_vertex_constants));
		dkCmdBufBindUniformBuffer(dk.commands, DkStage_Vertex, DK_BINDING_VERTEX_PARAMETERS,
			dk.uniform_gpu + UNIFORM_VERTEX_PARAMETERS, sizeof(struct dk_vertex_parameters));
		dkCmdBufBindUniformBuffer(dk.commands, DkStage_Fragment, DK_BINDING_PIXEL_PARAMETERS,
			dk.uniform_gpu + UNIFORM_PIXEL_PARAMETERS, sizeof(struct dk_pixel_parameters));
		dk.uniforms_bound = 1;
	}
	if (!dk.shaders_dirty)
		return 1;
	shaders[0] = (const DkShader *)host_dk_shader(dk.vertex_shader);
	shaders[1] = (const DkShader *)host_dk_shader(dk.pixel_shader);
	if (!shaders[0] || !shaders[1])
		return 0;
	dkCmdBufBindShaders(dk.commands, DkStageFlag_GraphicsMask, shaders, 2);
	dk.shaders_dirty = 0;
	return 1;
}

/* where a range a draw reads is for the GPU: in the window, where the game
keeps it, or (if it is somewhere else, or crosses from one chunk of the window
into the next) copied into the upload buffer */
static DkGpuAddr range_read(const void *data, uint32_t size)
{
	DkGpuAddr gpu = window_read((uint64_t)(uintptr_t)data, size);

	return gpu ? gpu : upload_copy(data, size);
}

static void draw_problem(const char *what)
{
	if (dk.said_draw_problem < 8)
	{
		dk.said_draw_problem++;
		host_logf(HOST_LOG_ERROR, "deko3d: a draw is skipped: %s", what);
	}
}

static void draw(const struct dk_command_draw *command)
{
	DkBufExtents extents[DK_STREAM_COUNT + 1];
	const unsigned char *inline_data = (const unsigned char *)&command->streams[command->stream_count];
	uint32_t binding = 0, stream, streams = 0;
	DkGpuAddr index_gpu = 0;

	for (stream = 0; stream < DK_STREAM_COUNT; stream++)
		streams += (dk.format.stream_mask >> stream) & 1;
	if (!dk.format_valid || !dk.state_valid || (!dk.color && !dk.depth) || streams != command->stream_count)
	{
		draw_problem("what it needs is not in place");
		return;
	}
	if (!state_apply())
		return;
	if (!shaders_apply())
	{
		draw_problem("a shader is not loaded");
		return;
	}
	if (!textures_apply())
	{
		draw_problem("the textures could not be bound");
		return;
	}
	for (binding = 0; binding < streams; binding++)
	{
		uint32_t size = command->streams[binding][1];
		DkGpuAddr gpu = command->inline_bytes ? upload_copy(inline_data, command->inline_bytes) :
			range_read((const void *)(uintptr_t)command->streams[binding][0], size);

		if (!gpu)
		{
			draw_problem("a stream could not be read");
			return;
		}
		extents[binding].addr = gpu;
		extents[binding].size = command->inline_bytes ? command->inline_bytes : size;
	}
	if (dk.vertex.has_constants)
	{
		if (dk.constants_generation != dk.upload_generation || !dk.constants_gpu)
		{
			dk.constants_gpu = upload_copy(dk.format.constants, sizeof(dk.format.constants));
			dk.constants_generation = dk.upload_generation;
		}
		if (!dk.constants_gpu)
		{
			draw_problem("the constants could not be copied");
			return;
		}
		extents[binding].addr = dk.constants_gpu;
		extents[binding].size = sizeof(dk.format.constants);
		binding++;
	}
	if (command->index_address)
	{
		index_gpu = range_read((const void *)(uintptr_t)command->index_address, command->count * sizeof(uint16_t));
		if (!index_gpu)
		{
			draw_problem("the indices could not be read");
			return;
		}
	}
	if (dk.format_dirty)
	{
		dkCmdBufBindVtxAttribState(dk.commands, dk.vertex.attributes, DK_ATTRIBUTE_COUNT);
		dkCmdBufBindVtxBufferState(dk.commands, dk.vertex.buffers, dk.vertex.buffer_count);
		dk.format_dirty = 0;
	}
	if (binding)
		dkCmdBufBindVtxBuffers(dk.commands, 0, extents, binding);
	if (index_gpu)
	{
		dkCmdBufBindIdxBuffer(dk.commands, DkIdxFormat_Uint16, index_gpu);
		dkCmdBufDrawIndexed(dk.commands, primitive_of(command->primitive), command->count, 1, 0, command->vertex_offset, 0);
	}
	else
	{
		dkCmdBufDraw(dk.commands, primitive_of(command->primitive), command->count, 1, 0, 0);
	}
	dk.recorded = 1;
	tex.draws_unfenced = 1;
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

		if (dk.command_memory_low)
			command_memory_roll();
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
		case DK_COMMAND_STATE:
			state_receive((const struct dk_command_state *)header);
			break;
		case DK_COMMAND_SHADERS:
			shaders_receive((const struct dk_command_shaders *)header);
			break;
		case DK_COMMAND_CONSTANTS:
			constants_receive((const struct dk_command_constants *)header);
			dk.recorded = 1;
			break;
		case DK_COMMAND_VERTEX_PARAMETERS:
			vertex_parameters_receive((const struct dk_command_vertex_parameters *)header);
			dk.recorded = 1;
			break;
		case DK_COMMAND_PIXEL_PARAMETERS:
			pixel_parameters_receive((const struct dk_command_pixel_parameters *)header);
			dk.recorded = 1;
			break;
		case DK_COMMAND_VERTEX_FORMAT:
			format_receive((const struct dk_command_vertex_format *)header);
			break;
		case DK_COMMAND_DRAW:
			draw((const struct dk_command_draw *)header);
			break;
		case DK_COMMAND_TEXTURE:
			texture_receive((const struct dk_command_texture *)header);
			break;
		case DK_COMMAND_TEXTURE_FREE:
			texture_free_receive((const struct dk_command_texture_free *)header);
			break;
		case DK_COMMAND_TEXTURES:
			textures_receive((const struct dk_command_textures *)header);
			break;
		case DK_COMMAND_TEXTURE_ROWS:
			texture_rows_receive((const struct dk_command_texture_rows *)header);
			break;
		case DK_COMMAND_COMPOSITE:
			composite_receive((const struct dk_command_composite *)header);
			break;
		case DK_COMMAND_VISIBILITY_BEGIN:
			visibility_begin();
			break;
		case DK_COMMAND_VISIBILITY_END:
			visibility_end((const struct dk_command_visibility_end *)header);
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
