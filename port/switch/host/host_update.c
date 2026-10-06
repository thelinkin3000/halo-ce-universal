/*
HOST_UPDATE.C

The Switch port's self-updater, as the other platforms have (Updater.java,
port/linux/src/updater.c): a build of main made by GitHub Actions knows its
build number (HALO_BUILD_NUMBER, which names its release: build-<number>), and
before the game starts it asks GitHub for the latest release. If that is newer, the
player is offered it; taking it downloads the release's Switch archive,
installs the program and the game image, and restarts into them. Builds made
anywhere else have no number and never look, and config.toml's update.auto
turns the look off.

GitHub names the latest release by where /releases/latest redirects to
(/releases/tag/build-<number>), so the check is one request and no JSON. The
archive (halo-switch-release.zip, or -debug.zip for a debug build) is stored,
not compressed - the release is zipped with zip -0 - and each file's size and
CRC come before its data, so it is unpacked as it downloads: the two files the
port needs are written beside the old ones as .new, checked against their CRC,
and only then put in place. The archive itself is never kept, which saves
17 MB of card and the time to write it.

Nothing restarts. The program being run is replaced at once - the loader read
it whole when it started - and the player is told to quit and start the game
again; until then the session goes on as it was, on the old program and the
old game image, which belong together. The new image waits beside the old one
and the new program puts it in place when it starts (host_update_finish). A
restart through the loader (envSetNextLoad) was tried first, and under
Sphaira the new build stopped as soon as it had started.
*/

#include "host.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <switch.h>

#include "tomlc17.h"

#include "host_ui.h"

/* (given for this file by the build: tools/switch_build.py) */
#ifndef HALO_BUILD_NUMBER
#define HALO_BUILD_NUMBER 0
#endif
#ifndef HALO_BUILD_FLAVOR
#define HALO_BUILD_FLAVOR "release"
#endif

#define UPDATE_REPOSITORY "thelinkin3000/halo-ce-universal"
#define UPDATE_ASSET "halo-switch-" HALO_BUILD_FLAVOR ".zip"
#define UPDATE_PROGRAM "halo.nro"
#define UPDATE_IMAGE "halo_guest.elf"
/* the deko3d renderer's image as releases before it became the game image
named it: an update takes it away (install) */
#define UPDATE_OLD_DK_IMAGE "halo_guest_dk.elf"
/* internet play's brokers (network.brokers_file), beside config.toml */
#define UPDATE_BROKERS "brokers.txt"

enum
{
	ZIP_LOCAL_HEADER = 30,
	ZIP_LOCAL_SIGNATURE = 0x04034b50,
	ZIP_CENTRAL_SIGNATURE = 0x02014b50,
	/* the files taken from the archive: the program and the game image,
	which it must hold, then the brokers, which it may */
	WANTED_FILES = 3,
	REQUIRED_FILES = 2,
	PROGRESS_EVERY = 512 * 1024,
};

static PadState *menu_pad;

static int fail(char *error, size_t error_size, const char *format, ...) __attribute__((format(printf, 3, 4)));

static int fail(char *error, size_t error_size, const char *format, ...)
{
	va_list arguments;

	va_start(arguments, format);
	vsnprintf(error, error_size, format, arguments);
	va_end(arguments);
	host_logf(HOST_LOG_WARN, "update: %s", error);
	return 0;
}

/* ---------- where the files are */

/* the program's path: the loader's first argument, or the usual place */
static void program_path(char *path, size_t size)
{
	const char *arguments = envGetArgv();
	size_t length = 0;

	snprintf(path, size, "%s/%s", host_executable_root(), UPDATE_PROGRAM);
	if (!arguments)
		return;
	if (*arguments == '"')
	{
		const char *end = strchr(arguments + 1, '"');

		if (!end)
			return;
		length = (size_t)(end - arguments - 1);
		arguments++;
	}
	else
	{
		while (arguments[length] && arguments[length] != ' ')
			length++;
	}
	if (length < 5 || length >= size || strncmp(arguments + length - 4, ".nro", 4) || strncmp(arguments, "sdmc:/", 6))
		return;
	memcpy(path, arguments, length);
	path[length] = 0;
}

static void wanted_path(int which, char *path, size_t size)
{
	if (which == 0)
		program_path(path, size);
	else
		snprintf(path, size, "%s/%s", host_executable_root(),
			which == 1 ? UPDATE_IMAGE : UPDATE_BROKERS);
}

/* ---------- the check */

/* update.auto in config.toml; true if it is not there */
static int automatic_updates(void)
{
	char path[512];
	toml_result_t result;
	int value = 1;

	snprintf(path, sizeof(path), "%s/config.toml", host_executable_root());
	result = toml_parse_file_ex(path);
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
	char location[HOST_HTTPS_MAXIMUM_URL];
	char error[256];
	const char *tag;
	int status = host_https_get("https://github.com/" UPDATE_REPOSITORY "/releases/latest", 0, NULL, NULL,
		location, sizeof(location), error, sizeof(error));

	if (status < 300 || status >= 400)
	{
		if (status >= 0)
			host_logf(HOST_LOG_INFO, "update: GitHub answered %d for the latest release", status);
		return 0;
	}
	tag = strstr(location, "/releases/tag/build-");
	return tag ? strtol(tag + strlen("/releases/tag/build-"), NULL, 10) : 0;
}

/* ---------- unpacking as it downloads */

static unsigned long crc_table[256];

static unsigned long crc_update(unsigned long crc, const unsigned char *data, size_t size)
{
	size_t index;

	if (!crc_table[1])
	{
		unsigned long entry, bit;

		for (entry = 0; entry < 256; entry++)
		{
			unsigned long value = entry;

			for (bit = 0; bit < 8; bit++)
				value = value & 1 ? 0xedb88320UL ^ (value >> 1) : value >> 1;
			crc_table[entry] = value;
		}
	}
	crc = ~crc & 0xffffffffUL;
	for (index = 0; index < size; index++)
		crc = crc_table[(crc ^ data[index]) & 0xff] ^ (crc >> 8);
	return ~crc & 0xffffffffUL;
}

static unsigned long little(const unsigned char *bytes, int count)
{
	unsigned long value = 0;
	int index;

	for (index = count - 1; index >= 0; index--)
		value = (value << 8) | bytes[index];
	return value;
}

enum
{
	_unpack_header,
	_unpack_name,
	_unpack_data,
	_unpack_done,
};

struct unpack
{
	int state;
	unsigned char header[ZIP_LOCAL_HEADER];
	size_t have;
	char name[256];
	size_t name_length, extra_length;
	unsigned long remaining, expected_crc, crc;
	/* the file being written, and which of the wanted ones it is (-1 none) */
	FILE *file;
	int which;
	int written[WANTED_FILES];
	long long received, reported, total;
	/* the build being downloaded, for the progress screen */
	long build;
	int stopped;
	char error[256];
};

static void draw_progress(const struct unpack *unpack)
{
	char message[64];

	snprintf(message, sizeof(message), "Downloading build %ld...", unpack->build);
	host_ui_progress("UPDATING", message, unpack->received, unpack->total,
		(const char *const[]){ "B", "Stop", NULL });
}

static int unpack_failed(struct unpack *unpack, const char *format, const char *detail)
{
	snprintf(unpack->error, sizeof(unpack->error), format, detail);
	return 0;
}

/* starts the entry whose header and name have arrived */
static int unpack_entry(struct unpack *unpack)
{
	const unsigned char *header = unpack->header;
	unsigned long flags = little(header + 6, 2), method = little(header + 8, 2);
	unsigned long compressed = little(header + 18, 4), size = little(header + 22, 4);
	char path[512];
	int which;

	unpack->name[unpack->name_length < sizeof(unpack->name) ? unpack->name_length : sizeof(unpack->name) - 1] = 0;
	which = !strcmp(unpack->name, UPDATE_PROGRAM) ? 0 : !strcmp(unpack->name, UPDATE_IMAGE) ? 1 :
		!strcmp(unpack->name, UPDATE_BROKERS) ? 2 : -1;
	unpack->remaining = compressed;
	unpack->expected_crc = little(header + 14, 4);
	unpack->crc = 0;
	unpack->which = which;
	unpack->file = NULL;
	if (which < 0)
		return 1;
	/* the release is stored, and says each size up front; anything else is
	an archive this was not written for (the releases' are stored:
.github/workflows/build.yml) */
	if (method != 0 || (flags & 8) || compressed != size)
		return unpack_failed(unpack, "The release's %s is packed in a way the console cannot unpack.", unpack->name);
	wanted_path(which, path, sizeof(path));
	strcat(path, ".new");
	unpack->file = fopen(path, "wb");
	if (!unpack->file)
		return unpack_failed(unpack, "Cannot write %s to the card.", path);
	setvbuf(unpack->file, NULL, _IOFBF, 256 * 1024);
	return 1;
}

static int unpack_finish_entry(struct unpack *unpack)
{
	if (unpack->which < 0)
		return 1;
	if (fclose(unpack->file) != 0)
	{
		unpack->file = NULL;
		return unpack_failed(unpack, "Writing %s to the card failed.", unpack->name);
	}
	unpack->file = NULL;
	if (unpack->crc != unpack->expected_crc)
		return unpack_failed(unpack, "%s did not arrive intact.", unpack->name);
	unpack->written[unpack->which] = 1;
	return 1;
}

static int unpack_body(void *context, const void *data, size_t size, long long total)
{
	struct unpack *unpack = context;
	const unsigned char *bytes = data;

	unpack->total = total;
	unpack->received += (long long)size;
	if (unpack->received - unpack->reported >= PROGRESS_EVERY)
	{
		unpack->reported = unpack->received;
		draw_progress(unpack);
		padUpdate(menu_pad);
		if (padGetButtonsDown(menu_pad) & HidNpadButton_B)
		{
			unpack->stopped = 1;
			return unpack_failed(unpack, "%s", "The update was stopped.");
		}
	}
	while (size && unpack->state != _unpack_done)
	{
		size_t take;

		switch (unpack->state)
		{
		case _unpack_header:
			take = ZIP_LOCAL_HEADER - unpack->have < size ? ZIP_LOCAL_HEADER - unpack->have : size;
			memcpy(unpack->header + unpack->have, bytes, take);
			unpack->have += take;
			if (unpack->have >= 4 && little(unpack->header, 4) == ZIP_CENTRAL_SIGNATURE)
			{
				/* past the files: the directory at the end is not needed */
				unpack->state = _unpack_done;
				break;
			}
			if (unpack->have == ZIP_LOCAL_HEADER)
			{
				if (little(unpack->header, 4) != ZIP_LOCAL_SIGNATURE)
					return unpack_failed(unpack, "%s", "The release is not an archive the console can read.");
				unpack->name_length = little(unpack->header + 26, 2);
				unpack->extra_length = little(unpack->header + 28, 2);
				unpack->have = 0;
				unpack->state = _unpack_name;
			}
			break;
		case _unpack_name:
			take = unpack->name_length + unpack->extra_length - unpack->have;
			take = take < size ? take : size;
			{
				size_t index;

				/* the name, then the extra field, which is passed over */
				for (index = 0; index < take; index++)
				{
					size_t at = unpack->have + index;

					if (at < unpack->name_length && at < sizeof(unpack->name) - 1)
						unpack->name[at] = (char)bytes[index];
				}
			}
			unpack->have += take;
			if (unpack->have == unpack->name_length + unpack->extra_length)
			{
				unpack->have = 0;
				if (!unpack_entry(unpack))
					return 0;
				unpack->state = _unpack_data;
				if (!unpack->remaining)
				{
					if (!unpack_finish_entry(unpack))
						return 0;
					unpack->state = _unpack_header;
				}
			}
			break;
		case _unpack_data:
			take = unpack->remaining < size ? unpack->remaining : size;
			if (unpack->file)
			{
				if (fwrite(bytes, 1, take, unpack->file) != take)
					return unpack_failed(unpack, "Writing %s to the card failed.", unpack->name);
				unpack->crc = crc_update(unpack->crc, bytes, take);
			}
			unpack->remaining -= take;
			if (!unpack->remaining)
			{
				if (!unpack_finish_entry(unpack))
					return 0;
				unpack->state = _unpack_header;
			}
			break;
		}
		bytes += take;
		size -= take;
	}
	return 1;
}

/* ---------- the screens' part */

static u64 wait_for(u64 buttons)
{
	for (;;)
	{
		u64 down;

		if (!appletMainLoop())
		{
			host_ui_close();
			host_exit(0);
		}
		padUpdate(menu_pad);
		down = padGetButtonsDown(menu_pad);
		if (down & buttons)
			return down;
		svcSleepThread(16666667);
	}
}

/* new_path in place of target: the card will not rename onto a file that
is there, so the old one goes first */
static int replace(const char *new_path, const char *target)
{
	remove(target);
	return rename(new_path, target) == 0;
}

/* Puts the new program in place, and leaves the new game image waiting.

The session goes on after an update, on the program that is running, which
loads the game image when the game starts: it has to be the image built with
it, or its imports and the image's may not agree. So the image stays as
halo_guest.elf.new until the next start, when the new program puts it in
place before anything loads it (host_update_finish). The program itself can be
replaced now: the loader read it whole when it started. The deko3d image of
the releases that had two (halo_guest_dk.elf) is taken away: the game image is
deko3d's now, and nothing loads that one again. The brokers' list goes as the
game image does (the game reads it when internet play starts), and a release
without one leaves the old one. */
static int install(char *error, size_t error_size)
{
	char target[512], temporary[520];

	snprintf(target, sizeof(target), "%s/%s", host_executable_root(), UPDATE_OLD_DK_IMAGE);
	remove(target);
	wanted_path(0, target, sizeof(target));
	snprintf(temporary, sizeof(temporary), "%s.new", target);
	if (!replace(temporary, target))
		return fail(error, error_size, "Cannot put %s in place; it is on the card as %s.", target, temporary);
	return 1;
}

void host_update_finish(void)
{
	int which;

	for (which = 1; which < WANTED_FILES; which++)
	{
		char target[512], temporary[520];
		FILE *pending;

		wanted_path(which, target, sizeof(target));
		snprintf(temporary, sizeof(temporary), "%s.new", target);
		pending = fopen(temporary, "rb");
		if (!pending)
			continue;
		fclose(pending);
		if (replace(temporary, target))
			host_logf(HOST_LOG_INFO, "update: the new %s is in place", target);
		else
			host_logf(HOST_LOG_ERROR, "update: cannot put %s in place; it is still %s", target, temporary);
	}
}

static void remove_partial_files(void)
{
	int which;

	for (which = 0; which < WANTED_FILES; which++)
	{
		char target[512], temporary[520];

		wanted_path(which, target, sizeof(target));
		snprintf(temporary, sizeof(temporary), "%s.new", target);
		remove(temporary);
	}
}

static int download_and_install(long build, char *error, size_t error_size)
{
	char url[256];
	struct unpack *unpack = calloc(1, sizeof(*unpack));
	int status, which;

	if (!unpack)
		return fail(error, error_size, "Out of memory for the update.");
	snprintf(url, sizeof(url), "https://github.com/" UPDATE_REPOSITORY "/releases/download/build-%ld/" UPDATE_ASSET,
		build);
	host_logf(HOST_LOG_INFO, "update: downloading %s", url);
	unpack->build = build;
	draw_progress(unpack);
	status = host_https_get(url, 1, unpack_body, unpack, NULL, 0, error, error_size);
	if (unpack->file)
		fclose(unpack->file);
	if (unpack->error[0])
	{
		fail(error, error_size, "%s", unpack->error);
		status = -1;
	}
	else if (status >= 0 && status != 200)
	{
		fail(error, error_size, "GitHub answered %d for build %ld's %s.", status, build, UPDATE_ASSET);
		status = -1;
	}
	for (which = 0; status == 200 && which < REQUIRED_FILES; which++)
	{
		if (!unpack->written[which])
		{
			fail(error, error_size, "Build %ld's archive has no %s.", build, which ? UPDATE_IMAGE : UPDATE_PROGRAM);
			status = -1;
		}
	}
	free(unpack);
	if (status != 200)
	{
		remove_partial_files();
		return 0;
	}
	return install(error, error_size);
}

void host_update_offer(void *pad)
{
	char error[256];
	char message[160];
	long build;

	menu_pad = pad;
	if (HALO_BUILD_NUMBER <= 0)
	{
		host_logf(HOST_LOG_INFO, "update: this build has no number, so it does not look for updates");
		return;
	}
	if (!automatic_updates())
	{
		host_logf(HOST_LOG_INFO, "update: update.auto is off in config.toml");
		return;
	}
	host_ui_message("HALO: COMBAT EVOLVED", "Looking for an update...", NULL);
	build = latest_build();
	if (build <= HALO_BUILD_NUMBER)
	{
		host_logf(HOST_LOG_INFO, "update: build %d is the latest%s", HALO_BUILD_NUMBER,
			build ? "" : " that could be checked");
		return;
	}
	host_logf(HOST_LOG_INFO, "update: build %ld is available (this is build %d)", build, HALO_BUILD_NUMBER);
	snprintf(message, sizeof(message), "Build %ld is available; this is build %d.", build, HALO_BUILD_NUMBER);
	host_ui_message("AN UPDATE IS AVAILABLE", message, (const char *const[]){ "A", "Update now", "B", "Not now", NULL });
	if (wait_for(HidNpadButton_A | HidNpadButton_B) & HidNpadButton_B)
	{
		host_logf(HOST_LOG_INFO, "update: not now");
		return;
	}
	if (!download_and_install(build, error, sizeof(error)))
	{
		host_ui_message("THE UPDATE DID NOT FINISH", error, (const char *const[]){ "B", "Continue without it", NULL });
		wait_for(HidNpadButton_B);
		return;
	}
	host_logf(HOST_LOG_INFO, "update: build %ld is installed; it is used from the next start", build);
	/* the session goes on with this build (install, above) */
	snprintf(message, sizeof(message), "Build %ld is installed. To use it, quit the game and start it again.",
		build);
	host_ui_message("UPDATE INSTALLED", message, (const char *const[]){ "A", "Continue", NULL });
	wait_for(HidNpadButton_A);
}
