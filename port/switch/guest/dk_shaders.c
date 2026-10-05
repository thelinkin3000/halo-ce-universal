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
file a release ships, or a file a player copies in (phase 8). They are read
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

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* the host's half of the cache, through the import stubs (a 64-bit hash
goes in one X register on both sides; the pointers are guest addresses,
zero-extended by the stubs) */
uint32_t host_dk_shader_find(uint32_t stage, uint64_t hash, uint32_t state_out);
void host_dk_shader_compile(uint32_t stage, uint64_t hash, uint32_t glsl, uint32_t glsl_size, uint32_t priority);
uint32_t host_dk_shader_known(uint32_t stage, uint64_t hash);

/* d3d8_dk.c: the vertex programs the game has made. The ids are the order
the game makes them in, the same in both images (phase 4's dump relies on
it), and the records name programs by id */
unsigned long d3d8_dk_vertex_shader_count(void);
int d3d8_dk_vertex_program_by_id(unsigned long id, uint64_t *program_hash, const uint32_t **instructions,
	unsigned long *instruction_count);

/* ---------- the keys */

#define DK_KEY_MAGIC 0x314b4b44UL /* "DKK1" */
#define DK_KEY_FORMAT 1
#define DK_KEY_FOLDER "z:\\shader_keys"
#define DK_KEY_CONSOLE_FILE "z:\\shader_keys\\console.dkk"
#define DK_KEY_SHARED_FILE "z:\\shader_keys\\keys.dkk"
/* d3d8_gl.c's program records (phase 4 read them for the dump) */
#define DK_RECORDS_PATH "z:\\shader_programs.bin"

/* how many keys the startup pass takes a frame (DEKO3D.md: generating the
GLSL costs the game thread time, and the game must not be held up) */
#define DK_PASS_PER_FRAME 8

/* the key's data as it is in a record: a vertex key or a pixel key,
padded to the pixel key's size (the larger of the two) so a record is one
size whatever it holds. The vertex key's own bytes are the program hash
then the packed mask, in that order - not the struct's, which has four
bytes of padding at its end that would otherwise have to be promised zero
by every caller */
#define DK_KEY_DATA_SIZE ((unsigned long)sizeof(struct nv2a_pixel_shader_key))
#define DK_VERTEX_KEY_BYTES 12UL

struct dk_key_record
{
	uint8_t stage;
	uint8_t reserved[3];
	uint32_t map_hash;
	uint8_t data[DK_KEY_DATA_SIZE];
};

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
};

static struct dk_key *keys;
static unsigned long key_count, key_capacity;
static BOOL started, pass_finished;
static unsigned long pass_index;
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
pixel key is mixed whole (it has no padding); the vertex key's fields one
by one, for the padding's sake */
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
		hash = dk_shader_hash_mix(hash, data, sizeof(struct nv2a_pixel_shader_key));
	return hash;
}

/* how many bytes of a record a stage's key takes */
static unsigned long key_data_size(uint32_t stage)
{
	return stage == DK_SHADER_STAGE_VERTEX ? DK_VERTEX_KEY_BYTES : sizeof(struct nv2a_pixel_shader_key);
}

/* ---------- the key store */

/* records are deduplicated by their shader hash as they are read; the
first of a hash wins, which is also what a file a player shares counts on */
static struct dk_key *key_add(uint32_t stage, uint32_t map_hash, const void *data)
{
	uint64_t hash = key_hash(stage, data);
	unsigned long index;

	for (index = 0; index < key_count; index++)
	{
		if (keys[index].hash == hash)
			return NULL;
	}
	if (key_count == key_capacity)
	{
		unsigned long grown_capacity = key_capacity ? key_capacity * 2 : 1024;
		struct dk_key *grown = realloc(keys, grown_capacity * sizeof(*grown));

		if (!grown)
			return NULL;
		keys = grown;
		key_capacity = grown_capacity;
	}
	memset(&keys[key_count], 0, sizeof(keys[key_count]));
	keys[key_count].stage = stage;
	keys[key_count].map_hash = map_hash;
	memcpy(&keys[key_count].data, data, key_data_size(stage));
	keys[key_count].hash = hash;
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
	if (fread(header, sizeof(header), 1, file) == 1 && header[0] == DK_KEY_MAGIC && header[1] == DK_KEY_FORMAT &&
		header[2] == sizeof(record))
	{
		while (fread(&record, sizeof(record), 1, file) == 1)
		{
			if (record.stage == DK_SHADER_STAGE_VERTEX || record.stage == DK_SHADER_STAGE_PIXEL)
			{
				if (key_add(record.stage, record.map_hash, record.data))
					added++;
			}
		}
	}
	else
		platform_log("shader keys: %s is not a key file of this build", path);
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
		console_append(keys[id].stage, keys[id].map_hash, &keys[id].data, key_data_size(keys[id].stage));
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

/* the GLSL for a key, from the generators (phase 4); NULL if the game has
not made the program a vertex key names (a record from another build, or
one the game never makes: the key is kept and skipped) */
static char *key_glsl(const struct dk_key *key)
{
	if (key->stage == DK_SHADER_STAGE_VERTEX)
	{
		const uint32_t *instructions;
		unsigned long instruction_count;

		if (!vertex_program(key->data.vertex.program_hash, &instructions, &instruction_count))
			return NULL;
		return nv2a_dk_vertex_shader_to_glsl(instructions, instruction_count, key->data.vertex.packed_mask);
	}
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
static BOOL pass_ask(struct dk_key *key, uint32_t priority)
{
	uint32_t known;

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
	if (!console)
		import_program_records();
	else if (key_count == before)
		platform_log("shader keys: %s holds no keys of this build", DK_KEY_CONSOLE_FILE);
	platform_log("shader cache: %lu keys known; the rest is compiled in the background while the game runs",
		key_count);
}

/* the startup pass: eight keys a frame, each asked of the host in turn and
queued if the host does not have it. Runs until every key is asked, and
says when it is done - the second launch's numbers come from these lines */
void dk_shader_frame(void)
{
	unsigned long taken = 0;

	if (!started || pass_finished)
		return;
	while (taken < DK_PASS_PER_FRAME && pass_index < key_count)
	{
		struct dk_key *key = &keys[pass_index++];

		if (key->asked)
			continue;
		/* a vertex key whose program the game has not made: kept and
		skipped, and it costs nothing, so it does not count against the
		frame's eight */
		{
			const uint32_t *instructions;
			unsigned long instruction_count;

			if (key->stage == DK_SHADER_STAGE_VERTEX &&
				!vertex_program(key->data.vertex.program_hash, &instructions, &instruction_count))
			{
				key->asked = 2;
				continue;
			}
		}
		key->ready = pass_ask(key, key->map_hash && key->map_hash == current_map ? DK_SHADER_PRIORITY_MAP :
			DK_SHADER_PRIORITY_REST);
		key->asked = 1;
		taken++;
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

/* the map the game finished loading (d3d8_gl_map_loaded): its keys go to
the front of the background compile's queue, raised in priority */
void dk_shader_map_loaded(uint32_t map_hash)
{
	unsigned long index, raised = 0;

	current_map = map_hash;
	if (!started)
		return;
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
#define HANDLE_SLOTS 4096

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
		}
		return handle;
	}
	if (state == 1)
	{
		/* queued or being compiled: the draw's priority, if more urgent
		than whoever queued it (no GLSL is wanted for that) */
		host_dk_shader_compile(stage, hash, 0, 0, DK_SHADER_PRIORITY_DRAW);
		return 0;
	}
	/* unknown to the host: kept in console.dkk if this console has never
	met it, and queued at the draw's priority - the compile is a few frames
	away and the draws that need it are skipped until then */
	if (key_add(stage, current_map, &data))
		console_append(stage, current_map, &data, size);
	{
		char *glsl = NULL;

		if (stage == DK_SHADER_STAGE_VERTEX)
		{
			const uint32_t *instructions;
			unsigned long instruction_count;

			if (vertex_program(data.vertex.program_hash, &instructions, &instruction_count))
				glsl = nv2a_dk_vertex_shader_to_glsl(instructions, instruction_count, data.vertex.packed_mask);
		}
		else
		{
			glsl = nv2a_dk_pixel_shader_to_glsl(&data.pixel);
		}
		if (glsl)
		{
			host_dk_shader_compile(stage, hash, (uint32_t)(uintptr_t)glsl, (uint32_t)strlen(glsl),
				DK_SHADER_PRIORITY_DRAW);
			free(glsl);
		}
	}
	return 0;
}
