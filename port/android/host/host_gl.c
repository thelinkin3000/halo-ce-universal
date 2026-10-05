/*
HOST_GL.C

OpenGL ES for the guest. Its generated entry points (guest_gl.c) import
hostgl_<function>, resolved here to the driver's function; the arguments
already have host types by then. Only strings need copying back.
*/

#include "host.h"

#include <EGL/egl.h>
#include <GLES3/gl32.h>
#include <dlfcn.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

/* ---------- the GL calls the Vulkan image still makes

The Vulkan image shares objects with the GL ES one whose own GL calls are not
replaced yet (xbox_textures.c, hud_hires.c, text_hires.c, menu_files.c and the
shader generators: phase 6). Under Vulkan there is no GL context, so they must
reach nothing; and phase 6 needs to know what they reach. Each such import
resolves to a stub of its own that counts the call, remembers its caller (the
guest address the call returns to, which a symbolizer names from the image's
map) and returns zero without calling the driver. Not atomic across threads: the
counts are for reading, not for deciding. */

#define GL_STUB_COUNT 256
#define GL_STUB_SIZE 64

struct gl_count
{
	uint64_t calls;
	uint64_t caller; /* the guest's link register at the latest call */
};

struct gl_count host_gl_counts[GL_STUB_COUNT] __attribute__((visibility("hidden"))); /* named by the stubs below */
static char *gl_names[GL_STUB_COUNT];
static int gl_names_used;
int host_gl_statistics;

/* one stub per count: bump the call count (an exclusive load and store, so that
guest threads' calls are not lost), record the caller, return zero in the
integer and the floating point result registers. x15 to x17 are the registers
a call may clobber without being asked to preserve them. Each is padded to
GL_STUB_SIZE. */
__asm__(
	".text\n"
	".balign 64\n"
	".globl host_gl_stubs\n"
	".hidden host_gl_stubs\n"
	"host_gl_stubs:\n"
	".set host_gl_stub_index, 0\n"
	".rept " "256" "\n"
	"adrp x16, host_gl_counts + host_gl_stub_index * 16\n"
	"add x16, x16, :lo12:(host_gl_counts + host_gl_stub_index * 16)\n"
	"1: ldxr x17, [x16]\n"
	"add x17, x17, #1\n"
	"stxr w15, x17, [x16]\n"
	"cbnz w15, 1b\n"
	"str x30, [x16, #8]\n"
	"mov x0, xzr\n"
	"movi v0.2d, #0\n"
	"ret\n"
	".balign 64\n"
	".set host_gl_stub_index, host_gl_stub_index + 1\n"
	".endr\n");
extern char host_gl_stubs[];

static void *gl_stub(const char *name)
{
	static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
	void *stub = NULL;
	int index;

	pthread_mutex_lock(&lock);
	for (index = 0; index < gl_names_used; index++)
	{
		if (!strcmp(gl_names[index], name))
			break;
	}
	if (index == gl_names_used && gl_names_used < GL_STUB_COUNT)
	{
		gl_names[gl_names_used++] = strdup(name);
		index = gl_names_used - 1;
	}
	if (index < gl_names_used)
		stub = host_gl_stubs + (size_t)index * GL_STUB_SIZE;
	pthread_mutex_unlock(&lock);
	return stub;
}

/* the calls counted since the last report, one line each: the function, how
many calls, and the guest address one of them came from */
static void gl_report(unsigned frames)
{
	int index, called = 0;

	for (index = 0; index < gl_names_used; index++)
	{
		uint64_t calls = host_gl_counts[index].calls;

		if (!calls)
			continue;
		called++;
		host_logf(HOST_LOG_INFO, "vk gl call: %s %llu calls in %u frames, from %08llx", gl_names[index],
			(unsigned long long)calls, frames, (unsigned long long)host_gl_counts[index].caller);
		host_gl_counts[index].calls = 0;
	}
	host_logf(HOST_LOG_INFO, "vk gl calls: %d of the %d GL imports called in %u frames", called, gl_names_used, frames);
}

/* called once a frame by the stand-in swap (host_sdl.c) */
void host_gl_frame(void)
{
	static unsigned frames;
	static struct timespec last;
	struct timespec now;

	frames++;
	clock_gettime(CLOCK_MONOTONIC, &now);
	if (!last.tv_sec)
		last = now;
	if (now.tv_sec - last.tv_sec >= 10)
	{
		if (host_gl_statistics)
			gl_report(frames);
		else
			memset(host_gl_counts, 0, sizeof(host_gl_counts));
		frames = 0;
		last = now;
	}
}

void *host_gl_resolve(const char *name)
{
	static void *library;
	void *function = NULL;

	if (host_renderer_vulkan)
	{
		void *stub = gl_stub(name);
		static int tested;

		/* once: a call through the first stub must be counted, and the count taken back */
		if (stub && !tested)
		{
			uint64_t before = host_gl_counts[0].calls;

			tested = 1;
			((long (*)(void))stub)();
			host_logf(HOST_LOG_INFO, "vk gl stubs: self-test %s", host_gl_counts[0].calls == before + 1 ? "ok" : "FAILED");
			host_gl_counts[0].calls = before;
			host_gl_counts[0].caller = 0;
		}
		return stub;
	}
	if (!library)
		library = dlopen("libGLESv3.so", RTLD_NOW | RTLD_GLOBAL);
	if (library)
		function = dlsym(library, name);
	if (!function)
		function = (void *)eglGetProcAddress(name);
	return function;
}

void host_gl_get_string(uint32_t name, int index, char *buffer, uint32_t size)
{
	const GLubyte *text;

	if (!size)
		return;
	buffer[0] = 0;
	/* under Vulkan there is no GL context, in which a GL library answers NULL,
	and the platform layer prints what it is told: "OpenGL %s on %s" (host.h) */
	if (host_renderer_vulkan)
	{
		if (index < 0)
			strncpy(buffer, "Vulkan", size - 1);
		buffer[size - 1] = 0;
		return;
	}
	text = index >= 0 ? glGetStringi(name, (GLuint)index) : glGetString(name);
	if (text)
	{
		strncpy(buffer, (const char *)text, size - 1);
		buffer[size - 1] = 0;
	}
}

int host_gl_has_extension(const char *name)
{
	GLint count = 0, index;

	if (host_renderer_vulkan)
		return 0;
	glGetIntegerv(GL_NUM_EXTENSIONS, &count);
	for (index = 0; index < count; index++)
	{
		const char *extension = (const char *)glGetStringi(GL_EXTENSIONS, (GLuint)index);

		if (extension && !strcmp(extension, name))
			return 1;
	}
	return 0;
}

/* one 32-bit word of a buffer object (the visibility test counters of
d3d8_gl.c); ES has no glGetBufferSubData, and the mapping it offers
instead is a host pointer */
uint32_t host_gl_read_buffer_word(uint32_t buffer, uint32_t offset)
{
	uint32_t value = 0;
	GLint previous = 0;
	const void *mapping;

	if (host_renderer_vulkan)
		return 0;
	glGetIntegerv(GL_ATOMIC_COUNTER_BUFFER_BINDING, &previous);
	glBindBuffer(GL_ATOMIC_COUNTER_BUFFER, buffer);
	mapping = glMapBufferRange(GL_ATOMIC_COUNTER_BUFFER, offset, sizeof(value), GL_MAP_READ_BIT);
	if (mapping)
	{
		memcpy(&value, mapping, sizeof(value));
		glUnmapBuffer(GL_ATOMIC_COUNTER_BUFFER);
	}
	glBindBuffer(GL_ATOMIC_COUNTER_BUFFER, (GLuint)previous);
	return value;
}

/* The renderer streams each frame's vertices and indices into the next of
a ring of buffers (d3d8_gl.c). A fence marks the end of each frame's work,
and a buffer is written again only once the GPU has passed the fence of the
frame that last used it: drivers queue several frames, and a draw still
waiting to run would otherwise read a later frame's vertices. */
#define FRAME_FENCE_SLOTS 8

static GLsync frame_fences[FRAME_FENCE_SLOTS];

void host_gl_fence_frame(uint32_t slot)
{
	if (host_renderer_vulkan)
		return;
	if (slot >= FRAME_FENCE_SLOTS)
		return;
	if (frame_fences[slot])
		glDeleteSync(frame_fences[slot]);
	frame_fences[slot] = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
}

void host_gl_wait_frame(uint32_t slot)
{
	if (host_renderer_vulkan)
		return;
	if (slot >= FRAME_FENCE_SLOTS || !frame_fences[slot])
		return;
	/* at most a second: a lost context must not hang the game */
	glClientWaitSync(frame_fences[slot], GL_SYNC_FLUSH_COMMANDS_BIT, 1000000000ull);
	glDeleteSync(frame_fences[slot]);
	frame_fences[slot] = NULL;
}

/* writes data into the buffer bound to target. The renderer streams a
range per draw, so a frame makes hundreds of these, and the cost per call
rather than per byte is what a frame is made of.

GL_MAP_INVALIDATE_RANGE_BIT was what made that cost ruinous. It tells the
driver the range's previous contents are undefined and must be discarded,
which is the very work GL_MAP_UNSYNCHRONIZED_BIT exists to avoid: that one
promises the caller that no queued draw is reading the range. Asked to do
both, Adreno pays for the discard - about 0.85 ms a call on a Galaxy Z
Flip 4, whatever the range written - and with a few hundred calls a frame
that came to 96% of a 550 ms frame at 1.8 fps, with the GPU idle throughout.

Without the invalidation the same call is well under a microsecond and the
same game runs at the display's refresh rate. The promise unsynchronized
makes still holds: the renderer only writes ranges that no queued draw reads,
because host_gl_wait_frame releases the ring slot first. */
void host_gl_buffer_write(uint32_t target, uint32_t offset, uint32_t size, const void *data)
{
	void *mapping;

	if (host_renderer_vulkan)
		return;
	mapping = glMapBufferRange(target, offset, size, GL_MAP_WRITE_BIT | GL_MAP_UNSYNCHRONIZED_BIT);

	if (!mapping)
	{
		glBufferSubData(target, offset, size, data);
		return;
	}
	memcpy(mapping, data, size);
	glUnmapBuffer(target);
}
