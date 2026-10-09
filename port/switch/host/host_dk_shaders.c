/*
HOST_DK_SHADERS.C

The deko3d renderer's shader service (port/switch/DEKO3D.md, phase 5, step
3): compiled shaders kept on the card, loaded into GPU code memory when
wanted, and compiled in the background for every key the console knows of
but has not compiled.

The guest (port/switch/guest/dk_shaders.c) never sends a key here, only a
64-bit hash of one and GLSL, through the imports host_dk_shader_find and
host_dk_shader_compile. On the card, a shader is

    <data root>/shader_cache/<UAM commit, 12 hex>/<stage letter><hash, 16 hex>.dksh

one file a shader, written a batch at a time every DKSH_WRITE_INTERVAL
seconds (the writer, below), a new one under its own name and one replacing
a broken file as .tmp and renamed; a file cut short is taken for broken and
compiled again. At start this folder's
names are listed once into a set (the files are not opened; one is read only
when its shader is asked for), and the other folders under shader_cache/ -
other UAM versions' - are removed, and nothing else is touched. The
generators' version is in the guest's hash, so a generator change makes new
names, not a new folder; a stale file is just never asked for.

One compile thread (step 1 measured more to be slower), which moves itself
off the game thread's core (host_thread.c's place_thread) and never touches
deko3d: it takes the queue's most urgent entry, compiles it with
host_dk_compile_glsl into memory, and adds its hash to the set; a load takes
it from memory until the writer has put it on the card. Loading is
the game thread's (the only thread that touches deko3d), about 2 ms a
shader, and a shader once loaded stays for the program's life in one code
memory block, never freed - the cache on the card is what keeps that
bounded.

A handle (what find returns, and what phase 6's draws will name in the
command stream) is an index into the table of loaded shaders, plus one.
*/

#include "host.h"

#include <deko3d.h>
#include <switch.h>

#include <dirent.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* the compiler whose DKSH files these are: the folder name, so a new UAM
   does not read an old compiler's files (tools/switch_build.py defines it;
   a hand build says "unknown" and shares whatever folder that makes) */
#ifndef HOST_DK_UAM_COMMIT
#define HOST_DK_UAM_COMMIT "unknown"
#endif

#define DKSH_CODE_SIZE (16 * 1024 * 1024)
#define DKSH_CODE_FLAGS (DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached | DkMemBlockFlags_Code)
#define DKSH_TABLE_LIMIT 4096
#define DKSH_SET_SLOTS 8192 /* a power of two, for the mask */
/* the most the set holds: seven eighths of its slots, so linear probing
always finds an empty slot (a full set would probe for ever) */
#define DKSH_SET_FILL (DKSH_SET_SLOTS / 8 * 7)
/* how often the compile thread says where it is; not one line a shader */
#define DKSH_LOG_EVERY 16
/* the compile thread's stack: Mesa's compiler recurses, and 2 MB is what
the probe's workers ran with (phase 5, step 1) */
#define DKSH_THREAD_STACK (2 * 1024 * 1024)

/* the DKSH file's header, as the probe's shader_load reads it */
struct dksh_header
{
	uint32_t magic, header_size, control_size, code_size, programs_offset, program_count;
};

/* a shader the host knows: on the card, or loaded (a handle into the table
below). handle 0 means the file is on the card and has not been loaded. */
struct dksh_slot
{
	uint64_t hash;
	uint32_t stage;
	uint32_t handle;
	/* its file would not load (cut short, or not a DKSH): taken for absent,
	so it is compiled again - and not read again every time it is asked
	for, which a draw would do every frame */
	uint8_t broken;
	/* whole, but no room for it in code memory or the table: not read
	again either (a cache past 16 MB of code would want a bigger block) */
	uint8_t unloadable;
	/* queued for the loader thread, or being loaded */
	uint8_t loading;
	/* a file of its own in the folder (the cache before the pack), which
	moves into the pack once it is loaded */
	uint8_t loose;
	/* where its DKSH is in the pack, and how long; 0 if it is not there */
	uint32_t pack_offset, pack_size;
};

/* one loaded shader; the handle the guest gets is this's index plus one */
struct dksh_loaded
{
	DkShader shader;
	uint64_t hash;
	uint32_t stage;
};

/* what the guest asks to have compiled */
struct dksh_request
{
	uint64_t hash;
	uint32_t stage;
	/* 0 a draw needs it now, 1 the map being played, 2 the rest (the
	guest's dk_shaders.h); lower is more urgent */
	uint32_t priority;
	char *glsl;
};

#define DKSH_LOAD_QUEUE 1024
/* how often the shaders compiled since are written to the card (seconds) */
#define DKSH_WRITE_INTERVAL 30

/* a shader compiled and not yet on the card: its DKSH, which a load takes
from here meanwhile */
struct dksh_pending
{
	uint64_t hash;
	uint32_t stage;
	void *data;
	size_t size;
	/* the card has a file of it already (a recompile of one that would not
	load), which the new one replaces; a new shader's is written as it is */
	int replaces;
	/* read from its own file in the folder, which goes once it is in the
	pack */
	int from_loose;
};

/* the pack: <cache root>/<UAM commit>.pack, a header and then records, each
a record header and a DKSH. Read through once at start into the set (one
open, not one a shader: a file among thousands in one folder took 80 ms to
open on the card); appended to by the writer; a record cut short (the
console off mid-write) ends it, and the next append writes over it. A later
record of a hash stands for an earlier one. */
#define DKSH_PACK_MAGIC 0x4b504b44u /* 'DKPK' */
#define DKSH_PACK_VERSION 1u
#define DKSH_RECORD_MAGIC 0x52504b44u /* 'DKPR' */
#define DKSH_RECORD_LIMIT (4u * 1024 * 1024)

struct dksh_pack_header
{
	uint32_t magic, version;
};

struct dksh_pack_record
{
	uint32_t magic, stage;
	uint64_t hash;
	uint32_t size, reserved;
};

static struct
{
	pthread_mutex_t lock;
	pthread_cond_t queue_condition;

	int started;
	/* the set of shaders known: on the card or loaded. Linear probing;
	0 marks an empty slot, and no hash is 0 (FNV-1a never returns it) */
	struct dksh_slot set[DKSH_SET_SLOTS];
	/* entries in the set; it takes no more than SET_FILL of its slots, so
	a probe always meets an empty one and ends */
	uint32_t set_count;
	int said_set_full;

	/* the loaded shaders, and the code memory they live in. Both are the
	game thread's alone: find loads, and only the game thread calls find */
	DkMemBlock code_memory;
	uint32_t code_used;
	struct dksh_loaded loaded[DKSH_TABLE_LIMIT];
	uint32_t loaded_count;

	/* the queue, in arrival order; the thread takes the most urgent entry
	(minimum priority), first queued among equals */
	struct dksh_request *queue;
	uint32_t queue_count, queue_capacity;
	pthread_t compile_thread;

	/* the shader the compile thread has taken off the queue and not yet
	finished: neither queued nor on the card, it is still being made, and a
	find says so rather than "unknown" (which would have it queued again) */
	int compiling;
	uint64_t compiling_hash;
	uint32_t compiling_stage;

	/* the compile thread's progress, for the log */
	unsigned long compiled, failed;
	unsigned long logged;

	int logged_first_hash;
	/* the loader thread's queue (shaders on the card, to be read and put
	in code memory) */
	struct { uint64_t hash; uint32_t stage; } load_queue[DKSH_LOAD_QUEUE];
	uint32_t load_count;
	pthread_cond_t load_condition;
	int loader_started;
	/* a load the loader thread is doing now (host_dk_shader_loading) */
	int load_running;

	/* the shaders compiled since the writer last wrote (under lock) */
	struct dksh_pending *pending;
	uint32_t pending_count, pending_capacity;

	/* the pack, open for the run (read and written under the card's lock),
	and where the next record goes; NULL: none, the shaders are files */
	FILE *pack;
	uint32_t pack_end;
	/* the guest's preload has asked for every shader it knows (all that is
	worth keeping is in the pack or on its way), and the folder's files left
	are the cache's leftovers, removed a file at a time by the writer */
	volatile int preload_done;
	int folder_cleared;
	/* loads this run read from a file of the cache before the pack (the
	guest's "moving" message) */
	uint32_t loose_loads;
} dksh;

/* ---------- paths */

/* the cache's root, <data root>/shader_cache */
static void cache_root(char *path, size_t size)
{
	snprintf(path, size, "%s/shader_cache", host_data_root());
}

/* this UAM's folder in it */
static void commit_folder(char *path, size_t size)
{
	snprintf(path, size, "%s/shader_cache/%s", host_data_root(), HOST_DK_UAM_COMMIT);
}

/* the pack */
static void pack_path(char *path, size_t size)
{
	snprintf(path, size, "%s/shader_cache/%s.pack", host_data_root(), HOST_DK_UAM_COMMIT);
}

/* the file a shader is, and its temporary name while it is written */
static void shader_path(char *path, size_t size, uint32_t stage, uint64_t hash, int temporary)
{
	snprintf(path, size, "%s/shader_cache/%s/%c%016llx.dksh%s", host_data_root(), HOST_DK_UAM_COMMIT,
		stage ? 'f' : 'v', (unsigned long long)hash, temporary ? ".tmp" : "");
}

/* ---------- the set of known shaders (under dksh.lock) */

static uint32_t set_slot(uint64_t hash)
{
	uint32_t slot = (uint32_t)(hash * 1099511628211ULL) & (DKSH_SET_SLOTS - 1);

	for (; dksh.set[slot].hash; slot = (slot + 1) & (DKSH_SET_SLOTS - 1))
	{
		if (dksh.set[slot].hash == hash)
			break;
	}
	return slot;
}

static struct dksh_slot *set_find(uint64_t hash)
{
	uint32_t slot = set_slot(hash);

	return dksh.set[slot].hash ? &dksh.set[slot] : NULL;
}

/* the entry for hash, made if it is not there; NULL when the set is as
full as it may be. That is a cache that stops growing, not a crash - and
not a hang: an unbounded set would fill and its probing never end. Stale
shaders on the card (a generator change leaves its old names; nothing
removes them) count towards it, so it is said once, in the log. */
static struct dksh_slot *set_insert(uint64_t hash, uint32_t stage)
{
	uint32_t slot = set_slot(hash);

	if (!dksh.set[slot].hash)
	{
		if (dksh.set_count >= DKSH_SET_FILL)
		{
			dksh.said_set_full = 1;
			return NULL;
		}
		dksh.set[slot].hash = hash;
		dksh.set[slot].stage = stage;
		dksh.set[slot].handle = 0;
		dksh.set_count++;
	}
	dksh.set[slot].broken = 0;
	return &dksh.set[slot];
}

/* ---------- the card, at start (under dksh.lock; no deko3d here) */

/* the files of one folder under the cache's root, deleted (a folder of
another UAM version's; nothing else is ever named to this) */
/* (the caller holds the SD lock: the card's driver is shared with the
guest's file calls and the logger, so one lock is held while a driver call
runs and never while logging - host.h) */
static void folder_remove_locked(const char *root, const char *name)
{
	char folder[1536];
	DIR *directory;
	struct dirent *entry;

	snprintf(folder, sizeof(folder), "%s/%s", root, name);
	directory = opendir(folder);
	if (!directory)
	{
		rmdir(folder);
		return;
	}
	while ((entry = readdir(directory)) != NULL)
	{
		char file[1792]; /* the folder's 1536 and a name (255) fit */

		if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, ".."))
			continue;
		snprintf(file, sizeof(file), "%s/%s", folder, entry->d_name);
		unlink(file);
	}
	closedir(directory);
	rmdir(folder);
}

/* opens this compiler's pack (made if there is none, or if it is not one)
and reads its records into the set; the card's lock is held */
static void pack_open_locked(unsigned long *packed)
{
	char path[1024];
	struct dksh_pack_header header;
	long length;
	uint32_t offset;

	pack_path(path, sizeof(path));
	dksh.pack = fopen(path, "r+b");
	if (dksh.pack && (fread(&header, sizeof(header), 1, dksh.pack) != 1 || header.magic != DKSH_PACK_MAGIC ||
		header.version != DKSH_PACK_VERSION))
	{
		fclose(dksh.pack);
		dksh.pack = NULL;
	}
	if (!dksh.pack)
	{
		dksh.pack = fopen(path, "w+b");
		if (!dksh.pack)
			return;
		header.magic = DKSH_PACK_MAGIC;
		header.version = DKSH_PACK_VERSION;
		if (fwrite(&header, sizeof(header), 1, dksh.pack) != 1 || fflush(dksh.pack) != 0)
		{
			fclose(dksh.pack);
			dksh.pack = NULL;
			return;
		}
	}
	fseek(dksh.pack, 0, SEEK_END);
	length = ftell(dksh.pack);
	offset = sizeof(header);
	for (;;)
	{
		struct dksh_pack_record record;
		struct dksh_slot *slot;

		if ((long)offset + (long)sizeof(record) > length || fseek(dksh.pack, offset, SEEK_SET) != 0 ||
			fread(&record, sizeof(record), 1, dksh.pack) != 1 || record.magic != DKSH_RECORD_MAGIC ||
			record.stage > 1 || !record.size || record.size > DKSH_RECORD_LIMIT ||
			(long)offset + (long)sizeof(record) + (long)record.size > length)
		{
			break;
		}
		slot = set_insert(record.hash, record.stage);
		if (slot)
		{
			slot->pack_offset = offset + (uint32_t)sizeof(record);
			slot->pack_size = record.size;
			(*packed)++;
		}
		offset += (uint32_t)sizeof(record) + record.size;
	}
	dksh.pack_end = offset;
}

/* lists this compiler's folder into the set, and removes the other folders
under the cache's root (they are other UAM versions'; nothing else there is
touched). The whole pass is under the SD lock (it is one walk of two
folders), and what it has to say is said after, with the lock gone */
static void cache_scan(void)
{
	char root[1024], folder[1536];
	char removed[8][1536];
	unsigned removed_count = 0;
	DIR *directory;
	struct dirent *entry;
	unsigned long listed = 0, packed = 0;

	int say_list = 0, list_error = 0;

	cache_root(root, sizeof(root));
	commit_folder(folder, sizeof(folder));
	host_sd_lock();
	mkdir(root, 0777);
	mkdir(folder, 0777);
	directory = opendir(folder);
	if (!directory)
	{
		list_error = errno;
		say_list = 1;
	}
	while (directory && (entry = readdir(directory)) != NULL)
	{
		/* <stage letter><hash, 16 hex>.dksh: 22 characters */
		const char *dot = strrchr(entry->d_name, '.');
		uint64_t hash = 0;
		int stage, digit;

		if (!dot || strcmp(dot, ".dksh") || strlen(entry->d_name) != 22)
			continue;
		stage = entry->d_name[0] == 'f' ? 1 : entry->d_name[0] == 'v' ? 0 : -1;
		if (stage < 0)
			continue;
		for (digit = 1; digit <= 16; digit++)
		{
			char c = entry->d_name[digit];
			int value = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;

			if (value < 0)
				break;
			hash = hash << 4 | (uint64_t)value;
		}
		if (digit != 17)
			continue;
		{
			struct dksh_slot *slot = set_insert(hash, (uint32_t)stage);

			if (slot)
			{
				slot->loose = 1;
				listed++;
			}
		}
	}
	if (directory)
		closedir(directory);

	/* the other compilers' folders, removed so the card is not filled with
	versions nobody asks for */
	directory = opendir(root);
	while (directory && (entry = readdir(directory)) != NULL)
	{
		struct stat information;
		char inside[1536];

		if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..") ||
			!strcmp(entry->d_name, HOST_DK_UAM_COMMIT))
			continue;
		snprintf(inside, sizeof(inside), "%s/%s", root, entry->d_name);
		if (stat(inside, &information) == 0 && S_ISDIR(information.st_mode))
		{
			folder_remove_locked(root, entry->d_name);
			if (removed_count < 8)
				snprintf(removed[removed_count++], sizeof(removed[0]), "%s", inside);
		}
		else if (strlen(entry->d_name) > 5 && !strcmp(entry->d_name + strlen(entry->d_name) - 5, ".pack") &&
			strncmp(entry->d_name, HOST_DK_UAM_COMMIT, strlen(HOST_DK_UAM_COMMIT)))
		{
			/* (another compiler's pack) */
			unlink(inside);
			if (removed_count < 8)
				snprintf(removed[removed_count++], sizeof(removed[0]), "%s", inside);
		}
	}
	if (directory)
		closedir(directory);
	pack_open_locked(&packed);
	host_sd_unlock();
	if (dksh.pack)
		host_logf(HOST_LOG_INFO, "dk shader: %lu shaders in the pack (%u KB)", packed,
			(unsigned)(dksh.pack_end / 1024));
	else
		host_logf(HOST_LOG_ERROR, "dk shader: the pack cannot be opened; shaders are kept as files");
	if (say_list)
		host_logf(HOST_LOG_ERROR, "dk shader: %s cannot be listed (%s); nothing is loaded from the card", folder,
			strerror(list_error));
	host_logf(HOST_LOG_INFO, "dk shader: %lu shaders on the card in %s", listed, folder);
	if (dksh.said_set_full)
		host_logf(HOST_LOG_ERROR, "dk shader: more than %u shaders on the card; the rest are not listed, and no more "
			"are kept (delete %s to start the cache again)", (unsigned)DKSH_SET_FILL, folder);
	for (listed = 0; listed < removed_count; listed++)
		host_logf(HOST_LOG_INFO, "dk shader: removed the old shader cache %s", removed[listed]);
}

/* ---------- the pending list (under dksh.lock) */

/* the shader's DKSH (data, size: the list takes it, and frees it once it is
on the card) put on the list for the writer, unless it is there already;
whether the list took it */
static int pending_add_locked(uint64_t hash, uint32_t stage, void *data, size_t size, int from_loose)
{
	struct dksh_pending *pending;
	uint32_t index;

	for (index = 0; index < dksh.pending_count; index++)
	{
		if (dksh.pending[index].hash == hash && dksh.pending[index].stage == stage)
			return 0;
	}
	if (dksh.pending_count == dksh.pending_capacity)
	{
		uint32_t capacity = dksh.pending_capacity ? dksh.pending_capacity * 2 : 64;
		struct dksh_pending *grown = realloc(dksh.pending, capacity * sizeof(*grown));

		if (!grown)
			return 0;
		dksh.pending = grown;
		dksh.pending_capacity = capacity;
	}
	pending = &dksh.pending[dksh.pending_count++];
	pending->hash = hash;
	pending->stage = stage;
	pending->data = data;
	pending->size = size;
	pending->replaces = set_find(hash) != NULL && !from_loose;
	pending->from_loose = from_loose;
	return 1;
}

/* ---------- the compile thread */

static void *compile_thread(void *unused)
{
	unsigned long since_logged = 0;

	(void)unused;
	/* off the game thread's core (host_thread.c), as libnx would otherwise
	leave this on the core the process starts threads on */
	host_thread_place_on_helper_core();
	/* a step below the game thread's, so a frame is never put off for a
	compile; the cores being apart already keeps them out of each other's
	way, this is belt and braces */
	{
		s32 priority = 0;

		if (R_SUCCEEDED(svcGetThreadPriority(&priority, CUR_THREAD_HANDLE)) && priority < 0x3f)
			svcSetThreadPriority(CUR_THREAD_HANDLE, (u32)priority + 1);
	}
	for (;;)
	{
		struct dksh_request request;
		uint32_t pick, index;
		void *dksh_data = NULL;
		size_t dksh_size = 0;
		int ok;

		pthread_mutex_lock(&dksh.lock);
		while (!dksh.queue_count)
			pthread_cond_wait(&dksh.queue_condition, &dksh.lock);
		/* the most urgent entry: the lowest priority, the first queued
		among equals */
		pick = 0;
		for (index = 1; index < dksh.queue_count; index++)
		{
			if (dksh.queue[index].priority < dksh.queue[pick].priority)
				pick = index;
		}
		request = dksh.queue[pick];
		dksh.queue[pick] = dksh.queue[--dksh.queue_count];
		dksh.compiling = 1;
		dksh.compiling_hash = request.hash;
		dksh.compiling_stage = request.stage;
		pthread_mutex_unlock(&dksh.lock);

		/* compiled into memory: the card is the writer's, every
		DKSH_WRITE_INTERVAL seconds, so a compile never holds the game's file
		calls up behind the card's lock (each one wrote, deleted and renamed a
		file; at three a second that was the menus' saved game check taking
		17 s instead of 2) */
		ok = host_dk_compile_glsl(request.stage == 1, request.glsl, &dksh_data, &dksh_size) != 0;
		free(request.glsl);

		pthread_mutex_lock(&dksh.lock);
		dksh.compiling = 0;
		if (ok && pending_add_locked(request.hash, request.stage, dksh_data, dksh_size, 0))
		{
			dksh_data = NULL;
			/* in the set now, so a find from this moment answers "known", and
			its load takes it from the pending list until the writer has
			written it */
			set_insert(request.hash, request.stage);
			dksh.compiled++;
		}
		else
			dksh.failed++;
		free(dksh_data);
		dksh_data = NULL;
		/* what there is to say is said with the lock gone: a host_logf
		inside it would hold this thread's place in the queue's mutex for
		as long as the logger takes, and the game thread asks the queue
		every key it meets */
		{
			unsigned long compiled = dksh.compiled, failed = dksh.failed;
			uint32_t queued = dksh.queue_count;
			uint64_t hash = request.hash;
			uint32_t stage = request.stage;
			int log_now = ++since_logged >= DKSH_LOG_EVERY;

			if (log_now)
				since_logged = 0;
			pthread_mutex_unlock(&dksh.lock);
			if (!ok)
				host_logf(HOST_LOG_ERROR, "dk shader: %c%016llx did not compile", stage ? 'f' : 'v',
					(unsigned long long)hash);
			if (log_now)
				host_logf(HOST_LOG_INFO, "dk shader: %lu compiled (%lu failed), %u queued", compiled, failed,
					(unsigned)queued);
			continue;
		}
	}
	return NULL;
}

/* ---------- the writer

Every DKSH_WRITE_INTERVAL seconds, the shaders compiled since go to the card,
oldest first and one at a time, each under the card's lock for its own write
only, so the game's file calls get in between. A new shader's file is
written as it is, under its own name (no .tmp, no delete, no rename: a write
cut short leaves a short file, which a load takes for broken and the next
compile replaces); one replacing a file that would not load goes through a
.tmp and a rename, as this console's rename will not replace a file, the old
one deleted first. Until it is written, a load takes a shader from memory
(shader_load); one the console quits before writing is compiled again next
time. */

/* appends one pending shader to the pack, under the card's lock; where its
DKSH is (0: not written). A shader read from its own file loses the file. */
static uint32_t pending_pack(const struct dksh_pending *pending)
{
	struct dksh_pack_record record;
	uint32_t offset = 0;
	int written;

	memset(&record, 0, sizeof(record));
	record.magic = DKSH_RECORD_MAGIC;
	record.stage = pending->stage;
	record.hash = pending->hash;
	record.size = (uint32_t)pending->size;
	host_sd_lock();
	written = dksh.pack && pending->size <= DKSH_RECORD_LIMIT &&
		fseek(dksh.pack, (long)dksh.pack_end, SEEK_SET) == 0 &&
		fwrite(&record, sizeof(record), 1, dksh.pack) == 1 &&
		fwrite(pending->data, 1, pending->size, dksh.pack) == pending->size && fflush(dksh.pack) == 0;
	if (written)
	{
		offset = dksh.pack_end + (uint32_t)sizeof(record);
		dksh.pack_end = offset + (uint32_t)pending->size;
	}
	host_sd_unlock();
	if (written && pending->from_loose)
	{
		char path[1024];

		shader_path(path, sizeof(path), pending->stage, pending->hash, 0);
		host_sd_lock();
		unlink(path);
		host_sd_unlock();
		/* (a file's removal walks the big folder too: the others' calls get
		the card between two) */
		usleep(20000);
	}
	return offset;
}

/* writes one pending shader as its own file (no pack); whether it is on the
card */
static int pending_write(const struct dksh_pending *pending)
{
	char path[1024], temporary[1032];
	FILE *file;
	int written = 0;

	shader_path(path, sizeof(path), pending->stage, pending->hash, 0);
	shader_path(temporary, sizeof(temporary), pending->stage, pending->hash, 1);
	host_sd_lock();
	file = fopen(pending->replaces ? temporary : path, "wb");
	if (file)
	{
		written = fwrite(pending->data, 1, pending->size, file) == pending->size;
		if (fclose(file) != 0)
			written = 0;
		if (!written)
			unlink(pending->replaces ? temporary : path);
	}
	if (written && pending->replaces)
	{
		unlink(path);
		if (rename(temporary, path) != 0)
		{
			unlink(temporary);
			written = 0;
		}
	}
	host_sd_unlock();
	return written;
}

/* Once the guest's preload has asked for every shader it knows (and they
are in the pack, or on the pending list on their way), the folder's files
left are the cache's leftovers: names a changed generator or key left
behind, which nothing asks for. They are removed DKSH_CLEAR_PER_CYCLE a
writer's cycle, each a walk of the big folder, with a pause after each so
the game's own file calls get the card. */
#define DKSH_CLEAR_PER_CYCLE 256

static void folder_clear_some(void)
{
	char folder[1536], names[64][32];
	unsigned removed = 0, found;
	int busy;

	pthread_mutex_lock(&dksh.lock);
	busy = !dksh.preload_done || dksh.folder_cleared || dksh.pending_count || dksh.load_count || dksh.load_running;
	pthread_mutex_unlock(&dksh.lock);
	if (busy || !dksh.pack)
		return;
	commit_folder(folder, sizeof(folder));
	do
	{
		DIR *directory;
		struct dirent *entry;
		unsigned index;

		found = 0;
		host_sd_lock();
		directory = opendir(folder);
		while (directory && found < 64 && (entry = readdir(directory)) != NULL)
		{
			if (strlen(entry->d_name) == 22 && !strcmp(entry->d_name + 17, ".dksh"))
				snprintf(names[found++], sizeof(names[0]), "%s", entry->d_name);
		}
		if (directory)
			closedir(directory);
		host_sd_unlock();
		for (index = 0; index < found; index++)
		{
			char file[1600];
			uint64_t hash = strtoull(names[index] + 1, NULL, 16);

			snprintf(file, sizeof(file), "%s/%s", folder, names[index]);
			host_sd_lock();
			unlink(file);
			host_sd_unlock();
			pthread_mutex_lock(&dksh.lock);
			{
				struct dksh_slot *slot = set_find(hash);

				/* (not in the pack: as if never compiled, compiled again if it
				is ever asked for) */
				if (slot && slot->loose)
				{
					slot->loose = 0;
					if (!slot->pack_size && !slot->handle)
						slot->broken = 1;
				}
			}
			pthread_mutex_unlock(&dksh.lock);
			removed++;
			usleep(20000);
		}
	} while (found && removed < DKSH_CLEAR_PER_CYCLE);
	if (!found)
	{
		dksh.folder_cleared = 1;
		host_sd_lock();
		rmdir(folder);
		host_sd_unlock();
	}
	if (removed || dksh.folder_cleared)
		host_logf(HOST_LOG_INFO, "dk shader: %u files of the cache before the pack removed%s", removed,
			dksh.folder_cleared ? "; the folder is empty" : "");
}

static void *writer_thread(void *unused)
{
	(void)unused;
	host_thread_place_on_helper_core();
	for (;;)
	{
		unsigned written = 0, failed = 0;

		sleep(DKSH_WRITE_INTERVAL);
		for (;;)
		{
			struct dksh_pending pending;
			int ok;

			/* the oldest (compiles only append, so it stays first; loads
			copy from it meanwhile) */
			pthread_mutex_lock(&dksh.lock);
			if (!dksh.pending_count)
			{
				pthread_mutex_unlock(&dksh.lock);
				break;
			}
			pending = dksh.pending[0];
			pthread_mutex_unlock(&dksh.lock);

			{
				uint32_t offset = dksh.pack ? pending_pack(&pending) : 0;

				ok = offset != 0 || (!dksh.pack && pending_write(&pending));
				pthread_mutex_lock(&dksh.lock);
				if (offset)
				{
					struct dksh_slot *slot = set_find(pending.hash);

					if (slot)
					{
						slot->pack_offset = offset;
						slot->pack_size = (uint32_t)pending.size;
						if (pending.from_loose)
							slot->loose = 0;
					}
				}
			}
			memmove(&dksh.pending[0], &dksh.pending[1], (dksh.pending_count - 1) * sizeof(dksh.pending[0]));
			dksh.pending_count--;
			if (!ok)
			{
				/* not on the card: a shader not loaded yet is compiled again
				when it is asked for (a load would find no file) */
				struct dksh_slot *slot = set_find(pending.hash);

				if (slot && !slot->handle)
					slot->broken = 1;
			}
			pthread_mutex_unlock(&dksh.lock);
			free(pending.data);
			if (ok)
				written++;
			else
				failed++;
		}
		if (written || failed)
			host_logf(failed ? HOST_LOG_ERROR : HOST_LOG_INFO, "dk shader: %u written to the %s%s", written,
				dksh.pack ? "pack" : "card", failed ? " (and some could not be: the card full?)" : "");
		folder_clear_some();
	}
	return NULL;
}

/* ---------- starting (the game thread; the card scan is under the lock
and touches no deko3d, so compile may call this too) */

static void *loader_thread(void *unused);

static void service_start(void)
{
	pthread_attr_t attributes;

	if (dksh.started)
		return;
	dksh.started = 1;
	pthread_mutex_init(&dksh.lock, NULL);
	pthread_cond_init(&dksh.queue_condition, NULL);
	pthread_cond_init(&dksh.load_condition, NULL);
	cache_scan();
	pthread_attr_init(&attributes);
	pthread_attr_setstacksize(&attributes, DKSH_THREAD_STACK);
	if (pthread_create(&dksh.compile_thread, &attributes, compile_thread, NULL) != 0)
	{
		host_logf(HOST_LOG_ERROR, "dk shader: the compile thread could not be made; shaders are only loaded, never "
			"compiled");
		dksh.compile_thread = 0;
	}
	else
	{
		pthread_detach(dksh.compile_thread);
		host_logf(HOST_LOG_INFO, "dk shader: one compile thread (UAM %s)", HOST_DK_UAM_COMMIT);
	}
	{
		pthread_t writer;

		if (pthread_create(&writer, &attributes, writer_thread, NULL) == 0)
			pthread_detach(writer);
		else
			host_logf(HOST_LOG_ERROR, "dk shader: the writer thread could not be made; compiled shaders are kept "
				"for this run only");
	}
	{
		pthread_t loader;

		if (pthread_create(&loader, &attributes, loader_thread, NULL) == 0)
		{
			pthread_detach(loader);
			dksh.loader_started = 1;
		}
		else
			host_logf(HOST_LOG_WARN, "dk shader: the loader thread could not be made; shaders are loaded on the "
				"game thread");
	}
	pthread_attr_destroy(&attributes);
}

/* the code memory, on the game thread (the only thread that touches
deko3d). Made once, at the first shader that is asked for; if the console
refuses it nothing is loaded and every find answers "not ready", which the
guest takes as "skip the draw" (phase 6) */
static void code_memory_make(void)
{
	DkMemBlockMaker maker;

	if (dksh.code_memory)
		return;
	dkMemBlockMakerDefaults(&maker, host_dk_device(), DKSH_CODE_SIZE);
	maker.flags = DKSH_CODE_FLAGS;
	dksh.code_memory = dkMemBlockCreate(&maker);
	if (!dksh.code_memory)
		host_logf(HOST_LOG_ERROR, "dk shader: no %u bytes of code memory; no shader is loaded",
			(unsigned)DKSH_CODE_SIZE);
}

/* ---------- loading (the game thread) */

/* a DKSH file into code memory and the table: its handle, or 0 (no file, no
room, or the console refused the code memory). The probe's shader_load,
minus its logging. The file is read under the SD lock (the card's driver is
shared with the guest's file calls and the logger, host.h), and what it has
to say is said with the queue's lock gone */
static uint32_t shader_load(uint64_t hash, uint32_t stage)
{
	char path[1024];
	FILE *file;
	struct dksh_header header;
	unsigned char *contents = NULL;
	long size = 0;
	uint32_t handle = 0;
	/* what to say once the locks are gone */
	int say_invalid = 0, say_room = 0, say_dksh = 0, say_table = 0;
	/* (compiled, not yet written: dksh.pending) */
	int from_memory = 0;
	/* (in the pack, where; or read from its own file in the folder) */
	uint32_t pack_offset = 0, pack_size = 0;
	int from_loose = 0;

	shader_path(path, sizeof(path), stage, hash, 0);
	/* compiled and not yet written: from memory (a copy, as the writer
	frees its own when it has written it) */
	pthread_mutex_lock(&dksh.lock);
	{
		uint32_t index;

		for (index = 0; index < dksh.pending_count; index++)
		{
			if (dksh.pending[index].hash == hash && dksh.pending[index].stage == stage)
			{
				contents = malloc(dksh.pending[index].size);
				if (contents)
				{
					memcpy(contents, dksh.pending[index].data, dksh.pending[index].size);
					size = (long)dksh.pending[index].size;
					from_memory = 1;
				}
				break;
			}
		}
		if (!from_memory)
		{
			struct dksh_slot *slot = set_find(hash);

			if (slot && slot->stage == stage)
			{
				pack_offset = slot->pack_offset;
				pack_size = slot->pack_size;
			}
		}
	}
	pthread_mutex_unlock(&dksh.lock);
	if (!from_memory && pack_size)
	{
		/* from the pack: a seek and a read in the file the run keeps open */
		contents = malloc(pack_size);
		if (contents)
		{
			int read_whole;

			host_sd_lock();
			read_whole = dksh.pack && fseek(dksh.pack, (long)pack_offset, SEEK_SET) == 0 &&
				fread(contents, 1, pack_size, dksh.pack) == pack_size;
			host_sd_unlock();
			if (read_whole)
			{
				size = (long)pack_size;
				from_memory = 1;
			}
			else
			{
				free(contents);
				contents = NULL;
			}
		}
	}
	if (!from_memory)
	{
		host_sd_lock();
		file = fopen(path, "rb");
		if (file)
		{
			fseek(file, 0, SEEK_END);
			size = ftell(file);
			fseek(file, 0, SEEK_SET);
			if (size >= (long)sizeof(header))
			{
				contents = malloc((size_t)size);
				if (contents && fread(contents, 1, (size_t)size, file) != (size_t)size)
				{
					free(contents);
					contents = NULL;
				}
			}
			fclose(file);
		}
		host_sd_unlock();
		from_loose = contents != NULL;
		if (from_loose)
		{
			pthread_mutex_lock(&dksh.lock);
			dksh.loose_loads++;
			pthread_mutex_unlock(&dksh.lock);
		}
	}
	if (contents && size < (long)sizeof(header))
	{
		free(contents);
		contents = NULL;
	}
	if (!contents)
	{
		/* listed, but gone or unreadable: compiled again rather than read
		again */
		pthread_mutex_lock(&dksh.lock);
		{
			struct dksh_slot *slot = set_find(hash);

			if (slot)
				slot->broken = 1;
		}
		pthread_mutex_unlock(&dksh.lock);
		return 0;
	}

	memcpy(&header, contents, sizeof(header));
	if (header.magic != 0x48534b44 || header.control_size + header.code_size > (uint32_t)size)
		say_dksh = 1;
	else
	{
		/* a file of the cache before the pack: into the pack (the writer
		appends it and removes the file) */
		if (from_loose && dksh.pack)
		{
			void *copy = malloc((size_t)size);

			if (copy)
			{
				memcpy(copy, contents, (size_t)size);
				pthread_mutex_lock(&dksh.lock);
				if (!pending_add_locked(hash, stage, copy, (size_t)size, 1))
					free(copy);
				pthread_mutex_unlock(&dksh.lock);
			}
		}
		code_memory_make();
		pthread_mutex_lock(&dksh.lock);
		if (!dksh.code_memory)
			say_room = 1; /* no code memory: nothing can be loaded */
		else if (dksh.loaded_count >= DKSH_TABLE_LIMIT)
			say_table = 1;
		else
		{
			DkShaderMaker maker;
			struct dksh_loaded *loaded = &dksh.loaded[dksh.loaded_count];

			dksh.code_used = (dksh.code_used + DK_SHADER_CODE_ALIGNMENT - 1) & ~(DK_SHADER_CODE_ALIGNMENT - 1);
			if (dksh.code_used + header.code_size <= DKSH_CODE_SIZE - DK_SHADER_CODE_UNUSABLE_SIZE)
			{
				memcpy((unsigned char *)dkMemBlockGetCpuAddr(dksh.code_memory) + dksh.code_used,
					contents + header.control_size, header.code_size);
				dkShaderMakerDefaults(&maker, dksh.code_memory, dksh.code_used);
				maker.control = contents;
				dkShaderInitialize(&loaded->shader, &maker);
				dksh.code_used += header.code_size;
				loaded->hash = hash;
				loaded->stage = stage;
				if (dkShaderIsValid(&loaded->shader))
				{
					struct dksh_slot *slot = set_insert(hash, stage);

					handle = ++dksh.loaded_count;
					if (slot)
						slot->handle = handle;
				}
				else
					say_invalid = 1;
			}
			else
				say_room = 1;
		}
		pthread_mutex_unlock(&dksh.lock);
	}
	/* a load that failed is not tried again on every find: a bad file is
	compiled again, and a shader with no room stays out */
	if (!handle)
	{
		pthread_mutex_lock(&dksh.lock);
		{
			struct dksh_slot *slot = set_find(hash);

			if (slot && (say_dksh || say_invalid))
				slot->broken = 1;
			else if (slot)
				slot->unloadable = 1;
		}
		pthread_mutex_unlock(&dksh.lock);
	}
	free(contents);
	if (say_invalid)
		host_logf(HOST_LOG_ERROR, "dk shader: %c%016llx's DKSH is not valid for the device", stage ? 'f' : 'v',
			(unsigned long long)hash);
	if (say_room)
		host_logf(HOST_LOG_ERROR, "dk shader: no room in the code memory for %c%016llx; it is not loaded", stage ? 'f' : 'v',
			(unsigned long long)hash);
	if (say_table)
		host_logf(HOST_LOG_ERROR, "dk shader: more than %u shaders loaded; %c%016llx is not",
			(unsigned)DKSH_TABLE_LIMIT, stage ? 'f' : 'v', (unsigned long long)hash);
	if (say_dksh)
		host_logf(HOST_LOG_ERROR, "dk shader: %s is not a DKSH file", path);
	return handle;
}

/* The loader thread. A shader on the card was read and put in code memory
on the game thread when a draw first asked for it: an open and a read on the
card each, 3% of the game thread in a match. The find queues it here and
answers that it is coming, and the draw is skipped until then, as for one
being compiled - a few milliseconds, not a compile's seconds, which is why
this is not the compile thread's queue. The code memory is made on the game
thread, before the first load is queued. */
static void *loader_thread(void *unused)
{
	/* how the card keeps up: a batch's loads (until the queue empties, or
	every LOADER_LOG_EVERY), their time and the longest, said in the log */
	enum { LOADER_LOG_EVERY = 256 };
	unsigned long batch = 0;
	u64 batch_ns = 0, longest_ns = 0;

	(void)unused;
	host_thread_place_on_helper_core();
	for (;;)
	{
		uint64_t hash;
		uint32_t stage;
		uint32_t waiting;
		u64 started_ns;

		pthread_mutex_lock(&dksh.lock);
		while (!dksh.load_count)
			pthread_cond_wait(&dksh.load_condition, &dksh.lock);
		hash = dksh.load_queue[0].hash;
		stage = dksh.load_queue[0].stage;
		memmove(&dksh.load_queue[0], &dksh.load_queue[1], (dksh.load_count - 1) * sizeof(dksh.load_queue[0]));
		dksh.load_count--;
		dksh.load_running = 1;
		pthread_mutex_unlock(&dksh.lock);

		started_ns = armTicksToNs(armGetSystemTick());
		shader_load(hash, stage);
		{
			u64 took = armTicksToNs(armGetSystemTick()) - started_ns;

			batch++;
			batch_ns += took;
			if (took > longest_ns)
				longest_ns = took;
			/* a slow load (a file of its own in the big folder, before the
			pack has it) held the card's lock all that time: the others'
			calls get it before the next (the menus ran at a frame a second
			when the loads went back to back) */
			if (took > 20000000ULL)
				usleep(30000);
		}

		pthread_mutex_lock(&dksh.lock);
		{
			struct dksh_slot *slot = set_find(hash);

			if (slot)
				slot->loading = 0;
		}
		dksh.load_running = 0;
		waiting = dksh.load_count;
		{
			uint32_t code_kb = dksh.code_used / 1024, loaded = dksh.loaded_count;

			pthread_mutex_unlock(&dksh.lock);
			if (!waiting || batch >= LOADER_LOG_EVERY)
			{
				host_logf(HOST_LOG_INFO, "dk shader: %lu loaded from the card, %.1f ms each, the longest %.1f ms; "
					"%u waiting; %u loaded in all, %u KB of %u KB of code memory", batch,
					(double)batch_ns / 1e6 / (double)batch, (double)longest_ns / 1e6, (unsigned)waiting,
					(unsigned)loaded, (unsigned)code_kb, (unsigned)(DKSH_CODE_SIZE / 1024));
				batch = 0;
				batch_ns = longest_ns = 0;
			}
		}
	}
	return NULL;
}

/* ---------- the imports (the game thread) */

uint32_t host_dk_shader_find(uint32_t stage, uint64_t hash, uint32_t state_out)
{
	struct dksh_slot *slot;
	uint32_t handle = 0, state = 0;

	service_start();
	if (!dksh.logged_first_hash)
	{
		dksh.logged_first_hash = 1;
		/* the hash crosses as one 64-bit value in a register, and the guest
		logs the same one; this says the two halves agree */
		host_logf(HOST_LOG_INFO, "dk shader: the first shader asked for is %c%016llx", stage ? 'f' : 'v',
			(unsigned long long)hash);
	}

	/* (made here, on the game thread, before anything is loaded) */
	if (dksh.loader_started)
		code_memory_make();
	pthread_mutex_lock(&dksh.lock);
	slot = set_find(hash);
	if (slot && (slot->stage != stage || slot->broken))
		slot = NULL;
	if (slot && slot->handle)
		handle = slot->handle;
	else if (slot && !slot->unloadable && dksh.loader_started)
	{
		/* on the card: the loader's, and coming (the draw is skipped) */
		if (!slot->loading && dksh.load_count < DKSH_LOAD_QUEUE)
		{
			slot->loading = 1;
			dksh.load_queue[dksh.load_count].hash = hash;
			dksh.load_queue[dksh.load_count].stage = stage;
			dksh.load_count++;
			pthread_cond_signal(&dksh.load_condition);
		}
		state = 1;
		slot = NULL;
	}
	else if (!slot)
	{
		/* not on the card: being compiled, or queued? */
		uint32_t index;

		if (dksh.compiling && dksh.compiling_hash == hash && dksh.compiling_stage == stage)
			state = 1;
		for (index = 0; !state && index < dksh.queue_count; index++)
		{
			if (dksh.queue[index].hash == hash && dksh.queue[index].stage == stage)
				state = 1;
		}
	}
	pthread_mutex_unlock(&dksh.lock);

	/* a shader on the card and not yet in the GPU's memory, with no loader
	thread: read and load it now, on this thread - unless loading it failed
	before */
	if (!handle && slot && !slot->unloadable)
		handle = shader_load(hash, stage);
	if (state_out)
		*(uint32_t *)(uintptr_t)state_out = state;
	return handle;
}

const void *host_dk_shader(uint32_t handle)
{
	if (!handle || handle > dksh.loaded_count)
		return NULL;
	return &dksh.loaded[handle - 1].shader;
}

/* what the host has of a shader, without loading it: 0 nothing, 1 queued
or being compiled, 2 on the card, 3 loaded. The guest's startup pass asks
this of every key it knows: loading each on the card there (find) cost the
menus a third of their frames for four seconds on a console with a full
cache. A shader is loaded when it is first wanted - its map's load, under
the loading screen, or its first draw - not all at once at start. */
uint32_t host_dk_shader_known(uint32_t stage, uint64_t hash)
{
	struct dksh_slot *slot;
	uint32_t known = 0, index;

	service_start();
	pthread_mutex_lock(&dksh.lock);
	slot = set_find(hash);
	if (slot && slot->stage == stage && !slot->broken)
		known = slot->handle ? 3 : 2;
	else if (dksh.compiling && dksh.compiling_hash == hash && dksh.compiling_stage == stage)
		known = 1;
	for (index = 0; !known && index < dksh.queue_count; index++)
	{
		if (dksh.queue[index].hash == hash && dksh.queue[index].stage == stage)
			known = 1;
	}
	pthread_mutex_unlock(&dksh.lock);
	return known;
}

uint32_t host_dk_shader_loose_loads(void)
{
	uint32_t loads;

	if (!dksh.started)
		return 0;
	pthread_mutex_lock(&dksh.lock);
	loads = dksh.loose_loads;
	pthread_mutex_unlock(&dksh.lock);
	return loads;
}

void host_dk_shader_preload_done(void)
{
	if (!dksh.preload_done)
		host_logf(HOST_LOG_INFO, "dk shader: the guest's preload has asked for every shader it knows");
	dksh.preload_done = 1;
}

uint32_t host_dk_shader_loading(void)
{
	uint32_t loading;

	if (!dksh.started)
		return 0;
	pthread_mutex_lock(&dksh.lock);
	loading = dksh.load_count + (dksh.load_running ? 1 : 0);
	pthread_mutex_unlock(&dksh.lock);
	return loading;
}

uint32_t host_dk_shader_pending(void)
{
	uint32_t pending;

	/* (the lock is made when the service starts) */
	if (!dksh.started)
		return 0;
	pthread_mutex_lock(&dksh.lock);
	pending = dksh.queue_count + (dksh.compiling ? 1 : 0);
	pthread_mutex_unlock(&dksh.lock);
	return pending;
}

void host_dk_shader_compile(uint32_t stage, uint64_t hash, uint32_t glsl, uint32_t glsl_size, uint32_t priority)
{
	struct dksh_request request;
	uint32_t index;

	service_start();
	pthread_mutex_lock(&dksh.lock);
	{
		struct dksh_slot *slot = set_find(hash);

		/* already on the card (and its file good), or being compiled:
		nothing to ask for */
		if ((slot && !slot->broken) ||
			(dksh.compiling && dksh.compiling_hash == hash && dksh.compiling_stage == stage))
		{
			pthread_mutex_unlock(&dksh.lock);
			return;
		}
	}
	for (index = 0; index < dksh.queue_count; index++)
	{
		/* queued: the priority is raised if the new one is higher, and the
		GLSL it came with compiles the same, so it is dropped */
		if (dksh.queue[index].hash == hash && dksh.queue[index].stage == stage)
		{
			if (priority < dksh.queue[index].priority)
				dksh.queue[index].priority = priority;
			pthread_mutex_unlock(&dksh.lock);
			return;
		}
	}
	pthread_mutex_unlock(&dksh.lock);

	/* the GLSL is copied out of the guest's memory before it is queued: it
	lives in the queue until its compile is done, which the game does not
	wait for, and the guest's buffer is its own again the moment this
	returns */
	memset(&request, 0, sizeof(request));
	request.hash = hash;
	request.stage = stage;
	request.priority = priority;
	request.glsl = malloc(glsl_size + 1);
	if (!request.glsl)
	{
		host_logf(HOST_LOG_ERROR, "dk shader: the GLSL for %c%016llx could not be kept; it is not queued",
			stage ? 'f' : 'v', (unsigned long long)hash);
		return;
	}
	memcpy(request.glsl, (const void *)(uintptr_t)glsl, glsl_size);
	request.glsl[glsl_size] = 0;

	pthread_mutex_lock(&dksh.lock);
	/* another thread may have queued it between the look and the copy */
	for (index = 0; index < dksh.queue_count; index++)
	{
		if (dksh.queue[index].hash == hash && dksh.queue[index].stage == stage)
		{
			if (priority < dksh.queue[index].priority)
				dksh.queue[index].priority = priority;
			pthread_mutex_unlock(&dksh.lock);
			free(request.glsl);
			return;
		}
	}
	if (dksh.queue_count == dksh.queue_capacity)
	{
		uint32_t grown_capacity = dksh.queue_capacity ? dksh.queue_capacity * 2 : 64;
		struct dksh_request *grown = realloc(dksh.queue, grown_capacity * sizeof(*grown));

		if (!grown)
		{
			pthread_mutex_unlock(&dksh.lock);
			free(request.glsl);
			host_logf(HOST_LOG_ERROR, "dk shader: the queue could not grow past %u; %c%016llx is not queued",
				(unsigned)dksh.queue_count, stage ? 'f' : 'v', (unsigned long long)hash);
			return;
		}
		dksh.queue = grown;
		dksh.queue_capacity = grown_capacity;
	}
	dksh.queue[dksh.queue_count++] = request;
	pthread_cond_signal(&dksh.queue_condition);
	pthread_mutex_unlock(&dksh.lock);
}
