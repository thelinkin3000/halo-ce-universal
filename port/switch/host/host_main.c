/*
HOST_MAIN.C

Entry point of the Switch port: an ordinary homebrew program built with
devkitA64 that runs the Android port's guest image (port/android/README.md),
the game compiled as ILP32 AArch64 code.

It is the Android host library (port/android/host) with three files replaced:
this one (no JNI, no APK: the image and the game data are files on the SD
card, and the display is the console's), host_sdl2.c (SDL3's calls answered
by devkitPro's SDL2, whose Switch video driver puts an EGL surface on the
console's default nwindow, over Mesa) and the NDK's log functions (the
standard error stream, which the Homebrew Menu shows). See port/switch/README.md.

Paths, each overridable from the environment:
- HALO_GUEST_IMAGE: the guest image (default: halo_guest.elf next to the
  executable);
- HALO_DATA_ROOT: the folder that holds maps/ and config.toml (default: the
  executable's folder);
- HALO_SAVE_ROOT: the saved games (default: save/ in the data folder);
- HALO_DISPLAY_WIDTH: the columns of the 480-line picture (default: from the
  display mode SDL reports: 1280 in the handheld, 1920 docked).
*/

#include "host.h"

#include "xiso.h"
#include "host_ui.h"

#include <SDL2/SDL.h>
#include <errno.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <dirent.h>
#include <fcntl.h>
#include <malloc.h>
#include <pthread.h>
#include <unistd.h>

#include <switch.h>

#include "tomlc17.h"

void host_install_signal_handlers(void);

/* ---------- logging and termination

The Android host logs through the NDK's __android_log_* functions, which
this port does not have; these are the same names, defined here so the files
shared with that port need no changes. */

static const char *priority_name(int priority)
{
	switch (priority)
	{
	case HOST_LOG_WARN: return "W";
	case HOST_LOG_ERROR: return "E";
	case HOST_LOG_FATAL: return "F";
	default: return "I";
	}
}

/* The log file, as a descriptor.

The first attempt at logging on the Switch redirected stderr with freopen,
and produced an empty file: the file was created, so main() had run and the
redirect had worked, and nothing ever reached it. Rather than keep reasoning
about which layer of buffering lost the lines, the log is now written with
write() to a descriptor this opens itself, and the console copy is a separate
fprintf that cannot affect it. Every line goes to both, and each is flushed
at once, so the file is readable the moment a line is written.
*/
static int log_descriptor = -1;

/* The log is written from the main thread, from the game thread and from the
 * guest watcher, and they write through one descriptor that they all share.
 * Two write() calls on one descriptor interleave: the card driver keeps the
 * file position per descriptor, not per call, so two threads writing at once
 * can land on the same offset and one line overwrites part of the other. That
 * is not a theory here - it is in the logs, a heartbeat spliced into the
 * middle of another thread's line and lost.
 *
 * O_SYNC makes a line durable, which is a different problem and is solved
 * where the file is opened. This is the other one: one writer at a time. */
static pthread_mutex_t log_lock = PTHREAD_MUTEX_INITIALIZER;

/* The guest's file calls take the logger's lock around their driver calls,
 * so that only one thread is ever inside the SD card driver at a time. See
 * host.h. */
void host_sd_lock(void)
{
	pthread_mutex_lock(&log_lock);
}

void host_sd_unlock(void)
{
	pthread_mutex_unlock(&log_lock);
}

/* Seconds since the log was opened, for the front of every line.

 * The console copy of a line has a clock on it and the file copy never has,
 * which was fine until two runs of the same code produced logs of exactly the
 * same length ending at exactly the same place. There was then no way to tell
 * a fresh log from the previous one, and no way to tell where a run stopped in
 * time. The elapsed time also answers a question the heartbeat only samples:
 * how long between two lines is real. */
static struct timespec log_opened;

static double seconds_since_the_log_opened(void)
{
	struct timespec now;

	clock_gettime(CLOCK_MONOTONIC, &now);
	return (double)(now.tv_sec - log_opened.tv_sec) +
		(double)(now.tv_nsec - log_opened.tv_nsec) / 1000000000.0;
}

/* Lines below an error are written by a thread of their own. Written where
 * they were logged, each was a synchronous write to the card (the file is
 * O_SYNC) and, for warnings - every line of the guest's platform_log - an
 * fsync, on whatever thread logged it, usually the game's, and under the
 * lock every guest file call takes: a profile of a match had the game thread
 * in the log's writes 2.8% of the time. The logger now copies the line into
 * a buffer and goes on; the writer writes it a few milliseconds later. Errors
 * and worse are still written by the thread that logs them, after what is
 * buffered (so the order holds), and synced: the lines that explain a death
 * are the ones that must not wait. A buffer that fills is written by whoever
 * fills it. */
#define LOG_BUFFER_SIZE (256 * 1024)

static char log_buffer[LOG_BUFFER_SIZE];
static size_t log_buffered;
static pthread_mutex_t log_buffer_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t log_buffer_wake = PTHREAD_COND_INITIALIZER;
static int log_writer_started;

/* writes what is buffered (under log_lock, which the caller holds) */
static void log_drain_locked(void)
{
	static char out[LOG_BUFFER_SIZE];
	size_t size;

	pthread_mutex_lock(&log_buffer_lock);
	size = log_buffered;
	memcpy(out, log_buffer, size);
	log_buffered = 0;
	pthread_mutex_unlock(&log_buffer_lock);
	if (size && log_descriptor >= 0)
		(void)!write(log_descriptor, out, size);
	/* (and to stderr, which nxlink shows, as each line once was) */
	if (size)
	{
		fwrite(out, 1, size, stderr);
		fflush(stderr);
	}
}

static void *log_writer(void *unused)
{
	(void)unused;
	for (;;)
	{
		pthread_mutex_lock(&log_buffer_lock);
		while (!log_buffered)
			pthread_cond_wait(&log_buffer_wake, &log_buffer_lock);
		pthread_mutex_unlock(&log_buffer_lock);
		pthread_mutex_lock(&log_lock);
		log_drain_locked();
		pthread_mutex_unlock(&log_lock);
	}
	return NULL;
}

int __android_log_write(int priority, const char *tag, const char *text)
{
	char line[2048];
	int length;

	length = snprintf(line, sizeof(line), "%7.2f %s %s: %s\n",
		seconds_since_the_log_opened(), priority_name(priority), tag, text);
	if (length >= (int)sizeof(line))
		length = (int)sizeof(line) - 1;
	if (length > 0 && priority < HOST_LOG_ERROR && log_descriptor >= 0)
	{
		int queued = 0;

		pthread_mutex_lock(&log_buffer_lock);
		if (!log_writer_started)
		{
			pthread_t thread;

			log_writer_started = pthread_create(&thread, NULL, log_writer, NULL) == 0 ? 1 : -1;
			if (log_writer_started > 0)
				pthread_detach(thread);
		}
		if (log_writer_started > 0 && log_buffered + (size_t)length <= LOG_BUFFER_SIZE)
		{
			memcpy(log_buffer + log_buffered, line, (size_t)length);
			log_buffered += (size_t)length;
			pthread_cond_signal(&log_buffer_wake);
			queued = 1;
		}
		pthread_mutex_unlock(&log_buffer_lock);
		if (queued)
			return 1;
	}
	pthread_mutex_lock(&log_lock);
	if (log_descriptor >= 0 && length > 0)
	{
		log_drain_locked();
		(void)!write(log_descriptor, line, (size_t)length);
		/* fsync is reserved for lines that say something went wrong.
		*
		* An fsync per line - several hundred a second while the guest's
		* allocator churns - proved to be more than the card driver could
		* take: runs ended with the console itself locked up, the log cut
		* off mid-line and even the lock-free heartbeat file ending in
		* garbage, which is the signature of a card write that never came
		* back. O_SYNC alone still gets the line to the driver; the fsync
		* on errors and worse keeps the lines that explain a death
		* durable. */
		if (priority >= HOST_LOG_ERROR)
			(void)fsync(log_descriptor);
	}
	if (length > 0)
	{
		fwrite(line, 1, (size_t)length, stderr);
		fflush(stderr);
	}
	pthread_mutex_unlock(&log_lock);
	return 1;
}

/* a marker that cannot be lost: no formatting, no clock, one write, after
what is buffered */
static void log_marker(const char *text)
{
	pthread_mutex_lock(&log_lock);
	if (log_descriptor >= 0)
	{
		log_drain_locked();
		(void)!write(log_descriptor, text, strlen(text));
		(void)!write(log_descriptor, "\n", 1);
		(void)fsync(log_descriptor);
	}
	pthread_mutex_unlock(&log_lock);
}

/* A debugger gate used to stand here: the port would hold at the top of main
 * while a file was present on the card, so that gdb-multiarch had time to
 * attach to a process that otherwise died in seconds. It was taken out again
 * once it was clear the failures worth seeing are ones the port detects for
 * itself, which host_backtrace() covers from host_fatal(). */

int __android_log_vprint(int priority, const char *tag, const char *format, va_list arguments)
{
	char text[2048];

	vsnprintf(text, sizeof(text), format, arguments);
	return __android_log_write(priority, tag, text);
}

/* The one thing the disc image reader (xiso.c) uses from the game's platform
 * layer, which it declares rather than includes here - see the guard in that
 * file. It is the port's own logging with the guest's prefix on it, so the
 * two read the same in the log. */
void platform_log(const char *format, ...)
{
	va_list arguments;
	char text[2048];

	va_start(arguments, format);
	vsnprintf(text, sizeof(text), format, arguments);
	va_end(arguments);
	host_logf(HOST_LOG_INFO, "%s", text);
}

int __android_log_print(int priority, const char *tag, const char *format, ...)
{
	va_list arguments;
	int result;

	va_start(arguments, format);
	result = __android_log_vprint(priority, tag, format, arguments);
	va_end(arguments);
	return result;
}

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

/* The Android host shows a fatal error in an SDL message box. There is no
SDL window here and no message box driver for the Switch's SDL2 build, so
the message goes to the log the player can read over the Homebrew Menu's
console, and the program stops. */
void host_fatal(const char *format, ...)
{
	char message[1024];
	va_list arguments;

	va_start(arguments, format);
	vsnprintf(message, sizeof(message), format, arguments);
	va_end(arguments);
	__android_log_write(HOST_LOG_FATAL, "halo", message);
	/* The last thing said before the process goes, so it is the only chance
	 * to say how it got here. The console records faults but not exits, and
	 * this is an exit. */
	host_backtrace(message);
	_exit(1);
}

void host_abort(const char *reason)
{
	__android_log_print(HOST_LOG_FATAL, "halo", "guest abort: %s", reason);
	abort();
}

void host_exit(int code)
{
	host_logf(HOST_LOG_INFO, "the game exited (%d)", code);
	host_restore_clocks();
	fflush(stderr);
	_exit(code);
}

int host_errno(void)
{
	return errno;
}

/* ---------- paths */

static char executable_root[PATH_MAX];
static char data_root[PATH_MAX];
static char save_root[PATH_MAX];

void host_android_path(int which, char *buffer, uint32_t size)
{
	snprintf(buffer, size, "%s", which ? save_root : data_root);
}

/* Homebrew runs from the SD card root, so the executable sits in a
directory the player put it in; unlike Linux there is no /proc/self/exe to
ask, and the Switch's SDL2 has no filesystem (--disable-filesystem), so the
working directory that libnx set up is what the data is found next to. */
/* The SD card, named outright.

The obvious getcwd is not dependable here. The memory probe wrote its report
to a bare relative name and produced no file at all; libnx 4.x moved the cwd
handling that used to place a program's working directory beside its
executable, and the working directory depends on how the player launched the
thing. LittleGPTracker's Switch port takes the same view - it never uses a
relative path and names the device directly:

	freopen("sdmc:/switch/lgpt/lgpt.log", "w", stdout);

so the card is mounted by the time main() runs and only the prefix was ever
missing. The root is therefore an absolute one, and the player can move the
game anywhere by setting HALO_DATA_ROOT. */
static void find_executable_root(void)
{
	strcpy(executable_root, "sdmc:/switch/halo");
}

/* the folder the guest image is loaded from (the updater replaces it there) */
const char *host_executable_root(void)
{
	return executable_root;
}

/* the data root, where config.toml and the game's maps are. The deko3d
renderer's shader cache lives under it (host_dk_shaders.c). */
const char *host_data_root(void)
{
	return data_root;
}

static struct timespec extraction_started;

static int directory_has_maps(const char *root)
{
	char path[PATH_MAX + 32];
	struct stat information;

	snprintf(path, sizeof(path), "%s/maps/ui.map", root);
	return stat(path, &information) == 0;
}

/* The disc image to unpack, if there is one beside the port.
 *
 * A fixed name first, because that is the one the README tells people to use
 * and the one that keeps working when the card holds several images; then any
 * image in the folder, so that someone who copied one over under its own name
 * does not have to rename it. The console has no file browser, so this cannot
 * ask - which is why the names are fixed and the search is in one directory. */
static const char *find_disc_image(const char *root)
{
	static const char *const names[] = { "halo.iso", "halo.xiso", "maps.iso", "halo-xbox.iso" };
	char path[PATH_MAX + 32];
	struct stat information;
	unsigned index;

	for (index = 0; index < sizeof(names) / sizeof(*names); index++)
	{
		snprintf(path, sizeof(path), "%s/%s", root, names[index]);
		if (stat(path, &information) == 0)
			return names[index];
	}
	/* anything else that looks like an image, in whatever order the card
	 * gives them back */
	{
		static char found[256];
		DIR *directory = opendir(root);
		struct dirent *entry;

		if (!directory)
			return NULL;
		while ((entry = readdir(directory)) != NULL)
		{
			const char *dot = strrchr(entry->d_name, '.');
			size_t length = strlen(entry->d_name);

			if (!dot || length < 5 || strlen(dot) != 4 ||
				(strcasecmp(dot, ".iso") != 0 && strcasecmp(dot, ".bin") != 0))
				continue;
			snprintf(found, sizeof(found), "%s", entry->d_name);
			closedir(directory);
			return found;
		}
		closedir(directory);
	}
	return NULL;
}

/* Progress as the copy goes, which for a game-sized folder on a memory card is
 * minutes rather than seconds. Saying nothing for that long looks like a hang,
 * so it says every time the file being copied changes and every megabyte or so
 * within it. */
static unsigned long long extraction_last_done;
static const char *extraction_last_file;
static unsigned long long extraction_total;
static unsigned extraction_seconds;
/* the image's name, and whether the host's screens are showing the unpack:
the updater has gone by now and SDL has not come yet, so the screen is free,
and was left black for the minutes this takes */
static const char *extraction_image;
static int extraction_on_screen;
static unsigned long long extraction_drawn;

/* The updater's screens, before the game starts. Internet games used to be
chosen here too, from a menu of the host's own; the game's menus have that
now, so the host only asks when there is something to update. */
static void offer_an_update(void)
{
	PadState pad;

	padInitializeDefault(&pad);
	if (!host_ui_open())
	{
		host_logf(HOST_LOG_ERROR, "update: the screens could not be set up; starting the game");
		host_ui_close();
		return;
	}
	host_update_offer(&pad);
	/* the window goes back to SDL, for the game */
	host_ui_close();
}

static void draw_extraction(const char *file, unsigned long long done, unsigned long long total)
{
	char message[256];

	if (!extraction_on_screen)
		return;
	snprintf(message, sizeof(message), "Copying %s out of %s. This only happens once.",
		file ? file : "the game's maps", extraction_image);
	host_ui_progress("UNPACKING THE GAME", message, (long long)done, (long long)total, NULL);
	extraction_drawn = done;
}

static void extraction_progress(void *context, const char *file, unsigned long long done, unsigned long long total)
{
	(void)context;
	extraction_total = total;
	/* the screen at each file and every few megabytes: often enough to move,
	seldom enough not to slow the copy */
	if (file != extraction_last_file || done - extraction_drawn >= 4 * 1024 * 1024)
		draw_extraction(file, done, total);
	if (file != extraction_last_file)
	{
		extraction_last_file = file;
		extraction_last_done = done;
		host_logf(HOST_LOG_INFO, "unpacking %s", file);
		return;
	}
	if (done - extraction_last_done < 16 * 1024 * 1024)
		return;
	extraction_last_done = done;
	/* seconds since it started, so a stall reads as a stall rather than as a
	 * percentage that never moves */
	{
		struct timespec now;
		struct timespec started = extraction_started;
		double elapsed;

		clock_gettime(CLOCK_MONOTONIC, &now);
		elapsed = (double)(now.tv_sec - started.tv_sec) + (double)(now.tv_nsec - started.tv_nsec) / 1e9;
		host_logf(HOST_LOG_INFO, "  %llu of %llu MB (%llu s)", done / (1024 * 1024),
			total / (1024 * 1024), (unsigned long long)elapsed);
		extraction_seconds = (unsigned)elapsed;
	}
}

/* Make sure the game data is there, unpacking a disc image if that is how it
 * arrived.
 *
 * Both routes stay: a maps folder that is already in place is used as it is,
 * and an .iso beside the port is unpacked into one. The second is here because
 * the first asks for a gigabyte and a half of somebody else's files, and most
 * people who want to run this have the disc image instead. */
static void ensure_game_data(const char *root)
{
	char image[PATH_MAX + 32];
	char error[512];
	const char *name;

	if (directory_has_maps(root))
		return;
	name = find_disc_image(root);
	if (!name)
		host_fatal("The Halo game data was not found: %s/maps/ui.map is missing, and no disc image is "
			"beside the port. Either put the maps folder of an Xbox disc image at %s/maps, or put the "
			"image itself at %s/halo.iso and the port will unpack it.", root, root, root);
	snprintf(image, sizeof(image), "%s/%s", root, name);
	host_logf(HOST_LOG_INFO, "no maps folder; unpacking %s into %s/maps", name, root);
	extraction_image = name;
	extraction_on_screen = host_ui_open();
	draw_extraction(NULL, 0, 0);
	clock_gettime(CLOCK_MONOTONIC, &extraction_started);
	if (!xiso_extract_maps(image, root, extraction_progress, NULL, error, sizeof(error)))
	{
		/* said on the screen too, until a button is pressed, rather than the
		program closing on a black screen */
		if (extraction_on_screen)
		{
			char message[768];
			PadState pad;

			snprintf(message, sizeof(message), "%s could not be unpacked: %.400s", name, error);
			host_ui_message("UNPACKING FAILED", message,
				(const char *const[]){ "A", "Close", NULL });
			padInitializeDefault(&pad);
			while (appletMainLoop())
			{
				padUpdate(&pad);
				if (padGetButtonsDown(&pad) & (HidNpadButton_A | HidNpadButton_Plus))
					break;
				svcSleepThread(16666667);
			}
			host_ui_close();
		}
		host_fatal("could not unpack %s: %s", name, error);
	}
	if (extraction_on_screen)
		host_ui_close();
	host_logf(HOST_LOG_INFO, "unpacked %llu MB from %s in %u s", extraction_total / (1024 * 1024), name,
		extraction_seconds);
	if (!directory_has_maps(root))
		host_fatal("%s was unpacked but %s/maps/ui.map is still not there", name, root);
}

/* The columns of the 480-line picture for the display's shape. SDL2's
console's own answer, which is 1280x720 in the handheld and 1920x1080
docked, and which is not always SDL's: the Switch video driver reports
1920x1080 whichever it is (devkitPro/SDL src/video/switch/
SDL_switchvideo.c hardcodes that as the desktop mode). The game renders
480 lines unless display.screen_width in config.toml says otherwise
(d3d8_gl.c). */
static int display_width(void)
{
	/* Asked of the console, not of SDL.
	 *
	 * SDL's Switch video driver reports 1920x1080 as the desktop mode
	 * whatever the console actually is: devkitPro's SDL_switchvideo.c
	 * hardcodes that as the desktop and offers 1280x720 as a second mode,
	 * and never asks appletGetOperationMode(). So a Switch Lite, which is
	 * always 1280x720, is reported as 1920x1080.
	 *
	 * It happens not to matter for the number this returns, because both
	 * are 16:9 and the game renders 480 lines either way - 852 columns
	 * from either. It matters for the window that gets created, which is
	 * why host_sdl2.c asks SDL to keep the window resizable so that it
	 * follows the console being docked or not. */
	AppletOperationMode mode = appletGetOperationMode();
	int width = mode == AppletOperationMode_Handheld ? 1280 : 1920;
	int height = mode == AppletOperationMode_Handheld ? 720 : 1080;
	int longer = width > height ? width : height;
	int shorter = width > height ? height : width;

	return (480 * longer / shorter) & ~1;
}

/* newlib's <sys/unistd.h> declares getpagesize but its library does not
define it. The console's page size is fixed, and the guest's own memory
management assumes the same 4 KB. */
int getpagesize(void)
{
	return 4096;
}

/* The main thread parks here once the game thread is started, because the
game ends the process itself (host_exit). An idle loop is enough: there is
nothing to wait for and nothing to wake it. */
int pause(void)
{
	for (;;)
		svcSleepThread(UINT64_MAX);
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

/* passes the host's HALO_ variables on to the guest: the settings the game
also takes from the environment (port/linux/src/port_config.c) */
static void environment_copy_halo(struct environment *environment)
{
	extern char **environ;
	char **entry;

	for (entry = environ; *entry; entry++)
	{
		const char *equals = strchr(*entry, '=');
		char name[128];

		if (strncmp(*entry, "HALO_", 5) || !equals || (size_t)(equals - *entry) >= sizeof(name))
			continue;
		memcpy(name, *entry, (size_t)(equals - *entry));
		name[equals - *entry] = 0;
		environment_set(environment, name, equals + 1);
	}
}

/* the game draws with deko3d (host.h) */
int host_renderer_deko3d = 1;

/* the profiler's rate from config.toml: 0 unless debug.profiler is true,
then debug.profile_hz, or 500 if that is not set */
static unsigned config_profile_hz(const char *path)
{
	toml_result_t result = toml_parse_file_ex(path);
	unsigned hz = 0;

	if (!result.ok)
		return 0;
	{
		toml_datum_t enabled = toml_seek(result.toptab, "debug.profiler");
		toml_datum_t value = toml_seek(result.toptab, "debug.profile_hz");

		if (enabled.type == TOML_BOOLEAN && enabled.u.boolean)
			hz = value.type == TOML_INT64 && value.u.int64 > 0 ? (unsigned)value.u.int64 : 500;
	}
	toml_free(result);
	return hz;
}

/* debug.sample_seconds from config.toml, as text for the sampler, or 0 */
static int config_sample_seconds(const char *path, char *text, size_t size)
{
	toml_result_t result = toml_parse_file_ex(path);
	int found = 0;

	if (!result.ok)
		return 0;
	{
		toml_datum_t seconds = toml_seek(result.toptab, "debug.sample_seconds");
		double value = seconds.type == TOML_FP64 ? seconds.u.fp64 :
			seconds.type == TOML_INT64 ? (double)seconds.u.int64 : 0.0;

		if (value > 0.0)
		{
			snprintf(text, size, "%g", value);
			found = 1;
		}
	}
	toml_free(result);
	return found;
}

/* POSIX TZ for the current local offset (the guest's musl has no zone
database). newlib's struct tm has no tm_gmtoff, as Linux's has, so the
offset is found by putting the same instant through both localtime_r and
gmtime_r and keeping the difference in their fields. */
static void time_zone(char *buffer, size_t size)
{
	time_t now = time(NULL);
	struct tm local, greenwich;
	long offset;

	localtime_r(&now, &local);
	gmtime_r(&now, &greenwich);
	offset = (long)local.tm_hour - greenwich.tm_hour;
	/* the day may differ, which is the offset crossing noon or midnight */
	if (local.tm_yday != greenwich.tm_yday)
	{
		if (local.tm_yday > greenwich.tm_yday ||
			(greenwich.tm_yday == 0 && local.tm_yday == 365))
			offset += 24;
		else
			offset -= 24;
	}
	offset *= 60;
	snprintf(buffer, size, "<L>%s%ld:%02ld", offset < 0 ? "-" : "", labs(offset) / 60, labs(offset) % 60);
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
	/* (the Switch's image is always where it was linked) */
	boot->image_shift = 0;
	return (uint32_t)(uintptr_t)boot;
}

static void *read_file(const char *path, size_t *size)
{
	FILE *file = fopen(path, "rb");
	void *data = NULL;
	long length;

	if (!file)
		return NULL;
	if (fseek(file, 0, SEEK_END) == 0 && (length = ftell(file)) > 0 && fseek(file, 0, SEEK_SET) == 0)
	{
		data = malloc((size_t)length);
		if (data && fread(data, 1, (size_t)length, file) != (size_t)length)
		{
			free(data);
			data = NULL;
		}
		*size = (size_t)length;
	}
	fclose(file);
	return data;
}

/* ---------- main */

#define MAIN_STACK_SIZE (16 * 1024 * 1024)

/* How much can the window's address actually take?
 *
 * The guest reserves the contiguous window itself and demands the exact
 * address (port/linux/src/xbox_memory.c), so the port cannot move it. What
 * it can do is find out what that address will hold, and the answer has
 * been the last thing unexplained: 128 MB is refused at 0x80000000 while
 * the same size maps without trouble a few hundred MB lower, and 16 MB
 * mapped there in the memory probe. So this walks the size down until one
 * is accepted and reports the largest.
 *
 * It runs before the guest starts, costs a handful of mappings that are
 * handed straight back, and turns a refusal into a number.
 */
static void probe_window_capacity(void)
{
	static const size_t megabytes[] = { 128, 96, 64, 32, 16 };
	size_t index;

	host_logf(HOST_LOG_INFO, "how much can the window's address at %08x take?",
		HALO_GUEST_WINDOW_BASE);
	for (index = 0; index < sizeof(megabytes) / sizeof(*megabytes); index++)
	{
		size_t length = megabytes[index] * 1024 * 1024;
		void *backing = memalign(0x1000, length);
		Result result;

		if (!backing)
			break;
		result = (Result)svcMapMemory((void *)(uintptr_t)HALO_GUEST_WINDOW_BASE, backing, length);
		if (R_SUCCEEDED(result))
		{
			host_logf(HOST_LOG_INFO, "  %3zu MB: yes", megabytes[index]);
			svcUnmapMemory((void *)(uintptr_t)HALO_GUEST_WINDOW_BASE, backing, length);
		}
		else
			host_logf(HOST_LOG_INFO, "  %3zu MB: no (0x%08x)", megabytes[index], (unsigned)result);
		free(backing);
	}
}

static void *game_main(void *unused)
{
	struct environment environment = { { 0 }, 0 };
	const char *setting;
	char zone[64];
	char path[PATH_MAX + 32];
	char width[16];
	size_t image_size = 0;
	void *image;
	uint32_t boot;

	(void)unused;
	log_marker("marker: game thread running");
	setting = getenv("HALO_DATA_ROOT");
	snprintf(data_root, sizeof(data_root), "%s", setting && *setting ? setting : executable_root);
	setting = getenv("HALO_SAVE_ROOT");
	if (setting && *setting)
		snprintf(save_root, sizeof(save_root), "%s", setting);
	else
		snprintf(save_root, sizeof(save_root), "%s/save", data_root);
	mkdir(save_root, 0755);
	log_marker("marker: data paths resolved");
	ensure_game_data(data_root);

	environment_copy_halo(&environment);
	environment_set(&environment, "HOME", save_root);
	environment_set(&environment, "HALO_DATA_ROOT", data_root);
	environment_set(&environment, "HALO_SAVE_ROOT", save_root);
	setting = getenv("HALO_DISPLAY_WIDTH");
	if (setting && *setting)
		snprintf(width, sizeof(width), "%s", setting);
	else
		snprintf(width, sizeof(width), "%d", display_width());
	environment_set(&environment, "HALO_DISPLAY_WIDTH", width);
	host_logf(HOST_LOG_INFO, "rendering %sx480", width);
	time_zone(zone, sizeof(zone));
	environment_set(&environment, "TZ", zone);

	setting = getenv("HALO_GUEST_IMAGE");
	if (setting && *setting)
		snprintf(path, sizeof(path), "%s", setting);
	else
		snprintf(path, sizeof(path), "%s/halo_guest.elf", executable_root);
	host_logf(HOST_LOG_INFO, "renderer: deko3d (%s)", path);
	image = read_file(path, &image_size);
	probe_window_capacity();
	log_marker("marker: reading the guest image");
	if (!image)
		host_fatal("cannot read the game image %s: %s", path, strerror(errno));
	log_marker("marker: loading the guest image into memory");
	if (host_load_image(image, image_size) != 0)
		host_fatal("cannot load the game image %s", path);
	log_marker("marker: guest image loaded");
	/* Prove the whole image is there, and not just its code.
	 *
	 * The guest faults writing twelve bytes at 0x4051652c, which is in the
	 * image's data. The code-memory check in host_mman.c does not cover
	 * that: it only walks the 2.8 MB range that was made executable, which
	 * ends at 0x402BB000, some 2.5 MB short of where the fault is. So it
	 * reported everything fine and had said nothing about the address that
	 * matters.
	 *
	 * This walks the image end to end, writes a pattern to every page and
	 * puts back what was there, and names the first few pages that do not
	 * keep it. A page that cannot hold a byte the host itself wrote is a
	 * page the guest will fault on the first time it writes one. */
	{
		size_t offset;
		int bad = 0;
		unsigned char *bytes = (unsigned char *)(uintptr_t)host_memory_image_base();

		for (offset = 0; offset < image_size; offset += 4096)
		{
			unsigned char before = bytes[offset];

			bytes[offset] = (unsigned char)(before ^ 0x5a);
			if (bytes[offset] != (unsigned char)(before ^ 0x5a))
			{
				if (bad < 8)
					host_logf(HOST_LOG_ERROR,
						"image page %p (%zu bytes in) did not hold what was written to it",
						(void *)(bytes + offset), offset);
				bad++;
			}
			bytes[offset] = before;
		}
		host_logf(bad ? HOST_LOG_ERROR : HOST_LOG_INFO,
			"image check: %zu bytes at %p, %d of %zu pages unusable",
			image_size, (void *)bytes, bad, (image_size + 4095) / 4096);
	}
	free(image);

	snprintf(path, sizeof(path), "%s/config.toml", data_root);
	{
		char seconds[32];

		if (config_sample_seconds(path, seconds, sizeof(seconds)))
			host_debug_start_sampler(seconds);
		host_debug_start_profiler(config_profile_hz(path), "halo_guest.elf");
	}
	log_marker("marker: building the guest's boot structure");
	boot = make_boot(&environment);
	/* The guest's contiguous window is reserved by the guest itself, and it
	 * demands the exact address it was given here. Its own log says it is
	 * reserving at 0, which means it did not get the value - so this is
	 * what was handed over, to say which side is losing it. */
	host_logf(HOST_LOG_INFO, "telling the guest its contiguous window is at %08x (host has %08x)",
		((const struct halo_guest_boot *)boot)->contiguous_base, host_memory_window_base());
	/* The boot structure is ordinary guest memory, and the guest's heap
	 * grows through the same allocator that placed it. If something has
	 * overwritten it between here and the guest reading it, the guest gets
	 * zeros - which is what it reported. So the structure is printed in
	 * full, here and again after the guest has been running a moment, and
	 * the difference between the two is the answer. */
	{
		const uint32_t *words = (const uint32_t *)(uintptr_t)boot;

		host_logf(HOST_LOG_INFO, "  boot structure at %08x: %08x %08x %08x %08x %08x",
			boot, words[0], words[1], words[2], words[3], words[4]);
	}
	host_logf(HOST_LOG_INFO, "data %s, saves %s", data_root, save_root);
	log_marker("marker: handing control to the guest");
	/* already on the stack the host mapped below 4 GB: every thread that runs
	 * guest code is moved onto its own as it starts (host_thread.c) */
	host_logf(HOST_LOG_INFO, "entering the guest on a stack at %p, below 4 GB", __builtin_frame_address(0));
	host_run_guest_main(boot);
}


/* Hold at the top of main, so that a debugger has time to arrive.
 *
 * Attaching to a running process is a race, and this port wins it: main() can
 * sit here doing nothing for two minutes, which is far longer than it takes to
 * notice a process, attach to it and set a breakpoint. The gate that used to
 * be at the last moment before the guest runs is gone - the port used to die
 * before reaching it, and it does not now - but the same idea at the top of
 * main is what makes a debugger usable at all.
 *
 * It is armed by a file on the card, so a run with no debugger in it is
 * unaffected: with nothing at sdmc:/switch/halo/attach, this returns at once.
 *
 * It also **ends itself**, which is the part that matters. Launching this port
 * kills the FTP server - it is another homebrew, and the launcher reaps those
 * - so anything that needed a file deleted while the port was running could
 * not be relied on. So the gate waits a fixed time and then removes its own
 * trigger: the next run is an ordinary run, and no file has to be touched
 * during this one. That makes the whole sequence possible with only a push
 * beforehand and nothing at all afterwards. */
#define ATTACH_GATE_SECONDS 45

static void wait_at_the_top_of_main(void)
{
	char path[PATH_MAX + 32];
	int waited;

	snprintf(path, sizeof(path), "%s/attach", executable_root);
	/* TEMPORARY, for the debugging push, and to be put back: the gate used
	 * to be armed by a file on the card so that ordinary runs were not
	 * delayed. It is armed unconditionally now because arming it needs the
	 * FTP server, and launching this port kills the FTP server, so a file
	 * could not be put there at the moment it was needed. Holding for 45 s
	 * on every run is not a cost worth paying once the debugging is over -
	 * put the access() test back when it is. */
	(void)access(path, F_OK);
	host_logf(HOST_LOG_INFO,
		"holding at the top of main for %d s, then carrying on by itself",
		ATTACH_GATE_SECONDS);
	for (waited = 0; waited < ATTACH_GATE_SECONDS; waited++)
	{
		if (access(path, F_OK) != 0)
		{
			host_logf(HOST_LOG_INFO, "released after %d s", waited);
			return;
		}
		if ((waited % 10) == 0)
			host_logf(HOST_LOG_INFO, "  still held (%d s)", waited);
		sleep(1);
	}
	host_logf(HOST_LOG_INFO, "carrying on after %d s", ATTACH_GATE_SECONDS);
}

int main(int argc, char *argv[])
{
	(void)argc;
	(void)argv;
	/* before any thread exists: libnx places their stacks at random, and one
	in the guest image's range keeps the image from loading (host_memory.c) */
	host_memory_hold_image_range();
	/* The log goes to a file on the card, as well as to the standard
	error stream.

	The file is the point. Whether a launcher shows a program's console is
	the launcher's business and differs between them: the Homebrew Menu has
	a console toggle, Sphaira documents no equivalent, and a port that can
	only be debugged through whichever menu happens to be installed is a
	port that is hard to debug. A file can be read back over the card, over
	FTP, or by whatever means the player already has, and it survives the
	program's exit - which matters most of all when it crashed.

	LittleGPTracker's Switch port opens its log the same way, and the memory
	probe established that writing to sdmc:/ works on this console. */
	/* Send everything to a nxlink host on the network, as well as to the
	 * console and to the file on the card.
	 *
	 * The third of those is not enough on its own, and the reason is worth
	 * writing down. Launching this port kills the FTP server - it is
	 * another homebrew and the launcher reaps those - so the log cannot be
	 * fetched until the console is restarted again, and by then the run is
	 * over. Everything said about a crash has to have been recorded before
	 * it happened, and watching it happen is better than reading it after.
	 *
	 * A connection refused means nothing is listening, which is the normal
	 * case and not an error: the port then says as much on the console and
	 * in the file as it ever did.
	 *
	 * socketInitializeDefault() first, and not as an optional extra: the
	 * sockets have to exist before anything can connect over them. The first
	 * version of this called nxlinkConnectToHost without it, which uploaded
	 * the program perfectly and then captured nothing at all - the port ran,
	 * nxlink attached, and every line went somewhere the host side never
	 * saw. That is the whole of why the previous attempts produced no output
	 * despite deploying correctly. */
	socketInitializeDefault();
	if (nxlinkConnectToHost(false, true) >= 0)
		host_logf(HOST_LOG_INFO, "output is going to a nxlink host as well as here");
	else
		host_logf(HOST_LOG_INFO, "no nxlink host; output is going to the console and the card only");

	setvbuf(stderr, NULL, _IOLBF, 0);
	find_executable_root();
	{
		char log_path[PATH_MAX + 32];

		snprintf(log_path, sizeof(log_path), "%s/halo.log", executable_root);
		/* O_SYNC, and not for tidiness.

		The log is written with write(), which is unbuffered as far as the
		port is concerned - but the card's driver buffers a file opened for
		writing, so "written" means "in the driver's hands". When the process
		dies without unwinding, that buffer goes with it, and the lines that
		explain the death are the ones that are lost. That is exactly the
		case the log exists for. O_SYNC makes each line reach the card
		before the next thing happens; the cost is a card write per line,
		which is nothing next to a game frame.

		The garbled fragments that appeared in earlier logs - a line of one
		message spliced into the middle of another - are the same buffer
		seen from the other side. */
		log_descriptor = open(log_path, O_WRONLY | O_CREAT | O_TRUNC | O_SYNC, 0666);
		if (log_descriptor < 0)
		{
			/* Not fatal: the log is worth having even buffered, and
			 * failing to open it is not a reason to stop a game. */
			log_descriptor = open(log_path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
		}
		/* Said once, because it is the number the guest would have been
		 * handed before host_syscall.c gave the guest a descriptor table
		 * of its own, and the two numbering schemes are worth being able
		 * to compare when a guest string turns up in this file.
		 *
		 * Moving it out of the way was tried here - F_DUPFD_CLOEXEC to a
		 * high number, so no open() could hand it out - and broke logging
		 * completely: the log came back empty, because the port died
		 * before writing its first line.
		 *
		 * The cause is worth remembering. newlib does not number its fcntl
		 * commands the way Linux does: in sys/_default_fcntl.h
		 * F_DUPFD_CLOEXEC is 14, where Linux has 1030. The code compiled
		 * happily, because the constant came from newlib's own header, and
		 * then asked newlib for whatever command 14 is to it. Anything
		 * here that reaches for a Linux fcntl number has to take it from
		 * newlib's headers instead, and be suspicious of a value that
		 * looks like it should be 1000-odd.
		 *
		 * The descriptor table in host_syscall.c is the real answer to a
		 * guest reaching this file; this was a belt to braces that cost
		 * more than it was worth. */
		/* The clock starts here, before anything is written, or the first
		 * few lines are stamped with the time since an arbitrary epoch -
		 * which is what made the "the log is descriptor" line read
		 * 2601817344.10. */
		clock_gettime(CLOCK_MONOTONIC, &log_opened);
		host_logf(HOST_LOG_INFO, "the log is descriptor %d", log_descriptor);
		wait_at_the_top_of_main();
	}
	/* the markers below say how far the start-up got, in order, because
	the alternative to knowing where it stopped is guessing */
	log_marker("marker: main() entered");
	log_marker("marker: log file opened");
	host_logf(HOST_LOG_INFO, "Halo for Switch starting (%s)", executable_root);
	/* the game image an update installed last time, before anything loads it */
	host_update_finish();
	host_memory_log_regions();
	/* Ask for more memory than the console's default for a homebrew
	 * process.
	 *
	 * The game needs a 16 MB thread stack, a 9 MB image and a 128 MB
	 * contiguous window - some 153 MB of guest address space below 4 GB,
	 * which is its own constraint and is a separate matter. What this
	 * concerns is how much of it the process is allowed to map at all,
	 * and the default on recent firmware is well under what the port
	 * needs: svcMapMemory was answering ENOMEM for the first 16 MB the
	 * game thread asked for.
	 *
	 * pglBoostSystemMemoryResourceLimit raises the system's limit, and is
	 * what a game would use for the same reason. It takes a size to add.
	 * Whether it succeeds depends on the firmware and on what the other
	 * applets are holding, so the result is logged either way rather than
	 * treated as something the port can rely on. */
	{
		Result boosted = pglBoostSystemMemoryResourceLimit(256ULL * 1024 * 1024);

		host_logf(R_SUCCEEDED(boosted) ? HOST_LOG_INFO : HOST_LOG_WARN,
			"asked for 256 MB more memory for the process: %s (0x%08x)",
			R_SUCCEEDED(boosted) ? "granted" : "refused", (unsigned)boosted);
	}

	host_install_signal_handlers();

	/* libnx: the console's pad, focus and applet state. The Homebrew Menu
	still runs this program in the background and takes focus back when it
	is opened. */
	log_marker("marker: before libnx init");
	appletInitialize();
	romfsInit();
	padConfigureInput(1, HidNpadStyleSet_NpadStandard);
	log_marker("marker: libnx init done");

	/* a newer release of the port, if there is one; on the default window,
	before the game thread's SDL takes it */
	offer_an_update();

	{
		/* The game thread's stack is guest memory, so it has to be below
		4 GB, and it is asked for at decreasing sizes until one is granted.
		That is not only a convenience: the console refused to raise this
		process's memory allowance, so there may be little to spare, and
		the first run that reaches here is what establishes how much. Each
		attempt is logged, so the largest stack that works is visible in the
		log rather than guessed at.
		 *
		A stack is one of the few places the port can safely economise: the
		guest's own musl gives its threads far less, and the game's main
		thread is not the one that recurses deepest - that is the renderer.
		*/
		static const size_t sizes[] = {
			MAIN_STACK_SIZE, 8 * 1024 * 1024, 4 * 1024 * 1024, 2 * 1024 * 1024,
			1024 * 1024, 512 * 1024,
		};
		size_t index;
		int error = EAGAIN;

		for (index = 0; index < sizeof(sizes) / sizeof(*sizes); index++)
		{
			host_logf(HOST_LOG_INFO, "asking for a %zu byte game thread stack", sizes[index]);
			error = host_native_thread_create(game_main, NULL, sizes[index]);
			if (!error)
				break;
			host_logf(HOST_LOG_WARN, "  refused: %s (%d)", strerror(error), error);
		}
		if (error)
			host_fatal("cannot start the game thread: %s (%d)", strerror(error), error);
		host_logf(HOST_LOG_INFO, "the game thread has a %zu byte stack", sizes[index]);
	}
	/* the game ends the process itself (host_exit) */
	for (;;)
		pause();
}