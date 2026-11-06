/*
DK_SHADERS.C

The deko3d renderer's shader cache, guest half (port/switch/DEKO3D.md,
phase 5, step 4): the keys, the key files players share, the import of the
OpenGL image's program records, and the startup pass that hands every key
the console knows of to the host (host_dk_shaders.c, through the
host_dk_shader_find and host_dk_shader_compile imports) to be compiled in
the background while the game runs.

A key is data, and that is the point of it (see the plan's "Decisions"): a
compiled shader is GPU code nobody can check, and it stops being valid the
moment the generators or the compiler change; a key just makes a new hash,
and the console compiles again. A vertex shader's key is its program's hash
(the hash of its instruction count and words, kept in the object d3d8_dk.c
makes) and the packed-attribute mask it is drawn with; a pixel shader's is
the nv2a_pixel_shader_key itself, with count_samples set to 0 (it is
Android's occlusion counting; under deko3d the visibility tests count with
DkCounter_SamplesPassed, and it does not change the GLSL). The hash of a
key is FNV-1a 64 over the generators' version and the key's data, so
nothing but the shaders themselves is ever shared.

The key files, in z:\shader_keys\: console.dkk, the keys this console met
first (phase 6's draws append the ones they miss; the import below writes
it once), and keys.dkk, whatever is put there to be shared - the merged
file a release ships, or a file a player copies in (phase 8). console.dkk
never holds a key keys.dkk has: the draws append only keys neither file
holds, the import leaves the shared ones out, and every start rewrites it
without its duplicates and the keys a newer keys.dkk brought
(console_compact) - which the preload, loading every console key at the
start, would otherwise load there rather than with their map. They are read
by these names, not by listing the folder, because the console's host has
no way to list a directory for the guest (host_syscall.c's getdents64: no
libc function and no service for it).

The import: the OpenGL image recorded the keys of everything the player
has played in z:\shader_programs.bin (d3d8_gl.c's program records), and the
first time the deko3d image starts with no console.dkk it converts them -
the vertex shader ids through the id table phase 4's dump keeps (both
images make their vertex shaders from the same table in the same order),
the pixel keys as they are - and writes them as console.dkk. That is also
what tests the writer in this phase.
*/

#include "xgpu.h"
#include "dk_shaders.h"

#include "platform.h"
#include "posix.h"
#include "port_config.h"

#include <pthread.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* the host's half of the cache, through the import stubs (a 64-bit hash
goes in one X register on both sides; the pointers are guest addresses,
zero-extended by the stubs) */
uint32_t host_dk_shader_find(uint32_t stage, uint64_t hash, uint32_t state_out);
void host_dk_shader_compile(uint32_t stage, uint64_t hash, uint32_t glsl, uint32_t glsl_size, uint32_t priority);
uint32_t host_dk_shader_known(uint32_t stage, uint64_t hash);
uint32_t host_dk_shader_pending(void);
uint32_t host_dk_shader_pending_urgent(uint32_t priority);
void host_dk_shader_priority_lower(uint32_t from, uint32_t to);
uint32_t host_dk_shader_loading(void);
uint32_t host_dk_shader_load_first(uint32_t stage, uint64_t hash);
void host_dk_shader_preload_done(void);
uint32_t host_dk_shader_loose_loads(void);

/* the game's console (source/interface/terminal.c, cseries.h), spelled out:
this file sees none of the game's headers */
union real_argb_color;
void terminal_status_printf(union real_argb_color const *color, char const *format, ...);
extern const union real_argb_color *global_real_argb_yellow;

/* d3d8_dk.c: the vertex programs the game has made. The ids are the order
the game makes them in, the same in both images (phase 4's dump relies on
it), and the records name programs by id */
unsigned long d3d8_dk_vertex_shader_count(void);
int d3d8_dk_vertex_program_by_id(unsigned long id, uint64_t *program_hash, const uint32_t **instructions,
	unsigned long *instruction_count);

/* ---------- the keys */

#define DK_KEY_FOLDER "z:\\shader_keys"
#define DK_KEY_CONSOLE_FILE "z:\\shader_keys\\console.dkk"
#define DK_KEY_SHARED_FILE "z:\\shader_keys\\keys.dkk"
/* d3d8_gl.c's program records (phase 4 read them for the dump) */
#define DK_RECORDS_PATH "z:\\shader_programs.bin"

/* how many keys the startup pass takes a frame (DEKO3D.md: generating the
GLSL costs the game thread time, and the game must not be held up) */
#define DK_PASS_PER_FRAME 8
/* the player is told the shaders are being compiled while at least this
many wait (a map's few new ones are not worth a line on screen), and asked
about it every this many frames */
#define DK_STATUS_MINIMUM 16
#define DK_STATUS_EVERY_FRAMES 30
/* the preload: after the startup pass, every key on the card handed to the
host's loader thread, this many a frame and while it has fewer than
DK_PRELOAD_QUEUE waiting; and a map's load waits under the loading screen
for it (and the loader), and for the map's own keys to be compiled, for up
to DK_MAP_WAIT_MS in all (dk_shader_map_loading, dk_shader_map_loaded) */
#define DK_PRELOAD_ENABLED 1
#define DK_PRELOAD_PER_FRAME 16
#define DK_PRELOAD_QUEUE 128
#define DK_MAP_WAIT_MS 10000
/* the startup pass's time a frame, beside its DK_PASS_PER_FRAME keys whose GLSL it makes: a key the host has needs
only to be asked (17942 keys at eight a frame took the pass fifteen seconds, the menus stuttering throughout) */
#define DK_PASS_BUDGET_US 2000
/* and while the first loading screen waits for it (boot_wait), when the game
thread draws only the loading screen: as many keys as a frame's time allows.
At eight a frame (about 240 a second at the loading screen's 30), the 1,569
keys a new keys.dkk brought held the loading screen 8.4 s, and a new console's
17,000 held it its 30 s; the compile threads, which make about 240 a second
on two cores, are then handed the whole queue at once, the urgent first */
#define DK_PASS_BOOT_PER_FRAME 4096
#define DK_PASS_BOOT_BUDGET_US 25000
/* the first loading screen (the menus' map, at the start) stays up until the shader cache has started, its pass and
the preload are done and nothing urgent is compiling, for this long at most, so that the menus come up smooth; the
preload loads this many a frame meanwhile */
#define DK_BOOT_WAIT_MS 30000
#define DK_BOOT_PRELOAD_PER_FRAME 64

/* the key's data as it is in a record: a vertex key or a pixel key,
padded to the pixel key's size (the larger of the two) so a record is one
size whatever it holds. The vertex key's own bytes are the program hash
then the packed mask, in that order - not the struct's, which has four
bytes of padding at its end that would otherwise have to be promised zero
by every caller */
#define DK_KEY_DATA_SIZE 256UL
/* (the pixel key's fields past them - ce_meter_channels, which deko3d never sets - are left 0 in a key read back) */
typedef char dk_key_data_within_pixel_key[sizeof(struct nv2a_pixel_shader_key) >= DK_KEY_DATA_SIZE ? 1 : -1];
#define DK_VERTEX_KEY_BYTES 12UL

struct dk_key_record
{
	uint8_t stage;
	uint8_t reserved[3];
	uint32_t map_hash;
	uint8_t data[DK_KEY_DATA_SIZE];
};
typedef char dk_key_record_size_check[sizeof(struct dk_key_record) == DK_KEY_RECORD_SIZE ? 1 : -1];

struct dk_key
{
	uint32_t stage;
	uint32_t map_hash;
	union
	{
		struct dk_vertex_key vertex;
		struct nv2a_pixel_shader_key pixel;
	} data;
	uint64_t hash;
	/* the pass: 0 not reached, 1 asked of the host (loaded, queued, or
	compiling), 2 skipped (its program the game never made) */
	int asked;
	/* the handle the host gave, once it has one */
	uint32_t handle;
	/* the pass found it on the card (or loaded): nothing to compile */
	int ready;
	/* known only from the shared keys (keys.dkk): loaded with its map, not by the preload */
	int shared;
};

static struct dk_key *keys;
static unsigned long key_count, key_capacity;
static BOOL started, pass_finished;
static unsigned long pass_index;
static BOOL preload_finished;
/* the first loading screen's wait for the start (DK_BOOT_WAIT_MS): going on, and over */
static BOOL boot_waiting, boot_waited;
static unsigned long long boot_started_ms;
static unsigned long preload_index, preload_asked;
/* the keys the preload has to ask for (on the card, not loaded), counted
when it starts, for its message */
static unsigned long preload_total;
static BOOL preload_counted;
static uint32_t current_map;
static int logged_hash;

/* ---------- hashes (dk_shaders.h's) */

uint64_t dk_shader_hash_init(void)
{
	/* FNV-1a 64's offset basis */
	return 0xcbf29ce484222325ULL;
}

uint64_t dk_shader_hash_mix(uint64_t hash, const void *data, unsigned long size)
{
	const unsigned char *bytes = data;

	for (; size; size--)
		hash = (hash ^ (uint64_t)*bytes++) * 0x100000001b3ULL;
	return hash;
}

/* the hash of a key: over the generators' version and the key's data, so a
changed generator makes new hashes and a new set of names on the card. The
pixel key is mixed up to the fields the OpenGL renderer's settings added
after it (per_pixel_lighting, alpha_test_samples: always 0 under deko3d,
whose generator does not read them), and those only when they are not 0,
so that its hashes stay the ones the cards' shaders were compiled under:
mixed whole, their two bytes and the padding after them made every pixel
shader on a card a stranger, compiled again one at a time while the game
ran and its draws skipped till then. The vertex key's fields one by one,
for the padding's sake */
/* The pixel key's bytes hashed whole, frozen as the cards' shaders were
compiled under them: everything before per_pixel_lighting, point_threshold
the last (a field upstream added for the OpenGL renderer, always 0 here; it
moved the old end, and every pixel shader on every card was compiled again
for nothing). The key is checked below to be laid out as it was then: a
field added anywhere fails the build, so whoever adds one decides here how
it is hashed - after these bytes, and only when it is not 0, as
per_pixel_lighting and alpha_test_samples are - instead of every name on
the cards changing with it. */
#define DK_PIXEL_KEY_HASHED_BYTES 253
typedef char dk_pixel_key_point_threshold_where_hashed[
	offsetof(struct nv2a_pixel_shader_key, point_threshold) == 252 ? 1 : -1];
typedef char dk_pixel_key_per_pixel_lighting_after_hashed[
	offsetof(struct nv2a_pixel_shader_key, per_pixel_lighting) == DK_PIXEL_KEY_HASHED_BYTES ? 1 : -1];
typedef char dk_pixel_key_alpha_test_samples_where_hashed[
	offsetof(struct nv2a_pixel_shader_key, alpha_test_samples) == 254 ? 1 : -1];
typedef char dk_pixel_key_size_as_hashed[offsetof(struct nv2a_pixel_shader_key, ce_meter_channels) == DK_KEY_DATA_SIZE ? 1 : -1];

/* the debug options that change the pixel shaders' GLSL (nv2a_psh_dk.c):
mixed into their hash when any is set, so a debug run compiles its own
shaders rather than drawing with the cached ones of a normal run, and a
normal run's names stay as they were */
static uint64_t debug_hash_mix(uint64_t hash)
{
	static int looked;
	static char options[256];
	static unsigned long length;

	if (!looked)
	{
		looked = 1;
		snprintf(options, sizeof(options), "%s|%d|%d%s", config_string("debug.gpu_debug_expression"),
			config_boolean("debug.gpu_debug_texture0") ? 1 : 0, config_boolean("debug.gpu_debug_flat") ? 1 : 0,
			config_boolean("debug.gpu_shader_ranges") ? "" : "|no ranges");
		length = strcmp(options, "|0|0") ? (unsigned long)strlen(options) : 0;
		if (length)
			platform_log("shader cache: pixel shaders made for the debug options \"%s\" (expression|texture0|flat"
				"|ranges)", options);
	}
	return length ? dk_shader_hash_mix(hash, options, length) : hash;
}

static uint64_t key_hash(uint32_t stage, const void *data)
{
	uint32_t version = DK_SHADER_GENERATOR_VERSION;
	uint64_t hash = dk_shader_hash_init();

	hash = dk_shader_hash_mix(hash, &version, sizeof(version));
	if (stage == DK_SHADER_STAGE_VERTEX)
	{
		const struct dk_vertex_key *key = data;

		hash = dk_shader_hash_mix(hash, &key->program_hash, sizeof(key->program_hash));
		hash = dk_shader_hash_mix(hash, &key->packed_mask, sizeof(key->packed_mask));
	}
	else
	{
		const struct nv2a_pixel_shader_key *key = data;

		hash = dk_shader_hash_mix(hash, data, DK_PIXEL_KEY_HASHED_BYTES);
		if (key->texture_modes & NV2A_PIXEL_KEY_WARP_BLEND)
		{
			uint32_t warp_version = DK_WARP_BLEND_VERSION;

			hash = dk_shader_hash_mix(hash, &warp_version, sizeof(warp_version));
		}
		if (key->per_pixel_lighting || key->alpha_test_samples)
		{
			hash = dk_shader_hash_mix(hash, &key->per_pixel_lighting, sizeof(key->per_pixel_lighting));
			hash = dk_shader_hash_mix(hash, &key->alpha_test_samples, sizeof(key->alpha_test_samples));
		}
		hash = debug_hash_mix(hash);
	}
	return hash;
}

/* how many bytes of a record a stage's key takes */
static unsigned long key_data_size(uint32_t stage)
{
	return stage == DK_SHADER_STAGE_VERTEX ? DK_VERTEX_KEY_BYTES : DK_KEY_DATA_SIZE;
}

/* ---------- the key store */

/* The keys by their hash: an open-addressed table of their indices plus one, twice the store's capacity, so that
adding a key does not compare it with every key before it (17942 keys made that 160 million comparisons, four
seconds of the game thread at the start) */
static unsigned long *key_slots;
static unsigned long key_slot_count;

static unsigned long key_slot_of(uint64_t hash)
{
	unsigned long slot = (unsigned long)(hash ^ (hash >> 32)) & (key_slot_count - 1);

	while (key_slots[slot] && keys[key_slots[slot] - 1].hash != hash)
		slot = (slot + 1) & (key_slot_count - 1);
	return slot;
}

/* the table made again for the store's capacity; 0 if there is no memory for it */
static int key_slots_make(unsigned long capacity)
{
	unsigned long count = 1, index;
	unsigned long *made;

	while (count < capacity * 2)
		count <<= 1;
	made = calloc(count, sizeof(*made));
	if (!made)
		return 0;
	free(key_slots);
	key_slots = made;
	key_slot_count = count;
	for (index = 0; index < key_count; index++)
		key_slots[key_slot_of(keys[index].hash)] = index + 1;
	return 1;
}

/* the key of a hash, or NULL */
static struct dk_key *key_find(uint64_t hash)
{
	unsigned long slot;

	if (!key_slot_count)
		return NULL;
	slot = key_slot_of(hash);
	return key_slots[slot] ? &keys[key_slots[slot] - 1] : NULL;
}

/* records are deduplicated by their shader hash as they are read; the
first of a hash wins, which is also what a file a player shares counts on */
static struct dk_key *key_add(uint32_t stage, uint32_t map_hash, const void *data)
{
	uint64_t hash = key_hash(stage, data);

	if (key_slot_count && key_slots[key_slot_of(hash)])
		return NULL;
	if (key_count == key_capacity)
	{
		unsigned long grown_capacity = key_capacity ? key_capacity * 2 : 1024;
		struct dk_key *grown = realloc(keys, grown_capacity * sizeof(*grown));

		if (!grown)
			return NULL;
		keys = grown;
		/* (the table for the new capacity, from the keys where they are now) */
		if (!key_slots_make(grown_capacity))
			return NULL;
		key_capacity = grown_capacity;
	}
	memset(&keys[key_count], 0, sizeof(keys[key_count]));
	keys[key_count].stage = stage;
	keys[key_count].map_hash = map_hash;
	memcpy(&keys[key_count].data, data, key_data_size(stage));
	keys[key_count].hash = hash;
	key_slots[key_slot_of(hash)] = key_count + 1;
	return &keys[key_count++];
}

/* ---------- the key files */

/* reads one file's records into the store; a file whose format differs is
refused, whole (its magic, its format version, or its record size) */
static unsigned long keys_read_file(const char *xbox_path)
{
	char path[512];
	FILE *file;
	uint32_t header[3];
	struct dk_key_record record;
	unsigned long added = 0;

	platform_translate_path(xbox_path, path, sizeof(path));
	if ((file = fopen(path, "rb")) == NULL)
		return 0;
	/* (read a quarter of a megabyte at a time, not stdio's kilobyte: each read is a call to the host and the card's,
	and the shared keys are 4.5 MB) */
	setvbuf(file, NULL, _IOFBF, 256 * 1024);
	if (fread(header, sizeof(header), 1, file) == 1 && header[0] == DK_KEY_MAGIC && header[1] == DK_KEY_FORMAT &&
		header[2] == sizeof(record))
	{
		while (fread(&record, sizeof(record), 1, file) == 1)
		{
			if (record.stage == DK_SHADER_STAGE_VERTEX || record.stage == DK_SHADER_STAGE_PIXEL)
			{
				struct dk_key *key = key_add(record.stage, record.map_hash, record.data);
				int shared = !strcmp(xbox_path, DK_KEY_SHARED_FILE);

				if (key)
				{
					key->shared = shared;
					added++;
				}
				/* (a key console.dkk had first, which keys.dkk has too: the
				shared file's, loaded with its map - console_compact takes it
				out of console.dkk) */
				else if (shared && (key = key_find(key_hash(record.stage, record.data))) != NULL)
					key->shared = 1;
			}
		}
	}
	else
	{
		platform_log("shader keys: %s is not a key file of this build", path);
		/* This console's own file is started again, for console_write to
		write with this build's header. Appended to, as it was, it stayed
		the old build's file: refused at every start, so no key was known
		and every shader was compiled or loaded at its first draw (the draw
		skipped till then), the keys met appended where nothing would read
		them. (Another's file, keys.dkk, is left as it was given.) */
		if (!strcmp(xbox_path, DK_KEY_CONSOLE_FILE))
		{
			fclose(file);
			if (remove(path) == 0)
				platform_log("shader keys: %s removed, to be written again", path);
			return 0;
		}
	}
	fclose(file);
	return added;
}

/* Records for console.dkk, written on a thread of their own. Opening and
writing a file on the card stalls whoever does it, and a draw meets new keys
by the hundred on a map not played before: written from the draw, each one
was two opens and a write on the game thread, and a first online match on a
new map hitched for its first minute (frames of 200-400 ms, the game thread
mostly waiting). The draw queues the record; the writer appends what has
queued in one open, at most once a second. Records still queued when the
game ends are lost, and met again. A queue that fills (the import of the
OpenGL records at the first start, all at once) is written by whoever fills
it, as before. */
#define APPEND_QUEUE_LIMIT 1024
#define APPEND_INTERVAL_US 1000000

static struct dk_key_record append_queue[APPEND_QUEUE_LIMIT];
static unsigned long append_count;
static pthread_mutex_t append_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t append_wake = PTHREAD_COND_INITIALIZER;
static int append_started;
/* (one writer of the file at a time: the thread, or a caller whose queue filled) */
static pthread_mutex_t write_lock = PTHREAD_MUTEX_INITIALIZER;

/* writes records to console.dkk, the header first if the file does not exist yet */
static void console_write(const struct dk_key_record *records, unsigned long count)
{
	static int exists = -1;
	char path[512];
	FILE *file;

	pthread_mutex_lock(&write_lock);
	platform_translate_path(DK_KEY_CONSOLE_FILE, path, sizeof(path));
	if (exists < 0)
	{
		file = fopen(path, "rb");
		exists = file != NULL;
		if (file)
			fclose(file);
	}
	file = fopen(path, exists ? "ab" : "wb");
	if (!file)
	{
		platform_log("shader keys: %s cannot be written; %lu keys are not recorded", path, count);
		pthread_mutex_unlock(&write_lock);
		return;
	}
	if (!exists)
	{
		uint32_t header[3] = { DK_KEY_MAGIC, DK_KEY_FORMAT, sizeof(struct dk_key_record) };

		fwrite(header, sizeof(header), 1, file);
		exists = 1;
	}
	fwrite(records, sizeof(*records), count, file);
	fclose(file);
	pthread_mutex_unlock(&write_lock);
}

static void *console_writer(void *unused)
{
	static struct dk_key_record taken[APPEND_QUEUE_LIMIT];

	(void)unused;
	for (;;)
	{
		unsigned long count;

		pthread_mutex_lock(&append_lock);
		while (!append_count)
			pthread_cond_wait(&append_wake, &append_lock);
		pthread_mutex_unlock(&append_lock);
		/* (the rest of a burst joins it) */
		usleep(APPEND_INTERVAL_US);
		pthread_mutex_lock(&append_lock);
		count = append_count;
		memcpy(taken, append_queue, count * sizeof(*taken));
		append_count = 0;
		pthread_mutex_unlock(&append_lock);
		console_write(taken, count);
	}
	return NULL;
}

/* queues one record for console.dkk, the writer the import and phase 6's
draws share */
static void console_append(uint32_t stage, uint32_t map_hash, const void *data, unsigned long size)
{
	struct dk_key_record record;

	memset(&record, 0, sizeof(record));
	record.stage = (uint8_t)stage;
	record.map_hash = map_hash;
	memcpy(record.data, data, size);
	pthread_mutex_lock(&append_lock);
	if (!append_started)
	{
		pthread_t thread;

		append_started = 1;
		if (pthread_create(&thread, NULL, console_writer, NULL) == 0)
			pthread_detach(thread);
		else
			append_started = -1;
	}
	if (append_started < 0)
	{
		pthread_mutex_unlock(&append_lock);
		console_write(&record, 1);
		return;
	}
	if (append_count == APPEND_QUEUE_LIMIT)
	{
		/* (written here, under the queue's lock, so the order stays) */
		console_write(append_queue, append_count);
		append_count = 0;
	}
	append_queue[append_count++] = record;
	pthread_cond_signal(&append_wake);
	pthread_mutex_unlock(&append_lock);
}

/* ---------- the OpenGL records, imported once */

/* d3d8_gl.c's program record (phase 4 read them for the dump); the struct
is shared, so a record read here is the same size - checked anyway */
struct program_record
{
	uint32_t map_hash;
	uint32_t vertex_id;
	uint32_t variant;
	uint32_t packed_mask;
	struct nv2a_pixel_shader_key key;
};

#define PROGRAM_RECORD_MAGIC 0x31435350UL /* "PSC1" */

static void import_program_records(void)
{
	char path[512];
	FILE *file;
	uint32_t header[2];
	struct program_record record;
	unsigned long records = 0, vertex_keys = 0, pixel_keys = 0, skipped = 0;
	unsigned long id;

	platform_translate_path(DK_RECORDS_PATH, path, sizeof(path));
	if ((file = fopen(path, "rb")) == NULL)
	{
		platform_log("shader keys: no %s to import (nothing has been played under OpenGL here)", DK_RECORDS_PATH);
		return;
	}
	if (fread(header, sizeof(header), 1, file) != 1 || header[0] != PROGRAM_RECORD_MAGIC ||
		header[1] != sizeof(record))
	{
		platform_log("shader keys: %s is not a program records file of this build; nothing is imported", path);
		fclose(file);
		return;
	}
	/* every record's pixel key, and its vertex program and packed mask as
	a vertex key; two keys that differ only in count_samples (the OpenGL
	image's occlusion counting, which does not change the GLSL) are one */
	while (fread(&record, sizeof(record), 1, file) == 1)
	{
		struct dk_vertex_key vertex;
		struct nv2a_pixel_shader_key pixel = record.key;
		uint64_t program_hash;

		records++;
		pixel.count_samples = 0;
		if (key_add(DK_SHADER_STAGE_PIXEL, record.map_hash, &pixel))
			pixel_keys++;
		if (record.vertex_id < d3d8_dk_vertex_shader_count() &&
			d3d8_dk_vertex_program_by_id(record.vertex_id, &program_hash, NULL, NULL))
		{
			vertex.program_hash = program_hash;
			vertex.packed_mask = record.packed_mask;
			if (key_add(DK_SHADER_STAGE_VERTEX, record.map_hash, &vertex))
				vertex_keys++;
		}
		else
			skipped++;
	}
	fclose(file);

	/* the immediate-mode variant of every program the game has made (mask
	0), as phase 4's corpus has them: the game draws immediate-mode
	vertices with every program */
	for (id = 0; id < d3d8_dk_vertex_shader_count(); id++)
	{
		struct dk_vertex_key vertex;
		uint64_t program_hash;

		if (!d3d8_dk_vertex_program_by_id(id, &program_hash, NULL, NULL))
			continue;
		vertex.program_hash = program_hash;
		vertex.packed_mask = 0;
		if (key_add(DK_SHADER_STAGE_VERTEX, 0, &vertex))
			vertex_keys++;
	}

	/* the store, as the console's own key file: what a player sends, and
	what the next start reads instead of importing again */
	for (id = 0; id < key_count; id++)
	{
		/* (never the shared file's: they are keys.dkk's to give) */
		if (!keys[id].shared)
			console_append(keys[id].stage, keys[id].map_hash, &keys[id].data, key_data_size(keys[id].stage));
	}
	platform_log("shader keys: %lu program records imported as %lu keys (%lu vertex, %lu pixel, %lu records' "
		"programs not made), written to %s", records, key_count, vertex_keys, pixel_keys, skipped,
		DK_KEY_CONSOLE_FILE);
}

/* ---------- GLSL for a key */

/* the vertex program a key names, by its hash */
static int vertex_program(uint64_t program_hash, const uint32_t **instructions, unsigned long *instruction_count)
{
	unsigned long id;

	for (id = 0; id < d3d8_dk_vertex_shader_count(); id++)
	{
		uint64_t hash;
		const uint32_t *program;
		unsigned long count;

		if (d3d8_dk_vertex_program_by_id(id, &hash, &program, &count) && hash == program_hash)
		{
			*instructions = program;
			*instruction_count = count;
			return 1;
		}
	}
	return 0;
}

/* whether a vertex key names a program there is GLSL for: one the game has
made, or the fixed-function pipeline's (DK_FIXED_FUNCTION_PROGRAM_HASH) */
static int vertex_known(uint64_t program_hash)
{
	const uint32_t *instructions;
	unsigned long instruction_count;

	return program_hash == DK_FIXED_FUNCTION_PROGRAM_HASH ||
		vertex_program(program_hash, &instructions, &instruction_count);
}

/* a vertex key's GLSL; NULL if it names no program there is GLSL for */
static char *vertex_glsl(const struct dk_vertex_key *key)
{
	const uint32_t *instructions;
	unsigned long instruction_count;

	if (key->program_hash == DK_FIXED_FUNCTION_PROGRAM_HASH)
		return nv2a_dk_fixed_function_glsl();
	if (!vertex_program(key->program_hash, &instructions, &instruction_count))
		return NULL;
	return nv2a_dk_vertex_shader_to_glsl(instructions, instruction_count, key->packed_mask);
}

/* the GLSL for a key, from the generators (phase 4); NULL if the game has
not made the program a vertex key names (a record from another build, or
one the game never makes: the key is kept and skipped) */
static char *key_glsl(const struct dk_key *key)
{
	if (key->stage == DK_SHADER_STAGE_VERTEX)
		return vertex_glsl(&key->data.vertex);
	{
		struct nv2a_pixel_shader_key pixel = key->data.pixel;

		pixel.count_samples = 0;
		return nv2a_dk_pixel_shader_to_glsl(&pixel);
	}
}

/* the first hash handed to the host, logged beside the host's own first
(the two halves agree that a 64-bit value travels in one register) */
static void hash_log_once(uint64_t hash)
{
	if (!logged_hash)
	{
		logged_hash = 1;
		platform_log("shader cache: the first key handed to the host hashes to %016llx",
			(unsigned long long)hash);
	}
}

/* asks for one key: a handle if the shader is ready, and the key queued
with the host if it was unknown to it. The priority is the caller's */
static uint32_t key_ask(struct dk_key *key, uint32_t priority)
{
	uint32_t state = 0;
	uint32_t handle;

	hash_log_once(key->hash);
	handle = host_dk_shader_find(key->stage, key->hash, (uint32_t)(uintptr_t)&state);
	if (handle)
		return handle;
	if (state == 1)
	{
		/* queued or compiling: raise it in the queue, if the caller is
		more urgent than whoever queued it (no GLSL is wanted for that) */
		host_dk_shader_compile(key->stage, key->hash, 0, 0, priority);
		return 0;
	}
	{
		char *glsl = key_glsl(key);

		if (!glsl)
			return 0;
		host_dk_shader_compile(key->stage, key->hash, (uint32_t)(uintptr_t)glsl, (uint32_t)strlen(glsl), priority);
		free(glsl);
	}
	return 0;
}

/* the startup pass's question for one key: whether the host has it, asked
without loading it (host_dk_shader_known) - loading every shader on the card
here cost the menus a third of their frames for four seconds. A key the
host has not got is queued; one already queued is raised to the priority.
TRUE if the shader is on the card or loaded */
static BOOL pass_ask(struct dk_key *key, uint32_t priority, int *generated)
{
	uint32_t known;

	*generated = 0;
	hash_log_once(key->hash);
	known = host_dk_shader_known(key->stage, key->hash);
	if (known >= 2)
		return TRUE;
	if (known == 1)
	{
		host_dk_shader_compile(key->stage, key->hash, 0, 0, priority);
		return FALSE;
	}
	{
		char *glsl = key_glsl(key);

		*generated = 1;
		if (glsl)
		{
			host_dk_shader_compile(key->stage, key->hash, (uint32_t)(uintptr_t)glsl, (uint32_t)strlen(glsl),
				priority);
			free(glsl);
		}
	}
	return FALSE;
}

/* ---------- start, the pass, maps, draws */

/* console.dkk rewritten without what it should not hold - a record of a
key it holds already (older builds appended some twice), and a key keys.dkk
has (keys_read_file marked them shared: a newer keys.dkk brought them) - if
it holds any: the records kept go to a file beside it, which then takes its
place, so a write cut short leaves the file as it was. Before anything is
appended (dk_shader_start), and under the writer's lock all the same. */
static void console_compact(void)
{
	char path[512], new_path[520];
	FILE *file, *out = NULL;
	uint32_t header[3];
	struct dk_key_record record;
	unsigned long kept = 0, duplicates = 0, shared = 0;
	/* which keys of the store a record of this file has given already (each
	record's key is in the store: keys_read_file put it there) */
	unsigned char *seen = calloc(key_count ? key_count : 1, 1);
	int pass;

	if (!seen)
		return;

	platform_translate_path(DK_KEY_CONSOLE_FILE, path, sizeof(path));
	snprintf(new_path, sizeof(new_path), "%s.new", path);
	pthread_mutex_lock(&write_lock);
	/* two passes over it: the first counts what would go, the second (only
	if anything does) writes what stays */
	for (pass = 0; pass < 2; pass++)
	{
		if ((file = fopen(path, "rb")) == NULL)
			break;
		setvbuf(file, NULL, _IOFBF, 256 * 1024);
		if (fread(header, sizeof(header), 1, file) != 1 || header[0] != DK_KEY_MAGIC || header[1] != DK_KEY_FORMAT ||
			header[2] != sizeof(record))
		{
			fclose(file);
			break;
		}
		if (pass == 1)
		{
			if ((out = fopen(new_path, "wb")) == NULL)
			{
				platform_log("shader keys: %s cannot be written; %s keeps its %lu keys to drop till the next start",
					new_path, path, duplicates + shared);
				fclose(file);
				break;
			}
			setvbuf(out, NULL, _IOFBF, 256 * 1024);
			fwrite(header, sizeof(header), 1, out);
		}
		memset(seen, 0, key_count ? key_count : 1);
		while (fread(&record, sizeof(record), 1, file) == 1)
		{
			struct dk_key *key;
			unsigned long index;

			if (record.stage != DK_SHADER_STAGE_VERTEX && record.stage != DK_SHADER_STAGE_PIXEL)
				continue;
			key = key_find(key_hash(record.stage, record.data));
			if (!key)
				continue;
			if (key->shared)
			{
				shared += pass == 0;
				continue;
			}
			index = (unsigned long)(key - keys);
			if (seen[index])
			{
				duplicates += pass == 0;
				continue;
			}
			seen[index] = 1;
			if (pass == 1)
			{
				fwrite(&record, sizeof(record), 1, out);
				kept++;
			}
		}
		fclose(file);
		if (pass == 0 && !duplicates && !shared)
			break;
	}
	if (out)
	{
		int written = !ferror(out);

		written = fclose(out) == 0 && written;
		if (written && rename(new_path, path) == 0)
			platform_log("shader keys: %s rewritten with its %lu keys, without %lu records of keys it held already "
				"and %lu of keys keys.dkk has", path, kept, duplicates, shared);
		else
		{
			remove(new_path);
			platform_log("shader keys: %s could not be rewritten; it keeps its %lu keys to drop till the next start",
				path, duplicates + shared);
		}
	}
	free(seen);
	pthread_mutex_unlock(&write_lock);
}

void dk_shader_start(void)
{
	char path[512], folder[512];
	unsigned long before = key_count;
	FILE *console;

	if (started)
		return;
	started = TRUE;
	platform_translate_path(DK_KEY_FOLDER, folder, sizeof(folder));
	posix_make_directory(folder);
	platform_translate_path(DK_KEY_CONSOLE_FILE, path, sizeof(path));
	console = fopen(path, "rb");
	if (console)
		fclose(console);
	keys_read_file(DK_KEY_CONSOLE_FILE);
	keys_read_file(DK_KEY_SHARED_FILE);
	if (console)
		console_compact();
	if (!console)
		import_program_records();
	else if (key_count == before)
		platform_log("shader keys: %s holds no keys of this build", DK_KEY_CONSOLE_FILE);
	/* the fixed-function pipeline's (the loading screen's draws), which no
	key file needs to hold: made at the startup pass, before the first
	map's load wants it */
	{
		struct dk_vertex_key fixed_function;

		memset(&fixed_function, 0, sizeof(fixed_function));
		fixed_function.program_hash = DK_FIXED_FUNCTION_PROGRAM_HASH;
		key_add(DK_SHADER_STAGE_VERTEX, 0, &fixed_function);
	}
	platform_log("shader cache: %lu keys known; the rest is compiled in the background while the game runs",
		key_count);
}

/* The preload. A shader on the card was loaded at its first draw, or at
its map's load if that map had met it first, so the shaders the maps share
(most of them, met first on another map) came a draw at a time at the start
of a match, one file each from the card: half a minute of missing objects.
After the startup pass, every key the pass found on the card is handed to
the host's loader thread (which reads it on its own core), a few a frame
while the menus are up. */
static void preload_frame(void)
{
	unsigned long taken = 0;

	/* (a shader was one file of thousands in one folder, about 80 ms to
	open on the card; the host's pack makes it one read, and the loader
	paces the files still to move into it, so the menus keep the card) */
	if (!DK_PRELOAD_ENABLED)
		return;
	if (!pass_finished || preload_finished)
		return;
	if (!preload_counted)
	{
		unsigned long index;

		preload_counted = TRUE;
		for (index = 0; index < key_count; index++)
			preload_total += keys[index].ready && !keys[index].handle && !keys[index].shared ? 1 : 0;
	}
	if (host_dk_shader_loading() >= DK_PRELOAD_QUEUE)
		return;
	while (taken < (boot_waiting ? DK_BOOT_PRELOAD_PER_FRAME : DK_PRELOAD_PER_FRAME) && preload_index < key_count)
	{
		struct dk_key *key = &keys[preload_index++];
		uint32_t state = 0;

		/* (a key only the shared keys hold is its map's to load: the shared keys are every map's, 17939
		shaders, which loaded at the start took the preload 14 s) */
		if (!key->ready || key->handle || key->shared)
			continue;
		/* (loaded: its handle; on the card: queued for the loader) */
		key->handle = host_dk_shader_find(key->stage, key->hash, (uint32_t)(uintptr_t)&state);
		taken++;
		preload_asked++;
	}
	if (preload_index >= key_count)
	{
		preload_finished = TRUE;
		platform_log("shader cache: the preload has asked for every shader on the card (%lu)", preload_asked);
		/* (what is left of the cache's old folder is its leftovers) */
		host_dk_shader_preload_done();
	}
}

/* While many shaders wait to be compiled - the first start after an update
that changed them, when the cache on the card no longer matches - the game
is slow to answer: the menus wait for the card, which the compile writes
to. The console says so while it lasts, and once when it is done. */
static void status_frame(void)
{
	static unsigned long frames;
	static BOOL shown;
	uint32_t pending;

	if (++frames < DK_STATUS_EVERY_FRAMES)
		return;
	frames = 0;
	pending = host_dk_shader_pending();
	if (pending >= DK_STATUS_MINIMUM || (shown && pending))
	{
		shown = TRUE;
		terminal_status_printf(global_real_argb_yellow,
			"Compiling shaders after an update: %u left. The game may be slow to respond until it is done.",
			(unsigned)pending);
		return;
	}
	if (shown)
	{
		shown = FALSE;
		terminal_status_printf(global_real_argb_yellow, "Shaders compiled.");
		return;
	}
	/* the preload (for watching it; to go once it is known to be quick):
	"moving" while it reads the cache before the pack, a file a shader,
	into the pack (a first run's), else "preloading"; left: the keys it has
	not asked for yet and the loads waiting */
	{
		static BOOL preload_shown;
		uint32_t loading = host_dk_shader_loading();
		unsigned long left = (preload_total > preload_asked ? preload_total - preload_asked : 0) + loading;

		if (preload_counted && (!preload_finished || loading))
		{
			preload_shown = TRUE;
			terminal_status_printf(global_real_argb_yellow, host_dk_shader_loose_loads() ?
				"Moving shaders to the new cache: %lu left." : "Preloading shaders: %lu left.", left);
		}
		else if (preload_shown && preload_finished)
		{
			preload_shown = FALSE;
			terminal_status_printf(global_real_argb_yellow, "Shaders preloaded.");
		}
	}
}

/* the startup pass: eight keys a frame (limit), each asked of the host in
turn and queued if the host does not have it. Runs until every key is asked,
and says when it is done - the second launch's numbers come from these lines */
static unsigned long long now_us(void);

static void pass_frame(unsigned long limit, unsigned long long budget_us)
{
	unsigned long taken = 0, asked = 0;
	unsigned long long started = budget_us ? now_us() : 0;

	if (pass_finished)
		return;
	while (taken < limit && pass_index < key_count)
	{
		struct dk_key *key = &keys[pass_index++];
		int generated;

		/* (the clock read every 64 keys: a read is a call to the host) */
		if (budget_us && !(++asked % 64) && now_us() - started >= budget_us)
			break;

		if (key->asked)
			continue;
		/* a vertex key whose program the game has not made: kept and
		skipped, and it costs nothing, so it does not count against the
		frame's eight */
		if (key->stage == DK_SHADER_STAGE_VERTEX && !vertex_known(key->data.vertex.program_hash))
		{
			key->asked = 2;
			continue;
		}
		key->ready = pass_ask(key, key->map_hash && key->map_hash == current_map ? DK_SHADER_PRIORITY_MAP :
			DK_SHADER_PRIORITY_REST, &generated);
		key->asked = 1;
		/* (only a key whose GLSL was made counts against the frame's DK_PASS_PER_FRAME) */
		taken += generated;
	}
	if (pass_index < key_count)
		return;
	pass_finished = TRUE;
	{
		unsigned long index, on_card = 0, queued = 0, skipped = 0;

		for (index = 0; index < key_count; index++)
		{
			if (keys[index].ready || keys[index].handle)
				on_card++;
			else if (keys[index].asked == 2)
				skipped++;
			else
				queued++;
		}
		platform_log("shader cache: the startup pass is done: %lu keys (%lu ready, %lu left to the background "
			"compile, %lu skipped: programs not made)", key_count, on_card, queued, skipped);
	}
}

static void stall_frame(void);

void dk_shader_frame(void)
{
	if (!started)
		return;
	stall_frame();
	status_frame();
	preload_frame();
	if (boot_waiting)
		pass_frame(DK_PASS_BOOT_PER_FRAME, DK_PASS_BOOT_BUDGET_US);
	else
		pass_frame(DK_PASS_PER_FRAME, DK_PASS_BUDGET_US);
}

/* The first seconds of a map's play, campaign or multiplayer: a draw whose
shader is not ready waits for it (the loader reading it from the card, or a
compile thread making it) instead of being skipped, so the level does not
appear a piece at a time - the shaders the maps share are loaded at a draw,
not with the map that did not meet them first. DK_STALL_WINDOW_MS from the
map's first draw, DK_STALL_DRAW_MS at most for a draw and DK_STALL_TOTAL_MS
in all (a network game's other machines wait on a host's frames); then, and
past either, a draw skips as it always has. */
#define DK_STALL_WINDOW_MS 10000
#define DK_STALL_DRAW_MS 500
#define DK_STALL_TOTAL_MS 3000

static BOOL stall_armed;
static BOOL stall_open;
static unsigned long long stall_until_ms;
static unsigned long long stall_spent_us;
static unsigned long stall_given_up;

/* what the window's waits were for, for its line in the log: a shader being
compiled (queued, or a compile thread's: it is loaded once it is made), or
one on the card being loaded; and its frames, how many waited and the most a
frame waited */
enum
{
	STALL_COMPILE,
	STALL_LOAD,
	STALL_KINDS
};

static struct
{
	unsigned long draws;
	unsigned long long spent_us, longest_us;
} stall_kinds[STALL_KINDS];
static unsigned long stall_frames, stall_frames_waited;
static unsigned long long stall_frame_us, stall_frame_longest_us;

/* The wait for a map's shaders under the loading screen, DK_MAP_WAIT_MS at
most from when its load began: first while the loading screen is drawn, as
the map is read into the cache (dk_shader_map_loading, dk_shader_map_waiting:
the game's own loop, game_precache_new_map), until the map's keys are
compiled; then in scenario_load, where nothing is drawn, for what the
loader has to read from the card and anything of the map's still being
compiled (dk_shader_map_loaded). A map read into the cache before (nothing
to show the loading screen for) has only the second. */
static uint32_t wait_map;
static unsigned long long wait_started_ms;
/* the most shaders the wait has seen still to load or compile, which its progress is measured against */
static uint32_t wait_most;

static unsigned long long now_us(void)
{
	struct timespec now;

	clock_gettime(CLOCK_MONOTONIC, &now);
	return (unsigned long long)now.tv_sec * 1000000ULL + (unsigned long long)now.tv_nsec / 1000ULL;
}

static unsigned long long now_ms(void)
{
	return now_us() / 1000ULL;
}

/* what is left of the map's wait */
static unsigned long wait_left_ms(void)
{
	unsigned long long waited = now_ms() - wait_started_ms;

	return waited < DK_MAP_WAIT_MS ? (unsigned long)(DK_MAP_WAIT_MS - waited) : 0;
}

/* the map's keys go to the front of the background compile's queue, raised
in priority, and its wait starts; once a load (the loading screen's and
scenario_load's, for the same map, share it) */
static void map_wait_begin(uint32_t map_hash)
{
	unsigned long index, raised = 0;

	current_map = map_hash;
	if (!started || wait_map == map_hash)
		return;
	wait_map = map_hash;
	wait_started_ms = now_ms();
	wait_most = 0;
	/* (the map before's left in the queue are the rest's again: this map's
	wait is for its own) */
	host_dk_shader_priority_lower(DK_SHADER_PRIORITY_MAP, DK_SHADER_PRIORITY_REST);
	/* (and the draws' of the map before, which a draw of this one raises again if it wants one) */
	host_dk_shader_priority_lower(DK_SHADER_PRIORITY_DRAW, DK_SHADER_PRIORITY_REST);
	for (index = 0; index < key_count; index++)
	{
		if (keys[index].map_hash != map_hash || keys[index].handle)
			continue;
		if (key_ask(&keys[index], DK_SHADER_PRIORITY_MAP))
			continue;
		raised++;
	}
	platform_log("shader cache: the map's keys raised in the compile queue (%lu)", raised);
}

/* the map the loading screen is being drawn for (game_precache_new_map,
d3d8_gl_map_loading) */
void dk_shader_map_loading(uint32_t map_hash)
{
	if (!boot_waited && !boot_waiting)
	{
		boot_waiting = TRUE;
		boot_started_ms = now_ms();
	}
	map_wait_begin(map_hash);
}

/* the first loading screen's wait: whether the start is still going on (the cache not started yet, its pass or the
preload not done, a shader still to load or something urgent to compile), with time left */
static void map_wait_begin(uint32_t map_hash);

static int boot_wait(void)
{
	static unsigned long long said;
	static BOOL raised;
	unsigned long long waited = now_ms() - boot_started_ms;

	/* the menus' keys raised once the pass is done (their loading screen came before the cache had started, and
	a key only the shared keys hold is not the preload's), so that their shaders are loaded before the menus show */
	if (started && pass_finished && !raised)
	{
		raised = TRUE;
		wait_map = 0;
		map_wait_begin(current_map);
	}
	uint32_t urgent = started ? host_dk_shader_pending_urgent(DK_SHADER_PRIORITY_MAP) : 0;
	uint32_t loading = started ? host_dk_shader_loading() : 0;

	if (waited < DK_BOOT_WAIT_MS && (!started || !pass_finished || (DK_PRELOAD_ENABLED && !preload_finished) ||
		loading || urgent))
	{
		if (now_ms() - said >= 1000)
		{
			said = now_ms();
			terminal_status_printf(global_real_argb_yellow, "Preparing shaders: %lu to check, %lu to load, %u to "
				"compile.", started ? key_count - pass_index : key_count,
				started && preload_counted && preload_total > preload_asked ? preload_total - preload_asked : 0UL,
				(unsigned)urgent);
		}
		return 1;
	}
	boot_waiting = FALSE;
	boot_waited = TRUE;
	wait_most = 0;
	platform_log("shader cache: the first loading screen waited %llu ms for the start (%s; the pass %s, the preload "
		"%s, %u loading, %u urgent to compile)", waited, waited >= DK_BOOT_WAIT_MS ? "the most it waits" : "all done",
		pass_finished ? "done" : "not done", preload_finished ? "done" : "not done", (unsigned)loading, (unsigned)urgent);
	return 0;
}

/* how far a wait has come, 0 to 1: the shaders still to load or compile against the most it has seen, and no less
than its share of its time (so that it is at the end when the wait gives up) */
static float wait_fraction(uint32_t left, unsigned long long waited_ms, unsigned long long most_ms)
{
	float done, timed = (float)waited_ms / (float)most_ms;

	if (left > wait_most)
		wait_most = left;
	done = wait_most ? 1.0f - (float)left / (float)wait_most : 1.0f;
	if (timed > done)
		done = timed;
	return done > 1.0f ? 1.0f : done;
}

/* How far the shaders' part of a loading screen has come, 0 to 1 (half of the loading screen's progress, the map's
copy the other half: game_precache_new_map): at the first loading screen the start's work - the pass, the preload,
and what the menus need loaded or compiled - and at a map's, its shaders still to compile; 1 with nothing to wait
for. The light sweeps across the screen as the progress goes, which the copy alone (0.3 s for the menus' map read
ahead) sent across at once, the screen then dark for the rest of the wait. Below 0 when a map's load has had nothing
to wait for: its progress is the copy's alone. */
float dk_shader_map_progress(void)
{
	if (boot_waiting)
	{
		float pass = started && key_count ? (float)pass_index / (float)key_count : 0.0f;
		float preload = !started || !pass_finished ? 0.0f : !DK_PRELOAD_ENABLED || preload_finished ? 1.0f :
			preload_counted && preload_total ? (float)preload_asked / (float)preload_total : 0.0f;
		uint32_t left = started && pass_finished ?
			host_dk_shader_loading() + host_dk_shader_pending_urgent(DK_SHADER_PRIORITY_MAP) : 1;

		if (preload > 1.0f)
			preload = 1.0f;
		return pass * 0.4f + preload * 0.4f +
			wait_fraction(left, 0, DK_BOOT_WAIT_MS) * (started && pass_finished ? 0.2f : 0.0f);
	}
	if (!started || !wait_map)
		return -1.0f;
	{
		float done = wait_fraction(host_dk_shader_pending_urgent(DK_SHADER_PRIORITY_MAP), now_ms() - wait_started_ms,
			DK_MAP_WAIT_MS);

		return wait_most ? done : -1.0f;
	}
}

/* whether the loading screen stays up for the map's keys: some still to
compile (or a draw's, ahead of them), with time left */
int dk_shader_map_waiting(void)
{
	static unsigned long long said;
	uint32_t pending;

	if (boot_waiting && boot_wait())
		return 1;
	if (!started || !wait_map || !wait_left_ms())
		return 0;
	pending = host_dk_shader_pending_urgent(DK_SHADER_PRIORITY_MAP);
	if (!pending)
		return 0;
	/* (the console says how many, once a second) */
	if (now_ms() - said >= 1000)
	{
		said = now_ms();
		terminal_status_printf(global_real_argb_yellow, "Compiling this map's shaders: %u left.", (unsigned)pending);
	}
	return 1;
}

/* the map the game finished loading (d3d8_gl_map_loaded) */
void dk_shader_map_loaded(uint32_t map_hash)
{
	map_wait_begin(map_hash);
	if (!started)
		return;
	/* under the loading screen (scenario_load), the preload finished, the
	loader's queue emptied and the map's keys compiled, for what is left of
	DK_MAP_WAIT_MS: a match starting before then would draw nothing of what
	it has not loaded yet. (A network game's client tolerates this: 75 s
	before its game starts.) */
	{
		unsigned long waited = 0, left = wait_left_ms();

		/* the rest of the startup pass first, which the preload follows: a
		map started within the pass's first seconds waited for none of it,
		and drew a second and more of the level with draws missing (frames
		of 1500 skipped), the intro's fade and bars among them. Asking the
		host is cheap for a shader on the card (it loads nothing) */
		pass_frame(key_count, 0);
		while (pass_finished && waited < left &&
			((DK_PRELOAD_ENABLED && !preload_finished) || host_dk_shader_loading() ||
			host_dk_shader_pending_urgent(DK_SHADER_PRIORITY_MAP)))
		{
			preload_frame();
			usleep(10000);
			waited += 10;
		}
		platform_log("shader cache: waited %llu ms for the map's shaders, %lu of them in scenario_load (%u still to "
			"load, %u of the map's still to compile%s)", now_ms() - wait_started_ms, waited,
			(unsigned)host_dk_shader_loading(), (unsigned)host_dk_shader_pending_urgent(DK_SHADER_PRIORITY_MAP),
			preload_finished ? "" : ", the preload not done");
	}
	/* (the next load of a map, this one again too, waits anew) */
	wait_map = 0;
	/* and its first seconds of play wait for their shaders (stall_wait) */
	stall_armed = TRUE;
}

/* for phase 6: the shader a draw needs. 0 means not ready, and the draw is
skipped. A key unknown to the host is generated and queued at the draw's
priority, and kept in console.dkk the first time this console meets it, so
that the next console that meets it compiles it too; one queued or being
compiled is only raised in the queue - a draw that needs it comes back every
frame until it is ready, and must not generate it, queue it or write it down
again each time. The guest-side table of hash to handle is what keeps the
import off the draw's path: a hit never crosses. */

/* open addressing over a power of two of slots, at most seven eighths
full, so a probe always ends; a full table only means more draws ask the
host, which answers the same */
#define HANDLE_SLOTS 16384

struct handle_entry
{
	uint64_t hash;
	uint32_t handle;
};
static struct handle_entry handle_table[HANDLE_SLOTS];
static unsigned long handle_count;

static struct handle_entry *handle_slot(uint64_t hash)
{
	unsigned long slot = (unsigned long)(hash ^ (hash >> 32)) & (HANDLE_SLOTS - 1);

	while (handle_table[slot].hash && handle_table[slot].hash != hash)
		slot = (slot + 1) & (HANDLE_SLOTS - 1);
	return &handle_table[slot];
}

/* debug.gpu_glsl_folder: a shader the game draws with, written once - the
first time its handle is kept for draws - as <folder>/<v or f><hash>.glsl,
the name the GPU profile's capture logs it by (port/switch/host/host_dk.c).
The folder must be there already. */
static void glsl_write(uint32_t stage, uint64_t hash, const void *key_data)
{
	static const char *folder;
	static int looked;
	char path[512];
	char *glsl = NULL;
	FILE *file;

	if (!looked)
	{
		looked = 1;
		folder = config_string("debug.gpu_glsl_folder");
	}
	if (!folder || !folder[0])
		return;
	if (stage == DK_SHADER_STAGE_VERTEX)
		glsl = vertex_glsl(key_data);
	else
		glsl = nv2a_dk_pixel_shader_to_glsl((const struct nv2a_pixel_shader_key *)key_data);
	if (!glsl)
		return;
	snprintf(path, sizeof(path), "%s/%c%016llx.glsl", folder, stage == DK_SHADER_STAGE_PIXEL ? 'f' : 'v',
		(unsigned long long)hash);
	file = fopen(path, "w");
	if (file)
	{
		fputs(glsl, file);
		fclose(file);
	}
	else
		platform_log("shader cache: cannot write %s", path);
	free(glsl);
}

/* the window closed, and what it waited for said */
static void stall_close(void)
{
	stall_open = FALSE;
	platform_log("shader cache: the map's first %d s: %lu of %lu frames waited for shaders, the longest %llu ms; "
		"%lu waits for a compile, %llu ms in all, the longest %llu ms; %lu for a load from the card, %llu ms, the "
		"longest %llu ms; %lu gave up", DK_STALL_WINDOW_MS / 1000, stall_frames_waited, stall_frames,
		stall_frame_longest_us / 1000ULL, stall_kinds[STALL_COMPILE].draws,
		stall_kinds[STALL_COMPILE].spent_us / 1000ULL, stall_kinds[STALL_COMPILE].longest_us / 1000ULL,
		stall_kinds[STALL_LOAD].draws, stall_kinds[STALL_LOAD].spent_us / 1000ULL,
		stall_kinds[STALL_LOAD].longest_us / 1000ULL, stall_given_up);
}

/* each frame (dk_shader_frame): the frame before counted, if the window is
open, and the window closed once its time is up even with no draw waiting */
static void stall_frame(void)
{
	if (!stall_open)
		return;
	stall_frames++;
	if (stall_frame_us)
	{
		stall_frames_waited++;
		if (stall_frame_us > stall_frame_longest_us)
			stall_frame_longest_us = stall_frame_us;
	}
	stall_frame_us = 0;
	if (now_ms() >= stall_until_ms)
		stall_close();
}

/* a dk_shader_variant_for_draw is asking; and whether it found the host did
not know the key */
static BOOL asking_variant;
static BOOL variant_unknown;

/* whether a draw now may wait for its shader (the window opened at the
map's first draw, and said when it closes) */
static BOOL stall_allowed(void)
{
	unsigned long long now;

	/* (a variant with a stock shader to fall back on waits for nothing, and
	spends none of the window's time: dk_shader_variant_for_draw) */
	if (asking_variant)
		return FALSE;
	now = now_ms();

	if (stall_armed)
	{
		stall_armed = FALSE;
		stall_open = TRUE;
		stall_until_ms = now + DK_STALL_WINDOW_MS;
		stall_spent_us = 0;
		stall_given_up = 0;
		memset(stall_kinds, 0, sizeof(stall_kinds));
		stall_frames = stall_frames_waited = 0;
		stall_frame_us = stall_frame_longest_us = 0;
	}
	if (!stall_open)
		return FALSE;
	if (now >= stall_until_ms)
	{
		stall_close();
		return FALSE;
	}
	return stall_spent_us < DK_STALL_TOTAL_MS * 1000ULL;
}

/* a draw's shader waited for (its compile or load already asked for): the
handle, or 0 if it did not come in time */
static uint32_t stall_wait(uint32_t stage, uint64_t hash)
{
	unsigned long long started = now_us(), waited = 0, most = DK_STALL_DRAW_MS * 1000ULL;
	uint32_t handle = 0, state;
	/* (on the card and not yet loaded: a load's; else a compile's) */
	int kind = host_dk_shader_known(stage, hash) == 2 ? STALL_LOAD : STALL_COMPILE;

	if (most > DK_STALL_TOTAL_MS * 1000ULL - stall_spent_us)
		most = DK_STALL_TOTAL_MS * 1000ULL - stall_spent_us;
	while (waited < most)
	{
		/* (read next, if it is on the card: after a compile it is queued
		behind the map's loads like any other) */
		host_dk_shader_load_first(stage, hash);
		usleep(1000);
		state = 0;
		handle = host_dk_shader_find(stage, hash, (uint32_t)(uintptr_t)&state);
		waited = now_us() - started;
		if (handle || !state)
			break;
	}
	stall_spent_us += waited;
	stall_frame_us += waited;
	stall_kinds[kind].draws++;
	stall_kinds[kind].spent_us += waited;
	if (waited > stall_kinds[kind].longest_us)
		stall_kinds[kind].longest_us = waited;
	if (!handle)
		stall_given_up++;
	return handle;
}

/* stall_wait, and the handle it brings kept as a found one is */
static uint32_t stall_found(uint32_t stage, uint64_t hash, const void *data)
{
	uint32_t handle = stall_wait(stage, hash);
	struct handle_entry *entry;

	if (!handle)
		return 0;
	entry = handle_slot(hash);
	if (entry->hash != hash && handle_count < HANDLE_SLOTS / 8 * 7)
	{
		entry->hash = hash;
		entry->handle = handle;
		handle_count++;
		glsl_write(stage, hash, data);
	}
	return handle;
}

uint32_t dk_shader_for_draw(uint32_t stage, const void *key_data)
{
	union
	{
		struct dk_vertex_key vertex;
		struct nv2a_pixel_shader_key pixel;
	} data;
	unsigned long size = key_data_size(stage);
	struct handle_entry *entry;
	uint64_t hash;
	uint32_t state = 0;
	uint32_t handle;
	/* each stage's last key and its handle: draws in a row mostly share
	their shaders, and the copy and the hash below are a byte at a time
	(dk_shader_for_draw was 3.8% of the game thread in a match) */
	static uint32_t last_key[2][(sizeof(struct nv2a_pixel_shader_key) + 3) / 4];
	static uint32_t last_handle[2];
	int which = stage == DK_SHADER_STAGE_PIXEL;
	const uint32_t *words = key_data;
	unsigned long word;

	if (last_handle[which] && !(size % 4) && !((uintptr_t)key_data % 4))
	{
		for (word = 0; word < size / 4 && words[word] == last_key[which][word]; word++)
			;
		if (word == size / 4)
			return last_handle[which];
	}

	/* the key as it is known: a pixel key with count_samples at 0 (it does
	not change the GLSL), which is how the imported and shared keys hash */
	memset(&data, 0, sizeof(data));
	memcpy(&data, key_data, size);
	if (stage == DK_SHADER_STAGE_PIXEL)
		data.pixel.count_samples = 0;
	hash = key_hash(stage, &data);

	entry = handle_slot(hash);
	if (entry->hash == hash)
	{
		memcpy(last_key[which], key_data, size);
		last_handle[which] = entry->handle;
		return entry->handle;
	}
	handle = host_dk_shader_find(stage, hash, (uint32_t)(uintptr_t)&state);
	if (handle)
	{
		if (handle_count < HANDLE_SLOTS / 8 * 7)
		{
			entry->hash = hash;
			entry->handle = handle;
			handle_count++;
			glsl_write(stage, hash, &data);
		}
		return handle;
	}
	if (state == 1)
	{
		/* queued or being compiled: the draw's priority, if more urgent
		than whoever queued it (no GLSL is wanted for that) */
		host_dk_shader_compile(stage, hash, 0, 0, DK_SHADER_PRIORITY_DRAW);
		/* (in a map's first seconds, waited for: stall_wait) */
		return stall_allowed() ? stall_found(stage, hash, &data) : 0;
	}
	/* unknown to the host: kept in console.dkk if this console has never
	met it, and queued at the draw's priority - the compile is a few frames
	away and the draws that need it are skipped until then */
	variant_unknown = asking_variant;
	if (key_add(stage, current_map, &data))
		console_append(stage, current_map, &data, size);
	{
		char *glsl = NULL;

		if (stage == DK_SHADER_STAGE_VERTEX)
			glsl = vertex_glsl(&data.vertex);
		else
			glsl = nv2a_dk_pixel_shader_to_glsl(&data.pixel);
		if (glsl)
		{
			host_dk_shader_compile(stage, hash, (uint32_t)(uintptr_t)glsl, (uint32_t)strlen(glsl),
				DK_SHADER_PRIORITY_DRAW);
			free(glsl);
			if (stall_allowed())
				return stall_found(stage, hash, &data);
		}
	}
	return 0;
}

uint32_t dk_shader_variant_for_draw(const void *key_data, int *unknown)
{
	uint32_t handle;

	asking_variant = TRUE;
	variant_unknown = FALSE;
	handle = dk_shader_for_draw(DK_SHADER_STAGE_VERTEX, key_data);
	asking_variant = FALSE;
	*unknown = variant_unknown;
	return handle;
}
