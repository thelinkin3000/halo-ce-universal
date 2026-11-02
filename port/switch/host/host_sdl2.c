/*
HOST_SDL2.C

SDL on behalf of the guest (guest/runtime/guest_sdl.c) for the Switch port.
The guest calls SDL3's functions; devkitPro's SDL2 for the Switch answers
them, because there is no SDL3 for the platform (devkitPro/SDL has
switch-sdl-3.2 and switch-sdl-3.4 branches, but no packaged, working port).
The replaced file, port/android/host/host_sdl.c, is the model: SDL objects
are 64-bit pointers, which the guest cannot hold, so it gets small handles
into the table here instead.

Where the SDL2 gets its GL comes from switch-mesa: devkitPro's package
links EGL and GLES statically, and SDL2's Switch video driver (devkitPro/
SDL src/video/switch/SDL_switchvideo.c) puts an EGL surface on the
console's default nwindow, so SDL_GL_CreateContext and SDL_GL_SwapWindow -
the two calls the renderer makes - work as they do on Knulli's framebuffer.

Note that devkitPro builds this SDL2 with --disable-filesystem: there are
no SDL_GetPrefPath and no SDL_LoadFile here. The host reads the game image
and finds the game data itself (host_main.c).

The guest's values are SDL3's. Most agree with SDL2's (the init flags,
scancodes, keycodes, key modifiers, gamepad buttons and axes, audio
formats, GL profile and flag bits); host_sdl3_events.c translates the
others (the GL attributes, gamepad types and the layout of events).

SDL2's audio callback runs on SDL's thread, which has no guest stack: as on
Android, each request goes to a thread that has one.
*/

#include "host.h"
#include "host_switch.h"

#include <SDL2/SDL.h>
#include <switch.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define HANDLE_COUNT 256

enum handle_type
{
	_handle_free,
	_handle_window,
	_handle_context,
	_handle_gamepad,
	_handle_audio,
	/* voice chat's microphone, a stream without a callback (host_microphone.c) */
	_handle_microphone,
};

struct handle
{
	int type;
	void *object;
};

static struct handle handles[HANDLE_COUNT];
static pthread_mutex_t handle_lock = PTHREAD_MUTEX_INITIALIZER;

static uint32_t handle_new(int type, void *object)
{
	uint32_t index;

	if (!object)
		return 0;
	pthread_mutex_lock(&handle_lock);
	/* an object that already has a handle keeps it */
	for (index = 1; index < HANDLE_COUNT; index++)
	{
		if (handles[index].type == type && handles[index].object == object)
		{
			pthread_mutex_unlock(&handle_lock);
			return index;
		}
	}
	for (index = 1; index < HANDLE_COUNT; index++)
	{
		if (handles[index].type == _handle_free)
		{
			handles[index].type = type;
			handles[index].object = object;
			pthread_mutex_unlock(&handle_lock);
			return index;
		}
	}
	pthread_mutex_unlock(&handle_lock);
	host_logf(HOST_LOG_ERROR, "out of SDL handles");
	return 0;
}

static void *handle_get(uint32_t handle, int type)
{
	void *object = NULL;

	if (handle == 0 || handle >= HANDLE_COUNT)
		return NULL;
	pthread_mutex_lock(&handle_lock);
	if (handles[handle].type == type)
		object = handles[handle].object;
	pthread_mutex_unlock(&handle_lock);
	return object;
}

/* ---------- general */

int host_sdl_init(uint32_t flags)
{
	/* SDL3's subsystem bits are SDL2's (SDL_INIT_GAMEPAD is
	SDL_INIT_GAMECONTROLLER). Under deko3d SDL's video stays down: the
	display is deko3d's, and input, audio and events need no window (the
	deko3d probe, port/switch/DEKO3D.md) */
	if (host_renderer_deko3d)
		flags &= ~(uint32_t)SDL_INIT_VIDEO;
	if (SDL_Init(flags) != 0)
	{
		host_logf(HOST_LOG_ERROR, "SDL_Init(%#x): %s", flags, SDL_GetError());
		return 0;
	}
	{
		const char *video = SDL_GetCurrentVideoDriver();
		const char *audio = SDL_GetCurrentAudioDriver();

		host_logf(HOST_LOG_INFO, "SDL %d.%d.%d, video %s, audio %s", SDL_MAJOR_VERSION, SDL_MINOR_VERSION,
			SDL_PATCHLEVEL, video ? video : "none", audio ? audio : "none");
	}
	return 1;
}

int host_sdl_set_hint(const char *name, const char *value)
{
	return SDL_SetHint(name, value) == SDL_TRUE;
}

void host_sdl_get_error(char *buffer, uint32_t size)
{
	SDL_strlcpy(buffer, SDL_GetError(), size);
}

int64_t host_sdl_ticks(void)
{
	return (int64_t)SDL_GetTicks64();
}

int64_t host_sdl_thread_id(void)
{
	return (int64_t)SDL_ThreadID();
}

/* ---------- video */

/* SDL3's window flags */
#define SDL3_WINDOW_FULLSCREEN 0x1ull
#define SDL3_WINDOW_OPENGL 0x2ull

/* Under deko3d the guest's window and GL context are these: handles to
nothing, which the functions below recognise and answer without SDL. The
guest's platform layer opens a window and a context as it does over Mesa, and
its event loop runs only while it has a window. */
static char standin_window, standin_context;

/* the console's screen, which under deko3d stands for the window's size */
static void standin_window_size(int *width, int *height)
{
	int docked = appletGetOperationMode() == AppletOperationMode_Console;

	*width = docked ? 1920 : 1280;
	*height = docked ? 1080 : 720;
}

uint32_t host_sdl_create_window(const char *title, int width, int height, int64_t flags)
{
	Uint32 host_flags = SDL_WINDOW_FULLSCREEN;
	SDL_Window *window;

	if (host_renderer_deko3d)
	{
		host_logf(HOST_LOG_INFO, "window: a stand-in, the display being deko3d's");
		return handle_new(_handle_window, &standin_window);
	}

	if ((uint64_t)flags & SDL3_WINDOW_OPENGL)
		host_flags |= SDL_WINDOW_OPENGL;
	/* Resizable, so that SDL follows the console being docked or undocked.
	 * Its Switch driver watches the operation mode and resizes the window
	 * when SDL_WINDOW_RESIZABLE is set, which is how the framebuffer keeps
	 * matching the screen while the game runs. Without it the window is
	 * created once at whatever size the game asked for and stays there,
	 * which on a Lite means a 1920x1080 framebuffer on a 1280x720 panel.
	 * The game draws 480 lines either way - both are 16:9 - so this is
	 * about the window matching the screen, not about the render size. */
	host_flags |= SDL_WINDOW_RESIZABLE;
	window = SDL_CreateWindow(title, SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, width, height, host_flags);
	if (!window)
	{
		host_logf(HOST_LOG_ERROR, "SDL_CreateWindow: %s", SDL_GetError());
		return 0;
	}
	SDL_GetWindowSize(window, &width, &height);
	host_logf(HOST_LOG_INFO, "window %dx%d", width, height);
	SDL_ShowCursor(SDL_DISABLE);
	return handle_new(_handle_window, window);
}

void host_sdl_window_size_in_pixels(uint32_t window, int *width, int *height)
{
	SDL_Window *object = handle_get(window, _handle_window);

	*width = 0;
	*height = 0;
	if ((void *)object == (void *)&standin_window)
		standin_window_size(width, height);
	else if (object)
		SDL_GL_GetDrawableSize(object, width, height);
}

int host_sdl_set_relative_mouse(uint32_t window, int enabled)
{
	(void)window;
	(void)enabled;
	return 1;
}

int host_sdl_gl_set_attribute(int attribute, int value)
{
	static const SDL_GLattr attributes[] =
	{
		[_host_gl_red_size] = SDL_GL_RED_SIZE,
		[_host_gl_green_size] = SDL_GL_GREEN_SIZE,
		[_host_gl_blue_size] = SDL_GL_BLUE_SIZE,
		[_host_gl_alpha_size] = SDL_GL_ALPHA_SIZE,
		[_host_gl_buffer_size] = SDL_GL_BUFFER_SIZE,
		[_host_gl_doublebuffer] = SDL_GL_DOUBLEBUFFER,
		[_host_gl_depth_size] = SDL_GL_DEPTH_SIZE,
		[_host_gl_stencil_size] = SDL_GL_STENCIL_SIZE,
		[_host_gl_multisamplebuffers] = SDL_GL_MULTISAMPLEBUFFERS,
		[_host_gl_multisamplesamples] = SDL_GL_MULTISAMPLESAMPLES,
		[_host_gl_context_major_version] = SDL_GL_CONTEXT_MAJOR_VERSION,
		[_host_gl_context_minor_version] = SDL_GL_CONTEXT_MINOR_VERSION,
		[_host_gl_context_flags] = SDL_GL_CONTEXT_FLAGS,
		[_host_gl_context_profile_mask] = SDL_GL_CONTEXT_PROFILE_MASK,
	};
	int index = host_sdl3_gl_attribute(attribute);

	if (host_renderer_deko3d)
		return 1;
	if (index < 0 || index >= (int)(sizeof(attributes) / sizeof(attributes[0])))
		return 0;
	return SDL_GL_SetAttribute(attributes[index], value) == 0;
}

uint32_t host_sdl_gl_create_context(uint32_t window)
{
	SDL_Window *object = handle_get(window, _handle_window);
	SDL_GLContext context;

	if (!object)
		return 0;
	if ((void *)object == (void *)&standin_window)
		return handle_new(_handle_context, &standin_context);
	context = SDL_GL_CreateContext(object);
	if (!context)
	{
		host_logf(HOST_LOG_ERROR, "SDL_GL_CreateContext: %s", SDL_GetError());
		return 0;
	}
	return handle_new(_handle_context, context);
}

int host_sdl_gl_make_current(uint32_t window, uint32_t context)
{
	if (handle_get(window, _handle_window) == (void *)&standin_window)
		return 1;
	return SDL_GL_MakeCurrent(handle_get(window, _handle_window), handle_get(context, _handle_context)) == 0;
}

/* the swap interval in effect (host_sdl_gl_swap_interval) */
static int swap_interval = 1;

int host_sdl_gl_set_swap_interval(int interval)
{
	/* HALO_SWAP_INTERVAL overrides the game's display.vsync */
	const char *setting = getenv("HALO_SWAP_INTERVAL");

	if (setting && *setting)
		interval = atoi(setting);
	swap_interval = interval;
	if (host_renderer_deko3d)
		return 1;
	return SDL_GL_SetSwapInterval(interval) == 0;
}

/* whether swaps wait for the display, for frame pacing (host_glthread.c):
the interval set last, the override included */
int host_sdl_gl_swap_interval(void)
{
	return swap_interval;
}

/* The clocks: raised for the game, and said once.

The game is bound by one CPU core - the game itself, the Direct3D to GL
translation and Mesa's driver all run on its thread, and only 2-4% of a
frame was spent waiting on the GPU - and a homebrew title starts at the
handheld defaults, 1020 MHz. 1785 MHz is the CPU's highest rate Nintendo's
own titles are allowed, and 1600 MHz the memory's docked rate, which titles
may also use in handheld. The rates found are put back when the game exits.

performance.cpu_clock chooses the CPU's rate (host_clock_cpu): 1785 MHz,
the default, 1020 MHz (cooler, and the battery lasts longer), or the rate
found ("stock").

The GPU was left where a homebrew title starts, 307 MHz, on the strength of
that 2-4%. It was measured in light scenes: in an internet game of 24
players the game fell to 27-35 fps with the swap still taking 0.2 ms, which
a driver blocking inside its own calls on a busy GPU would also look like.
So it is given what titles run at: 768 MHz docked, 460.8 MHz in handheld,
chosen by the mode the console is in when the game starts. */
#define GAME_CPU_HZ 1785000000u
#define GAME_MEMORY_HZ 1600000000u
#define GAME_GPU_DOCKED_HZ 768000000u
#define GAME_GPU_HANDHELD_HZ 460800000u

static u32 original_cpu_hz, original_memory_hz, original_gpu_hz;
/* performance.cpu_clock, as the game last said it (host_clock_cpu): the CPU's
rate, or 0 for the console's own; and whether the clocks are set yet */
static u32 wanted_cpu_hz = GAME_CPU_HZ;
static int clocks_set;

static void set_clock(PcvModuleId module, u32 hz)
{
	ClkrstSession session;

	if (!hz || R_FAILED(clkrstOpenSession(&session, module, 3)))
		return;
	clkrstSetClockRate(&session, hz);
	clkrstCloseSession(&session);
}

static u32 get_clock(PcvModuleId module)
{
	ClkrstSession session;
	u32 hz = 0;

	if (R_SUCCEEDED(clkrstOpenSession(&session, module, 3)))
	{
		clkrstGetClockRate(&session, &hz);
		clkrstCloseSession(&session);
	}
	return hz;
}

/* the guest's, when performance.cpu_clock is read and when it changes: the
CPU's rate in MHz (1785, 1020), or 0 for the rate the console had. Set at
once once the clocks are (log_clocks), else when they are */
void host_clock_cpu(unsigned int mhz)
{
	u32 hz = mhz * 1000000u;

	if (hz == wanted_cpu_hz)
		return;
	wanted_cpu_hz = hz;
	if (!clocks_set || R_FAILED(clkrstInitialize()))
		return;
	set_clock(PcvModuleId_CpuBus, hz ? hz : original_cpu_hz);
	host_logf(HOST_LOG_INFO, "clocks: cpu %u MHz (performance.cpu_clock)", get_clock(PcvModuleId_CpuBus) / 1000000);
	clkrstExit();
}

void host_restore_clocks(void)
{
	if (R_FAILED(clkrstInitialize()))
		return;
	set_clock(PcvModuleId_CpuBus, original_cpu_hz);
	set_clock(PcvModuleId_EMC, original_memory_hz);
	set_clock(PcvModuleId_GPU, original_gpu_hz);
	clkrstExit();
}

static void log_clocks(void)
{
	static const struct
	{
		const char *name;
		PcvModuleId module;
	} clocks[] = {
		{ "cpu", PcvModuleId_CpuBus },
		{ "gpu", PcvModuleId_GPU },
		{ "memory", PcvModuleId_EMC },
	};
	unsigned index;

	if (R_FAILED(clkrstInitialize()))
	{
		host_logf(HOST_LOG_INFO, "clocks: clkrst is not available");
		return;
	}
	original_cpu_hz = get_clock(PcvModuleId_CpuBus);
	original_memory_hz = get_clock(PcvModuleId_EMC);
	original_gpu_hz = get_clock(PcvModuleId_GPU);
	host_logf(HOST_LOG_INFO, "clocks: found cpu %u MHz, memory %u MHz, gpu %u MHz",
		original_cpu_hz / 1000000, original_memory_hz / 1000000, original_gpu_hz / 1000000);
	if (original_cpu_hz && wanted_cpu_hz)
		set_clock(PcvModuleId_CpuBus, wanted_cpu_hz);
	clocks_set = 1;
	if (original_memory_hz && original_memory_hz < GAME_MEMORY_HZ)
		set_clock(PcvModuleId_EMC, GAME_MEMORY_HZ);
	{
		u32 gpu_hz = appletGetOperationMode() == AppletOperationMode_Console ? GAME_GPU_DOCKED_HZ :
			GAME_GPU_HANDHELD_HZ;

		if (original_gpu_hz && original_gpu_hz < gpu_hz)
			set_clock(PcvModuleId_GPU, gpu_hz);
	}
	for (index = 0; index < sizeof(clocks) / sizeof(clocks[0]); index++)
	{
		ClkrstSession session;
		u32 hz = 0;

		if (R_SUCCEEDED(clkrstOpenSession(&session, clocks[index].module, 3)))
		{
			clkrstGetClockRate(&session, &hz);
			clkrstCloseSession(&session);
		}
		host_logf(HOST_LOG_INFO, "clocks: %s %u MHz", clocks[index].name, hz / 1000000);
	}
	clkrstExit();
}

/* time spent inside SDL_GL_SwapWindow since the last report: waiting for
the GPU and for vsync, as against the frame's own work */
static uint64_t swap_ticks;

/* The frame rate and the longest frame in the log every 5 seconds, or every
HALO_FPS_LOG seconds (0 turns it off) */
/* The ticks the calling thread has run for, on any core; 0 if the kernel
will not say. The game thread's share of the time is what tells a game bound
by its own work (near all of it) from one waiting on something else - the
GPU, through the driver's calls - which a short swap does not rule out. */
static u64 thread_ticks(void)
{
	u64 ticks = 0;

	if (R_FAILED(svcGetInfo(&ticks, InfoType_ThreadTickCount, CUR_THREAD_HANDLE, TickCountInfo_Total)) &&
		R_FAILED(svcGetInfo(&ticks, InfoType_ThreadTickCountDeprecated, CUR_THREAD_HANDLE, TickCountInfo_Total)))
		return 0;
	return ticks;
}

/* the slow frames said in a window of frame_statistics, the rest counted */
#define SLOW_FRAMES_SAID 20

static void frame_statistics(void)
{
	/* the game thread's own run time, and the clock's, at the window's start */
	static u64 busy_start, wall_start;
	static double interval = -1.0;
	static uint64_t start, previous;
	static uint64_t longest;
	static uint32_t frames, frame_buckets[6];
	uint64_t now;

	if (interval < 0.0)
	{
		const char *setting = getenv("HALO_FPS_LOG");

		interval = setting && *setting ? atof(setting) : 5.0;
		log_clocks();
	}
	if (interval <= 0.0)
		return;
	now = SDL_GetPerformanceCounter();
	if (!start)
	{
		start = previous = now;
		busy_start = thread_ticks();
		wall_start = armGetSystemTick();
		return;
	}
	frames++;
	if (now - previous > longest)
		longest = now - previous;
	/* each frame over 20 ms said, with what each thread waited for in it
	(host_dk_frame_report), at most SLOW_FRAMES_SAID a window and the rest
	counted: where a dropped frame went */
	{
		static u64 frame_busy_start;
		static uint32_t said, unsaid;
		struct host_dk_frame_report report;
		u64 busy = thread_ticks();
		double milliseconds = (double)(now - previous) * 1000.0 / (double)SDL_GetPerformanceFrequency();

		host_dk_frame_report(&report);
		host_debug_profile_spike((uint64_t)(milliseconds * 1000000.0));
		if (milliseconds > 20.0)
		{
			if (said < SLOW_FRAMES_SAID)
			{
				said++;
				host_logf(HOST_LOG_INFO, "slow frame: %.1f ms; the game thread ran %.1f ms and waited %.1f ms for room "
					"in the render queue (%u streams); the render thread presented %u, waited %.1f ms at most for a "
					"screen image and %.1f ms for a frame's slot; the GPU %.1f ms on its last frame (%.1f on average)",
					milliseconds,
					busy && frame_busy_start ? (double)(busy - frame_busy_start) * 1000.0 / (double)armGetSystemTickFreq() :
						0.0,
					report.submit_wait_us / 1000.0, (unsigned)report.streams, (unsigned)report.presents,
					report.acquire_max_us / 1000.0, report.fence_max_us / 1000.0, report.gpu_last_us / 1000.0,
					report.gpu_average_us / 1000.0);
			}
			else
				unsaid++;
		}
		frame_busy_start = busy;
		if (now - start >= (uint64_t)(interval * (double)SDL_GetPerformanceFrequency()))
		{
			if (unsaid)
				host_logf(HOST_LOG_INFO, "slow frame: %u more in these seconds not said", (unsigned)unsaid);
			said = unsaid = 0;
		}
	}
	{
		/* how the frames' times spread: vsync puts them on multiples of
		16.7 ms, a slower thread or the GPU anywhere */
		double milliseconds = (double)(now - previous) * 1000.0 / (double)SDL_GetPerformanceFrequency();
		int bucket = milliseconds < 15.0 ? 0 : milliseconds < 18.0 ? 1 : milliseconds < 22.0 ? 2 :
			milliseconds < 28.0 ? 3 : milliseconds < 35.0 ? 4 : 5;

		frame_buckets[bucket]++;
	}
	previous = now;
	if (now - start >= (uint64_t)(interval * (double)SDL_GetPerformanceFrequency()))
	{
		double frequency = (double)SDL_GetPerformanceFrequency();

		double elapsed_ms = (double)(now - start) * 1000.0 / frequency;
		double swap_ms = (double)swap_ticks * 1000.0 / frequency;

		host_logf(HOST_LOG_INFO, "fps %.1f, longest frame %.1f ms, %.1f ms a frame in the swap (%.0f%% of the time)",
			frames * frequency / (double)(now - start), longest * 1000.0 / frequency,
			frames ? swap_ms / frames : 0.0, elapsed_ms > 0.0 ? 100.0 * swap_ms / elapsed_ms : 0.0);
		swap_ticks = 0;
		host_logf(HOST_LOG_INFO, "frame times: %u under 15 ms, %u 15-18, %u 18-22, %u 22-28, %u 28-35, %u over 35",
			frame_buckets[0], frame_buckets[1], frame_buckets[2], frame_buckets[3], frame_buckets[4], frame_buckets[5]);
		memset(frame_buckets, 0, sizeof(frame_buckets));
		/* (and every sixth time, 30 s, the memory: host_memory_report) */
		{
			static unsigned reports;

			if (++reports % 6 == 0)
			{
				host_memory_report("in play");
				host_texture_cache_report();
			}
		}
		{
			u64 busy = thread_ticks();
			u64 wall = armGetSystemTick();

			if (busy && wall > wall_start)
				host_logf(HOST_LOG_INFO, "game thread: running %.0f%% of the time, %.1f ms a frame",
					100.0 * (double)(busy - busy_start) / (double)(wall - wall_start),
					frames ? (double)(busy - busy_start) * 1000.0 / (double)armGetSystemTickFreq() / frames : 0.0);
			busy_start = busy;
			wall_start = wall;
		}
		start = now;
		frames = 0;
		longest = 0;
	}
}

int host_sdl_gl_swap_window(uint32_t window)
{
	SDL_Window *object = handle_get(window, _handle_window);

	/* (the datagrams the game held this frame go: host_net.c) */
	host_net_frame_end();
	if (!object)
		return 0;
	if ((void *)object == (void *)&standin_window)
	{
		/* until deko3d presents (and its swapchain waits for the display),
		frames are held to the display's 60 Hz here, as the swap's vsync
		holds them over Mesa */
		static uint64_t next;
		uint64_t now = armTicksToNs(armGetSystemTick()), interval = 1000000000ull / 60;

		if (swap_interval > 0 && !host_dk_presenting && next > now)
		{
			svcSleepThread((int64_t)(next - now));
			now = next;
		}
		next = now - next > interval ? now + interval : next + interval;
		/* (the frame handed over: the game goes on once the one before it is
		presented, host_dk_pace) */
		if (host_dk_presenting)
			host_dk_pace();
		frame_statistics();
		return 1;
	}
	{
		uint64_t before = SDL_GetPerformanceCounter();

		SDL_GL_SwapWindow(object);
		swap_ticks += SDL_GetPerformanceCounter() - before;
	}
	frame_statistics();
	return 1;
}

/* ---------- events */

static int translate(const SDL_Event *event, struct host_event *result)
{
	memset(result, 0, sizeof(*result));
	result->timestamp_ns = (uint64_t)event->common.timestamp * 1000000ull;
	switch (event->type)
	{
	case SDL_QUIT:
		result->kind = _host_event_quit;
		return 1;
	case SDL_KEYDOWN:
	case SDL_KEYUP:
		result->kind = _host_event_key;
		result->scancode = event->key.keysym.scancode;
		result->keycode = event->key.keysym.sym;
		result->modifiers = event->key.keysym.mod;
		result->down = event->type == SDL_KEYDOWN;
		result->repeat = event->key.repeat != 0;
		return 1;
	case SDL_CONTROLLERDEVICEADDED:
		/* SDL2 names the new device by its index, SDL3 by its instance */
		result->kind = _host_event_gamepad_added;
		result->which = (uint32_t)SDL_JoystickGetDeviceInstanceID(event->cdevice.which);
		return 1;
	case SDL_CONTROLLERDEVICEREMOVED:
		result->kind = _host_event_gamepad_removed;
		result->which = (uint32_t)event->cdevice.which;
		return 1;
	/* (the buttons are read from the gamepad's state, not its events; there is no button combination that quits:
	Start and Back together once closed the game at once, mid-match) */
	case SDL_CONTROLLERBUTTONDOWN:
	case SDL_CONTROLLERBUTTONUP:
		return 0;
	case SDL_WINDOWEVENT:
		if (event->window.event == SDL_WINDOWEVENT_FOCUS_GAINED)
		{
			result->kind = _host_event_focus_gained;
			return 1;
		}
		if (event->window.event == SDL_WINDOWEVENT_FOCUS_LOST)
		{
			result->kind = _host_event_focus_lost;
			return 1;
		}
		return 0;
	default:
		return 0;
	}
}

int host_sdl_poll_event(void *event)
{
	SDL_Event host_event;

	while (SDL_PollEvent(&host_event))
	{
		struct host_event translated;

		if (translate(&host_event, &translated))
		{
			host_event_to_sdl3(&translated, event);
			return 1;
		}
	}
	return 0;
}

/* ---------- gamepads */

static int device_index_of(SDL_JoystickID id)
{
	int count = SDL_NumJoysticks(), index;

	for (index = 0; index < count; index++)
	{
		if (SDL_JoystickGetDeviceInstanceID(index) == id)
			return index;
	}
	return -1;
}

int host_sdl_get_gamepads(uint32_t *ids, int capacity)
{
	int count = SDL_NumJoysticks(), index, found = 0;

	for (index = 0; index < count && found < capacity; index++)
	{
		if (SDL_IsGameController(index))
			ids[found++] = (uint32_t)SDL_JoystickGetDeviceInstanceID(index);
	}
	return found;
}

uint32_t host_sdl_open_gamepad(uint32_t id)
{
	int index = device_index_of((SDL_JoystickID)id);
	SDL_GameController *gamepad;

	if (index < 0)
		return 0;
	gamepad = SDL_GameControllerFromInstanceID((SDL_JoystickID)id);
	if (!gamepad)
	{
		gamepad = SDL_GameControllerOpen(index);
		if (gamepad)
			host_logf(HOST_LOG_INFO, "gamepad %u: %s (%04x:%04x)", (unsigned)id, SDL_GameControllerName(gamepad),
				SDL_GameControllerGetVendor(gamepad), SDL_GameControllerGetProduct(gamepad));
		else
			host_logf(HOST_LOG_WARN, "cannot open gamepad %u: %s", (unsigned)id, SDL_GetError());
	}
	return handle_new(_handle_gamepad, gamepad);
}

uint32_t host_sdl_gamepad_from_id(uint32_t id)
{
	SDL_GameController *gamepad = SDL_GameControllerFromInstanceID((SDL_JoystickID)id);

	/* SDL3 opens nothing here, but a gamepad SDL2 listed and nobody has
	opened yet (the handheld's own, present at start-up) is opened now */
	if (!gamepad)
		return host_sdl_open_gamepad(id);
	return handle_new(_handle_gamepad, gamepad);
}

/* (the event thread, when the controller goes: sdl_platform.c) */
void host_sdl_close_gamepad(uint32_t gamepad)
{
	SDL_GameController *object = handle_get(gamepad, _handle_gamepad);

	if (!object)
		return;
	pthread_mutex_lock(&handle_lock);
	handles[gamepad].type = _handle_free;
	handles[gamepad].object = NULL;
	pthread_mutex_unlock(&handle_lock);
	SDL_GameControllerClose(object);
}

int host_sdl_gamepad_axis(uint32_t gamepad, int axis)
{
	SDL_GameController *object = handle_get(gamepad, _handle_gamepad);

	return object ? SDL_GameControllerGetAxis(object, (SDL_GameControllerAxis)axis) : 0;
}

int host_sdl_gamepad_button(uint32_t gamepad, int button)
{
	SDL_GameController *object = handle_get(gamepad, _handle_gamepad);

	return object ? SDL_GameControllerGetButton(object, (SDL_GameControllerButton)button) : 0;
}

int host_sdl_gamepad_type(uint32_t gamepad)
{
	SDL_GameController *object = handle_get(gamepad, _handle_gamepad);
	int kind;

	if (!object)
		return 0; /* SDL_GAMEPAD_TYPE_UNKNOWN */
	switch (SDL_GameControllerGetType(object))
	{
	case SDL_CONTROLLER_TYPE_XBOXONE: kind = _host_gamepad_xboxone; break;
	case SDL_CONTROLLER_TYPE_PS3: kind = _host_gamepad_ps3; break;
	case SDL_CONTROLLER_TYPE_PS4: kind = _host_gamepad_ps4; break;
	case SDL_CONTROLLER_TYPE_PS5: kind = _host_gamepad_ps5; break;
	case SDL_CONTROLLER_TYPE_NINTENDO_SWITCH_PRO: kind = _host_gamepad_switch_pro; break;
	default: kind = _host_gamepad_xbox360; break;
	}
	return host_sdl3_gamepad_type(kind);
}

/* ---------- rumble, through libnx's HD rumble and not SDL's

devkitPro's SDL2 (switch-sdl-2.28's SDL_sysjoystick.c) readies the
vibration devices of its pads 1 to 7 as it starts, but not of pad 0, the
first player's: that one is readied only if its controller's kind changes
while the program runs, so whether the first player's controller rumbles
at all depended on when it connected. Even then it is No1's, never the
handheld's (the Joy-Cons on the console), and it sends amplitudes of 320
where HD rumble takes 0 to 1. So the host rumbles the controller itself:
SDL's pad i is npad No1 + i (its pad 0 also reads the handheld), whose
devices are readied for the style it has, again when that changes. The
low (heavy) motor is the left device's, the high one the right's; a
single Joy-Con has one device, which takes the stronger. The game sends
the motors every frame, and 0 when they stop.

The game sends them holding its input lock, which its game thread waits
for, so the request only notes the motors wanted, and a thread of its own
makes the calls to HID (one that is slow is logged). */

#define RUMBLE_PADS 8
#define RUMBLE_LOW_HZ 160.0f
#define RUMBLE_HIGH_HZ 320.0f

/* SDL pad i is npad No1+i. Pad 0 also reads the handheld, which is how the
console's own Joy-Cons are seen when they are attached. 1 if it has a style. */
static int pad_npad(int pad, HidNpadIdType *id, u32 *style)
{
	*id = (HidNpadIdType)(HidNpadIdType_No1 + pad);
	*style = hidGetNpadStyleSet(*id);
	if (pad == 0 && !*style && hidGetNpadStyleSet(HidNpadIdType_Handheld))
	{
		*id = HidNpadIdType_Handheld;
		*style = hidGetNpadStyleSet(*id);
	}
	return *style != 0;
}

static struct
{
	HidNpadIdType id;
	u32 style;
	int count;
	HidVibrationDeviceHandle handles[2];
	/* the motors sent (the thread's alone, as are the devices) */
	uint32_t low, high;
	/* those wanted, and whether that is news (under rumble_lock) */
	uint32_t wanted_low, wanted_high;
	int wanted, requested;
} rumble_pads[RUMBLE_PADS];
static pthread_mutex_t rumble_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t rumble_condition = PTHREAD_COND_INITIALIZER;
static int rumble_thread_started;

/* the pad's devices for the style its controller has now (count 0: none) */
static void rumble_ready(int pad)
{
	HidNpadIdType id;
	u32 style;
	HidNpadStyleTag tag;
	int count = 2;
	Result result;

	pad_npad(pad, &id, &style);
	if (rumble_pads[pad].id == id && rumble_pads[pad].style == style && (rumble_pads[pad].count || !style))
		return;
	rumble_pads[pad].id = id;
	rumble_pads[pad].style = style;
	rumble_pads[pad].count = 0;
	/* (sent again on the next request) */
	rumble_pads[pad].low = rumble_pads[pad].high = (uint32_t)-1;
	if (style & HidNpadStyleTag_NpadHandheld)
		tag = HidNpadStyleTag_NpadHandheld;
	else if (style & HidNpadStyleTag_NpadFullKey)
		tag = HidNpadStyleTag_NpadFullKey;
	else if (style & HidNpadStyleTag_NpadJoyDual)
		tag = HidNpadStyleTag_NpadJoyDual;
	else if (style & HidNpadStyleTag_NpadJoyLeft)
		tag = HidNpadStyleTag_NpadJoyLeft, count = 1;
	else if (style & HidNpadStyleTag_NpadJoyRight)
		tag = HidNpadStyleTag_NpadJoyRight, count = 1;
	else
		return;
	result = hidInitializeVibrationDevices(rumble_pads[pad].handles, count, id, tag);
	if (R_FAILED(result))
	{
		host_logf(HOST_LOG_WARN, "rumble: pad %d's vibration devices (style %#x): 0x%x", pad, (unsigned)style,
			(unsigned)result);
		return;
	}
	rumble_pads[pad].count = count;
	host_logf(HOST_LOG_INFO, "rumble: pad %d is npad %d, style %#x, %d device(s)", pad, (int)id, (unsigned)style, count);
}

static void rumble_value(HidVibrationValue *value, uint32_t speed)
{
	float amplitude = (float)speed / 65535.0f;

	value->amp_low = amplitude;
	value->freq_low = RUMBLE_LOW_HZ;
	value->amp_high = amplitude;
	value->freq_high = RUMBLE_HIGH_HZ;
}

/* (the thread's, rumble_lock not held) the pad's motors, sent if they changed */
static void rumble_send(int pad, uint32_t low, uint32_t high)
{
	HidVibrationValue values[2];
	uint64_t start, took;
	Result result;

	rumble_ready(pad);
	if (!rumble_pads[pad].count || (rumble_pads[pad].low == low && rumble_pads[pad].high == high))
		return;
	if (rumble_pads[pad].count == 1)
		rumble_value(&values[0], low > high ? low : high);
	else
	{
		rumble_value(&values[0], low);
		rumble_value(&values[1], high);
	}
	start = armTicksToNs(armGetSystemTick());
	result = hidSendVibrationValues(rumble_pads[pad].handles, values, rumble_pads[pad].count);
	took = armTicksToNs(armGetSystemTick()) - start;
	if (took > 5000000ULL)
		host_logf(HOST_LOG_WARN, "rumble: pad %d's motors took %llu ms to send", pad, (unsigned long long)(took / 1000000ULL));
	if (R_SUCCEEDED(result))
	{
		rumble_pads[pad].low = low;
		rumble_pads[pad].high = high;
	}
}

static void *rumble_thread(void *unused)
{
	(void)unused;
	host_thread_place_on_helper_core();
	pthread_mutex_lock(&rumble_lock);
	for (;;)
	{
		int pad, any = 0;

		for (pad = 0; pad < RUMBLE_PADS; pad++)
		{
			uint32_t low, high;

			if (!rumble_pads[pad].wanted)
				continue;
			rumble_pads[pad].wanted = 0;
			low = rumble_pads[pad].wanted_low;
			high = rumble_pads[pad].wanted_high;
			any = 1;
			pthread_mutex_unlock(&rumble_lock);
			rumble_send(pad, low, high);
			pthread_mutex_lock(&rumble_lock);
		}
		if (!any)
			pthread_cond_wait(&rumble_condition, &rumble_lock);
	}
	return NULL;
}

int host_sdl_rumble_gamepad(uint32_t gamepad, uint32_t low, uint32_t high, uint32_t milliseconds)
{
	SDL_GameController *object = handle_get(gamepad, _handle_gamepad);
	SDL_JoystickID pad;

	(void)milliseconds;
	if (!object)
		return 0;
	pad = SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(object));
	if (pad < 0 || pad >= RUMBLE_PADS)
		return 0;
	pthread_mutex_lock(&rumble_lock);
	if (!rumble_thread_started)
	{
		pthread_t thread;

		rumble_thread_started = 1;
		if (pthread_create(&thread, NULL, rumble_thread, NULL) == 0)
			pthread_detach(thread);
		else
			host_logf(HOST_LOG_ERROR, "rumble: its thread could not be made");
	}
	/* (every request the first time, and after that each change; the
	thread also readies a pad whose controller changed as it sends) */
	low &= 0xffff;
	high &= 0xffff;
	if (!rumble_pads[pad].requested || rumble_pads[pad].wanted_low != low || rumble_pads[pad].wanted_high != high)
	{
		rumble_pads[pad].requested = 1;
		rumble_pads[pad].wanted_low = low;
		rumble_pads[pad].wanted_high = high;
		rumble_pads[pad].wanted = 1;
		pthread_cond_signal(&rumble_condition);
	}
	pthread_mutex_unlock(&rumble_lock);
	return 1;
}

/* ---------- gyroscope aiming

The same pad map as rumble. The guest asks for the turn since its last ask,
in radians, and applies input.gyro_aim itself. A reading quieter than the
sensor's noise at rest is dropped, and so is a gap long enough that one
sample would spin the view (the sensor was not being read). Joy-Con Dual is
the only style that has two sensors; that pair's right one is read. A Pro
Controller, the handheld and a single Joy-Con have one.

The sensor's axes, buttons toward the player: +x to its right, +y up, +z
toward the player. Turning right is a negative rotation about +y, and tipping
the top away (looking up) is a negative rotation about +x. A single Joy-Con
is held on its side with SL and SR up, so those axes are turned to match. */

#define GYRO_NOISE 0.01f
/* the turn handed to the guest for the controller's: half again the sensor's,
which turned too little for aiming at every input.gyro_sensitivity_* */
#define GYRO_SCALE 1.5f
/* a sample further than this from the one before it is not a turn */
#define GYRO_LONGEST_GAP_NS 100000000ULL
#define GYRO_SAMPLE_COUNT 16

static struct
{
	HidNpadIdType id;
	u32 style;
	int started;
	int sensor;
	HidSixAxisSensorHandle handles[2];
	u64 last_sample;
	int had_sample;
} gyro_pads[RUMBLE_PADS];

/* the style rumble would pick, and how many sensors it has (2 only for a pair) */
static HidNpadStyleTag gyro_style_tag(u32 style, int *sensors)
{
	*sensors = 1;
	if (style & HidNpadStyleTag_NpadHandheld)
		return HidNpadStyleTag_NpadHandheld;
	if (style & HidNpadStyleTag_NpadFullKey)
		return HidNpadStyleTag_NpadFullKey;
	if (style & HidNpadStyleTag_NpadJoyDual)
	{
		*sensors = 2;
		return HidNpadStyleTag_NpadJoyDual;
	}
	if (style & HidNpadStyleTag_NpadJoyLeft)
		return HidNpadStyleTag_NpadJoyLeft;
	if (style & HidNpadStyleTag_NpadJoyRight)
		return HidNpadStyleTag_NpadJoyRight;
	return 0;
}

/* adds this sample's turn. Positive yaw is to the player's right, positive
pitch looks up. */
static void gyro_add_sample(u32 style, const HidSixAxisSensorState *state, float *yaw, float *pitch)
{
	float x = state->angular_velocity.x;
	float y = state->angular_velocity.y;
	float seconds;
	HidNpadStyleTag tag;
	int sensors;

	if (!(state->attributes & HidSixAxisSensorAttribute_IsConnected))
		return;
	if (state->delta_time == 0 || state->delta_time > GYRO_LONGEST_GAP_NS)
		return;
	if (x < GYRO_NOISE && x > -GYRO_NOISE)
		x = 0.0f;
	if (y < GYRO_NOISE && y > -GYRO_NOISE)
		y = 0.0f;
	/* delta_time is nanoseconds */
	seconds = (float)state->delta_time * 1.0e-9f;
	tag = gyro_style_tag(style, &sensors);
	(void)sensors;
	if (tag == HidNpadStyleTag_NpadJoyRight)
	{
		*yaw += -x * seconds;
		*pitch += y * seconds;
	}
	else if (tag == HidNpadStyleTag_NpadJoyLeft)
	{
		*yaw += x * seconds;
		*pitch += -y * seconds;
	}
	else
	{
		*yaw += -y * seconds;
		*pitch += -x * seconds;
	}
}

static void gyro_ready(int pad)
{
	HidNpadIdType id;
	u32 style;
	HidNpadStyleTag tag;
	int sensors = 1, sensor;
	Result result;

	pad_npad(pad, &id, &style);
	if (gyro_pads[pad].id == id && gyro_pads[pad].style == style && (gyro_pads[pad].started || !style))
		return;
	if (gyro_pads[pad].started)
		hidStopSixAxisSensor(gyro_pads[pad].handles[gyro_pads[pad].sensor]);
	gyro_pads[pad].id = id;
	gyro_pads[pad].style = style;
	gyro_pads[pad].started = 0;
	gyro_pads[pad].had_sample = 0;
	tag = gyro_style_tag(style, &sensors);
	if (!tag)
		return;
	result = hidGetSixAxisSensorHandles(gyro_pads[pad].handles, sensors, id, tag);
	if (R_FAILED(result))
	{
		host_logf(HOST_LOG_WARN, "gyro: pad %d's sensors (style %#x): 0x%x", pad, (unsigned)style,
			(unsigned)result);
		return;
	}
	/* a pair's right Joy-Con; the only sensor otherwise */
	sensor = sensors == 2 ? 1 : 0;
	result = hidStartSixAxisSensor(gyro_pads[pad].handles[sensor]);
	if (R_FAILED(result))
	{
		host_logf(HOST_LOG_WARN, "gyro: pad %d's sensor did not start: 0x%x", pad, (unsigned)result);
		return;
	}
	gyro_pads[pad].sensor = sensor;
	gyro_pads[pad].started = 1;
	host_logf(HOST_LOG_INFO, "gyro: pad %d is npad %d, style %#x, sensor %d", pad, (int)id, (unsigned)style, sensor);
}

/* the turn since the last call, in radians. The first call only notes where
the sensor is, so enabling aiming does not apply the samples already queued. */
static void gyro_read(int pad, float *yaw, float *pitch)
{
	HidSixAxisSensorState states[GYRO_SAMPLE_COUNT];
	size_t count, index;
	u64 previous;

	*yaw = 0.0f;
	*pitch = 0.0f;
	gyro_ready(pad);
	if (!gyro_pads[pad].started)
		return;
	count = hidGetSixAxisSensorStates(gyro_pads[pad].handles[gyro_pads[pad].sensor], states, GYRO_SAMPLE_COUNT);
	if (!count)
		return;
	/* states[0] is the latest. The first read keeps it and turns nothing. */
	if (!gyro_pads[pad].had_sample)
	{
		gyro_pads[pad].last_sample = states[0].sampling_number;
		gyro_pads[pad].had_sample = 1;
		return;
	}
	previous = gyro_pads[pad].last_sample;
	for (index = count; index > 0; index--)
	{
		const HidSixAxisSensorState *state = &states[index - 1];

		if (state->sampling_number <= previous)
			continue;
		gyro_add_sample(gyro_pads[pad].style, state, yaw, pitch);
		previous = state->sampling_number;
	}
	gyro_pads[pad].last_sample = previous;
}

void host_sdl_gamepad_gyro(uint32_t gamepad, float *yaw, float *pitch)
{
	SDL_GameController *object;
	SDL_JoystickID pad;

	if (!yaw || !pitch)
		return;
	*yaw = 0.0f;
	*pitch = 0.0f;
	object = handle_get(gamepad, _handle_gamepad);
	if (!object)
		return;
	pad = SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(object));
	if (pad < 0 || pad >= RUMBLE_PADS)
		return;
	gyro_read((int)pad, yaw, pitch);
	*yaw *= GYRO_SCALE;
	*pitch *= GYRO_SCALE;
}

/* ---------- audio

SDL2 pulls audio with a callback on its own thread; the guest's stream
callback (SDL3's model) pushes it. Each SDL2 request asks the guest, on a
thread with a guest stack (audio_thread), for what the stream lacks; what
the guest puts into the stream collects in the binding's buffer, and the
SDL2 callback copies it out. */
struct audio_binding
{
	uint32_t handle;
	uint32_t callback;
	uint32_t userdata;
	SDL_AudioDeviceID device;
	pthread_mutex_t lock;
	pthread_cond_t requested;
	pthread_cond_t done;
	int pending;
	int additional;
	unsigned char *buffer;
	int buffer_length;
	int buffer_size;
};

static void *audio_thread(void *context)
{
	struct audio_binding *binding = context;

	pthread_mutex_lock(&binding->lock);
	for (;;)
	{
		int additional;

		while (!binding->pending)
			pthread_cond_wait(&binding->requested, &binding->lock);
		additional = binding->additional;
		pthread_mutex_unlock(&binding->lock);
		host_call_guest(binding->callback, binding->userdata, binding->handle, (uint32_t)additional,
			(uint32_t)additional);
		pthread_mutex_lock(&binding->lock);
		binding->pending = 0;
		pthread_cond_signal(&binding->done);
	}
	return NULL;
}

/* appends to the binding's buffer; the caller holds its lock */
static int audio_keep(struct audio_binding *binding, const void *data, int length)
{
	if (length < 0)
		return 0;
	if (binding->buffer_length + length > binding->buffer_size)
	{
		int size = (binding->buffer_length + length) * 2;
		unsigned char *buffer = SDL_realloc(binding->buffer, (size_t)size);

		if (!buffer)
			return 0;
		binding->buffer = buffer;
		binding->buffer_size = size;
	}
	memcpy(binding->buffer + binding->buffer_length, data, (size_t)length);
	binding->buffer_length += length;
	return 1;
}

static void SDLCALL audio_callback(void *userdata, Uint8 *stream, int length)
{
	struct audio_binding *binding = userdata;
	int copied;

	pthread_mutex_lock(&binding->lock);
	if (binding->callback && binding->buffer_length < length)
	{
		binding->additional = length - binding->buffer_length;
		binding->pending = 1;
		pthread_cond_signal(&binding->requested);
		while (binding->pending)
			pthread_cond_wait(&binding->done, &binding->lock);
	}
	copied = binding->buffer_length < length ? binding->buffer_length : length;
	memcpy(stream, binding->buffer, (size_t)copied);
	if (copied < length)
		memset(stream + copied, 0, (size_t)(length - copied));
	binding->buffer_length -= copied;
	if (binding->buffer_length)
		memmove(binding->buffer, binding->buffer + copied, (size_t)binding->buffer_length);
	pthread_mutex_unlock(&binding->lock);
}

/* SDL3's SDL_AudioSpec */
struct sdl3_audio_spec
{
	int32_t format;
	int32_t channels;
	int32_t freq;
};

uint32_t host_sdl_open_audio_stream(uint32_t device, const void *spec, uint32_t callback, uint32_t userdata)
{
	const struct sdl3_audio_spec *guest_spec = spec;
	struct audio_binding *binding = SDL_calloc(1, sizeof(*binding));
	SDL_AudioSpec wanted, obtained;
	const char *setting = getenv("HALO_AUDIO_SAMPLES");

	(void)device;
	/* one without a callback is voice chat's microphone, which the guest
	reads (port/linux/src/voice_audio.c: 48 kHz mono floats); the Switch's
	SDL2 cannot record, so libnx's audin takes it */
	if (!callback)
	{
		uint32_t handle;

		SDL_free(binding);
		if (guest_spec->format != (int32_t)AUDIO_F32SYS || guest_spec->channels != 1 || guest_spec->freq != 48000)
		{
			host_logf(HOST_LOG_ERROR, "microphone: asked for format 0x%x, %d channel(s) at %d Hz; it gives "
				"48 kHz mono floats", (unsigned)guest_spec->format, (int)guest_spec->channels, (int)guest_spec->freq);
			return 0;
		}
		if (!host_microphone_open())
			return 0;
		/* (any non-null object: there is one microphone) */
		handle = handle_new(_handle_microphone, (void *)1);
		if (!handle)
			host_microphone_close();
		return handle;
	}
	binding->callback = callback;
	binding->userdata = userdata;
	pthread_mutex_init(&binding->lock, NULL);
	pthread_cond_init(&binding->requested, NULL);
	pthread_cond_init(&binding->done, NULL);
	SDL_zero(wanted);
	/* SDL3's audio format values are SDL2's */
	wanted.format = (SDL_AudioFormat)guest_spec->format;
	wanted.channels = (Uint8)guest_spec->channels;
	wanted.freq = guest_spec->freq;
	wanted.samples = setting && *setting ? (Uint16)atoi(setting) : 1024;
	wanted.callback = audio_callback;
	wanted.userdata = binding;
	/* SDL converts to what the device takes */
	binding->device = SDL_OpenAudioDevice(NULL, 0, &wanted, &obtained, 0);
	if (!binding->device)
	{
		host_logf(HOST_LOG_ERROR, "SDL_OpenAudioDevice: %s", SDL_GetError());
		SDL_free(binding);
		return 0;
	}
	host_logf(HOST_LOG_INFO, "audio %d Hz, %d channels, %d frames a callback", obtained.freq, obtained.channels,
		obtained.samples);
	/* the device starts paused, so no callback can run before this */
	binding->handle = handle_new(_handle_audio, binding);
	if (!binding->handle)
	{
		SDL_CloseAudioDevice(binding->device);
		SDL_free(binding);
		return 0;
	}
	if (callback && host_native_thread_create(audio_thread, binding, 256 * 1024) != 0)
		host_fatal("cannot start the audio thread");
	return binding->handle;
}

int host_sdl_put_audio_stream_data(uint32_t stream, const void *data, int length)
{
	struct audio_binding *binding = handle_get(stream, _handle_audio);
	int result;

	if (!binding)
		return 0;
	/* the guest's callback runs while the SDL2 callback waits for it
	without holding the lock */
	pthread_mutex_lock(&binding->lock);
	result = audio_keep(binding, data, length);
	pthread_mutex_unlock(&binding->lock);
	return result;
}

int host_sdl_resume_audio_stream_device(uint32_t stream)
{
	struct audio_binding *binding = handle_get(stream, _handle_audio);

	/* (the microphone captures from its opening) */
	if (handle_get(stream, _handle_microphone))
		return 1;
	if (!binding)
		return 0;
	SDL_PauseAudioDevice(binding->device, 0);
	return 1;
}

/* voice chat's microphone (port/linux/src/voice_audio.c), the only stream
the guest reads or closes */
int host_sdl_get_audio_stream_data(uint32_t stream, void *data, int length)
{
	if (!handle_get(stream, _handle_microphone))
		return -1;
	return host_microphone_read(data, length);
}

int host_sdl_get_audio_stream_available(uint32_t stream)
{
	if (!handle_get(stream, _handle_microphone))
		return -1;
	return host_microphone_available();
}

void host_sdl_destroy_audio_stream(uint32_t stream)
{
	if (!handle_get(stream, _handle_microphone))
		return;
	pthread_mutex_lock(&handle_lock);
	handles[stream].type = _handle_free;
	handles[stream].object = NULL;
	pthread_mutex_unlock(&handle_lock);
	host_microphone_close();
}

/* ---------- the clipboard (internet play's invite links): the process's own */

static char clipboard[1024];

int host_sdl_set_clipboard_text(const char *text)
{
	SDL_strlcpy(clipboard, text, sizeof(clipboard));
	host_logf(HOST_LOG_INFO, "clipboard: %s", clipboard);
	return 1;
}

void host_sdl_get_clipboard_text(char *buffer, uint32_t size)
{
	SDL_strlcpy(buffer, clipboard, size);
}

/* The guest asks a key by name to bind it and by name again to show it in a
control prompt, and the names SDL knows are the host's (port/android/host/
host_imports.list declares these for every host, so each provides them). */
void host_sdl_scancode_name(int32_t scancode, char *buffer, uint32_t size)
{
	SDL_strlcpy(buffer, SDL_GetScancodeName((SDL_Scancode)scancode), size);
}

int32_t host_sdl_scancode_from_name(const char *name)
{
	return (int32_t)SDL_GetScancodeFromName(name);
}

int host_sdl_show_toast(const char *message, int duration, int gravity, int x, int y)
{
	(void)duration;
	(void)gravity;
	(void)x;
	(void)y;
	host_logf(HOST_LOG_INFO, "notice: %s", message);
	return 1;
}

/* ---------- a message for the player: the log (the framebuffer has no
message boxes) */

int host_sdl_show_simple_message_box(uint32_t flags, const char *title, const char *message)
{
	(void)flags;
	host_logf(HOST_LOG_WARN, "%s: %s", title, message);
	return 1;
}

/* a question (the mods at startup: port_mods.c): the log, and no answer,
which the guest takes as not able to ask */
int host_sdl_show_message_box(struct host_message_box *box)
{
	host_logf(HOST_LOG_WARN, "%s: %s", (const char *)(uintptr_t)box->title, (const char *)(uintptr_t)box->message);
	return 0;
}

/* ---------- Android's on-screen touch controls (port/android/host/host_touch.c)

The guest's touch input asks for them through the import list the Switch
shares; the Switch has no overlay and no system gestures, so they are
nothing: no edges kept, nothing pressed, no swipe, and the rest is
ignored (the controller's rumble is host_sdl_rumble_gamepad's). */

void host_gesture_insets(int *insets)
{
	memset(insets, 0, 4 * sizeof(*insets));
}

/* (the overlay's devices: the Switch has no Android touch overlay to show or
hide, touch_input.c) */
int host_touch_devices(void)
{
	return 0;
}

/* (the phone's GPU family, for the GL ES renderer's Android fixes: none on
the Switch, guest_render_policy.c) */
int host_android_chip_family(void)
{
	return 0;
}

/* (Android's keyboard for a name typed on the touchscreen, sdl_platform.c:
the Switch types its names with its own keyboard applet) */
int host_sdl_text_input(uint32_t window, int type)
{
	(void)window;
	(void)type;
	return 0;
}

/* (a page opened in the browser: Discord's link, player_link.c; none on the
Switch) */
int host_sdl_open_url(const char *url)
{
	(void)url;
	return 0;
}

/* (the Theater's EXPORT and IMPORT are the Android app's pickers: the
Switch's replays are on the card, port/linux/game/menu_functions.c) */
void host_android_export_replays(const char *folder)
{
	(void)folder;
}

void host_android_import_replays(const char *folder)
{
	(void)folder;
}

/* (and the Mods screen's IMPORT, Video Setup's IMPORT DRIVER, and the map
lists' IMPORT) */
void host_android_import_mods(const char *folder)
{
	(void)folder;
}

void host_android_import_vk_drivers(const char *folder)
{
	(void)folder;
}

void host_android_import_maps(const char *folder)
{
	(void)folder;
}

void host_touch_read(int32_t *state)
{
	memset(state, 0, 7 * sizeof(*state));
}

void host_touch_look_read(float *delta)
{
	memset(delta, 0, 4 * sizeof(*delta));
}

void host_touch_rumble(unsigned int low, unsigned int high)
{
	(void)low;
	(void)high;
}

void host_touch_scene(int scene)
{
	(void)scene;
}

void host_touch_bindings(const int32_t *controls)
{
	(void)controls;
}

/* (no overlay: the controller's gyroscope is read by xinput_sdl.c) */
void host_touch_gyro_mode(int mode)
{
	(void)mode;
}

int host_touch_menu_fingers(void)
{
	return 3;
}

/* (a replay's buttons for the overlay to press: none to press them) */
void host_touch_targets(const int32_t *targets, int count)
{
	(void)targets;
	(void)count;
}
