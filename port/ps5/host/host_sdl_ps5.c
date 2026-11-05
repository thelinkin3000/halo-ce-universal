/*
HOST_SDL_PS5.C

The game's SDL3 calls on the PS5 (port/android/guest/runtime/guest_sdl.c and
port/linux/host/guest_desktop.c send them here, as the host_sdl_* imports
of host_imports.list), which Android answers with SDL3 (host_sdl.c) and the
Linux host with SDL3 and its desktop half (host_desktop.c).

Since phase 3 of port/ps5/PS5.md the pad, the sound and the events are
pacbrew's SDL2's (the Switch's host_sdl2.c is the model): SDL's start-up,
the pad and its events in host_sdl_ps5_input.c, the sound in
host_sdl_ps5_audio.c, which are compiled against SDL2's headers. This file
has SDL3's, for the guest's constants and layouts: it writes the events as
SDL3's, checks the SDL3 values the SDL2 files use (ps5_host.h), and keeps
the stand-ins for what SDL2 is not asked for (its video is never started).
The swap is a stand-in until phase 4's renderer presents, after which its
swapchain paces the frames instead. What stays a stand-in for good:
the window, the GL context, the displays, the 2D renderer and the desktop's
dialogs.

- The stand-in swap paces the game: with a swap interval, each frame waits
  for the next tick of 1/59.94 s (the console's only display mode,
  phase 0 B) on the monotonic clock, as the Switch's stand-in did under
  deko3d before it presented. Every 5 s a line says how many frames, their
  mean and longest: the headless run's heartbeat; every minute it adds the
  host's heap and direct memory's largest free block.
- The first start's question about the disc image is answered "Yes", and
  the file dialog with the first .iso or .xiso in the data folder, so the
  extraction runs with nobody at the console; the extraction's progress
  screen (the 2D renderer) draws nothing and counts its frames.
- The clocks are the monotonic clock's, not SDL2's (phase 0 B: its ticks
  ran 1 ms behind in 5 s).

Only integers, floats, strings and flat structures cross, as in host_sdl.c;
SDL's objects are handles into the table here (host.h). The scancode names
are SDL2's table, which takes and gives the same values as SDL3's (USB HID
usage numbers) under the same names and needs no SDL_Init: the one SDL2
call made through SDL3's declarations.
*/

#include "host.h"
#include "ps5_host.h"

#include <SDL3/SDL.h>

#include <dirent.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

_Static_assert(sizeof(SDL_Event) == 128, "SDL_Event is 128 bytes for the guest too");

/* the SDL3 values host_sdl_ps5_input.c and host_sdl_ps5_audio.c use, as
ps5_host.h writes them */
_Static_assert(SDL_GAMEPAD_BUTTON_SOUTH == 0 && SDL_GAMEPAD_BUTTON_DPAD_RIGHT == 14 && SDL_GAMEPAD_BUTTON_MISC1 == 15 &&
	SDL_GAMEPAD_BUTTON_TOUCHPAD == PS5_SDL3_GAMEPAD_BUTTON_TOUCHPAD, "SDL3's gamepad buttons");
_Static_assert(SDL_GAMEPAD_AXIS_LEFTX == 0 && SDL_GAMEPAD_AXIS_RIGHT_TRIGGER == 5 &&
	SDL_GAMEPAD_AXIS_COUNT == PS5_SDL3_GAMEPAD_AXIS_COUNT, "SDL3's gamepad axes");
_Static_assert((int)SDL_GAMEPAD_TYPE_UNKNOWN == PS5_SDL3_GAMEPAD_TYPE_UNKNOWN &&
	(int)SDL_GAMEPAD_TYPE_STANDARD == PS5_SDL3_GAMEPAD_TYPE_STANDARD &&
	(int)SDL_GAMEPAD_TYPE_XBOX360 == PS5_SDL3_GAMEPAD_TYPE_XBOX360 && (int)SDL_GAMEPAD_TYPE_XBOXONE == PS5_SDL3_GAMEPAD_TYPE_XBOXONE &&
	(int)SDL_GAMEPAD_TYPE_PS3 == PS5_SDL3_GAMEPAD_TYPE_PS3 && (int)SDL_GAMEPAD_TYPE_PS4 == PS5_SDL3_GAMEPAD_TYPE_PS4 &&
	(int)SDL_GAMEPAD_TYPE_PS5 == PS5_SDL3_GAMEPAD_TYPE_PS5 &&
	(int)SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO == PS5_SDL3_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO &&
	(int)SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_LEFT == PS5_SDL3_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_LEFT &&
	(int)SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_RIGHT == PS5_SDL3_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_RIGHT &&
	(int)SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_PAIR == PS5_SDL3_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_PAIR, "SDL3's gamepad types");
_Static_assert(SDL_AUDIO_F32 == PS5_SDL3_AUDIO_F32, "SDL3's float format");
_Static_assert(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK == PS5_SDL3_AUDIO_DEVICE_DEFAULT_PLAYBACK &&
	SDL_AUDIO_DEVICE_DEFAULT_RECORDING == PS5_SDL3_AUDIO_DEVICE_DEFAULT_RECORDING, "SDL3's default devices");
_Static_assert(sizeof(SDL_AudioSpec) == sizeof(struct ps5_sdl3_audio_spec) &&
	offsetof(SDL_AudioSpec, format) == offsetof(struct ps5_sdl3_audio_spec, format) &&
	offsetof(SDL_AudioSpec, channels) == offsetof(struct ps5_sdl3_audio_spec, channels) &&
	offsetof(SDL_AudioSpec, freq) == offsetof(struct ps5_sdl3_audio_spec, freq), "SDL3's SDL_AudioSpec");
_Static_assert(!__builtin_strcmp(SDL_HINT_AUDIO_DEVICE_SAMPLE_FRAMES, PS5_SDL3_HINT_AUDIO_DEVICE_SAMPLE_FRAMES),
	"SDL3's hint for the device's period");

/* ---------- handles (host.h; Android's host_sdl.c's) */

#define HANDLE_COUNT 256

struct handle
{
	int type;
	void *object;
};

static struct handle handles[HANDLE_COUNT];
static pthread_mutex_t handle_lock = PTHREAD_MUTEX_INITIALIZER;

uint32_t host_sdl_handle_new(int type, void *object)
{
	uint32_t index;

	if (!object)
		return 0;
	pthread_mutex_lock(&handle_lock);
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

void *host_sdl_handle_get(uint32_t handle, int type)
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

void host_sdl_handle_free(uint32_t handle)
{
	if (handle == 0 || handle >= HANDLE_COUNT)
		return;
	pthread_mutex_lock(&handle_lock);
	handles[handle].type = _handle_free;
	handles[handle].object = NULL;
	pthread_mutex_unlock(&handle_lock);
}

/* the stand-ins' objects: a handle to one of these is all there is */
static char standin_window, standin_context, standin_renderer;

/* the console's display: VideoOut's mode (3840x2160 at 59.94 Hz on a 4K
display), and the game renders at the stand-in window's size, the maximum
resolution against that mode (host_main.c sets it, at the start and when
config.toml changes: port/ps5/PS5.md, phase 7), which phase 4's present
scales to the mode */
int ps5_display_width = 1920, ps5_display_height = 1080;
#define DISPLAY_WIDTH ps5_display_width
#define DISPLAY_HEIGHT ps5_display_height
#define DISPLAY_RATE (60000.0f / 1001.0f)
#define DISPLAY_ID 1

/* one line, the first time a name is called */
static void log_once(int *done, const char *format, ...) __attribute__((format(printf, 2, 3)));
static void log_once(int *done, const char *format, ...)
{
	char line[512];
	va_list arguments;

	if (__atomic_exchange_n(done, 1, __ATOMIC_RELAXED))
		return;
	va_start(arguments, format);
	vsnprintf(line, sizeof(line), format, arguments);
	va_end(arguments);
	host_log(HOST_LOG_INFO, line);
}

/* ---------- start-up */

int64_t host_sdl_ticks(void)
{
	return (int64_t)(ps5_now_ns() / 1000000ull);
}

int64_t host_sdl_ticks_ns(void)
{
	return (int64_t)ps5_now_ns();
}

/* a small number of the calling thread's, as SDL's are */
int64_t host_sdl_thread_id(void)
{
	return ps5_thread_id();
}

/* the game's data folder, with its separator: where the desktop game looks
for config.toml (port_config.c) */
void host_sdl_get_base_path(char *buffer, uint32_t size)
{
	if (size)
		snprintf(buffer, size, "%s/", PS5_DATA_FOLDER);
}

int host_sdl_get_hint_boolean(const char *name, int default_value)
{
	(void)name;
	return default_value != 0;
}

int host_sdl_has_screen_keyboard_support(void)
{
	return 0;
}

int host_sdl_text_input_active(uint32_t window)
{
	(void)window;
	return 0;
}

int host_sdl_start_text_input(uint32_t window, int multiline, int type)
{
	(void)window;
	(void)multiline;
	(void)type;
	return 0;
}

int host_sdl_stop_text_input(uint32_t window)
{
	(void)window;
	return 1;
}

/* ---------- the window and the context */

static int is_window(uint32_t window)
{
	return host_sdl_handle_get(window, _handle_window) == &standin_window;
}

uint32_t host_sdl_create_window(const char *title, int width, int height, int64_t flags)
{
	host_logf(HOST_LOG_INFO, "window \"%s\" (%dx%d asked, flags 0x%llx): the stand-in, %dx%d fullscreen",
		title ? title : "", width, height, (unsigned long long)flags, DISPLAY_WIDTH, DISPLAY_HEIGHT);
	return host_sdl_handle_new(_handle_window, &standin_window);
}

void host_sdl_window_size_in_pixels(uint32_t window, int *width, int *height)
{
	*width = is_window(window) ? DISPLAY_WIDTH : 0;
	*height = is_window(window) ? DISPLAY_HEIGHT : 0;
}

void host_sdl_get_window_size(uint32_t window, int *width, int *height)
{
	host_sdl_window_size_in_pixels(window, width, height);
}

int host_sdl_set_window_size(uint32_t window, int width, int height)
{
	(void)width;
	(void)height;
	return is_window(window);
}

int64_t host_sdl_get_window_flags(uint32_t window)
{
	return is_window(window) ? (int64_t)(SDL_WINDOW_FULLSCREEN | SDL_WINDOW_OPENGL | SDL_WINDOW_INPUT_FOCUS) : 0;
}

int host_sdl_set_window_fullscreen(uint32_t window, int fullscreen)
{
	(void)fullscreen;
	return is_window(window);
}

int host_sdl_set_window_fullscreen_mode(uint32_t window, uint32_t display, int width, int height, float refresh_rate)
{
	(void)display;
	(void)width;
	(void)height;
	(void)refresh_rate;
	return is_window(window);
}

void host_sdl_destroy_window(uint32_t window)
{
	if (is_window(window))
		host_sdl_handle_free(window);
}

int host_sdl_warp_mouse_in_window(uint32_t window, float x, float y)
{
	(void)window;
	(void)x;
	(void)y;
	return 1;
}

int host_sdl_set_relative_mouse(uint32_t window, int enabled)
{
	(void)enabled;
	return is_window(window);
}

int host_sdl_gl_set_attribute(int attribute, int value)
{
	(void)attribute;
	(void)value;
	return 1;
}

uint32_t host_sdl_gl_create_context(uint32_t window)
{
	if (!is_window(window))
		return 0;
	host_logf(HOST_LOG_INFO, "GL context: the stand-in (the console has no OpenGL; the image's renderer is Vulkan's)");
	return host_sdl_handle_new(_handle_context, &standin_context);
}

int host_sdl_gl_make_current(uint32_t window, uint32_t context)
{
	return is_window(window) && host_sdl_handle_get(context, _handle_context) == &standin_context;
}

static int swap_interval = 1;

int host_sdl_gl_set_swap_interval(int interval)
{
	swap_interval = interval;
	return 1;
}

/* the heartbeat: frames since the last line, their mean and longest, and how
many took over 20 and over 33 ms (a hitch shows as a count, not only as the
longest); when the window's frames came at over 80 a second (a 120 Hz
display), over 10 and over 17 ms too */
#define HEARTBEAT_NS 5000000000ull
#define MEMORY_LINE_NS 60000000000ull

static uint64_t frames_swapped;

uint64_t ps5_frames_swapped(void)
{
	return __atomic_load_n(&frames_swapped, __ATOMIC_RELAXED);
}

static void heartbeat(uint64_t now)
{
	static uint64_t line_start, last_frame, longest, frames, memory_reported;
	static unsigned over10, over17, over20, over33;
	char buckets[96], costs[160];
	double seconds;

	if (last_frame)
	{
		uint64_t frame = now - last_frame;

		if (frame > longest)
			longest = frame;
		over10 += frame > 10000000ull;
		over17 += frame > 17000000ull;
		over20 += frame > 20000000ull;
		over33 += frame > 33000000ull;
	}
	last_frame = now;
	frames++;
	if (!line_start)
	{
		line_start = memory_reported = now;
		host_logf(HOST_LOG_INFO, "heartbeat: the first frame at %.1f s", ps5_seconds());
		return;
	}
	if (now - line_start < HEARTBEAT_NS)
		return;
	seconds = (now - line_start) / 1e9;
	{
		unsigned lines, slow;
		double klog_ms, file_ms;

		/* (before this line is written, so it is not counted in itself) */
		ps5_log_costs(&lines, &slow, &klog_ms, &file_ms);
		snprintf(costs, sizeof(costs), "; the log: %u lines, %u over 2 ms, the longest %.2f ms to the kernel log and "
			"%.2f ms to the file", lines, slow, klog_ms, file_ms);
	}
	if (frames / seconds > 80.0)
		snprintf(buckets, sizeof(buckets), "over 10 ms %u, over 17 ms %u, over 20 ms %u, over 33 ms %u", over10,
			over17, over20, over33);
	else
		snprintf(buckets, sizeof(buckets), "over 20 ms %u, over 33 ms %u", over20, over33);
	if (now - memory_reported >= MEMORY_LINE_NS)
	{
		double heap, peak, direct;

		ps5_memory_figures(&heap, &peak, &direct);
		host_logf(HOST_LOG_INFO, "heartbeat: %llu frames in %.2f s (%.2f a second), mean %.2f ms, longest %.2f ms, "
			"%s; the host's heap %.0f MB (peak %.0f MB), direct memory's largest free block %.0f MB%s",
			(unsigned long long)frames, seconds, frames / seconds, (now - line_start) / 1e6 / (double)frames,
			longest / 1e6, buckets, heap, peak, direct, costs);
		ps5_alloc_report();
		memory_reported = now;
	}
	else
	{
		host_logf(HOST_LOG_INFO, "heartbeat: %llu frames in %.2f s (%.2f a second), mean %.2f ms, longest %.2f ms, %s%s",
			(unsigned long long)frames, seconds, frames / seconds, (now - line_start) / 1e6 / (double)frames,
			longest / 1e6, buckets, costs);
	}
	line_start = now;
	frames = 0;
	longest = 0;
	over10 = over17 = over20 = over33 = 0;
}

/* The stand-in swap: until the renderer presents (host_vk_presenting, phase
4) and its swapchain waits for the display, each frame is held to the
display's 59.94 Hz here, as the swap's vsync holds them elsewhere. */
int host_sdl_gl_swap_window(uint32_t window)
{
	static uint64_t next;
	uint64_t now = ps5_now_ns();

	if (!is_window(window))
		return 0;
	if (swap_interval > 0 && !host_vk_presenting)
	{
		const uint64_t interval = (uint64_t)(1e9 / DISPLAY_RATE);

		if (next > now)
		{
			struct timespec deadline;

			deadline.tv_sec = (time_t)(next / 1000000000ull);
			deadline.tv_nsec = (long)(next % 1000000000ull);
			clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &deadline, NULL);
			now = ps5_now_ns();
		}
		/* a frame more than an interval late starts the count again */
		next = now > next + interval ? now + interval : next + interval;
	}
	__atomic_add_fetch(&frames_swapped, 1, __ATOMIC_RELAXED);
	host_gl_frame();
	heartbeat(now);
	return 1;
}

/* ---------- displays */

/* host_desktop.c's struct host_display_mode, which guest_desktop.c reads */
struct host_display_mode
{
	uint32_t display;
	uint32_t format;
	int32_t width, height;
	float pixel_density;
	float refresh_rate;
	int32_t refresh_rate_numerator, refresh_rate_denominator;
};

static void display_mode(struct host_display_mode *mode)
{
	mode->display = DISPLAY_ID;
	mode->format = SDL_PIXELFORMAT_XRGB8888;
	mode->width = DISPLAY_WIDTH;
	mode->height = DISPLAY_HEIGHT;
	mode->pixel_density = 1.0f;
	mode->refresh_rate = DISPLAY_RATE;
	mode->refresh_rate_numerator = 60000;
	mode->refresh_rate_denominator = 1001;
}

uint32_t host_sdl_get_display_for_window(uint32_t window)
{
	return is_window(window) ? DISPLAY_ID : 0;
}

uint32_t host_sdl_get_primary_display(void)
{
	return DISPLAY_ID;
}

int host_sdl_get_display_mode(uint32_t display, int current, void *result)
{
	(void)current;
	if (display != DISPLAY_ID)
		return 0;
	display_mode(result);
	return 1;
}

int host_sdl_get_fullscreen_display_modes(uint32_t display, void *result, int maximum)
{
	if (display != DISPLAY_ID)
		return 0;
	if (maximum > 0)
		display_mode(result);
	return 1;
}

int host_sdl_get_closest_fullscreen_display_mode(uint32_t display, int width, int height, float refresh_rate,
	int include_high_density_modes, void *result)
{
	(void)width;
	(void)height;
	(void)refresh_rate;
	(void)include_high_density_modes;
	return host_sdl_get_display_mode(display, 1, result);
}

int host_sdl_get_display_usable_bounds(uint32_t display, void *rectangle)
{
	SDL_Rect bounds = { 0, 0, DISPLAY_WIDTH, DISPLAY_HEIGHT };

	if (display != DISPLAY_ID)
		return 0;
	memcpy(rectangle, &bounds, sizeof(bounds));
	return 1;
}

/* ---------- events: the guest's own pushed ones first, then SDL2's and the
overlay's (host_sdl_ps5_input.c), written as SDL3's */

#define EVENT_QUEUE 32

static SDL_Event event_queue[EVENT_QUEUE];
static int event_first, event_count;
static pthread_mutex_t event_lock = PTHREAD_MUTEX_INITIALIZER;

int host_sdl_poll_event(void *event)
{
	SDL_Event result;
	uint32_t which;
	int found = 0;

	/* (the window's size, on the thread that asks it) */
	ps5_display_check(ps5_now_ns());
	pthread_mutex_lock(&event_lock);
	if (event_count)
	{
		memcpy(event, &event_queue[event_first], sizeof(SDL_Event));
		event_first = (event_first + 1) % EVENT_QUEUE;
		event_count--;
		found = 1;
	}
	pthread_mutex_unlock(&event_lock);
	if (found)
		return 1;
	memset(&result, 0, sizeof(result));
	switch (ps5_input_poll(&which))
	{
	case PS5_EVENT_QUIT:
		result.type = SDL_EVENT_QUIT;
		break;
	case PS5_EVENT_GAMEPAD_ADDED:
		result.type = SDL_EVENT_GAMEPAD_ADDED;
		result.gdevice.which = (SDL_JoystickID)which;
		break;
	case PS5_EVENT_GAMEPAD_REMOVED:
		result.type = SDL_EVENT_GAMEPAD_REMOVED;
		result.gdevice.which = (SDL_JoystickID)which;
		break;
	case PS5_EVENT_FOCUS_LOST:
		result.type = SDL_EVENT_WINDOW_FOCUS_LOST;
		result.window.windowID = 1;
		break;
	case PS5_EVENT_FOCUS_GAINED:
		result.type = SDL_EVENT_WINDOW_FOCUS_GAINED;
		result.window.windowID = 1;
		break;
	default:
		return 0;
	}
	result.common.timestamp = ps5_now_ns();
	memcpy(event, &result, sizeof(result));
	return 1;
}

void host_sdl_pump_events(void)
{
}

/* (the menus' Quit pushes SDL_EVENT_QUIT) */
int host_sdl_push_event(const void *event)
{
	int pushed = 0;

	pthread_mutex_lock(&event_lock);
	if (event_count < EVENT_QUEUE)
	{
		memcpy(&event_queue[(event_first + event_count) % EVENT_QUEUE], event, sizeof(SDL_Event));
		event_count++;
		pushed = 1;
	}
	pthread_mutex_unlock(&event_lock);
	host_logf(HOST_LOG_INFO, "SDL: the game pushed event 0x%x", ((const SDL_Event *)event)->type);
	return pushed;
}

/* the desktop guest maps keys by name at start: SDL's own table */
void host_sdl_scancode_name(int32_t scancode, char *buffer, uint32_t size)
{
	const char *name = SDL_GetScancodeName((SDL_Scancode)scancode);

	if (size)
		snprintf(buffer, size, "%s", name ? name : "");
}

int32_t host_sdl_scancode_from_name(const char *name)
{
	return (int32_t)SDL_GetScancodeFromName(name);
}

/* ---------- the clipboard, the toast, the messages */

static char clipboard[1024];
static pthread_mutex_t clipboard_lock = PTHREAD_MUTEX_INITIALIZER;

int host_sdl_set_clipboard_text(const char *text)
{
	pthread_mutex_lock(&clipboard_lock);
	snprintf(clipboard, sizeof(clipboard), "%s", text ? text : "");
	pthread_mutex_unlock(&clipboard_lock);
	return 1;
}

void host_sdl_get_clipboard_text(char *buffer, uint32_t size)
{
	pthread_mutex_lock(&clipboard_lock);
	if (size)
		snprintf(buffer, size, "%s", clipboard);
	pthread_mutex_unlock(&clipboard_lock);
}

int host_sdl_show_toast(const char *message, int duration, int gravity, int x, int y)
{
	(void)duration;
	(void)gravity;
	(void)x;
	(void)y;
	host_logf(HOST_LOG_INFO, "toast: %s", message ? message : "");
	return 0;
}

int host_sdl_show_simple_message_box(uint32_t flags, const char *title, const char *message)
{
	host_logf(HOST_LOG_INFO, "message box (flags 0x%x) \"%s\": %s", flags, title ? title : "", message ? message : "");
	return 1;
}

/* set when the file dialog found no disc image to answer with: the game asks
its question again, and is answered "No" this time, so it quits rather than
asking forever (sdl_platform.c's platform_offer_game_data) */
static int no_disc_image;

/* nobody is there to answer: "Yes" if a button says so, else the first; "No"
once the file dialog has found no disc image */
int host_sdl_show_message_box(struct host_message_box *box)
{
	const char *texts = (const char *)(uintptr_t)box->button_texts;
	char buttons[256];
	size_t used = 0, listed = 0;
	int index, chosen = 0;

	if (box->button_count < 1 || box->button_count > HOST_MESSAGE_BOX_BUTTONS)
		return 0;
	buttons[0] = 0;
	for (index = 0; index < box->button_count; index++)
	{
		const char *end = used < box->button_texts_size ? memchr(texts + used, 0, box->button_texts_size - used) : NULL;

		if (!end)
			return 0;
		if (!strcasecmp(texts + used, no_disc_image ? "No" : "Yes"))
			chosen = index;
		if (listed < sizeof(buttons))
			listed += (size_t)snprintf(buttons + listed, sizeof(buttons) - listed, "%s\"%s\"", index ? ", " : "",
				texts + used);
		used = (size_t)(end - texts) + 1;
	}
	host_logf(HOST_LOG_INFO, "message box \"%s\": %s", (const char *)(uintptr_t)box->title,
		(const char *)(uintptr_t)box->message);
	host_logf(HOST_LOG_INFO, "message box: buttons %s; answered %d (button %d) with nobody at the console%s", buttons,
		box->button_ids[chosen], chosen, no_disc_image ? ", for there is no disc image: the game ends" : "");
	box->chosen = box->button_ids[chosen];
	return 1;
}

/* whether name ends in .iso or .xiso, in any case */
static int is_disc_image(const char *name)
{
	size_t length = strlen(name);

	return (length > 4 && !strcasecmp(name + length - 4, ".iso")) ||
		(length > 5 && !strcasecmp(name + length - 5, ".xiso"));
}

/* the first disc image in the data folder, for the first start's question
(sdl_platform.c's data_choose_image) */
int host_sdl_open_file_dialog(int filter_count, const char *filter_names, const char *filter_patterns, char *path,
	uint32_t size)
{
	DIR *folder;
	struct dirent *entry;
	int index, found = 0;

	for (index = 0; index < filter_count && index < 8; index++)
	{
		host_logf(HOST_LOG_INFO, "file dialog: filter \"%s\" (%s)", filter_names, filter_patterns);
		filter_names += strlen(filter_names) + 1;
		filter_patterns += strlen(filter_patterns) + 1;
	}
	if (!size)
		return 0;
	path[0] = 0;
	folder = opendir(PS5_DATA_FOLDER);
	while (folder && !found && (entry = readdir(folder)))
	{
		if (entry->d_type != DT_DIR && is_disc_image(entry->d_name))
			found = (uint32_t)snprintf(path, size, "%s/%s", PS5_DATA_FOLDER, entry->d_name) < size;
	}
	if (folder)
		closedir(folder);
	if (found)
	{
		host_logf(HOST_LOG_INFO, "file dialog: answered %s with nobody at the console", path);
		ps5_notify("SlipstreamCE: extracting the game's maps from %s (about 1.8 GB, under a minute)", strrchr(path, '/') + 1);
	}
	else
	{
		path[0] = 0;
		host_logf(HOST_LOG_ERROR, "file dialog: no .iso or .xiso in %s to answer with: put the game's disc image "
			"there (port/ps5/README.md)", PS5_DATA_FOLDER);
		if (!no_disc_image)
			ps5_notify("SlipstreamCE: put the game's disc image (.iso or .xiso) in %s over FTP and start again",
				PS5_DATA_FOLDER);
		no_disc_image = 1;
	}
	return found;
}

/* ---------- the 2D renderer (the extraction's progress screen): nothing
drawn, its presents counted; the progress it would write is sent as
notifications, one every 10 %, and one when it is done */

static uint64_t renderer_presents;
/* the extraction's progress, from its text: the megabytes done and in all,
and the last tenth notified */
static unsigned long long progress_done, progress_total;
static int progress_tenths = -1;

uint32_t host_sdl_create_renderer(uint32_t window, const char *name)
{
	static int logged;

	log_once(&logged, "2D renderer (%s): the stand-in, which draws nothing", name ? name : "default");
	return is_window(window) ? host_sdl_handle_new(_handle_renderer, &standin_renderer) : 0;
}

static int is_renderer(uint32_t renderer)
{
	return host_sdl_handle_get(renderer, _handle_renderer) == &standin_renderer;
}

void host_sdl_destroy_renderer(uint32_t renderer)
{
	if (is_renderer(renderer))
	{
		host_sdl_handle_free(renderer);
		host_logf(HOST_LOG_INFO, "2D renderer: destroyed after %llu presents", (unsigned long long)renderer_presents);
		if (progress_total && progress_done >= progress_total)
			ps5_notify("SlipstreamCE: the maps are in place (%llu MB); the game starts", progress_total);
		else if (progress_total)
			ps5_notify("SlipstreamCE: the extraction stopped at %llu of %llu MB", progress_done, progress_total);
	}
}

int host_sdl_set_render_vsync(uint32_t renderer, int vsync)
{
	(void)vsync;
	return is_renderer(renderer);
}

int host_sdl_set_render_draw_color(uint32_t renderer, uint32_t red, uint32_t green, uint32_t blue, uint32_t alpha)
{
	(void)red;
	(void)green;
	(void)blue;
	(void)alpha;
	return is_renderer(renderer);
}

int host_sdl_set_render_scale(uint32_t renderer, float x, float y)
{
	(void)x;
	(void)y;
	return is_renderer(renderer);
}

int host_sdl_render_clear(uint32_t renderer)
{
	return is_renderer(renderer);
}

int host_sdl_render_fill_rect(uint32_t renderer, const void *rectangle)
{
	(void)rectangle;
	return is_renderer(renderer);
}

/* (what it would write: the extraction's file and its megabytes, "<file>
(<done> of <total> MB)" (sdl_platform.c's data_extract), logged every ten
seconds, and a notification at every tenth) */
int host_sdl_render_debug_text(uint32_t renderer, float x, float y, const char *text)
{
	static uint64_t logged_at;
	uint64_t now = ps5_now_ns();
	const char *figures = text ? strrchr(text, '(') : NULL;
	unsigned long long done, total;

	(void)x;
	if (y > 50.0f && text && now - logged_at >= 10000000000ull)
	{
		logged_at = now;
		host_logf(HOST_LOG_INFO, "2D renderer: \"%s\" at %.1f s", text, ps5_seconds());
	}
	if (y > 50.0f && figures && sscanf(figures, "(%llu of %llu MB)", &done, &total) == 2 && total)
	{
		int tenths = (int)(done * 10 / total);

		progress_done = done;
		progress_total = total;
		if (tenths > progress_tenths)
		{
			if (progress_tenths >= 0 && tenths < 10)
				ps5_notify("SlipstreamCE: extracting the maps: %d %% (%llu of %llu MB)", tenths * 10, done, total);
			progress_tenths = tenths;
		}
	}
	return is_renderer(renderer);
}

/* (the heartbeat's line while the 2D renderer is the screen: the game swaps
no frames then) */
int host_sdl_render_present(uint32_t renderer)
{
	static uint64_t line_start, line_presents;
	uint64_t now = ps5_now_ns();

	if (!is_renderer(renderer))
		return 0;
	__atomic_add_fetch(&renderer_presents, 1, __ATOMIC_RELAXED);
	line_presents++;
	if (!line_start)
		line_start = now;
	else if (now - line_start >= HEARTBEAT_NS)
	{
		host_logf(HOST_LOG_INFO, "heartbeat: the stand-in 2D renderer (the first start's progress screen, nothing "
			"drawn): %llu presents in %.2f s, the extraction at %llu of %llu MB", (unsigned long long)line_presents,
			(now - line_start) / 1e9, progress_done, progress_total);
		line_start = now;
		line_presents = 0;
	}
	return 1;
}

void ps5_sdl_report(void)
{
	host_logf(HOST_LOG_INFO, "SDL stand-ins: %llu frames swapped, %llu presents of the 2D renderer",
		(unsigned long long)frames_swapped, (unsigned long long)renderer_presents);
	ps5_input_report();
	ps5_audio_report();
}
