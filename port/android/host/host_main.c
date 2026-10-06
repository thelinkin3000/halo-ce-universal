/*
HOST_MAIN.C

Entry point of the Android port (SDL_main, called by SDLActivity on its
own thread).

It loads the guest image (the game, built as ILP32 code) from the APK's
assets, gives it an environment describing where the game data and saves
live, and runs its main() on a thread of its own with its stack in guest
memory (host_thread.c), on which everything here after startup runs; the
SDL thread waits for it.

Storage (see port/android/README.md): the game data (the directory holding
maps/) is the app's external files directory,
/sdcard/Android/data/<package>/files, where the launcher activity copies it
on first run; saves go to its save/ subdirectory. The settings,
config.toml, live there too (port/linux/src/port_config.c, which the game
reads); this file reads only debug.sample_seconds and debug.profile_hz from
it, for the samplers that run here, and debug.vk_probe, which runs the
Vulkan probe (host_vk_probe.c) instead of the game, on the driver that
display.vk_driver names (host_vk_driver.c). It also reads display.renderer, to
choose between the two game images (halo_guest.elf, drawing with OpenGL ES,
and halo_guest_vk.elf, with Vulkan: port/android/VULKAN.md), and
debug.vk_validation for the Vulkan renderer.
*/

#include "host.h"
#include "tomlc17.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <android/log.h>
#include <errno.h>
#include <ftw.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

void host_install_signal_handlers(void);

int host_renderer_vulkan;

/* ---------- logging and termination */

void host_logf(int priority, const char *format, ...)
{
	va_list arguments;

	va_start(arguments, format);
	__android_log_vprint(priority, "halo", format, arguments);
	va_end(arguments);
}

void host_log(int priority, const char *text)
{
	__android_log_write(priority, "halo", text);
}

void host_fatal(const char *format, ...)
{
	char message[1024];
	va_list arguments;

	va_start(arguments, format);
	vsnprintf(message, sizeof(message), format, arguments);
	va_end(arguments);
	__android_log_write(ANDROID_LOG_FATAL, "halo", message);
	SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Halo", message, NULL);
	_exit(1);
}

void host_abort(const char *reason)
{
	__android_log_print(ANDROID_LOG_FATAL, "halo", "guest abort: %s", reason);
	abort();
}

void host_exit(int code)
{
	host_logf(HOST_LOG_INFO, "the game exited (%d)", code);
	host_vk_exit();
	/* the process ends with the game; Android restarts it from the
	launcher next time */
	_exit(code);
}

int host_errno(void)
{
	return errno;
}

/* ---------- paths */

static char data_root[512];
static char save_root[512];

void host_android_path(int which, char *buffer, uint32_t size)
{
	snprintf(buffer, size, "%s", which ? save_root : data_root);
}

static int directory_has_maps(const char *root)
{
	char path[600];
	struct stat information;

	snprintf(path, sizeof(path), "%s/maps/ui.map", root);
	return stat(path, &information) == 0;
}

/* Directories the app creates in its external storage are private to it
(mode 0770 under the app's own group), so the shell user (adb) cannot list
them. Open the save tree for reading, with set-group-ID directories as
posix_make_directory creates them (port/linux/src/posix_files.c). */
static int share_entry(const char *path, const struct stat *information, int type, struct FTW *walk)
{
	(void)information;
	(void)walk;
	if (type == FTW_D || type == FTW_DP)
		chmod(path, 02775);
	else if (type == FTW_F)
		chmod(path, 0664);
	return 0;
}

static void share_save_tree(const char *root)
{
	nftw(root, share_entry, 16, FTW_PHYS);
}

/* ---------- the guest's environment */

#define ENVIRONMENT_MAXIMUM 64

struct environment
{
	char *entries[ENVIRONMENT_MAXIMUM];
	int count;
};

static void environment_set(struct environment *environment, const char *name, const char *value)
{
	size_t length = strlen(name);
	char *entry;
	int index;

	entry = malloc(length + strlen(value) + 2);
	sprintf(entry, "%s=%s", name, value);
	for (index = 0; index < environment->count; index++)
	{
		if (!strncmp(environment->entries[index], name, length) && environment->entries[index][length] == '=')
		{
			free(environment->entries[index]);
			environment->entries[index] = entry;
			return;
		}
	}
	if (environment->count < ENVIRONMENT_MAXIMUM)
		environment->entries[environment->count++] = entry;
	else
		free(entry);
}

/* the settings of [debug] that this file reads, from config.toml */
struct host_settings
{
	char sample_seconds[32]; /* as text for the sampler; empty for none */
	int profile_hz;
	char vk_probe[128]; /* empty: the game runs */
	char vk_driver[256]; /* display.vk_driver: empty (the default) for the phone's own driver, "auto", or an archive */
	char renderer[32]; /* display.renderer: "vulkan" (the default, also when empty) or "gl" */
	int vk_validation; /* debug.vk_validation */
	int gpu_stats; /* debug.gpu_stats */
	int vk_present_marker; /* debug.vk_present_marker */
	int vk_self_test; /* debug.vk_self_test */
};

static void config_read(const char *path, struct host_settings *settings)
{
	toml_result_t result = toml_parse_file_ex(path);

	memset(settings, 0, sizeof(*settings));
	/* the defaults of port_config.c's rows, for a file not written yet (the first start) or without them */
	snprintf(settings->renderer, sizeof(settings->renderer), "vulkan");
	if (!result.ok)
	{
		host_logf(HOST_LOG_WARN, "cannot read %s: %s; the host's settings are the defaults", path, result.errmsg);
		return;
	}
	{
		toml_datum_t seconds = toml_seek(result.toptab, "debug.sample_seconds");
		toml_datum_t hz = toml_seek(result.toptab, "debug.profile_hz");
		toml_datum_t probe = toml_seek(result.toptab, "debug.vk_probe");
		toml_datum_t driver = toml_seek(result.toptab, "display.vk_driver");
		toml_datum_t renderer = toml_seek(result.toptab, "display.renderer");
		toml_datum_t validation = toml_seek(result.toptab, "debug.vk_validation");
		toml_datum_t statistics = toml_seek(result.toptab, "debug.gpu_stats");
		toml_datum_t marker = toml_seek(result.toptab, "debug.vk_present_marker");
		toml_datum_t self_test = toml_seek(result.toptab, "debug.vk_self_test");
		double value = seconds.type == TOML_FP64 ? seconds.u.fp64 :
			seconds.type == TOML_INT64 ? (double)seconds.u.int64 : 0.0;

		if (value > 0.0)
			snprintf(settings->sample_seconds, sizeof(settings->sample_seconds), "%g", value);
		if (hz.type == TOML_INT64 && hz.u.int64 > 0)
			settings->profile_hz = hz.u.int64 > 10000 ? 10000 : (int)hz.u.int64;
		if (probe.type == TOML_STRING)
			snprintf(settings->vk_probe, sizeof(settings->vk_probe), "%s", probe.u.s);
		if (driver.type == TOML_STRING)
			snprintf(settings->vk_driver, sizeof(settings->vk_driver), "%s", driver.u.s);
		if (renderer.type == TOML_STRING)
			snprintf(settings->renderer, sizeof(settings->renderer), "%s", renderer.u.s);
		settings->vk_validation = validation.type == TOML_BOOLEAN && validation.u.boolean;
		settings->gpu_stats = statistics.type == TOML_BOOLEAN && statistics.u.boolean;
		settings->vk_present_marker = marker.type == TOML_BOOLEAN && marker.u.boolean;
		settings->vk_self_test = self_test.type == TOML_BOOLEAN && self_test.u.boolean;
	}
	toml_free(result);
}

/* POSIX TZ for the current local offset (the guest's musl has no zone
database) */
static void time_zone(char *buffer, size_t size)
{
	time_t now = time(NULL);
	struct tm local;
	long offset;

	localtime_r(&now, &local);
	offset = -local.tm_gmtoff;
	snprintf(buffer, size, "<L>%s%ld:%02ld", offset < 0 ? "-" : "", labs(offset) / 3600, (labs(offset) / 60) % 60);
}

/* copies argv and the environment into guest memory */
static uint32_t make_boot(const struct environment *environment)
{
	size_t size = 0x10000;
	char *memory = host_low_map(size, PROT_READ | PROT_WRITE);
	struct halo_guest_boot *boot = (struct halo_guest_boot *)memory;
	uint32_t *argv = (uint32_t *)(memory + sizeof(*boot));
	uint32_t *environ_list = argv + 2;
	char *strings = (char *)(environ_list + ENVIRONMENT_MAXIMUM + 1);
	int index;

	if (!memory)
		host_fatal("cannot allocate the guest's environment");
	strcpy(strings, "halo");
	argv[0] = (uint32_t)(uintptr_t)strings;
	argv[1] = 0;
	strings += strlen(strings) + 1;
	for (index = 0; index < environment->count; index++)
	{
		size_t length = strlen(environment->entries[index]) + 1;

		if (strings + length > memory + size)
			break;
		memcpy(strings, environment->entries[index], length);
		environ_list[index] = (uint32_t)(uintptr_t)strings;
		strings += length;
	}
	environ_list[index] = 0;
	boot->argc = 1;
	boot->argv = (uint32_t)(uintptr_t)argv;
	boot->environment = (uint32_t)(uintptr_t)environ_list;
	boot->page_size = (uint32_t)getpagesize();
	boot->contiguous_base = host_memory_window_base();
	boot->image_shift = host_image.shift;
	return (uint32_t)(uintptr_t)boot;
}

/* ---------- main */

#define MAIN_STACK_SIZE (16 * 1024 * 1024)

static void *game_main(void *unused)
{
	struct environment environment = { { 0 }, 0 };
	const char *external;
	char zone[64];
	char path[600];
	size_t image_size = 0, vk_image_size = 0, image_used_size;
	void *image, *vk_image = NULL, *image_used;
	uint32_t span, image_base = 0;
	char vulkan_refused[300] = ""; /* why Vulkan, asked for, cannot be tried */
	int want_vulkan = 0;
	uint32_t boot;
	struct host_settings settings;

	(void)unused;
	external = SDL_GetAndroidExternalStoragePath();
	if (!external)
		host_fatal("Android storage is unavailable: %s", SDL_GetError());
	snprintf(data_root, sizeof(data_root), "%s", external);
	snprintf(save_root, sizeof(save_root), "%s/save", external);
	/* readable by adb (the shell user), for managing saves */
	mkdir(save_root, 0775);
	share_save_tree(save_root);
	if (!directory_has_maps(data_root))
	{
		host_fatal("The Halo game data was not found.\n\nCopy the PAL game data (build 01.01.14.2342), "
			"the folder that contains maps, into\n%s\nor import it from the launcher screen.", data_root);
	}

	environment_set(&environment, "HOME", save_root);
	environment_set(&environment, "HALO_DATA_ROOT", data_root);
	environment_set(&environment, "HALO_SAVE_ROOT", save_root);
	time_zone(zone, sizeof(zone));
	environment_set(&environment, "TZ", zone);
	/* internet play's MQTT brokers (network.brokers_file): the APK's list,
	written beside config.toml at each start, as a desktop update replaces
	the file beside its game */
	{
		size_t brokers_size = 0;
		void *brokers = SDL_LoadFile("brokers.txt", &brokers_size);

		snprintf(path, sizeof(path), "%s/brokers.txt", data_root);
		if (!brokers || !SDL_SaveFile(path, brokers, brokers_size))
			host_logf(HOST_LOG_ERROR, "cannot write %s: %s", path, SDL_GetError());
		SDL_free(brokers);
	}
	snprintf(path, sizeof(path), "%s/config.toml", data_root);

	config_read(path, &settings);
	/* the renderer decides the image (host.h). The probe runs on the GL image,
	as before, whatever the renderer is */
	if (settings.vk_probe[0])
		want_vulkan = 0;
	else if (strcmp(settings.renderer, "gl"))
	{
		want_vulkan = 1;
		if (settings.renderer[0] && strcmp(settings.renderer, "vulkan"))
			host_logf(HOST_LOG_WARN, "display.renderer \"%s\" is not \"gl\" or \"vulkan\"; using Vulkan", settings.renderer);
	}

	image = SDL_LoadFile("halo_guest.elf", &image_size);
	if (!image)
		host_fatal("cannot read the game image from the APK: %s", SDL_GetError());
	if (want_vulkan)
	{
		vk_image = SDL_LoadFile("halo_guest_vk.elf", &vk_image_size);
		if (!vk_image)
		{
			snprintf(vulkan_refused, sizeof(vulkan_refused), "the APK has no halo_guest_vk.elf: %s", SDL_GetError());
			want_vulkan = 0;
		}
	}

	/* the image's range is reserved before anything else is brought up: bringing the
	display and Vulkan up map memory of their own, which could take the address the
	image is linked at and make it move when it need not (host_loader.c moves it only
	where the address was already taken). Both images are linked to run at the same
	address; the larger one's span is reserved (there, or wherever there is room:
	image_base), and the other image's data is freed once the choice is made */
	span = host_image_span(image, image_size);
	if (want_vulkan && !host_image_span(vk_image, vk_image_size))
	{
		snprintf(vulkan_refused, sizeof(vulkan_refused), "halo_guest_vk.elf is not a guest image");
		want_vulkan = 0;
		SDL_free(vk_image);
		vk_image = NULL;
	}
	if (want_vulkan && host_image_span(vk_image, vk_image_size) > span)
		span = host_image_span(vk_image, vk_image_size);
	if (!span || host_memory_initialize(HALO_GUEST_IMAGE_BASE, span, &image_base) != 0)
		host_fatal("cannot load the game image; see logcat (tag \"halo\") for details");

	{
		/* the game renders 480 lines at the display's aspect ratio
		(landscape) unless display.screen_width says otherwise (d3d8_gl.c) */
		const SDL_DisplayMode *mode;
		char width[16];

		SDL_InitSubSystem(SDL_INIT_VIDEO);
		mode = SDL_GetDesktopDisplayMode(SDL_GetPrimaryDisplay());
		if (mode && mode->w > 0 && mode->h > 0)
		{
			int longer = mode->w > mode->h ? mode->w : mode->h;
			int shorter = mode->w > mode->h ? mode->h : mode->w;

			snprintf(width, sizeof(width), "%d", (480 * longer / shorter) & ~1);
			environment_set(&environment, "HALO_DISPLAY_WIDTH", width);
			host_logf(HOST_LOG_INFO, "display %dx%d: rendering %sx480", mode->w, mode->h, width);
		}
	}

	if (want_vulkan)
	{
		char line[600];

		host_renderer_vulkan = host_vk_startup(settings.vk_driver, settings.vk_validation, line, sizeof(line));
		host_logf(HOST_LOG_INFO, "renderer: %s", line);
	}
	else
	{
		/* one line either way: a reason found before Vulkan was tried goes in it */
		if (vulkan_refused[0])
			host_logf(HOST_LOG_WARN, "renderer: GL ES (Vulkan was asked for: %s)", vulkan_refused);
		else
			host_logf(HOST_LOG_INFO, "renderer: GL ES");
	}
	if (host_renderer_vulkan)
	{
		image_used = vk_image;
		image_used_size = vk_image_size;
	}
	else
	{
		image_used = image;
		image_used_size = image_size;
	}
	{
		/* where the chosen image's pointers are, in case its address was taken
		(host_loader.c): each image has its own table */
		size_t relocations_size = 0;
		void *relocations = SDL_LoadFile(image_used == vk_image ? "halo_guest_vk.relocs" : "halo_guest.relocs",
			&relocations_size);
		int loaded = host_load_image_reserved(image_used, image_used_size, relocations, relocations_size, image_base);

		SDL_free(relocations);
		if (loaded != 0)
			host_fatal("cannot load the game image; see logcat (tag \"halo\") for details");
	}
	SDL_free(image);
	SDL_free(vk_image);

	if (settings.vk_probe[0])
	{
		/* the probe owns the window and ends the app; the game does not start */
		host_logf(HOST_LOG_INFO, "running the Vulkan probe (%s) instead of the game", settings.vk_probe);
		host_vk_probe_run(settings.vk_probe, data_root, settings.vk_driver);
	}
	if (settings.sample_seconds[0])
		host_debug_start_sampler(settings.sample_seconds);
	host_debug_start_profiler(settings.profile_hz, data_root);
	host_gl_statistics = settings.gpu_stats;
	host_vk_present_marker = settings.vk_present_marker;
	host_vk_self_test = settings.vk_self_test;
	boot = make_boot(&environment);
	host_logf(HOST_LOG_INFO, "data %s, saves %s", data_root, save_root);
	host_run_guest_main(boot);
}

int main(int argc, char *argv[])
{
	(void)argc;
	(void)argv;
	host_logf(HOST_LOG_INFO, "Halo for Android starting");
	host_install_signal_handlers();

	if (host_native_thread_create(game_main, NULL, MAIN_STACK_SIZE) != 0)
		host_fatal("cannot start the game thread");
	/* the game ends the process itself (host_exit) */
	for (;;)
		pause();
}
