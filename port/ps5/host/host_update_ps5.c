/*
HOST_UPDATE_PS5.C

The PS5 title's self-updater (port/ps5/PS5.md, phase 8), the Switch's
(port/switch/host/host_update.c) on the console: a release's build, made by
GitHub Actions for a pushed tag v<number>, knows its build number
(HALO_BUILD_NUMBER, which names its release: build-<number>), and while the
game runs it asks GitHub for the latest release, on a thread of its own. If
that is newer it is downloaded and installed, and the player is told by a
notification to close the game and start it again. Builds made anywhere
else have no number and never look, and config.toml's update.auto turns the
look off. Nothing asks first: the console has no dialog the host can answer
from the pad (its message boxes are notifications), and the user asked for
the updates to be automatic.

The check is GitHub's answer for the latest release (its "tag_name"), and
the download its slipstreamce-ps5-<release|debug>.zip, or its
halo-ps5-<release|debug>.zip if GitHub answers 404 for that one (the name the
zips have until they take the new one), both through posix_update.c (HTTPS
with Mbed TLS, redirects followed, the certificate and the host's name always
checked) into the data folder. The console has
no certificate store a title can read, so the title carries the roots
GitHub's hosts chain to (certificates.pem in its folder), which
posix_update.c takes through SSL_CERT_FILE when no system bundle exists.

The archive is stored, not deflated (the release job zips the PS5's with
zip -0, as the Switch's), so it is read entry by entry with no inflater:
each entry under PPSA99500/ is written into the title's folder as
<path>.new and checked against its CRC (host_rules_ps5.c reads the headers
and the names). Only when every one has arrived intact are they renamed
into place: a stamp (update.txt) first, eboot.bin last, the stamp removed
after. Nothing of the title is read again once the guest is loaded (the
host, the guest image and libc.prx are in memory, param.json is the
system's at launch), so the session goes on on the old build until the
player closes it; a title cannot start itself again, and the new host and
the new guest must start together. A start that finds the stamp (a session
closed during the renames) puts the rest in place before the guest loads
(ps5_update_recover); .new files without one are a download that did not
finish, and go. What the updater writes is 0777, files and folders, as the
console's FTP server writes a title's: the console does not start an
eboot.bin without the execute bit, and the FTP server can still replace
them by hand.

The guest's own updater (port/linux/src/updater.c, compiled into the
desktop guest) is built for the PS5 with no number, so it never looks, and
the host_update_* imports below answer it "unavailable", as a build without
it.
*/

#include "host.h"
#include "ps5_host.h"
#include "host_rules_ps5.h"
#include "tomlc17.h"
#include "update.h"

#include <dirent.h>
#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* (given for this file by the build: tools/ps5_build.py) */
#ifndef HALO_BUILD_NUMBER
#define HALO_BUILD_NUMBER 0
#endif
#ifndef HALO_BUILD_FLAVOR
#define HALO_BUILD_FLAVOR "release"
#endif

#define UPDATE_REPOSITORY "thelinkin3000/SlipstreamCE"
#define UPDATE_ASSET "slipstreamce-ps5-" HALO_BUILD_FLAVOR ".zip"
#define UPDATE_OLD_ASSET "halo-ps5-" HALO_BUILD_FLAVOR ".zip"
#define UPDATE_LATEST "https://api.github.com/repos/" UPDATE_REPOSITORY "/releases/latest"
/* the archive's folder that is the title's */
#define UPDATE_TITLE_ENTRY "PPSA99500/"
#define UPDATE_ANSWER PS5_DATA_FOLDER "/update.latest.json"
#define UPDATE_ARCHIVE PS5_DATA_FOLDER "/update.partial.zip"
/* the most of each download taken (update.h), as the game's own updater
has them (port/linux/src/updater.c): the release's answer, and the archive
(52 MB at build 106) */
#define UPDATE_MAXIMUM_ANSWER (1024ULL * 1024)
#define UPDATE_MAXIMUM_ARCHIVE (256ULL * 1024 * 1024)
#define UPDATE_STAMP PS5_TITLE_FOLDER "/update.txt"
#define UPDATE_CERTIFICATES PS5_TITLE_FOLDER "/certificates.pem"
/* the program, put in place last */
#define UPDATE_PROGRAM "eboot.bin"
#define UPDATE_IMAGE "halo_guest.elf"

enum
{
	/* the title's files an archive may hold (it has nine) */
	MAXIMUM_FILES = 64,
	/* the game's start is left alone this long before the check */
	CHECK_AFTER_SECONDS = 10,
	PATH_SIZE = 512,
};

struct installed
{
	char paths[MAXIMUM_FILES][PATH_SIZE];
	int count;
};

/* ---------- where the files go */

/* a folder and the folders above it under the title's folder, 0777 */
static int make_folders(const char *path)
{
	char partial[PATH_SIZE];
	size_t index, length = strlen(path);

	if (length >= sizeof(partial))
		return 0;
	memcpy(partial, path, length + 1);
	for (index = strlen(PS5_TITLE_FOLDER) + 1; index <= length; index++)
	{
		if (partial[index] != '/' && partial[index])
			continue;
		partial[index] = 0;
		if (mkdir(partial, 0777) == 0)
			chmod(partial, 0777);
		else if (errno != EEXIST)
			return 0;
		partial[index] = path[index];
	}
	return 1;
}

static int has_suffix(const char *text, const char *suffix)
{
	size_t length = strlen(text), suffix_length = strlen(suffix);

	return length >= suffix_length && !strcmp(text + length - suffix_length, suffix);
}

/* each <path>.new under folder, in found, at most MAXIMUM_FILES */
static void find_new_files(const char *folder, int depth, struct installed *found)
{
	DIR *directory;
	struct dirent *entry;

	if (depth > 3 || !(directory = opendir(folder)))
		return;
	while ((entry = readdir(directory)))
	{
		char path[PATH_SIZE];

		if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..") ||
			(size_t)snprintf(path, sizeof(path), "%s/%s", folder, entry->d_name) >= sizeof(path))
			continue;
		if (entry->d_type == DT_DIR)
			find_new_files(path, depth + 1, found);
		else if (has_suffix(path, ".new") && found->count < MAXIMUM_FILES)
			snprintf(found->paths[found->count++], PATH_SIZE, "%.*s", (int)(strlen(path) - 4), path);
	}
	closedir(directory);
}

/* <path>.new in place of <path>, for each, the program last; how many were
not put in place */
static int put_in_place(const struct installed *files)
{
	int pass, index, failed = 0;

	for (pass = 0; pass < 2; pass++)
	{
		for (index = 0; index < files->count; index++)
		{
			char temporary[PATH_SIZE + 4];
			int program = has_suffix(files->paths[index], "/" UPDATE_PROGRAM);

			if (program != pass)
				continue;
			snprintf(temporary, sizeof(temporary), "%s.new", files->paths[index]);
			if (rename(temporary, files->paths[index]) != 0)
			{
				ps5_log("update: cannot put %s in place (%s); it is still %s", files->paths[index], strerror(errno),
					temporary);
				failed++;
			}
		}
	}
	return failed;
}

static void remove_new_files(const struct installed *files)
{
	int index;

	for (index = 0; index < files->count; index++)
	{
		char temporary[PATH_SIZE + 4];

		snprintf(temporary, sizeof(temporary), "%s.new", files->paths[index]);
		unlink(temporary);
	}
}

/* ---------- a session closed halfway */

void ps5_update_recover(void)
{
	struct installed *found = calloc(1, sizeof(*found));
	char stamp[32] = "";
	int index, program = 0;
	FILE *file;

	/* (the console has no store of certificates: the title's own roots, for
	posix_update.c; set here, at the start, before the game's thread and
	the network's read the environment: setenv is not safe beside them) */
	setenv("SSL_CERT_FILE", UPDATE_CERTIFICATES, 1);
	if (!found)
		return;
	find_new_files(PS5_TITLE_FOLDER, 0, found);
	file = fopen(UPDATE_STAMP, "r");
	if (file)
	{
		if (!fgets(stamp, sizeof(stamp), file))
			stamp[0] = 0;
		fclose(file);
		stamp[strcspn(stamp, "\r\n")] = 0;
	}
	if (!file)
	{
		/* a download that did not finish: its files go */
		if (found->count)
		{
			remove_new_files(found);
			ps5_log("update: %d files of an update that did not finish removed", found->count);
		}
		free(found);
		return;
	}
	for (index = 0; index < found->count; index++)
		program |= has_suffix(found->paths[index], "/" UPDATE_PROGRAM);
	index = put_in_place(found);
	unlink(UPDATE_STAMP);
	ps5_log("update: the install of build %s was finished at this start: %d files put in place, %d not", stamp,
		found->count - index, index);
	free(found);
	if (program)
	{
		/* this program is the old one, and the game image beside it now the
		new one's: the new build has to start instead */
		ps5_notify("SlipstreamCE: the update to build %s is installed; start the game again", stamp);
		ps5_log_sync(1);
		ps5_log("the host ends: the new build starts at the next start");
		_exit(0);
	}
}

/* ---------- the check and the download */

/* update.auto in config.toml; true if it is not there */
static int automatic_updates(void)
{
	toml_result_t result = toml_parse_file_ex(PS5_DATA_FOLDER "/config.toml");
	int value = 1;

	if (!result.ok)
		return 1;
	{
		toml_datum_t setting = toml_seek(result.toptab, "update.auto");

		if (setting.type == TOML_BOOLEAN)
			value = setting.u.boolean;
	}
	toml_free(result);
	return value;
}

/* the latest release's build number, 0 if it cannot be learned */
static long latest_build(void)
{
	char error[512] = "";
	char *text;
	long size, build;
	FILE *file;

	if (!update_download(UPDATE_LATEST, UPDATE_ANSWER, UPDATE_MAXIMUM_ANSWER, NULL, NULL, error, sizeof(error)))
	{
		ps5_log("update: the latest release cannot be asked for: %s", error);
		return 0;
	}
	file = fopen(UPDATE_ANSWER, "rb");
	text = NULL;
	if (file && fseek(file, 0, SEEK_END) == 0 && (size = ftell(file)) > 0 && size < 1024 * 1024 &&
		fseek(file, 0, SEEK_SET) == 0 && (text = calloc(1, (size_t)size + 1)) &&
		fread(text, 1, (size_t)size, file) != (size_t)size)
	{
		free(text);
		text = NULL;
	}
	if (file)
		fclose(file);
	unlink(UPDATE_ANSWER);
	build = ps5_update_release_build(text);
	if (!build)
		ps5_log("update: GitHub's answer for the latest release names no build");
	free(text);
	return build;
}

struct progress
{
	long build;
	int quarter;
};

static void download_progress(void *context, unsigned long long received, unsigned long long total)
{
	struct progress *progress = context;
	int quarter = total ? (int)(received * 4 / total) : 0;

	if (quarter > progress->quarter && quarter < 4)
	{
		progress->quarter = quarter;
		ps5_notify("SlipstreamCE: downloading build %ld: %d %%", progress->build, quarter * 25);
		ps5_log("update: %llu of %llu bytes", received, total);
	}
}

/* ---------- the install */

static int fail(char *error, size_t size, const char *format, ...) __attribute__((format(printf, 3, 4)));

static int fail(char *error, size_t size, const char *format, ...)
{
	va_list arguments;

	va_start(arguments, format);
	vsnprintf(error, size, format, arguments);
	va_end(arguments);
	return 0;
}

/* the entry's data, size bytes of archive, into path (its CRC checked) */
static int copy_entry(FILE *archive, const struct ps5_zip_entry *entry, const char *path, char *error, size_t size)
{
	static unsigned char buffer[256 * 1024];
	unsigned long left = entry->compressed, crc = 0;
	int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0777);

	if (fd < 0)
		return fail(error, size, "cannot write %s (%s)", path, strerror(errno));
	/* (0777, as the FTP server writes a title's files: the console does
	not start an eboot.bin, or load its libc.prx, without the execute bit;
	an update written 0666 installed and then would not start) */
	fchmod(fd, 0777);
	while (left)
	{
		size_t take = left < sizeof(buffer) ? left : sizeof(buffer);

		if (fread(buffer, 1, take, archive) != take)
		{
			close(fd);
			return fail(error, size, "the archive ends inside %s", path);
		}
		if (write(fd, buffer, take) != (ssize_t)take)
		{
			close(fd);
			return fail(error, size, "writing %s failed (%s)", path, strerror(errno));
		}
		crc = ps5_crc32(crc, buffer, take);
		left -= take;
	}
	if (close(fd) != 0)
		return fail(error, size, "writing %s failed (%s)", path, strerror(errno));
	if (crc != entry->crc)
		return fail(error, size, "%s did not arrive intact", path);
	return 1;
}

/* every entry of the title's folder as <path>.new, each one listed in files
as it is written; 1 when all arrived intact */
static int unpack(struct installed *files, char *error, size_t size)
{
	FILE *archive = fopen(UPDATE_ARCHIVE, "rb");
	int program = 0, image = 0, result = 0;

	if (!archive)
		return fail(error, size, "the archive cannot be read (%s)", strerror(errno));
	for (;;)
	{
		unsigned char header[PS5_ZIP_LOCAL_HEADER];
		char name[PATH_SIZE], relative[PATH_SIZE], path[PATH_SIZE + 8];
		struct ps5_zip_entry entry;
		int kind, which;

		if (fread(header, 1, sizeof(header), archive) != sizeof(header))
		{
			fail(error, size, "the archive ends before its directory");
			break;
		}
		kind = ps5_zip_local_header(header, &entry);
		if (kind == 0)
		{
			result = program && image ? 1 : fail(error, size, "the archive has no " UPDATE_TITLE_ENTRY "%s",
				program ? UPDATE_IMAGE : UPDATE_PROGRAM);
			break;
		}
		if (kind < 0 || entry.name_length >= sizeof(name) ||
			fread(name, 1, entry.name_length, archive) != entry.name_length ||
			fseek(archive, (long)entry.extra_length, SEEK_CUR) != 0)
		{
			fail(error, size, "the archive is not one the console can read");
			break;
		}
		name[entry.name_length] = 0;
		which = ps5_update_entry_path(name, UPDATE_TITLE_ENTRY, relative, sizeof(relative));
		if (which < 0)
		{
			fail(error, size, "the archive names %s, which the title refuses", name);
			break;
		}
		if (which == 0 || !relative[0])
		{
			/* (the release's other files, and the title's folder itself) */
			if (fseek(archive, (long)entry.compressed, SEEK_CUR) != 0)
			{
				fail(error, size, "the archive ends inside %s", name);
				break;
			}
			continue;
		}
		snprintf(path, sizeof(path), PS5_TITLE_FOLDER "/%s", relative);
		if (has_suffix(path, "/"))
		{
			path[strlen(path) - 1] = 0;
			if (!make_folders(path))
			{
				fail(error, size, "cannot make %s (%s)", path, strerror(errno));
				break;
			}
			continue;
		}
		if (!ps5_zip_entry_stored(&entry))
		{
			fail(error, size, "%s is packed in a way the console cannot unpack", name);
			break;
		}
		if (files->count == MAXIMUM_FILES)
		{
			fail(error, size, "the archive has more files than a title's");
			break;
		}
		*strrchr(path, '/') = 0;
		if (!make_folders(path))
		{
			fail(error, size, "cannot make %s (%s)", path, strerror(errno));
			break;
		}
		snprintf(files->paths[files->count], PATH_SIZE, PS5_TITLE_FOLDER "/%s", relative);
		snprintf(path, sizeof(path), "%s.new", files->paths[files->count]);
		files->count++;
		if (!copy_entry(archive, &entry, path, error, size))
			break;
		program |= !strcmp(relative, UPDATE_PROGRAM);
		image |= !strcmp(relative, UPDATE_IMAGE);
	}
	fclose(archive);
	return result;
}

static int install(long build, char *error, size_t size)
{
	struct installed *files = calloc(1, sizeof(*files));
	int failed;
	FILE *stamp;

	if (!files)
		return fail(error, size, "out of memory");
	if (!unpack(files, error, size))
	{
		remove_new_files(files);
		free(files);
		return 0;
	}
	stamp = fopen(UPDATE_STAMP, "w");
	if (!stamp || fprintf(stamp, "%ld\n", build) < 0 || fclose(stamp) != 0)
	{
		remove_new_files(files);
		free(files);
		return fail(error, size, "cannot write " UPDATE_STAMP " (%s)", strerror(errno));
	}
	chmod(UPDATE_STAMP, 0666);
	failed = put_in_place(files);
	if (!failed)
		unlink(UPDATE_STAMP);
	ps5_log("update: %d files put in place, %d not", files->count - failed, failed);
	free(files);
	return failed ? fail(error, size, "%d files could not be put in place; the next start tries again", failed) : 1;
}

/* ---------- the thread */

static void *update_thread(void *argument)
{
	char error[512] = "";
	struct progress progress = { 0, 0 };
	uint64_t start;
	long build;

	(void)argument;
	sleep(CHECK_AFTER_SECONDS);
	start = ps5_now_ns();
	build = latest_build();
	if (build <= HALO_BUILD_NUMBER)
	{
		ps5_log("update: build %d is the latest%s (%.1f s)", HALO_BUILD_NUMBER, build ? "" : " that could be checked",
			(ps5_now_ns() - start) / 1e9);
		return NULL;
	}
	ps5_log("update: build %ld is available (this is build %d)", build, HALO_BUILD_NUMBER);
	ps5_notify("SlipstreamCE: downloading build %ld", build);
	progress.build = build;
	{
		static const char *const assets[] = { UPDATE_ASSET, UPDATE_OLD_ASSET };
		char url[256];
		int asset, answered = 0, downloaded = 0;

		start = ps5_now_ns();
		for (asset = 0; asset < 2 && !downloaded; asset++)
		{
			snprintf(url, sizeof(url), "https://github.com/" UPDATE_REPOSITORY "/releases/download/build-%ld/%s", build,
				assets[asset]);
			ps5_log("update: downloading %s", url);
			downloaded = update_download_status(url, UPDATE_ARCHIVE, UPDATE_MAXIMUM_ARCHIVE, download_progress, &progress,
				&answered, error, sizeof(error));
			if (!downloaded && answered != 404)
				break;
		}
		if (!downloaded)
		{
			unlink(UPDATE_ARCHIVE);
			ps5_log("update: the download of %s did not finish: %s", url, error);
			ps5_notify("SlipstreamCE: the update did not finish: %s", error);
			return NULL;
		}
		ps5_log("update: downloaded in %.1f s", (ps5_now_ns() - start) / 1e9);
	}
	start = ps5_now_ns();
	if (!install(build, error, sizeof(error)))
	{
		unlink(UPDATE_ARCHIVE);
		ps5_log("update: the install did not finish: %s", error);
		ps5_notify("SlipstreamCE: the update did not finish: %s", error);
		return NULL;
	}
	unlink(UPDATE_ARCHIVE);
	ps5_log("update: build %ld is installed (%.1f s); it starts at the next start", build,
		(ps5_now_ns() - start) / 1e9);
	ps5_notify("SlipstreamCE: build %ld is installed: close the game and start it again to use it", build);
	return NULL;
}

void ps5_update_start(void)
{
	pthread_attr_t attributes;
	pthread_t thread;

	if (HALO_BUILD_NUMBER <= 0)
	{
		ps5_log("update: this build has no number, so it does not look for updates");
		return;
	}
	if (!automatic_updates())
	{
		ps5_log("update: build %d; update.auto is off in config.toml", HALO_BUILD_NUMBER);
		return;
	}
	ps5_log("update: build %d (%s) looks for a newer release in %d s", HALO_BUILD_NUMBER, HALO_BUILD_FLAVOR,
		CHECK_AFTER_SECONDS);
	pthread_attr_init(&attributes);
	/* (Mbed TLS's handshake wants more than a small stack) */
	pthread_attr_setstacksize(&attributes, 1024 * 1024);
	pthread_attr_setdetachstate(&attributes, PTHREAD_CREATE_DETACHED);
	if (pthread_create(&thread, &attributes, update_thread, NULL) != 0)
		ps5_log("update: the updater's thread cannot be started");
	pthread_attr_destroy(&attributes);
}

/* posix_update.c's update_launch, which this file never calls, starts a
new executable with execl, which no module a title loads exports: the
build names it this (tools/ps5_build.py) */
int ps5_update_execl(const char *path, const char *argument, ...);

int ps5_update_execl(const char *path, const char *argument, ...)
{
	(void)path;
	(void)argument;
	errno = ENOSYS;
	return -1;
}

/* ---------- the guest's updater (updater.c), which is never given a number */

static void unavailable(int *logged, const char *what)
{
	if (!__atomic_exchange_n(logged, 1, __ATOMIC_RELAXED))
		host_logf(HOST_LOG_INFO, "updater: %s is not the guest's on the PS5 (the host updates the title)", what);
}

int host_update_download(const char *url, const char *path, uint32_t progress, uint32_t context, char *error,
	int error_size)
{
	static int logged;

	(void)url;
	(void)path;
	(void)progress;
	(void)context;
	unavailable(&logged, "downloading a release");
	if (error && error_size > 0)
		snprintf(error, (size_t)error_size, "The PS5 build is updated by its host.");
	return 0;
}

int host_update_executable_path(char *path, int size)
{
	if (!path || size <= 0)
		return 0;
	return snprintf(path, (size_t)size, "%s/" UPDATE_PROGRAM, PS5_TITLE_FOLDER) < size;
}

int host_update_replace_file(const char *path, const char *new_path, const char *old_path)
{
	static int logged;

	(void)path;
	(void)new_path;
	(void)old_path;
	unavailable(&logged, "replacing a file");
	return 0;
}

void host_update_delete_file(const char *path)
{
	static int logged;

	(void)path;
	unavailable(&logged, "deleting a file");
}

int host_update_make_directory(const char *path)
{
	static int logged;

	(void)path;
	unavailable(&logged, "making a folder");
	return 0;
}

int host_update_launch(const char *path)
{
	static int logged;

	(void)path;
	unavailable(&logged, "starting the new version");
	return 0;
}
