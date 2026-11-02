/*
HOST_TEXTURE_CACHE.C

Textures' pixels kept in the host's memory, outside the Xbox window, so that
a texture the game's texture cache gave up is read back from here and not
from the SD card. The game's cache lives in the window and is half of it (128
or 256 MB); a mod of uncompressed textures needs more than that for one map,
and each texture the cache took back was read from the card in the middle of
a frame: most of a tester's hitches. The host has far more memory free (about
1.8 GB of its heap on a Switch Lite in a 64-player match with that mod), which
the 32-bit guest cannot address, so the guest asks for a texture's bytes by a
key and the host copies them (its buffer a guest address, as 32 bits) (host_texture_cache_get, _put).

The bytes kept are a file's, as read, so that what the game does with them
after the read is the same either way: a mod's bitmaps
(port/linux/game/mod_bitmaps.c: before the channels are told and the
hardware format rebuilt), and a Custom Edition map's
(port/linux/game/custom_edition_cache.c: before they are converted).

The map's mod bitmaps are read into it ahead, on a thread of its own
(host_texture_cache_prefetch): Halo reads a texture the first time a frame
draws it, and a mod's from the card took a frame past 33 ms in a 48-bot
match, again and again as new weapons, vehicles and effects came into view
(every hitch the Switch Lite's profile of a played minute had, 189 textures,
none from here).

It keeps at most 512 MB, emptied with each map's mods (more was planned,
sized from the heap's free memory, but a map with a mod of uncompressed
textures needed under 50 MB of it); and while the heap's free memory is
under 256 MB it keeps nothing more, and lets its oldest go. */

#include "host.h"

#include <malloc.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../guest/dk_commands.h"

#define CACHE_BUCKETS 4096
#define CACHE_BUDGET (512ull * 1024 * 1024)
#define CACHE_FLOOR (256ull * 1024 * 1024)
/* the heap's free memory looked at again once this much more is kept */
#define CACHE_FLOOR_LOOK_BYTES (32ull * 1024 * 1024)

struct cache_entry
{
	struct cache_entry *bucket_next;
	struct cache_entry *older, *newer;
	uint64_t key;
	uint32_t size;
	unsigned char *bytes;
};

static pthread_mutex_t cache_lock = PTHREAD_MUTEX_INITIALIZER;
static struct cache_entry *buckets[CACHE_BUCKETS];
static struct cache_entry *oldest, *newest;
static uint64_t cache_bytes, cache_budget, bytes_since_look;
static unsigned long cache_entries, hits, misses, cache_puts, refused;

extern char *fake_heap_start;
extern char *fake_heap_end;

/* the heap's free memory: what is free in what it handed out, and what it never handed out */
static uint64_t heap_free(void)
{
	struct mallinfo heap = mallinfo();
	size_t size = (size_t)(fake_heap_end - fake_heap_start);

	return (uint64_t)heap.fordblks + (size > heap.arena ? size - heap.arena : 0);
}

static unsigned bucket_of(uint64_t key)
{
	return (unsigned)((key * 0x9E3779B97F4A7C15ull) >> 52) % CACHE_BUCKETS;
}

/* (under the lock) */
static void entry_unlink(struct cache_entry *entry)
{
	struct cache_entry **link = &buckets[bucket_of(entry->key)];

	while (*link && *link != entry)
		link = &(*link)->bucket_next;
	if (*link)
		*link = entry->bucket_next;
	if (entry->older)
		entry->older->newer = entry->newer;
	else
		oldest = entry->newer;
	if (entry->newer)
		entry->newer->older = entry->older;
	else
		newest = entry->older;
	cache_bytes -= entry->size;
	cache_entries--;
	free(entry->bytes);
	free(entry);
}

/* (under the lock) the most lately used */
static void entry_touch(struct cache_entry *entry)
{
	if (entry == newest)
		return;
	if (entry->older)
		entry->older->newer = entry->newer;
	else
		oldest = entry->newer;
	entry->newer->older = entry->older;
	entry->older = newest;
	entry->newer = NULL;
	newest->newer = entry;
	newest = entry;
}

static struct cache_entry *entry_find(uint64_t key)
{
	struct cache_entry *entry;

	for (entry = buckets[bucket_of(key)]; entry; entry = entry->bucket_next)
	{
		if (entry->key == key)
			return entry;
	}
	return NULL;
}

/* (under the lock) everything let go, and the budget for what comes now */
static void cache_empty(void)
{
	uint64_t free_bytes, budget;

	while (oldest)
		entry_unlink(oldest);
	free_bytes = heap_free();
	budget = CACHE_BUDGET;
	if (hits || misses || cache_puts)
		host_logf(HOST_LOG_INFO, "texture cache (host): the map before had %lu textures read from here, %lu from the "
			"card", hits, misses);
	/* (the counts, the map's from now: the report's are since this) */
	hits = misses = cache_puts = refused = 0;
	cache_budget = budget;
	bytes_since_look = 0;
	host_logf(HOST_LOG_INFO, "texture cache (host): emptied; up to %.0f MB from now (%.0f MB of the heap free)",
		(double)budget / 1048576.0, (double)free_bytes / 1048576.0);
}

static void prefetch_stop(void);

void host_texture_cache_clear(void)
{
	/* (what is being read ahead is the map's that goes) */
	prefetch_stop();
	pthread_mutex_lock(&cache_lock);
	cache_empty();
	pthread_mutex_unlock(&cache_lock);
}

int host_texture_cache_get(uint64_t key, uint32_t guest_buffer, uint32_t size)
{
	void *buffer = (void *)(uintptr_t)guest_buffer;
	struct cache_entry *entry;
	int found = 0;

	pthread_mutex_lock(&cache_lock);
	entry = entry_find(key);
	if (entry && entry->size == size)
	{
		memcpy(buffer, entry->bytes, size);
		entry_touch(entry);
		hits++;
		found = 1;
	}
	else
	{
		misses++;
	}
	pthread_mutex_unlock(&cache_lock);
	return found;
}

/* size bytes at buffer kept by key; 0 if they are not (the budget, the heap's floor) */
static int cache_put(uint64_t key, const void *buffer, uint32_t size)
{
	struct cache_entry *entry;

	if (!size)
		return 0;
	pthread_mutex_lock(&cache_lock);
	if (!cache_budget)
		cache_empty();
	entry = entry_find(key);
	if (entry)
		entry_unlink(entry);
	/* (the oldest let go to keep within the budget; and, now and then, to keep the heap's floor) */
	while (oldest && cache_bytes + size > cache_budget)
		entry_unlink(oldest);
	bytes_since_look += size;
	if (bytes_since_look >= CACHE_FLOOR_LOOK_BYTES)
	{
		bytes_since_look = 0;
		while (oldest && heap_free() < CACHE_FLOOR + size)
			entry_unlink(oldest);
		if (heap_free() < CACHE_FLOOR + size)
		{
			refused++;
			pthread_mutex_unlock(&cache_lock);
			return 0;
		}
	}
	if (size > cache_budget)
	{
		pthread_mutex_unlock(&cache_lock);
		return 0;
	}
	entry = calloc(1, sizeof(*entry));
	if (entry)
		entry->bytes = malloc(size);
	if (!entry || !entry->bytes)
	{
		free(entry);
		refused++;
		pthread_mutex_unlock(&cache_lock);
		return 0;
	}
	memcpy(entry->bytes, buffer, size);
	entry->key = key;
	entry->size = size;
	entry->bucket_next = buckets[bucket_of(key)];
	buckets[bucket_of(key)] = entry;
	entry->older = newest;
	if (newest)
		newest->newer = entry;
	else
		oldest = entry;
	newest = entry;
	cache_bytes += size;
	cache_entries++;
	cache_puts++;
	pthread_mutex_unlock(&cache_lock);
	return 1;
}

void host_texture_cache_put(uint64_t key, uint32_t guest_buffer, uint32_t size)
{
	cache_put(key, (const void *)(uintptr_t)guest_buffer, size);
}

/* ---------- reading ahead

One thread, made the first time, reads a list at a time: its textures sorted by file and place, so that each file
is opened once and read front to back, each read into the host's memory and kept, skipped where the cache has it
already. A new list, or the cache emptied (the map's mods let go), stops the one being read at the next texture:
what it was reading then is not kept. It sleeps a little between textures, so that the game's own reads of the card
wait less for it. */

#define PREFETCH_PAUSE_NS 1000000L

struct prefetch_item
{
	uint64_t key;
	uint32_t offset, size;
	char *path;
};

static struct
{
	pthread_mutex_t lock;
	pthread_cond_t wake;
	int started;
	/* the list waiting to be read, and the generation it belongs to: a new list or a clear makes a new one */
	struct prefetch_item *items;
	uint32_t count;
	unsigned long generation;
} prefetch = { PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER };

static void prefetch_items_free(struct prefetch_item *items, uint32_t count)
{
	uint32_t index;

	for (index = 0; items && index < count; index++)
		free(items[index].path);
	free(items);
}

static int prefetch_compare(const void *first, const void *second)
{
	const struct prefetch_item *a = first, *b = second;
	int order = strcmp(a->path, b->path);

	if (order)
		return order;
	return a->offset < b->offset ? -1 : a->offset > b->offset;
}

static unsigned long prefetch_generation(void)
{
	unsigned long generation;

	pthread_mutex_lock(&prefetch.lock);
	generation = prefetch.generation;
	pthread_mutex_unlock(&prefetch.lock);
	return generation;
}

static int cache_has(uint64_t key, uint32_t size)
{
	struct cache_entry *entry;
	int found;

	pthread_mutex_lock(&cache_lock);
	entry = entry_find(key);
	found = entry && entry->size == size;
	pthread_mutex_unlock(&cache_lock);
	return found;
}

static void *prefetch_thread(void *unused)
{
	(void)unused;
	for (;;)
	{
		struct prefetch_item *items;
		uint32_t count, index, kept = 0, there = 0, failed = 0;
		unsigned long generation;
		uint64_t bytes = 0, start;
		struct timespec now;
		FILE *file = NULL;
		const char *open_path = NULL;

		pthread_mutex_lock(&prefetch.lock);
		while (!prefetch.items)
			pthread_cond_wait(&prefetch.wake, &prefetch.lock);
		items = prefetch.items;
		count = prefetch.count;
		generation = prefetch.generation;
		prefetch.items = NULL;
		prefetch.count = 0;
		pthread_mutex_unlock(&prefetch.lock);
		clock_gettime(CLOCK_MONOTONIC, &now);
		start = (uint64_t)now.tv_sec * 1000000000ull + (uint64_t)now.tv_nsec;
		for (index = 0; index < count && prefetch_generation() == generation; index++)
		{
			struct prefetch_item *item = &items[index];
			unsigned char *bytes_read;
			struct timespec pause = { 0, PREFETCH_PAUSE_NS };

			if (cache_has(item->key, item->size))
			{
				there++;
				continue;
			}
			if (!open_path || strcmp(open_path, item->path))
			{
				if (file)
					fclose(file);
				file = fopen(item->path, "rb");
				open_path = item->path;
			}
			bytes_read = file ? malloc(item->size) : NULL;
			if (!bytes_read || fseek(file, (long)item->offset, SEEK_SET) != 0 ||
				fread(bytes_read, 1, item->size, file) != item->size)
			{
				free(bytes_read);
				failed++;
				continue;
			}
			/* (not kept if the list it was in has been let go meanwhile) */
			if (prefetch_generation() == generation && cache_put(item->key, bytes_read, item->size))
			{
				kept++;
				bytes += item->size;
			}
			free(bytes_read);
			nanosleep(&pause, NULL);
		}
		if (file)
			fclose(file);
		clock_gettime(CLOCK_MONOTONIC, &now);
		host_logf(HOST_LOG_INFO, "texture cache (host): read ahead %u of %u textures, %.1f MB in %.0f ms (%u there "
			"already, %u not read%s)", kept, count, (double)bytes / 1048576.0,
			(double)((uint64_t)now.tv_sec * 1000000000ull + (uint64_t)now.tv_nsec - start) / 1e6, there, failed,
			index < count ? "; stopped: the map's mods were let go" : "");
		prefetch_items_free(items, count);
	}
	return NULL;
}

void host_texture_cache_prefetch(uint32_t guest_items, uint32_t count)
{
	const struct host_texture_prefetch *from = (const struct host_texture_prefetch *)(uintptr_t)guest_items;
	struct prefetch_item *items;
	uint32_t index, made = 0;

	if (!count)
		return;
	items = calloc(count, sizeof(*items));
	if (!items)
		return;
	for (index = 0; index < count; index++)
	{
		const char *path = (const char *)(uintptr_t)from[index].path;

		if (!path || !from[index].size || !(items[made].path = strdup(path)))
			continue;
		items[made].key = from[index].key;
		items[made].offset = from[index].offset;
		items[made].size = from[index].size;
		made++;
	}
	qsort(items, made, sizeof(*items), prefetch_compare);
	pthread_mutex_lock(&prefetch.lock);
	prefetch_items_free(prefetch.items, prefetch.count);
	prefetch.items = items;
	prefetch.count = made;
	prefetch.generation++;
	if (!prefetch.started)
	{
		pthread_t thread;

		prefetch.started = pthread_create(&thread, NULL, prefetch_thread, NULL) == 0 ? 1 : -1;
		if (prefetch.started > 0)
			pthread_detach(thread);
	}
	pthread_cond_signal(&prefetch.wake);
	pthread_mutex_unlock(&prefetch.lock);
	if (prefetch.started < 0)
	{
		pthread_mutex_lock(&prefetch.lock);
		prefetch_items_free(prefetch.items, prefetch.count);
		prefetch.items = NULL;
		prefetch.count = 0;
		pthread_mutex_unlock(&prefetch.lock);
	}
}

static void prefetch_stop(void)
{
	pthread_mutex_lock(&prefetch.lock);
	prefetch_items_free(prefetch.items, prefetch.count);
	prefetch.items = NULL;
	prefetch.count = 0;
	prefetch.generation++;
	pthread_mutex_unlock(&prefetch.lock);
}

/* (host_memory_report's) its size and how it has done since the last report */
void host_texture_cache_report(void)
{
	pthread_mutex_lock(&cache_lock);
	if (hits || misses || cache_puts || cache_entries)
		host_logf(HOST_LOG_INFO, "texture cache (host): %.0f MB of %.0f MB in %lu textures; since the map loaded, %lu "
			"read from here, %lu from the card, %lu kept, %lu not kept for want of memory",
			(double)cache_bytes / 1048576.0, (double)cache_budget / 1048576.0, cache_entries, hits, misses, cache_puts,
			refused);
	pthread_mutex_unlock(&cache_lock);
}
