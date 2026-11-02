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
#include <stdarg.h>
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
/* The end of each piece is left unused. deko3d 0.5.0's dkCmdBufBindVtxBuffers
asks for room for 5 words a buffer and writes 6, so a call that finds
between 5 and 6 words a buffer left runs up to a word a buffer past the
piece; deko3d then gives the next command a new piece, and closes the old
one with those words in it. With the pieces end to end, the next command's
header was written over them before the GPU read them: it took
dkCmdBufDraw's or dkCmdBufBindIdxBuffer's header for the third vertex
buffer's limit, and the 3D engine stopped on SET_VERTEX_STREAM_LIMIT_A(2)
(the kernel's error info, with that header as the value; crowded games on
Longest, after minutes or after seconds). Here what runs past a piece lands
in memory nothing else writes. */
#define COMMAND_PIECE_GUARD 1024
#define IMAGE_BLOCK_SIZE (32 * 1024 * 1024)
#define IMAGE_BLOCK_LIMIT 16
#define TARGET_LIMIT 256
/* the output: 1280x720 in handheld, 1920x1080 docked (host_dk_output_height) */
#define SCREEN_WIDTH 1280
#define SCREEN_HEIGHT 720
#define SCREEN_FULL_WIDTH 1920
#define SCREEN_FULL_HEIGHT 1080
/* a target's size in its own pixels (the screen's targets can be drawn
bigger than the game's: dk_commands.h) */
#define SURFACE_PIXEL_WIDTH(surface) ((surface)->pixel_width ? (surface)->pixel_width : (surface)->width)
#define SURFACE_PIXEL_HEIGHT(surface) ((surface)->pixel_height ? (surface)->pixel_height : (surface)->height)

/* the window is made real a chunk at a time (host_memory.c); the largest
it can be, HALO_GUEST_WINDOW_MAXIMUM (512 MB), is 32 of them */
#define WINDOW_CHUNK_SIZE (16 * 1024 * 1024)
#define WINDOW_CHUNKS (HALO_GUEST_WINDOW_MAXIMUM / WINDOW_CHUNK_SIZE)

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
	/* sampled since the last barrier: drawn into again only once the draws
	that sampled it are done (textures_targets_changing) */
	int sampled;
	/* its image's memory, for the GPU trace */
	DkGpuAddr gpu;
	uint32_t bytes;
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
	/* the GPU's time on a frame, averaged over the last 16 or so, in
	microseconds (host_dk_gpu_frame_us) */
	volatile uint32_t gpu_average_us;
	uint32_t command_rollovers;
	/* set when the last piece is given: the rest of the frame's commands will
	not fit unless the host submits and starts over (host_dk_submit does, between
	commands) */
	int command_memory_low;

	DkMemBlock screen_memory;
	DkImage screen_images[SCREEN_IMAGES];
	/* the swapchain's size (swapchain_make) */
	uint32_t screen_width, screen_height;
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
	/* the GPU faulted: the queue takes nothing more (queue_faulted) */
	int faulted;
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

/* ---------- the record of the commands run, for a GPU fault (debug.gpu_trace)

A GPU fault leaves deko3d's queue in an error state and says nothing more.
So the host keeps each command it runs in a ring of the last TRACE_EVENTS,
and after each one that gives the GPU work it has the GPU write the
command's number into the trace memory as the GPU gets there
(dkCmdBufReportValue): after a fault, the number there is the last command
the GPU finished, and the one it faulted in comes after it. The log then
gets the kernel's account of the fault (the queue's GPU channel's error
notification and error info), where the GPU got to, the commands around it
and what the host held (trace_dump). What a command asks for is checked as
it is run, and anything wrong said at once (trace_check). The game says
which part of the frame it is drawing (DK_COMMAND_MARKER), and each record
has it. */

int host_dk_trace;
/* debug.gpu_fault_test: the GPU made to fault on purpose, a write to memory
it does not have, at the 1200th frame presented: the record checked */
int host_dk_fault_test;

#define TRACE_EVENTS 32768
/* the commands written before the first one the GPU had not finished, and
the most written in all */
#define TRACE_DUMP_BEFORE 64
#define TRACE_DUMP_LIMIT 900
#define TRACE_DUMP_TAIL 100
#define TRACE_CHECK_LIMIT 96
#define TRACE_NAMES 64

/* the kinds of record past the commands' own (DK_COMMAND_*) */
enum
{
	TRACE_SUBMIT = 100,
	TRACE_QUEUE,
	TRACE_ROLLOVER,
	TRACE_COMPOSITE_COPY,
	TRACE_RELEASE,
};

struct trace_event
{
	uint32_t number;
	/* the submission being recorded, and the frames presented before it */
	uint32_t serial;
	uint32_t frame;
	uint16_t kind;
	/* the GPU writes number when it gets past this one */
	uint8_t reported;
	/* the part of the frame (trace.names) */
	uint8_t pass;
	uint32_t a[12];
	uint64_t g[4];
};

static struct
{
	struct trace_event *events;
	/* the next record's number (from 1) */
	uint32_t next;
	/* written by the GPU, each a report of 16 bytes (the value, then a
	timestamp): [0] the last command it finished, [4] the last submission it
	began */
	DkMemBlock memory;
	volatile uint32_t *progress;
	DkGpuAddr progress_gpu;
	char names[TRACE_NAMES][DK_MARKER_NAME_SIZE];
	uint32_t name_count;
	uint8_t pass;
	uint32_t checks;
	/* a fault seen; the record written (on the game thread, at its next
	submission: trace_dump_if_faulted) */
	int dump_pending, dumped;
	/* the texture handles bound for the draws (textures_apply) */
	uint32_t handles[4];
	/* the GPU address texture_write copied from */
	uint64_t texture_source_gpu;
} trace;

static DkMemBlock memory_block(uint32_t size, uint32_t flags, void *storage);

static void trace_initialize(void)
{
	if (!host_dk_trace)
		return;
	trace.events = (struct trace_event *)calloc(TRACE_EVENTS, sizeof(struct trace_event));
	trace.memory = memory_block(DK_MEMBLOCK_ALIGNMENT, DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuUncached, NULL);
	if (!trace.events || !trace.memory)
	{
		host_logf(HOST_LOG_ERROR, "deko3d: no memory for the GPU trace; it is off");
		free(trace.events);
		trace.events = NULL;
		return;
	}
	trace.progress = (volatile uint32_t *)dkMemBlockGetCpuAddr(trace.memory);
	memset((void *)trace.progress, 0, 32);
	trace.progress_gpu = dkMemBlockGetGpuAddr(trace.memory);
	trace.next = 1;
	strcpy(trace.names[0], "(none yet)");
	trace.name_count = 1;
	host_logf(HOST_LOG_INFO, "deko3d: the GPU trace keeps the last %u commands (%u KB)", (unsigned)TRACE_EVENTS,
		(unsigned)(TRACE_EVENTS * sizeof(struct trace_event) / 1024));
}

/* a new record, filled by the caller; a spare one when there is no trace */
static struct trace_event *trace_add(uint32_t kind)
{
	static struct trace_event spare;
	struct trace_event *event;

	if (!trace.events)
	{
		/* (the GPU profile's capture reads it: command_done) */
		memset(&spare, 0, sizeof(spare));
		spare.kind = (uint16_t)kind;
		spare.pass = trace.pass;
		return &spare;
	}
	event = &trace.events[trace.next % TRACE_EVENTS];
	memset(event, 0, sizeof(*event));
	event->number = trace.next++;
	event->serial = dk.serial;
	event->frame = (uint32_t)dk.frames_presented;
	event->kind = (uint16_t)kind;
	event->pass = trace.pass;
	return event;
}

/* the GPU writes the record's number once it has finished what came before */
static void trace_reached(struct trace_event *event)
{
	if (!trace.events || !event->number || dk.faulted)
		return;
	dkCmdBufReportValue(dk.commands, event->number, trace.progress_gpu);
	event->reported = 1;
}

static const char *trace_pass_name(uint32_t pass)
{
	return pass < trace.name_count ? trace.names[pass] : "?";
}

static void trace_check(const char *format, ...) __attribute__((format(printf, 1, 2)));

/* something a command asks for that the GPU may not take, said at once */
static void trace_check(const char *format, ...)
{
	char message[320];
	va_list arguments;

	if (trace.checks >= TRACE_CHECK_LIMIT)
		return;
	if (++trace.checks == TRACE_CHECK_LIMIT)
		host_logf(HOST_LOG_WARN, "deko3d check: (no more of these are said)");
	va_start(arguments, format);
	vsnprintf(message, sizeof(message), format, arguments);
	va_end(arguments);
	host_logf(HOST_LOG_WARN, "deko3d check: %s (command %u, submission %u, frame %lu, %s)", message,
		(unsigned)trace.next, (unsigned)dk.serial, dk.frames_presented, trace_pass_name(trace.pass));
}

static void profile_mark(uint32_t pass);

static void marker_receive(const struct dk_command_marker *command)
{
	char name[DK_MARKER_NAME_SIZE];
	uint32_t index;
	struct trace_event *event;

	memcpy(name, command->name, sizeof(name));
	name[sizeof(name) - 1] = 0;
	for (index = 0; index < trace.name_count; index++)
	{
		if (!strcmp(trace.names[index], name))
			break;
	}
	if (index == trace.name_count)
	{
		if (trace.name_count == TRACE_NAMES)
			index = 0;
		else
			strcpy(trace.names[trace.name_count++], name);
	}
	trace.pass = (uint8_t)index;
	profile_mark(index);
	event = trace_add(DK_COMMAND_MARKER);
	event->a[0] = index;
}

static void trace_dump(void);
static void trace_held(void);

/* the record of a fault written, once, on the game thread */
static void trace_dump_if_faulted(void)
{
	if (trace.dump_pending && !trace.dumped)
	{
		trace.dumped = 1;
		trace_dump();
	}
}

/* ---------- the GPU's time by part of the frame (debug.gpu_profile)

The GPU writes a timestamp (dkCmdBufReportCounter) where each part of the
frame the game names begins (DK_COMMAND_MARKER), and where each submission
ends and the next begins: the time from one to the next is the part's, and
from a submission's end to the next's start the GPU's wait for the CPU. A
frame slot's timestamps are read once its fence has passed (frame_begin).
What a frame asks of the GPU is counted as well - draws, barriers, target
changes, clears, texels written - and every PROFILE_FRAMES frames the
averages go into the log, the parts that took longest first. */

int host_dk_profile;

#define PROFILE_MARKS 512
#define PROFILE_FRAMES 300
/* (the pass of the time between submissions) */
#define PROFILE_IDLE TRACE_NAMES

struct profile_counts
{
	uint32_t draws, skipped, targets, clears, barriers_full, barriers_fragments, barriers_other;
	uint32_t texture_writes, rows, composite_copies, submissions, rollovers;
	uint64_t texture_bytes, upload_bytes;
};

static struct
{
	DkMemBlock memory;
	const char *cpu;
	DkGpuAddr gpu;
	uint8_t pass[FRAMES][PROFILE_MARKS];
	uint32_t count[FRAMES];
	/* the time of each part (PROFILE_IDLE: between submissions) over the
	frames timed, and how many */
	uint64_t ns[TRACE_NAMES + 1];
	uint32_t timed_frames;
	uint32_t lost_marks;
	struct profile_counts frame, total;
	uint32_t frames;
} profile;

static void profile_initialize(void)
{
	if (!host_dk_profile)
		return;
	profile.memory = memory_block(FRAMES * PROFILE_MARKS * 16, DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached,
		NULL);
	if (!profile.memory)
	{
		host_logf(HOST_LOG_ERROR, "deko3d: no memory for the GPU profile; it is off");
		return;
	}
	profile.cpu = (const char *)dkMemBlockGetCpuAddr(profile.memory);
	profile.gpu = dkMemBlockGetGpuAddr(profile.memory);
	memset((void *)profile.cpu, 0, FRAMES * PROFILE_MARKS * 16);
	host_logf(HOST_LOG_INFO, "deko3d: the GPU profile is on (debug.gpu_profile): every %u frames, the GPU's time by "
		"part of the frame", (unsigned)PROFILE_FRAMES);
}

/* the GPU's time from here on is the part pass's */
static void profile_mark(uint32_t pass)
{
	uint32_t index;

	if (!profile.memory || dk.faulted)
		return;
	index = profile.count[dk.frame];
	if (index >= PROFILE_MARKS)
		return;
	dkCmdBufReportCounter(dk.commands, DkCounter_Timestamp,
		profile.gpu + ((DkGpuAddr)dk.frame * PROFILE_MARKS + index) * 16);
	profile.pass[dk.frame][index] = (uint8_t)pass;
	profile.count[dk.frame] = index + 1;
}

/* (frame_begin, the slot's fence passed) the slot's last frame timed */
static void profile_frame_begin(void)
{
	const uint64_t *reports;
	uint32_t index, count = profile.count[dk.frame];

	if (!profile.memory || !count)
		return;
	reports = (const uint64_t *)(profile.cpu + (size_t)dk.frame * PROFILE_MARKS * 16);
	for (index = 0; index + 1 < count; index++)
	{
		uint64_t start = reports[index * 2 + 1], end = reports[index * 2 + 3];

		/* (a mark recorded and then thrown away, by a rollover, is 0) */
		if (!start || !end || end < start)
		{
			profile.lost_marks++;
			continue;
		}
		profile.ns[profile.pass[dk.frame][index]] += dkTimestampToNs(end - start);
	}
	profile.timed_frames++;
	memset((void *)reports, 0, (size_t)count * 16);
	profile.count[dk.frame] = 0;
}

static void capture_arm(void);

static void barrier(DkBarrier mode, uint32_t invalidate)
{
	if (mode == DkBarrier_Full)
		profile.frame.barriers_full++;
	else if (mode == DkBarrier_Fragments)
		profile.frame.barriers_fragments++;
	else
		profile.frame.barriers_other++;
	dkCmdBufBarrier(dk.commands, mode, invalidate);
}

/* (present) the frame's counts added up, and every PROFILE_FRAMES frames
the averages logged */
static void profile_frame_end(void)
{
	struct profile_counts *total = &profile.total, *frame = &profile.frame;
	uint32_t order[TRACE_NAMES + 1], count = 0, index, frames, timed;
	uint64_t all = 0;
	char line[1024];
	int length;

	if (!profile.memory)
	{
		memset(frame, 0, sizeof(*frame));
		return;
	}
	frame->upload_bytes += dk.upload_used;
	total->draws += frame->draws;
	total->skipped += frame->skipped;
	total->targets += frame->targets;
	total->clears += frame->clears;
	total->barriers_full += frame->barriers_full;
	total->barriers_fragments += frame->barriers_fragments;
	total->barriers_other += frame->barriers_other;
	total->texture_writes += frame->texture_writes;
	total->rows += frame->rows;
	total->composite_copies += frame->composite_copies;
	total->submissions += frame->submissions;
	total->rollovers += frame->rollovers;
	total->texture_bytes += frame->texture_bytes;
	total->upload_bytes += frame->upload_bytes;
	memset(frame, 0, sizeof(*frame));
	if (++profile.frames < PROFILE_FRAMES)
		return;
	frames = profile.frames;
	timed = profile.timed_frames ? profile.timed_frames : 1;
	for (index = 0; index <= TRACE_NAMES; index++)
	{
		all += profile.ns[index];
		if (profile.ns[index])
			order[count++] = index;
	}
	/* (the longest first) */
	for (index = 1; index < count; index++)
	{
		uint32_t moving = order[index], at = index;

		while (at > 0 && profile.ns[order[at - 1]] < profile.ns[moving])
		{
			order[at] = order[at - 1];
			at--;
		}
		order[at] = moving;
	}
	length = snprintf(line, sizeof(line), "deko3d profile: GPU %.2f ms a frame:", (double)all / timed / 1e6);
	for (index = 0; index < count && length > 0 && length < (int)sizeof(line) - 64; index++)
	{
		double ms = (double)profile.ns[order[index]] / timed / 1e6;

		if (ms < 0.01)
			break;
		length += snprintf(line + length, sizeof(line) - (size_t)length, " %s %.2f%s",
			order[index] == PROFILE_IDLE ? "(between submissions)" : trace_pass_name(order[index]), ms,
			index + 1 < count ? "," : "");
	}
	host_logf(HOST_LOG_INFO, "%s", line);
	host_logf(HOST_LOG_INFO, "deko3d profile: a frame: %.0f draws (%.1f skipped), %.1f target changes, %.1f clears, "
		"barriers %.1f full %.1f fragments %.1f other, %.1f texture writes and %.1f row writes (%.0f KB), %.2f composite "
		"copies, %.1f submissions, %.0f KB uploaded, %.2f rollovers; %u marks lost",
		(double)total->draws / frames, (double)total->skipped / frames, (double)total->targets / frames,
		(double)total->clears / frames, (double)total->barriers_full / frames, (double)total->barriers_fragments / frames,
		(double)total->barriers_other / frames, (double)total->texture_writes / frames, (double)total->rows / frames,
		(double)total->texture_bytes / frames / 1024.0, (double)total->composite_copies / frames,
		(double)total->submissions / frames, (double)total->upload_bytes / frames / 1024.0,
		(double)total->rollovers / frames, (unsigned)profile.lost_marks);
	memset(total, 0, sizeof(*total));
	memset(profile.ns, 0, sizeof(profile.ns));
	profile.frames = profile.timed_frames = profile.lost_marks = 0;
	capture_arm();
}

/* ---------- one frame timed command by command (debug.gpu_capture)

With the profile, the frame after each window of PROFILE_FRAMES is timed
command by command: the GPU writes a timestamp as it finishes each draw,
clear, texel write, copy and the present (command_done), and the time from
one to the next is the command's. The GPU overlaps neighbouring draws, so a
command's time is what it added, not what it took alone; summed, they are
the frame's. Once the frame's fence has passed (frame_begin) the log gets
its time by pixel shader, vertex shader, target and kind of command, and
its longest commands. A command that follows a submission's end carries the
GPU's wait for the CPU, and is counted apart. */

int host_dk_capture;
int host_dk_no_compression, host_dk_no_target_barriers, host_dk_lod_bias;

#define CAPTURE_LIMIT 6144
#define CAPTURE_GROUPS 512
#define CAPTURE_LONGEST 16

struct capture_entry
{
	uint16_t kind;
	uint8_t pass;
	uint8_t after_gap;
	uint32_t vertex_shader, pixel_shader;
	uint32_t count, primitive;
	uint32_t color, depth;
	uint32_t texture0;
	/* the draw's blend (on, source, destination, equation, as the guest's
	state says them) and depth test and write */
	uint32_t blend, blend_source, blend_destination, blend_equation, depth_state;
};

static struct
{
	DkMemBlock memory;
	const uint64_t *reports;
	DkGpuAddr gpu;
	struct capture_entry entries[CAPTURE_LIMIT];
	uint32_t count;
	int armed, recording, waiting;
	int slot;
	int gap_next;
	unsigned long frame;
	/* armed by a slow frame: its GPU time (capture_slow_frame), else 0 */
	uint64_t slow_ns;
	uint64_t slow_ns_recording;
} capture;

static void capture_initialize(void)
{
	if (!host_dk_capture || !host_dk_profile)
		return;
	capture.memory = memory_block((CAPTURE_LIMIT + 1) * 16, DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached,
		NULL);
	if (!capture.memory)
		return;
	capture.reports = (const uint64_t *)dkMemBlockGetCpuAddr(capture.memory);
	capture.gpu = dkMemBlockGetGpuAddr(capture.memory);
	host_logf(HOST_LOG_INFO, "deko3d: the GPU capture is on (debug.gpu_capture): a frame of every %u timed command "
		"by command", (unsigned)PROFILE_FRAMES);
}

/* (frame_begin, the slot's fence passed) a captured frame read, and the
next started if one is asked for */
static void capture_frame_begin(void);

/* (the profile's window ended) the next frame to be captured */
static void capture_arm(void)
{
	if (capture.memory && !capture.recording && !capture.waiting)
		capture.armed = 1;
}

/* A frame the GPU took more than CAPTURE_SLOW_NS on: the next one captured
too - a dip lasts frames (a hit's effects, a fight's lights) - at most once
every CAPTURE_SLOW_GAP frames, and said as one */
#define CAPTURE_SLOW_NS 18000000ull
#define CAPTURE_SLOW_GAP 120

static void capture_slow_frame(uint64_t busy_ns)
{
	static unsigned long last;

	if (!capture.memory || busy_ns < CAPTURE_SLOW_NS || capture.armed || capture.recording || capture.waiting ||
		(last && dk.frames_presented - last < CAPTURE_SLOW_GAP))
		return;
	last = dk.frames_presented;
	capture.armed = 1;
	capture.slow_ns = busy_ns;
}

/* a command that gives the GPU work has been recorded: the trace's report
and the capture's timestamp */
static void command_done(struct trace_event *event)
{
	trace_reached(event);
	if (capture.recording && dk.frame == capture.slot && capture.count < CAPTURE_LIMIT && !dk.faulted)
	{
		struct capture_entry *entry = &capture.entries[capture.count];

		dkCmdBufReportCounter(dk.commands, DkCounter_Timestamp, capture.gpu + (DkGpuAddr)(capture.count + 1) * 16);
		entry->kind = event->kind;
		entry->pass = event->pass;
		entry->after_gap = (uint8_t)capture.gap_next;
		capture.gap_next = 0;
		entry->vertex_shader = event->kind == DK_COMMAND_DRAW ? event->a[6] : 0;
		entry->pixel_shader = event->kind == DK_COMMAND_DRAW ? event->a[7] : 0;
		entry->count = event->kind == DK_COMMAND_DRAW ? event->a[1] : 0;
		entry->primitive = event->kind == DK_COMMAND_DRAW ? event->a[0] : 0;
		entry->color = dk.color ? dk.color->surface.data : 0;
		entry->depth = dk.depth ? dk.depth->surface.data : 0;
		entry->texture0 = event->kind == DK_COMMAND_DRAW ? event->a[8] : 0;
		entry->blend = dk.state.blend;
		entry->blend_source = dk.state.blend_source;
		entry->blend_destination = dk.state.blend_destination;
		entry->blend_equation = dk.state.blend_equation;
		entry->depth_state = dk.state.depth_test | dk.state.depth_write << 1;
		capture.count++;
	}
}

/* Whether a GPU fault has put the queue in an error state. deko3d ends the
program at the next call that needs such a queue (dkQueueSubmitCommands,
dkQueueSignalFence, dkQueueWaitIdle, the swapchain's), with nothing said and
whatever cbDebug is: release deko3d calls diagAbortWithResult. So once it is,
nothing more goes to the queue: what is recorded is thrown away, every
submission counts as finished (nothing waits for a GPU that has stopped),
and nothing is presented. The game goes on, the picture stops, and the log
says at which submission and frame. */
static int queue_faulted(void)
{
	if (dk.faulted)
		return 1;
	if (!dk.queue || !dkQueueIsInErrorState(dk.queue))
		return 0;
	dk.faulted = 1;
	trace.dump_pending = 1;
	host_logf(HOST_LOG_ERROR, "deko3d: the GPU faulted (the queue is in an error state) by submission %u, frame %lu; "
		"nothing more is drawn or presented", (unsigned)dk.serial, dk.frames_presented);
	return 1;
}

/* (under dk_lock) every submission finished, and no fence waited for: the
queue has faulted, and the GPU reaches none of them */
static void fences_forget(void)
{
	int index;

	for (index = 0; index < FRAMES; index++)
		dk.fence_pending[index] = 0;
	dk.serial_retired = dk.serial;
}

/* the queue drained, unless it has faulted (queue_faulted) */
/* what the render thread is doing, for its watchdog (render_watchdog) */
static const char *volatile render_where = "starting";

static void queue_wait_idle(void)
{
	const char *was = render_where;

	render_where = "waiting for the GPU to go idle (dkQueueWaitIdle)";
	if (!queue_faulted())
		dkQueueWaitIdle(dk.queue);
	render_where = was;
}

/* gives the command buffer the next piece of this frame's command memory */
static void command_piece_add(void)
{
	dkCmdBufAddMemory(dk.commands, dk.command_memory,
		(uint32_t)dk.frame * COMMAND_MEMORY_SIZE + dk.command_pieces * COMMAND_PIECE_SIZE,
		COMMAND_PIECE_SIZE - COMMAND_PIECE_GUARD);
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
	if (dk.command_pieces == COMMAND_PIECES || needed > COMMAND_PIECE_SIZE - COMMAND_PIECE_GUARD)
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
		DkImageFlags_UsageRender | DkImageFlags_Usage2DEngine | (host_dk_no_compression ? 0 : DkImageFlags_HwCompression),
		SURFACE_PIXEL_WIDTH(surface), SURFACE_PIXEL_HEIGHT(surface));
	if (!image_memory(&layout, &block, &offset))
	{
		host_logf(HOST_LOG_ERROR, "deko3d: no image memory for the %ux%u target at %08x",
			(unsigned)SURFACE_PIXEL_WIDTH(surface), (unsigned)SURFACE_PIXEL_HEIGHT(surface), (unsigned)surface->data);
		return NULL;
	}
	target = &dk.targets[dk.target_count++];
	target->surface = *surface;
	dkImageInitialize(&target->image, &layout, block, offset);
	target->gpu = dkMemBlockGetGpuAddr(block) + offset;
	target->bytes = (uint32_t)dkImageLayoutGetSize(&layout);
	host_logf(HOST_LOG_INFO, "deko3d: %s target %08x, %ux%u (%ux%u pixels)",
		surface->kind == DK_SURFACE_DEPTH ? "depth" : "color", (unsigned)surface->data, (unsigned)surface->width,
		(unsigned)surface->height, (unsigned)SURFACE_PIXEL_WIDTH(surface), (unsigned)SURFACE_PIXEL_HEIGHT(surface));
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

	render_where = what;
	while (dkFenceWait(&dk.fences[index], 2000000000LL) != DkResult_Success)
	{
		if (queue_faulted())
			return;
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

/* ---------- the slow frames' figures (host_sdl2.c's frame_statistics)

What each thread waited for since the last report: the game thread for room
in the render queue (host_dk_submit), the render thread for a screen image
(dkQueueAcquireImage) and for the GPU to give back a frame's slot
(frame_begin's fence), and how many frames it presented. Written by their
own threads, read by the game thread's report: a figure a frame late is no
matter in a line about a slow frame. */
/* (the frame's pace, below with host_dk_frame_pacing) */
static void pace_present_run(void);

static struct
{
	uint64_t submit_wait_ns;
	uint32_t streams;
	volatile uint64_t acquire_max_ns, fence_max_ns;
	volatile uint32_t presents;
	volatile uint32_t gpu_last_us;
} slow;

static uint64_t slow_now_ns(void)
{
	return armTicksToNs(armGetSystemTick());
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

static void capture_slow_frame(uint64_t busy_ns);

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
		/* (an average over about 16 frames, for host_dk_gpu_frame_us) */
		{
			uint32_t busy_us = (uint32_t)(busy / 1000);

			slow.gpu_last_us = busy_us;

			dk.gpu_average_us = dk.gpu_average_us ? dk.gpu_average_us + ((int32_t)busy_us - (int32_t)dk.gpu_average_us) / 16
				: busy_us;
		}
		capture_slow_frame(busy);
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

	if (!dk.timing_commands || !dk.timing_armed || pair >= TIMING_PAIRS || queue_faulted())
		return 0;
	reports = dkMemBlockGetGpuAddr(dk.timing_memory) + gpu_timing_region(dk.frame) + pair * 2 * 16;
	dkCmdBufReportCounter(dk.timing_commands, DkCounter_Timestamp, reports);
	dkQueueSubmitCommands(dk.queue, dkCmdBufFinishList(dk.timing_commands));
	dkCmdBufReportCounter(dk.commands, DkCounter_Timestamp, reports + 16);
	dk.timing_count[dk.frame] = pair + 1;
	return 1;
}

void host_dk_frame_report(struct host_dk_frame_report *report)
{
	report->submit_wait_us = (uint32_t)(slow.submit_wait_ns / 1000);
	report->streams = slow.streams;
	report->acquire_max_us = (uint32_t)(slow.acquire_max_ns / 1000);
	report->fence_max_us = (uint32_t)(slow.fence_max_ns / 1000);
	report->presents = slow.presents;
	report->gpu_last_us = slow.gpu_last_us;
	report->gpu_average_us = dk.gpu_average_us;
	slow.submit_wait_ns = 0;
	slow.streams = 0;
	slow.acquire_max_ns = slow.fence_max_ns = 0;
	slow.presents = 0;
}

static void frame_begin(void)
{
	/* (under the lock: the guest's other threads read the fences through
	host_dk_retired) */
	pthread_mutex_lock(&dk_lock);
	if (queue_faulted())
		fences_forget();
	if (dk.fence_pending[dk.frame])
	{
		uint64_t waited = slow_now_ns();

		fence_wait_said(dk.frame, "the next frame");
		waited = slow_now_ns() - waited;
		if (waited > slow.fence_max_ns)
			slow.fence_max_ns = waited;
		dk.fence_pending[dk.frame] = 0;
		if (dk.fence_serial[dk.frame] > dk.serial_retired)
			dk.serial_retired = dk.fence_serial[dk.frame];
	}
	pthread_mutex_unlock(&dk_lock);
	gpu_timing_frame_begin();
	profile_frame_begin();
	command_memory_begin();
	capture_frame_begin();
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

	trace_add(TRACE_ROLLOVER)->a[0] = used;
	if (dk.recorded)
		commands_submit_partial();
	pthread_mutex_lock(&dk_lock);
	if (queue_faulted())
		fences_forget();
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
	profile.frame.rollovers++;
	profile_mark(trace.pass);
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
	struct trace_event *event = trace_add(TRACE_QUEUE);

	event->a[0] = finished;
	event->a[1] = command_memory_used();
	event->a[2] = dk.upload_used;
	event->a[3] = finished != dk.serial;
	/* (a faulted queue takes nothing: what is recorded goes, and the
	submission counts as finished) */
	if (queue_faulted())
	{
		command_memory_begin();
		pthread_mutex_lock(&dk_lock);
		fences_forget();
		pthread_mutex_unlock(&dk_lock);
		dk.cleaned_count = 0;
		dk.recorded = 0;
		return;
	}
	profile_mark(PROFILE_IDLE);
	profile.frame.submissions++;
	capture.gap_next = 1;
	gpu_timing_bracket();
	dkQueueSubmitCommands(dk.queue, dkCmdBufFinishList(dk.commands));
	if (queue_faulted())
	{
		pthread_mutex_lock(&dk_lock);
		fences_forget();
		pthread_mutex_unlock(&dk_lock);
		dk.cleaned_count = 0;
		dk.recorded = 0;
		return;
	}
	pthread_mutex_lock(&dk_lock);
	dkQueueSignalFence(dk.queue, &dk.fences[dk.frame], true);
	dk.fence_pending[dk.frame] = 1;
	dk.fence_serial[dk.frame] = finished;
	dk.serial_fenced = finished;
	pthread_mutex_unlock(&dk_lock);
	/* Sent to the GPU now: dkQueueSubmitCommands only queues them, and until
	a present or a wait for the queue nothing reaches the GPU. A submission
	the game ends mid-frame to wait for (a resource lock: c40's opening
	draws into a 320x240 target and waits for it) was then never run, the
	game waited for it forever, and the picture froze. */
	dkQueueFlush(dk.queue);
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
	profile_mark(trace.pass);
}

/* (under dk_lock) */
static void fences_poll(void)
{
	int index;

	if (queue_faulted())
	{
		fences_forget();
		return;
	}
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

	/* (the fences polled when the lock is free; while the render thread
	holds it, the count as it last left it, which only ever falls behind:
	a buffer is busy a little longer, and renamed or waited for, never
	taken early. Waiting for the lock was where the game thread spent its
	time at the Halo level's falls, 79% of it zoomed, as each dynamic
	vertex buffer's lock asked here) */
	if (pthread_mutex_trylock(&dk_lock) != 0)
		return __atomic_load_n(&dk.serial_retired, __ATOMIC_ACQUIRE);
	if (dk.ready)
		fences_poll();
	retired = dk.serial_retired;
	pthread_mutex_unlock(&dk_lock);
	return retired;
}

/* the queue drained, before a map's memory goes or the process exits: a
submission still running reads that memory */
static void render_queue_drain(void);

void host_dk_idle(void)
{
	render_queue_drain();
	if (dk.ready)
		queue_wait_idle();
}

/* the guest's, for display.dynamic_resolution (d3d8_dk.c, between frames):
the GPU's time on a frame, averaged over the last 16 or so, in
microseconds; 0 before the first */
unsigned int host_dk_gpu_frame_us(void)
{
	return dk.gpu_average_us;
}

/* the height host_dk_output_height last chose: 720 in handheld, 1080 docked */
static uint32_t output_height = SCREEN_HEIGHT;

/* the guest's, as it chooses its screen's scale (d3d8_dk.c, between frames):
the height presented at, 1080 while the console is docked and 720 in
handheld. present remakes the swapchain when it changes */
unsigned int host_dk_output_height(void)
{
	output_height = appletGetOperationMode() == AppletOperationMode_Console ? SCREEN_FULL_HEIGHT : SCREEN_HEIGHT;
	return output_height;
}

/* the swapchain's images and the swapchain, at output_height */
static void swapchain_make(void)
{
	DkSwapchainMaker swapchain_maker;
	DkImageLayout layout;
	DkImage const *screen_images[SCREEN_IMAGES];
	uint32_t image_size;
	int index;

	dk.screen_height = output_height;
	dk.screen_width = output_height == SCREEN_FULL_HEIGHT ? SCREEN_FULL_WIDTH : SCREEN_WIDTH;
	layout_make(&layout, DkImageFormat_RGBA8_Unorm,
		DkImageFlags_UsageRender | DkImageFlags_UsagePresent | DkImageFlags_Usage2DEngine | DkImageFlags_HwCompression,
		dk.screen_width, dk.screen_height);
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
}

/* the swapchain made again at output_height, once the GPU has finished with
its images (the console docked or undocked) */
static void swapchain_remake(void)
{
	queue_wait_idle();
	if (dk.faulted)
		return;
	dkSwapchainDestroy(dk.swapchain);
	dkMemBlockDestroy(dk.screen_memory);
	swapchain_make();
	host_logf(HOST_LOG_INFO, "deko3d: presenting at %ux%u", (unsigned)dk.screen_width, (unsigned)dk.screen_height);
}

static int initialize(void)
{
	DkDeviceMaker device_maker;
	DkQueueMaker queue_maker;
	DkCmdBufMaker command_maker;

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

	swapchain_make();
	host_logf(HOST_LOG_INFO, "deko3d: device ready, presenting at %ux%u", (unsigned)dk.screen_width,
		(unsigned)dk.screen_height);

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
	trace_initialize();
	profile_initialize();
	capture_initialize();
	dk.uniform_memory = memory_block(UNIFORM_MEMORY_SIZE, DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached, NULL);
	if (dk.uniform_memory)
		dk.uniform_gpu = dkMemBlockGetGpuAddr(dk.uniform_memory);
	else
		host_logf(HOST_LOG_ERROR, "deko3d: no uniform buffer; nothing will be drawn");

	frame_begin();
	return 1;
}

static void textures_targets_changing(const struct target *color, const struct target *depth);

static void targets_bind(const struct dk_command_targets *command)
{
	DkImageView color_view, depth_view;
	DkImageView const *colors[1] = { &color_view };
	struct target *size_from;

	struct trace_event *event = trace_add(DK_COMMAND_TARGETS);

	dk.color = target_get(&command->color);
	dk.depth = target_get(&command->depth);
	textures_targets_changing(dk.color, dk.depth);
	profile.frame.targets++;
	event->a[0] = command->color.data;
	event->a[1] = SURFACE_PIXEL_WIDTH(&command->color) | SURFACE_PIXEL_HEIGHT(&command->color) << 16;
	event->a[2] = command->color.kind | command->depth.kind << 8;
	event->a[3] = command->depth.data;
	event->a[4] = SURFACE_PIXEL_WIDTH(&command->depth) | SURFACE_PIXEL_HEIGHT(&command->depth) << 16;
	event->a[5] = dk.color ? (uint32_t)(dk.color - dk.targets) + 1 : 0;
	event->a[6] = dk.depth ? (uint32_t)(dk.depth - dk.targets) + 1 : 0;
	event->a[7] = command->color.width | command->color.height << 16;
	event->g[0] = dk.color ? dk.color->gpu : 0;
	event->g[1] = dk.depth ? dk.depth->gpu : 0;
	if (command->color.kind != DK_SURFACE_NONE && !dk.color)
		trace_check("color target %08x (%ux%u) has no image", (unsigned)command->color.data,
			(unsigned)command->color.width, (unsigned)command->color.height);
	if (dk.color && dk.depth && (SURFACE_PIXEL_WIDTH(&dk.color->surface) > SURFACE_PIXEL_WIDTH(&dk.depth->surface) ||
		SURFACE_PIXEL_HEIGHT(&dk.color->surface) > SURFACE_PIXEL_HEIGHT(&dk.depth->surface)))
		trace_check("color target %08x (%ux%u pixels) is bigger than its depth target %08x (%ux%u pixels)",
			(unsigned)dk.color->surface.data, (unsigned)SURFACE_PIXEL_WIDTH(&dk.color->surface),
			(unsigned)SURFACE_PIXEL_HEIGHT(&dk.color->surface), (unsigned)dk.depth->surface.data,
			(unsigned)SURFACE_PIXEL_WIDTH(&dk.depth->surface), (unsigned)SURFACE_PIXEL_HEIGHT(&dk.depth->surface));
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
		DkViewport viewport = { 0.0f, 0.0f, (float)SURFACE_PIXEL_WIDTH(&size_from->surface),
			(float)SURFACE_PIXEL_HEIGHT(&size_from->surface), 0.0f, 1.0f };
		DkScissor scissor = { 0, 0, SURFACE_PIXEL_WIDTH(&size_from->surface), SURFACE_PIXEL_HEIGHT(&size_from->surface) };

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

	struct trace_event *event = trace_add(DK_COMMAND_CLEAR);
	const struct target *size_from = dk.color ? dk.color : dk.depth;

	event->a[0] = command->flags;
	event->a[1] = command->rectangle_count;
	if (command->rectangle_count)
	{
		event->a[2] = command->rectangles[0][0];
		event->a[3] = command->rectangles[0][1];
		event->a[4] = command->rectangles[0][2];
		event->a[5] = command->rectangles[0][3];
	}
	if (size_from)
	{
		event->a[6] = SURFACE_PIXEL_WIDTH(&size_from->surface) | SURFACE_PIXEL_HEIGHT(&size_from->surface) << 16;
		for (index = 0; index < command->rectangle_count; index++)
		{
			const uint32_t *rectangle = command->rectangles[index];

			if ((uint64_t)rectangle[0] + rectangle[2] > SURFACE_PIXEL_WIDTH(&size_from->surface) ||
				(uint64_t)rectangle[1] + rectangle[3] > SURFACE_PIXEL_HEIGHT(&size_from->surface))
				trace_check("a clear of %u,%u %ux%u is outside its %ux%u target %08x", (unsigned)rectangle[0],
					(unsigned)rectangle[1], (unsigned)rectangle[2], (unsigned)rectangle[3],
					(unsigned)SURFACE_PIXEL_WIDTH(&size_from->surface), (unsigned)SURFACE_PIXEL_HEIGHT(&size_from->surface),
					(unsigned)size_from->surface.data);
		}
	}
	profile.frame.clears++;
	if (!dk.color)
		mask = 0;
	if (!mask && !depth && !stencil)
		return;
	/* A clear is drawn into the current viewport. Binding the target left
	that as the whole target, so a later split-screen window's clear would
	erase the earlier one (player 1 is drawn first). The rectangle is the
	window, and the viewport is set to it as the scissor is; the next draw
	puts both back (state_dirty). */
	dk.state_dirty = 1;
	for (index = 0; index < command->rectangle_count; index++)
	{
		const uint32_t *rectangle = command->rectangles[index];
		DkViewport viewport = { (float)rectangle[0], (float)rectangle[1], (float)rectangle[2],
			(float)rectangle[3], 0.0f, 1.0f };
		DkScissor scissor = { rectangle[0], rectangle[1], rectangle[2], rectangle[3] };

		if (!rectangle[2] || !rectangle[3])
			continue;
		dkCmdBufSetViewports(dk.commands, 0, &viewport, 1);
		dkCmdBufSetScissors(dk.commands, 0, &scissor, 1);
		if (mask)
			dkCmdBufClearColorFloat(dk.commands, 0, mask, command->color[0], command->color[1], command->color[2],
				command->color[3]);
		if (depth || stencil)
			dkCmdBufClearDepthStencil(dk.commands, depth, command->depth, stencil ? 0xff : 0,
				(uint8_t)command->stencil);
	}
	command_done(event);
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
	struct trace_event *event = trace_add(DK_COMMAND_PRESENT);
	int slot;

	event->a[0] = command->back_buffer.data;
	event->a[1] = SURFACE_PIXEL_WIDTH(&command->back_buffer) | SURFACE_PIXEL_HEIGHT(&command->back_buffer) << 16;
	event->a[2] = command->screenshot;
	event->g[0] = back_buffer ? back_buffer->gpu : 0;
	/* A GPU fault puts the queue in an error state, and deko3d then ends the
	program at the next call that needs the queue - the swapchain's acquire,
	here - with nothing said about the fault. Say it, with the frame, and
	stop presenting: the game goes on, the picture stops, the log has the
	frame the GPU faulted in */
	if (queue_faulted())
	{
		/* what was recorded is thrown away, so the command memory does not
		fill with frames that are never submitted (frame_begin would wait for
		a fence the faulted GPU never reaches) */
		command_memory_begin();
		dk.recorded = 0;
		return;
	}
	if (output_height != dk.screen_height)
		swapchain_remake();
	/* the frame's own commands go to the GPU before a screen image is asked
	for, which waits while the display holds them all: the GPU starts on the
	frame meanwhile, rather than after the wait (Android's Vulkan renderer
	gained this way, port/android/host/host_vk_render.c's
	frame_submit_split). A submission mid-stream, as a lock's wait makes one:
	its fence does not yet say the stream is finished, and the present
	submits the rest */
	if (dk.recorded)
		commands_submit_partial();
	{
		uint64_t waited = slow_now_ns();

		render_where = "acquiring a screen image (dkQueueAcquireImage)";
		slot = dkQueueAcquireImage(dk.queue, dk.swapchain);
		render_where = "presenting";
		waited = slow_now_ns() - waited;
		if (waited > slow.acquire_max_ns)
			slow.acquire_max_ns = waited;
	}
	DkImageView screen_view;
	DkImageView const *screen_views[1] = { &screen_view };
	DkScissor scissor = { 0, 0, dk.screen_width, dk.screen_height };
	DkViewport viewport = { 0.0f, 0.0f, (float)dk.screen_width, (float)dk.screen_height, 0.0f, 1.0f };

	dkImageViewDefaults(&screen_view, &dk.screen_images[slot]);
	dkCmdBufBindRenderTargets(dk.commands, screen_views, 1, NULL);
	dkCmdBufSetViewports(dk.commands, 0, &viewport, 1);
	dkCmdBufSetScissors(dk.commands, 0, &scissor, 1);
	dkCmdBufClearColorFloat(dk.commands, 0, DkColorMask_RGBA, 0.0f, 0.0f, 0.0f, 1.0f);
	if (back_buffer)
	{
		/* letterboxed to the back buffer's shape */
		uint32_t source_width = SURFACE_PIXEL_WIDTH(&back_buffer->surface);
		uint32_t source_height = SURFACE_PIXEL_HEIGHT(&back_buffer->surface);
		uint32_t width = dk.screen_width, height = dk.screen_width * source_height / source_width;
		DkImageView source;
		DkImageRect from = { 0, 0, 0, source_width, source_height, 1 };
		DkImageRect to;

		if (height > dk.screen_height)
		{
			height = dk.screen_height;
			width = dk.screen_height * source_width / source_height;
		}
		to.x = (dk.screen_width - width) / 2;
		to.y = (dk.screen_height - height) / 2;
		to.z = 0;
		to.width = width;
		to.height = height;
		to.depth = 1;
		dkImageViewDefaults(&source, &back_buffer->image);
		barrier(DkBarrier_Fragments, 0);
		if (command->screenshot && readback_make(source_width, source_height))
		{
			DkImageView readback;

			dkImageViewDefaults(&readback, &dk.readback_image);
			dkCmdBufBlitImage(dk.commands, &source, &from, &readback, &from, 0, 0);
			screenshot = back_buffer;
		}
		dkCmdBufBlitImage(dk.commands, &source, &from, &screen_view, &to, DkBlitFlag_FilterLinear, 0);
	}
	command_done(event);
	profile_frame_end();
	commands_submit();
	if (capture.recording && dk.frame == capture.slot)
	{
		capture.recording = 0;
		capture.waiting = 1;
	}
	/* (the GPU may have faulted since the frame began: the image is not
	presented, as a faulted queue would end the program) */
	if (queue_faulted())
		return;
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
			uint32_t row, row_bytes = SURFACE_PIXEL_WIDTH(&screenshot->surface) * 4;

			for (row = 0; row < SURFACE_PIXEL_HEIGHT(&screenshot->surface); row++)
				memcpy(out + (size_t)row * row_bytes, rows + (size_t)row * dk.readback_pitch, row_bytes);
		}
		else
		{
			host_logf(HOST_LOG_ERROR, "deko3d: the GPU did not finish the frame in two seconds; no screenshot (and "
				"the GPU may have stopped)");
		}
	}
	dkQueuePresentImage(dk.queue, dk.swapchain, slot);
	slow.presents++;
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
		if (trace.events && dk.frames_presented % 600 == 0)
			trace_held();
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
	uint32_t kind, format, width, height, depth, levels, swizzle;
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
	/* who holds each descriptor slot, for the GPU trace's checks
	(slot_owner_name): 0 nobody */
	uint32_t slot_owner[IMAGE_SLOTS];
} tex;

/* a slot's holder: a texture's id, or one of these with an index */
#define SLOT_OWNER_TARGET 0x10000u
#define SLOT_OWNER_COMPOSITE 0x20000u
#define SLOT_OWNER_DUMMY 0x30000u
#define SLOT_OWNER_RELEASED 0x40000u

static const char *slot_owner_name(uint32_t owner, char *text, size_t size)
{
	if (!owner)
		snprintf(text, size, "nobody");
	else if (owner == SLOT_OWNER_DUMMY)
		snprintf(text, size, "the dummy");
	else if ((owner & 0xf0000u) == SLOT_OWNER_TARGET)
		snprintf(text, size, "target %u", (unsigned)(owner & 0xffffu));
	else if ((owner & 0xf0000u) == SLOT_OWNER_COMPOSITE)
		snprintf(text, size, "composite %u", (unsigned)(owner & 0xffffu));
	else if ((owner & 0xf0000u) == SLOT_OWNER_RELEASED)
		snprintf(text, size, "texture %u, released", (unsigned)(owner & 0xffffu));
	else
		snprintf(text, size, "texture %u", (unsigned)owner);
	return text;
}

/* a slot taken by owner: said if somebody holds it still */
static void slot_claim(uint32_t slot, uint32_t owner)
{
	char held[48], taker[48];

	if (slot >= IMAGE_SLOTS)
	{
		trace_check("descriptor slot %u is past the %u there are", (unsigned)slot, (unsigned)IMAGE_SLOTS);
		return;
	}
	if (tex.slot_owner[slot])
		trace_check("descriptor slot %u given to %s while %s holds it", (unsigned)slot,
			slot_owner_name(owner, taker, sizeof(taker)), slot_owner_name(tex.slot_owner[slot], held, sizeof(held)));
	tex.slot_owner[slot] = owner;
}

/* a slot back on the free list */
static void slot_unclaim(uint32_t slot)
{
	if (slot >= IMAGE_SLOTS)
		return;
	if (!tex.slot_owner[slot])
		trace_check("descriptor slot %u is let go twice", (unsigned)slot);
	tex.slot_owner[slot] = 0;
}

static uint32_t texture_owner(const struct texture *texture);

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
		if (offset < block->free[index].offset + block->free[index].size && block->free[index].offset < offset + size)
		{
			trace_check("image memory %08x+%08x of block %u is let go while %08x+%08x of it is free already",
				(unsigned)offset, (unsigned)size, (unsigned)(block - tex.blocks), (unsigned)block->free[index].offset,
				(unsigned)block->free[index].size);
			break;
		}
	}
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
		{
			slot_unclaim(release->slot);
			tex.slots_free[tex.slots_free_count++] = release->slot;
		}
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
	struct trace_event *event;

	if (!texture->present)
		return;
	event = trace_add(TRACE_RELEASE);
	event->a[0] = texture_owner(texture);
	event->a[1] = texture->slot;
	event->a[2] = texture->block;
	event->a[3] = texture->offset;
	event->a[4] = texture->size;
	event->a[5] = tex.release_count;
	if (texture->slot < IMAGE_SLOTS && tex.slot_owner[texture->slot] == texture_owner(texture))
		tex.slot_owner[texture->slot] = SLOT_OWNER_RELEASED | (texture_owner(texture) & 0xffffu);
	if (tex.release_count == RELEASE_LIMIT)
	{
		/* too many waiting: the GPU is waited for, which is rare and lets them all go */
		queue_wait_idle();
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
	else
		trace_check("an image could not wait for the GPU to finish with it: slot %u and %u bytes of block %u are lost",
			(unsigned)texture->slot, (unsigned)texture->size, (unsigned)texture->block);
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
		queue_wait_idle();
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

/* The tile height of a 2D (or cube) image whose level 0 is height rows of
blocks (4 texels each for the compressed formats).

deko3d chooses one from the height times 1.5 (dkImageLayoutInitialize's
pickTileSize), and its level offsets (calcLevelOffset) shrink it again for
level 0 where the level is shorter than the tile (adjustTileSize), but its
copies into level 0 and the image's descriptor use it unshrunk. For an image
6 to 8 rows of blocks tall - 21 to 32 texels in DXT - level 0 then fills
twice the room its offsets leave it: level 1's copy landed on the second half
of level 0's first row, and the GPU sampled the levels from where they were
not (the pause menu's panels and its selection frame; DXT textures decoded
to BGRA, 6 to 8 texels tall instead, drew right). Given already shrunk,
every one of them agrees. */
static uint32_t tile_height(uint32_t height)
{
	uint32_t gobs = (height + height / 2 + 7) / 8;
	uint32_t shift = gobs >= 16 ? 4 : gobs >= 8 ? 3 : gobs >= 4 ? 2 : gobs >= 2 ? 1 : 0;

	/* (deko3d's adjustTileSize, for the unit of a GOB's 8 rows) */
	if (shift && (8u << (shift - 1)) >= height)
	{
		uint32_t rows = 8u << (shift - 1);

		while (--shift)
		{
			rows >>= 1;
			if (rows < height)
				break;
		}
	}
	return shift;
}

/* the maker's tile height set from tile_height, for a 2D or cube image of
height texels in blocks of block rows */
static void tile_height_set(DkImageLayoutMaker *maker, uint32_t height, uint32_t block)
{
	maker->flags |= DkImageFlags_CustomTileSize;
	maker->tileSize = (DkTileSize)tile_height((height + block - 1) / block);
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
	slot_claim(texture->slot, texture_owner(texture));
	dkImageLayoutMakerDefaults(&maker, dk.device);
	maker.type = command->kind == DK_TEXTURE_CUBE ? DkImageType_Cubemap : command->kind == DK_TEXTURE_3D ?
		DkImageType_3D : DkImageType_2D;
	maker.flags = 0;
	maker.format = image_format(command->format);
	maker.dimensions[0] = command->width;
	maker.dimensions[1] = command->height;
	maker.dimensions[2] = command->kind == DK_TEXTURE_CUBE ? 6 : command->kind == DK_TEXTURE_3D ? command->depth : 0;
	maker.mipLevels = command->levels;
	if (command->kind != DK_TEXTURE_3D)
		tile_height_set(&maker, command->height, command->format == DK_TEXTURE_BGRA ? 1 : 4);
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
			{
				slot_unclaim(texture->slot);
				tex.slots_free[tex.slots_free_count++] = texture->slot;
			}
			texture_problem("no image memory for texture %u (%ux%u, %u levels, %u bytes)", (unsigned)command->id,
				(unsigned)command->width, (unsigned)command->height, (unsigned)command->levels, (unsigned)size);
			return 0;
		}
	}
	texture->size = size;
	if (trace.events)
	{
		uint32_t index;

		/* (memory a live image has, or one the GPU may still read) */
		for (index = 0; index < DK_TEXTURE_LIMIT; index++)
		{
			const struct texture *other = &tex.table[index];

			if (other != texture && other->present && other->block == texture->block &&
				texture->offset < other->offset + other->size && other->offset < texture->offset + size)
				trace_check("texture %u's image memory %08x+%08x of block %u is texture %u's, %08x+%08x",
					(unsigned)command->id, (unsigned)texture->offset, (unsigned)size, (unsigned)texture->block,
					(unsigned)index, (unsigned)other->offset, (unsigned)other->size);
		}
		for (index = 0; index < tex.release_count; index++)
		{
			const struct release *release = &tex.releases[index];

			if (release->size && release->block == texture->block &&
				texture->offset < release->offset + release->size && release->offset < texture->offset + size)
				trace_check("texture %u's image memory %08x+%08x of block %u is still waiting for submission %u "
					"(retired %u)", (unsigned)command->id, (unsigned)texture->offset, (unsigned)size,
					(unsigned)texture->block, (unsigned)release->serial, (unsigned)host_dk_retired());
		}
		if (texture->offset + size > TEXTURE_BLOCK_SIZE)
			trace_check("texture %u's image runs past its block: %08x+%08x", (unsigned)command->id,
				(unsigned)texture->offset, (unsigned)size);
	}
	dkImageInitialize(&texture->image, &layout, tex.blocks[texture->block].memory, texture->offset);
	dkImageViewDefaults(&view, &texture->image);
	if (command->swizzle)
	{
		uint32_t channel;

		for (channel = 0; channel < 4; channel++)
			view.swizzle[channel] = (DkImageSwizzle)(DkImageSwizzle_Red + ((command->swizzle >> (8 * channel)) & 3));
	}
	dkImageDescriptorInitialize(&descriptors[texture->slot], &view, false, false);
	texture->swizzle = command->swizzle;
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
			barrier(DkBarrier_Full, 0);
		tex.draws_unfenced = 0;
		return;
	}
	if (!tex.written_unfenced)
		return;
	barrier(DkBarrier_Full, DkInvalidateFlags_Image | DkInvalidateFlags_Descriptors);
	tex.written_unfenced = 0;
	tex.draws_unfenced = 0;
}

/* debug.texture_verify_compressed: reads a compressed texture's levels back
out of its image, waiting for the GPU, and says where they differ from the
game's bytes; whether the copy into the image is right, or the fault is in
how the image is sampled */
static void texture_verify(struct texture *texture, const struct dk_command_texture *command)
{
	static DkMemBlock readback;
	static void *readback_cpu;
	static DkGpuAddr readback_gpu;
	static unsigned long verified, mismatched;
	uint32_t faces = command->kind == DK_TEXTURE_CUBE ? 6 : 1;
	uint32_t face, level;

	if (!readback)
	{
		readback = memory_block(STAGING_SLICE_SIZE, DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached, NULL);
		if (!readback)
			return;
		readback_cpu = dkMemBlockGetCpuAddr(readback);
		readback_gpu = dkMemBlockGetGpuAddr(readback);
	}
	if (command->source_bytes > STAGING_SLICE_SIZE)
		return;
	barrier(DkBarrier_Full, 0);
	{
		uint32_t at = 0;

		for (face = 0; face < faces; face++)
		{
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
				copy.addr = readback_gpu + at;
				copy.rowLength = 0;
				copy.imageHeight = 0;
				dkCmdBufCopyImageToBuffer(dk.commands, &view, &rectangle, &copy, 0);
				at += level_bytes(command, level);
			}
		}
	}
	commands_submit_partial();
	queue_wait_idle();
	{
		const uint8_t *game = (const uint8_t *)(uintptr_t)command->source;
		const uint8_t *image = (const uint8_t *)readback_cpu;
		uint32_t at = 0;
		int bad = 0;

		for (face = 0; face < faces && !bad; face++)
		{
			uint32_t offset = face * command->face_bytes;

			for (level = 0; level < command->levels && !bad; level++)
			{
				uint32_t bytes = level_bytes(command, level), index;

				for (index = 0; index < bytes; index++)
				{
					if (game[offset + index] != image[at + index])
					{
						bad = 1;
						if (mismatched < 64)
							host_logf(HOST_LOG_WARN, "texture verify: image %u (BC%u %ux%ux%u, %u levels, kind %u) "
								"differs at face %u level %u (%ux%u, %u bytes), byte %u: %02x in the image, %02x in the "
								"game's", (unsigned)command->id, (unsigned)command->format, (unsigned)command->width,
								(unsigned)command->height, (unsigned)command->depth, (unsigned)command->levels,
								(unsigned)command->kind, (unsigned)face, (unsigned)level,
								(unsigned)level_size(command->width, level), (unsigned)level_size(command->height, level),
								(unsigned)bytes, (unsigned)index, image[at + index], game[offset + index]);
						break;
					}
				}
				offset += bytes;
				at += bytes;
			}
		}
		verified++;
		mismatched += bad;
		if (verified % 100 == 0)
			host_logf(HOST_LOG_INFO, "texture verify: %lu compressed textures checked, %lu differ", verified, mismatched);
	}
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
	trace.texture_source_gpu = source;
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
	profile.frame.texture_writes++;
	profile.frame.texture_bytes += command->source_bytes;
	if (host_dk_verify_compressed && !host_texels && command->format != DK_TEXTURE_BGRA)
		texture_verify(texture, command);
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


/* debug.texture_verify_compressed: each compressed texture's levels copied
back out of its image after it is written, and compared with the game's
bytes (texture_verify) */
int host_dk_verify_compressed;

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
	struct trace_event *event;

	if (!dk.ready || !tex.image_descriptors)
		return;
	event = trace_add(DK_COMMAND_TEXTURE);
	trace.texture_source_gpu = 0;
	texture = &tex.table[command->id];
	event->a[0] = command->id;
	event->a[1] = command->kind | command->format << 8 | command->levels << 16;
	event->a[2] = command->width;
	event->a[3] = command->height;
	event->a[4] = command->depth;
	event->a[5] = command->source;
	event->a[6] = command->source_bytes;
	event->a[11] = texture->present;
	/* the same shape (and channels): written in place; else remade */
	if (texture->present && (texture->kind != command->kind || texture->format != command->format ||
		texture->width != command->width || texture->height != command->height || texture->depth != command->depth ||
		texture->levels != command->levels || texture->swizzle != command->swizzle))
		texture_release(texture);
	if (!texture->present && !texture_make(texture, command))
		return;
	/* (made only, for DK_COMMAND_TEXTURE_ROWS to fill) */
	if (command->source_bytes)
		texture_write(texture, command, NULL);
	tex.stages_dirty = 1;
	event->a[7] = texture->slot;
	event->a[8] = texture->block;
	event->a[9] = texture->offset;
	event->a[10] = texture->size;
	event->g[0] = dkMemBlockGetGpuAddr(tex.blocks[texture->block].memory) + texture->offset;
	event->g[1] = trace.texture_source_gpu;
	command_done(event);
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

	struct trace_event *event;

	if (!dk.ready || !tex.image_descriptors || !command->id || command->id >= DK_TEXTURE_LIMIT)
		return;
	event = trace_add(DK_COMMAND_TEXTURE_ROWS);
	event->a[0] = command->id;
	event->a[1] = command->level;
	event->a[2] = command->top;
	event->a[3] = command->rows;
	event->a[4] = command->source;
	event->a[5] = command->source_bytes;
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
	profile.frame.rows++;
	profile.frame.texture_bytes += command->source_bytes;
	event->a[6] = texture->slot;
	event->g[0] = source;
	event->g[1] = dkMemBlockGetGpuAddr(tex.blocks[texture->block].memory) + texture->offset;
	command_done(event);
}

static void texture_free_receive(const struct dk_command_texture_free *command)
{
	struct trace_event *event = trace_add(DK_COMMAND_TEXTURE_FREE);

	event->a[0] = command->id;
	if (command->id && command->id < DK_TEXTURE_LIMIT && dk.ready)
	{
		event->a[1] = tex.table[command->id].present;
		texture_release(&tex.table[command->id]);
		tex.stages_dirty = 1;
	}
}

/* ---------- the high-res HUD's sheets (host_dk_hud.c)

An image made from a sheet's decoded levels: the buffer they are in is the
storage of a memory block of its own, which the GPU copies them from, so
nothing is copied by a CPU; the block and its buffer are let go once the
GPU has run the copy (its submission retired). */

#define HUD_SOURCES 64

static struct
{
	uint32_t serial;
	DkMemBlock memory;
	unsigned char *texels;
} hud_sources[HUD_SOURCES];
static uint32_t hud_source_count;

static void hud_sources_collect(int all)
{
	uint32_t retired = host_dk_retired(), index, kept = 0;

	/* (every one: what is recorded submitted, and the GPU waited for) */
	if (all && hud_source_count)
	{
		if (dk.recorded)
			commands_submit_partial();
		queue_wait_idle();
	}
	for (index = 0; index < hud_source_count; index++)
	{
		if (!all && hud_sources[index].serial > retired)
		{
			hud_sources[kept++] = hud_sources[index];
			continue;
		}
		dkMemBlockDestroy(hud_sources[index].memory);
		free(hud_sources[index].texels);
	}
	hud_source_count = kept;
}

static void hud_prepare_receive(const struct dk_command_hud_prepare *command)
{
	if (command->header.size < sizeof(*command) ||
		command->count > (command->header.size - sizeof(*command)) / sizeof(command->sheets[0]))
	{
		host_logf(HOST_LOG_ERROR, "deko3d: a bad HUD prepare command (%u sheets in %u bytes)",
			(unsigned)command->count, (unsigned)command->header.size);
		return;
	}
	host_dk_hud_prepare(command->sheets, command->count);
}

static void hud_image_receive(const struct dk_command_hud_image *command)
{
	struct dk_command_texture made;
	struct host_dk_hud_levels levels;
	struct texture *texture;
	DkMemBlock memory;
	DkGpuAddr source;
	uint64_t start = armGetSystemTick();
	uint32_t level, offset = 0;
	int late = 0;

	if (!dk.ready || !tex.image_descriptors || !command->id || command->id >= DK_TEXTURE_LIMIT)
		return;
	render_where = "making a high-res HUD image (hud_image_receive)";
	hud_sources_collect(0);
	if (!host_dk_hud_take(command->sheet.asset, &levels))
	{
		/* (let go since the guest was told it was ready: a map's change) */
		late = 1;
		if (!host_dk_hud_decode((const unsigned char *)(uintptr_t)command->sheet.png, command->sheet.png_size,
			&levels))
		{
			texture_problem("high-res hud sheet %u could not be decoded for image %u", (unsigned)command->sheet.asset,
				(unsigned)command->id);
			return;
		}
	}
	if (hud_source_count == HUD_SOURCES)
		hud_sources_collect(1);
	memset(&made, 0, sizeof(made));
	made.id = command->id;
	made.kind = DK_TEXTURE_2D;
	made.format = DK_TEXTURE_BGRA;
	made.width = levels.width;
	made.height = levels.height;
	made.depth = 1;
	made.levels = levels.levels;
	texture = &tex.table[command->id];
	if (!texture_command_valid(&made))
	{
		texture_problem("high-res hud sheet %u is %ux%u, %u levels: no image", (unsigned)command->sheet.asset,
			(unsigned)levels.width, (unsigned)levels.height, (unsigned)levels.levels);
		free(levels.texels);
		return;
	}
	if (texture->present && (texture->kind != made.kind || texture->format != made.format ||
		texture->width != made.width || texture->height != made.height || texture->depth != 1 ||
		texture->levels != made.levels || texture->swizzle != 0))
		texture_release(texture);
	render_where = "making a high-res HUD image's memory block (dkMemBlockCreate)";
	memory = memory_block(levels.bytes, DkMemBlockFlags_CpuCached | DkMemBlockFlags_GpuCached, levels.texels);
	render_where = "making a high-res HUD image (hud_image_receive)";
	if (!memory || (!texture->present && !texture_make(texture, &made)))
	{
		texture_problem("no memory for high-res hud sheet %u's image %u", (unsigned)command->sheet.asset,
			(unsigned)command->id);
		if (memory)
			dkMemBlockDestroy(memory);
		free(levels.texels);
		return;
	}
	source = dkMemBlockGetGpuAddr(memory);
	textures_fence(1);
	for (level = 0; level < levels.levels; level++)
	{
		DkImageView view;
		DkImageRect rectangle;
		DkCopyBuf copy;

		dkImageViewDefaults(&view, &texture->image);
		view.mipLevelOffset = (uint8_t)level;
		view.mipLevelCount = 1;
		rectangle.x = rectangle.y = rectangle.z = 0;
		rectangle.width = level_size(levels.width, level);
		rectangle.height = level_size(levels.height, level);
		rectangle.depth = 1;
		copy.addr = source + offset;
		copy.rowLength = 0;
		copy.imageHeight = 0;
		dkCmdBufCopyBufferToImage(dk.commands, &copy, &view, &rectangle, 0);
		offset += rectangle.width * rectangle.height * 4;
	}
	tex.written_unfenced = 1;
	tex.stages_dirty = 1;
	dk.recorded = 1;
	profile.frame.texture_writes++;
	profile.frame.texture_bytes += offset;
	hud_sources[hud_source_count].serial = dk.serial;
	hud_sources[hud_source_count].memory = memory;
	hud_sources[hud_source_count].texels = levels.texels;
	hud_source_count++;
	if (host_dk_hud_log)
		host_logf(HOST_LOG_INFO, "deko3d: high-res hud: %s made image %u (%ux%u) in %.2f ms%s",
			command->sheet.tag ? (const char *)(uintptr_t)command->sheet.tag : "?", (unsigned)command->id,
			(unsigned)levels.width, (unsigned)levels.height, armTicksToNs(armGetSystemTick() - start) / 1e6,
			late ? " (decoded here: let go before it was drawn)" : "");
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

/* who a texture's slot is for: its id, or the dummy */
static uint32_t texture_owner(const struct texture *texture)
{
	return texture == &tex.dummy ? SLOT_OWNER_DUMMY : (uint32_t)(texture - tex.table);
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
	made.lodBias = sampler->lod_bias + (host_dk_lod_bias ? 3.0f : 0.0f);
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

	struct trace_event *event;

	if (!tex.sampler_descriptors)
		return;
	event = trace_add(DK_COMMAND_TEXTURES);
	for (stage = 0; stage < 4; stage++)
	{
		event->a[stage] = command->stages[stage].id;
		event->a[4 + stage] = command->stages[stage].target;
		event->a[8 + stage] = command->stages[stage].composite;
		if (command->stages[stage].id >= DK_TEXTURE_LIMIT)
			trace_check("stage %u samples image %u, past the %u there are", (unsigned)stage,
				(unsigned)command->stages[stage].id, (unsigned)DK_TEXTURE_LIMIT);
		tex.stage_id[stage] = command->stages[stage].id;
		tex.stage_target[stage] = command->stages[stage].target;
		tex.stage_composite[stage] = command->stages[stage].composite;
		tex.stage_sampler[stage] = sampler_slot(&command->stages[stage].sampler);
	}
	tex.stages_dirty = 1;
}

/* every target's draws that sampled it are done (a barrier was recorded) */
static void targets_sampled_forget(void)
{
	int index;

	for (index = 0; index < dk.target_count; index++)
		dk.targets[index].sampled = 0;
	tex.targets_sampled = 0;
}

/* The targets are about to change to color and depth: one of them sampled
since the last barrier is drawn into only once the draws that sampled it are
done, and a stage that samples a target looks again (textures_apply). Only
those two need the wait: drawing into a target nothing has sampled - the
next object's shadow map, while the last one's is projected - can overlap
the draws before it, which a crowd's shadows did 40 times a frame. A
composite's copies (the 2D engine's) end before any target is drawn into. */
static void textures_targets_changing(const struct target *color, const struct target *depth)
{
	if (host_dk_no_target_barriers && !tex.targets_copied)
	{
		targets_sampled_forget();
		tex.stages_dirty = 1;
		return;
	}
	if (tex.targets_copied)
	{
		barrier(DkBarrier_Full, 0);
		tex.targets_copied = 0;
		targets_sampled_forget();
	}
	else if ((color && color->sampled) || (depth && depth->sampled))
	{
		barrier(DkBarrier_Fragments, 0);
		targets_sampled_forget();
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
		slot_claim(best->slot, SLOT_OWNER_TARGET | (uint32_t)(best - dk.targets));
		dkImageViewDefaults(&view, &best->image);
		dkImageDescriptorInitialize(&descriptors[best->slot], &view, false, false);
		best->has_slot = 1;
		barrier(DkBarrier_None, DkInvalidateFlags_Descriptors);
	}
	if (best->drawn)
	{
		barrier(DkBarrier_Fragments, DkInvalidateFlags_Image);
		best->drawn = 0;
	}
	tex.targets_sampled = 1;
	best->sampled = 1;
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
	tile_height_set(&maker, command->height, 1);
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
	slot_claim(composite->slot, SLOT_OWNER_COMPOSITE | (uint32_t)composite_count);
	{
		struct trace_event *event = trace_add(DK_COMMAND_COMPOSITE);

		event->a[0] = command->data;
		event->a[1] = command->width;
		event->a[2] = command->height;
		event->a[3] = command->levels;
		event->a[4] = composite->slot;
	}
	composite->defined = *command;
	dkImageInitialize(&composite->image, &layout, block, offset);
	dkImageViewDefaults(&view, &composite->image);
	dkImageDescriptorInitialize(&((DkImageDescriptor *)dkMemBlockGetCpuAddr(tex.image_descriptors))[composite->slot],
		&view, false, false);
	barrier(DkBarrier_None, DkInvalidateFlags_Descriptors);
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

		/* (drawn at its own size: a composite's levels are copied as they are) */
		if (target->surface.data == data && target->surface.kind == DK_SURFACE_COLOR &&
			target->surface.width == width && target->surface.height == height &&
			SURFACE_PIXEL_WIDTH(&target->surface) == width && SURFACE_PIXEL_HEIGHT(&target->surface) == height &&
			(!best || target->bound > best->bound))
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
	barrier(DkBarrier_Fragments, 0);
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
			barrier(DkBarrier_Full, 0);
			dkImageViewDefaults(&source, &composite->image);
			source.mipLevelOffset = (uint8_t)(level - 1);
			source.mipLevelCount = 1;
			from.width = level_size(composite->defined.width, level - 1);
			from.height = level_size(composite->defined.height, level - 1);
			dkCmdBufBlitImage(dk.commands, &source, &from, &destination, &to, DkBlitFlag_FilterLinear, 0);
		}
	}
	barrier(DkBarrier_Full, DkInvalidateFlags_Image);
	composite->copied = 1;
	composite->rendered_levels = rendered;
	composite->stamp = dk.target_clock;
	{
		struct trace_event *event = trace_add(TRACE_COMPOSITE_COPY);

		event->a[0] = composite->defined.data;
		event->a[1] = rendered;
		event->a[2] = levels;
		event->a[3] = composite->slot;
		profile.frame.composite_copies++;
		command_done(event);
	}
	tex.targets_copied = 1;
	dk.recorded = 1;
	return composite;
}

/* ---------- copies between targets (DK_COMMAND_BLIT) */

/* the 2D engine's copy of a rectangle of one target into another (made if
it never was), as the composites' copies are made: the source's draws done
and the destination's samplers done first, the copy done before anything
samples the destination or draws again */
static void blit(const struct dk_command_blit *command)
{
	struct target *source = target_get(&command->source);
	struct target *destination = target_get(&command->destination);
	struct trace_event *event = trace_add(DK_COMMAND_BLIT);
	DkImageView from_view, to_view;
	DkImageRect from = { command->from[0], command->from[1], 0, command->from[2], command->from[3], 1 };
	DkImageRect to = { command->to[0], command->to[1], 0, command->to[2], command->to[3], 1 };

	event->a[0] = command->source.data;
	event->a[1] = command->from[0];
	event->a[2] = command->from[1];
	event->a[3] = command->from[2];
	event->a[4] = command->from[3];
	event->a[5] = command->destination.data;
	event->a[6] = command->to[0];
	event->a[7] = command->to[1];
	event->a[8] = command->to[2] | command->to[3] << 16;
	if (!source || !destination || source == destination || command->source.kind != DK_SURFACE_COLOR ||
		command->destination.kind != DK_SURFACE_COLOR || !from.width || !from.height || !to.width || !to.height)
		return;
	if ((uint64_t)from.x + from.width > SURFACE_PIXEL_WIDTH(&source->surface) ||
		(uint64_t)from.y + from.height > SURFACE_PIXEL_HEIGHT(&source->surface) ||
		(uint64_t)to.x + to.width > SURFACE_PIXEL_WIDTH(&destination->surface) ||
		(uint64_t)to.y + to.height > SURFACE_PIXEL_HEIGHT(&destination->surface))
	{
		trace_check("a blit of %08x %u,%u %ux%u into %08x %u,%u %ux%u is outside a target", (unsigned)command->source.data,
			(unsigned)from.x, (unsigned)from.y, (unsigned)from.width, (unsigned)from.height,
			(unsigned)command->destination.data, (unsigned)to.x, (unsigned)to.y, (unsigned)to.width, (unsigned)to.height);
		return;
	}
	event->a[9] = 1;
	dkImageViewDefaults(&from_view, &source->image);
	dkImageViewDefaults(&to_view, &destination->image);
	barrier(DkBarrier_Fragments, 0);
	dkCmdBufBlitImage(dk.commands, &from_view, &from, &to_view, &to, DkBlitFlag_FilterLinear, 0);
	barrier(DkBarrier_Full, DkInvalidateFlags_Image);
	targets_sampled_forget();
	tex.stages_dirty = 1;
	/* (the latest image at its data, which a stage samples: target_sampled) */
	destination->bound = ++dk.target_clock;
	destination->drawn = 0;
	command_done(event);
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
	{
		struct trace_event *event = trace_add(DK_COMMAND_VISIBILITY_END);

		event->a[0] = command->index;
		command_done(event);
	}
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
		if (trace.events && texture->slot < IMAGE_SLOTS && tex.slot_owner[texture->slot] != texture_owner(texture))
		{
			char held[48];

			trace_check("stage %u samples image %u by slot %u, which %s holds", (unsigned)stage, (unsigned)id,
				(unsigned)texture->slot, slot_owner_name(tex.slot_owner[texture->slot], held, sizeof(held)));
		}
		handles[stage] = dkMakeTextureHandle(texture->slot, tex.stage_sampler[stage]);
	}
	for (stage = 0; stage < 4; stage++)
		trace.handles[stage] = handles[stage];
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
	struct trace_event *event = trace_add(DK_COMMAND_STATE);

	event->a[0] = (uint32_t)command->state.viewport[0];
	event->a[1] = (uint32_t)command->state.viewport[1];
	event->a[2] = (uint32_t)command->state.viewport[2];
	event->a[3] = (uint32_t)command->state.viewport[3];
	event->a[4] = (uint32_t)command->state.scissor[0];
	event->a[5] = (uint32_t)command->state.scissor[1];
	event->a[6] = (uint32_t)command->state.scissor[2];
	event->a[7] = (uint32_t)command->state.scissor[3];
	event->a[8] = command->state.blend | command->state.depth_test << 8 | command->state.depth_write << 9 |
		command->state.stencil_test << 10 | command->state.cull << 12 | command->state.fill_mode << 16;
	event->a[9] = command->state.color_mask;
	dk.state = command->state;
	dk.state_valid = 1;
	dk.state_dirty = 1;
}

static void shaders_receive(const struct dk_command_shaders *command)
{
	struct trace_event *event = trace_add(DK_COMMAND_SHADERS);

	event->a[0] = command->vertex;
	event->a[1] = command->pixel;
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

	struct trace_event *event = trace_add(DK_COMMAND_VERTEX_FORMAT);

	event->a[0] = command->format.stream_mask;
	for (stream = 0, index = 0; stream < DK_STREAM_COUNT && index < 8; stream++)
	{
		if ((command->format.stream_mask >> stream) & 1)
			event->a[1 + index++] = command->format.strides[stream];
	}
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
	int apply_rest;

	if (state->viewport[2] <= 0 || state->viewport[3] <= 0 || !size_from)
		return dk.state_dirty ? 0 : 1;
	apply_rest = dk.state_dirty;
	dk.state_dirty = 0;

	viewport.x = (float)state->viewport[0];
	viewport.y = (float)state->viewport[1];
	viewport.width = (float)state->viewport[2];
	viewport.height = (float)state->viewport[3];
	viewport.near = state->depth_range[0];
	viewport.far = state->depth_range[1];
	left = state->scissor[0] < 0 ? 0 : state->scissor[0];
	top = state->scissor[1] < 0 ? 0 : state->scissor[1];
	right = state->scissor[0] + state->scissor[2];
	bottom = state->scissor[1] + state->scissor[3];
	if (right > (int32_t)SURFACE_PIXEL_WIDTH(&size_from->surface))
		right = (int32_t)SURFACE_PIXEL_WIDTH(&size_from->surface);
	if (bottom > (int32_t)SURFACE_PIXEL_HEIGHT(&size_from->surface))
		bottom = (int32_t)SURFACE_PIXEL_HEIGHT(&size_from->surface);
	scissor.x = (uint32_t)left;
	scissor.y = (uint32_t)top;
	scissor.width = right > left ? (uint32_t)(right - left) : 0;
	scissor.height = bottom > top ? (uint32_t)(bottom - top) : 0;

	if (apply_rest)
	{
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
	}
	/* After the rasterizer state, and on every draw: binding a target left
	the scissor as the whole target. The window is put back so a triangle the
	clipper does not take cannot paint the other split-screen pane. */
	dkCmdBufSetViewports(dk.commands, 0, &viewport, 1);
	dkCmdBufSetScissors(dk.commands, 0, &scissor, 1);
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

/* whether [gpu, gpu + size) is memory the GPU reads draws' data from: one
mapped chunk of the window, or the upload buffer. A GPU fault in a crowded
game named a vertex stream's end as nonsense (the kernel's error info:
SET_VERTEX_STREAM_LIMIT_A), so a draw's every stream and its indices are
checked before the GPU is given them. */
static int gpu_range_known(DkGpuAddr gpu, uint64_t size)
{
	int index;

	if (!gpu || size > WINDOW_CHUNK_SIZE)
		return 0;
	if (dk.upload_memory && gpu >= dk.upload_gpu && gpu + size <= dk.upload_gpu + (DkGpuAddr)FRAMES * UPLOAD_SLICE_SIZE)
		return 1;
	for (index = 0; index < dk.window_chunk_count; index++)
	{
		const struct window_chunk *chunk = &dk.window_chunks[index];

		if (chunk->block && gpu >= chunk->gpu && gpu + size <= chunk->gpu + WINDOW_CHUNK_SIZE)
			return 1;
	}
	return 0;
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
	struct trace_event *event = trace_add(DK_COMMAND_DRAW);

	event->a[0] = command->primitive | command->stream_count << 8 |
		(command->index_address == DK_INDEX_INLINE ? 1u : command->index_address ? 2u : 0u) << 16;
	event->a[1] = command->count;
	event->a[2] = (uint32_t)command->vertex_offset;
	if (command->stream_count)
	{
		event->a[3] = command->inline_bytes ? 0 : command->streams[0][0];
		event->a[4] = command->inline_bytes ? command->inline_bytes : command->streams[0][1];
	}
	event->a[6] = dk.vertex_shader;
	event->a[7] = dk.pixel_shader;
	event->g[3] = (uint64_t)(dk.color ? dk.color->surface.data : 0) << 32 | (dk.depth ? dk.depth->surface.data : 0);
	for (stream = 0; stream < DK_STREAM_COUNT; stream++)
		streams += (dk.format.stream_mask >> stream) & 1;
	if (!dk.format_valid || !dk.state_valid || (!dk.color && !dk.depth) || streams != command->stream_count)
	{
		profile.frame.skipped++;
		event->a[0] |= 1u << 24;
		draw_problem("what it needs is not in place");
		return;
	}
	if (!state_apply())
	{
		profile.frame.skipped++;
		event->a[0] |= 2u << 24;
		return;
	}
	if (!shaders_apply())
	{
		profile.frame.skipped++;
		event->a[0] |= 3u << 24;
		draw_problem("a shader is not loaded");
		return;
	}
	if (!textures_apply())
	{
		profile.frame.skipped++;
		event->a[0] |= 4u << 24;
		draw_problem("the textures could not be bound");
		return;
	}
	for (stream = 0; stream < 4; stream++)
		event->a[8 + stream] = trace.handles[stream];
	for (binding = 0; binding < streams; binding++)
	{
		uint32_t size = command->streams[binding][1];
		DkGpuAddr gpu = command->inline_bytes ? upload_copy(inline_data, command->inline_bytes) :
			range_read((const void *)(uintptr_t)command->streams[binding][0], size);

		if (!gpu)
		{
			profile.frame.skipped++;
			event->a[0] |= 5u << 24;
			draw_problem("a stream could not be read");
			return;
		}
		extents[binding].addr = gpu;
		extents[binding].size = command->inline_bytes ? command->inline_bytes : size;
		if (binding < 2)
			event->g[binding] = gpu;
		if (!extents[binding].size)
			trace_check("a draw's stream %u is empty (guest %08x)", (unsigned)binding,
				(unsigned)command->streams[binding][0]);
		else if (dk.vertex.buffers[binding].stride && extents[binding].size < dk.vertex.buffers[binding].stride)
			trace_check("a draw's stream %u has %u bytes, less than a vertex of %u", (unsigned)binding,
				(unsigned)extents[binding].size, (unsigned)dk.vertex.buffers[binding].stride);
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
			profile.frame.skipped++;
			event->a[0] |= 6u << 24;
			draw_problem("the constants could not be copied");
			return;
		}
		extents[binding].addr = dk.constants_gpu;
		extents[binding].size = sizeof(dk.format.constants);
		binding++;
	}
	if (command->index_address == DK_INDEX_INLINE)
	{
		index_gpu = upload_copy(inline_data + ((command->inline_bytes + 3) & ~3u), command->count * sizeof(uint16_t));
		if (!index_gpu)
		{
			profile.frame.skipped++;
			event->a[0] |= 7u << 24;
			draw_problem("the indices could not be copied");
			return;
		}
	}
	else if (command->index_address)
	{
		index_gpu = range_read((const void *)(uintptr_t)command->index_address, command->count * sizeof(uint16_t));
		if (!index_gpu)
		{
			profile.frame.skipped++;
			event->a[0] |= 8u << 24;
			draw_problem("the indices could not be read");
			return;
		}
	}
	event->g[2] = index_gpu;
	event->a[5] = binding;
	/* (each stream, and the indices, where the GPU can read them: a draw
	that would hand it anything else is said and skipped) */
	{
		uint32_t checked;

		for (checked = 0; checked < binding; checked++)
		{
			if (!gpu_range_known(extents[checked].addr, extents[checked].size))
			{
				trace_check("a draw's stream %u of %u is at gpu %010llx, %u bytes (guest %08x, inline %u), which "
					"is not memory the GPU reads draws from: skipped", (unsigned)checked, (unsigned)binding,
					(unsigned long long)extents[checked].addr, (unsigned)extents[checked].size,
					checked < command->stream_count ? (unsigned)command->streams[checked][0] : 0u,
					(unsigned)command->inline_bytes);
				event->a[0] |= 9u << 24;
				profile.frame.skipped++;
				return;
			}
		}
		if (index_gpu && !gpu_range_known(index_gpu, (uint64_t)command->count * sizeof(uint16_t)))
		{
			trace_check("a draw's %u indices are at gpu %010llx, which is not memory the GPU reads draws from: "
				"skipped", (unsigned)command->count, (unsigned long long)index_gpu);
			event->a[0] |= 10u << 24;
			profile.frame.skipped++;
			return;
		}
	}
	if (!command->count)
		trace_check("a draw of no vertices (primitive %u)", (unsigned)command->primitive);
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
	profile.frame.draws++;
	command_done(event);
}

/* runs size bytes of commands at commands (a guest address): one
submission, which the guest numbers as this does (host_dk_retired) */
/* one stream of the guest's, run: on the render thread (below), or on the
game thread with debug.render_thread = false */
static void submit_run(const unsigned char *at, uint32_t size)
{
	const unsigned char *end = at + size;

	trace_dump_if_faulted();
	dk.serial++;
	chunks_map();
	trace_add(TRACE_SUBMIT)->a[0] = size;
	profile_mark(trace.pass);
	if (trace.events && !dk.faulted)
		dkCmdBufReportValue(dk.commands, dk.serial, trace.progress_gpu + 16);
	if (host_dk_fault_test && dk.frames_presented == 1200 && !dk.faulted)
	{
		host_dk_fault_test = 0;
		host_logf(HOST_LOG_WARN, "deko3d: the GPU is made to fault now (debug.gpu_fault_test)");
		trace_add(TRACE_SUBMIT)->a[0] = 0xdead;
		dkCmdBufReportValue(dk.commands, 1, 0x1000);
	}
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
			pace_present_run();
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
		case DK_COMMAND_MARKER:
			if (header->size >= sizeof(struct dk_command_marker))
				marker_receive((const struct dk_command_marker *)header);
			break;
		case DK_COMMAND_HUD_PREPARE:
			hud_prepare_receive((const struct dk_command_hud_prepare *)header);
			break;
		case DK_COMMAND_HUD_IMAGE:
			if (header->size >= sizeof(struct dk_command_hud_image))
				hud_image_receive((const struct dk_command_hud_image *)header);
			break;
		case DK_COMMAND_HUD_FORGET:
			host_dk_hud_forget();
			break;
		case DK_COMMAND_BLIT:
			if (header->size >= sizeof(struct dk_command_blit))
			{
				blit((const struct dk_command_blit *)header);
				dk.recorded = 1;
			}
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
	trace_dump_if_faulted();
}

/* ---------- the render thread

The guest hands its stream over at each frame's end (and when it fills, or
a busy check needs it submitted); running it - turning its commands into
deko3d's, presenting - took the game thread a sixth of its time in a
40-player match, while the console's other cores waited. The stream is
copied into one of RENDER_QUEUE buffers and run by a thread of its own, on
a helper core, in order; the game goes on with its next frame. It waits
when all of them are still to run; how far ahead it goes is the frame's
pace's (below), not the queue's. The guest's checks against the GPU (host_dk_retired) count
submissions the GPU has finished, which this thread has run before, so
what the guest keeps for a submission is kept until it is run; the queue
drains before host_dk_idle waits for the GPU. debug.render_thread = false
runs each stream on the game thread, as before. */
int host_dk_render_thread = 1;

/* ---------- the frame's pace (debug.frame_pacing)

With the render thread, the game thread was held back only by a full render
queue: it ran frames ahead in a burst (under 15 ms apart), then waited out
two of the display's refreshes at once inside the next frame (the slow
frames' lines: 79 of 87 at the Halo level, the game thread running 4-12 ms
and waiting 20-39 ms while two presents waited 16 ms each for a screen
image). Shown a refresh apart, frames begun 5 ms and then 33 ms apart
judder under the interpolation, though the display shows 60. Here the game
thread, as it hands a frame over (the stand-in window's swap), waits for
the present of the frame before it: never more than one frame ahead, and
each begun a refresh after the last. 100 ms at most, should a present
never come (a faulted queue presents nothing). */
int host_dk_frame_pacing = 1;

#define PACE_AHEAD 1
#define PACE_LIMIT_NS 100000000ULL

static Mutex pace_lock;
static CondVar pace_presented;
/* the presents the render thread has run, and the frames the game thread
has handed over */
static uint32_t pace_run, pace_handed;

static void pace_present_run(void)
{
	mutexLock(&pace_lock);
	pace_run++;
	condvarWakeAll(&pace_presented);
	mutexUnlock(&pace_lock);
}

void host_dk_pace(void)
{
	static int said;
	uint64_t start;

	if (!host_dk_frame_pacing || !host_dk_render_thread)
		return;
	mutexLock(&pace_lock);
	pace_handed++;
	start = slow_now_ns();
	while ((int32_t)(pace_handed - pace_run) > PACE_AHEAD)
	{
		uint64_t waited = slow_now_ns() - start;

		if (waited >= PACE_LIMIT_NS)
		{
			if (!said)
			{
				said = 1;
				host_logf(HOST_LOG_WARN, "deko3d: a frame's pace waited 100 ms for a present (%u handed over, %u "
					"presented); it goes on", (unsigned)pace_handed, (unsigned)pace_run);
			}
			/* (caught up, so as not to wait so again for the same) */
			pace_run = pace_handed - PACE_AHEAD;
			break;
		}
		condvarWaitTimeout(&pace_presented, &pace_lock, PACE_LIMIT_NS - waited);
	}
	mutexUnlock(&pace_lock);
}

/* (8: room for a frame's streams and the next one's. A slot is held while
its stream runs, a present's wait for a screen image included, so at 3 a
frame flushed three times - a busy check, KickPushBuffer, the frees after
handover - waited mid-frame for the frame before it to be presented:
33 ms frames at 60 fps, the game thread running 11 ms of them. The
streams' buffers grow as used, 1 MB at most each.) */
#define RENDER_QUEUE 8
#define RENDER_THREAD_STACK (1024 * 1024)

static struct
{
	pthread_mutex_t lock;
	pthread_cond_t work;
	pthread_cond_t room;
	int started;
	int failed;
	unsigned head, count;
	struct
	{
		unsigned char *data;
		uint32_t capacity, size;
	} streams[RENDER_QUEUE];
} render_queue = { PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER, PTHREAD_COND_INITIALIZER };

/* (read without the lock: the watchdog's look) */
static uint32_t render_queued(void)
{
	return (uint32_t)render_queue.count;
}

/* ---------- the render thread's watchdog

A render thread that stops says nothing: what it waits in (a screen image,
a lock, deko3d itself) has no time limit there, and the game thread then
waits for its submissions (a resource lock) with the picture frozen. So a
thread of its own looks once a second: with streams queued and none run for
RENDER_STALL_SECONDS, it logs what the render thread was doing and its
stack (host_debug_thread_backtrace), once for each stop. */

#define RENDER_STALL_SECONDS 3

static Handle render_handle;
static volatile uint32_t render_progress;

/* (libnx's mutex word: the owner's thread handle, with the waiters' bit) */
#define LOCK_WAITERS 0x40000000u

/* The other two ways the picture has stopped with nothing said (c40's
opening): dk_lock held and never let go, which host_dk_retired's trylock
then answers with a count that never moves; and the GPU never finishing a
submission while nothing waits for it with a limit (the render thread idle,
the game thread in a resource lock). Each said once a stop, with the lock's
holder's stack, or the GPU's state and its trace (debug.gpu_trace). */
static void watchdog_lock_and_gpu(void)
{
	static uint32_t lock_owner, lock_seconds, retired_seen, gpu_seconds;
	static int lock_said, gpu_said;
	uint32_t owner = __atomic_load_n(&dk_lock.normal, __ATOMIC_ACQUIRE) & ~LOCK_WAITERS;

	if (owner && owner == lock_owner)
	{
		if (++lock_seconds >= RENDER_STALL_SECONDS && !lock_said)
		{
			lock_said = 1;
			host_logf(HOST_LOG_ERROR, "deko3d: dk_lock has been held for %u s by thread handle 0x%x%s (render "
				"thread: %s)", (unsigned)lock_seconds, (unsigned)owner, owner == render_handle ? ", the render thread" :
				"", render_where);
			host_debug_thread_backtrace(owner, "deko3d: dk_lock's holder");
		}
		return;
	}
	lock_owner = owner;
	lock_seconds = 0;
	lock_said = 0;
	if (owner || pthread_mutex_trylock(&dk_lock) != 0)
		return;
	{
		uint32_t retired, waiting = 0;
		int index, error;

		if (dk.ready)
			fences_poll();
		retired = dk.serial_retired;
		for (index = 0; index < FRAMES; index++)
			if (dk.fence_pending[index] && dk.fence_serial[index] > waiting)
				waiting = dk.fence_serial[index];
		error = dk.queue && dkQueueIsInErrorState(dk.queue);
		pthread_mutex_unlock(&dk_lock);
		if (!waiting || retired != retired_seen || render_queued())
		{
			retired_seen = retired;
			gpu_seconds = 0;
			gpu_said = 0;
			return;
		}
		if (++gpu_seconds >= RENDER_STALL_SECONDS && !gpu_said)
		{
			gpu_said = 1;
			host_logf(HOST_LOG_ERROR, "deko3d: the GPU has finished nothing for %u s: submissions up to %u retired, %u "
				"waited for, %u recorded; the queue %s; the render thread idle (%s)%s", (unsigned)gpu_seconds,
				(unsigned)retired, (unsigned)waiting, (unsigned)dk.serial, error ? "in an error state" :
				"not in an error state", render_where, host_dk_trace ? "; the trace follows" :
				"; debug.gpu_trace = true would say which command");
			if (host_dk_trace)
				trace_dump();
		}
	}
}

static void *render_watchdog(void *unused)
{
	uint32_t seen = render_progress, still = 0;
	int said = 0;

	(void)unused;
	for (;;)
	{
		svcSleepThread(1000000000LL);
		watchdog_lock_and_gpu();
		if (render_progress != seen || !render_queued())
		{
			seen = render_progress;
			still = 0;
			said = 0;
			continue;
		}
		if (++still < RENDER_STALL_SECONDS || said)
			continue;
		said = 1;
		host_logf(HOST_LOG_ERROR, "deko3d: the render thread has run nothing for %u s with %u streams queued: %s "
			"(submission %u, %u retired, the queue %s)", (unsigned)still, (unsigned)render_queued(), render_where,
			(unsigned)dk.serial, (unsigned)dk.serial_retired,
			dk.queue && dkQueueIsInErrorState(dk.queue) ? "in an error state" : "not in an error state");
		host_debug_thread_backtrace(render_handle, "deko3d: the render thread");
	}
	return NULL;
}

static void *render_thread(void *unused)
{
	(void)unused;
	host_thread_place_on_helper_core();
	render_handle = threadGetCurHandle();
	{
		pthread_t watchdog;

		if (pthread_create(&watchdog, NULL, render_watchdog, NULL) == 0)
			pthread_detach(watchdog);
	}
	for (;;)
	{
		unsigned index;

		pthread_mutex_lock(&render_queue.lock);
		while (!render_queue.count)
			pthread_cond_wait(&render_queue.work, &render_queue.lock);
		index = render_queue.head;
		pthread_mutex_unlock(&render_queue.lock);
		render_where = "running a stream";

		submit_run(render_queue.streams[index].data, render_queue.streams[index].size);

		render_progress++;
		render_where = "waiting for a stream";
		pthread_mutex_lock(&render_queue.lock);
		render_queue.head = (render_queue.head + 1) % RENDER_QUEUE;
		render_queue.count--;
		pthread_cond_broadcast(&render_queue.room);
		pthread_mutex_unlock(&render_queue.lock);
	}
	return NULL;
}

/* the queue's buffers and its thread, the first time; FALSE if they cannot
be made (the stream is then run on the game thread) */
static int render_thread_start(void)
{
	pthread_attr_t attributes;
	pthread_t thread;

	if (render_queue.started || render_queue.failed)
		return render_queue.started;
	pthread_attr_init(&attributes);
	pthread_attr_setstacksize(&attributes, RENDER_THREAD_STACK);
	if (pthread_create(&thread, &attributes, render_thread, NULL) != 0)
	{
		host_logf(HOST_LOG_ERROR, "deko3d: the render thread could not be made; the game thread runs the commands");
		render_queue.failed = 1;
	}
	else
	{
		pthread_detach(thread);
		render_queue.started = 1;
		host_logf(HOST_LOG_INFO, "deko3d: a render thread runs the commands (%d streams queued at most)", RENDER_QUEUE);
	}
	pthread_attr_destroy(&attributes);
	return render_queue.started;
}

/* every stream handed over run (not the GPU's work: queue_wait_idle) */
static void render_queue_drain(void)
{
	if (!render_queue.started)
		return;
	pthread_mutex_lock(&render_queue.lock);
	while (render_queue.count)
		pthread_cond_wait(&render_queue.room, &render_queue.lock);
	pthread_mutex_unlock(&render_queue.lock);
}

void host_dk_submit(uint32_t commands, uint32_t size)
{
	const unsigned char *at = (const unsigned char *)(uintptr_t)commands;
	unsigned index;

	if (!dk.ready && !initialize())
		return;
	if (!host_dk_render_thread || !render_thread_start())
	{
		submit_run(at, size);
		return;
	}
	pthread_mutex_lock(&render_queue.lock);
	slow.streams++;
	if (render_queue.count == RENDER_QUEUE)
	{
		uint64_t waited = slow_now_ns();

		while (render_queue.count == RENDER_QUEUE)
			pthread_cond_wait(&render_queue.room, &render_queue.lock);
		slow.submit_wait_ns += slow_now_ns() - waited;
	}
	index = (render_queue.head + render_queue.count) % RENDER_QUEUE;
	pthread_mutex_unlock(&render_queue.lock);
	/* (the buffer is the game thread's until counted: the render thread
	takes only those counted) */
	if (render_queue.streams[index].capacity < size)
	{
		unsigned char *grown = realloc(render_queue.streams[index].data, size);

		if (!grown)
		{
			host_logf(HOST_LOG_ERROR, "deko3d: no memory to queue a stream of %u bytes; run on the game thread",
				(unsigned)size);
			render_queue_drain();
			submit_run(at, size);
			return;
		}
		render_queue.streams[index].data = grown;
		render_queue.streams[index].capacity = size;
	}
	memcpy(render_queue.streams[index].data, at, size);
	render_queue.streams[index].size = size;
	pthread_mutex_lock(&render_queue.lock);
	render_queue.count++;
	pthread_cond_signal(&render_queue.work);
	pthread_mutex_unlock(&render_queue.lock);
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

/* ---------- the record of a GPU fault (debug.gpu_trace) */

static const char *trace_kind_name(uint32_t kind)
{
	switch (kind)
	{
	case DK_COMMAND_TARGETS: return "targets";
	case DK_COMMAND_CLEAR: return "clear";
	case DK_COMMAND_PRESENT: return "present";
	case DK_COMMAND_STATE: return "state";
	case DK_COMMAND_SHADERS: return "shaders";
	case DK_COMMAND_VERTEX_FORMAT: return "format";
	case DK_COMMAND_DRAW: return "draw";
	case DK_COMMAND_TEXTURE: return "texture";
	case DK_COMMAND_TEXTURE_FREE: return "texture free";
	case DK_COMMAND_TEXTURES: return "stages";
	case DK_COMMAND_TEXTURE_ROWS: return "texture rows";
	case DK_COMMAND_COMPOSITE: return "composite";
	case DK_COMMAND_BLIT: return "blit";
	case DK_COMMAND_VISIBILITY_END: return "visibility";
	case DK_COMMAND_MARKER: return "pass";
	case TRACE_SUBMIT: return "SUBMISSION";
	case TRACE_QUEUE: return "to the queue";
	case TRACE_ROLLOVER: return "rollover";
	case TRACE_COMPOSITE_COPY: return "composite copy";
	case TRACE_RELEASE: return "release";
	default: return "?";
	}
}

/* (the kernel's NvNotificationType) */
static const char *notification_name(uint32_t type)
{
	switch (type)
	{
	case 8: return "FIFO idle timeout";
	case 13: return "graphics software notify";
	case 24: return "graphics semaphore timeout";
	case 25: return "graphics illegal notify (a bad method or state)";
	case 31: return "MMU fault (memory the GPU may not read or write)";
	case 32: return "PBDMA error (the command stream)";
	case 43: return "channel reset";
	case 80: return "push buffer CRC mismatch";
	default: return "?";
	}
}

static void trace_event_describe(const struct trace_event *event, char *text, size_t size)
{
	const uint32_t *a = event->a;
	const char *pass = trace_pass_name(event->pass);
	int length;

	length = snprintf(text, size, "#%u s%u f%u %c %-13s [%s] ", (unsigned)event->number, (unsigned)event->serial,
		(unsigned)event->frame, event->reported ? 'r' : ' ', trace_kind_name(event->kind), pass);
	if (length < 0 || (size_t)length >= size)
		return;
	text += length;
	size -= (size_t)length;
	switch (event->kind)
	{
	case DK_COMMAND_TARGETS:
		snprintf(text, size, "color %08x %ux%u (game %ux%u) #%u gpu %010llx, depth %08x %ux%u #%u gpu %010llx, kinds %x",
			(unsigned)a[0], (unsigned)(a[1] & 0xffff), (unsigned)(a[1] >> 16), (unsigned)(a[7] & 0xffff),
			(unsigned)(a[7] >> 16), (unsigned)a[5], (unsigned long long)event->g[0], (unsigned)a[3],
			(unsigned)(a[4] & 0xffff), (unsigned)(a[4] >> 16), (unsigned)a[6], (unsigned long long)event->g[1],
			(unsigned)a[2]);
		break;
	case DK_COMMAND_CLEAR:
		snprintf(text, size, "flags %02x, %u rectangles, the first %u,%u %ux%u, target %ux%u", (unsigned)a[0],
			(unsigned)a[1], (unsigned)a[2], (unsigned)a[3], (unsigned)a[4], (unsigned)a[5], (unsigned)(a[6] & 0xffff),
			(unsigned)(a[6] >> 16));
		break;
	case DK_COMMAND_PRESENT:
		snprintf(text, size, "back buffer %08x %ux%u gpu %010llx, screenshot %08x", (unsigned)a[0],
			(unsigned)(a[1] & 0xffff), (unsigned)(a[1] >> 16), (unsigned long long)event->g[0], (unsigned)a[2]);
		break;
	case DK_COMMAND_STATE:
		snprintf(text, size, "viewport %d,%d %dx%d, scissor %d,%d %dx%d, flags %x, color mask %x", (int)a[0], (int)a[1],
			(int)a[2], (int)a[3], (int)a[4], (int)a[5], (int)a[6], (int)a[7], (unsigned)a[8], (unsigned)a[9]);
		break;
	case DK_COMMAND_SHADERS:
		snprintf(text, size, "vertex %u, pixel %u", (unsigned)a[0], (unsigned)a[1]);
		break;
	case DK_COMMAND_VERTEX_FORMAT:
		snprintf(text, size, "streams %04x, strides %u %u %u %u %u %u", (unsigned)a[0], (unsigned)a[1], (unsigned)a[2],
			(unsigned)a[3], (unsigned)a[4], (unsigned)a[5], (unsigned)a[6]);
		break;
	case DK_COMMAND_DRAW:
		snprintf(text, size, "%s primitive %u, %u %s, offset %d, %u streams (first %08x %u bytes gpu %010llx, second gpu "
			"%010llx), indices %s gpu %010llx, shaders %u/%u, textures %08x %08x %08x %08x, into %08x/%08x",
			(a[0] >> 24) ? "SKIPPED" : "", (unsigned)(a[0] & 0xff), (unsigned)a[1],
			((a[0] >> 16) & 0xff) ? "indices" : "vertices", (int)a[2], (unsigned)((a[0] >> 8) & 0xff), (unsigned)a[3],
			(unsigned)a[4], (unsigned long long)event->g[0], (unsigned long long)event->g[1],
			((a[0] >> 16) & 0xff) == 1 ? "inline" : ((a[0] >> 16) & 0xff) == 2 ? "at an address" : "none",
			(unsigned long long)event->g[2], (unsigned)a[6], (unsigned)a[7], (unsigned)a[8], (unsigned)a[9],
			(unsigned)a[10], (unsigned)a[11], (unsigned)(event->g[3] >> 32), (unsigned)(event->g[3] & 0xffffffffu));
		break;
	case DK_COMMAND_TEXTURE:
		snprintf(text, size, "image %u kind %u format %u %ux%ux%u %u levels, from %08x (%u bytes, gpu %010llx), slot %u, "
			"block %u %08x+%08x gpu %010llx%s", (unsigned)a[0], (unsigned)(a[1] & 0xff), (unsigned)((a[1] >> 8) & 0xff),
			(unsigned)a[2], (unsigned)a[3], (unsigned)a[4], (unsigned)(a[1] >> 16), (unsigned)a[5], (unsigned)a[6],
			(unsigned long long)event->g[1], (unsigned)a[7], (unsigned)a[8], (unsigned)a[9], (unsigned)a[10],
			(unsigned long long)event->g[0], a[11] ? ", was there" : ", new");
		break;
	case DK_COMMAND_TEXTURE_ROWS:
		snprintf(text, size, "image %u level %u rows %u+%u from %08x (%u bytes, gpu %010llx), slot %u, image gpu %010llx",
			(unsigned)a[0], (unsigned)a[1], (unsigned)a[2], (unsigned)a[3], (unsigned)a[4], (unsigned)a[5],
			(unsigned long long)event->g[0], (unsigned)a[6], (unsigned long long)event->g[1]);
		break;
	case DK_COMMAND_TEXTURE_FREE:
		snprintf(text, size, "image %u%s", (unsigned)a[0], a[1] ? "" : " (not there)");
		break;
	case DK_COMMAND_TEXTURES:
		snprintf(text, size, "images %u %u %u %u, targets %08x %08x %08x %08x, composites %u %u %u %u", (unsigned)a[0],
			(unsigned)a[1], (unsigned)a[2], (unsigned)a[3], (unsigned)a[4], (unsigned)a[5], (unsigned)a[6], (unsigned)a[7],
			(unsigned)a[8], (unsigned)a[9], (unsigned)a[10], (unsigned)a[11]);
		break;
	case DK_COMMAND_COMPOSITE:
		snprintf(text, size, "%08x %ux%u %u levels, slot %u", (unsigned)a[0], (unsigned)a[1], (unsigned)a[2],
			(unsigned)a[3], (unsigned)a[4]);
		break;
	case DK_COMMAND_BLIT:
		snprintf(text, size, "%08x %u,%u %ux%u into %08x %u,%u %ux%u%s", (unsigned)a[0], (unsigned)a[1], (unsigned)a[2],
			(unsigned)a[3], (unsigned)a[4], (unsigned)a[5], (unsigned)a[6], (unsigned)a[7], (unsigned)(a[8] & 0xffff),
			(unsigned)(a[8] >> 16), a[9] ? "" : " (SKIPPED)");
		break;
	case TRACE_COMPOSITE_COPY:
		snprintf(text, size, "%08x, %u of %u levels drawn, slot %u", (unsigned)a[0], (unsigned)a[1], (unsigned)a[2],
			(unsigned)a[3]);
		break;
	case DK_COMMAND_VISIBILITY_END:
		snprintf(text, size, "slot %u", (unsigned)a[0]);
		break;
	case DK_COMMAND_MARKER:
		snprintf(text, size, "%s", trace_pass_name(a[0]));
		break;
	case TRACE_SUBMIT:
		snprintf(text, size, "%u bytes of commands", (unsigned)a[0]);
		break;
	case TRACE_QUEUE:
		snprintf(text, size, "fenced as %u%s, %u KB of command memory, %u bytes uploaded", (unsigned)a[0],
			a[3] ? " (part of a stream)" : "", (unsigned)(a[1] / 1024), (unsigned)a[2]);
		break;
	case TRACE_ROLLOVER:
		snprintf(text, size, "%u KB of command memory", (unsigned)(a[0] / 1024));
		break;
	case TRACE_RELEASE:
		snprintf(text, size, "image %u slot %u block %u %08x+%08x (%u waiting)", (unsigned)a[0], (unsigned)a[1],
			(unsigned)a[2], (unsigned)a[3], (unsigned)a[4], (unsigned)a[5]);
		break;
	default:
		text[0] = 0;
		break;
	}
}

/* what the host holds: a line every ten seconds, and in a fault's record */
static void trace_held(void)
{
	uint32_t index, present = 0, free_ranges = 0, image_bytes = 0;

	for (index = 0; index < DK_TEXTURE_LIMIT; index++)
		present += tex.table[index].present ? 1 : 0;
	for (index = 0; index < tex.block_count; index++)
		free_ranges += tex.blocks[index].free_count;
	for (index = 0; index < (uint32_t)dk.image_block_count; index++)
		image_bytes += dk.image_blocks[index].used;
	host_logf(HOST_LOG_INFO, "deko3d: held: %u images, slots %u made %u free, %u releases waiting, %u texture blocks "
		"(%u free ranges), %u targets and composites in %u KB, %u composites, %u samplers; commands %u, checks %u",
		(unsigned)present, (unsigned)tex.slot_next, (unsigned)tex.slots_free_count, (unsigned)tex.release_count,
		(unsigned)tex.block_count, (unsigned)free_ranges, (unsigned)dk.target_count, (unsigned)(image_bytes / 1024),
		(unsigned)composite_count, (unsigned)tex.sampler_count, (unsigned)trace.next, (unsigned)trace.checks);
}

static void trace_dump_events(uint32_t first, uint32_t end, uint32_t finished)
{
	char line[640];
	uint32_t number;
	int said_boundary = 0;

	for (number = first; number < end; number++)
	{
		const struct trace_event *event = &trace.events[number % TRACE_EVENTS];

		if (event->number != number)
			continue;
		if (!said_boundary && finished && number > finished)
		{
			host_logf(HOST_LOG_WARN, "deko3d fault: ---- the GPU finished the commands above; it faulted in one of "
				"those below, most likely the first marked r ----");
			said_boundary = 1;
		}
		trace_event_describe(event, line, sizeof(line));
		host_logf(HOST_LOG_WARN, "deko3d fault: %s", line);
	}
}

static void trace_dump(void)
{
	NvGpuChannel *channel;
	NvNotification notification;
	NvError error;
	Result result;
	uint32_t finished, begun, oldest, first, index;
	char words[400];
	int length = 0;

	host_logf(HOST_LOG_ERROR, "deko3d fault: the record of the GPU fault follows (debug.gpu_trace)");
	/* the queue's GPU channel: deko3d's Queue keeps it 0x18 bytes in (its
	checkError asks the channel the same two questions); this deko3d's
	layout, read from its dkQueueIsInErrorState and Queue::checkError */
	channel = (NvGpuChannel *)((char *)dk.queue + 0x18);
	memset(&notification, 0, sizeof(notification));
	result = nvGpuChannelGetErrorNotification(channel, &notification);
	host_logf(HOST_LOG_WARN, "deko3d fault: the kernel's notification (result %08x): type %u, %s; info16 %u, status %u, "
		"timestamp %llu", (unsigned)result, (unsigned)notification.info32, notification_name(notification.info32),
		(unsigned)notification.info16, (unsigned)notification.status, (unsigned long long)notification.timestamp);
	memset(&error, 0, sizeof(error));
	result = nvGpuChannelGetErrorInfo(channel, &error);
	for (index = 0; index < 31 && length < (int)sizeof(words) - 12; index++)
		length += snprintf(words + length, sizeof(words) - (size_t)length, " %08x", (unsigned)error.info[index]);
	host_logf(HOST_LOG_WARN, "deko3d fault: the kernel's error info (result %08x): type %u, %s; words%s", (unsigned)result,
		(unsigned)error.type, notification_name(error.type), words);

	finished = trace.progress ? trace.progress[0] : 0;
	begun = trace.progress ? trace.progress[4] : 0;
	host_logf(HOST_LOG_WARN, "deko3d fault: submission %u being recorded, %u fenced, %u retired; the GPU began "
		"submission %u and finished command #%u; %u commands recorded; frame %lu", (unsigned)dk.serial,
		(unsigned)dk.serial_fenced, (unsigned)dk.serial_retired, (unsigned)begun, (unsigned)finished,
		(unsigned)(trace.next - 1), dk.frames_presented);
	for (index = 0; index < FRAMES; index++)
		host_logf(HOST_LOG_WARN, "deko3d fault: fence %u: %s, submission %u, %s", (unsigned)index,
			dk.fence_pending[index] ? "waited for" : "not waited for", (unsigned)dk.fence_serial[index],
			dkFenceWait(&dk.fences[index], 0) == DkResult_Success ? "passed" : "not passed");

	trace_held();
	host_logf(HOST_LOG_WARN, "deko3d fault: upload %u bytes, staging %u bytes, command memory %u KB, %d window chunks, "
		"%d image blocks, %u texture blocks", (unsigned)dk.upload_used, (unsigned)tex.staging_used,
		(unsigned)(command_memory_used() / 1024), dk.window_chunk_count, dk.image_block_count, (unsigned)tex.block_count);
	for (index = 0; index < (uint32_t)dk.window_chunk_count; index++)
		host_logf(HOST_LOG_WARN, "deko3d fault: window chunk %u at %08llx, gpu %010llx", (unsigned)index,
			(unsigned long long)dk.window_chunks[index].address, (unsigned long long)dk.window_chunks[index].gpu);
	for (index = 0; index < tex.block_count; index++)
	{
		uint32_t range, free_bytes = 0, largest = 0;

		for (range = 0; range < tex.blocks[index].free_count; range++)
		{
			free_bytes += tex.blocks[index].free[range].size;
			if (tex.blocks[index].free[range].size > largest)
				largest = tex.blocks[index].free[range].size;
		}
		host_logf(HOST_LOG_WARN, "deko3d fault: texture block %u gpu %010llx: %u ranges free, %u KB, the largest %u KB",
			(unsigned)index, (unsigned long long)dkMemBlockGetGpuAddr(tex.blocks[index].memory),
			(unsigned)tex.blocks[index].free_count, (unsigned)(free_bytes / 1024), (unsigned)(largest / 1024));
	}
	for (index = 0; index < (uint32_t)dk.target_count; index++)
	{
		const struct target *target = &dk.targets[index];

		host_logf(HOST_LOG_WARN, "deko3d fault: target #%u %s %08x game %ux%u pixels %ux%u gpu %010llx+%u, slot %d, "
			"bound at %u%s", (unsigned)index + 1, target->surface.kind == DK_SURFACE_DEPTH ? "depth" : "color",
			(unsigned)target->surface.data, (unsigned)target->surface.width, (unsigned)target->surface.height,
			(unsigned)SURFACE_PIXEL_WIDTH(&target->surface), (unsigned)SURFACE_PIXEL_HEIGHT(&target->surface),
			(unsigned long long)target->gpu, (unsigned)target->bytes, target->has_slot ? (int)target->slot : -1,
			(unsigned)target->bound, target == dk.color || target == dk.depth ? ", bound now" : "");
	}
	for (index = 0; index < (uint32_t)composite_count; index++)
		host_logf(HOST_LOG_WARN, "deko3d fault: composite %u %08x %ux%u %u levels, slot %u", (unsigned)index,
			(unsigned)composites[index].defined.data, (unsigned)composites[index].defined.width,
			(unsigned)composites[index].defined.height, (unsigned)composites[index].defined.levels,
			(unsigned)composites[index].slot);

	if (!trace.events)
	{
		host_logf(HOST_LOG_ERROR, "deko3d fault: no commands were kept");
		return;
	}
	oldest = trace.next > TRACE_EVENTS ? trace.next - TRACE_EVENTS + 1 : 1;
	if (finished >= oldest && finished < trace.next)
		first = finished > oldest + TRACE_DUMP_BEFORE ? finished - TRACE_DUMP_BEFORE : oldest;
	else
	{
		host_logf(HOST_LOG_WARN, "deko3d fault: the command the GPU finished last (#%u) is not kept; the last ones "
			"recorded follow", (unsigned)finished);
		first = trace.next > oldest + TRACE_DUMP_LIMIT ? trace.next - TRACE_DUMP_LIMIT : oldest;
		finished = 0;
	}
	host_logf(HOST_LOG_WARN, "deko3d fault: the commands (#number, submission, frame, r: the GPU reports getting past it, "
		"kind, [part of the frame]):");
	if (trace.next - first <= TRACE_DUMP_LIMIT + TRACE_DUMP_TAIL)
		trace_dump_events(first, trace.next, finished);
	else
	{
		trace_dump_events(first, first + TRACE_DUMP_LIMIT, finished);
		host_logf(HOST_LOG_WARN, "deko3d fault: ... %u commands left out ...",
			(unsigned)(trace.next - TRACE_DUMP_TAIL - first - TRACE_DUMP_LIMIT));
		trace_dump_events(trace.next - TRACE_DUMP_TAIL, trace.next, 0);
	}
	host_logf(HOST_LOG_ERROR, "deko3d fault: the record ends");
}

/* ---------- a captured frame read (debug.gpu_capture) */

struct capture_group
{
	uint32_t key;
	uint64_t ns;
	uint32_t commands;
	uint64_t vertices;
	/* the time in each part of the frame, for the part it is mostly in */
	uint64_t pass_ns[TRACE_NAMES];
};

static struct capture_group *capture_group_get(struct capture_group *groups, uint32_t *count, uint32_t key)
{
	uint32_t index;

	for (index = 0; index < *count; index++)
	{
		if (groups[index].key == key)
			return &groups[index];
	}
	if (*count == CAPTURE_GROUPS)
		return NULL;
	memset(&groups[*count], 0, sizeof(groups[0]));
	groups[*count].key = key;
	return &groups[(*count)++];
}

static void capture_groups_sort(struct capture_group *groups, uint32_t count)
{
	uint32_t index;

	for (index = 1; index < count; index++)
	{
		struct capture_group moving = groups[index];
		uint32_t at = index;

		while (at > 0 && groups[at - 1].ns < moving.ns)
		{
			groups[at] = groups[at - 1];
			at--;
		}
		groups[at] = moving;
	}
}

static const char *capture_group_pass(const struct capture_group *group)
{
	uint32_t pass, best = 0;

	for (pass = 1; pass < TRACE_NAMES; pass++)
	{
		if (group->pass_ns[pass] > group->pass_ns[best])
			best = pass;
	}
	return trace_pass_name(best);
}

static void capture_shader_name(uint32_t handle, char *text, size_t size)
{
	uint64_t hash;
	uint32_t stage, code_size, registers;

	if (host_dk_shader_describe(handle, &hash, &stage, &code_size, &registers))
		snprintf(text, size, "%c%016llx (%u bytes of code, %u registers)", stage ? 'f' : 'v', (unsigned long long)hash,
			(unsigned)code_size, (unsigned)registers);
	else
		snprintf(text, size, "handle %u", (unsigned)handle);
}

static void capture_target_name(uint32_t data, char *text, size_t size)
{
	int index;

	for (index = dk.target_count - 1; index >= 0; index--)
	{
		const struct target *target = &dk.targets[index];

		if (target->surface.data == data && target->surface.kind == DK_SURFACE_COLOR)
		{
			snprintf(text, size, "%08x %ux%u", (unsigned)data, (unsigned)SURFACE_PIXEL_WIDTH(&target->surface),
				(unsigned)SURFACE_PIXEL_HEIGHT(&target->surface));
			return;
		}
	}
	snprintf(text, size, "%08x", (unsigned)data);
}

static void capture_report(void)
{
	static struct capture_group pixel[CAPTURE_GROUPS], vertex[CAPTURE_GROUPS], targets[CAPTURE_GROUPS],
		kinds[CAPTURE_GROUPS];
	uint32_t pixel_count = 0, vertex_count = 0, target_count = 0, kind_count = 0;
	uint32_t longest[CAPTURE_LONGEST], longest_count = 0;
	static uint64_t ns[CAPTURE_LIMIT];
	uint64_t total = 0, gaps = 0, lost = 0;
	uint32_t index, gap_count = 0;
	char name[96], target[64];

	for (index = 0; index < capture.count; index++)
	{
		uint64_t start = capture.reports[index * 2 + 1], end = capture.reports[(index + 1) * 2 + 1];
		const struct capture_entry *entry = &capture.entries[index];
		struct capture_group *group;

		ns[index] = 0;
		if (!start || !end || end < start)
		{
			lost++;
			continue;
		}
		ns[index] = dkTimestampToNs(end - start);
		if (entry->after_gap)
		{
			gaps += ns[index];
			gap_count++;
			continue;
		}
		total += ns[index];
		if ((group = capture_group_get(kinds, &kind_count, entry->kind)) != NULL)
		{
			group->ns += ns[index];
			group->commands++;
		}
		if ((group = capture_group_get(targets, &target_count, entry->color)) != NULL)
		{
			group->ns += ns[index];
			group->commands++;
			group->pass_ns[entry->pass % TRACE_NAMES] += ns[index];
		}
		if (entry->kind == DK_COMMAND_DRAW)
		{
			if ((group = capture_group_get(pixel, &pixel_count, entry->pixel_shader)) != NULL)
			{
				group->ns += ns[index];
				group->commands++;
				group->vertices += entry->count;
				group->pass_ns[entry->pass % TRACE_NAMES] += ns[index];
			}
			if ((group = capture_group_get(vertex, &vertex_count, entry->vertex_shader)) != NULL)
			{
				group->ns += ns[index];
				group->commands++;
				group->vertices += entry->count;
				group->pass_ns[entry->pass % TRACE_NAMES] += ns[index];
			}
		}
		/* (the longest, kept in order) */
		{
			uint32_t at;

			if (longest_count < CAPTURE_LONGEST)
				at = longest_count++;
			else if (ns[index] > ns[longest[CAPTURE_LONGEST - 1]])
				at = CAPTURE_LONGEST - 1;
			else
				continue;
			while (at > 0 && ns[longest[at - 1]] < ns[index])
			{
				longest[at] = longest[at - 1];
				at--;
			}
			longest[at] = index;
		}
	}
	if (capture.slow_ns_recording)
		host_logf(HOST_LOG_INFO, "deko3d capture: (after a slow frame: the GPU %.2f ms on it)",
			(double)capture.slow_ns_recording / 1e6);
	host_logf(HOST_LOG_INFO, "deko3d capture: frame %lu: %u commands, the GPU %.2f ms on them, and %.2f ms on %u after "
		"a submission (its wait for the CPU in it); %u not timed", capture.frame, (unsigned)capture.count,
		(double)total / 1e6, (double)gaps / 1e6, (unsigned)gap_count, (unsigned)lost);
	capture_groups_sort(kinds, kind_count);
	for (index = 0; index < kind_count; index++)
		host_logf(HOST_LOG_INFO, "deko3d capture: %s: %.2f ms in %u", trace_kind_name(kinds[index].key),
			(double)kinds[index].ns / 1e6, (unsigned)kinds[index].commands);
	capture_groups_sort(pixel, pixel_count);
	host_logf(HOST_LOG_INFO, "deko3d capture: %u pixel shaders drew; the 24 that took longest:", (unsigned)pixel_count);
	for (index = 0; index < pixel_count && index < 24; index++)
	{
		capture_shader_name(pixel[index].key, name, sizeof(name));
		host_logf(HOST_LOG_INFO, "deko3d capture:   %.3f ms, %u draws, %llu vertices: %s, mostly in %s",
			(double)pixel[index].ns / 1e6, (unsigned)pixel[index].commands, (unsigned long long)pixel[index].vertices,
			name, capture_group_pass(&pixel[index]));
	}
	capture_groups_sort(vertex, vertex_count);
	host_logf(HOST_LOG_INFO, "deko3d capture: %u vertex shaders drew; the 12 that took longest:", (unsigned)vertex_count);
	for (index = 0; index < vertex_count && index < 12; index++)
	{
		capture_shader_name(vertex[index].key, name, sizeof(name));
		host_logf(HOST_LOG_INFO, "deko3d capture:   %.3f ms, %u draws, %llu vertices: %s, mostly in %s",
			(double)vertex[index].ns / 1e6, (unsigned)vertex[index].commands, (unsigned long long)vertex[index].vertices,
			name, capture_group_pass(&vertex[index]));
	}
	capture_groups_sort(targets, target_count);
	host_logf(HOST_LOG_INFO, "deko3d capture: %u color targets drawn into; the 10 that took longest:",
		(unsigned)target_count);
	for (index = 0; index < target_count && index < 10; index++)
	{
		capture_target_name(targets[index].key, target, sizeof(target));
		host_logf(HOST_LOG_INFO, "deko3d capture:   %.3f ms, %u commands: %s, mostly in %s",
			(double)targets[index].ns / 1e6, (unsigned)targets[index].commands, target,
			capture_group_pass(&targets[index]));
	}
	host_logf(HOST_LOG_INFO, "deko3d capture: the longest commands:");
	for (index = 0; index < longest_count; index++)
	{
		const struct capture_entry *entry = &capture.entries[longest[index]];
		char vertex_name[96];

		capture_shader_name(entry->pixel_shader, name, sizeof(name));
		capture_shader_name(entry->vertex_shader, vertex_name, sizeof(vertex_name));
		capture_target_name(entry->color, target, sizeof(target));
		if (entry->kind == DK_COMMAND_DRAW)
			host_logf(HOST_LOG_INFO, "deko3d capture:   %.3f ms, #%u %s [%s]: primitive %u, %u %s, pixel %s, vertex %s, "
				"texture %08x, blend %u (%u, %u, op %u), depth test %u write %u, into %s", (double)ns[longest[index]] / 1e6,
				(unsigned)longest[index], trace_kind_name(entry->kind), trace_pass_name(entry->pass),
				(unsigned)(entry->primitive & 0xff), (unsigned)entry->count,
				((entry->primitive >> 16) & 0xff) ? "indices" : "vertices", name, vertex_name, (unsigned)entry->texture0,
				(unsigned)entry->blend, (unsigned)entry->blend_source, (unsigned)entry->blend_destination,
				(unsigned)entry->blend_equation, (unsigned)(entry->depth_state & 1), (unsigned)(entry->depth_state >> 1 & 1),
				target);
		else
			host_logf(HOST_LOG_INFO, "deko3d capture:   %.3f ms, #%u %s [%s], into %s", (double)ns[longest[index]] / 1e6,
				(unsigned)longest[index], trace_kind_name(entry->kind), trace_pass_name(entry->pass), target);
	}
}

static void capture_frame_begin(void)
{
	if (!capture.memory)
		return;
	if (capture.waiting && dk.frame == capture.slot)
	{
		capture.waiting = 0;
		capture_report();
	}
	if (capture.armed && !capture.recording && !capture.waiting && !dk.faulted)
	{
		capture.armed = 0;
		capture.recording = 1;
		capture.slot = dk.frame;
		capture.count = 0;
		capture.gap_next = 0;
		capture.frame = dk.frames_presented;
		capture.slow_ns_recording = capture.slow_ns;
		capture.slow_ns = 0;
		memset((void *)capture.reports, 0, (CAPTURE_LIMIT + 1) * 16);
		dkCmdBufReportCounter(dk.commands, DkCounter_Timestamp, capture.gpu);
	}
}
