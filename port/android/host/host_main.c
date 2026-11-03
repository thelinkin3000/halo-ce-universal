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
reads); the host reads its debugging settings (host_common.c), and this
file debug.profile_hz, for the profiler, and debug.vk_probe, which runs the
Vulkan probe (host_vk_probe.c) instead of the game, on the driver that
display.vk_driver names (host_vk_driver.c). It also reads display.renderer, to
choose between the two game images (halo_guest.elf, drawing with OpenGL ES,
and halo_guest_vk.elf, with Vulkan: port/android/VULKAN.md), and
debug.vk_validation for the Vulkan renderer. The log is logcat's, tag
"halo".

debug.profile_run records a profile of each run into a folder of its own,
which the launcher makes (Download/halo_profiles/DDMMYYYY_HHMMSS, with the
build and the phone in run.txt: LauncherActivity.java) and names in
profile_folder.txt; without it, one is made in the data folder's
halo_profiles. The log goes there too (halo.log: the host's lines and the
game's, host_syscall.c), this run's part of the game's debug.txt (copied as
it grows, from where it ended when the run began: the file keeps every
run's), and the CPU profiler
(profile.bin: host_debug.c, at debug.profile_hz, 500 if that is 0) and the
Vulkan renderer's GPU times (gpu.txt: host_vk_profile.c) write there.

debug.halo_log (true by default) writes the log to halo.log in the data
folder too, begun again each start, as the Switch's is beside its program
and the PS5's in /data/halo: from when
config.toml is read, so the lines before it (the memory's reservation, the
start) are logcat's only.
*/

#include "host.h"
#include "tomlc17.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <android/log.h>
#include <jni.h>
#include <ftw.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sys/system_properties.h>
#include <sys/uio.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* ---------- the log: logcat */

int host_renderer_vulkan;

static void log_to_logcat(int priority, const char *line)
{
	__android_log_write(priority, "halo", line);
}

/* ---------- the log's files: a profiled run's (debug.profile_run) and the data folder's (debug.halo_log) */

static char profile_folder[512];
/* their halo.log, written as logcat is (write: the sink may be called from a signal handler, host_debug.c) */
static int profile_log = -1;
static int data_log = -1;
static struct timespec log_started;

static void log_to_file(int descriptor, const char *prefix, const char *line)
{
	struct iovec parts[3];

	parts[0].iov_base = (void *)prefix;
	parts[0].iov_len = strlen(prefix);
	parts[1].iov_base = (void *)line;
	parts[1].iov_len = strlen(line);
	parts[2].iov_base = (void *)"\n";
	parts[2].iov_len = 1;
	writev(descriptor, parts, 3);
}

static void log_to_logcat_and_files(int priority, const char *line)
{
	static const char levels[] = "??VDIWEF";
	struct timespec now;
	char prefix[32];
	double seconds;

	__android_log_write(priority, "halo", line);
	clock_gettime(CLOCK_MONOTONIC, &now);
	seconds = (double)(now.tv_sec - log_started.tv_sec) + (double)(now.tv_nsec - log_started.tv_nsec) / 1e9;
	snprintf(prefix, sizeof(prefix), "%8.2f %c ", seconds, priority >= 0 && priority < 8 ? levels[priority] : '?');
	if (profile_log >= 0)
		log_to_file(profile_log, prefix, line);
	if (data_log >= 0)
		log_to_file(data_log, prefix, line);
}

/* the data folder's halo.log, begun again (debug.halo_log) */
static void data_log_begin(const char *data_root_path)
{
	char path[600];

	snprintf(path, sizeof(path), "%s/halo.log", data_root_path);
	data_log = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0664);
	if (data_log < 0)
	{
		host_logf(HOST_LOG_WARN, "cannot write %s (%s); the log is logcat's only", path, strerror(errno));
		return;
	}
	host_log_set_sink(log_to_logcat_and_files);
	host_logf(HOST_LOG_INFO, "the log goes to %s too (debug.halo_log)", path);
}

/* every folder along path made */
static void make_folders(const char *path)
{
	char partial[512];
	size_t index;

	snprintf(partial, sizeof(partial), "%s", path);
	for (index = 1; partial[index]; index++)
	{
		if (partial[index] == '/')
		{
			partial[index] = 0;
			mkdir(partial, 0775);
			partial[index] = '/';
		}
	}
	mkdir(partial, 0775);
}

static int folder_writable(const char *path)
{
	struct stat information;

	return stat(path, &information) == 0 && S_ISDIR(information.st_mode) && access(path, W_OK) == 0;
}

/* this run's part of the game's debug.txt (the data folder's, which every run adds to): what it adds past where it
ended as the run began, copied into the profile folder's debug.txt every DEBUG_COPY_SECONDS, so that a run that is
killed loses no more than that */
#define DEBUG_COPY_SECONDS 2

static char debug_source[600], debug_copy[600];
static long debug_copied;

static void debug_copy_new(void)
{
	FILE *source = fopen(debug_source, "rb");
	FILE *copy;
	char buffer[16384];
	long size;
	size_t got;

	if (!source)
		return;
	fseek(source, 0, SEEK_END);
	size = ftell(source);
	/* (a file begun again: from its start) */
	if (size < debug_copied)
		debug_copied = 0;
	if (size == debug_copied)
	{
		fclose(source);
		return;
	}
	copy = fopen(debug_copy, "ab");
	if (copy)
	{
		fseek(source, debug_copied, SEEK_SET);
		while (debug_copied < size && (got = fread(buffer, 1, sizeof(buffer), source)) > 0)
		{
			fwrite(buffer, 1, got, copy);
			debug_copied += (long)got;
		}
		fclose(copy);
	}
	fclose(source);
}

static void *debug_copier(void *unused)
{
	(void)unused;
	for (;;)
	{
		sleep(DEBUG_COPY_SECONDS);
		debug_copy_new();
	}
	return NULL;
}

static void debug_copy_begin(const char *data_root_path)
{
	struct stat information;
	pthread_t thread;

	snprintf(debug_source, sizeof(debug_source), "%s/debug.txt", data_root_path);
	snprintf(debug_copy, sizeof(debug_copy), "%s/debug.txt", profile_folder);
	debug_copied = stat(debug_source, &information) == 0 ? (long)information.st_size : 0;
	if (pthread_create(&thread, NULL, debug_copier, NULL) == 0)
		pthread_detach(thread);
}

/* the run's profile folder, its log, and what the profilers need: the launcher's folder, or one in the data folder */
static void profile_run_begin(const char *data_root_path, int *profile_hz)
{
	char path[600];
	FILE *named;

	snprintf(path, sizeof(path), "%s/profile_folder.txt", data_root_path);
	named = fopen(path, "r");
	if (named)
	{
		if (fgets(profile_folder, sizeof(profile_folder), named))
			profile_folder[strcspn(profile_folder, "\r\n")] = 0;
		fclose(named);
	}
	if (!profile_folder[0] || !folder_writable(profile_folder))
	{
		time_t now = time(NULL);
		struct tm local;
		char stamp[32];

		if (profile_folder[0])
			host_logf(HOST_LOG_WARN, "profile: cannot write to %s (%s); the profile goes in the data folder",
				profile_folder, strerror(errno));
		localtime_r(&now, &local);
		strftime(stamp, sizeof(stamp), "%d%m%Y_%H%M%S", &local);
		snprintf(profile_folder, sizeof(profile_folder), "%s/halo_profiles/%s", data_root_path, stamp);
		make_folders(profile_folder);
		if (!folder_writable(profile_folder))
		{
			host_logf(HOST_LOG_ERROR, "profile: cannot make %s; no profile", profile_folder);
			profile_folder[0] = 0;
			return;
		}
	}
	snprintf(path, sizeof(path), "%s/halo.log", profile_folder);
	profile_log = open(path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0664);
	if (profile_log >= 0)
		host_log_set_sink(log_to_logcat_and_files);
	snprintf(host_vk_profile_folder, sizeof(host_vk_profile_folder), "%s", profile_folder);
	debug_copy_begin(data_root_path);
	if (*profile_hz <= 0)
		*profile_hz = 500;
	host_logf(HOST_LOG_INFO, "profile: this run's profile goes to %s (debug.profile_run)", profile_folder);
}

/* what the run's renderer is, added to the launcher's run.txt */
static void profile_run_describe(const char *renderer_line)
{
	char path[600], value[PROP_VALUE_MAX];
	FILE *file;

	if (!profile_folder[0])
		return;
	snprintf(path, sizeof(path), "%s/run.txt", profile_folder);
	file = fopen(path, "a");
	if (!file)
		return;
	fprintf(file, "renderer: %s\n", renderer_line);
	value[0] = 0;
	__system_property_get("ro.soc.model", value);
	if (value[0])
		fprintf(file, "soc: %s\n", value);
	value[0] = 0;
	__system_property_get("ro.hardware", value);
	if (value[0])
		fprintf(file, "hardware: %s\n", value);
	fclose(file);
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

/* ---------- the system's gesture edges */

/* asks the activity for Android's system gesture insets via JNI
(HaloActivity.getSystemGestureInsetsPixels); the guest calls it at every
finger down because the insets change when the phone rotates; the method is
found on the activity's own class because the game's native thread has the
system's class loader, which does not know the app's classes; a change is
logged so the log shows the value the game works with; all 0 on any failure
(the touch controls then use the whole screen) */
void host_gesture_insets(int *insets)
{
	static int logged[4] = { -1, -1, -1, -1 };
	JNIEnv *env = (JNIEnv *)SDL_GetAndroidJNIEnv();
	jobject activity = (jobject)SDL_GetAndroidActivity();
	jint values[4] = { 0, 0, 0, 0 };
	int index;

	if (env && activity)
	{
		jclass activity_class = (*env)->GetObjectClass(env, activity);
		jmethodID method = activity_class ? (*env)->GetMethodID(env, activity_class, "getSystemGestureInsetsPixels", "()[I") : NULL;
		jintArray array = method ? (jintArray)(*env)->CallObjectMethod(env, activity, method) : NULL;

		if (array && !(*env)->ExceptionCheck(env) && (*env)->GetArrayLength(env, array) == 4)
			(*env)->GetIntArrayRegion(env, array, 0, 4, values);
		/* a Java exception left pending would break the next JNI call of the
		thread, and a failed read must leave zeros */
		if ((*env)->ExceptionCheck(env))
		{
			(*env)->ExceptionClear(env);
			for (index = 0; index < 4; index++)
				values[index] = 0;
		}
		if (array)
			(*env)->DeleteLocalRef(env, array);
		if (activity_class)
			(*env)->DeleteLocalRef(env, activity_class);
		(*env)->DeleteLocalRef(env, activity);
	}
	for (index = 0; index < 4; index++)
		insets[index] = (int)values[index];
	if (memcmp(logged, insets, sizeof(logged)))
	{
		memcpy(logged, insets, sizeof(logged));
		host_logf(HOST_LOG_INFO, "system gesture insets %d,%d,%d,%d", insets[0], insets[1], insets[2], insets[3]);
	}
}

/* ---------- the Theater's EXPORT and IMPORT, the Mods screen's IMPORT and Video
Setup's IMPORT DRIVER */

/* HaloActivity.nativeLog: how an import or export went, into the log (logcat and halo.log) as well as its Toast */
JNIEXPORT void JNICALL Java_com_halo_decomp_HaloActivity_nativeLog(JNIEnv *env, jclass cls, jstring text)
{
	const char *line = text ? (*env)->GetStringUTFChars(env, text, NULL) : NULL;

	(void)cls;
	if (!line)
		return;
	host_log(HOST_LOG_INFO, line);
	(*env)->ReleaseStringUTFChars(env, text, line);
}

/* calls the activity's method (a String argument, the replays' or the
mods' folder), found on the activity's class as host_gesture_insets' method
is */
static void host_android_folder_call(const char *method_name, const char *folder)
{
	JNIEnv *env = (JNIEnv *)SDL_GetAndroidJNIEnv();
	jobject activity = (jobject)SDL_GetAndroidActivity();

	if (!env || !activity || !folder)
	{
		host_logf(HOST_LOG_WARN, "%s: no activity", method_name);
		return;
	}
	{
		jclass activity_class = (*env)->GetObjectClass(env, activity);
		jmethodID method = activity_class ?
			(*env)->GetMethodID(env, activity_class, method_name, "(Ljava/lang/String;)V") : NULL;
		jstring path = method ? (*env)->NewStringUTF(env, folder) : NULL;

		if (path)
			(*env)->CallVoidMethod(env, activity, method, path);
		if ((*env)->ExceptionCheck(env))
		{
			(*env)->ExceptionClear(env);
			host_logf(HOST_LOG_WARN, "%s: the activity's method failed", method_name);
		}
		else
			host_logf(HOST_LOG_INFO, "%s: %s", method_name, folder);
		if (path)
			(*env)->DeleteLocalRef(env, path);
		if (activity_class)
			(*env)->DeleteLocalRef(env, activity_class);
	}
	(*env)->DeleteLocalRef(env, activity);
}

/* asks the activity to copy the replays in folder to a folder the player
chooses (HaloActivity.exportReplays: the system's folder picker, then the
copy on a thread of its own) */
void host_android_export_replays(const char *folder)
{
	host_android_folder_call("exportReplays", folder);
}

/* asks the activity to copy replays the player chooses into folder
(HaloActivity.importReplays: the system's file picker, then the copy on a
thread of its own) */
void host_android_import_replays(const char *folder)
{
	host_android_folder_call("importReplays", folder);
}

/* asks the activity to take the mods in .zip files the player chooses into
folder, a folder each (HaloActivity.importMods: the system's file picker,
then the copy on a thread of its own) */
void host_android_import_mods(const char *folder)
{
	host_android_folder_call("importMods", folder);
}

/* asks the activity to copy the Vulkan driver archives the player chooses
into folder, freedreno_drivers (HaloActivity.importVkDrivers: the system's
file picker, then the copy on a thread of its own) */
void host_android_import_vk_drivers(const char *folder)
{
	host_android_folder_call("importVkDrivers", folder);
}

/* asks the activity to copy the maps the player chooses (.map files, and
those in .zip files) into folder, custom_maps (HaloActivity.importMaps: the
system's file picker, then the copy on a thread of its own) */
void host_android_import_maps(const char *folder)
{
	host_android_folder_call("importMaps", folder);
}

/* the settings of [debug] that this file reads, from config.toml */
struct host_settings
{
	int profile_hz;
	int profile_run; /* debug.profile_run */
	int halo_log; /* debug.halo_log */
	int gpu_split_parts; /* debug.gpu_split_parts */
	int original_multisample; /* debug.original_multisample */
	char vk_probe[128]; /* empty: the game runs */
	char vk_driver[256]; /* display.vk_driver: empty (the default) for the phone's own driver, or an archive */
	char renderer[32]; /* display.renderer: "vulkan" (the default, also when empty) or "gl" */
	int vk_validation; /* debug.vk_validation */
	int gpu_stats; /* debug.gpu_stats */
	int vk_present_marker; /* debug.vk_present_marker */
	int vk_render_thread; /* debug.vk_render_thread */
	int vk_self_test; /* debug.vk_self_test */
	/* debug.window_base: the window's address, where it will go there (for
	trying saved games across windows); 0 for the usual placement */
	uint32_t window_base;
	/* debug.image_base: the same for the guest image (its pointers are moved
	as where the Java runtime holds its linked address) */
	uint32_t image_base;
};

static void config_read(const char *path, struct host_settings *settings)
{
	toml_result_t result = toml_parse_file_ex(path);

	memset(settings, 0, sizeof(*settings));
	/* the defaults of port_config.c's rows, for a file not written yet (the first start) or without them */
	snprintf(settings->renderer, sizeof(settings->renderer), "vulkan");
	settings->halo_log = 1;
	settings->vk_render_thread = 1;
	if (!result.ok)
	{
		host_logf(HOST_LOG_WARN, "cannot read %s: %s; the host's settings are the defaults", path, result.errmsg);
		return;
	}
	{
		toml_datum_t hz = toml_seek(result.toptab, "debug.profile_hz");
		toml_datum_t probe = toml_seek(result.toptab, "debug.vk_probe");
		toml_datum_t driver = toml_seek(result.toptab, "display.vk_driver");
		toml_datum_t renderer = toml_seek(result.toptab, "display.renderer");
		toml_datum_t validation = toml_seek(result.toptab, "debug.vk_validation");
		toml_datum_t statistics = toml_seek(result.toptab, "debug.gpu_stats");
		toml_datum_t marker = toml_seek(result.toptab, "debug.vk_present_marker");
		toml_datum_t render_thread = toml_seek(result.toptab, "debug.vk_render_thread");
		toml_datum_t self_test = toml_seek(result.toptab, "debug.vk_self_test");
		toml_datum_t profile_run = toml_seek(result.toptab, "debug.profile_run");
		toml_datum_t halo_log = toml_seek(result.toptab, "debug.halo_log");
		toml_datum_t log_files = toml_seek(result.toptab, "debug.log_files");
		toml_datum_t split = toml_seek(result.toptab, "debug.gpu_split_parts");
		toml_datum_t multisample = toml_seek(result.toptab, "debug.original_multisample");
		toml_datum_t window = toml_seek(result.toptab, "debug.window_base");
		toml_datum_t image = toml_seek(result.toptab, "debug.image_base");

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
		/* (true unless false) */
		settings->vk_render_thread = !(render_thread.type == TOML_BOOLEAN && !render_thread.u.boolean);
		settings->vk_self_test = self_test.type == TOML_BOOLEAN && self_test.u.boolean;
		settings->profile_run = profile_run.type == TOML_BOOLEAN && profile_run.u.boolean;
		/* (true unless false; a file from before it, which the game has not
		completed yet, says with debug.log_files, as the Switch's host does) */
		if (halo_log.type != TOML_BOOLEAN)
			halo_log = log_files;
		settings->halo_log = !(halo_log.type == TOML_BOOLEAN && !halo_log.u.boolean);
		settings->gpu_split_parts = split.type == TOML_BOOLEAN && split.u.boolean;
		settings->original_multisample = multisample.type == TOML_BOOLEAN && multisample.u.boolean;
		if (window.type == TOML_INT64 && window.u.int64 > 0 && window.u.int64 < 0x100000000LL)
			settings->window_base = (uint32_t)window.u.int64;
		if (image.type == TOML_INT64 && image.u.int64 > 0 && image.u.int64 < 0x100000000LL)
			settings->image_base = (uint32_t)image.u.int64;
	}
	toml_free(result);
}

/* Whether the app runs through an ARM translator. The x86 emulator runs the
app's ARM code through one, which ro.dalvik.vm.native.bridge names; it
cannot deliver the page faults of the write tracking to the app
(host_memory.c). */
static int native_bridge_active(void)
{
	char value[PROP_VALUE_MAX] = "";

	__system_property_get("ro.dalvik.vm.native.bridge", value);
	return value[0] && strcmp(value, "0") != 0;
}

/* ---------- main */

#define MAIN_STACK_SIZE (16 * 1024 * 1024)

/* the thread that runs the game: passes the display and its density, the
time zone and the data and save folders to the guest through its
environment, chooses the write tracking (page protection, or page hashes when
translated or when debug.memory_watch is false), and runs the guest's main;
never returns normally */
static void *game_main(void *unused)
{
	struct host_environment environment = { { 0 }, 0 };
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
	clock_gettime(CLOCK_MONOTONIC, &log_started);
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

	host_environment_set(&environment, "HOME", save_root);
	host_environment_set(&environment, "HALO_DATA_ROOT", data_root);
	host_environment_set(&environment, "HALO_SAVE_ROOT", save_root);
	host_time_zone(zone, sizeof(zone));
	host_environment_set(&environment, "TZ", zone);
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
	if (settings.halo_log)
		data_log_begin(data_root);
	if (settings.profile_run)
		profile_run_begin(data_root, &settings.profile_hz);
	host_vk_profile_split = settings.profile_run && settings.gpu_split_parts;
	host_vk_original_multisample = settings.original_multisample;
	/* (the benchmark's results go there too: port/linux/game/benchmark.c) */
	if (profile_folder[0])
		host_environment_set(&environment, "HALO_PROFILE_FOLDER", profile_folder);
	/* (before the image's imports are resolved: the GL ES image's calls are
	counted on the way, host_gl.c) */
	host_gl_statistics = settings.gpu_stats;
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
	host_memory_window_wanted = settings.window_base;
	host_memory_image_wanted = settings.image_base;
	if (!span)
		host_fatal("cannot load the game image; see logcat (tag \"halo\") for details");
	if (host_memory_initialize(HALO_GUEST_IMAGE_BASE, span, &image_base) != 0)
	{
		FILE *report;

		if (!host_memory_fixed_unavailable())
			host_fatal("cannot load the game image; see logcat (tag \"halo\") for details");

		snprintf(path, sizeof(path), "%s/memory_map.txt", data_root);
		report = fopen(path, "w");
		host_memory_report_low_mappings(report);
		if (report)
			fclose(report);
		host_fatal("The game cannot start: the memory it needs below 4 GB is taken by Android's "
			"Java runtime on this device.\n\nRestarting the device may help. Please report it with "
			"memory_map.txt from\n%s\n(or adb logcat -s halo).", data_root);
	}

	{
		/* the game renders 480 lines at the display's aspect ratio
		(landscape) unless display.screen_width says otherwise (d3d8_gl.c) */
		const SDL_DisplayMode *mode;
		char width[16];
		char refresh[16];

		SDL_InitSubSystem(SDL_INIT_VIDEO);
		mode = SDL_GetDesktopDisplayMode(SDL_GetPrimaryDisplay());
		if (mode && mode->w > 0 && mode->h > 0)
		{
			int longer = mode->w > mode->h ? mode->w : mode->h;
			int shorter = mode->w > mode->h ? mode->h : mode->w;

			snprintf(width, sizeof(width), "%d", (480 * longer / shorter) & ~1);
			host_environment_set(&environment, "HALO_DISPLAY_WIDTH", width);
			host_logf(HOST_LOG_INFO, "display %dx%d: rendering %sx480", mode->w, mode->h, width);
		}
		if (mode)
		{
			/* display.max_fps's "twice the refresh rate" (sdl_platform.c):
			the guest cannot ask SDL for the display mode */
			snprintf(refresh, sizeof(refresh), "%g", mode->refresh_rate > 0.0f ? mode->refresh_rate : 60.0f);
			host_environment_set(&environment, "HALO_DISPLAY_REFRESH", refresh);
		}
		{
			/* dp for the touch controls (port/linux/src/touch_input.c): SDL
			gives Android's densityDpi / 160 */
			float density = SDL_GetDisplayContentScale(SDL_GetPrimaryDisplay());
			char text[16];

			if (density > 0.0f)
			{
				snprintf(text, sizeof(text), "%g", density);
				host_environment_set(&environment, "HALO_DISPLAY_DENSITY", text);
				host_logf(HOST_LOG_INFO, "display density %s", text);
			}
		}
	}

	if (want_vulkan)
	{
		char line[600];

		host_renderer_vulkan = host_vk_startup(settings.vk_driver, settings.vk_validation, line, sizeof(line));
		host_logf(HOST_LOG_INFO, "renderer: %s", line);
		profile_run_describe(line);
	}
	else
	{
		/* one line either way: a reason found before Vulkan was tried goes in it */
		if (vulkan_refused[0])
			host_logf(HOST_LOG_WARN, "renderer: GL ES (Vulkan was asked for: %s)", vulkan_refused);
		else
			host_logf(HOST_LOG_INFO, "renderer: GL ES");
		profile_run_describe("GL ES (no GPU times: gpu.txt is the Vulkan renderer's)");
	}
	/* Vulkan asked for (not for the probe, which runs on the GL image) and
	not started: the GL ES image sets display.renderer to "gl" (d3d8_gl.c),
	so that Video Setup shows the backend the game runs on */
	if (!settings.vk_probe[0] && strcmp(settings.renderer, "gl") && !host_renderer_vulkan)
		host_environment_set(&environment, "HALO_VULKAN_REFUSED", "1");
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
	/* (the sampler and the write tracking: host_common.c) */
	host_apply_debug_settings(path, native_bridge_active() ? "ARM translation" : NULL);
	host_debug_start_profiler(settings.profile_hz, profile_folder[0] ? profile_folder : data_root);
	host_vk_present_marker = settings.vk_present_marker;
	host_vk_render_thread = settings.vk_render_thread;
	host_vk_self_test = settings.vk_self_test;
	boot = host_make_boot(&environment, "halo");
	host_logf(HOST_LOG_INFO, "data %s, saves %s", data_root, save_root);
	host_run_guest_main(boot);
}

/* Called when HaloApplication loads this library at the start of the
game's own process (":game"), before its Java side has allocated anything
large: ART's large object space, which on some devices covers the Xbox
window, is then idle there and can be reclaimed (host_memory.c). */
JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM *vm, void *reserved)
{
	(void)vm;
	(void)reserved;
	/* (before main: what the reservation finds goes to logcat too) */
	host_log_set_sink(log_to_logcat);
	host_memory_reserve_early();
	return JNI_VERSION_1_6;
}

int main(int argc, char *argv[])
{
	(void)argc;
	(void)argv;
	host_log_set_sink(log_to_logcat);
	host_logf(HOST_LOG_INFO, "Halo for Android starting");
	host_install_signal_handlers();
	host_signal_stack_install();
	if (host_native_thread_create(game_main, NULL, MAIN_STACK_SIZE) != 0)
		host_fatal("cannot start the game thread");
	/* the game ends the process itself (host_exit) */
	for (;;)
		pause();
}
